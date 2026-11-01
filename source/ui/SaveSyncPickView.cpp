#include "ui/SaveSyncPickView.hpp"
#include "ui/PushedActivity.hpp"
#include "ui/ThemeManager.hpp"
#include "ui/UiSfx.hpp"
#include "app/Config.hpp"
#include "romm/RomMService.hpp"
#include "util/ActionLog.hpp"

#include <algorithm>
#include <sstream>
#include <borealis/views/cells/cell_detail.hpp>

namespace sf::ui {

namespace {

std::string formatSize(int64_t bytes)
{
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    return out.str();
}

const char* actionGlyph(const sf::romm::SyncCandidate& c)
{
    switch (c.action) {
        case sf::romm::SyncCandidate::Action::Upload: return "↑";
        case sf::romm::SyncCandidate::Action::Download: return "↓";
        case sf::romm::SyncCandidate::Action::Conflict: return "⚠";
        case sf::romm::SyncCandidate::Action::Delete: return "✕";
    }
    return "·";
}

std::string resolutionLabel(sf::romm::SyncCandidate::Resolution r)
{
    using R = sf::romm::SyncCandidate::Resolution;
    switch (r) {
        case R::KeepBoth: return "keep both";
        case R::KeepLocal: return "keep local";
        case R::KeepRemote: return "keep remote";
        case R::Acknowledge: return "acknowledge";
    }
    return {};
}

std::string freshnessNote(const sf::romm::SyncCandidate& c)
{
    if (!c.reason.empty())
        return c.reason;
    using A = sf::romm::SyncCandidate::Action;
    switch (c.action) {
        case A::Upload: return "local newer";
        case A::Download: return "remote newer";
        case A::Conflict: return "both changed";
        case A::Delete: return "server deleted";
    }
    return {};
}

/** Cycle order for the per-row resolution; delete rows only offer the two archive choices. */
sf::romm::SyncCandidate::Resolution nextResolution(sf::romm::SyncCandidate::Resolution r,
                                                    sf::romm::SyncCandidate::Action action)
{
    using R = sf::romm::SyncCandidate::Resolution;
    if (action == sf::romm::SyncCandidate::Action::Delete) {
        return r == R::KeepBoth ? R::KeepLocal : R::KeepBoth;
    }
    switch (r) {
        case R::KeepBoth: return R::KeepLocal;
        case R::KeepLocal: return R::KeepRemote;
        case R::KeepRemote: return R::Acknowledge;
        case R::Acknowledge: return R::KeepBoth;
    }
    return r;
}

} // namespace

SaveSyncPickView::SaveSyncPickView(std::string systemId, std::string romPath, std::string coreId,
                                   std::string gameName)
    : systemId_(std::move(systemId))
    , romPath_(std::move(romPath))
    , coreId_(std::move(coreId))
    , gameName_(std::move(gameName))
{
    auto& theme = ThemeManager::instance();
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.f);
    this->setBackgroundColor(theme.color("nxstation/overlay_bg"));
    this->setPadding(28, 36, 36, 36);
    this->setFocusable(false);
    this->setHideHighlightBackground(true);
    this->setHideHighlightBorder(true);

    titleLabel_ = new brls::Label();
    titleLabel_->setText(gameName_.empty() ? "Sync Save Files" : gameName_);
    titleLabel_->setFontSize(30);
    titleLabel_->setTextColor(theme.color("brls/text"));
    titleLabel_->setFocusable(false);
    titleLabel_->setMarginBottom(14);
    this->addView(titleLabel_);

    auto* hint = new brls::Label();
    hint->setFontSize(16);
    hint->setSingleLine(false);
    hint->setLineHeight(1.3f);
    hint->setTextColor(theme.color("nxstation/detail_text"));
    hint->setText("A selects a file, X cycles its conflict choice. Deselected files are skipped "
                  "this session, so the next negotiation proposes them again.");
    // Informational labels must not take focus: the highlight ring around a focused label
    // reaches above it and covers the text on the row before (the repo convention — see
    // GameDetailView). The extra vertical margin keeps the ring clear even where a label
    // does get focus.
    hint->setFocusable(false);
    hint->setMarginBottom(24);
    this->addView(hint);

    statusLabel_ = new brls::Label();
    statusLabel_->setFontSize(20);
    statusLabel_->setTextColor(theme.color("nxstation/log_progress"));
    statusLabel_->setText("Preparing sync…");
    statusLabel_->setFocusable(false);
    statusLabel_->setMarginTop(18);
    statusLabel_->setMarginBottom(24);
    this->addView(statusLabel_);

    listScroller_ = new brls::ScrollingFrame();
    listScroller_->setGrow(1.f);
    listScroller_->setScrollingIndicatorVisible(false);
    listScroller_->setClipsToBounds(true);
    // Headroom for the focus ring: the highlight around the first focused row is drawn
    // slightly outside the cell, so the list must start lower or the ring lands on the
    // status line above it.
    listScroller_->setMarginTop(16);

    listBox_ = new brls::Box();
    listBox_->setAxis(brls::Axis::COLUMN);
    listBox_->setGrow(1.f);
    listBox_->setMarginTop(12);  // headroom so the first row's focus ring isn't clipped at the scroller top
    listScroller_->setContentView(listBox_);
    this->addView(listScroller_);

    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setMarginTop(12);

    auto addFooterButton = [&](const char* label, std::function<void()> action) {
        auto* button = new brls::Button();
        button->setText(label);
        button->setMarginRight(14);
        button->registerClickAction([action](brls::View*) {
            if (action)
                action();
            return true;
        });
        footer->addView(button);
    };

    addFooterButton("Select all", [this] {
        if (applying_ || !*alive_)
            return;
        for (size_t i = 0; i < candidates_.size(); ++i) {
            candidates_[i].selected = true;
            refreshRow(i);
        }
        refreshCounts();
    });

    addFooterButton("Select none", [this] {
        if (applying_ || !*alive_)
            return;
        for (size_t i = 0; i < candidates_.size(); ++i) {
            candidates_[i].selected = false;
            refreshRow(i);
        }
        refreshCounts();
    });

    applyButton_ = new brls::Button();
    applyButton_->setText("Apply sync");
    applyButton_->setState(brls::ButtonState::DISABLED);
    applyButton_->registerClickAction([this](brls::View*) {
        if (applying_ || !*alive_)
            return true;
        applySelection();
        return true;
    });
    footer->addView(applyButton_);

    this->addView(footer);

    this->registerAction(
        "Page Up", brls::ControllerButton::BUTTON_LB, [this](brls::View*) {
            playNavSfx();
            const float page = listScroller_->getHeight() * 0.85f;
            listScroller_->setContentOffsetY(std::max(0.f, listScroller_->getContentOffsetY() - page), true);
            return true;
        });
    this->registerAction(
        "Page Down", brls::ControllerButton::BUTTON_RB, [this](brls::View*) {
            playNavSfx();
            const float page = listScroller_->getHeight() * 0.85f;
            listScroller_->setContentOffsetY(listScroller_->getContentOffsetY() + page, true);
            return true;
        });
}

SaveSyncPickView::~SaveSyncPickView()
{
    *alive_ = false;
    abort_->store(true);
}

void SaveSyncPickView::present()
{
    SF_LOG_ACTION("SaveSyncPickView");
    PushedActivity::push(new SaveSyncPickView("", "", ""));
}

void SaveSyncPickView::presentForGame(const std::string& systemId, const sf::GameItem& game)
{
    SF_LOG_ACTION("SaveSyncPickView");
    const SystemConfig* sys = Config::instance().findSystem(systemId);
    PushedActivity::push(new SaveSyncPickView(systemId, game.path, sys ? sys->core : "", game.displayName));
}

void SaveSyncPickView::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);
    *alive_ = true;
    if (!prepared_) {
        prepared_ = true;
        prepareAndPopulate();
    }
}

void SaveSyncPickView::willDisappear(bool resetState)
{
    *alive_ = false;
    abort_->store(true);  // stop the worker mid-apply; already-applied files keep their results
    brls::Box::willDisappear(resetState);
}

void SaveSyncPickView::prepareAndPopulate()
{
    auto& theme = ThemeManager::instance();
    auto alive = alive_;

    auto& romm = sf::romm::RomMService::instance();
    auto done = [this, alive, &theme](bool ok, std::vector<sf::romm::SyncCandidate> candidates,
                                      const std::string& error) {
        if (!*alive)
            return;
        if (!ok) {
            statusLabel_->setText(error.empty() ? "No operations to pick from" : error);
            statusLabel_->setTextColor(theme.color("nxstation/log_failure"));
            applyButton_->setState(brls::ButtonState::DISABLED);
            return;
        }
        candidates_ = std::move(candidates);
        buildList();
        refreshCounts();
    };

    if (romPath_.empty())
        romm.prepareAllCandidatesAsync(done);
    else
        romm.prepareCandidatesAsync(romPath_, systemId_, coreId_, done);
}

void SaveSyncPickView::buildList()
{
    auto& theme = ThemeManager::instance();
    brls::Application::giveFocus(listScroller_);
    listBox_->clearViews();
    cells_.assign(candidates_.size(), nullptr);

    if (candidates_.empty()) {
        statusLabel_->setText("Nothing to sync — no operations from the negotiation");
        return;
    }

    auto addGroupHeader = [&](const std::string& text, float fontSize, const std::string& color) {
        auto* header = new brls::Label();
        header->setText(text);
        header->setFontSize(fontSize);
        header->setTextColor(theme.color(color));
        header->setFocusable(false);  // informational: focus must stay on the rows
        header->setMarginTop(12);
        header->setMarginBottom(8);
        listBox_->addView(header);
    };

    size_t index = 0;
    std::string lastGame, lastType, lastSlot;
    for (auto& c : candidates_) {
        // Group: game (only when picking over all games) → saves/states → slot → device row.
        if (gameName_.empty() && c.gameName != lastGame) {
            addGroupHeader(c.gameName, 22, "nxstation/title_text");
            lastGame = c.gameName;
            lastType.clear();
            lastSlot.clear();
        }
        if (c.type != lastType) {
            addGroupHeader(c.type == "state" ? "States" : "Saves", 18, "nxstation/detail_text");
            lastType = c.type;
            lastSlot.clear();
        }
        const std::string slotLabel = c.slot.empty() ? "no slot" : "slot " + c.slot;
        if (slotLabel != lastSlot) {
            addGroupHeader(slotLabel, 16, "nxstation/muted_text");
            lastSlot = slotLabel;
        }

        const size_t i = index++;
        auto* row = new brls::DetailCell();
        row->setHeight(54);
        // Vertical room for the focus ring: a focused row's highlight is drawn slightly
        // outside the cell, so rows need a little margin or the ring crowds the neighbors.
        row->setMarginTop(6);
        row->setMarginBottom(6);
        row->setFocusable(true);
        cells_[i] = row;

        row->registerClickAction([this, i](brls::View*) {
            toggleSelected(i);
            return true;
        });
        if (c.canResolve()) {
            row->registerAction("Resolve", brls::ControllerButton::BUTTON_X, [this, i](brls::View*) {
                cycleResolution(i);
                return true;
            });
        }

        refreshRow(i);
        listBox_->addView(row);
    }

    // Focus the first row so A/X work straight away.
    const auto& children = listBox_->getChildren();
    if (!children.empty())
        brls::Application::giveFocus(children.front());
}

void SaveSyncPickView::refreshRow(size_t index)
{
    if (index >= candidates_.size() || !cells_[index])
        return;
    const auto& c = candidates_[index];
    auto& theme = ThemeManager::instance();
    auto* row = cells_[index];

    row->setText(std::string(c.selected ? "[x] " : "[ ] ") + actionGlyph(c) + " " +
                 (c.fileName.empty() ? c.deviceName : c.fileName));

    std::string detail = c.deviceName + " · " + formatSize(c.localExists ? c.localSize : c.remoteSize) +
                         " · " + freshnessNote(c);
    if (c.canResolve())
        detail += " · " + resolutionLabel(c.resolution);

    if (!c.result.empty()) {
        // Applied: color the row by outcome so the user sees what actually happened.
        detail += " — " + c.result;
        row->setDetailTextColor(theme.color(c.success ? "success" : "error"));
    } else {
        row->setDetailTextColor(theme.color("nxstation/detail_text"));
    }
    row->setDetailText(detail);
}

void SaveSyncPickView::refreshCounts()
{
    const size_t selected = std::count_if(candidates_.begin(), candidates_.end(),
                                          [](const sf::romm::SyncCandidate& c) { return c.selected; });
    applyButton_->setText("Apply sync (" + std::to_string(selected) + ")");
    applyButton_->setState(selected > 0 ? brls::ButtonState::ENABLED : brls::ButtonState::DISABLED);
    if (applying_)
        return;
    statusLabel_->setText(std::to_string(selected) + " of " + std::to_string(candidates_.size()) +
                          " file(s) selected");
}

void SaveSyncPickView::toggleSelected(size_t index)
{
    if (applying_ || !*alive_ || index >= candidates_.size())
        return;
    playToggleSfx();
    candidates_[index].selected = !candidates_[index].selected;
    refreshRow(index);
    refreshCounts();
}

void SaveSyncPickView::cycleResolution(size_t index)
{
    if (applying_ || !*alive_ || index >= candidates_.size())
        return;
    auto& c = candidates_[index];
    if (!c.canResolve())
        return;
    playToggleSfx();
    c.resolution = nextResolution(c.resolution, c.action);
    refreshRow(index);
}

void SaveSyncPickView::applySelection()
{
    if (applying_ || !*alive_)
        return;
    const size_t selected = std::count_if(candidates_.begin(), candidates_.end(),
                                          [](const sf::romm::SyncCandidate& c) { return c.selected; });
    if (selected == 0) {
        // An empty selection does nothing — the point of the picker is that deselected files
        // never reach the server, so no session churn either.
        brls::Application::notify("Nothing selected");
        return;
    }

    applying_ = true;
    statusLabel_->setText("Applying sync…");
    auto& theme = ThemeManager::instance();
    auto alive = alive_;

    auto& romm = sf::romm::RomMService::instance();
    romm.applyCandidatesAsync(
        candidates_,
        [this](const std::string& message, int percent) {
            if (!*alive_)
                return;
            statusLabel_->setText(message + (percent >= 0 ? " " + std::to_string(percent) + "%" : ""));
        },
        abort_.get(),
        [this, alive, &theme](bool ok, const std::string& message, std::vector<sf::romm::SyncCandidate> results) {
            if (!*alive)
                return;
            applying_ = false;
            candidates_ = std::move(results);
            for (size_t i = 0; i < candidates_.size(); ++i)
                refreshRow(i);
            statusLabel_->setTextColor(theme.color(ok ? "success" : "nxstation/log_failure"));
            statusLabel_->setText(message);
            refreshCounts();
        });
}

} // namespace sf::ui
