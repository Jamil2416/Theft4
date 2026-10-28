#import <UIKit/UIKit.h>

// Presentation only. The app controller owns preferences and the game runtime.
@interface Theft4LauncherView : UIView
@property(nonatomic, readonly) UILabel *statusLabel;
@property(nonatomic, readonly) UILabel *detailLabel;
@property(nonatomic, readonly) UIButton *startButton;
// Shares the start button's row: boots into The Ballad of Gay Tony.
@property(nonatomic, readonly) UIButton *balladButton;
// Under the start row: whether TBoGT is installed and how to add it. Empty
// text hides the line.
- (void)setEpisodeStatus:(NSString *)text;
@property(nonatomic, readonly) UIButton *prepareButton;
@property(nonatomic, readonly) UIButton *restartButton;
@property(nonatomic, readonly) UISwitch *showCPUUsage;
@property(nonatomic, readonly) UISwitch *showFPS;
@property(nonatomic, readonly) UISwitch *showFrameTime;
@property(nonatomic, readonly) UISwitch *showControls;
// Controller light-bar color, 0-255 per channel.
@property(nonatomic, readonly) UISlider *lightRed;
@property(nonatomic, readonly) UISlider *lightGreen;
@property(nonatomic, readonly) UISlider *lightBlue;
// Plays a short pulse on connected controllers through iOS directly.
@property(nonatomic, readonly) UIButton *testVibrationButton;
@property(nonatomic, readonly) UILabel *vibrationTestResult;
@property(nonatomic, readonly) UISwitch *anisotropicFiltering;
@property(nonatomic, readonly) UISwitch *enhancedOutput;
@property(nonatomic, readonly) UISwitch *fsrBoost;
@property(nonatomic, readonly) UISegmentedControl *renderResolution;
@property(nonatomic, readonly) UISwitch *fsrUpscaling;
@property(nonatomic, readonly) UISlider *sharpening;
@property(nonatomic, readonly) uint32_t renderHeight;
@property(nonatomic, readonly) UISwitch *motionBlur;
@property(nonatomic, readonly) UISwitch *depthOfField;
@property(nonatomic, readonly) UIButton *lowPowerButton;
@property(nonatomic, readonly) UIButton *originalPresetButton;
@property(nonatomic, readonly) UISegmentedControl *shadowQuality;
@property(nonatomic, readonly) UISegmentedControl *drawDistance;
@property(nonatomic, readonly) UISegmentedControl *modelDetail;
@property(nonatomic, readonly) UISegmentedControl *reflectionQuality;
@property(nonatomic, readonly) UISegmentedControl *antiAliasing;
@property(nonatomic, readonly) UISwitch *performanceCapture;
@property(nonatomic, readonly) UISwitch *skipIntro;
@property(nonatomic, readonly) UISwitch *customTimeCycle;
@property(nonatomic, readonly) UISwitch *nativeAspect;
@property(nonatomic, readonly) UISwitch *godMode;
@property(nonatomic, readonly) UISwitch *unlimitedAmmo;
// Further trainer switches on the Mods page, keyed by user-defaults key
// (the keys in Theft4ModMenu's persistentSettings).
@property(nonatomic, readonly) NSDictionary<NSString *, UISwitch *> *trainerSwitches;
@property(nonatomic, readonly) UIButton *frameSpeedButton;
@property(nonatomic, readonly) UIButton *restoreGraphicsButton;
#ifdef THEFT4_BC_TEXTURE_COMPATIBILITY
@property(nonatomic, readonly) UISwitch *astcConversion;
@property(nonatomic, readonly) UIButton *deleteTextureCacheButton;
#endif
@property(nonatomic, readonly) UIButton *downloadLogButton;
@property(nonatomic, readonly) UIButton *exportSavesButton;
@property(nonatomic, readonly) UIButton *importSavesButton;
- (void)refreshConfigurationSummary;
- (void)setActive:(BOOL)active;
- (void)retireScene;
@end
