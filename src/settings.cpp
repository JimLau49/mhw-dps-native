#include "settings.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "common.h"
#include "log.h"

namespace dps {
namespace {

Lock g_lock;
Settings g_settings;
bool g_loaded = false;

struct KeyName {
    const char* name;
    int vk;
};

const KeyName kKeyNames[] = {
    {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE},     {"HOME", VK_HOME},     {"END", VK_END},
    {"PAGEUP", VK_PRIOR},    {"PAGEDOWN", VK_NEXT},     {"PAUSE", VK_PAUSE},   {"SCROLLLOCK", VK_SCROLL},
    {"TAB", VK_TAB},         {"BACKSPACE", VK_BACK},    {"ENTER", VK_RETURN},  {"SPACE", VK_SPACE},
    {"ESCAPE", VK_ESCAPE},   {"UP", VK_UP},             {"DOWN", VK_DOWN},     {"LEFT", VK_LEFT},
    {"RIGHT", VK_RIGHT},     {"GRAVE", VK_OEM_3},       {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS},
    {"LBRACKET", VK_OEM_4},  {"RBRACKET", VK_OEM_6},    {"BACKSLASH", VK_OEM_5}, {"SEMICOLON", VK_OEM_1},
    {"QUOTE", VK_OEM_7},     {"COMMA", VK_OEM_COMMA},   {"PERIOD", VK_OEM_PERIOD}, {"SLASH", VK_OEM_2},
};

std::string Upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

int ParseKeyName(const std::string& name) {
    if (name.empty())
        return 0;
    if (name.size() > 2 && name[0] == '0' && name[1] == 'X')
        return static_cast<int>(std::strtol(name.c_str() + 2, nullptr, 16)) & 0xFF;
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    if (name[0] == 'F' && name.size() <= 3) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    if (StartsWith(name, "NUMPAD") && name.size() == 7 && name[6] >= '0' && name[6] <= '9')
        return VK_NUMPAD0 + (name[6] - '0');
    for (const auto& key : kKeyNames) {
        if (name == key.name)
            return key.vk;
    }
    return 0;
}

std::wstring SettingsPath() { return JoinPath(DataDirectory(), L"settings.json"); }

Hotkey HotkeyOr(const nlohmann::json& keys, const char* name, const Hotkey& fallback) {
    if (!keys.is_object() || !keys.contains(name) || !keys[name].is_string())
        return fallback;
    const Hotkey parsed = ParseHotkey(keys[name].get<std::string>());
    if (!parsed.Valid() && !keys[name].get<std::string>().empty()) {
        LOG_WARN("settings: unknown key \"%s\" for %s, keeping %s", keys[name].get<std::string>().c_str(), name,
                 fallback.text.c_str());
        return fallback;
    }
    return parsed;
}

template <typename T>
void ReadValue(const nlohmann::json& json, const char* name, T& value) {
    try {
        if (json.contains(name) && !json[name].is_null())
            value = json[name].get<T>();
    } catch (...) {
        LOG_WARN("settings: ignoring invalid value for %s", name);
    }
}

void SaveLocked() {
    nlohmann::json json;
    const Settings& s = g_settings;
    json["overlayVisible"] = s.overlayVisible;
    json["showMonsterHealth"] = s.showMonsterHealth;
    json["overlayOpacity"] = s.overlayOpacity;
    json["trialDurationSeconds"] = s.trialDurationSeconds;
    json["fontSize"] = s.fontSize;
    json["writeFightLogs"] = s.writeFightLogs;
    json["logLevel"] = s.logLevel;
    json["fontPath"] = s.fontPath;
    json["cjkFontPath"] = s.cjkFontPath;
    json["keys"] = {
        {"menu", s.keyMenu.text},   {"overlay", s.keyOverlay.text},         {"reset", s.keyReset.text},
        {"trial", s.keyTrial.text}, {"diagnostics", s.keyDiagnostics.text},
    };
    json["hooks"] = {
        {"partContext", s.hookPartContext},
        {"actions", s.hookActions},
        {"mainUpdate", s.hookMainUpdate},
        {"questNames", s.questNames},
    };

    EnsureDirectory(DataDirectory());
    if (!WriteFileAtomic(SettingsPath(), json.dump(2) + "\n"))
        LOG_WARN("could not save settings.json");
}

}  // namespace

Hotkey ParseHotkey(const std::string& text) {
    Hotkey hotkey;
    hotkey.text = text;
    std::string rest = Upper(text);
    rest.erase(std::remove(rest.begin(), rest.end(), ' '), rest.end());
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus + 1 >= rest.size())
            break;
        const std::string modifier = rest.substr(0, plus);
        if (modifier == "CTRL" || modifier == "CONTROL")
            hotkey.ctrl = true;
        else if (modifier == "SHIFT")
            hotkey.shift = true;
        else if (modifier == "ALT")
            hotkey.alt = true;
        else
            break;
        rest = rest.substr(plus + 1);
    }
    hotkey.vk = ParseKeyName(rest);
    return hotkey;
}

namespace settings {

std::wstring FilePath() { return SettingsPath(); }

void Load() {
    LockGuard guard(g_lock);
    g_loaded = true;
    std::string text;
    if (!ReadWholeFile(SettingsPath(), text)) {
        SaveLocked();  // write the defaults so they are easy to find and edit
        return;
    }

    nlohmann::json json = nlohmann::json::parse(text, nullptr, false, true);
    if (!json.is_object()) {
        LOG_WARN("settings.json is not valid JSON; using defaults");
        return;
    }

    Settings& s = g_settings;
    ReadValue(json, "overlayVisible", s.overlayVisible);
    ReadValue(json, "showMonsterHealth", s.showMonsterHealth);
    ReadValue(json, "overlayOpacity", s.overlayOpacity);
    ReadValue(json, "trialDurationSeconds", s.trialDurationSeconds);
    ReadValue(json, "fontSize", s.fontSize);
    ReadValue(json, "writeFightLogs", s.writeFightLogs);
    ReadValue(json, "logLevel", s.logLevel);
    ReadValue(json, "fontPath", s.fontPath);
    ReadValue(json, "cjkFontPath", s.cjkFontPath);
    if (json.contains("keys")) {
        const auto& keys = json["keys"];
        s.keyMenu = HotkeyOr(keys, "menu", s.keyMenu);
        s.keyOverlay = HotkeyOr(keys, "overlay", s.keyOverlay);
        s.keyReset = HotkeyOr(keys, "reset", s.keyReset);
        s.keyTrial = HotkeyOr(keys, "trial", s.keyTrial);
        s.keyDiagnostics = HotkeyOr(keys, "diagnostics", s.keyDiagnostics);
    }
    if (json.contains("hooks") && json["hooks"].is_object()) {
        const auto& hooks = json["hooks"];
        ReadValue(hooks, "partContext", s.hookPartContext);
        ReadValue(hooks, "actions", s.hookActions);
        ReadValue(hooks, "mainUpdate", s.hookMainUpdate);
        ReadValue(hooks, "questNames", s.questNames);
    }

    s.overlayOpacity = std::clamp(s.overlayOpacity, 0.25f, 1.0f);
    s.trialDurationSeconds = std::clamp(s.trialDurationSeconds, 5, 600);
    s.fontSize = s.fontSize <= 0.0f ? 0.0f : std::clamp(s.fontSize, 8.0f, 64.0f);
}

Settings Get() {
    LockGuard guard(g_lock);
    return g_settings;
}

void Update(const std::function<void(Settings&)>& fn) {
    LockGuard guard(g_lock);
    fn(g_settings);
    g_settings.overlayOpacity = std::clamp(g_settings.overlayOpacity, 0.25f, 1.0f);
    g_settings.trialDurationSeconds = std::clamp(g_settings.trialDurationSeconds, 5, 600);
    SaveLocked();
}

}  // namespace settings

}  // namespace dps
