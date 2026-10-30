#pragma once

#include "app/Models.hpp"

#include <borealis.hpp>
#include <algorithm>
#include <functional>
#include <memory>
#include <random>
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

/** Cover used by every dashboard section (Focus, Continue playing, Favourites, Random
 *  picks), so they all behave the same way: the node is the baseline box (3:4 by default,
 *  height pinned by the theme) and the loaded image is fitted inside it with an object-fit
 *  rule (fit = contain, cover, fill, none). The box is what the wrapping card wraps, and
 *  the texture is only known once the asynchronous load completes, so the bounds are
 *  recomputed on the next frame and the card is notified through onSized(). */
class DashboardCover : public brls::Image {
public:
    /* maxWidth is an optional cap for the box (card_width); 0 leaves the box at the
     * baseline shape. */
    DashboardCover(float height, float aspect, float radius, brls::ImageAlignment align,
                   brls::ImageScalingType fit, float maxWidth = 0.f);

    /** Loads the game art (or the theme placeholder) and sizes the cover from it. */
    void setGame(const GameItem& game);

    /** Notified after every sizing, including the one that follows an asynchronous load. */
    void onSized(std::function<void(DashboardCover*)> callback) { onSized_.push_back(std::move(callback)); }

    float naturalWidth() const { return naturalWidth_; }
    float naturalHeight() const { return naturalHeight_; }

private:
    void applySize();

    float height_;
    float aspect_;
    brls::ImageScalingType fit_;
    float maxWidth_;
    float naturalWidth_ = 0.f;
    float naturalHeight_ = 0.f;
    int retries_ = 0;
    std::vector<std::function<void(DashboardCover*)>> onSized_;
};

/** Row of cards that share one height: every card wraps its own cover, so only the width
 *  changes between games. The height is the tallest natural card in the row, recomputed
 *  when a cover is sized after an asynchronous texture load. */
struct CardRow {
    struct Entry {
        DashboardCover* cover;
        brls::Box* cell;
        float labelsHeight;
    };

    float padding = 0.f;
    std::vector<Entry> entries;

    void add(DashboardCover* cover, brls::Box* cell, float labelsHeight)
    {
        entries.push_back({cover, cell, labelsHeight});
    }

    float tallest() const
    {
        float tallest = 0.f;
        for (const auto& entry : entries)
            tallest = std::max(tallest, padding * 2 + entry.cover->naturalHeight() + entry.labelsHeight);
        return tallest;
    }

    /** Optional hook, for instance to keep a scrolling frame tall enough for the row. */
    std::function<void(float)> onTallest;

    void apply()
    {
        const float height = tallest();
        for (const auto& entry : entries)
            entry.cell->setHeight(height);
        if (onTallest)
            onTallest(height);
    }
};

/** Label that truly wraps. Borealis only measures and wraps a label when its width is
 *  constrained and its height is left free, so callers must set the width here (never the
 *  height): with both dimensions fixed Yoga skips the measure function and the label draws a
 *  single truncated line. setMaxLines() caps the visible lines and ellipsizes the overflow,
 *  like the RomM platform cards. */
class MultilineLabel : public brls::Label {
public:
    void setMaxLines(int lines);

    void onLayout() override;

private:
    int maxLines_ = 0;
    bool trimming_ = false;
    std::string measuredText_;
};

/** RomMmate-style dashboard home, active when the theme declares layout="dashboard".
 *  Section order and visibility come from the theme's <sections> (theme.xml stays the only
 *  customization surface); appearance comes from its <style> tokens with fallback defaults. */
class DashboardHomeView : public brls::Box {
public:
    static brls::View* create();

    DashboardHomeView();
    ~DashboardHomeView() override;

    void willAppear(bool resetState = false) override;
    void frame(brls::FrameContext* ctx) override;

    void onCellFocused(DashboardCell* cell);
    void openCell(DashboardCell* cell);

private:
    void rebuild();
    void registerActions();

    brls::Box* makeSectionHeader(const std::string& title, const std::string& count);
    brls::Box* makeCard(const GameItem& game, float imageHeight, float maxWidth,
                        const std::string& detail, const std::shared_ptr<CardRow>& row);
    brls::Box* makeTile(const std::string& systemId);
    brls::Box* makeBadge(const std::string& text);

    void buildTopBar();
    brls::Box* buildHeroSection(float columnWidth);
    brls::Box* buildGamesSection(const std::string& headerTitle, const std::vector<GameItem>& games);
    brls::Box* buildPlatformsSection();
    brls::Box* buildRandomSection();
    brls::Box* buildMetadataSection();

    std::vector<GameItem> randomGames(size_t count);
    const GameItem* pickRandomGame(const std::string& systemId);

    void updateHero(const GameItem* game);
    void setMetaBadge(const std::string& text);
    void setMetaRow(const std::string& key, const std::string& value);

    uint32_t themeGenApplied_ = 0;
    bool firstFocus_ = true;
    std::vector<std::string> systemIds_;

    brls::Box* firstFocusable_ = nullptr;
    std::unordered_map<std::string, brls::Box*> focusTargets_;

    DashboardCover* heroCover_ = nullptr;
    brls::Label* heroTitle_ = nullptr;
    brls::Label* heroMeta_ = nullptr;
    brls::Label* heroDescription_ = nullptr;
    brls::Box* metaBadgeRow_ = nullptr;  // badges moved to Details section
    std::unordered_map<std::string, brls::Label*> metaLabels_;
    GameItem heroGame_;

    /** Background art job: the top bar shows it only while a job is running. The view can be rebuilt,
     *  so the listener id is replaced and dropped in the destructor. */
    brls::Label* artIndicator_ = nullptr;
    int artListenerId_ = -1;
    std::shared_ptr<bool> artAlive_ = std::make_shared<bool>(true);    std::mt19937 rng_;
};

} // namespace sf::ui
