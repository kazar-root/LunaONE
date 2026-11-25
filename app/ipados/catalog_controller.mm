// SPDX-License-Identifier: GPL-3.0-only

#include "catalog_controller.hpp"
#import <QuartzCore/QuartzCore.h>
#include "player_controller.hpp"
#include "core/runtime_journal.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "core/update_check.hpp"
#include "luna/amazon_auth.hpp"
#include "ui/device_qr.hpp"
#include "ui/library_sections.hpp"
#include "ui/image_url_policy.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace
{
using namespace lunanx::luna;
using lunanx::ui::LibrarySection;

NSString* Text(const std::string& value)
{
    return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

NSString* Localized(NSString* key)
{
    return key ? Text(lunanx::Tr(key.UTF8String ?: "")) : nil;
}

UIColor* Violet() { return [UIColor colorWithRed:.67 green:.51 blue:1 alpha:1]; }
UIColor* LibraryInk() { return [UIColor colorWithRed:.06 green:.05 blue:.10 alpha:1]; }

UIImage* QRImage(NSURL* address)
{
    const auto qr = lunanx::ui::MakeDeviceQr(address.absoluteString.UTF8String ?: "");
    if (!qr.size)
        return nil;
    constexpr int scale = 4;
    const auto side = qr.size * scale;
    auto renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
        CGContextSetFillColorWithColor(context.CGContext, UIColor.whiteColor.CGColor);
        CGContextFillRect(context.CGContext, CGRectMake(0, 0, side, side));
        CGContextSetFillColorWithColor(context.CGContext, UIColor.blackColor.CGColor);
        for (int y = 0; y < qr.size; ++y)
            for (int x = 0; x < qr.size; ++x)
                if (qr.dark[y * qr.size + x])
                    CGContextFillRect(context.CGContext, CGRectMake(x * scale, y * scale, scale, scale));
    }];
}

UILabel* Label(UIFontTextStyle style, UIColor* color)
{
    auto label = [UILabel new];
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.textColor = color;
    label.numberOfLines = 0;
    return label;
}

UIButton* Button(NSString* title, BOOL primary, id target, SEL action)
{
    auto configuration = primary ? UIButtonConfiguration.filledButtonConfiguration
                                 : UIButtonConfiguration.tintedButtonConfiguration;
    configuration.title = Localized(title);
    configuration.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(11, 16, 11, 16);
    auto button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
    [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

// The sign-in page uses the Mac's palette: the logo's violet deepening to night
// blue, frosted cards, violet main buttons and glass secondary ones.
UIColor* Argb(std::uint32_t value)
{
    return [UIColor colorWithRed:((value >> 16) & 0xff) / 255.0 green:((value >> 8) & 0xff) / 255.0
                            blue:(value & 0xff) / 255.0 alpha:(value >> 24) / 255.0];
}

constexpr std::uint32_t kLoginInk = 0xfffaf8ff, kLoginMuted = 0xffc7bdd4, kLoginViolet = 0xffaa88ff,
                        kLoginCard = 0x1fffffff, kLoginCardEdge = 0x40aa88ff, kLoginButton = 0xff6a4cff,
                        kLoginSecondary = 0x1affffff, kLoginOutline = 0x4dffffff;

UIButton* LoginButton(NSString* title, BOOL primary, id target, SEL action)
{
    auto configuration = UIButtonConfiguration.plainButtonConfiguration;
    configuration.title = Localized(title);
    configuration.baseForegroundColor = UIColor.whiteColor;
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(13, 16, 13, 16);
    configuration.background.backgroundColor = Argb(primary ? kLoginButton : kLoginSecondary);
    configuration.background.cornerRadius = 12;
    if (!primary)
    {
        configuration.background.strokeColor = Argb(kLoginOutline);
        configuration.background.strokeWidth = 1;
    }
    configuration.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
        NSDictionary<NSAttributedStringKey, id>* attributes) {
        NSMutableDictionary<NSAttributedStringKey, id>* result = [attributes mutableCopy];
        result[NSFontAttributeName] = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        return result;
    };
    auto button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
    if (action)
        [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

UIStackView* LoginCard(NSArray<UIView*>* views)
{
    auto card = [[UIStackView alloc] initWithArrangedSubviews:views];
    card.axis = UILayoutConstraintAxisVertical;
    card.spacing = 12;
    card.layoutMarginsRelativeArrangement = YES;
    card.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(20, 22, 22, 22);
    card.backgroundColor = Argb(kLoginCard);
    card.layer.cornerRadius = 18;
    card.layer.borderWidth = 1;
    card.layer.borderColor = Argb(kLoginCardEdge).CGColor;
    return card;
}

CAGradientLayer* Glow(std::uint32_t color)
{
    auto glow = [CAGradientLayer layer];
    glow.type = kCAGradientLayerRadial;
    glow.colors = @[(__bridge id)Argb(color).CGColor, (__bridge id)Argb(color & 0x00ffffff).CGColor];
    glow.startPoint = CGPointMake(.5, .5);
    glow.endPoint = CGPointMake(1, 1);
    return glow;
}

struct Catalog
{
    std::shared_ptr<LunaClient> client;
    std::vector<Game> games;
    std::vector<Shelf> shelves;
};

Catalog FetchCatalog(DeviceRegistration registration)
{
    lunanx::SetUiLanguageForMarket(registration.market_code);
    CookieJar cookies;
    cookies.SetCookies(registration.cookies);
    auto client = std::make_shared<LunaClient>(std::move(cookies), std::move(registration));
    client->SignIn();
    Catalog catalog {client, {}, client->FetchShelves()};
    std::set<std::string> seen;
    for (const auto& shelf : catalog.shelves)
        for (const auto& game : shelf.games)
            if (!game.title.empty() && InsertGameKeys(seen, game))
                catalog.games.push_back(game);
    return catalog;
}

} // namespace

@interface LunaLibraryShade : UIView
@end
@implementation LunaLibraryShade
+ (Class)layerClass { return CAGradientLayer.class; }
@end

// The sign-in page's background: a diagonal gradient and two soft glows, as on the Mac.
@interface LunaLoginBackdrop : UIView
@end
@implementation LunaLoginBackdrop
{
    CAGradientLayer* _glow;
    CAGradientLayer* _low;
}
+ (Class)layerClass { return CAGradientLayer.class; }

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame]))
        return nil;
    self.userInteractionEnabled = NO;
    auto base = (CAGradientLayer*)self.layer;
    base.colors = @[(__bridge id)Argb(0xff3a1f9a).CGColor, (__bridge id)Argb(0xff1b1150).CGColor,
                    (__bridge id)Argb(0xff0c0820).CGColor];
    base.locations = @[@0, @.55, @1];
    base.startPoint = CGPointMake(0, 0);
    base.endPoint = CGPointMake(1, 1);
    _glow = Glow(0x707c4dff);
    _low = Glow(0x40b06bff);
    [self.layer addSublayer:_glow];
    [self.layer addSublayer:_low];
    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGFloat width = self.bounds.size.width, height = self.bounds.size.height;
    const CGFloat high = .45 * MAX(width, height), low = .4 * MAX(width, height);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _glow.frame = CGRectMake(.82 * width - high, .18 * height - high, 2 * high, 2 * high);
    _low.frame = CGRectMake(.08 * width - low, .92 * height - low, 2 * low, 2 * low);
    [CATransaction commit];
}
@end

@interface LunaGameCell : UICollectionViewCell
- (void)showGame:(const Game&)game rank:(NSInteger)rank simple:(BOOL)simple;
@end

@implementation LunaGameCell
{
    UIImageView* _art;
    UILabel* _name;
    UILabel* _offer;
    UILabel* _rank;
    NSLayoutConstraint* _artMinHeight;
    NSURLSessionDataTask* _imageTask;
    NSString* _imageURL;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame]))
        return nil;
    self.contentView.backgroundColor = LibraryInk();
    self.contentView.layer.borderWidth = 1;
    self.contentView.layer.borderColor = [UIColor colorWithWhite:1 alpha:.25].CGColor;
    self.contentView.layer.cornerRadius = 16;
    self.contentView.clipsToBounds = YES;
    _art = [UIImageView new];
    _art.contentMode = UIViewContentModeScaleAspectFill;
    _art.clipsToBounds = YES;
    _art.backgroundColor = [UIColor colorWithRed:0.14 green:0.12 blue:0.22 alpha:1.0];
    _name = Label(UIFontTextStyleHeadline, UIColor.labelColor);
    _name.numberOfLines = 2;
    _offer = Label(UIFontTextStyleCaption1, UIColor.secondaryLabelColor);
    _offer.numberOfLines = 1;

    auto details = [[UIStackView alloc] initWithArrangedSubviews:@[_name, _offer]];
    details.axis = UILayoutConstraintAxisVertical;
    details.spacing = 4;
    details.layoutMarginsRelativeArrangement = YES;
    details.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12, 14, 12, 14);
    auto content = [[UIStackView alloc] initWithArrangedSubviews:@[_art, details]];
    content.axis = UILayoutConstraintAxisVertical;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:content];
    _artMinHeight = [_art.heightAnchor constraintGreaterThanOrEqualToConstant:92];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
        [content.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor],
        _artMinHeight,
    ]];
    _rank = Label(UIFontTextStyleCaption1, UIColor.whiteColor);
    _rank.textAlignment = NSTextAlignmentCenter;
    _rank.backgroundColor = [UIColor colorWithRed:.42 green:.24 blue:.82 alpha:1];
    _rank.layer.cornerRadius = 10;
    _rank.clipsToBounds = YES;
    _rank.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:_rank];
    [NSLayoutConstraint activateConstraints:@[
        [_rank.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:8],
        [_rank.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:8],
        [_rank.widthAnchor constraintGreaterThanOrEqualToConstant:34],
        [_rank.heightAnchor constraintEqualToConstant:23],
    ]];
    self.isAccessibilityElement = YES;
    self.accessibilityTraits = UIAccessibilityTraitButton;
    return self;
}

- (void)prepareForReuse
{
    [super prepareForReuse];
    [_imageTask cancel];
    _imageTask = nil;
    _imageURL = nil;
    _art.image = nil;
}

- (void)setSelected:(BOOL)selected
{
    [super setSelected:selected];
    self.contentView.layer.borderWidth = selected ? 3 : 1;
    self.contentView.layer.borderColor = (selected ? Violet() : [UIColor colorWithWhite:1 alpha:.25]).CGColor;
}

- (void)showGame:(const Game&)game rank:(NSInteger)rank simple:(BOOL)simple
{
    _artMinHeight.active = !simple;
    _art.hidden = simple;
    _rank.hidden = simple || rank == 0;
    _rank.text = [NSString stringWithFormat:@"#%ld", (long)rank];
    [_imageTask cancel];
    _name.text = simple && rank ? [NSString stringWithFormat:@"#%ld  %@", (long)rank, Text(game.title)] : Text(game.title);
    _offer.text = Text(game.offer_tag);
    _offer.hidden = _offer.text.length == 0;
    self.accessibilityLabel = rank ? [NSString stringWithFormat:@"%@ · %@", _rank.text, _name.text] : _name.text;
    _art.image = nil;
    if (simple) { _imageURL = nil; return; }
    _imageURL = Text(lunanx::ResizeAmazonImageUrl(game.image_portrait.empty() ? game.image_landscape : game.image_portrait, 440));
    NSURL* url = [NSURL URLWithString:_imageURL];
    if (![url.scheme isEqualToString:@"https"])
        return;

    static NSURLSession* artwork = [] {
        auto config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.HTTPShouldSetCookies = NO;
        config.HTTPCookieStorage = nil;
        config.URLCredentialStorage = nil;
        config.URLCache = [[NSURLCache alloc] initWithMemoryCapacity:20 * 1024 * 1024
                                                       diskCapacity:0 directoryURL:nil];
        return [NSURLSession sessionWithConfiguration:config];
    }();
    __weak LunaGameCell* weakSelf = self;
    NSString* represented = _imageURL;
    _imageTask = [artwork dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        if (!error && data.length > 0 && data.length <= 8 * 1024 * 1024)
        {
            UIImage* image = [UIImage imageWithData:data];
            dispatch_async(dispatch_get_main_queue(), ^{
                LunaGameCell* cell = weakSelf;
                if (cell && [cell->_imageURL isEqualToString:represented])
                    cell->_art.image = image;
            });
        }
    }];
    [_imageTask resume];
}
@end

@interface LunaCatalogController () <UICollectionViewDataSource, UICollectionViewDelegate, UISearchResultsUpdating>
@end

@implementation LunaCatalogController
{
    NSString* _storageError;
    NSString* _market;
    NSURL* _verifyURL;
    NSString* _deviceCode;
    dispatch_queue_t _accountQueue;
    dispatch_queue_t _catalogQueue;
    dispatch_queue_t _searchQueue;
    std::shared_ptr<std::atomic_bool> _cancel;
    std::shared_ptr<std::atomic_bool> _exploring;
    std::shared_ptr<std::atomic_bool> _searching;
    std::shared_ptr<LunaClient> _client;
    std::vector<Game> _games;
    std::vector<Shelf> _shelves;
    std::vector<Game> _sectionGames;
    std::set<std::string> _gameKeys;
    std::vector<Game> _serverResults;
    std::vector<const Game*> _results;
    NSString* _query;
    BOOL _explorationDone;
    BOOL _searchFailed;
    BOOL _busy;
    BOOL _registered;
    UIStackView* _panel;
    UIStackView* _cards;
    UIStackView* _formCard;
    UIStackView* _codeCard;
    UIStackView* _stepsCard;
    UIScrollView* _loginScroll;
    LunaLoginBackdrop* _loginBackdrop;
    NSLayoutConstraint* _panelWidth;
    UILabel* _address;
    UILabel* _heading;
    UILabel* _status;
    UILabel* _code;
    UIImageView* _qr;
    UIActivityIndicatorView* _spinner;
    UIButton* _marketButton;
    UIButton* _connect;
    UIButton* _verify;
    UIButton* _copy;
    UIButton* _cancelButton;
    UISearchController* _search;
    UICollectionView* _grid;
    UIRefreshControl* _refresh;
    UISegmentedControl* _sections;
    UIImageView* _backdrop;
    LunaLibraryShade* _shade;
    NSURLSessionDataTask* _sceneTask;
    NSString* _sceneKey;
    UILabel* _sectionLabel;
    UILabel* _titleLabel;
    UILabel* _metadata;
    UILabel* _version;
    UIButton* _play;
    UIStackView* _libraryHeader;
    UIStackView* _libraryDetails;
    NSInteger _selected;
    BOOL _simple;
}

- (instancetype)initWithStorageError:(NSString*)error
{
    if ((self = [super initWithNibName:nil bundle:nil]))
    {
        _storageError = [error copy];
        const auto prefs = lunanx::LoadPlaybackPreferences();
        _market = prefs.market.empty() ? ([NSUserDefaults.standardUserDefaults stringForKey:@"lunaMarket"] ?: @"fr") :
                                        Text(prefs.market);
        lunanx::SetUiLanguageForMarket(_market.UTF8String);
        _accountQueue = dispatch_queue_create("app.lunacore.account", DISPATCH_QUEUE_SERIAL);
        _catalogQueue = dispatch_queue_create("app.lunacore.catalog", DISPATCH_QUEUE_SERIAL);
        _searchQueue = dispatch_queue_create("app.lunacore.search", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"LunaONE";
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.view.backgroundColor = LibraryInk();
    self.view.tintColor = Violet();
    _backdrop = [[UIImageView alloc] initWithFrame:self.view.bounds];
    _backdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _backdrop.contentMode = UIViewContentModeScaleAspectFill;
    _backdrop.clipsToBounds = YES;
    [self.view addSubview:_backdrop];
    _shade = [[LunaLibraryShade alloc] initWithFrame:self.view.bounds];
    _shade.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _shade.userInteractionEnabled = NO;
    auto gradient = (CAGradientLayer*)_shade.layer;
    gradient.colors = @[
        (__bridge id)[LibraryInk() colorWithAlphaComponent:.74].CGColor,
        (__bridge id)[LibraryInk() colorWithAlphaComponent:.27].CGColor,
        (__bridge id)LibraryInk().CGColor,
    ];
    gradient.locations = @[@0, @.38, @1];
    [self.view addSubview:_shade];

    _simple = lunanx::LoadPlaybackPreferences().simple_library;
    __weak LunaCatalogController* weakSelf = self;
    auto layout = [[UICollectionViewCompositionalLayout alloc]
        initWithSectionProvider:^NSCollectionLayoutSection*(NSInteger section, id<NSCollectionLayoutEnvironment> environment) {
            const CGFloat width = environment.container.effectiveContentSize.width;
            auto controller = weakSelf;
            const BOOL simple = controller && controller->_simple;
            if (simple)
            {
                auto size = [NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1]
                    heightDimension:[NSCollectionLayoutDimension absoluteDimension:75]];
                auto item = [NSCollectionLayoutItem itemWithLayoutSize:size];
                auto group = [NSCollectionLayoutGroup verticalGroupWithLayoutSize:size subitems:@[item]];
                auto result = [NSCollectionLayoutSection sectionWithGroup:group];
                result.contentInsets = NSDirectionalEdgeInsetsMake(0, 16, 6, 16);
                result.interGroupSpacing = 7;
                return result;
            }
            const CGFloat cardWidth = width >= 650 ? 182 : 140;
            const CGFloat height = environment.container.effectiveContentSize.height;
            const CGFloat cardHeight = MIN(270, MAX(120, height - 18));
            auto itemSize = [NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0]
                                                           heightDimension:[NSCollectionLayoutDimension fractionalHeightDimension:1.0]];
            auto item = [NSCollectionLayoutItem itemWithLayoutSize:itemSize];
            item.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
            auto rowSize = [NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension absoluteDimension:cardWidth]
                                                          heightDimension:[NSCollectionLayoutDimension absoluteDimension:cardHeight]];
            auto row = [NSCollectionLayoutGroup horizontalGroupWithLayoutSize:rowSize subitems:@[item]];
            auto result = [NSCollectionLayoutSection sectionWithGroup:row];
            result.contentInsets = NSDirectionalEdgeInsetsMake(0, 12, 8, 12);
            result.orthogonalScrollingBehavior = UICollectionLayoutSectionOrthogonalScrollingBehaviorContinuousGroupLeadingBoundary;
            return result;
        }];
    _grid = [[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout];
    _grid.dataSource = self;
    _grid.delegate = self;
    _grid.alwaysBounceVertical = YES;
    _grid.backgroundColor = UIColor.clearColor;
    _grid.accessibilityIdentifier = @"game-catalog";
    [_grid registerClass:LunaGameCell.class forCellWithReuseIdentifier:@"game"];
    _refresh = [UIRefreshControl new];
    [_refresh addTarget:self action:@selector(refresh) forControlEvents:UIControlEventValueChanged];
    _grid.refreshControl = _refresh;
    _grid.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_grid];

    _search = [[UISearchController alloc] initWithSearchResultsController:nil];
    _search.searchResultsUpdater = self;
    _search.obscuresBackgroundDuringPresentation = NO;
    _search.searchBar.placeholder = @"Rechercher dans le catalogue";
    self.definesPresentationContext = YES;

    _heading = Label(UIFontTextStyleTitle3, Argb(kLoginViolet));
    _status = Label(UIFontTextStyleBody, Argb(kLoginMuted));
    _code = Label(UIFontTextStyleTitle1, Argb(kLoginInk));
    _code.font = [UIFontMetrics.defaultMetrics scaledFontForFont:
        [UIFont monospacedSystemFontOfSize:40 weight:UIFontWeightSemibold]];
    _code.textAlignment = NSTextAlignmentCenter;
    _code.accessibilityIdentifier = @"amazon-device-code";
    _address = Label(UIFontTextStyleHeadline, Argb(kLoginViolet));
    _address.textAlignment = NSTextAlignmentCenter;
    _qr = [UIImageView new];
    _qr.contentMode = UIViewContentModeScaleAspectFit;
    _qr.accessibilityLabel = @"QR";
    _qr.layer.magnificationFilter = kCAFilterNearest;
    _sections = [[UISegmentedControl alloc] initWithItems:@[@"Top 10", @"Reprendre", @"Tous les jeux", @"Luna Premium"]];
    _sections.selectedSegmentIndex = 2;
    _sections.selectedSegmentTintColor = [UIColor colorWithRed:.37 green:.23 blue:.62 alpha:1];
    [_sections addTarget:self action:@selector(changeSection) forControlEvents:UIControlEventValueChanged];
    _sectionLabel = Label(UIFontTextStyleSubheadline, Violet());
    _titleLabel = Label(UIFontTextStyleTitle1, UIColor.whiteColor);
    _titleLabel.numberOfLines = 1;
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _metadata = Label(UIFontTextStyleCaption1, UIColor.secondaryLabelColor);
    _metadata.numberOfLines = 1;
    _version = Label(UIFontTextStyleCaption2, UIColor.secondaryLabelColor);
    _version.textAlignment = NSTextAlignmentRight;
    _play = Button(@"Jouer", YES, self, @selector(playSelected));
    auto playConfig = _play.configuration;
    playConfig.image = [UIImage systemImageNamed:@"play.fill"];
    playConfig.imagePadding = 10;
    playConfig.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    playConfig.baseBackgroundColor = [UIColor colorWithRed:.91 green:.88 blue:1 alpha:1];
    playConfig.baseForegroundColor = LibraryInk();
    _play.configuration = playConfig;
    _libraryHeader = [[UIStackView alloc] initWithArrangedSubviews:@[_sections, _sectionLabel]];
    _libraryHeader.axis = UILayoutConstraintAxisVertical;
    _libraryHeader.spacing = 5;
    _libraryHeader.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_libraryHeader];
    _libraryDetails = [[UIStackView alloc] initWithArrangedSubviews:@[_titleLabel, _metadata, _play, _version]];
    _libraryDetails.axis = UILayoutConstraintAxisVertical;
    _libraryDetails.alignment = UIStackViewAlignmentLeading;
    _libraryDetails.spacing = 5;
    _libraryDetails.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_libraryDetails];
    [_titleLabel.widthAnchor constraintLessThanOrEqualToAnchor:_libraryDetails.widthAnchor].active = YES;
    [_metadata.widthAnchor constraintLessThanOrEqualToAnchor:_libraryDetails.widthAnchor].active = YES;
    [_version.widthAnchor constraintEqualToAnchor:_libraryDetails.widthAnchor].active = YES;
    [_play.widthAnchor constraintGreaterThanOrEqualToConstant:145].active = YES;
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    _spinner.color = Argb(kLoginInk);
    _marketButton = LoginButton(@"Boutique Amazon", NO, self, nil);
    _marketButton.showsMenuAsPrimaryAction = YES;
    _connect = LoginButton(@"Créer un code", YES, self, @selector(connectOrRefresh));
    _connect.accessibilityIdentifier = @"amazon-login";
    _verify = LoginButton(@"Ouvrir la page Amazon", YES, self, @selector(openAmazon));
    _copy = LoginButton(@"Copier le code", NO, self, @selector(copyCode));
    _cancelButton = LoginButton(@"Annuler", NO, self, @selector(cancelSignIn));

    // Sign-in page: logo and name, what LunaONE is, then the form card and,
    // beside it on a wide screen (under it on a narrow one), the steps or the code.
    auto logo = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"LunaONE"]];
    logo.contentMode = UIViewContentModeScaleAspectFill;
    logo.layer.cornerRadius = 16;
    logo.clipsToBounds = YES;
    [logo.widthAnchor constraintEqualToConstant:64].active = YES;
    [logo.heightAnchor constraintEqualToConstant:64].active = YES;
    auto name = Label(UIFontTextStyleLargeTitle, Argb(kLoginInk));
    name.font = [UIFontMetrics.defaultMetrics scaledFontForFont:[UIFont systemFontOfSize:34 weight:UIFontWeightBold]];
    name.text = @"LunaONE";
    auto names = [[UIStackView alloc] initWithArrangedSubviews:@[name, _heading]];
    names.axis = UILayoutConstraintAxisVertical;
    names.spacing = 2;
    auto header = [[UIStackView alloc] initWithArrangedSubviews:@[logo, names]];
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 16;
    auto tagline = Label(UIFontTextStyleBody, Argb(kLoginMuted));
    tagline.text = Localized(@"Tes jeux Amazon Luna, sans navigateur, sur tous tes écrans.");

    _formCard = LoginCard(@[_status, _spinner, _marketButton, _connect, _cancelButton]);
    _status.textAlignment = NSTextAlignmentNatural;

    // The QR on a white plate, which phone cameras read best.
    auto plate = [UIView new];
    plate.backgroundColor = UIColor.whiteColor;
    plate.layer.cornerRadius = 14;
    _qr.translatesAutoresizingMaskIntoConstraints = NO;
    [plate addSubview:_qr];
    [NSLayoutConstraint activateConstraints:@[
        [_qr.widthAnchor constraintEqualToConstant:156],
        [_qr.heightAnchor constraintEqualToConstant:156],
        [_qr.topAnchor constraintEqualToAnchor:plate.topAnchor constant:12],
        [_qr.bottomAnchor constraintEqualToAnchor:plate.bottomAnchor constant:-12],
        [_qr.leadingAnchor constraintEqualToAnchor:plate.leadingAnchor constant:12],
        [_qr.trailingAnchor constraintEqualToAnchor:plate.trailingAnchor constant:-12],
    ]];
    auto plateRow = [[UIStackView alloc] initWithArrangedSubviews:@[plate]];
    plateRow.axis = UILayoutConstraintAxisVertical;
    plateRow.alignment = UIStackViewAlignmentCenter;
    auto codeHeading = Label(UIFontTextStyleHeadline, Argb(kLoginInk));
    codeHeading.text = Localized(@"Scanne le QR code et saisis le code Amazon.");
    codeHeading.textAlignment = NSTextAlignmentCenter;
    _codeCard = LoginCard(@[codeHeading, plateRow, _address, _code, _verify, _copy]);

    auto stepsHeading = Label(UIFontTextStyleTitle3, Argb(kLoginInk));
    stepsHeading.font = [UIFontMetrics.defaultMetrics scaledFontForFont:[UIFont systemFontOfSize:20 weight:UIFontWeightBold]];
    stepsHeading.text = Localized(@"Comment se connecter");
    NSMutableArray<UIView*>* steps = [NSMutableArray arrayWithObject:stepsHeading];
    NSArray<NSString*>* lines = @[@"Choisis le pays de ton compte Amazon, puis crée un code.",
                                  @"Scanne le QR code avec ton téléphone ou ouvre la page Amazon.",
                                  @"Saisis le code et confirme : ta bibliothèque Luna s’ouvre ici."];
    for (NSUInteger i = 0; i < lines.count; ++i)
    {
        auto badge = Label(UIFontTextStyleHeadline, Argb(kLoginInk));
        badge.text = [NSString stringWithFormat:@"%lu", (unsigned long)(i + 1)];
        badge.textAlignment = NSTextAlignmentCenter;
        badge.backgroundColor = Argb(kLoginButton);
        badge.layer.cornerRadius = 17;
        badge.clipsToBounds = YES;
        [badge.widthAnchor constraintEqualToConstant:34].active = YES;
        [badge.heightAnchor constraintEqualToConstant:34].active = YES;
        auto line = Label(UIFontTextStyleBody, Argb(kLoginMuted));
        line.text = Localized(lines[i]);
        auto row = [[UIStackView alloc] initWithArrangedSubviews:@[badge, line]];
        row.alignment = UIStackViewAlignmentCenter;
        row.spacing = 14;
        [steps addObject:row];
    }
    auto note = Label(UIFontTextStyleSubheadline, Argb(kLoginViolet));
    note.text = Localized(@"Aucun mot de passe n’est saisi dans LunaONE.");
    [steps addObject:note];
    _stepsCard = LoginCard(steps);
    _stepsCard.spacing = 16;

    _cards = [[UIStackView alloc] initWithArrangedSubviews:@[_formCard, _codeCard, _stepsCard]];
    _cards.spacing = 18;
    _panel = [[UIStackView alloc] initWithArrangedSubviews:@[header, tagline, _cards]];
    _panel.axis = UILayoutConstraintAxisVertical;
    _panel.spacing = 18;
    [_panel setCustomSpacing:10 afterView:header];
    _panel.translatesAutoresizingMaskIntoConstraints = NO;

    // The page scrolls when it is taller than the screen (a phone on its side,
    // a large text size), and is centred otherwise.
    _loginBackdrop = [[LunaLoginBackdrop alloc] initWithFrame:self.view.bounds];
    _loginBackdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_loginBackdrop];
    _loginScroll = [UIScrollView new];
    _loginScroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    _loginScroll.alwaysBounceVertical = NO;
    _loginScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [_loginScroll addSubview:_panel];
    [self.view addSubview:_loginScroll];

    auto safe = self.view.safeAreaLayoutGuide;
    auto content = _loginScroll.contentLayoutGuide;
    auto frame = _loginScroll.frameLayoutGuide;
    _panelWidth = [_panel.widthAnchor constraintEqualToConstant:1040];
    _panelWidth.priority = UILayoutPriorityDefaultHigh;
    auto snug = [content.heightAnchor constraintEqualToAnchor:frame.heightAnchor];
    snug.priority = UILayoutPriorityDefaultLow;
    [NSLayoutConstraint activateConstraints:@[
        [_grid.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_grid.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_libraryHeader.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [_libraryHeader.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_libraryHeader.topAnchor constraintEqualToAnchor:safe.topAnchor constant:5],
        [_grid.topAnchor constraintEqualToAnchor:_libraryHeader.bottomAnchor constant:4],
        [_grid.bottomAnchor constraintEqualToAnchor:_libraryDetails.topAnchor constant:-4],
        [_libraryDetails.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [_libraryDetails.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_libraryDetails.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-5],
        [_loginScroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_loginScroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_loginScroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [_loginScroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [content.widthAnchor constraintEqualToAnchor:frame.widthAnchor],
        [content.heightAnchor constraintGreaterThanOrEqualToAnchor:frame.heightAnchor],
        snug,
        [_panel.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [_panel.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
        [_panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:content.leadingAnchor constant:20],
        [_panel.topAnchor constraintGreaterThanOrEqualToAnchor:content.topAnchor constant:16],
        _panelWidth,
    ]];

    [self updateMarketMenu];
    if (_storageError)
        [self showState:@"Stockage indisponible" detail:_storageError];
    else
        [self refresh];
}

- (void)updateMarketMenu
{
    const auto& selected = FindAmazonMarket(_market.UTF8String ?: "fr");
    auto configuration = _marketButton.configuration;
    configuration.title = [NSString stringWithFormat:@"%@ · %@", Localized(@"Boutique Amazon"), Text(selected.native_name)];
    _marketButton.configuration = configuration;
    NSMutableArray<UIAction*>* actions = [NSMutableArray array];
    __weak LunaCatalogController* weakSelf = self;
    for (const auto& market : AmazonMarkets())
    {
        NSString* code = Text(market.code);
        auto item = [UIAction actionWithTitle:Text(market.native_name) image:nil identifier:nil handler:^(UIAction*) {
            [weakSelf selectMarket:code];
        }];
        item.state = [code isEqualToString:_market] ? UIMenuElementStateOn : UIMenuElementStateOff;
        [actions addObject:item];
    }
    _marketButton.menu = [UIMenu menuWithChildren:actions];
}

- (void)changeSection
{
    [self filterCatalog];
}

- (void)videoSettings
{
    const auto saved = lunanx::LoadPlaybackPreferences();
    const auto choices = lunanx::PlaybackChoices(saved);
    auto options = [UIAlertController alertControllerWithTitle:Localized(@"Réglages")
        message:[NSString stringWithFormat:@"%@  %@ · v%@", Localized(@"Crédit"), Text(lunanx::ProjectCredit),
            Text(std::string(lunanx::InstalledVersion()))] preferredStyle:UIAlertControllerStyleAlert];
    for (std::size_t row = 0; row < choices.size(); ++row)
    {
        const auto& choice = choices[row];
        auto label = Text(choice.title + " · " + choice.labels[choice.selected]);
        [options addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*) { [self chooseSetting:row]; }]];
    }
    [options addAction:[UIAlertAction actionWithTitle:Localized(@"Vérifier les mises à jour") style:UIAlertActionStyleDefault
        handler:^(UIAlertAction*) { [self checkUpdates]; }]];
    if (_registered && !_busy)
        [options addAction:[UIAlertAction actionWithTitle:Localized(@"Se déconnecter") style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*) { [self confirmSignOut]; }]];
    [options addAction:[UIAlertAction actionWithTitle:Localized(@"Annuler") style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:options animated:YES completion:nil];
}

- (void)chooseSetting:(std::size_t)row
{
    const auto saved = lunanx::LoadPlaybackPreferences();
    auto choice = lunanx::PlaybackChoices(saved).at(row);
    auto options = [UIAlertController alertControllerWithTitle:Text(choice.title) message:nil
                                                preferredStyle:UIAlertControllerStyleAlert];
    for (std::size_t i = 0; i < choice.labels.size(); ++i)
    {
        auto label = Text((i == choice.selected ? "✓ " : "") + choice.labels[i]);
        [options addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
            auto settings = lunanx::LoadPlaybackPreferences();
            choice.apply(settings, i);
            if (lunanx::SavePlaybackPreferences(settings))
            {
                DeviceRegistration registration;
                lunanx::SetUiLanguageForMarket(AmazonAuth::Load(registration) ? registration.market_code : settings.market);
                if (!settings.market.empty()) _market = Text(settings.market);
                [self updateMarketMenu];
                _simple = settings.simple_library;
                [_grid.collectionViewLayout invalidateLayout];
                if (_client)
                {
                    [self showState:nil detail:nil];
                    [self filterCatalog];
                }
                [self videoSettings];
                return;
            }
            auto error = [UIAlertController alertControllerWithTitle:Localized(@"Réglages")
                message:Localized(@"Impossible d’enregistrer les réglages vidéo.") preferredStyle:UIAlertControllerStyleAlert];
            [error addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:error animated:YES completion:nil];
        }]];
    }
    [options addAction:[UIAlertAction actionWithTitle:Localized(@"Annuler") style:UIAlertActionStyleCancel
        handler:^(UIAlertAction*) { [self videoSettings]; }]];
    [self presentViewController:options animated:YES completion:nil];
}

- (void)checkUpdates
{
    auto progress = [UIAlertController alertControllerWithTitle:Localized(@"Vérifier les mises à jour")
        message:Localized(@"Vérification des mises à jour…") preferredStyle:UIAlertControllerStyleAlert];
    [progress addAction:[UIAlertAction actionWithTitle:Localized(@"Annuler") style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:progress animated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        // An iPhone or iPad app must be signed again to install: the new
        // version comes from GitHub through AltStore or Sideloadly.
        const auto update = lunanx::FindUpdate();
        auto result = Text(update.available ? update.message + "\n" + lunanx::Tr("Télécharge la nouvelle version sur GitHub.")
                                            : update.message);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.presentedViewController != progress || progress.isBeingDismissed) return;
            [progress dismissViewControllerAnimated:YES completion:^{
                auto alert = [UIAlertController alertControllerWithTitle:Localized(@"Vérifier les mises à jour")
                    message:result preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
                [alert addAction:[UIAlertAction actionWithTitle:Localized(@"Versions GitHub") style:UIAlertActionStyleDefault
                    handler:^(UIAlertAction*) {
                        [UIApplication.sharedApplication openURL:[NSURL URLWithString:Text(lunanx::ReleasesUrl)]
                                                         options:@{} completionHandler:nil];
                    }]];
                [self presentViewController:alert animated:YES completion:nil];
            }];
        });
    });
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [self exploreCatalog];
}

- (void)selectMarket:(NSString*)code
{
    _market = [code copy];
    [NSUserDefaults.standardUserDefaults setObject:_market forKey:@"lunaMarket"];
    [self updateMarketMenu];
}

- (void)showState:(NSString*)heading detail:(NSString*)detail
{
    const BOOL hasCatalog = _client != nullptr;
    const BOOL hasCode = _deviceCode.length > 0;
    self.navigationItem.searchController = hasCatalog ? _search : nil;
    auto settings = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"gearshape"]
        style:UIBarButtonItemStylePlain target:self action:@selector(videoSettings)];
    settings.accessibilityLabel = Localized(@"Réglages");
    self.navigationItem.leftBarButtonItem = settings;
    [_sections setTitle:Localized(@"Reprendre") forSegmentAtIndex:1];
    [_sections setTitle:Localized(@"Tous les jeux") forSegmentAtIndex:2];
    _search.searchBar.placeholder = Localized(@"Rechercher dans le catalogue");
    if (_registered && !_busy)
    {
        auto signOut = [UIAction actionWithTitle:Localized(@"Se déconnecter") image:[UIImage systemImageNamed:@"rectangle.portrait.and.arrow.right"]
            identifier:nil handler:^(UIAction*) { [self confirmSignOut]; }];
        auto account = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"person.crop.circle"]
            menu:[UIMenu menuWithChildren:@[signOut]]];
        account.accessibilityLabel = Localized(@"Compte");
        self.navigationItem.rightBarButtonItem = account;
    }
    else
        self.navigationItem.rightBarButtonItem = nil;
    _libraryHeader.hidden = _libraryDetails.hidden = _backdrop.hidden = _shade.hidden = !hasCatalog;
    _grid.hidden = !hasCatalog;
    _grid.userInteractionEnabled = !_busy;
    _loginScroll.hidden = _loginBackdrop.hidden = hasCatalog;
    // The page has its own header: no large "LunaONE" title above it.
    self.navigationItem.title = hasCatalog ? @"LunaONE" : @"";
    self.navigationItem.largeTitleDisplayMode = hasCatalog ? UINavigationItemLargeTitleDisplayModeAutomatic
                                                           : UINavigationItemLargeTitleDisplayModeNever;
    [self setContentScrollView:hasCatalog ? _grid : _loginScroll forEdge:NSDirectionalRectEdgeTop];
    _heading.text = Localized(heading);
    _status.text = Localized(detail);
    _status.hidden = detail.length == 0;
    _marketButton.hidden = _registered || _busy || _storageError;
    _connect.hidden = _busy || _storageError;
    auto configuration = _connect.configuration;
    configuration.title = Localized(_registered ? @"Réessayer" : @"Créer un code");
    _connect.configuration = configuration;
    // Steps before a code, the code once Amazon gives one; nothing beside the
    // form while a saved account loads.
    const BOOL codeAppears = hasCode && _codeCard.hidden;
    _codeCard.hidden = !hasCode;
    _stepsCard.hidden = hasCode || _registered || _storageError || (_busy && !_cancel);
    _panelWidth.constant = _codeCard.hidden && _stepsCard.hidden ? 520 : 1040;
    _qr.image = hasCode ? QRImage(_verifyURL) : nil;
    _code.text = _deviceCode;
    _address.text = hasCode ? [NSString stringWithFormat:@"%@%@", _verifyURL.host ?: @"", _verifyURL.path ?: @""] : nil;
    _cancelButton.hidden = !_cancel;
    _cancelButton.enabled = _cancel && !_cancel->load();
    if (_busy && !hasCode)
        [_spinner startAnimating];
    else
        [_spinner stopAnimating];
    if (!_busy)
        [_refresh endRefreshing];
    if (codeAppears && !hasCatalog)
    {
        // On a narrow screen the code sits under the form: bring it into view.
        [self.view layoutIfNeeded];
        [_loginScroll scrollRectToVisible:[_codeCard convertRect:_codeCard.bounds toView:_loginScroll] animated:YES];
    }
}

// Side by side on an iPad or a phone on its side, stacked on a phone held upright.
- (void)viewWillLayoutSubviews
{
    [super viewWillLayoutSubviews];
    const auto insets = self.view.safeAreaInsets;
    const BOOL wide = self.view.bounds.size.width - insets.left - insets.right >= 700;
    _cards.axis = wide ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
    _cards.alignment = wide ? UIStackViewAlignmentTop : UIStackViewAlignmentFill;
    _cards.distribution = wide ? UIStackViewDistributionFillEqually : UIStackViewDistributionFill;
}

- (void)deliverCatalog:(std::shared_ptr<Catalog>)catalog registered:(BOOL)registered error:(NSString*)error
{
    _busy = NO;
    _registered = registered;
    _cancel.reset();
    _deviceCode = nil;
    _verifyURL = nil;
    if (catalog)
    {
        _client = std::move(catalog->client);
        _games = std::move(catalog->games);
        _shelves = std::move(catalog->shelves);
        _gameKeys.clear();
        for (const auto& game : _games)
            InsertGameKeys(_gameKeys, game);
        _explorationDone = NO;
        [self filterCatalog];
        [self showState:nil detail:nil];
        [self exploreCatalog];
        NSLog(@"LunaONE iPadOS catalog ready: %zu games", _games.size());
    }
    else if (_client && error)
    {
        [self showState:nil detail:nil];
        [self exploreCatalog];
        auto alert = [UIAlertController alertControllerWithTitle:@"Catalogue indisponible" message:error
                                                     preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    }
    else
        [self showState:registered ? @"Catalogue indisponible" : @"Connexion Amazon"
                 detail:error ?: @"Crée un code et valide-le sur la page officielle Amazon."];
}

- (void)refresh
{
    if (_busy || _storageError)
        return;
    [self cancelCatalogWork];
    _busy = YES;
    [self showState:@"Catalogue Luna" detail:@"Chargement…"];
    dispatch_async(_accountQueue, ^{
        @autoreleasepool
        {
            BOOL registered = NO;
            std::shared_ptr<Catalog> catalog;
            NSString* error = nil;
            try
            {
                DeviceRegistration registration;
                registered = AmazonAuth::Load(registration);
                if (registered)
                    catalog = std::make_shared<Catalog>(FetchCatalog(std::move(registration)));
            }
            catch (const std::exception& ex) { error = [NSString stringWithUTF8String:ex.what()]; }
            dispatch_async(dispatch_get_main_queue(), ^{
                [self deliverCatalog:catalog registered:registered error:error];
            });
        }
    });
}

- (void)connectOrRefresh
{
    if (_registered)
    {
        [self refresh];
        return;
    }
    if (_busy || _storageError)
        return;
    _busy = YES;
    const auto cancelled = _cancel = std::make_shared<std::atomic_bool>(false);
    const std::string market(_market.UTF8String ?: "fr");
    [self showState:@"Connexion Amazon" detail:@"Création du code…"];

    dispatch_async(_accountQueue, ^{
        @autoreleasepool
        {
            BOOL registered = NO;
            std::shared_ptr<Catalog> catalog;
            NSString* error = nil;
            try
            {
                auto registration = AmazonAuth::SignInWithCode(market, [self, cancelled](const DeviceCodeChallenge& challenge) {
                    @autoreleasepool
                    {
                        NSString* code = Text(challenge.user_code);
                        NSURL* url = [NSURL URLWithString:Text(challenge.verification_url)];
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (!cancelled->load())
                            {
                                self->_deviceCode = code;
                                self->_verifyURL = url;
                                [self showState:@"Connexion Amazon"
                                    detail:@"Scanne le QR code et saisis le code Amazon."];
                            }
                        });
                    }
                }, [cancelled] { return cancelled->load(); });
                if (cancelled->load() || !AmazonAuth::Save(registration))
                {
                    try { AmazonAuth::Deregister(registration); } catch (...) {}
                    throw std::runtime_error(cancelled->load() ? "Connexion annulée." : "Impossible de sauvegarder la connexion.");
                }
                registered = YES;
                NSLog(@"LunaONE iPadOS Amazon registration accepted");
                dispatch_async(dispatch_get_main_queue(), ^{
                    self->_deviceCode = nil;
                    self->_verifyURL = nil;
                    self->_cancel.reset();
                    [self showState:@"Catalogue Luna" detail:@"Chargement…"];
                });
                catalog = std::make_shared<Catalog>(FetchCatalog(std::move(registration)));
            }
            catch (const std::exception& ex) { error = [NSString stringWithUTF8String:ex.what()]; }
            dispatch_async(dispatch_get_main_queue(), ^{
                [self deliverCatalog:catalog registered:registered error:error];
            });
        }
    });
}

- (void)cancelSignIn
{
    if (_cancel)
        _cancel->store(true);
    _deviceCode = nil;
    _verifyURL = nil;
    [self showState:@"Connexion Amazon" detail:@"Annulation…"];
}

- (void)openAmazon
{
    if (_verifyURL)
        [UIApplication.sharedApplication openURL:_verifyURL options:@{} completionHandler:nil];
}

- (void)copyCode
{
    if (_deviceCode)
        [UIPasteboard.generalPasteboard setItems:@[@{@"public.utf8-plain-text": _deviceCode}]
                                         options:@{UIPasteboardOptionLocalOnly: @YES,
                                                   UIPasteboardOptionExpirationDate: [NSDate dateWithTimeIntervalSinceNow:600]}];
}

- (void)confirmSignOut
{
    auto alert = [UIAlertController alertControllerWithTitle:@"Se déconnecter de LunaONE ?"
                                                    message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    __weak LunaCatalogController* weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Se déconnecter" style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction*) { [weakSelf signOut]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)signOut
{
    if (_busy)
        return;
    [self cancelCatalogWork];
    _busy = YES;
    _client.reset();
    _games.clear();
    _shelves.clear();
    _sectionGames.clear();
    [self filterCatalog];
    [self showState:@"Compte Amazon" detail:@"Déconnexion…"];
    dispatch_async(_accountQueue, ^{
        @autoreleasepool
        {
            DeviceRegistration registration;
            try
            {
                if (AmazonAuth::Load(registration))
                    AmazonAuth::Deregister(registration);
            }
            catch (...) {}
            const bool erased = AmazonAuth::EraseAccountData();
            dispatch_async(dispatch_get_main_queue(), ^{
                [self deliverCatalog:nullptr registered:!erased
                    error:erased ? nil : @"Impossible d’effacer toutes les données locales."];
            });
        }
    });
}

- (void)cancelCatalogWork
{
    if (_exploring)
        _exploring->store(true);
    _exploring.reset();
    if (_searching)
        _searching->store(true);
    _searching.reset();
    _query = nil;
    _serverResults.clear();
    _searchFailed = NO;
    [self filterCatalog];
}

- (void)exploreCatalog
{
    if (!_client || _busy || _exploring || _explorationDone || !self.view.window)
        return;
    const auto client = _client;
    const auto cancelled = _exploring = std::make_shared<std::atomic_bool>(false);
    const auto known = _gameKeys;
    __weak LunaCatalogController* weakSelf = self;
    dispatch_async(_catalogQueue, ^{
        @autoreleasepool
        {
            try
            {
                if (!cancelled->load())
                    client->ExploreCatalog(known, [weakSelf, cancelled](std::vector<Game> games) {
                        auto discovered = std::make_shared<std::vector<Game>>(std::move(games));
                        dispatch_async(dispatch_get_main_queue(), ^{
                            auto controller = weakSelf;
                            if (!controller || cancelled->load())
                                return;
                            for (auto& game : *discovered)
                                if (InsertGameKeys(controller->_gameKeys, game))
                                    controller->_games.push_back(std::move(game));
                            [controller filterCatalog];
                        });
                    }, [cancelled] { return !cancelled->load(); });
            }
            catch (const std::exception&)
            {
                lunanx::LogRuntimeEvent("ipados", "catalog_exploration_failed");
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                auto controller = weakSelf;
                if (controller && !cancelled->load())
                {
                    controller->_exploring.reset();
                    controller->_explorationDone = YES;
                    lunanx::LogRuntimeEvent("ipados", "catalog_expanded",
                        "games=" + std::to_string(controller->_games.size()));
                }
            });
        }
    });
}

- (void)filterCatalog
{
    _results.clear();
    if (_query.length == 0)
    {
        _sectionGames = lunanx::ui::LibraryGames(static_cast<LibrarySection>(_sections.selectedSegmentIndex), _shelves, _games,
                                                _client ? _client->Plan() : AccountPlan::Unknown);
        for (const auto& game : _sectionGames)
            _results.push_back(&game);
    }
    else
    {
        std::set<std::string> seen;
        for (const auto& game : _serverResults)
            if (InsertGameKeys(seen, game))
                _results.push_back(&game);
        for (const auto& game : _games)
            if ([Text(game.title) localizedStandardContainsString:_query] && InsertGameKeys(seen, game))
                _results.push_back(&game);
    }
    [_grid reloadData];
    _selected = _results.empty() ? NSNotFound : 0;
    if (_selected != NSNotFound)
        [_grid selectItemAtIndexPath:[NSIndexPath indexPathForItem:_selected inSection:0]
                           animated:NO scrollPosition:UICollectionViewScrollPositionNone];
    [self updateSelection];
    if (_results.empty() && _client)
    {
        auto empty = Label(UIFontTextStyleBody, UIColor.secondaryLabelColor);
        if (_query.length == 0)
            empty.text = @"Le catalogue est vide.";
        else if (_searching)
            empty.text = @"Recherche sur Amazon Luna…";
        else
            empty.text = _searchFailed ? @"Recherche Luna indisponible." : @"Aucun jeu correspondant.";
        empty.textAlignment = NSTextAlignmentCenter;
        _grid.backgroundView = empty;
    }
    else
        _grid.backgroundView = nil;
}

- (void)updateSearchResultsForSearchController:(UISearchController*)searchController
{
    NSString* query = searchController.isActive ? [searchController.searchBar.text
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : @"";
    if ([_query isEqualToString:query])
        return;
    if (_searching)
        _searching->store(true);
    _searching.reset();
    _query = [query copy];
    _searchFailed = NO;
    _serverResults.clear();
    if (_client && !_busy && query.length)
    {
        const auto client = _client;
        const std::string text(query.UTF8String ?: "");
        const auto cancelled = _searching = std::make_shared<std::atomic_bool>(false);
        __weak LunaCatalogController* weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 350 * NSEC_PER_MSEC), _searchQueue, ^{
            @autoreleasepool
            {
                if (cancelled->load())
                    return;
                auto results = std::make_shared<std::vector<Game>>();
                BOOL failed = NO;
                try
                {
                    *results = client->SearchGames(text, [cancelled] { return !cancelled->load(); });
                }
                catch (const std::exception&) { failed = YES; }
                dispatch_async(dispatch_get_main_queue(), ^{
                    auto controller = weakSelf;
                    if (!controller || cancelled->load())
                        return;
                    controller->_searching.reset();
                    controller->_serverResults = std::move(*results);
                    controller->_searchFailed = failed;
                    [controller filterCatalog];
                });
            }
        });
    }
    [self filterCatalog];
}

- (NSInteger)collectionView:(UICollectionView*)collectionView numberOfItemsInSection:(NSInteger)section
{
    return static_cast<NSInteger>(_results.size());
}

- (UICollectionViewCell*)collectionView:(UICollectionView*)collectionView cellForItemAtIndexPath:(NSIndexPath*)path
{
    auto cell = (LunaGameCell*)[collectionView dequeueReusableCellWithReuseIdentifier:@"game" forIndexPath:path];
    [cell showGame:*_results[path.item] rank:(_query.length == 0 && _sections.selectedSegmentIndex == 0 ? path.item + 1 : 0) simple:_simple];
    return cell;
}

- (void)collectionView:(UICollectionView*)collectionView didSelectItemAtIndexPath:(NSIndexPath*)path
{
    _selected = path.item;
    [self updateSelection];
}

- (void)updateSelection
{
    auto name = _query.length ? Localized(@"Rechercher") : [_sections titleForSegmentAtIndex:_sections.selectedSegmentIndex];
    _sectionLabel.text = [NSString stringWithFormat:@"%@   ·   %@", name,
        Text(lunanx::TrFormat("{} titres", {std::to_string(_results.size())}))];
    _version.text = [NSString stringWithFormat:@"%@    v%@", _client ? Text(_client->PlanLabel()) : @"",
        Text(std::string(lunanx::InstalledVersion()))];
    const BOOL selected = _selected != NSNotFound && static_cast<std::size_t>(_selected) < _results.size() && _client;
    _titleLabel.hidden = _metadata.hidden = _play.hidden = !selected;
    if (!selected)
    {
        [self loadBackdrop:""];
        return;
    }
    const auto& game = *_results[_selected];
    const auto presentation = lunanx::ui::PresentLibraryGame(game, _client->Plan());
    _titleLabel.text = Text(game.title);
    _play.enabled = !presentation.locked;
    auto line = [[NSMutableAttributedString alloc] initWithString:Text(presentation.offer)
        attributes:@{NSForegroundColorAttributeName: presentation.locked ? UIColor.systemOrangeColor :
            [UIColor colorWithRed:.47 green:.89 blue:.74 alpha:1]}];
    if (!presentation.metadata.empty())
        [line appendAttributedString:[[NSAttributedString alloc] initWithString:Text("   " + presentation.metadata)
            attributes:@{NSForegroundColorAttributeName:UIColor.secondaryLabelColor}]];
    _metadata.attributedText = line;
    [self loadBackdrop:presentation.backdrop];
}

- (void)loadBackdrop:(const std::string&)address
{
    auto key = Text(lunanx::ResizeAmazonImageUrl(address, 1280));
    if ([_sceneKey isEqualToString:key]) return;
    _sceneKey = key;
    [_sceneTask cancel];
    _backdrop.image = nil;
    auto url = [NSURL URLWithString:key];
    if (![url.scheme isEqualToString:@"https"] ||
        !([url.host hasSuffix:@".media-amazon.com"] || [url.host hasSuffix:@".images-amazon.com"]))
        return;
    static NSURLSession* images = [] {
        auto config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.HTTPShouldSetCookies = NO;
        config.timeoutIntervalForRequest = 8;
        return [NSURLSession sessionWithConfiguration:config];
    }();
    __weak LunaCatalogController* weakSelf = self;
    _sceneTask = [images dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        if (error || data.length == 0 || data.length > 8 * 1024 * 1024 ||
            [(NSHTTPURLResponse*)response statusCode] != 200) return;
        auto image = [UIImage imageWithData:data];
        dispatch_async(dispatch_get_main_queue(), ^{
            auto controller = weakSelf;
            if (controller && [controller->_sceneKey isEqualToString:key])
                controller->_backdrop.image = image;
        });
    }];
    [_sceneTask resume];
}

- (void)playSelected
{
    if (!_client || _busy || !_play.enabled || _selected == NSNotFound ||
        static_cast<std::size_t>(_selected) >= _results.size() ||
        (self.presentedViewController && self.presentedViewController != _search)) return;
    auto player = [[LunaPlayerController alloc] initWithClient:_client game:*_results[_selected]];
    player.modalPresentationStyle = UIModalPresentationFullScreen;
    [_search.searchBar resignFirstResponder];
    void (^launch)(void) = ^{
        [self cancelCatalogWork];
        [self presentViewController:player animated:YES completion:nil];
    };
    if (self.presentedViewController == _search)
        [_search dismissViewControllerAnimated:NO completion:launch];
    else
    {
        _search.active = NO;
        launch();
    }
}
@end
