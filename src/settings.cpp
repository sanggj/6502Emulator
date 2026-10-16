#include "settings.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// Portable INI storage: a tiny reader/writer used on every OS so the same
// config file works on Windows, Linux and macOS. Only the [config] section is
// managed; any other content in the file is preserved verbatim.
namespace {

constexpr char kSection[] = "config";
constexpr const char *kSectionHeader = "[config]";

std::string trim(const std::string &s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string sectionOf(const std::string &line) {
    const std::string t = trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') return t.substr(1, t.size() - 2);
    return {};
}

// Parse the whole file into key -> value for the requested section only.
std::map<std::string, std::string> readKeys(const std::string &file) {
    std::map<std::string, std::string> keys;
    std::ifstream in(file, std::ios::binary);
    if (!in) return keys;
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        if (const std::string sec = sectionOf(line); !sec.empty()) {
            current = sec;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || current != kSection) continue;
        keys[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return keys;
}

int parseInt(const std::map<std::string, std::string> &keys, const char *key, int def) {
    auto it = keys.find(key);
    if (it == keys.end() || it->second.empty()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

std::string parseStr(const std::map<std::string, std::string> &keys, const char *key,
                     const std::string &def) {
    auto it = keys.find(key);
    return it == keys.end() ? def : it->second;
}

// Rewrite the file, updating/adding keys inside [config] and keeping every
// other line (comments, other sections) untouched. Returns false when the
// file cannot be opened or written (previously this failed silently).
bool writeKeys(const std::string &file, const std::map<std::string, std::string> &updates) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
        }
    }

    // Which [config]-section lines already carry each updated key?
    std::map<std::string, bool> written;
    for (const auto &kv : updates) written[kv.first] = false;

    std::string current;
    bool haveConfigSection = false;
    std::size_t insertAt = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (const std::string sec = sectionOf(line); !sec.empty()) {
            if (sec == kSection) haveConfigSection = true;
            current = sec;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || current != kSection) continue;
        const std::string key = trim(line.substr(0, eq));
        auto it = updates.find(key);
        if (it == updates.end()) continue;
        lines[i] = key + "=" + it->second;
        written[key] = true;
    }

    if (!haveConfigSection && !updates.empty()) {
        if (!lines.empty() && !lines.front().empty() && lines.front()[0] != '[')
            lines.insert(lines.begin(), "");
        lines.insert(lines.begin(), kSectionHeader);
        insertAt = 1; // after header, before previous blank
    } else if (haveConfigSection) {
        // Append missing keys right after the last line of the [config] block.
        current.clear();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!sectionOf(lines[i]).empty()) {
                current = sectionOf(lines[i]);
                continue;
            }
            if (current == kSection) insertAt = i + 1;
        }
    }

    std::vector<std::string> pending;
    for (const auto &kv : updates)
        if (!written[kv.first]) pending.push_back(kv.first + "=" + kv.second);
    for (std::size_t i = 0; i < pending.size(); ++i) lines.insert(lines.begin() + insertAt + i, pending[i]);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto &line : lines) out << line << "\r\n";
    return static_cast<bool>(out);
}

} // namespace

std::string AppSettings::path() const {
    return platform::settingsDirectory() + "/6502_ide.ini";
}

AppSettings &Settings::get() {
    static AppSettings instance;
    return instance;
}

void Settings::load() {
    AppSettings &s = get();
    const std::string file = s.path();
    // One-time import: if no INI exists next to the exe but a legacy
    // %APPDATA% one does (pre-portable builds), bring it over.
    if (!platform::fileExists(file)) {
#ifdef _WIN32
        char appData[MAX_PATH]{};
        if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH)) {
            const std::string legacy = std::string(appData) + "\\6502IDE\\6502_ide.ini";
            std::error_code ec;
            if (platform::fileExists(legacy))
                std::filesystem::copy_file(legacy, file, std::filesystem::copy_options::overwrite_existing, ec);
        }
#endif
    }
    const auto keys = readKeys(file);

    // Schema versioning/migration: keys from older schemas are renamed or
    // reinterpreted here before anything reads them.
    const int schema = parseInt(keys, "schemaVersion", 1);
    int speed = parseInt(keys, "speed", 60000);
    if (schema < 2) {
        // v1 -> v2: "speed" used to be instructions/frame; it is now
        // instructions per idle slice. Values below 1000 were old-scale.
        if (speed < 1000) speed *= 60;
    }

    s.themeIndex = parseInt(keys, "theme", 0);
    s.editorFontSize = std::clamp(parseInt(keys, "fontSize", 15), 8, 32);
    s.tabWidth = parseInt(keys, "tabWidth", 4);
    s.fbSize = parseInt(keys, "fbSize", 64);
    s.speed = static_cast<uint32_t>(std::clamp(speed, 100, 50000000));
    s.smoothScreen = parseInt(keys, "smooth", 0) != 0;
    s.autoSave = parseInt(keys, "autoSave", 0) != 0;
    s.maxLeftPct = std::clamp(parseInt(keys, "leftPct", 38), 15, 75);
    s.rightPct = std::clamp(parseInt(keys, "rightPct", 22), 12, 45);
    s.midSplitPct = std::clamp(parseInt(keys, "midSplit", 55), 20, 85);
    s.regsPct = std::clamp(parseInt(keys, "regsPct", 34), 10, 80);
    s.hexPct = std::clamp(parseInt(keys, "hexPct", 42), 10, 80);
    s.maximized = parseInt(keys, "maximized", 1) != 0;
    s.windowW = parseInt(keys, "windowW", 1450);
    s.windowH = parseInt(keys, "windowH", 900);
    s.comPort = parseStr(keys, "comPort", "COM3");
    s.baud = static_cast<uint32_t>(parseInt(keys, "baud", 115200));
    s.vkUp = parseInt(keys, "vkUp", 0x26);
    s.vkDown = parseInt(keys, "vkDown", 0x28);
    s.vkLeft = parseInt(keys, "vkLeft", 0x25);
    s.vkRight = parseInt(keys, "vkRight", 0x27);
    s.vkButtonA = parseInt(keys, "vkA", 'Z');
    s.vkButtonB = parseInt(keys, "vkB", 'X');
    s.vkStart = parseInt(keys, "vkStart", 0x0D);
    s.vkSelect = parseInt(keys, "vkSelect", 0x10);
    s.remotePort = parseInt(keys, "remotePort", 0);
    s.debugConsoleHz = std::clamp(parseInt(keys, "debugHz", 10), 1, 60);
    s.cpuCore = parseInt(keys, "cpuCore", 0) != 0 ? 1 : 0;
    s.includePaths = parseStr(keys, "includePaths", "");
    s.predefinedDefines = parseStr(keys, "defines", "");
    s.warnLevel = std::clamp(parseInt(keys, "warnLevel", 3), 0, 7);
    s.autoRebuild = parseInt(keys, "autoRebuild", 0) != 0;
    s.turboEnabled = parseInt(keys, "turbo", 1) != 0;
    for (int i = 0; i < 10; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "recent%d", i);
        const std::string path = parseStr(keys, key, "");
        if (!path.empty()) s.recentFiles.push_back(path);
    }
    if (s.fbSize < 8) s.fbSize = 8;
    if (s.fbSize > 512) s.fbSize = 512;
}

bool Settings::save() {
    AppSettings &s = get();
    auto setInt = [](std::map<std::string, std::string> &m, const char *key, int value) {
        m[key] = std::to_string(value);
    };
    std::map<std::string, std::string> updates;
    setInt(updates, "theme", s.themeIndex);
    setInt(updates, "fontSize", s.editorFontSize);
    setInt(updates, "tabWidth", s.tabWidth);
    setInt(updates, "fbSize", s.fbSize);
    setInt(updates, "speed", static_cast<int>(s.speed));
    setInt(updates, "smooth", s.smoothScreen ? 1 : 0);
    setInt(updates, "autoSave", s.autoSave ? 1 : 0);
    setInt(updates, "leftPct", s.maxLeftPct);
    setInt(updates, "rightPct", s.rightPct);
    setInt(updates, "midSplit", s.midSplitPct);
    setInt(updates, "regsPct", s.regsPct);
    setInt(updates, "hexPct", s.hexPct);
    setInt(updates, "maximized", s.maximized ? 1 : 0);
    setInt(updates, "windowW", s.windowW);
    setInt(updates, "windowH", s.windowH);
    updates["comPort"] = s.comPort;
    setInt(updates, "baud", static_cast<int>(s.baud));
    setInt(updates, "vkUp", s.vkUp);
    setInt(updates, "vkDown", s.vkDown);
    setInt(updates, "vkLeft", s.vkLeft);
    setInt(updates, "vkRight", s.vkRight);
    setInt(updates, "vkA", s.vkButtonA);
    setInt(updates, "vkB", s.vkButtonB);
    setInt(updates, "vkStart", s.vkStart);
    setInt(updates, "vkSelect", s.vkSelect);
    setInt(updates, "remotePort", s.remotePort);
    setInt(updates, "debugHz", s.debugConsoleHz);
    setInt(updates, "cpuCore", s.cpuCore);
    setInt(updates, "warnLevel", s.warnLevel);
    setInt(updates, "autoRebuild", s.autoRebuild ? 1 : 0);
    setInt(updates, "turbo", s.turboEnabled ? 1 : 0);
    updates["includePaths"] = s.includePaths;
    updates["defines"] = s.predefinedDefines;
    updates["schemaVersion"] = std::to_string(kIniSchemaVersion);
    for (int i = 0; i < 10; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "recent%d", i);
        updates[key] = i < static_cast<int>(s.recentFiles.size()) ? s.recentFiles[static_cast<std::size_t>(i)]
                                                                  : "";
    }
    if (!writeKeys(s.path(), updates)) {
        // No shared GUI status path reaches here (15 call sites across GUI
        // modules, most outside this file's ownership): surface the failure
        // through the debugger output stream; callers may also check the
        // returned bool.
        const std::string msg = "6502_ide: Settings::save failed: " + s.path() + "\n";
#ifdef _WIN32
        OutputDebugStringA(msg.c_str());
#endif
        std::fputs(msg.c_str(), stderr);
        return false;
    }
    return true;
}

void Settings::pushRecent(const std::string &filePath) {
    AppSettings &s = get();
    auto it = std::find(s.recentFiles.begin(), s.recentFiles.end(), filePath);
    if (it != s.recentFiles.end()) s.recentFiles.erase(it);
    s.recentFiles.insert(s.recentFiles.begin(), filePath);
    if (s.recentFiles.size() > 10) s.recentFiles.resize(10);
}
