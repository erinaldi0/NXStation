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
#include "util/NavigationState.hpp"
#include "util/Placeholders.hpp"
#include "util/VirtualSystems.hpp"

#include <algorithm>
#include <cctype>
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
        const auto& games = AppState::instance().gamesFor(cell->systemId());
        updateHero(games.empty() ? nullptr : &games.front());
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
    const size_t focus = cell->isSystemCell() ? 0 : gameIndexForPath(systemId, cell->romPath());
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
                                       const std::string& detail)
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

    auto* image = new brls::Image();
    image->setDimensions(width - pad * 2, imageHeight);
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setImageAlign(imageAlign("card_image_align", brls::ImageAlignment::CENTER));
    const std::string art = game.meta.boxArtPath;
    const std::string path =
        firstExistingPath({art, theme.boxPlaceholderPath(), sf::gameArtPlaceholderPath()});
    if (!path.empty())
        image->setImageFromFile(path);
    cell->addView(image);

    auto* title = new brls::Label();
    title->setText(game.meta.title.empty() ? game.displayName : game.meta.title);
    title->setFontSize(tokenChain("card_title_font", "font_body", 17.f));
    title->setTextColor(theme.color("nxstation/title_text"));
    title->setSingleLine(true);
    title->setAnimated(false);
    cell->addView(title);

    if (!detail.empty()) {
        auto* detailLabel = new brls::Label();
        detailLabel->setText(detail);
        detailLabel->setFontSize(tokenChain("card_detail_font", "font_meta", 14.f));
        detailLabel->setTextColor(theme.color("nxstation/detail_text"));
        detailLabel->setSingleLine(true);
        detailLabel->setAnimated(false);
        cell->addView(detailLabel);
    }

    return cell;
}

brls::Box* DashboardHomeView::makePill(const std::string& systemId)
{
    auto& theme = ThemeManager::instance();
    const float height = token("pill_height", 44.f);
    const float radius = std::min(token("badge_radius", 14.f), height / 2.f);

    auto* pill = new DashboardCell(this, systemId, {}, true);
    pill->setAxis(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    const float pillPad = token("pill_padding", 14.f);
    pill->setPadding(0, pillPad, 0, pillPad);
    pill->setHeight(height);
    pill->setCornerRadius(radius);
    pill->setBackgroundColor(theme.color("nxstation/card_bg"));
    pill->setBorderColor(theme.color("nxstation/card_border"));
    pill->setIdleBorderColor(theme.color("nxstation/card_border"));
    pill->setFocusable(true);
    pill->setFocusSound(brls::SOUND_NONE);
    pill->setHideHighlight(true);

    const std::string icon = theme.systemIconPath(systemId);
    if (!icon.empty()) {
        auto* image = new brls::Image();
        image->setDimensions(token("pill_icon", 26.f), token("pill_icon", 26.f));
        image->setScalingType(brls::ImageScalingType::FIT);
        image->setImageFromFile(icon);
        pill->addView(image);

        auto* spacer = new brls::Box();
        spacer->setHeight(1.f);
        spacer->setMarginRight(token("pill_icon_gap", 8.f));
        pill->addView(spacer);
    }

    auto* label = new brls::Label();
    label->setText(browser::systemDisplayName(systemId));
    label->setFontSize(tokenChain("pill_font", "font_body", 17.f));
    label->setTextColor(theme.color("nxstation/favorite_text"));
    label->setSingleLine(true);
    label->setAnimated(false);
    pill->addView(label);

    focusTargets_[systemId] = pill;
    return pill;
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

void DashboardHomeView::buildHeroSection()
{
    auto& theme = ThemeManager::instance();
    const float heroSize = token("hero_size", 180.f);
    const float radius = token("card_radius", 12.f);
    const float pad = token("card_padding", 10.f);
    const float heroImageW = token("hero_image_width", heroSize - pad * 2);
    const float heroImageH = token("hero_image_height", heroSize - pad * 2);

    // Hero card (focusable — shows the focused game, like the reference layout).
    heroImage_ = new brls::Image();
    heroImage_->setDimensions(heroImageW, heroImageH);
    heroImage_->setScalingType(brls::ImageScalingType::FIT);
    heroImage_->setImageAlign(imageAlign("hero_image_align", brls::ImageAlignment::CENTER));

    auto& games = AppState::instance().gamesFor(systemIds_.empty() ? std::string() : systemIds_.front());

    const GameItem heroSeed = games.empty() ? GameItem{} : games.front();
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

    auto* middle = new brls::Box();
    middle->setAxis(brls::Axis::COLUMN);
    middle->setGrow(1.f);
    middle->setMarginLeft(token("hero_gap", 20.f));

    heroTitle_ = new brls::Label();
    heroTitle_->setFontSize(tokenChain("hero_title_font", "font_title", 28.f));
    heroTitle_->setTextColor(theme.color("nxstation/title_text"));
    heroTitle_->setSingleLine(true);
    heroTitle_->setAnimated(false);
    middle->addView(heroTitle_);

    heroMeta_ = new brls::Label();
    heroMeta_->setFontSize(tokenChain("hero_meta_font", "font_meta", 14.f));
    heroMeta_->setSingleLine(true);
    heroMeta_->setAnimated(false);
    middle->addView(heroMeta_);

    heroBadgeRow_ = new brls::Box();
    heroBadgeRow_->setAxis(brls::Axis::ROW);
    heroBadgeRow_->setPadding(token("hero_badge_pad", 10.f), 0, token("hero_badge_pad", 10.f), 0);
    middle->addView(heroBadgeRow_);

    heroDescription_ = new brls::Label();
    heroDescription_->setFontSize(tokenChain("hero_desc_font", "font_body", 17.f));
    heroDescription_->setTextColor(theme.color("nxstation/body_text"));
    heroDescription_->setSingleLine(false);
    heroDescription_->setLineHeight(1.4f);
    heroDescription_->setHeight(tokenChain("hero_desc_height", "description_height", 90.f));
    middle->addView(heroDescription_);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::FLEX_START);
    row->setPadding(0, 0, token("section_gap", 14.f), 0);
    row->addView(card);
    row->addView(middle);

    this->addView(makeSectionHeader("Focus", {}));
    this->addView(row);

    firstFocusable_ = card;
    focusTargets_[card->systemId()] = card;

    updateHero(games.empty() ? nullptr : &games.front());
}

void DashboardHomeView::buildGamesSection(const std::string& headerTitle,
                                          const std::vector<GameItem>& games)
{
    if (games.empty())
        return;

    auto* header = makeSectionHeader(headerTitle, std::to_string(games.size()));
    this->addView(header);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::FLEX_START);
    row->setPadding(0, 0, token("section_gap", 14.f), 0);

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

    this->addView(row);
}

void DashboardHomeView::buildPlatformsSection()
{
    std::vector<std::string> platforms;
    for (const auto& sys : Config::instance().systems())
        platforms.push_back(sys.id);
    if (platforms.empty())
        return;

    auto* header = makeSectionHeader("Platforms", std::to_string(platforms.size()));
    this->addView(header);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(0, 0, token("section_gap", 14.f), 0);

    for (const auto& id : platforms) {
        auto* pill = makePill(id);
        pill->setMarginRight(token("pill_gap", 10.f));
        if (!firstFocusable_)
            firstFocusable_ = pill;
        row->addView(pill);
    }

    this->addView(row);
}

void DashboardHomeView::buildMetadataSection()
{
    static const char* kKeys[] = {"Platform", "Genre", "Released", "Developer"};

    auto* header = makeSectionHeader("Details", {});
    this->addView(header);

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
        this->addView(row);
    }
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
        const std::string path = firstExistingPath(
            {game ? game->meta.boxArtPath : "", ThemeManager::instance().boxPlaceholderPath(),
             sf::gameArtPlaceholderPath()});
        if (!path.empty())
            heroImage_->setImageFromFile(path);
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

    buildTopBar();

    for (const auto& section : ThemeManager::instance().sections()) {
        if (section == "hero") {
            buildHeroSection();
        } else if (section == "continue") {
            buildGamesSection("Continue playing",
                              AppState::instance().gamesFor(kLastPlayedSystemId));
        } else if (section == "favourites") {
            buildGamesSection("Favourites",
                              AppState::instance().gamesFor(kFavoritesSystemId));
        } else if (section == "platforms") {
            buildPlatformsSection();
        } else if (section == "metadata") {
            buildMetadataSection();
        }
        // Unknown section names are ignored by design (forward-compatible theme keys).
    }

    auto& theme = ThemeManager::instance();
    auto* hints = new brls::Label();
    hints->setText("↑ ↓ Sections   ·   ← → Cards   ·   A Open   ·   Y Menu   ·   - Screensaver   ·   + Settings");
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
