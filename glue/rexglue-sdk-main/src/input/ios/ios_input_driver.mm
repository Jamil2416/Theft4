/**
 ******************************************************************************
 * ReXGlue — iOS GameController input driver (Objective-C++)
 ******************************************************************************
 *
 * Bridges Apple's GameController.framework (MFi / Xbox / PS / Switch
 * controllers over BT + Lightning) into the rex InputDriver contract.
 *
 * Connect/disconnect is wired via NSNotificationCenter; polling reads
 * extendedGamepad values directly from the live GCController object each
 * time GetState is called. Haptics uses CHHapticEngine obtained via
 * controller.haptics. This mirrors haptics_ios.mm's pattern but is
 * self-contained so the SDK does not depend on the app-side bridge.
 */

#include "ios_input_driver.h"

#if REX_PLATFORM_IOS

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/input/absolute_pointer.h>
#include <rex/logging.h>

// ---------------------------------------------------------------------------
// Per-controller Obj-C state
// ---------------------------------------------------------------------------

// One haptic actuator group driven like an Xbox 360 rumble motor. A looping
// continuous event plays at full base intensity; each motor change only sends
// an intensity-control parameter, so the game's per-frame XInputSetState calls
// never rebuild or restart a pattern. Every method runs on the owning
// controller's serial queue, including the engine's reset/stop callbacks.
API_AVAILABLE(ios(14.0), tvos(14.0))
@interface LRIOSRumbleChannel : NSObject
- (instancetype)initWithHaptics:(GCDeviceHaptics*)haptics
                       locality:(GCHapticsLocality)locality
                      sharpness:(float)sharpness
                          queue:(dispatch_queue_t)queue;
- (void)applyIntensity:(float)intensity;
- (void)teardown;
@end

@implementation LRIOSRumbleChannel {
  CHHapticEngine* _engine;
  id<CHHapticAdvancedPatternPlayer> _player;
  float _sharpness;
  float _applied;
  BOOL _running;
}

- (instancetype)initWithHaptics:(GCDeviceHaptics*)haptics
                       locality:(GCHapticsLocality)locality
                      sharpness:(float)sharpness
                          queue:(dispatch_queue_t)queue {
  self = [super init];
  if (!self) return nil;
  _engine = [haptics createEngineWithLocality:locality];
  if (!_engine) return nil;
  _sharpness = sharpness;
  // A reset invalidates every player and a stop halts playback; either way
  // the next nonzero intensity starts the engine and a player again.
  __weak LRIOSRumbleChannel* weakSelf = self;
  dispatch_block_t forget = ^{
    LRIOSRumbleChannel* channel = weakSelf;
    if (!channel) return;
    channel->_player = nil;
    channel->_running = NO;
    channel->_applied = 0.0f;
  };
  _engine.resetHandler = ^{ dispatch_async(queue, forget); };
  _engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    (void)reason;
    dispatch_async(queue, forget);
  };
  return self;
}

- (BOOL)preparePlayer {
  if (_player) return YES;
  NSError* error = nil;
  if (![_engine startAndReturnError:&error]) return NO;
  CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc]
      initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
  CHHapticEventParameter* sharpness = [[CHHapticEventParameter alloc]
      initWithParameterID:CHHapticEventParameterIDHapticSharpness value:_sharpness];
  // Continuous events are limited to 30 seconds; looping makes the motor
  // hold until the game changes it.
  CHHapticEvent* event = [[CHHapticEvent alloc]
      initWithEventType:CHHapticEventTypeHapticContinuous
             parameters:@[intensity, sharpness]
           relativeTime:0
               duration:30.0];
  CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
                                                          parameters:@[]
                                                               error:&error];
  if (!pattern) return NO;
  _player = [_engine createAdvancedPlayerWithPattern:pattern error:&error];
  if (!_player) return NO;
  _player.loopEnabled = YES;
  return YES;
}

- (void)applyIntensity:(float)intensity {
  intensity = std::clamp(intensity, 0.0f, 1.0f);
  // Motor speeds arrive as 16-bit values; ignore changes the actuators
  // cannot express rather than resending them every frame.
  if (std::fabs(intensity - _applied) < 1.0f / 255.0f && (intensity > 0.0f) == _running) return;
  NSError* error = nil;
  if (intensity <= 0.0f) {
    if (_running) [_player stopAtTime:CHHapticTimeImmediate error:&error];
    _running = NO;
    _applied = 0.0f;
    return;
  }
  if (![self preparePlayer]) return;
  CHHapticDynamicParameter* control = [[CHHapticDynamicParameter alloc]
      initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                    value:intensity
             relativeTime:0];
  if (!_running) {
    if (![_player startAtTime:CHHapticTimeImmediate error:&error]) {
      // The engine may have stopped itself (for example when idle); restart
      // it once and retry with a fresh player.
      _player = nil;
      if (![self preparePlayer] || ![_player startAtTime:CHHapticTimeImmediate error:&error]) return;
    }
    _running = YES;
  }
  [_player sendParameters:@[control] atTime:CHHapticTimeImmediate error:&error];
  _applied = intensity;
}

- (void)teardown {
  NSError* error = nil;
  if (_running) [_player stopAtTime:CHHapticTimeImmediate error:&error];
  _player = nil;
  _running = NO;
  [_engine stopWithCompletionHandler:^(NSError* _Nullable e) { (void)e; }];
  _engine = nil;
}

@end

API_AVAILABLE(ios(14.0), tvos(14.0))
@interface LRIOSControllerBridge : NSObject
@property(nonatomic, strong) GCController* controller;
- (instancetype)initWithController:(GCController*)controller;
- (void)rumbleLeft:(float)left right:(float)right;
- (void)teardown;
@end

@implementation LRIOSControllerBridge {
  // Separate grips when the controller exposes them (DualSense, DualShock 4,
  // Xbox): the Xbox low-frequency left motor drives the left grip and the
  // high-frequency right motor the right grip. Otherwise one mixed channel.
  // The channels are only touched on _queue.
  LRIOSRumbleChannel* _left;
  LRIOSRumbleChannel* _right;
  LRIOSRumbleChannel* _mixed;
  dispatch_queue_t _queue;
  std::atomic<uint32_t> _pending;
  std::atomic<bool> _scheduled;
  std::atomic<bool> _rumbleActive;
}

- (instancetype)initWithController:(GCController*)controller {
  self = [super init];
  if (!self) return nil;
  _controller = controller;
  _queue = dispatch_queue_create("rex.input.ios.rumble", DISPATCH_QUEUE_SERIAL);
  _pending.store(0);
  _scheduled.store(false);
  _rumbleActive.store(false);

  if (@available(iOS 14.0, tvOS 14.0, *)) {
    GCDeviceHaptics* haptics = controller.haptics;
    if (haptics) {
      NSSet<GCHapticsLocality>* localities = haptics.supportedLocalities;
      if ([localities containsObject:GCHapticsLocalityLeftHandle] &&
          [localities containsObject:GCHapticsLocalityRightHandle]) {
        _left = [[LRIOSRumbleChannel alloc] initWithHaptics:haptics
                                                   locality:GCHapticsLocalityLeftHandle
                                                  sharpness:0.25f
                                                      queue:_queue];
        _right = [[LRIOSRumbleChannel alloc] initWithHaptics:haptics
                                                    locality:GCHapticsLocalityRightHandle
                                                   sharpness:0.75f
                                                       queue:_queue];
      }
      if (!_left || !_right) {
        _left = nil;
        _right = nil;
        _mixed = [[LRIOSRumbleChannel alloc] initWithHaptics:haptics
                                                    locality:GCHapticsLocalityDefault
                                                   sharpness:0.5f
                                                       queue:_queue];
      }
      _rumbleActive.store(_left || _mixed);
    }
    // Enable motion sampling if supported (gyro/accel).
    if (controller.motion) {
      controller.motion.sensorsActive = YES;
    }
  }
  return self;
}

- (void)rumbleLeft:(float)left right:(float)right {
  if (!_rumbleActive.load(std::memory_order_acquire)) return;
  // Keep only the newest motor pair; at most one apply is queued at a time,
  // so a burst of SetState calls costs one haptic update.
  const uint32_t packed = (uint32_t(std::lround(std::clamp(left, 0.0f, 1.0f) * 65535.0f)) << 16) |
                          uint32_t(std::lround(std::clamp(right, 0.0f, 1.0f) * 65535.0f));
  _pending.store(packed, std::memory_order_release);
  if (_scheduled.exchange(true, std::memory_order_acq_rel)) return;
  __weak LRIOSControllerBridge* weakSelf = self;
  dispatch_async(_queue, ^{
    LRIOSControllerBridge* bridge = weakSelf;
    if (!bridge) return;
    bridge->_scheduled.store(false, std::memory_order_release);
    const uint32_t value = bridge->_pending.load(std::memory_order_acquire);
    const float l = float(value >> 16) / 65535.0f;
    const float r = float(value & 0xFFFFu) / 65535.0f;
    if (bridge->_left) {
      [bridge->_left applyIntensity:l];
      [bridge->_right applyIntensity:r];
    } else {
      [bridge->_mixed applyIntensity:std::min(1.0f, std::max(l, r) * 0.7f + std::min(l, r) * 0.3f)];
    }
  });
}

- (void)teardown {
  if (@available(iOS 14.0, tvOS 14.0, *)) {
    _rumbleActive.store(false, std::memory_order_release);
    // Runs after any queued apply; the block keeps the bridge alive until the
    // motors have stopped.
    dispatch_async(_queue, ^{
      [self->_left teardown];
      [self->_right teardown];
      [self->_mixed teardown];
      self->_left = nil;
      self->_right = nil;
      self->_mixed = nil;
    });
    if (_controller && _controller.motion) {
      _controller.motion.sensorsActive = NO;
    }
  }
  _controller = nil;
}

@end

// The launcher passes its light-bar choice as "R,G,B" (0-255 each). Called on
// the main queue with the controller itself, not the bridge, so a disconnect
// on the input thread cannot race it.
API_AVAILABLE(ios(14.0), tvos(14.0))
static void ApplyLightColor(GCController* controller) {
  const char* value = std::getenv("THEFT4_CONTROLLER_LIGHT");
  if (!value || !controller.light) return;
  unsigned red = 0, green = 0, blue = 0;
  char extra = 0;
  if (std::sscanf(value, "%u,%u,%u%c", &red, &green, &blue, &extra) != 3 ||
      red > 255 || green > 255 || blue > 255) {
    return;
  }
  controller.light.color = [[GCColor alloc] initWithRed:red / 255.0f
                                                  green:green / 255.0f
                                                   blue:blue / 255.0f];
}

namespace rex::input::ios {

// Opaque wrapper so the header doesn't leak Obj-C types.
struct IOSController {
  void* bridge_retained = nullptr;  // LRIOSControllerBridge* (__bridge_retained)
};

}  // namespace rex::input::ios

// ---------------------------------------------------------------------------
// NSNotificationCenter observer
// ---------------------------------------------------------------------------

@interface LRIOSInputObserver : NSObject {
@public
  rex::input::ios::IOSInputDriver* _driver;
}
@end

@implementation LRIOSInputObserver
- (void)onConnect:(NSNotification*)note {
  GCController* c = (GCController*)note.object;
  if (_driver && c) _driver->HandleConnect((__bridge void*)c);
}
- (void)onDisconnect:(NSNotification*)note {
  GCController* c = (GCController*)note.object;
  if (_driver && c) _driver->HandleDisconnect((__bridge void*)c);
}
@end

// ---------------------------------------------------------------------------
// Driver implementation
// ---------------------------------------------------------------------------

namespace rex::input::ios {

IOSInputDriver::IOSInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

IOSInputDriver::~IOSInputDriver() {
  std::lock_guard<std::mutex> g(mutex_);
  if (observer_) {
    LRIOSInputObserver* obs = (__bridge_transfer LRIOSInputObserver*)observer_;
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
    observer_ = nullptr;
  }
  for (auto& slot : slots_) {
    if (slot.inventory_id) {
      GetAbsolutePointerService().RemoveGameController(slot.inventory_id);
    }
    if (slot.ios) {
      if (slot.ios->bridge_retained) {
        LRIOSControllerBridge* b =
            (__bridge_transfer LRIOSControllerBridge*)slot.ios->bridge_retained;
        [b teardown];
        (void)b;
        slot.ios->bridge_retained = nullptr;
      }
      delete slot.ios;
      slot.ios = nullptr;
    }
  }
}

X_STATUS IOSInputDriver::Setup() {
  LRIOSInputObserver* obs = [[LRIOSInputObserver alloc] init];
  obs->_driver = this;
  observer_ = (__bridge_retained void*)obs;

  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:obs
         selector:@selector(onConnect:)
             name:GCControllerDidConnectNotification
           object:nil];
  [nc addObserver:obs
         selector:@selector(onDisconnect:)
             name:GCControllerDidDisconnectNotification
           object:nil];

  // Adopt any controllers already present (app launched with a pad paired).
  for (GCController* c in [GCController controllers]) {
    HandleConnect((__bridge void*)c);
  }

  initialized_ = true;
  REXLOG_INFO("iOS GameController input driver initialized");
  return X_STATUS_SUCCESS;
}

void IOSInputDriver::OnWindowAvailable(rex::ui::Window* /*window*/) {
  // Nothing window-specific; GameController is process-global.
}

void IOSInputDriver::HandleConnect(void* gc_raw) {
  GCController* controller = (__bridge GCController*)gc_raw;
  if (!controller || !controller.extendedGamepad) return;
  const uint64_t inventory_id = uint64_t(reinterpret_cast<uintptr_t>(gc_raw));
  GetAbsolutePointerService().AddGameController(inventory_id);

  std::lock_guard<std::mutex> g(mutex_);
  for (const auto& slot : slots_) {
    if (slot.inventory_id == inventory_id) {
      return;
    }
  }
  // Find a free slot.
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].ios) {
      auto* ios = new IOSController();
      if (@available(iOS 14.0, tvOS 14.0, *)) {
        LRIOSControllerBridge* bridge =
            [[LRIOSControllerBridge alloc] initWithController:controller];
        ios->bridge_retained = (__bridge_retained void*)bridge;
        // Light-bar changes are UI-thread GameController calls.
        dispatch_async(dispatch_get_main_queue(), ^{ ApplyLightColor(controller); });
      } else {
        // Pre-iOS 14: no haptics, but we still want polling.
        ios->bridge_retained = nullptr;
      }
      controller.playerIndex = static_cast<GCControllerPlayerIndex>(i);
      slots_[i].ios = ios;
      slots_[i].inventory_id = inventory_id;
      slots_[i].state = {};
      slots_[i].state_changed = true;
      REXLOG_INFO("iOS GameController connected at user index {}", i);
      return;
    }
  }
  REXLOG_WARN("iOS GameController connected but no free slot.");
}

void IOSInputDriver::HandleDisconnect(void* gc_raw) {
  GCController* controller = (__bridge GCController*)gc_raw;
  const uint64_t inventory_id = uint64_t(reinterpret_cast<uintptr_t>(gc_raw));
  GetAbsolutePointerService().RemoveGameController(inventory_id);
  std::lock_guard<std::mutex> g(mutex_);
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].ios || slots_[i].inventory_id != inventory_id) continue;
    if (!slots_[i].ios->bridge_retained) {
      delete slots_[i].ios;
      slots_[i] = {};
      REXLOG_INFO("iOS GameController disconnected at user index {}", i);
      return;
    }
    LRIOSControllerBridge* b =
        (__bridge LRIOSControllerBridge*)slots_[i].ios->bridge_retained;
    if (b.controller == controller) {
      LRIOSControllerBridge* consumed =
          (__bridge_transfer LRIOSControllerBridge*)slots_[i].ios->bridge_retained;
      [consumed teardown];
      (void)consumed;
      delete slots_[i].ios;
      slots_[i] = {};
      REXLOG_INFO("iOS GameController disconnected at user index {}", i);
      return;
    }
  }
}

IOSInputDriver::Slot* IOSInputDriver::FindSlot(uint32_t user_index) {
  if (user_index >= slots_.size()) return nullptr;
  auto& s = slots_[user_index];
  return s.ios ? &s : nullptr;
}

void IOSInputDriver::SampleLocked(Slot& slot) {
  if (!slot.ios || !slot.ios->bridge_retained) return;
  LRIOSControllerBridge* bridge =
      (__bridge LRIOSControllerBridge*)slot.ios->bridge_retained;
  GCController* c = bridge.controller;
  if (!c) return;
  GCExtendedGamepad* gp = c.extendedGamepad;
  if (!gp) return;

  uint16_t buttons = 0;
  if (gp.buttonA.pressed)        buttons |= X_INPUT_GAMEPAD_A;
  if (gp.buttonB.pressed)        buttons |= X_INPUT_GAMEPAD_B;
  if (gp.buttonX.pressed)        buttons |= X_INPUT_GAMEPAD_X;
  if (gp.buttonY.pressed)        buttons |= X_INPUT_GAMEPAD_Y;
  if (gp.leftShoulder.pressed)   buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (gp.rightShoulder.pressed)  buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (gp.dpad.up.pressed)        buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (gp.dpad.down.pressed)      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (gp.dpad.left.pressed)      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (gp.dpad.right.pressed)     buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (gp.leftThumbstickButton.pressed)  buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (gp.rightThumbstickButton.pressed) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (@available(iOS 13.0, tvOS 13.0, *)) {
    if (gp.buttonMenu.pressed)    buttons |= X_INPUT_GAMEPAD_START;
    if (gp.buttonOptions.pressed) buttons |= X_INPUT_GAMEPAD_BACK;
  }
  if (@available(iOS 14.0, tvOS 14.0, *)) {
    if (gp.buttonHome && gp.buttonHome.pressed) buttons |= X_INPUT_GAMEPAD_GUIDE;
  }

  auto to_axis = [](float v) -> int16_t {
    v = std::max(-1.0f, std::min(1.0f, v));
    float scaled = v * 32767.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return static_cast<int16_t>(std::lround(scaled));
  };
  auto to_trigger = [](float v) -> uint8_t {
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint8_t>(std::lround(v * 255.0f));
  };

  auto& pad = slot.state.gamepad;
  uint16_t prev_buttons = static_cast<uint16_t>(pad.buttons);
  pad.buttons = buttons;
  pad.left_trigger  = to_trigger(gp.leftTrigger.value);
  pad.right_trigger = to_trigger(gp.rightTrigger.value);
  pad.thumb_lx = to_axis(gp.leftThumbstick.xAxis.value);
  pad.thumb_ly = to_axis(gp.leftThumbstick.yAxis.value);
  pad.thumb_rx = to_axis(gp.rightThumbstick.xAxis.value);
  pad.thumb_ry = to_axis(gp.rightThumbstick.yAxis.value);

  if (prev_buttons != buttons || slot.state_changed) {
    slot.state.packet_number++;
    slot.state_changed = false;
  }
  slot.last_buttons = buttons;
}

X_RESULT IOSInputDriver::GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= HID_IOS_USER_COUNT || !out_caps) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard<std::mutex> g(mutex_);
  auto* slot = FindSlot(user_index);
  if (!slot) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;
  out_caps->sub_type = 0x01;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFFu;
  out_caps->vibration.right_motor_speed = 0xFFFFu;
  return X_ERROR_SUCCESS;
}

X_RESULT IOSInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index >= HID_IOS_USER_COUNT) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard<std::mutex> g(mutex_);
  auto* slot = FindSlot(user_index);
  if (!slot) return X_ERROR_DEVICE_NOT_CONNECTED;

  SampleLocked(*slot);

  if (out_state) {
    if (is_active()) {
      std::memcpy(out_state, &slot->state, sizeof(*out_state));
    } else {
      std::memcpy(out_state, &slot->state, sizeof(*out_state));
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT IOSInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (user_index >= HID_IOS_USER_COUNT || !vibration) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard<std::mutex> g(mutex_);
  auto* slot = FindSlot(user_index);
  if (!slot) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (!slot->ios || !slot->ios->bridge_retained) return X_ERROR_SUCCESS;

  if (@available(iOS 14.0, tvOS 14.0, *)) {
    LRIOSControllerBridge* bridge =
        (__bridge LRIOSControllerBridge*)slot->ios->bridge_retained;
    float left  = vibration->left_motor_speed  / 65535.0f;
    float right = vibration->right_motor_speed / 65535.0f;
    [bridge rumbleLeft:left right:right];
  }
  return X_ERROR_SUCCESS;
}

X_RESULT IOSInputDriver::GetKeystroke(uint32_t user_index, uint32_t /*flags*/,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // The iOS driver doesn't translate analog-to-keystroke events; games
  // that need menu navigation fall back to the higher-level input system
  // or the MnK driver. Report empty so the system moves on.
  if (user_index != 0xFF && user_index >= HID_IOS_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (out_keystroke) std::memset(out_keystroke, 0, sizeof(*out_keystroke));
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::ios

#endif  // REX_PLATFORM_IOS
