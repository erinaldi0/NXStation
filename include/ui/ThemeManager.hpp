#pragma once

#include <nanovg.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace sf::ui {

/** Loads color themes from data/theme/{name}/theme.xml and optional assets. */
class ThemeManager {
public:
    static ThemeManager& instance();

    // --- Schema v2: <style> tokens, <sections>, layout ---

    /** Theme layout: "carousel" (default) or "dashboard". */
    const std::string& layout() const { return layout_; }
    bool isDashboardLayout() const { return layout_ == "dashboard"; }
    int schemaVersion() const { return schema_; }

    /** Declared dashboard sections in order; empty when the XML omits <sections>. */
    const std::vector<std::string>& sections() const { return sections_; }
    bool hasSection(const std::string& name) const;

    /** A <style> entry value, or fallback when missing. Generic key/value — unknown
     *  keys are preserved so future features can honor them without format changes. */
    std::string style(const std::string& key, const std::string& fallback = "") const;
    float styleNum(const std::string& key, float fallback) const;
    /** Style value when it is a hex color, otherwise the theme color for fallbackKey. */
    NVGcolor styleColor(const std::string& key, const std::string& fallbackKey) const;
    bool hasStyle(const std::string& key) const;

    /** Theme asset folder icons: `icons/{systemId}.png` (jpg/webp too). */
    std::string systemIconPath(const std::string& systemId) const;
    /** Optional `header_logo.png` at the theme root. */
    std::string headerLogoPath() const;

    /** Install bundled themes, load user selection, apply Borealis colors. */
    void initialize(const std::string& themeName);

    void setTheme(const std::string& name);
    const std::string& currentTheme() const { return currentTheme_; }
    bool isLightTheme() const { return variant_ == "light"; }

    std::vector<std::string> availableThemes() const;

    /** e.g. "Abyss  ◆ Dark" or "Horizon  ◇ Light" */
    std::string displayLabel(const std::string& folderName) const;
    std::string currentDisplayLabel() const { return displayLabel(currentTheme_); }

    /** Resolve a brls/* or nxstation/* color key from the active theme. */
    NVGcolor color(const std::string& key) const;
    bool hasColor(const std::string& key) const;

    /** Text color on a focused row — respects nxstation/selected_text or highlight contrast. */
    NVGcolor selectionTextColor(NVGcolor idleText) const;

    /** Migrate legacy folder names (blue → Abyss, etc.). */
    static std::string migrateThemeName(const std::string& name);

    std::string themePath() const;
    std::string systemBackgroundPath(const std::string& systemId) const;
    /** Theme file `{systemId}-list.jpg` (also .png/.webp) for the list-style preview card. */
    std::string systemListArtPath(const std::string& systemId) const;
    std::string systemPlaceholderPath() const;
    std::string boxPlaceholderPath() const;

    /** Optional theme assets (empty when missing). */
    std::string backgroundMusicPath() const;
    std::string screensaverImagePath() const;
    std::string sfxPath(const std::string& baseName) const;

    bool themeForcesListBrowser() const { return browserStyle_ == "list"; }
    bool themeForcesDashboard() const { return layout_ == "dashboard"; }

    void applyColors();
    /** Incremented when colors change; views repaint when generation differs. */
    void markUiRefresh();
    uint32_t uiGeneration() const { return uiGeneration_; }

private:
    ThemeManager() = default;
    void ensureBundledThemes();
    void seedThemeAudio(const std::string& name);
    bool loadThemeXml(const std::string& name);
    static bool copyFileIfMissing(const std::string& src, const std::string& dst);
    std::string findThemeAsset(const std::vector<const char*>& fileNames,
                               const std::vector<const char*>& subdirs) const;
    void applyThemeAssets();

    std::string currentTheme_ = "Vampire";
    std::string variant_ = "dark";
    std::string browserStyle_ = "carousel";
    std::string layout_ = "carousel";
    int schema_ = 1;
    std::vector<std::string> sections_;
    std::unordered_map<std::string, std::string> style_;
    std::string themeRoot_;
    uint32_t uiGeneration_ = 0;
    std::vector<std::pair<std::string, std::string>> colors_;
};

} // namespace sf::ui
