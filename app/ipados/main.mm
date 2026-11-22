// SPDX-License-Identifier: GPL-3.0-only

#include "catalog_controller.hpp"
#import "watch_mirror.hpp"
#include "core/app_paths.hpp"
#include "core/i18n.hpp"
#include "core/runtime_journal.hpp"
#include "luna/amazon_auth.hpp"

#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>

@interface LunaAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, copy) NSString* storageError;
@end

@implementation LunaAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options
{
    umask(0077);
    [LunaWatchMirror.shared activate];
    try
    {
        NSURL* support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                              inDomains:NSUserDomainMask].firstObject;
        NSURL* state = [support URLByAppendingPathComponent:@"LunaONE" isDirectory:YES];
        NSURL* legacy = [support URLByAppendingPathComponent:@"LunaCore" isDirectory:YES];
        if (![NSFileManager.defaultManager fileExistsAtPath:state.path] &&
            [NSFileManager.defaultManager fileExistsAtPath:legacy.path])
            state = legacy;
        if (!state.fileSystemRepresentation || setenv("LUNAONE_DATA_DIR", state.fileSystemRepresentation, 1) != 0)
            throw std::runtime_error("Impossible de préparer l’espace de données local.");
        lunanx::PrepareAppStorage();
        NSError* backupError = nil;
        if (![state setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&backupError])
            throw std::runtime_error("Impossible de protéger les données de connexion des sauvegardes.");
        lunanx::luna::DeviceRegistration device;
        if (lunanx::luna::AmazonAuth::Load(device))
            lunanx::SetUiLanguageForMarket(device.market_code);
        lunanx::InitializeRuntimeJournal("LunaONE-0.3.0-ios");
    }
    catch (const std::exception& ex)
    {
        self.storageError = [NSString stringWithUTF8String:ex.what()] ?: @"Erreur de stockage.";
    }
    return YES;
}
@end

@interface LunaSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation LunaSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
                                            options:(UISceneConnectionOptions*)connectionOptions
{
    if (![scene isKindOfClass:UIWindowScene.class])
        return;
    auto delegate = (LunaAppDelegate*)UIApplication.sharedApplication.delegate;
    auto root = [[LunaCatalogController alloc] initWithStorageError:delegate.storageError];
    auto navigation = [[UINavigationController alloc] initWithRootViewController:root];
    navigation.navigationBar.prefersLargeTitles = YES;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.window.tintColor = [UIColor colorWithRed:0.72 green:0.62 blue:1.0 alpha:1.0];
    self.window.rootViewController = navigation;
    [self.window makeKeyAndVisible];
    NSLog(@"LunaONE iOS UI ready");
}
@end

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(LunaAppDelegate.class));
    }
}
