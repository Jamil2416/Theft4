#import "Theft4ModMenu.h"
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#include "theft4_trainer.h"
#ifdef THEFT4_HAS_GAME_STARTUP
#define TRAINER_GET(s) theft4_trainer_get(s)
#define TRAINER_SET(s, v) theft4_trainer_set((s), (v))
#define TRAINER_RUN(a) theft4_trainer_run(a)
#define TRAINER_SPAWN(m) theft4_trainer_spawn_vehicle(m)
#define TRAINER_ACTIVE() theft4_trainer_active()
#define TRAINER_SET_ENABLED(e) theft4_trainer_set_enabled(e)
#else
// Builds without the game runtime show the menu with nothing to drive.
#define TRAINER_GET(s) 0
#define TRAINER_SET(s, v) ((void)0)
#define TRAINER_RUN(a) ((void)0)
#define TRAINER_SPAWN(m) ((void)0)
#define TRAINER_ACTIVE() 0
#define TRAINER_SET_ENABLED(e) ((void)0)
#endif

static NSString *const kCheatsEnabledKey = @"Theft4CheatsEnabled";

static UIColor *Ink(uint32_t rgb) {
    return [UIColor colorWithRed:((rgb >> 16) & 255) / 255.0 green:((rgb >> 8) & 255) / 255.0
                            blue:(rgb & 255) / 255.0 alpha:1];
}

@implementation Theft4ModItem
+ (instancetype)submenu:(NSString *)title help:(NSString *)help children:(NSArray<Theft4ModItem *> *)children {
    Theft4ModItem *item = [self new];
    item.title = title; item.help = help; item.kind = Theft4ModItemSubmenu; item.children = children;
    return item;
}
+ (instancetype)toggle:(NSString *)title help:(NSString *)help
                  isOn:(BOOL (^)(void))isOn setOn:(void (^)(BOOL))setOn {
    Theft4ModItem *item = [self new];
    item.title = title; item.help = help; item.kind = Theft4ModItemToggle;
    item.isOn = isOn; item.setOn = setOn;
    return item;
}
+ (instancetype)action:(NSString *)title help:(NSString *)help run:(void (^)(void))run {
    Theft4ModItem *item = [self new];
    item.title = title; item.help = help; item.kind = Theft4ModItemAction; item.run = run;
    return item;
}
@end

static const NSInteger kMaxRows = 8;  // fewer when the screen is short
static const CGFloat kRowHeight = 28, kHeaderHeight = 44, kFooterHeight = 28, kPanelWidth = 260, kHelpHeight = 46;
// Touchpad: a click is placed by where the finger is (axes -1...1, y up).
// The middle square is the centre; outside it the larger axis picks the edge.
static const CGFloat kPadCentre = 0.30;
static const CFTimeInterval kDoubleClick = 0.40, kHold = 0.50;

typedef NS_ENUM(NSInteger, Theft4PadZone) {
    Theft4PadCentre, Theft4PadUp, Theft4PadDown, Theft4PadLeft, Theft4PadRight
};

static Theft4PadZone PadZone(CGPoint p) {
    if (fabs(p.x) < kPadCentre && fabs(p.y) < kPadCentre) return Theft4PadCentre;
    if (fabs(p.x) > fabs(p.y)) return p.x > 0 ? Theft4PadRight : Theft4PadLeft;
    return p.y > 0 ? Theft4PadUp : Theft4PadDown;
}

static NSString *PadZoneName(Theft4PadZone zone) {
    switch (zone) {
        case Theft4PadCentre: return @"centre";
        case Theft4PadUp: return @"up";
        case Theft4PadDown: return @"down";
        case Theft4PadLeft: return @"left";
        case Theft4PadRight: return @"right";
    }
    return @"?";
}

@interface Theft4ModMenu ()
@end

@implementation Theft4ModMenu {
    Theft4ModItem *_root;
    NSMutableArray<Theft4ModItem *> *_stack;     // submenu path, root first
    NSMutableArray<NSNumber *> *_selections;     // selected row per level
    UIView *_panel, *_header, *_footer, *_helpBar;
    UILabel *_title, *_subtitle, *_counter, *_help, *_status;
    NSArray<UIView *> *_rows;
    NSArray<UILabel *> *_rowTitles, *_rowValues;
    NSInteger _firstVisible;
    CADisplayLink *_poll;
    BOOL _wasClicked, _holdFired;
    CFTimeInterval _pressStart;
    CGPoint _pressPoint;
    CGPoint _lastTouch;                 // last nonzero finger position
    CFTimeInterval _lastTouchTime, _lastClickTime;
    NSUInteger _clickCount;
    NSString *_lastClickText;           // shown under Diagnostics > Touchpad
    uint32_t _statusCounter;
    CFTimeInterval _statusUntil;
    // Appearance, from the menu's own Settings page.
    NSInteger _accentIndex, _opacity, _visibleRows;
    UIView *_accentBar;
    CFTimeInterval _diagnosticsSaved;
}

static NSString *const kAccentKey = @"Theft4ModMenuAccent";
static NSString *const kOpacityKey = @"Theft4ModMenuOpacity";

// Accent choices: the colours of the rainbow. Yellow is the default.
static NSArray<NSString *> *AccentNames(void) {
    return @[@"Red", @"Orange", @"Yellow", @"Green", @"Blue", @"Indigo", @"Violet"];
}
static const uint32_t kAccentColors[] = {0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x0A84FF, 0x5E5CE6, 0xBF5AF2};
static const NSInteger kDefaultAccent = 2;

- (UIColor *)accent {
    return Ink(kAccentColors[MAX(0, MIN(6, _accentIndex))]);
}

- (void)loadAppearance {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    _accentIndex = [defaults objectForKey:kAccentKey] ? MAX(0, MIN(6, [defaults integerForKey:kAccentKey]))
                                                      : kDefaultAccent;
    // Opacity is chosen in 10% steps; older saved values round to the nearest.
    const NSInteger opacity = [defaults objectForKey:kOpacityKey] ? [defaults integerForKey:kOpacityKey] : 90;
    _opacity = MAX(0, MIN(100, (opacity + 5) / 10 * 10));
}

// Background panels follow the opacity setting; text always stays opaque.
- (void)applyAppearance {
    const CGFloat alpha = _opacity / 100.0;
    _panel.backgroundColor = [Ink(0x1C1F21) colorWithAlphaComponent:alpha];
    _helpBar.backgroundColor = [Ink(0x1C1F21) colorWithAlphaComponent:alpha];
    _header.backgroundColor = [UIColor colorWithWhite:0 alpha:alpha];
    _footer.backgroundColor = [UIColor colorWithWhite:0 alpha:alpha];
    _accentBar.backgroundColor = self.accent;
    [self refresh];
}

- (Theft4ModItem *)settingsMenu {
    __weak Theft4ModMenu *weakSelf = self;
    NSMutableArray<Theft4ModItem *> *colours = [NSMutableArray new];
    for (NSInteger i = 0; i < 7; ++i) {
        // One row per colour; the chosen one is marked [+].
        [colours addObject:[Theft4ModItem toggle:AccentNames()[i] help:@"Highlight colour"
            isOn:^BOOL { Theft4ModMenu *menu = weakSelf; return menu && menu->_accentIndex == i; }
            setOn:^(BOOL on) {
                Theft4ModMenu *menu = weakSelf;
                if (!menu) return;
                menu->_accentIndex = i;
                [NSUserDefaults.standardUserDefaults setInteger:i forKey:kAccentKey];
                [menu applyAppearance];
            }]];
    }
    Theft4ModItem *opacity = [Theft4ModItem new];
    opacity.title = @"Menu Opacity";
    opacity.help = @"Background transparency; text stays readable";
    opacity.kind = Theft4ModItemValue;
    opacity.minimum = 0; opacity.maximum = 100; opacity.step = 10;
    opacity.getValue = ^NSInteger { Theft4ModMenu *menu = weakSelf; return menu ? menu->_opacity : 90; };
    opacity.setValue = ^(NSInteger value) {
        Theft4ModMenu *menu = weakSelf;
        if (!menu) return;
        menu->_opacity = value;
        [NSUserDefaults.standardUserDefaults setInteger:value forKey:kOpacityKey];
        [menu applyAppearance];
    };
    opacity.format = ^NSString *(NSInteger v) { return [NSString stringWithFormat:@"%ld%%", (long)v]; };
    return [Theft4ModItem submenu:@"Settings" help:@"Menu colour and transparency" children:@[
        [Theft4ModItem submenu:@"Accent Colour" help:@"Highlight colour" children:colours],
        opacity,
        [Theft4ModItem submenu:@"Diagnostics" help:@"What the game reports to the mods" children:[self diagnosticsItems]],
    ]];
}

// Read-only rows: select one to read its state in the help line.
- (NSArray<Theft4ModItem *> *)diagnosticsItems {
    __weak Theft4ModMenu *weakSelf = self;
    Theft4ModItem *(^row)(NSString *, NSString *(^)(void)) = ^Theft4ModItem *(NSString *title, NSString *(^text)(void)) {
        Theft4ModItem *item = [Theft4ModItem action:title help:nil run:nil];
        item.helpText = text;
        return item;
    };
    return @[
        row(@"Mods", ^NSString * { return [Theft4ModMenu trainerDiagnostics]; }),
        row(@"Episode Start", ^NSString * { return [Theft4ModMenu episodeDiagnostics]; }),
        row(@"Touchpad", ^NSString * {
            Theft4ModMenu *menu = weakSelf;
            if (!menu || !menu->_clickCount) return @"No touchpad clicks yet";
            return [NSString stringWithFormat:@"%lu clicks · last %@", (unsigned long)menu->_clickCount,
                menu->_lastClickText];
        }),
        [self testVibrationItem],
        row(@"Controller Rumble", ^NSString * {
            NSDictionary *rumble = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"Theft4RumbleDiagnostics"];
            if (!rumble.count) return @"No rumble requests yet";
            return [NSString stringWithFormat:@"%@ requests, %@ nonzero · %@ players · %@ resets · %@ · %@",
                rumble[@"requests"] ?: @0, rumble[@"nonzero_requests"] ?: @0,
                rumble[@"player_starts"] ?: @0, rumble[@"engine_resets"] ?: @0,
                rumble[@"haptics_mode"] ?: @"?", rumble[@"last_error"] ?: @"no errors"];
        }),
    ];
}

// The launcher's test pulse, played during the game: felt here but not in
// play points at the game's rumble path rather than the controller.
- (Theft4ModItem *)testVibrationItem {
    __weak Theft4ModMenu *weakSelf = self;
    Theft4ModItem *item = [Theft4ModItem action:@"Test Vibration" help:nil run:^{
        Theft4ModMenu *menu = weakSelf;
        if (menu.testVibration) menu.testVibration();
    }];
    item.helpText = ^NSString * {
        return [@"Plays the launcher's test pulse now. Last result: " stringByAppendingString:
            [NSUserDefaults.standardUserDefaults stringForKey:@"Theft4RumbleLauncherTest"] ?: @"not run"];
    };
    return item;
}

+ (NSString *)trainerDiagnostics {
#ifdef THEFT4_HAS_GAME_STARTUP
    char text[512] = {0};
    theft4_trainer_diagnostics(text, sizeof(text));
    return [NSString stringWithUTF8String:text] ?: @"";
#else
    return @"Not available in this build";
#endif
}

+ (NSString *)episodeDiagnostics {
#ifdef THEFT4_HAS_GAME_STARTUP
    char text[256] = {0};
    theft4_episode_diagnostics(text, sizeof(text));
    return [NSString stringWithUTF8String:text] ?: @"";
#else
    return @"Not available in this build";
#endif
}

#pragma mark - Persistence

+ (NSDictionary<NSString *, NSNumber *> *)persistentSettings {
#ifdef THEFT4_HAS_GAME_STARTUP
    return @{
        @"Theft4ModGodMode": @(THEFT4_TRAINER_GOD_MODE),
        @"Theft4ModUnlimitedAmmo": @(THEFT4_TRAINER_UNLIMITED_AMMO),
        @"Theft4ModNeverWanted": @(THEFT4_TRAINER_NEVER_WANTED),
        @"Theft4ModInfiniteSprint": @(THEFT4_TRAINER_INFINITE_SPRINT),
        @"Theft4ModNoRagdoll": @(THEFT4_TRAINER_NO_RAGDOLL),
        @"Theft4ModCarGodMode": @(THEFT4_TRAINER_CAR_GOD_MODE),
        @"Theft4ModRadioOnFoot": @(THEFT4_TRAINER_RADIO_ON_FOOT),
        @"Theft4ModHideHUD": @(THEFT4_TRAINER_HIDE_HUD),
        @"Theft4ModExplosiveRounds": @(THEFT4_TRAINER_EXPLOSIVE_ROUNDS),
    };
#else
    return @{};
#endif
}

+ (BOOL)cheatsEnabled {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    return ![defaults objectForKey:kCheatsEnabledKey] || [defaults boolForKey:kCheatsEnabledKey];
}

+ (void)setCheatsEnabled:(BOOL)enabled {
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:kCheatsEnabledKey];
    TRAINER_SET_ENABLED(enabled ? 1 : 0);
}

+ (void)applyPersistedSettings {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    TRAINER_SET_ENABLED([self cheatsEnabled] ? 1 : 0);
    [[self persistentSettings] enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSNumber *setting, BOOL *stop) {
        TRAINER_SET((theft4_trainer_setting)setting.integerValue, [defaults boolForKey:key] ? 1 : 0);
    }];
}

#pragma mark - Menu model

static NSString *DefaultsKeyFor(NSInteger setting) {
    for (NSString *key in [Theft4ModMenu persistentSettings])
        if ([Theft4ModMenu persistentSettings][key].integerValue == setting) return key;
    return nil;
}

- (Theft4ModItem *)trainerToggle:(NSString *)title help:(NSString *)help setting:(NSInteger)setting {
    __weak Theft4ModMenu *weakSelf = self;
    return [Theft4ModItem toggle:title help:help
        isOn:^BOOL { return TRAINER_GET((theft4_trainer_setting)setting) != 0; }
        setOn:^(BOOL on) {
            TRAINER_SET((theft4_trainer_setting)setting, on ? 1 : 0);
            NSString *key = DefaultsKeyFor(setting);
            if (key) {
                [NSUserDefaults.standardUserDefaults setBool:on forKey:key];
                Theft4ModMenu *menu = weakSelf;
                if (menu.persistentSettingChanged) menu.persistentSettingChanged(key, on);
            }
        }];
}

- (Theft4ModItem *)trainerAction:(NSString *)title help:(NSString *)help action:(NSInteger)action {
    return [Theft4ModItem action:title help:help run:^{ TRAINER_RUN((theft4_trainer_action)action); }];
}

- (Theft4ModItem *)trainerValue:(NSString *)title help:(NSString *)help setting:(NSInteger)setting
                        minimum:(NSInteger)minimum maximum:(NSInteger)maximum step:(NSInteger)step
                         format:(NSString *(^)(NSInteger))format {
    Theft4ModItem *item = [Theft4ModItem new];
    item.title = title; item.help = help; item.kind = Theft4ModItemValue;
    item.minimum = minimum; item.maximum = maximum; item.step = step;
    item.getValue = ^NSInteger { return TRAINER_GET((theft4_trainer_setting)setting); };
    item.setValue = ^(NSInteger value) { TRAINER_SET((theft4_trainer_setting)setting, (int)value); };
    item.format = format;
    return item;
}

- (Theft4ModItem *)roadsPreset:(NSString *)title help:(NSString *)help traffic:(int)traffic peds:(int)peds {
    return [Theft4ModItem action:title help:help run:^{
        TRAINER_SET(THEFT4_TRAINER_TRAFFIC_DENSITY, traffic);
        TRAINER_SET(THEFT4_TRAINER_PED_DENSITY, peds);
    }];
}

- (NSArray<Theft4ModItem *> *)trafficItemsWithPercent:(NSString *(^)(NSInteger))percent {
    NSArray<NSString *> *mixes = @[@"Off", @"Sports Cars", @"Police", @"Unusual Vehicles"];
    Theft4ModItem *mix = [self trainerValue:@"Extra Traffic"
        help:@"Adds vehicles to the streets: sports cars, police, or airport tugs, trash trucks and other work vehicles"
        setting:THEFT4_TRAINER_EXTRA_TRAFFIC minimum:0 maximum:3 step:1 format:^NSString *(NSInteger v) {
            return mixes[MAX(0, MIN(3, v))];
        }];
    mix.wraps = YES;
    return @[
        [self roadsPreset:@"Empty Roads" help:@"Few cars and people" traffic:25 peds:50],
        [self roadsPreset:@"Normal Roads" help:@"The game's own traffic" traffic:100 peds:100],
        [self roadsPreset:@"Busy Roads" help:@"Heavy traffic and crowds" traffic:200 peds:150],
        [self trainerValue:@"Traffic Density" help:@"Cars on the streets"
            setting:THEFT4_TRAINER_TRAFFIC_DENSITY minimum:0 maximum:200 step:25 format:percent],
        [self trainerValue:@"Pedestrian Density" help:@"People on the streets"
            setting:THEFT4_TRAINER_PED_DENSITY minimum:0 maximum:200 step:25 format:percent],
        mix,
        [self trainerValue:@"Extra Traffic Interval" help:@"Seconds between added vehicles"
            setting:THEFT4_TRAINER_EXTRA_TRAFFIC_RATE minimum:2 maximum:15 step:1 format:^NSString *(NSInteger v) {
                return [NSString stringWithFormat:@"%lds", (long)v];
            }],
    ];
}

static NSArray<NSArray<NSString *> *> *VehicleGroups(void) {
    // Display name, model name. Models missing from the running game or
    // episode are reported on spawn rather than hidden.
    return @[
        @[@"Sports", @"Banshee:banshee", @"Comet:comet", @"Coquette:coquette", @"Feltzer:feltzer",
          @"Infernus:infernus", @"Sultan RS:sultanrs", @"Super GT:supergt", @"Turismo:turismo",
          @"Bullet GT:bullet", @"F620:f620", @"Super Diamond:superd"],
        @[@"Muscle", @"Buccaneer:buccaneer", @"Dukes:dukes", @"Faction:faction", @"Ruiner:ruiner",
          @"Sabre:sabre", @"Sabre GT:sabregt", @"Stallion:stalion", @"Vigero:vigero", @"Voodoo:voodoo"],
        @[@"Sedans & Coupes", @"Admiral:admiral", @"Blista Compact:blista", @"Cognoscenti:cognoscenti",
          @"Emperor:emperor", @"Esperanto:esperanto", @"Feroci:feroci", @"Fortune:fortune", @"Futo:futo",
          @"Hakumai:hakumai", @"Ingot:ingot", @"Intruder:intruder", @"Marbella:marbella", @"Merit:merit",
          @"Oracle:oracle", @"PMP 600:pmp600", @"Premier:premier", @"Presidente:pres", @"Primo:primo",
          @"Schafter:schafter", @"Sentinel:sentinel", @"Solair:solair", @"Stratum:stratum",
          @"Sultan:sultan", @"Vincent:vincent", @"Washington:washington", @"Willard:willard",
          @"Chavos:chavos", @"Dilettante:dilettante", @"Uranus:uranus", @"DF8-90:df8",
          @"Perennial:perennial", @"Serrano:serrano", @"Buffalo:buffalo", @"Stretch:stretch"],
        @[@"SUVs & Vans", @"Cavalcade:cavalcade", @"FXT:fxt", @"Habanero:habanero", @"Huntley Sport:huntley",
          @"Landstalker:landstalker", @"Patriot:patriot", @"Rancher:rancher", @"Rebla:rebla",
          @"Bobcat:bobcat", @"Peyote:peyote", @"Moonbeam:moonbeam", @"Minivan:minivan",
          @"Burrito:burrito", @"Speedo:speedo", @"Pony:pony", @"Boxville:boxville"],
        @[@"Work & Service", @"Taxi:taxi", @"Cabby:cabby", @"Bus:bus", @"Mr. Tasty:mrtasty",
          @"Trashmaster:trash", @"Flatbed:flatbed", @"Mule:mule", @"Benson:benson", @"Yankee:yankee",
          @"Biff:biff", @"Steed:steed", @"Packer:packer", @"Phantom:phantom", @"Forklift:forklift",
          @"Airtug:airtug", @"Romero:romero"],
        @[@"Emergency", @"Police Cruiser:police", @"Police Stinger:police2", @"Police Patriot:polpatriot",
          @"NOOSE Cruiser:noose", @"NOOSE Stockade:nstockade", @"FIB Buffalo:fbi",
          @"Police Stockade:pstockade", @"Ambulance:ambulance", @"Fire Truck:firetruk", @"APC:apc"],
        @[@"Motorcycles", @"Faggio:faggio", @"NRG 900:nrg900", @"PCJ-600:pcj", @"Sanchez:sanchez",
          @"Hellfury:hellfury", @"Zombie:zombieb", @"Akuma:akuma", @"Double T:double", @"Vader:vader",
          @"Hexer:hexer", @"Innovation:innovation", @"Daemon:daemon", @"Police Bike:policeb"],
        @[@"Helicopters", @"Annihilator:annihilator", @"Maverick:maverick", @"Police Maverick:polmav",
          @"Tour Maverick:tourmav", @"Buzzard:buzzard", @"Swift:swift"],
        @[@"Boats", @"Dinghy:dinghy", @"Jetmax:jetmax", @"Marquis:marquis", @"Predator:predator",
          @"Reefer:reefer", @"Squalo:squalo", @"Tropic:tropic", @"Tug:tuga", @"Floater:floater",
          @"Smuggler:smuggler"],
    ];
}

- (Theft4ModItem *)buildRootWithOverlays:(NSArray<Theft4ModItem *> *)overlays {
    NSString *(^plain)(NSInteger) = ^NSString *(NSInteger v) { return [NSString stringWithFormat:@"%ld", (long)v]; };
    NSString *(^percent)(NSInteger) = ^NSString *(NSInteger v) { return [NSString stringWithFormat:@"%ld%%", (long)v]; };

    NSArray *player = @[
        [self trainerToggle:@"God Mode" help:@"No damage or death for the player"
            setting:THEFT4_TRAINER_GOD_MODE],
        [self trainerToggle:@"Never Wanted" help:@"Police never chase you"
            setting:THEFT4_TRAINER_NEVER_WANTED],
        [self trainerValue:@"Wanted Level" help:@"Applied as soon as it changes"
            setting:THEFT4_TRAINER_WANTED_LEVEL minimum:0 maximum:6 step:1 format:^NSString *(NSInteger v) {
                return v ? [@"" stringByPaddingToLength:v withString:@"★" startingAtIndex:0] : @"None";
            }],
        [self trainerToggle:@"Infinite Sprint" help:@"Run without getting tired"
            setting:THEFT4_TRAINER_INFINITE_SPRINT],
        [self trainerToggle:@"No Ragdoll" help:@"Stay on your feet and on your bike"
            setting:THEFT4_TRAINER_NO_RAGDOLL],
        [self trainerAction:@"Ragdoll Player" help:@"Fall over now" action:THEFT4_TRAINER_RAGDOLL_PLAYER],
        [self trainerToggle:@"Invisible" help:@"Hide the player model"
            setting:THEFT4_TRAINER_INVISIBLE],
        [self trainerToggle:@"No Clip" help:@"Fly through walls · LB down, RB up, RT faster"
            setting:THEFT4_TRAINER_NOCLIP],
        [self trainerValue:@"No Clip & Camera Speed" help:@"Flying speed"
            setting:THEFT4_TRAINER_NOCLIP_SPEED minimum:1 maximum:10 step:1 format:plain],
        [self trainerToggle:@"Free Camera" help:@"Fly the camera · with No Clip, you follow it"
            setting:THEFT4_TRAINER_FREECAM],
        [self trainerValue:@"Free Camera Field of View" help:@"Degrees"
            setting:THEFT4_TRAINER_FREECAM_FOV minimum:30 maximum:100 step:5 format:^NSString *(NSInteger v) {
                return [NSString stringWithFormat:@"%ld°", (long)v];
            }],
        [self trainerAction:@"Refill Health & Armor" help:@"Full health and armor"
            action:THEFT4_TRAINER_REFILL_HEALTH_ARMOR],
        [self trainerAction:@"Health & Armor to Minimum" help:@"One hit from death"
            action:THEFT4_TRAINER_MINIMUM_HEALTH_ARMOR],
        [Theft4ModItem submenu:@"Money" help:@"Change your cash" children:@[
            [self trainerAction:@"Add $1,000" help:nil action:THEFT4_TRAINER_MONEY_ADD_1K],
            [self trainerAction:@"Add $10,000" help:nil action:THEFT4_TRAINER_MONEY_ADD_10K],
            [self trainerAction:@"Add $100,000" help:nil action:THEFT4_TRAINER_MONEY_ADD_100K],
            [self trainerAction:@"Set to $0" help:nil action:THEFT4_TRAINER_MONEY_ZERO]]],
        [self trainerToggle:@"Radio On Foot" help:@"Hear the radio while walking"
            setting:THEFT4_TRAINER_RADIO_ON_FOOT],
        [self trainerAction:@"Next Radio Station" help:nil action:THEFT4_TRAINER_RADIO_NEXT],
        [self trainerAction:@"Previous Radio Station" help:nil action:THEFT4_TRAINER_RADIO_PREVIOUS],
        [self trainerAction:@"Skip Radio Track" help:nil action:THEFT4_TRAINER_RADIO_SKIP_TRACK],
    ];

    NSMutableArray *groups = [NSMutableArray new];
    for (NSArray<NSString *> *group in VehicleGroups()) {
        NSMutableArray *models = [NSMutableArray new];
        for (NSUInteger i = 1; i < group.count; ++i) {
            NSArray<NSString *> *parts = [group[i] componentsSeparatedByString:@":"];
            NSString *model = parts[1];
            [models addObject:[Theft4ModItem action:parts[0] help:[@"Model " stringByAppendingString:model]
                run:^{ TRAINER_SPAWN(model.UTF8String); }]];
        }
        [groups addObject:[Theft4ModItem submenu:group[0] help:nil children:models]];
    }
    NSArray *vehicle = @[
        [Theft4ModItem submenu:@"Spawn Vehicle" help:@"Any car, bike, helicopter or boat"
            children:[@[[self trainerToggle:@"Spawn Inside" help:@"Put the player in the new vehicle"
                setting:THEFT4_TRAINER_SPAWN_INSIDE]] arrayByAddingObjectsFromArray:groups]],
        [self trainerToggle:@"Car God Mode" help:@"Your current vehicle takes no damage"
            setting:THEFT4_TRAINER_CAR_GOD_MODE],
        [self trainerAction:@"Boost Forward" help:@"A burst of speed" action:THEFT4_TRAINER_BOOST_FORWARD],
        [self trainerAction:@"Boost Up" help:@"Launch the vehicle upward" action:THEFT4_TRAINER_BOOST_UP],
        [self trainerValue:@"Boost Strength" help:@"For Boost Forward and Boost Up"
            setting:THEFT4_TRAINER_BOOST_STRENGTH minimum:1 maximum:10 step:1 format:plain],
        [self trainerAction:@"Fix Vehicle" help:nil action:THEFT4_TRAINER_FIX_VEHICLE],
        [self trainerToggle:@"Auto-Repair" help:@"Repair damage as it happens"
            setting:THEFT4_TRAINER_AUTO_REPAIR],
        [self trainerToggle:@"Invisible Vehicle" help:nil setting:THEFT4_TRAINER_CAR_INVISIBLE],
        [self trainerToggle:@"Horn Boost" help:@"Hold the horn to accelerate"
            setting:THEFT4_TRAINER_HORN_BOOST],
        [self trainerToggle:@"Instant Brake" help:@"Stop dead the moment you brake"
            setting:THEFT4_TRAINER_INSTANT_BRAKE],
        [self trainerAction:@"Explode Vehicle" help:nil action:THEFT4_TRAINER_EXPLODE_VEHICLE],
        [self trainerAction:@"Burst Tyres" help:nil action:THEFT4_TRAINER_BURST_TYRES],
        [self trainerAction:@"Wash Vehicle" help:nil action:THEFT4_TRAINER_WASH_VEHICLE],
        [self trainerAction:@"Set On Wheels" help:@"Flip the vehicle upright"
            action:THEFT4_TRAINER_VEHICLE_ON_GROUND],
        [self trainerAction:@"Delete Vehicle" help:@"Step out and remove it"
            action:THEFT4_TRAINER_DELETE_VEHICLE],
    ];

    NSArray *teleport = @[
        [self trainerAction:@"To Waypoint" help:@"Place a waypoint on the map first"
            action:THEFT4_TRAINER_TELEPORT_WAYPOINT],
        [self trainerAction:@"Save Position" help:@"Remember where you are"
            action:THEFT4_TRAINER_SAVE_POSITION],
        [self trainerAction:@"Return to Saved Position" help:nil action:THEFT4_TRAINER_LOAD_POSITION],
    ];

    NSArray *weapon = @[
        [self trainerToggle:@"Unlimited Ammo" help:@"Reserve ammo never runs out"
            setting:THEFT4_TRAINER_UNLIMITED_AMMO],
        [self trainerToggle:@"Explosive Rounds"
            help:@"TBoGT: your gun fires like the explosive AA-12, exploding where bullets land (others carrying the same gun too). Base game: explodes on the first person or vehicle in your aim, or the ground"
            setting:THEFT4_TRAINER_EXPLOSIVE_ROUNDS],
        [self trainerAction:@"Give All Weapons" help:@"Every weapon with full ammo"
            action:THEFT4_TRAINER_GIVE_ALL_WEAPONS],
        [self trainerAction:@"Remove All Weapons" help:nil action:THEFT4_TRAINER_REMOVE_ALL_WEAPONS],
    ];

    NSArray<NSString *> *weathers = @[@"Extra Sunny", @"Sunny", @"Sunny & Windy", @"Cloudy",
                                      @"Rain", @"Drizzle", @"Foggy", @"Thunderstorm"];
    NSArray<NSString *> *police = @[@"Relaxed", @"Normal", @"Aggressive", @"Relentless"];
    Theft4ModItem *weather = [self trainerValue:@"Weather" help:@"Changes the weather now"
        setting:THEFT4_TRAINER_WEATHER minimum:0 maximum:7 step:1 format:^NSString *(NSInteger v) {
            return weathers[MAX(0, MIN(7, v))];
        }];
    weather.wraps = YES;
    Theft4ModItem *hour = [self trainerValue:@"Time of Day" help:@"Changes the clock now"
        setting:THEFT4_TRAINER_HOUR minimum:0 maximum:23 step:1 format:^NSString *(NSInteger v) {
            return [NSString stringWithFormat:@"%02ld:00", (long)v];
        }];
    hour.wraps = YES;
    NSArray *world = @[
        hour,
        [self trainerToggle:@"Freeze Time" help:@"Keep the clock at the chosen hour"
            setting:THEFT4_TRAINER_FREEZE_TIME],
        weather,
        [self trainerToggle:@"Lock Weather" help:@"Keep the chosen weather"
            setting:THEFT4_TRAINER_LOCK_WEATHER],
        [Theft4ModItem submenu:@"Traffic" help:@"Road and pedestrian traffic"
            children:[self trafficItemsWithPercent:percent]],
        [self trainerValue:@"Police Response" help:@"How hard the police come after you"
            setting:THEFT4_TRAINER_POLICE_RESPONSE minimum:0 maximum:3 step:1 format:^NSString *(NSInteger v) {
                return police[MAX(0, MIN(3, v))];
            }],
        [self trainerToggle:@"Hide HUD & Radar" help:@"Cinematic view"
            setting:THEFT4_TRAINER_HIDE_HUD],
    ];

    return [Theft4ModItem submenu:@"Mod Menu" help:nil children:@[
        [Theft4ModItem submenu:@"Player" help:nil children:player],
        [Theft4ModItem submenu:@"Vehicle" help:nil children:vehicle],
        [Theft4ModItem submenu:@"Teleport" help:nil children:teleport],
        [Theft4ModItem submenu:@"Weapon" help:nil children:weapon],
        [Theft4ModItem submenu:@"World" help:nil children:world],
        [Theft4ModItem submenu:@"Display" help:@"Performance graphs and on-screen controls"
            children:overlays ?: @[]],
        [self settingsMenu],
    ]];
}

#pragma mark - View

static UILabel *MenuLabel(CGFloat size, UIFontWeight weight, BOOL mono) {
    UILabel *label = [UILabel new];
    label.font = mono ? [UIFont monospacedSystemFontOfSize:size weight:weight]
                      : [UIFont systemFontOfSize:size weight:weight];
    label.textColor = Ink(0xE4E4E4);
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.75;
    return label;
}

- (instancetype)initWithOverlays:(NSArray<Theft4ModItem *> *)overlays {
    self = [super initWithFrame:CGRectZero];
    if (!self) return nil;
    _visibleRows = kMaxRows;
    [self loadAppearance];
    _root = [self buildRootWithOverlays:overlays];
    _stack = [NSMutableArray arrayWithObject:_root];
    _selections = [NSMutableArray arrayWithObject:@0];
    self.hidden = YES;
    self.userInteractionEnabled = YES;

    _panel = [UIView new];
    _panel.backgroundColor = [Ink(0x2A2D2E) colorWithAlphaComponent:0.90];
    _panel.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.10].CGColor;
    _panel.layer.borderWidth = 1;
    _panel.clipsToBounds = YES;
    [self addSubview:_panel];

    _header = [UIView new];
    _header.backgroundColor = UIColor.blackColor;
    [_panel addSubview:_header];
    _title = MenuLabel(17, UIFontWeightBold, YES);
    _title.textAlignment = NSTextAlignmentCenter;
    [_header addSubview:_title];
    _subtitle = MenuLabel(9, UIFontWeightMedium, YES);
    _subtitle.textAlignment = NSTextAlignmentCenter;
    _subtitle.textColor = Ink(0x9A9A9A);
    _subtitle.text = @"THEFT4";
    [_header addSubview:_subtitle];
    _accentBar = [UIView new];
    [_panel addSubview:_accentBar];

    NSMutableArray *rows = [NSMutableArray new], *titles = [NSMutableArray new], *values = [NSMutableArray new];
    for (NSInteger i = 0; i < kMaxRows; ++i) {
        UIView *row = [UIView new];
        row.tag = i;
        UILabel *title = MenuLabel(13, UIFontWeightRegular, NO);
        UILabel *value = MenuLabel(12, UIFontWeightSemibold, YES);
        value.textAlignment = NSTextAlignmentRight;
        [row addSubview:title];
        [row addSubview:value];
        [row addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(rowTapped:)]];
        [_panel addSubview:row];
        [rows addObject:row]; [titles addObject:title]; [values addObject:value];
    }
    _rows = rows; _rowTitles = titles; _rowValues = values;

    _footer = [UIView new];
    _footer.backgroundColor = UIColor.blackColor;
    [_panel addSubview:_footer];
    _counter = MenuLabel(13, UIFontWeightBold, YES);
    _counter.textAlignment = NSTextAlignmentCenter;
    [_footer addSubview:_counter];

    _helpBar = [UIView new];
    _helpBar.backgroundColor = [Ink(0x2A2D2E) colorWithAlphaComponent:0.90];
    [self addSubview:_helpBar];
    _help = MenuLabel(11, UIFontWeightRegular, NO);
    _help.numberOfLines = 5;
    _help.adjustsFontSizeToFitWidth = NO;
    [_helpBar addSubview:_help];
    _status = MenuLabel(11, UIFontWeightSemibold, NO);
    _status.textColor = Ink(0x8FD18F);
    _status.numberOfLines = 1;
    [_helpBar addSubview:_status];

    UITapGestureRecognizer *backTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(back)];
    [_header addGestureRecognizer:backTap];
    [self applyAppearance];
    return self;
}

// Touches outside the menu reach the game.
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    UIView *hit = [super hitTest:point withEvent:event];
    return hit == self ? nil : hit;
}

- (void)setTopOffset:(CGFloat)topOffset {
    if (_topOffset == topOffset) return;
    _topOffset = topOffset;
    [self setNeedsLayout];
}

// Top right, below the frame-time graph or FPS counter when one is shown.
// Rows are dropped (down to four) when the screen is too short for eight.
- (void)layoutSubviews {
    [super layoutSubviews];
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat width = MIN(kPanelWidth, self.bounds.size.width - safe.left - safe.right - 20);
    const CGFloat y = safe.top + _topOffset;
    const CGFloat room = self.bounds.size.height - safe.bottom - 10 - y - kHeaderHeight - kFooterHeight -
                         6 - kHelpHeight;
    const NSInteger rows = MAX(4, MIN(kMaxRows, (NSInteger)floor(room / kRowHeight)));
    if (rows != _visibleRows) {
        _visibleRows = rows;
        [self refresh];
    }
    const CGFloat height = kHeaderHeight + kRowHeight * rows + kFooterHeight;
    const CGFloat x = self.bounds.size.width - safe.right - 10 - width;
    _panel.frame = CGRectMake(x, y, width, height);
    _header.frame = CGRectMake(0, 0, width, kHeaderHeight);
    _accentBar.frame = CGRectMake(0, kHeaderHeight - 2, width, 2);
    _title.frame = CGRectMake(10, 5, width - 20, 22);
    _subtitle.frame = CGRectMake(10, 27, width - 20, 12);
    for (NSInteger i = 0; i < kMaxRows; ++i) {
        _rows[i].frame = CGRectMake(0, kHeaderHeight + i * kRowHeight, width, kRowHeight);
        _rowTitles[i].frame = CGRectMake(10, 0, width * 0.60, kRowHeight);
        _rowValues[i].frame = CGRectMake(width * 0.60, 0, width * 0.40 - 10, kRowHeight);
    }
    _footer.frame = CGRectMake(0, height - kFooterHeight, width, kFooterHeight);
    _counter.frame = _footer.bounds;
    // The help line grows to fit its text (up to five lines); the newest
    // result sits under it while it is fresh.
    const CGFloat helpHeight = MIN(70.0, ceil([_help sizeThatFits:CGSizeMake(width - 20, CGFLOAT_MAX)].height));
    const CGFloat statusHeight = _status.hidden ? 0 : 15;
    _helpBar.frame = CGRectMake(x, CGRectGetMaxY(_panel.frame) + 6, width, 4 + helpHeight + statusHeight + 4);
    _help.frame = CGRectMake(10, 4, width - 20, helpHeight);
    _status.frame = CGRectMake(10, 4 + helpHeight, width - 20, statusHeight);
    _helpBar.hidden = !_help.text.length && _status.hidden;
}

- (Theft4ModItem *)level { return _stack.lastObject; }
- (NSInteger)selected { return _selections.lastObject.integerValue; }
- (void)setSelected:(NSInteger)index { _selections[_selections.count - 1] = @(index); }

- (NSString *)valueTextFor:(Theft4ModItem *)item selected:(BOOL)selected {
    switch (item.kind) {
        case Theft4ModItemSubmenu: return @">";
        case Theft4ModItemToggle: return item.isOn && item.isOn() ? @"[+]" : @"[x]";
        case Theft4ModItemAction: return @"";
        case Theft4ModItemValue: {
            const NSInteger v = item.getValue ? item.getValue() : 0;
            NSString *text = item.format ? item.format(v) : [NSString stringWithFormat:@"%ld", (long)v];
            return selected ? [NSString stringWithFormat:@"‹ %@ ›", text] : text;
        }
    }
    return @"";
}

- (void)refresh {
    Theft4ModItem *level = self.level;
    NSArray<Theft4ModItem *> *items = level.children;
    const NSInteger count = (NSInteger)items.count;
    NSInteger selected = MAX(0, MIN(self.selected, count - 1));
    [self setSelected:selected];
    const NSInteger visible = MAX(1, _visibleRows);
    if (selected < _firstVisible) _firstVisible = selected;
    if (selected >= _firstVisible + visible) _firstVisible = selected - visible + 1;
    _firstVisible = MAX(0, MIN(_firstVisible, MAX(0, count - visible)));

    _title.text = level.title.uppercaseString;
    _subtitle.text = _stack.count > 1 ? @"‹ BACK  ·  THEFT4" : @"THEFT4";
    for (NSInteger i = 0; i < kMaxRows; ++i) {
        const NSInteger index = _firstVisible + i;
        UIView *row = _rows[i];
        if (i >= visible || index >= count) { row.hidden = YES; continue; }
        row.hidden = NO;
        Theft4ModItem *item = items[index];
        const BOOL isSelected = index == selected;
        UIColor *accent = self.accent;
        row.backgroundColor = isSelected ? [accent colorWithAlphaComponent:0.16] : UIColor.clearColor;
        _rowTitles[i].text = item.title;
        _rowTitles[i].textColor = isSelected ? accent : Ink(0xE4E4E4);
        _rowValues[i].text = [self valueTextFor:item selected:isSelected];
        const BOOL on = item.kind == Theft4ModItemToggle && item.isOn && item.isOn();
        _rowValues[i].textColor = item.kind == Theft4ModItemToggle ? (on ? Ink(0x4CD964) : Ink(0xE5484D))
                                 : isSelected ? accent : Ink(0xBDBDBD);
    }
    _counter.text = count ? [NSString stringWithFormat:@"%ld / %ld", (long)selected + 1, (long)count] : @"0 / 0";
    Theft4ModItem *current = count ? items[selected] : nil;
    NSString *help = current.helpText ? current.helpText() : current.help;
    if (!TRAINER_ACTIVE() && !current.helpText) help = @"Mods start once you are in Liberty City";
    else if (![Theft4ModMenu cheatsEnabled] && !current.helpText)
        help = @"Cheats are off. Turn them on in File › Cheats";
    _help.text = help.length ? [@"(i) " stringByAppendingString:help] : @"";
    _status.hidden = CACurrentMediaTime() > _statusUntil;
    [self setNeedsLayout];
}

#pragma mark - Navigation

- (void)setOpen:(BOOL)open {
    if (_open == open) return;
    _open = open;
    self.hidden = !open;
    if (open) [self refresh];
}

- (void)moveSelection:(NSInteger)delta {
    const NSInteger count = (NSInteger)self.level.children.count;
    if (!count) return;
    [self setSelected:(self.selected + delta + count) % count];
    [self refresh];
}

- (void)adjust:(Theft4ModItem *)item direction:(NSInteger)direction {
    const NSInteger step = MAX(1, item.step);
    NSInteger value = (item.getValue ? item.getValue() : item.minimum) + direction * step;
    if (item.wraps) {
        const NSInteger span = item.maximum - item.minimum + step;
        value = item.minimum + ((value - item.minimum) % span + span) % span;
    } else {
        value = MAX(item.minimum, MIN(item.maximum, value));
    }
    if (item.setValue) item.setValue(value);
    [self refresh];
}

- (void)activate {
    NSArray<Theft4ModItem *> *items = self.level.children;
    if (!items.count) return;
    Theft4ModItem *item = items[self.selected];
    switch (item.kind) {
        case Theft4ModItemSubmenu:
            [_stack addObject:item];
            [_selections addObject:@0];
            _firstVisible = 0;
            break;
        case Theft4ModItemToggle:
            if (item.setOn) item.setOn(!(item.isOn && item.isOn()));
            break;
        case Theft4ModItemAction:
            if (item.run) item.run();
            break;
        case Theft4ModItemValue:
            [self adjust:item direction:1];
            return;
    }
    [self refresh];
}

- (void)back {
    if (_stack.count <= 1) { [self setOpen:NO]; return; }
    [_stack removeLastObject];
    [_selections removeLastObject];
    _firstVisible = 0;
    [self refresh];
}

// Right selects or raises; left goes back or lowers.
- (void)horizontal:(NSInteger)direction {
    NSArray<Theft4ModItem *> *items = self.level.children;
    Theft4ModItem *item = items.count ? items[self.selected] : nil;
    if (item.kind == Theft4ModItemValue) {
        [self adjust:item direction:direction];
    } else if (direction > 0) {
        [self activate];
    } else {
        [self back];
    }
}

- (void)rowTapped:(UITapGestureRecognizer *)tap {
    const NSInteger index = _firstVisible + tap.view.tag;
    NSArray<Theft4ModItem *> *items = self.level.children;
    if (index >= (NSInteger)items.count) return;
    [self setSelected:index];
    Theft4ModItem *item = items[index];
    if (item.kind == Theft4ModItemValue) {
        const CGPoint p = [tap locationInView:tap.view];
        [self adjust:item direction:p.x < tap.view.bounds.size.width / 2 ? -1 : 1];
    } else {
        [self activate];
    }
}

#pragma mark - Touchpad

- (void)setGameActive:(BOOL)gameActive {
    _gameActive = gameActive;
    if (gameActive && !_poll) {
        _poll = [CADisplayLink displayLinkWithTarget:self selector:@selector(pollController)];
        // Fast enough to see both presses of a quick double-click.
        _poll.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
        [_poll addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    } else if (!gameActive && _poll) {
        [_poll invalidate];
        _poll = nil;
        [self setOpen:NO];
    }
}

- (void)removeFromSuperview {
    [_poll invalidate];
    _poll = nil;
    [super removeFromSuperview];
}

- (GCDualSenseGamepad *)dualSense {
    for (GCController *controller in GCController.controllers)
        if ([controller.extendedGamepad isKindOfClass:GCDualSenseGamepad.class])
            return (GCDualSenseGamepad *)controller.extendedGamepad;
    return nil;
}

- (void)pollController {
    [self showTrainerStatus];
    // Keep the latest diagnostics for the next launch's log export.
    const CFTimeInterval now = CACurrentMediaTime();
    if (now - _diagnosticsSaved > 2.0) {
        _diagnosticsSaved = now;
        [NSUserDefaults.standardUserDefaults setObject:[Theft4ModMenu trainerDiagnostics]
                                                forKey:@"Theft4TrainerDiagnosticsText"];
        [NSUserDefaults.standardUserDefaults setObject:[Theft4ModMenu episodeDiagnostics]
                                                forKey:@"Theft4EpisodeDiagnosticsText"];
    }
    GCDualSenseGamepad *pad = self.dualSense;
    if (!pad) { _wasClicked = _holdFired = NO; return; }
    const CGPoint p = CGPointMake(pad.touchpadPrimary.xAxis.value, pad.touchpadPrimary.yAxis.value);
    // The pad may report the finger at zero on the frame of the click; use
    // where it was just before.
    if (p.x != 0 || p.y != 0) { _lastTouch = p; _lastTouchTime = now; }
    const BOOL clicked = pad.touchpadButton.pressed;
    if (clicked && !_wasClicked) {
        _pressStart = now;
        _pressPoint = (p.x != 0 || p.y != 0) ? p : now - _lastTouchTime < 0.3 ? _lastTouch : CGPointZero;
        _holdFired = NO;
    }
    // Holding the click opens or closes the menu; a shorter click acts when
    // it is released.
    if (clicked && !_holdFired && now - _pressStart >= kHold) {
        _holdFired = YES;
        _lastClickTime = 0;
        [self setOpen:!_open];
    }
    if (!clicked && _wasClicked && !_holdFired) [self touchpadClickedAt:_pressPoint time:now];
    _wasClicked = clicked;
}

// While the menu is open, a click on the top or bottom edge moves the
// selection, right selects, toggles or raises a value, left goes back or
// lowers one, and a double-click in the centre returns to the previous menu
// (closing it at the top level). Edge clicks never count toward a
// double-click, so they can be repeated quickly.
- (void)touchpadClickedAt:(CGPoint)point time:(CFTimeInterval)now {
    const Theft4PadZone zone = PadZone(point);
    ++_clickCount;
    _lastClickText = [NSString stringWithFormat:@"(%.2f, %.2f) %@", point.x, point.y, PadZoneName(zone)];
    if (!_open) return;
    const BOOL centre = zone == Theft4PadCentre;
    const BOOL doubleClick = centre && now - _lastClickTime < kDoubleClick;
    _lastClickTime = centre && !doubleClick ? now : 0;
    if (doubleClick) {
        [self back];
        return;
    }
    switch (zone) {
        case Theft4PadUp: [self moveSelection:-1]; break;
        case Theft4PadDown: [self moveSelection:1]; break;
        case Theft4PadRight: [self horizontal:1]; break;
        case Theft4PadLeft: [self horizontal:-1]; break;
        case Theft4PadCentre: break;
    }
}

- (void)showTrainerStatus {
#ifdef THEFT4_HAS_GAME_STARTUP
    char message[160] = {0};
    const uint32_t counter = theft4_trainer_status(message, sizeof(message));
    if (counter != _statusCounter) {
        _statusCounter = counter;
        _status.text = [NSString stringWithUTF8String:message] ?: @"";
        _statusUntil = CACurrentMediaTime() + 3.0;
    }
#endif
    if (_open) {
        // Toggle states can change from the launcher or the game itself.
        static CFTimeInterval lastRefresh;
        const CFTimeInterval now = CACurrentMediaTime();
        if (now - lastRefresh > 0.25) { lastRefresh = now; [self refresh]; }
    }
}

@end
