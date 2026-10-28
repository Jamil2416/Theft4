#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, Theft4ModItemKind) {
    Theft4ModItemSubmenu,
    Theft4ModItemToggle,
    Theft4ModItemAction,
    Theft4ModItemValue,
};

// One menu row. Toggles use isOn/setOn, actions run, values use
// getValue/setValue within minimum...maximum in steps of step.
@interface Theft4ModItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *help;
@property(nonatomic) Theft4ModItemKind kind;
@property(nonatomic, copy) NSArray<Theft4ModItem *> *children;
@property(nonatomic, copy) BOOL (^isOn)(void);
@property(nonatomic, copy) void (^setOn)(BOOL on);
@property(nonatomic, copy) void (^run)(void);
@property(nonatomic, copy) NSInteger (^getValue)(void);
@property(nonatomic, copy) void (^setValue)(NSInteger value);
@property(nonatomic, copy) NSString *(^format)(NSInteger value);
@property(nonatomic) NSInteger minimum, maximum, step;
@property(nonatomic) BOOL wraps;
+ (instancetype)submenu:(NSString *)title help:(NSString *)help children:(NSArray<Theft4ModItem *> *)children;
+ (instancetype)toggle:(NSString *)title help:(NSString *)help
                  isOn:(BOOL (^)(void))isOn setOn:(void (^)(BOOL on))setOn;
+ (instancetype)action:(NSString *)title help:(NSString *)help run:(void (^)(void))run;
@end

// The in-game mod menu, drawn over gameplay. A click on the DualSense
// touchpad opens or closes it; sliding up or down moves the selection; a
// slide right selects, toggles or raises a value; a slide left goes back or
// lowers a value. Rows can also be tapped on screen.
@interface Theft4ModMenu : UIView
// overlays: rows for the app's on-screen graphs, added under DISPLAY.
- (instancetype)initWithOverlays:(NSArray<Theft4ModItem *> *)overlays;
@property(nonatomic, readonly, getter=isOpen) BOOL open;
- (void)setOpen:(BOOL)open;
// Touchpad polling runs only while the game is presented.
@property(nonatomic) BOOL gameActive;
// Called after the player changes a persistent setting in game.
@property(nonatomic, copy) void (^persistentSettingChanged)(NSString *defaultsKey, BOOL on);

// Settings that persist between launches, keyed by user-defaults key, as
// NSNumber-wrapped theft4_trainer_setting values.
+ (NSDictionary<NSString *, NSNumber *> *)persistentSettings;
// Copies the persisted values into the trainer before the game starts.
+ (void)applyPersistedSettings;
@end
