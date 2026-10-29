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
#import <AVFoundation/AVFoundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <QuartzCore/QuartzCore.h>

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

// Rumble diagnostics. Retail launches write no runtime log, so a compact
// record of what the game asked for and what the controller accepted is kept
// in user defaults; the launcher's log export includes it. Updated on
// connect, on errors, on the first nonzero request and every 120 requests.
static NSString* const kRumbleDiagnosticsKey = @"Theft4RumbleDiagnostics";
static std::atomic<uint64_t> g_rumble_requests{0};
static std::atomic<uint64_t> g_rumble_nonzero{0};
static std::atomic<uint32_t> g_rumble_max_left{0};
static std::atomic<uint32_t> g_rumble_max_right{0};
static std::atomic<uint64_t> g_rumble_unrouted{0};
static std::atomic<uint64_t> g_rumble_player_starts{0};
static std::atomic<uint64_t> g_rumble_engine_resets{0};
// Rumble strength chosen in the app (Format > Rumble), 0...1.
static std::atomic<float> g_rumble_strength{1.0f};

extern "C" void rex_ios_input_set_rumble_strength(float strength) {
  g_rumble_strength.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
}

static dispatch_queue_t RumbleDiagnosticsQueue() {
  static dispatch_queue_t queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    queue = dispatch_queue_create("rex.input.ios.rumble-diagnostics", DISPATCH_QUEUE_SERIAL);
  });
  return queue;
}

// Only touched on RumbleDiagnosticsQueue(); starts empty each process.
static NSMutableDictionary<NSString*, id>* g_rumble_diagnostics;

static void NoteRumble(NSString* key, id value) {
  if (!key || !value) return;
  dispatch_async(RumbleDiagnosticsQueue(), ^{
    if (!g_rumble_diagnostics) g_rumble_diagnostics = [NSMutableDictionary new];
    g_rumble_diagnostics[key] = value;
    [NSUserDefaults.standardUserDefaults setObject:[g_rumble_diagnostics copy]
                                            forKey:kRumbleDiagnosticsKey];
  });
}

static void NoteRumbleError(NSString* stage, NSError* error) {
  NSString* text = error ? [NSString stringWithFormat:@"%@: %@ (%@ %ld)", stage,
                               error.localizedDescription, error.domain, (long)error.code]
                         : [stage stringByAppendingString:@": returned nil"];
  NoteRumble(@"last_error", text);
  // Keep the first failure too; later retries overwrite last_error.
  static std::atomic<bool> first_noted{false};
  if (!first_noted.exchange(true)) NoteRumble(@"first_error", text);
  AVAudioSession* session = AVAudioSession.sharedInstance;
  NoteRumble(@"audio_session_at_error",
             [NSString stringWithFormat:@"%@ options=%lu other_audio=%d", session.category,
                                        (unsigned long)session.categoryOptions,
                                        session.isOtherAudioPlaying]);
}

static void NoteRumbleCounters() {
  NoteRumble(@"requests", @(g_rumble_requests.load(std::memory_order_relaxed)));
  NoteRumble(@"nonzero_requests", @(g_rumble_nonzero.load(std::memory_order_relaxed)));
  NoteRumble(@"max_left_motor", @(g_rumble_max_left.load(std::memory_order_relaxed)));
  NoteRumble(@"max_right_motor", @(g_rumble_max_right.load(std::memory_order_relaxed)));
  NoteRumble(@"requests_without_controller", @(g_rumble_unrouted.load(std::memory_order_relaxed)));
}

// The controller's rumble, driven like the Xbox 360's two motors mixed into
// one. It follows the launcher's vibration test, the one path felt on
// device: the default-locality engine, made and started on the main thread
// and sharing the app's audio session, and plain pattern players whose
// continuous event carries its intensity. Grip engines with a looping
// player whose intensity was changed while playing started without errors
// in play yet were never felt, so each strength (in tenths) gets a player of
// its own, a new one at most every 0.1 s, and every burst lasts at least
// 0.15 s. Every method runs on the main queue, including the engine's
// reset/stop callbacks. Any failure drops the engine and waits a moment
// before a fresh try.

// A continuous event lasts at most 30 seconds; a motor held longer gets a new
// player just before it ends.
static constexpr CFTimeInterval kRumbleEventSeconds = 30.0;
static constexpr CFTimeInterval kRumbleRenewSeconds = 29.0;
static constexpr CFTimeInterval kRumbleMinimumSeconds = 0.15;
static constexpr CFTimeInterval kRumbleChangeSeconds = 0.10;
static constexpr float kRumbleLevels = 10.0f;

API_AVAILABLE(ios(14.0), tvos(14.0))
@interface LRIOSRumbleChannel : NSObject
- (instancetype)initWithHaptics:(GCDeviceHaptics*)haptics;
- (void)applyIntensity:(float)intensity;
- (void)teardown;
@end

@implementation LRIOSRumbleChannel {
  GCDeviceHaptics* _haptics;
  CHHapticEngine* _engine;
  id<CHHapticPatternPlayer> _player;  // the newest player, kept until replaced
  float _playing;                     // intensity of the running player, 0 when stopped
  float _wanted;                      // newest request, applied when the change limit allows
  BOOL _changeScheduled;
  BOOL _failed;
  CFTimeInterval _playerStarted;
  CFTimeInterval _retryAfter;
}

- (instancetype)initWithHaptics:(GCDeviceHaptics*)haptics {
  self = [super init];
  if (!self) return nil;
  _haptics = haptics;
  return self;
}

- (void)stopPlayerAtTime:(NSTimeInterval)time {
  NSError* error = nil;
  if (_playing > 0.0f) [_player stopAtTime:time error:&error];
  _playing = 0.0f;
}

- (void)fail:(NSString*)stage error:(NSError*)error {
  NoteRumbleError(stage, error);
  [self stopPlayerAtTime:CHHapticTimeImmediate];
  _player = nil;
  [_engine stopWithCompletionHandler:nil];
  _engine = nil;
  _failed = YES;
  _retryAfter = CACurrentMediaTime() + 2.0;
}

- (BOOL)startEngine {
  if (_engine) return YES;
  if (_failed && CACurrentMediaTime() < _retryAfter) return NO;
  _engine = [_haptics createEngineWithLocality:GCHapticsLocalityDefault];
  if (!_engine) {
    [self fail:@"create engine" error:nil];
    return NO;
  }
  // A reset invalidates every player and a stop halts playback; either way
  // the next nonzero intensity makes a fresh engine and player.
  __weak LRIOSRumbleChannel* weakSelf = self;
  CHHapticEngine* engine = _engine;
  dispatch_block_t forget = ^{
    LRIOSRumbleChannel* channel = weakSelf;
    if (!channel || channel->_engine != engine) return;
    channel->_player = nil;
    channel->_playing = 0.0f;
    channel->_engine = nil;
  };
  // Resets are counted: a haptics server that resets after every start
  // would silence rumble without any error.
  _engine.resetHandler = ^{
    NoteRumble(@"engine_resets", @(g_rumble_engine_resets.fetch_add(1, std::memory_order_relaxed) + 1));
    dispatch_async(dispatch_get_main_queue(), forget);
  };
  _engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    NoteRumble(@"last_engine_stop_reason", @(reason));
    dispatch_async(dispatch_get_main_queue(), forget);
  };
  NSError* error = nil;
  if (![_engine startAndReturnError:&error]) {
    [self fail:@"start engine" error:error];
    return NO;
  }
  if (_failed) NoteRumble(@"recovered_after_failure", @YES);
  _failed = NO;
  return YES;
}

// Starts a player at this intensity, then stops the one it replaces.
- (void)playIntensity:(float)level {
  if (![self startEngine]) return;
  NSError* error = nil;
  CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc]
      initWithParameterID:CHHapticEventParameterIDHapticIntensity value:level];
  CHHapticEventParameter* sharpness = [[CHHapticEventParameter alloc]
      initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
  CHHapticEvent* event = [[CHHapticEvent alloc]
      initWithEventType:CHHapticEventTypeHapticContinuous
             parameters:@[intensity, sharpness]
           relativeTime:0
               duration:kRumbleEventSeconds];
  CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
                                                          parameters:@[]
                                                               error:&error];
  if (!pattern) {
    [self fail:@"create pattern" error:error];
    return;
  }
  id<CHHapticPatternPlayer> player = [_engine createPlayerWithPattern:pattern error:&error];
  if (!player) {
    [self fail:@"create player" error:error];
    return;
  }
  if (![player startAtTime:CHHapticTimeImmediate error:&error]) {
    [self fail:@"start player" error:error];
    return;
  }
  [self stopPlayerAtTime:CHHapticTimeImmediate];
  _player = player;
  _playing = level;
  _playerStarted = CACurrentMediaTime();
  const uint64_t starts = g_rumble_player_starts.fetch_add(1, std::memory_order_relaxed) + 1;
  NoteRumble(@"player_starts", @(starts));
  if (starts == 1) {
    // The app's audio session as the first player starts; the engine shares
    // it, so this tells a session conflict apart from the rumble path.
    AVAudioSession* session = AVAudioSession.sharedInstance;
    NoteRumble(@"audio_session_at_first_player",
               [NSString stringWithFormat:@"%@ options=%lu other_audio=%d", session.category,
                                          (unsigned long)session.categoryOptions,
                                          session.isOtherAudioPlaying]);
  }
}

- (void)applyIntensity:(float)intensity {
  // Tenths, and never below one tenth while a motor is turning at all.
  intensity = std::clamp(intensity, 0.0f, 1.0f);
  const float level = intensity > 0.0f
      ? std::max(1.0f, std::round(intensity * kRumbleLevels)) / kRumbleLevels
      : 0.0f;
  _wanted = level;
  const CFTimeInterval played = CACurrentMediaTime() - _playerStarted;
  if (level <= 0.0f) {
    // Short bursts still play long enough to be felt.
    if (_playing > 0.0f)
      [self stopPlayerAtTime:_engine.currentTime + std::max(0.0, kRumbleMinimumSeconds - played)];
    return;
  }
  if (_playing == level && played < kRumbleRenewSeconds) return;
  if (_playing > 0.0f && played < kRumbleChangeSeconds) {
    // Changing strength every frame would replace a player every frame;
    // the newest strength is applied once the limit allows.
    if (_changeScheduled) return;
    _changeScheduled = YES;
    __weak LRIOSRumbleChannel* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 int64_t((kRumbleChangeSeconds - played) * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     LRIOSRumbleChannel* channel = weakSelf;
                     if (!channel) return;
                     channel->_changeScheduled = NO;
                     if (channel->_playing > 0.0f) [channel applyIntensity:channel->_wanted];
                   });
    return;
  }
  [self playIntensity:level];
}

- (void)teardown {
  [self stopPlayerAtTime:CHHapticTimeImmediate];
  _player = nil;
  [_engine stopWithCompletionHandler:nil];
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
  // Only touched on the main queue.
  LRIOSRumbleChannel* _rumble;
  std::atomic<uint32_t> _pending;
  std::atomic<bool> _scheduled;
  std::atomic<bool> _rumbleActive;
}

- (instancetype)initWithController:(GCController*)controller {
  self = [super init];
  if (!self) return nil;
  _controller = controller;
  _pending.store(0);
  _scheduled.store(false);
  _rumbleActive.store(false);

  if (@available(iOS 14.0, tvOS 14.0, *)) {
    GCDeviceHaptics* haptics = controller.haptics;
    if (haptics) {
      _rumble = [[LRIOSRumbleChannel alloc] initWithHaptics:haptics];
      _rumbleActive.store(true);
      NoteRumble(@"haptics_mode", @"default locality, fixed-intensity players");
    } else {
      NoteRumble(@"haptics_mode", @"controller reports no haptics");
    }
    NoteRumble(@"controller", controller.vendorName ?: controller.productCategory ?: @"unknown");
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
  dispatch_async(dispatch_get_main_queue(), ^{
    LRIOSControllerBridge* bridge = weakSelf;
    if (!bridge) return;
    bridge->_scheduled.store(false, std::memory_order_release);
    const uint32_t value = bridge->_pending.load(std::memory_order_acquire);
    const float l = float(value >> 16) / 65535.0f;
    const float r = float(value & 0xFFFFu) / 65535.0f;
    // The heavy left motor leads; the light right motor adds a little. The
    // player's rumble strength scales the result.
    const float mixed = std::min(1.0f, std::max(l, r) * 0.7f + std::min(l, r) * 0.3f);
    [bridge->_rumble applyIntensity:mixed * g_rumble_strength.load(std::memory_order_relaxed)];
  });
}

- (void)teardown {
  if (@available(iOS 14.0, tvOS 14.0, *)) {
    _rumbleActive.store(false, std::memory_order_release);
    // Runs after any queued apply; the block keeps the bridge alive until the
    // motors have stopped.
    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_rumble teardown];
      self->_rumble = nil;
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
  // Count every request, including ones with no controller in this slot, so
  // the diagnostics show whether the game asks for vibration at all.
  const uint32_t left_speed = vibration->left_motor_speed;
  const uint32_t right_speed = vibration->right_motor_speed;
  const uint64_t requests = g_rumble_requests.fetch_add(1, std::memory_order_relaxed) + 1;
  bool first_nonzero = false;
  if (left_speed || right_speed) {
    first_nonzero = g_rumble_nonzero.fetch_add(1, std::memory_order_relaxed) == 0;
    uint32_t seen = g_rumble_max_left.load(std::memory_order_relaxed);
    while (left_speed > seen && !g_rumble_max_left.compare_exchange_weak(seen, left_speed)) {}
    seen = g_rumble_max_right.load(std::memory_order_relaxed);
    while (right_speed > seen && !g_rumble_max_right.compare_exchange_weak(seen, right_speed)) {}
  }
  std::lock_guard<std::mutex> g(mutex_);
  auto* slot = FindSlot(user_index);
  if (!slot || !slot->ios || !slot->ios->bridge_retained) {
    g_rumble_unrouted.fetch_add(1, std::memory_order_relaxed);
  }
  if (requests == 1 || first_nonzero || requests % 120 == 0) NoteRumbleCounters();
  if (!slot) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (!slot->ios || !slot->ios->bridge_retained) return X_ERROR_SUCCESS;

  if (@available(iOS 14.0, tvOS 14.0, *)) {
    LRIOSControllerBridge* bridge =
        (__bridge LRIOSControllerBridge*)slot->ios->bridge_retained;
    [bridge rumbleLeft:left_speed / 65535.0f right:right_speed / 65535.0f];
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
