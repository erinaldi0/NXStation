#pragma once

#include "app/Models.hpp"

#include <borealis.hpp>
#include <string>
#include <vector>

namespace sf::ui::browser {

void requestSystemsDataRefresh();
bool consumeSystemsDataRefresh();

void rebuildSystemIds(std::vector<std::string>& out);
size_t initialSystemIndex(const std::vector<std::string>& systemIds);
std::string resolveSystemBackground(const std::string& systemId);
void applySystemPlaceholder(brls::Image* image);
std::string resolveSystemListArt(const std::string& systemId);
void applySystemListArt(brls::Image* image, const std::string& systemId);
std::string systemDescription(const std::string& systemId);
std::string systemDisplayName(const std::string& systemId);
std::string systemGameCountLabel(const std::string& systemId);

/** True when a dialog or pushed screen sits above the systems/game host. */
bool overlayBlocksHostInput();

/** All scanned library games (real systems only) for a library-wide scrape. */
std::vector<sf::GameItem> collectAllLibraryGames();

} // namespace sf::ui::browser
