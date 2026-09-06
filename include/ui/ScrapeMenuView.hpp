#pragma once

#include "scraper/ScrapeTypes.hpp"

#include <borealis.hpp>
#include <functional>
#include <string>

namespace sf::ui {

class FocusedMenuDialog;

using ScrapeMenuCallback = std::function<void(ScrapeMode mode)>;

class ScrapeMenuView : public brls::Box {
public:
    ScrapeMenuView(std::string systemId, ScrapeMenuCallback onSelect, bool libraryWide = false);
    ~ScrapeMenuView() override = default;

    /** Builds the menu wrapped in a Borealis dialog and opens it. */
    static void present(const std::string& systemId, ScrapeMenuCallback onSelect,
                        std::function<void()> onDismiss = nullptr);

    /** Library-wide scrape (all systems). Hides the single-game option. */
    static void presentLibrary(ScrapeMenuCallback onSelect,
                               std::function<void()> onDismiss = nullptr);

    /** Open the library scrape menu and start a batch if a mode is chosen. */
    static void presentLibraryAndStart(std::function<void()> onDismiss = nullptr);

private:
    std::string systemId_;
    ScrapeMenuCallback onSelect_;
    bool libraryWide_ = false;
    FocusedMenuDialog* dialog_ = nullptr;
};

} // namespace sf::ui
