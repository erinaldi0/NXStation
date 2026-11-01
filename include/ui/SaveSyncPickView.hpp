#pragma once

#include "app/Models.hpp"
#include "romm/RomMSyncCandidate.hpp"

#include <atomic>
#include <borealis.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sf::ui {

/**
 * Per-file save sync picker. Rows are grouped game → saves/states → slot → device;
 * clicking a row (A) toggles its selection, X cycles the per-file resolution on conflict
 * and delete rows. Selection is per session — a deselected file is never sent to the
 * server, so the next negotiation proposes it again.
 */
class SaveSyncPickView : public brls::Box {
public:
    /** Pick over every mapped, locally downloaded ROM. */
    static void present();

    /** Pick scoped to one game. */
    static void presentForGame(const std::string& systemId, const sf::GameItem& game);

    explicit SaveSyncPickView(std::string systemId, std::string romPath, std::string coreId,
                              std::string gameName = {});
    ~SaveSyncPickView() override;

    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    void prepareAndPopulate();
    void buildList();
    void refreshCounts();
    void refreshRow(size_t index);
    void toggleSelected(size_t index);
    void cycleResolution(size_t index);
    void applySelection();

    /** Empty romPath means "all mapped ROMs"; empty gameName means the generic title. */
    std::string systemId_, romPath_, coreId_, gameName_;

    std::vector<sf::romm::SyncCandidate> candidates_;
    std::vector<brls::DetailCell*> cells_;  // parallel to candidates_

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);
    std::shared_ptr<std::atomic<bool>> abort_ = std::make_shared<std::atomic<bool>>(false);
    bool prepared_ = false;
    bool applying_ = false;

    brls::Label* titleLabel_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    brls::ScrollingFrame* listScroller_ = nullptr;
    brls::Box* listBox_ = nullptr;
    brls::Button* applyButton_ = nullptr;
};

} // namespace sf::ui
