#pragma once

#include "app/Models.hpp"

#include <borealis.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace sf::ui {

class DashboardHomeView;

/** Focusable card/pill cell. Reports focus to the view so the hero block tracks the
 *  focused game and the A action can open it. */
class DashboardCell : public brls::Box {
public:
    DashboardCell(DashboardHomeView* owner, std::string systemId, std::string romPath,
                  bool isSystemCell)
        : owner_(owner), systemId_(std::move(systemId)), romPath_(std::move(romPath)),
          isSystemCell_(isSystemCell)
    {
    }

    void setIdleBorderColor(NVGcolor color) { idleBorderColor_ = color; }

    void onFocusGained() override;
    void onFocusLost() override;

    const std::string& systemId() const { return systemId_; }
    const std::string& romPath() const { return romPath_; }
    bool isSystemCell() const { return isSystemCell_; }

private:
    DashboardHomeView* owner_;
    std::string systemId_;
    std::string romPath_;
    bool isSystemCell_;
    NVGcolor idleBorderColor_{0, 0, 0, 0};
};

/** RomMmate-style dashboard home, active when the theme declares layout="dashboard".
 *  Section order and visibility come from the theme's <sections> (theme.xml stays the only
 *  customization surface); appearance comes from its <style> tokens with fallback defaults. */
class DashboardHomeView : public brls::Box {
public:
    static brls::View* create();

    DashboardHomeView();

    void willAppear(bool resetState = false) override;
    void frame(brls::FrameContext* ctx) override;

    void onCellFocused(DashboardCell* cell);
    void openCell(DashboardCell* cell);

private:
    void rebuild();
    void registerActions();

    brls::Box* makeSectionHeader(const std::string& title, const std::string& count);
    brls::Box* makeCard(const GameItem& game, float width, float imageHeight,
                        const std::string& detail, bool fillHeight = false);
    brls::Box* makeTile(const std::string& systemId);
    brls::Box* makeBadge(const std::string& text);

    void buildTopBar();
    brls::Box* buildHeroSection();
    brls::Box* buildGamesSection(const std::string& headerTitle, const std::vector<GameItem>& games);
    brls::Box* buildPlatformsSection();
    brls::Box* buildRandomSection();
    brls::Box* buildMetadataSection();

    std::vector<GameItem> randomGames(size_t count);

    void updateHero(const GameItem* game);
    void setHeroBadge(const std::string& text);
    void setMetaRow(const std::string& key, const std::string& value);

    uint32_t themeGenApplied_ = 0;
    bool firstFocus_ = true;
    std::vector<std::string> systemIds_;

    brls::Box* firstFocusable_ = nullptr;
    std::unordered_map<std::string, brls::Box*> focusTargets_;

    brls::Image* heroImage_ = nullptr;
    brls::Label* heroTitle_ = nullptr;
    brls::Label* heroMeta_ = nullptr;
    brls::Label* heroDescription_ = nullptr;
    brls::Box* heroBadgeRow_ = nullptr;
    std::unordered_map<std::string, brls::Label*> metaLabels_;
    GameItem heroGame_;
};

} // namespace sf::ui
