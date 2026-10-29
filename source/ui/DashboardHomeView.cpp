#include "ui/DashboardHomeView.hpp"

#include "app/AppState.hpp"
#include "app/Config.hpp"
#include "ui/GameListView.hpp"
#include "ui/GameSearch.hpp"
#include "ui/LibraryScanView.hpp"
#include "ui/MainMenuOptionsView.hpp"
#include "ui/PushedActivity.hpp"
#include "ui/ScrapeMenuView.hpp"
#include "ui/ScreensaverView.hpp"
#include "ui/SettingsTab.hpp"
#include "ui/SystemJumpView.hpp"
#include "ui/SystemsBrowserData.hpp"
#include "ui/ThemeManager.hpp"
#include "ui/UiSfx.hpp"
#include "util/ActionLog.hpp"
#include "util/FileSystem.hpp"
#include "util/NavigationState.hpp"
#include "util/Placeholders.hpp"
#include "util/VirtualSystems.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace sf::ui {

namespace {

constexpr int kMaxRowCards = 8;

float token(const char* key, float fallback)
{
    return ThemeManager::instance().styleNum(key, fallback);
}

// Per-component token with a chain fallback: use `primary` when the theme
// defines it, otherwise fall back to the shared `secondary` token (whose own
// fallback is `hardDefault`, i.e. the previous hardcoded value).
float tokenChain(const char* primary, const char* secondary, float hardDefault)
{
    if (ThemeManager::instance().hasStyle(primary))
        return ThemeManager::instance().styleNum(primary, hardDefault);
    return token(secondary, hardDefault);
}

std::string tokenStr(const char* key, const char* fallback)
{
    return ThemeManager::instance().style(key, fallback);
}

// Image position token: "left" | "center" | "right".
brls::ImageAlignment imageAlign(const char* key, brls::ImageAlignment fallback)
{
    const std::string value = ThemeManager::instance().style(key, "");
    if (value == "left")
        return brls::ImageAlignment::LEFT;
    if (value == "right")
        return brls::ImageAlignment::RIGHT;
    return fallback;
}

// Scroll behavior token: "centered" | "natural".
brls::ScrollingBehavior scrollBehavior(const char* key, brls::ScrollingBehavior fallback)
{
    const std::string value = ThemeManager::instance().style(key, "");
    if (value == "natural")
        return brls::ScrollingBehavior::NATURAL;
    if (value == "centered")
        return brls::ScrollingBehavior::CENTERED;
    return fallback;
}

// Height needed to wrap a tile precisely around the loaded image at `width`
// (its own aspect ratio), or `fallback` when the image dimensions are unknown.
// Corner radius for an image sitting inside a rounded card: the concentric radius
// (card radius - padding) keeps the two arcs parallel, with a floor so the cover still
// reads as rounded when the padding is large.
// Resolve the cover for an image view. Always sets something: a game with no scraped
// art falls back to the theme placeholder, then to the bundled one. Without this the
// tile would keep the previously loaded texture.
void setCoverArt(brls::Image* image, const GameItem& game)
{
    const std::string art = game.meta.boxArtPath;
    if (!art.empty() && FileSystem::exists(art)) {
        image->setImageFromFile(art);
        return;
    }

    const std::string custom = sf::gameArtPlaceholderPath();
    if (!custom.empty())
        image->setImageFromFile(custom);
    else
        image->setImageFromRes(sf::gameArtPlaceholderRes());
}

float imageRadius(const char* key, float cardRadius, float padding, float minRadius)
{
    if (ThemeManager::instance().hasStyle(key))
        return ThemeManager::instance().styleNum(key, minRadius);
    return std::clamp(cardRadius - padding, minRadius, cardRadius);
}

float wrappedImageHeight(brls::Image* image, float width, float fallback)
{
    const float ow = image->getOriginalImageWidth();
    const float oh = image->getOriginalImageHeight();
    if (ow > 0 && oh > 0)
        return width * oh / ow;
    return fallback;
}

float wrappedImageWidth(brls::Image* image, float height, float fallback)
{
    const float ow = image->getOriginalImageWidth();
    const float oh = image->getOriginalImageHeight();
    if (ow > 0 && oh > 0)
        return height * ow / oh;
    return fallback;
}

NVGcolor mutedColor()
{
    return ThemeManager::instance().color("nxstation/muted_text");
}

std::string firstExistingPath(const std::vector<std::string>& candidates)
{
    for (const auto& path : candidates) {
        if (!path.empty())
            return path;
    }
    return {};
}

size_t gameIndexForPath(const std::string& systemId, const std::string& romPath)
{
    const auto& games = AppState::instance().gamesFor(systemId);
    for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].path == romPath)
            return i;
    }
    return 0;
}

std::string releaseYear(const std::string& date)
{
    if (date.size() >= 4 && std::isdigit(static_cast<unsigned char>(date[0])))
        return date.substr(0, 4);
    return {};
}

} // namespace

void MultilineLabel::setMaxLines(int lines)
{
    maxLines_ = std::max(0, lines);
    trimming_ = false;
    measuredText_.clear();
    invalidate();
}

void MultilineLabel::onLayout()
{
    brls::Label::onLayout();

    if (maxLines_ <= 0)
        return;

    const float width = this->getWidth();
    if (width == 0)
        return;

    NVGcontext* vg = brls::Application::getNVGContext();
    if (!vg)
        return;

    const std::string text = this->getFullText();
    if (text.empty())
        return;

    // If the text changed (new full text from updateHero), reset trimming state
    // so we re-measure and truncate the new text. measuredText_ holds the last
    // truncated version, so a mismatch means the content was replaced.
    if (text != measuredText_)
        trimming_ = false;

    if (trimming_)
        return;

    nvgFontSize(vg, this->getFontSize());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFaceId(vg, this->getFont());
    nvgTextLineHeight(vg, this->getLineHeight());

    const float lineHeightPx = this->getFontSize() * this->getLineHeight();
    const float maxPx = lineHeightPx * maxLines_;

    auto textHeight = [&](const std::string& value) {
        float bounds[4];
        nvgTextBoxBounds(vg, 0, 0, width, value.c_str(), nullptr, bounds);
        return bounds[3] - bounds[1];
    };

    if (textHeight(text) <= maxPx + 0.5f) {
        measuredText_ = text;
        return;
    }

    // Longest prefix that still fits in maxLines lines once the ellipsis is appended.
    const std::string ellipsis = "\u2026";
    float ellipsisBounds[4];
    nvgTextBounds(vg, 0, 0, ellipsis.c_str(), nullptr, ellipsisBounds);

    size_t low = 0;
    size_t high = text.size();
    while (low < high) {
        const size_t mid = (low + high + 1) / 2;
        if (textHeight(text.substr(0, mid) + ellipsis) <= maxPx + 0.5f)
            low = mid;
        else
            high = mid - 1;
    }

    std::string cut = text.substr(0, low);
    const size_t lastWord = cut.find_last_of(" \n");
    if (lastWord != std::string::npos && lastWord > cut.size() / 2)
        cut = cut.substr(0, lastWord);

    measuredText_ = cut + ellipsis;
    trimming_ = true;
    setText(measuredText_); // measured again on the next layout pass, then it stays stable
}

void DashboardCell::onFocusGained()
{
    brls::Box::onFocusGained();
    const auto& theme = ThemeManager::instance();
    if (theme.hasStyle("focus_glow"))
        setBorderColor(theme.styleColor("focus_glow", "brls/highlight/color1"));
    setBorderThickness(theme.styleNum("focus_glow_width", 2.f));
    if (owner_)
        owner_->onCellFocused(this);
}

void DashboardCell::onFocusLost()
{
    brls::Box::onFocusLost();
    setBorderColor(idleBorderColor_);
}

// --- DashboardHomeView ---

DashboardHomeView::DashboardHomeView()
{
    rng_.seed(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));

    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.f);

    registerActions();
    rebuild();
}

brls::View* DashboardHomeView::create()
{
    return new DashboardHomeView();
}

void DashboardHomeView::registerActions()
{
    // No nav-button actions here on purpose: Borealis moves focus between the
    // focusable cells when nothing consumes the D-pad. A handler that returns
    // true would swallow every ↑↓←→ and freeze the focus chain.

    registerAction("Open", brls::ControllerButton::BUTTON_A, [this](brls::View*) {
        // Actions bubble from the focused cell up to this view, so resolve focus through
        // the application rather than the listener argument.
        auto* cell = dynamic_cast<DashboardCell*>(brls::Application::getCurrentFocus());
        if (cell)
            openCell(cell);
        return true;
    });

    registerAction("Settings", brls::ControllerButton::BUTTON_START, [](brls::View*) {
        SF_LOG_ACTION("Dashboard/Settings");
        playConfirmSfx();
        brls::sync([]() { SettingsTab::open(); });
        return true;
    });

    registerAction("Screensaver", brls::ControllerButton::BUTTON_BACK, [](brls::View*) {
        if (browser::overlayBlocksHostInput() || ScreensaverView::isActive())
            return true;
        ScreensaverView::present();
        return true;
    });

    registerAction("Menu", brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        SF_LOG_ACTION("Systems/MainMenu");
        playConfirmSfx();
        MainMenuOptionsView::present([this](MainMenuAction action) {
            if (action == MainMenuAction::Search)
                GameSearch::prompt();
            else if (action == MainMenuAction::JumpToSystem)
                SystemJumpView::present(systemIds_, [this](std::string systemId) {
                    brls::sync([this, systemId]() {
                        for (size_t i = 0; i < systemIds_.size(); ++i) {
                            if (systemIds_[i] == systemId) {
                                brls::Application::giveFocus(
                                    focusTargets_.count(systemId) ? focusTargets_[systemId]
                                                                 : firstFocusable_);
                                return;
                            }
                        }
                    });
                });
            else if (action == MainMenuAction::ScanGames)
                LibraryScanView::presentAll(true);
            else if (action == MainMenuAction::ScrapeLibrary)
                ScrapeMenuView::presentLibraryAndStart();
            else if (action == MainMenuAction::Settings)
                SettingsTab::open();
        });
        return true;
    });
}

void DashboardHomeView::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);

    if (browser::consumeSystemsDataRefresh())
        rebuild();
    if (firstFocusable_)
        brls::Application::giveFocus(firstFocusable_);
}

void DashboardHomeView::frame(brls::FrameContext* ctx)
{
    const uint32_t themeGen = ThemeManager::instance().uiGeneration();
    if (themeGen != themeGenApplied_) {
        themeGenApplied_ = themeGen;
        brls::sync([this]() { rebuild(); });
    }

    brls::Box::frame(ctx);
}

void DashboardHomeView::onCellFocused(DashboardCell* cell)
{
    if (!firstFocus_)
        playNavSfx();

    if (cell->isSystemCell()) {
        // Which game the Focus block shows for a platform tile is a theme behavior:
        // tile_focus_pick = "random" (default, a game picked inside that platform) or
        // "first" (the top of its list).
        const auto& games = AppState::instance().gamesFor(cell->systemId());
        if (games.empty()) {
            updateHero(nullptr);
            return;
        }

        if (tokenStr("tile_focus_pick", "random") == "first")
            updateHero(&games.front());
        else
            updateHero(pickRandomGame(cell->systemId()));
    } else {
        const auto& games = AppState::instance().gamesFor(cell->systemId());
        const GameItem* game = nullptr;
        for (const auto& item : games) {
            if (item.path == cell->romPath()) {
                game = &item;
                break;
            }
        }
        updateHero(game);
    }
}

void DashboardHomeView::openCell(DashboardCell* cell)
{
    if (cell->isSystemCell() && AppState::instance().gamesFor(cell->systemId()).empty())
        return;

    playConfirmSfx();
    SF_LOG_ACTION("Dashboard/Open");
    NavigationState::setLastSystem(cell->systemId());

    const std::string systemId = cell->systemId();
    size_t focus = gameIndexForPath(systemId, cell->romPath());
    if (cell->isSystemCell()) {
        // Land on the game the Focus block is showing for this platform.
        focus = (heroGame_.systemId == systemId && !heroGame_.path.empty())
                    ? gameIndexForPath(systemId, heroGame_.path)
                    : 0;
    }
    brls::sync([systemId, focus]() { PushedActivity::push(new GameListView(systemId, focus)); });
}

// --- Builders ---

brls::Box* DashboardHomeView::makeSectionHeader(const std::string& title,
                                                 const std::string& count)
{
    auto& theme = ThemeManager::instance();

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::BASELINE);
    row->setPadding(0, 0, token("header_gap", 10.f), 0);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(token("font_section", 20.f));
    label->setTextColor(theme.color("brls/text"));
    label->setSingleLine(true);
    row->addView(label);

    if (!count.empty()) {
        auto* countLabel = new brls::Label();
        countLabel->setText("  " + count);
        countLabel->setFontSize(token("font_meta", 14.f));
        countLabel->setTextColor(theme.color("nxstation/count_text"));
        countLabel->setSingleLine(true);
        row->addView(countLabel);
    }

    return row;
}

brls::Box* DashboardHomeView::makeCard(const GameItem& game, float width, float imageHeight,
                                       const std::string& detail, bool fillHeight,
                                       float* naturalHeight)
{
    auto& theme = ThemeManager::instance();
    const float radius = token("card_radius", 12.f);
    const float pad = token("card_padding", 10.f);

    auto* cell = new DashboardCell(this, game.systemId, game.path, false);
    cell->setAxis(brls::Axis::COLUMN);
    cell->setPadding(pad, pad, pad, pad);
    cell->setCornerRadius(radius);
    cell->setBackgroundColor(theme.color("nxstation/card_bg"));
    cell->setBorderColor(theme.color("nxstation/card_border"));
    cell->setIdleBorderColor(theme.color("nxstation/card_border"));
    cell->setFocusable(true);
    cell->setFocusSound(brls::SOUND_NONE);
    cell->setHideHighlight(true);
    cell->setMarginRight(token("card_gap", 10.f));
    cell->setWidth(width);

    auto* image = new brls::Image();
    const float innerWidth = width - pad * 2;
    image->setWidth(innerWidth);
    image->setCornerRadius(imageRadius("card_image_radius", radius, pad, 4.f));
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setImageAlign(imageAlign("card_image_align", brls::ImageAlignment::CENTER));

    setCoverArt(image, game);

    // Wrap the card precisely around the cover shape when its aspect ratio is known.
    // imageHeight is the fallback/cap for cards that should not grow.
    float imageHeightUsed = imageHeight;
    if (fillHeight) {
        image->setGrow(1.f); // fills the row height Yoga gives the card
    } else {
        imageHeightUsed = std::min(imageHeight, wrappedImageHeight(image, innerWidth, imageHeight));
        image->setHeight(imageHeightUsed);
    }
    cell->addView(image);

    auto* title = new brls::Label();
    title->setText(game.meta.title.empty() ? game.displayName : game.meta.title);
    title->setFontSize(tokenChain("card_title_font", "font_body", 17.f));
    title->setTextColor(theme.color("nxstation/title_text"));
    title->setSingleLine(true);
    title->setAnimated(false);
    title->setWidth(innerWidth);
    cell->addView(title);

    if (!detail.empty()) {
        auto* detailLabel = new brls::Label();
        detailLabel->setText(detail);
        detailLabel->setFontSize(tokenChain("card_detail_font", "font_meta", 14.f));
        detailLabel->setTextColor(theme.color("nxstation/detail_text"));
        detailLabel->setSingleLine(true);
        detailLabel->setAnimated(false);
        detailLabel->setWidth(innerWidth);
        cell->addView(detailLabel);
    }

    if (naturalHeight != nullptr) {
        const float titleFont = tokenChain("card_title_font", "font_body", 17.f);
        const float detailFont = tokenChain("card_detail_font", "font_meta", 14.f);
        *naturalHeight = pad * 2 + imageHeightUsed + titleFont * 1.3f +
                         (detail.empty() ? 0.f : detailFont * 1.3f);
    }

    return cell;
}

brls::Box* DashboardHomeView::makeTile(const std::string& systemId)
{
    auto& theme = ThemeManager::instance();
    const float size = token("tile_size", 112.f);
    const float pad = tokenChain("tile_padding", "card_padding", 8.f);
    const float radius = tokenChain("tile_radius", "card_radius", 12.f);
    const float iconSize = token("tile_icon", 40.f);

    auto* tile = new DashboardCell(this, systemId, {}, true);
    tile->setAxis(brls::Axis::COLUMN);
    tile->setAlignItems(brls::AlignItems::CENTER);
    tile->setJustifyContent(brls::JustifyContent::CENTER);
    tile->setPadding(pad, pad, pad, pad);
    tile->setDimensions(size, size);
    tile->setCornerRadius(radius);
    tile->setBackgroundColor(theme.color("nxstation/card_bg"));
    tile->setBorderColor(theme.color("nxstation/card_border"));
    tile->setIdleBorderColor(theme.color("nxstation/card_border"));
    tile->setFocusable(true);
    tile->setFocusSound(brls::SOUND_NONE);
    tile->setHideHighlight(true);

    const std::string icon = theme.systemIconPath(systemId);
    if (!icon.empty()) {
        auto* image = new brls::Image();
        image->setDimensions(iconSize, iconSize);
        image->setCornerRadius(imageRadius("tile_icon_radius", radius, pad, 4.f));
        image->setScalingType(brls::ImageScalingType::FIT);
        image->setImageAlign(imageAlign("tile_image_align", brls::ImageAlignment::CENTER));
        image->setImageFromFile(icon);
        tile->addView(image);
    }

    if (tokenStr("tile_show_name", "true") == "true") {
        auto* spacer = new brls::Box();
        spacer->setHeight(token("tile_name_gap", 6.f));
        tile->addView(spacer);

        const float nameFont = tokenChain("tile_name_font", "card_detail_font", 13.f);
        const int nameLines = std::max(1, static_cast<int>(token("tile_name_lines", 2.f)));

        auto* label = new MultilineLabel();
        label->setText(browser::systemDisplayName(systemId));
        label->setFontSize(nameFont);
        label->setTextColor(theme.color("nxstation/favorite_text"));
        label->setSingleLine(false);            // wrap, like the RomM platform cards
        label->setLineHeight(1.3f);
        label->setMaxLines(nameLines);
        // Wrapping needs a constrained width; the height stays free so the label can grow.
        label->setWidth(size - pad * 2);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        label->setAnimated(false);
        tile->addView(label);
    }

    focusTargets_[systemId] = tile;
    return tile;
}

brls::Box* DashboardHomeView::makeBadge(const std::string& text)
{
    auto& theme = ThemeManager::instance();
    const float height = token("badge_height", 26.f);

    auto* badge = new brls::Box();
    badge->setAxis(brls::Axis::ROW);
    badge->setAlignItems(brls::AlignItems::CENTER);
    badge->setPadding(0, token("badge_padding", 12.f), 0, token("badge_padding", 12.f));
    badge->setHeight(height);
    badge->setCornerRadius(std::min(token("badge_radius", 14.f), height / 2.f));
    badge->setBackgroundColor(theme.styleColor("badge_bg", "brls/highlight/background"));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(tokenChain("badge_font", "font_meta", 14.f));
    label->setTextColor(theme.color("brls/text"));
    label->setSingleLine(true);
    label->setAnimated(false);
    badge->addView(label);

    return badge;
}

void DashboardHomeView::buildTopBar()
{
    auto& theme = ThemeManager::instance();

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(0, 0, token("topbar_gap", 18.f), 0);

    const std::string logo = theme.headerLogoPath();
    if (!logo.empty()) {
        const float logoSize = token("topbar_logo", 44.f);
        auto* image = new brls::Image();
        image->setDimensions(logoSize, logoSize);
        image->setScalingType(brls::ImageScalingType::FIT);
        image->setImageFromFile(logo);
        row->addView(image);

        auto* spacer = new brls::Box();
        spacer->setHeight(1.f);
        spacer->setMarginRight(token("brand_gap", 14.f));
        row->addView(spacer);
    }

    auto* brand = new brls::Box();
    brand->setAxis(brls::Axis::COLUMN);

    auto* title = new brls::Label();
    title->setText("NXStation");
    title->setFontSize(tokenChain("brand_font", "font_title", 28.f));
    title->setTextColor(theme.color("brls/text"));
    title->setSingleLine(true);
    title->setAnimated(false);
    brand->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText(tokenStr("brand_subtitle", "Your retro collection"));
    subtitle->setFontSize(tokenChain("brand_sub_font", "font_meta", 13.f));
    subtitle->setTextColor(theme.color("brls/header/subtitle"));
    subtitle->setSingleLine(true);
    subtitle->setAnimated(false);
    brand->addView(subtitle);

    row->addView(brand);

    auto* grow = new brls::Box();
    grow->setGrow(1.f);
    row->addView(grow);

    size_t total = 0;
    for (const auto& sys : Config::instance().systems())
        total += AppState::instance().gamesFor(sys.id).size();

    auto* count = new brls::Label();
    count->setText(std::to_string(total) + (total == 1 ? " game" : " games"));
    count->setFontSize(tokenChain("count_font", "font_body", 17.f));
    count->setTextColor(theme.color("nxstation/count_text"));
    count->setSingleLine(true);
    count->setAnimated(false);
    row->addView(count);

    this->addView(row);
}

brls::Box* DashboardHomeView::buildHeroSection(float columnWidth)
{
    auto& theme = ThemeManager::instance();
    const float radius = tokenChain("hero_tile_radius", "card_radius", 12.f);
    const float pad = token("hero_tile_padding", 4.f);
    const float heroImageW = token("hero_image_width", 140.f);
    const float fixedHeight = token("hero_fixed_height", 0.f);

    // Hero tile (focusable): wraps tightly around the shape of the cover image.
    heroImage_ = new brls::Image();
    heroImage_->setWidth(heroImageW);
    heroImage_->setCornerRadius(imageRadius("hero_image_radius", radius, pad, 4.f));
    heroImage_->setScalingType(brls::ImageScalingType::FIT);
    heroImage_->setImageAlign(imageAlign("hero_image_align", brls::ImageAlignment::CENTER));

    auto& games = AppState::instance().gamesFor(systemIds_.empty() ? std::string() : systemIds_.front());

    // Default to the most recently played game (from the lastPlayed virtual system).
    // When a platform tile or game card gains focus, onCellFocused will update the hero.
    const auto& recentGames = AppState::instance().gamesFor(kLastPlayedSystemId);
    const GameItem heroSeed = recentGames.empty() ? GameItem{} : recentGames.front();
    setCoverArt(heroImage_, heroSeed);
    heroImage_->setHeight(wrappedImageHeight(heroImage_, heroImageW,
                                             token("hero_image_height", heroImageW * 1.4f)));

    auto* card = new DashboardCell(this, heroSeed.systemId, heroSeed.path, heroSeed.path.empty());
    card->setAxis(brls::Axis::COLUMN);
    card->setPadding(pad, pad, pad, pad);
    card->setCornerRadius(radius);
    card->setBackgroundColor(theme.color("nxstation/card_bg"));
    card->setBorderColor(theme.color("nxstation/card_border"));
    card->setFocusable(true);
    card->setFocusSound(brls::SOUND_NONE);
    card->setHideHighlight(true);
    card->addView(heroImage_);

    // The text column needs an explicit width for labels to wrap (Borealis labels
    // only wrap when their width is constrained).
    const float textWidth = std::min(token("hero_text_width", 860.f),
                                     columnWidth - heroImageW - pad * 2 - token("hero_gap", 20.f));

    auto* middle = new brls::Box();
    middle->setAxis(brls::Axis::COLUMN);
    middle->setWidth(textWidth);
    middle->setMarginLeft(token("hero_gap", 20.f));

    // When hero_fixed_height is set, constrain the middle column so badges
    // don't push the layout. The description truncates at hero_desc_lines.
    if (fixedHeight > 0.f) {
        // Estimate header height: section header (~30) + section_gap (14) = ~44
        // Row gets the rest. Middle column matches row height.
        middle->setHeight(fixedHeight - 44.f);
    }

    heroTitle_ = new brls::Label();
    heroTitle_->setFontSize(tokenChain("hero_title_font", "font_title", 28.f));
    heroTitle_->setTextColor(theme.color("nxstation/title_text"));
    heroTitle_->setSingleLine(true);
    heroTitle_->setAnimated(false);
    heroTitle_->setWidth(textWidth);
    middle->addView(heroTitle_);

    heroMeta_ = new brls::Label();
    heroMeta_->setFontSize(tokenChain("hero_meta_font", "font_meta", 14.f));
    heroMeta_->setSingleLine(true);
    heroMeta_->setAnimated(false);
    heroMeta_->setWidth(textWidth);
    middle->addView(heroMeta_);

    heroBadgeRow_ = new brls::Box();
    heroBadgeRow_->setAxis(brls::Axis::ROW);
    heroBadgeRow_->setPadding(token("hero_badge_pad", 10.f), 0, token("hero_badge_pad", 10.f), 0);
    heroBadgeRow_->setWidth(textWidth);
    middle->addView(heroBadgeRow_);

    const int descLines = std::max(1, static_cast<int>(token("hero_desc_lines", 4.f)));

    auto* desc = new MultilineLabel();
    desc->setFontSize(tokenChain("hero_desc_font", "font_body", 15.f));
    desc->setTextColor(theme.color("nxstation/body_text"));
    desc->setSingleLine(false);
    desc->setLineHeight(1.4f);
    desc->setMaxLines(descLines);
    desc->setVerticalAlign(brls::VerticalAlign::TOP);
    desc->setWidth(textWidth); // constrained width => real wrapping, height grows
    desc->setAnimated(false);
    if (ThemeManager::instance().hasStyle("hero_desc_height"))
        desc->setMinHeight(token("hero_desc_height", 90.f)); // keeps the block stable for short descriptions
    heroDescription_ = desc;
    middle->addView(desc);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::FLEX_START);
    row->addView(card);
    row->addView(middle);

    // Constrain row height when hero_fixed_height is set
    if (fixedHeight > 0.f) {
        row->setHeight(fixedHeight - 44.f); // minus header
    }

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setPadding(0, 0, token("section_gap", 14.f), 0);
    section->addView(makeSectionHeader("Focus", {}));
    section->addView(row);

    // Fixed height for the Focus section so the layout stays stable when content changes.
    if (fixedHeight > 0.f)
        section->setHeight(fixedHeight);

    firstFocusable_ = card;
    focusTargets_[card->systemId()] = card;

    updateHero(recentGames.empty() ? nullptr : &heroSeed);
    return section;
}

brls::Box* DashboardHomeView::buildGamesSection(const std::string& headerTitle,
                                                 const std::vector<GameItem>& games)
{
    if (games.empty())
        return nullptr;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setPadding(0, 0, token("section_gap", 14.f), 0);
    section->addView(makeSectionHeader(headerTitle, std::to_string(games.size())));

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::FLEX_START);

    const float cardWidth = token("card_width", 160.f);
    const float cardImage = token("card_image_height", 120.f);

    for (size_t i = 0; i < games.size() && i < static_cast<size_t>(token("row_cards", kMaxRowCards)); ++i) {
        const GameItem& game = games[i];
        const std::string detail = game.systemId.empty()
                                       ? game.meta.genre
                                       : systemAcronym(game.systemId);
        auto* card = makeCard(game, cardWidth, cardImage, detail);
        if (i == 0 && !firstFocusable_)
            firstFocusable_ = card;
        row->addView(card);
    }

    section->addView(row);
    return section;
}

const GameItem* DashboardHomeView::pickRandomGame(const std::string& systemId)
{
    const auto& games = AppState::instance().gamesFor(systemId);
    if (games.empty())
        return nullptr;
    return &games[std::uniform_int_distribution<size_t>(0, games.size() - 1)(rng_)];
}

std::vector<GameItem> DashboardHomeView::randomGames(size_t count)
{
    std::vector<GameItem> pool;
    for (const auto& sys : Config::instance().systems()) {
        const auto& games = AppState::instance().gamesFor(sys.id);
        pool.insert(pool.end(), games.begin(), games.end());
    }
    if (pool.empty())
        return {};

    std::shuffle(pool.begin(), pool.end(), rng_);
    if (pool.size() > count)
        pool.resize(count);
    return pool;
}

brls::Box* DashboardHomeView::buildRandomSection()
{
    const size_t count = static_cast<size_t>(token("random_games", 10.f));
    const size_t visible = std::max<size_t>(1, static_cast<size_t>(token("random_per_row", 5.f)));
    const std::vector<GameItem> games = randomGames(count);
    if (games.empty())
        return nullptr;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setPadding(0, 0, token("section_gap", 14.f), 0);
    section->addView(makeSectionHeader("Random picks", std::to_string(games.size())));

    const float gap = tokenChain("random_gap", "card_gap", 10.f);
    const float cardImage = token("card_image_height", 120.f);
    const float pad = token("card_padding", 10.f);
    const float titleFont = tokenChain("card_title_font", "font_body", 17.f);
    const float detailFont = tokenChain("card_detail_font", "font_meta", 14.f);
    const float titleHeight = titleFont * 1.3f;
    const float detailHeight = detailFont * 1.3f;

    // If the theme pins a fixed card width, use the old uniform-width behavior.
    if (ThemeManager::instance().hasStyle("random_card_width")) {
        const float cardWidth = token("random_card_width", 224.f);

        std::vector<brls::Box*> cards;
        float tallest = 0.f;
        for (const auto& game : games) {
            const std::string detail = game.systemId.empty()
                                           ? game.meta.genre
                                           : systemAcronym(game.systemId);
            float natural = 0.f;
            auto* card = makeCard(game, cardWidth, cardImage, detail, false, &natural);
            card->setMarginRight(gap);
            cards.push_back(card);
            tallest = std::max(tallest, natural);
        }

        if (ThemeManager::instance().hasStyle("random_card_height"))
            tallest = std::min(tallest, token("random_card_height", tallest));

        auto* content = new brls::Box();
        content->setAxis(brls::Axis::ROW);
        content->setAlignItems(brls::AlignItems::FLEX_START);

        for (auto* card : cards) {
            card->setHeight(tallest);
            if (!firstFocusable_)
                firstFocusable_ = card;
            content->addView(card);
        }

        auto* frame = new brls::HScrollingFrame();
        frame->setWidthPercentage(100.f);
        frame->setHeight(tallest + tokenChain("random_indicator_gap", "tile_indicator_gap", 22.f));
        frame->setScrollingBehavior(scrollBehavior("random_scroll_behavior",
                                                   brls::ScrollingBehavior::CENTERED));
        frame->setContentView(content);

        section->addView(frame);
        return section;
    }

    // Adaptive-width mode: each card gets a width that fits its cover at a common height.
    // 1) Create a provisional card for each game to load the cover and measure its aspect ratio.
    struct ProvisionalCard {
        GameItem game;
        std::string detail;
        brls::Box* card;
        brls::Image* image;
        float aspectRatio; // width / height of the cover
        float naturalHeight;
    };
    std::vector<ProvisionalCard> provisional;

    // Use a provisional width based on random_per_row to trigger image loading.
    const float inner = 1280.f - token("page_padding", 24.f) * 2.f;
    const float provisionalWidth = (inner - static_cast<float>(visible) * gap) / static_cast<float>(visible);

    for (const auto& game : games) {
        const std::string detail = game.systemId.empty()
                                       ? game.meta.genre
                                       : systemAcronym(game.systemId);

        // Create a temporary card to load the image and get its dimensions.
        auto* tempCard = makeCard(game, provisionalWidth, cardImage, detail, false, nullptr);
        // The image is the first child of the cell.
        brls::Image* tempImage = nullptr;
        for (auto* child : tempCard->getChildren()) {
            if (dynamic_cast<brls::Image*>(child)) {
                tempImage = static_cast<brls::Image*>(child);
                break;
            }
        }

        float aspectRatio = 0.75f; // fallback 3:4 portrait
        if (tempImage) {
            const float ow = tempImage->getOriginalImageWidth();
            const float oh = tempImage->getOriginalImageHeight();
            if (ow > 0 && oh > 0)
                aspectRatio = ow / oh;
        }

        // Measure natural height with the provisional width.
        float natural = 0.f;
        makeCard(game, provisionalWidth, cardImage, detail, false, &natural);
        // Clean up the temp card (it was added to no parent, so we own it).
        delete tempCard;

        provisional.push_back({game, detail, nullptr, nullptr, aspectRatio, natural});
    }

    // Determine the target total card height (tallest natural height).
    float tallest = 0.f;
    for (const auto& pc : provisional)
        tallest = std::max(tallest, pc.naturalHeight);

    if (ThemeManager::instance().hasStyle("random_card_height"))
        tallest = std::min(tallest, token("random_card_height", tallest));

    // The image height inside the card at this total height.
    const float targetImageHeight = tallest - pad * 2 - titleHeight - detailHeight;

    // 2) Build the final cards with per-card widths.
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setAlignItems(brls::AlignItems::FLEX_START);

    for (auto& pc : provisional) {
        // Width needed for this cover to fit at targetImageHeight.
        const float imageWidth = targetImageHeight * pc.aspectRatio;
        const float cardWidth = imageWidth + pad * 2;

        float natural = 0.f;
        auto* card = makeCard(pc.game, cardWidth, cardImage, pc.detail, false, &natural);
        card->setHeight(tallest);
        card->setMarginRight(gap);
        if (!firstFocusable_)
            firstFocusable_ = card;
        content->addView(card);
    }

    auto* frame = new brls::HScrollingFrame();
    frame->setWidthPercentage(100.f);
    frame->setHeight(tallest + tokenChain("random_indicator_gap", "tile_indicator_gap", 22.f));
    frame->setScrollingBehavior(scrollBehavior("random_scroll_behavior",
                                               brls::ScrollingBehavior::CENTERED));
    frame->setContentView(content);

    section->addView(frame);
    return section;
}

brls::Box* DashboardHomeView::buildPlatformsSection()
{
    // Only the platforms with at least one game in them.
    std::vector<std::string> platforms;
    for (const auto& sys : Config::instance().systems()) {
        if (AppState::instance().gamesFor(sys.id).empty())
            continue;
        platforms.push_back(sys.id);
    }
    if (platforms.empty())
        return nullptr;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setPadding(0, 0, token("section_gap", 14.f), 0);
    section->addView(makeSectionHeader("Platforms", std::to_string(platforms.size())));

    const float tileSize = token("tile_size", 112.f);

    // HScrollingFrame scrolls left-right; padding is not supported on the frame,
    // so all spacing lives on the content view.
    auto* frame = new brls::HScrollingFrame();
    frame->setWidthPercentage(100.f);
    frame->setHeight(tileSize + token("tile_indicator_gap", 22.f));
    frame->setScrollingBehavior(scrollBehavior("tile_scroll_behavior",
                                               brls::ScrollingBehavior::CENTERED));

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setAlignItems(brls::AlignItems::FLEX_START);

    for (const auto& id : platforms) {
        auto* tile = makeTile(id);
        tile->setMarginRight(tokenChain("tile_gap", "card_gap", 10.f));
        if (!firstFocusable_)
            firstFocusable_ = tile;
        content->addView(tile);
    }

    frame->setContentView(content);
    section->addView(frame);
    return section;
}

brls::Box* DashboardHomeView::buildMetadataSection()
{
    static const char* kKeys[] = {"Platform", "Genre", "Released", "Developer"};

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setPadding(0, 0, token("section_gap", 14.f), 0);
    section->addView(makeSectionHeader("Details", {}));

    for (const char* key : kKeys) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setPadding(0, 0, token("meta_row_gap", 6.f), 0);

        auto* keyLabel = new brls::Label();
        keyLabel->setText(key);
        keyLabel->setFontSize(tokenChain("meta_key_font", "font_meta", 14.f));
        keyLabel->setTextColor(mutedColor());
        keyLabel->setSingleLine(true);
        keyLabel->setAnimated(false);
        keyLabel->setDimensions(token("meta_key_width", 110.f), 20.f);
        row->addView(keyLabel);

        auto* valueLabel = new brls::Label();
        valueLabel->setText("—");
        valueLabel->setFontSize(tokenChain("meta_value_font", "font_body", 17.f));
        valueLabel->setTextColor(ThemeManager::instance().color("nxstation/detail_text"));
        valueLabel->setSingleLine(true);
        valueLabel->setAnimated(false);
        valueLabel->setGrow(1.f);
        row->addView(valueLabel);

        metaLabels_[key] = valueLabel;
        section->addView(row);
    }

    return section;
}

void DashboardHomeView::setMetaRow(const std::string& key, const std::string& value)
{
    auto it = metaLabels_.find(key);
    if (it != metaLabels_.end())
        it->second->setText(value.empty() ? "—" : value);
}

void DashboardHomeView::setHeroBadge(const std::string& text)
{
    if (!heroBadgeRow_)
        return;
    auto* badge = makeBadge(text);
    badge->setMarginRight(token("hero_badge_gap", 8.f));
    heroBadgeRow_->addView(badge);
}

void DashboardHomeView::updateHero(const GameItem* game)
{
    if (!heroTitle_)
        return;

    heroGame_ = game ? *game : GameItem{};

    heroTitle_->setText(game ? (game->meta.title.empty() ? game->displayName : game->meta.title)
                             : "No games");

    std::string systemName = game ? browser::systemDisplayName(game->systemId) : "";
    heroMeta_->setText(game ? (systemName +
                                (game->meta.genre.empty() ? "" : "  |  " + game->meta.genre) +
                                (releaseYear(game->meta.releaseDate).empty()
                                     ? ""
                                     : "  |  " + releaseYear(game->meta.releaseDate)))
                             : "—");

    heroBadgeRow_->clearViews(false);
    if (game) {
        if (game->cloudAvailable)
            setHeroBadge("Remote");
        else if (game->fromRomM)
            setHeroBadge("Downloaded");
        if (game->discCount > 1)
            setHeroBadge(std::to_string(game->discCount) + " discs");
        if (!game->meta.scraped)
            setHeroBadge("No metadata");
    }

    heroDescription_->setText(game && !game->meta.description.empty()
                                  ? game->meta.description
                                  : "No description — scrape from the Y menu to fill in metadata.");

    if (heroImage_) {
        const float w = token("hero_image_width", 140.f);
        setCoverArt(heroImage_, game ? *game : GameItem{});
        // The tile adapts to the newly focused cover's shape.
        heroImage_->setHeight(wrappedImageHeight(heroImage_, w,
                                                 token("hero_image_height", w * 1.4f)));
    }

    if (!metaLabels_.empty()) {
        setMetaRow("Platform", systemName);
        setMetaRow("Genre", game ? game->meta.genre : "");
        setMetaRow("Released", game ? game->meta.releaseDate : "");
        setMetaRow("Developer", game ? game->meta.developer : "");
    }
}

void DashboardHomeView::rebuild()
{
    systemIds_.clear();
    browser::rebuildSystemIds(systemIds_);

    metaLabels_.clear();
    focusTargets_.clear();
    firstFocusable_ = nullptr;
    this->clearViews(true);

    const float pagePad = token("page_padding", 24.f);
    this->setPadding(pagePad, pagePad, pagePad, pagePad);

    buildTopBar();

    const auto& sections = ThemeManager::instance().sections();

    // Focus and Details share the top row: Focus takes hero_width_pct of the row
    // (2/3 by default), Details meta_width_pct (1/3 by default). The column order
    // follows the order the theme lists them in.
    const bool hasHero = std::find(sections.begin(), sections.end(), "hero") != sections.end();
    const bool hasMeta = std::find(sections.begin(), sections.end(), "metadata") != sections.end();

    if (hasHero || hasMeta) {
        const float inner = 1280.f - pagePad * 2.f;
        const float heroPct = token("hero_width_pct", 66.7f);
        auto* hero = hasHero ? buildHeroSection(hasMeta ? inner * heroPct / 100.f : inner) : nullptr;
        auto* meta = hasMeta ? buildMetadataSection() : nullptr;

        if (hero && meta) {
            auto* topRow = new brls::Box();
            topRow->setAxis(brls::Axis::ROW);
            topRow->setAlignItems(brls::AlignItems::FLEX_START);
            topRow->setPadding(0, 0, token("section_gap", 14.f), 0);

            const float columnGap = token("column_gap", 16.f);
            const float metaPct = token("meta_width_pct",
                                        100.f - heroPct - columnGap / inner * 100.f);

            hero->setWidthPercentage(heroPct);
            hero->setMarginRight(columnGap);
            meta->setWidthPercentage(metaPct);

            const auto heroPos = std::find(sections.begin(), sections.end(), "hero");
            const auto metaPos = std::find(sections.begin(), sections.end(), "metadata");
            if (metaPos < heroPos) {
                topRow->addView(meta);
                topRow->addView(hero);
            } else {
                topRow->addView(hero);
                topRow->addView(meta);
            }
            this->addView(topRow);
        } else if (hero) {
            this->addView(hero);
        } else {
            this->addView(meta);
        }
    }

    // Sections below the top row go into a content box. The whole page keeps the
    // natural height of every component; when the theme's numbers make it taller than
    // the screen the body scrolls vertically. Yoga must not be left shrinking the
    // sections, which is what made the content overlap the Platforms row.
    auto* body = new brls::Box();
    body->setAxis(brls::Axis::COLUMN);

    for (const auto& section : sections) {
        if (section == "hero" || section == "metadata")
            continue;

        brls::Box* view = nullptr;
        if (section == "platforms")
            view = buildPlatformsSection();
        else if (section == "random")
            view = buildRandomSection();
        else if (section == "continue")
            view = buildGamesSection("Continue playing",
                                     AppState::instance().gamesFor(kLastPlayedSystemId));
        else if (section == "favourites")
            view = buildGamesSection("Favourites",
                                     AppState::instance().gamesFor(kFavoritesSystemId));
        // Unknown section names are ignored by design (forward-compatible theme keys).

        if (view)
            body->addView(view);
    }

    // page_overflow: "scroll" (default) lets the sections scroll when they do not fit,
    // "fit" keeps the single-screen layout for themes whose sizes already fit.
    if (tokenStr("page_overflow", "scroll") == "scroll") {
        auto* scroller = new brls::ScrollingFrame();
        scroller->setGrow(1.f);
        scroller->setScrollingBehavior(scrollBehavior("page_scroll_behavior",
                                                      brls::ScrollingBehavior::CENTERED));
        scroller->setContentView(body);
        this->addView(scroller);
    } else {
        body->setGrow(1.f);
        this->addView(body);
    }

    auto& theme = ThemeManager::instance();
    auto* hints = new brls::Label();
    hints->setText("↑ ↓ Sections   ·   ← → Cards/Tiles   ·   A Open   ·   Y Menu   ·   - Screensaver   ·   + Settings");
    hints->setFontSize(tokenChain("hint_font", "font_meta", 14.f));
    hints->setTextColor(theme.color("nxstation/hint_text"));
    hints->setSingleLine(true);
    hints->setAnimated(false);
    this->addView(hints);

    if (firstFocusable_) {
        firstFocus_ = true;
        brls::Application::giveFocus(firstFocusable_);
        firstFocus_ = false;
    }
}

} // namespace sf::ui
