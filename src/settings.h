#pragma once

#include <functional>
#include <string>

namespace dps {

struct Hotkey {
    int vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::string text;  // as written in settings.json

    bool Valid() const { return vk != 0; }
};

Hotkey ParseHotkey(const std::string& text);

/// User preferences, persisted as nativePC/plugins/MhwDpsMeter/settings.json.
struct Settings {
    bool overlayVisible = true;
    bool showMonsterHealth = true;
    float overlayOpacity = 0.92f;
    int trialDurationSeconds = 60;
    /// Overlay text size in pixels; 0 picks one from the window height.
    float fontSize = 0.0f;
    bool writeFightLogs = true;
    std::string logLevel = "info";

    Hotkey keyMenu = ParseHotkey("F9");
    Hotkey keyOverlay = ParseHotkey("F10");
    Hotkey keyReset = ParseHotkey("F7");
    Hotkey keyTrial = ParseHotkey("F8");
    Hotkey keyDiagnostics = ParseHotkey("F6");

    // Optional hooks; each can be turned off if it misbehaves on a given setup.
    bool hookPartContext = true;
    bool hookActions = true;
    bool hookMainUpdate = true;
    bool questNames = true;

    /// Extra fonts (absolute Windows paths) tried before the built-in search list.
    std::string fontPath;
    std::string cjkFontPath;
};

namespace settings {

void Load();
Settings Get();
/// Applies fn to the current settings and saves them.
void Update(const std::function<void(Settings&)>& fn);
std::wstring FilePath();

}  // namespace settings

}  // namespace dps
