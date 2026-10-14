#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Bump when persisted keys change meaning; migrate() upgrades old files.
constexpr int kIniSchemaVersion = 2;

struct AppSettings {
    int themeIndex = 0;
    int editorFontSize = 15;
    int tabWidth = 4;
    int fbSize = 64;
    uint16_t fbBase = 0x0200;
    uint32_t speed = 60000;
    std::string comPort = "COM3";
    uint32_t baud = 115200;
    bool smoothScreen = false;
    bool autoSave = false;
    int maxLeftPct = 38;
    int rightPct = 22;   // width of the right column (regs/hex/disasm), % of window
    int midPct = 58;
    int midSplitPct = 55;
    int regsPct = 34;    // regs height, % of content height (right column)
    int hexPct = 42;     // hex height, % of the space below regs
    bool maximized = true;
    int windowW = 1450;
    int windowH = 900;
    std::vector<std::string> recentFiles;

    int vkUp = 0x26;
    int vkDown = 0x28;
    int vkLeft = 0x25;
    int vkRight = 0x27;
    int vkButtonA = 'Z';
    int vkButtonB = 'X';
    int vkStart = 0x0D;
    int vkSelect = 0x10;
    int remotePort = 0;
    int debugConsoleHz = 10; // Debug console update rate (Hz)

    // CPU core selection: 0 = NMOS 6502 (default), 1 = WDC 65C02.
    int cpuCore = 0;
    // Additional assembler include search paths ("path1;path2").
    std::string includePaths;
    // Warning level bitmask: 1=warnings on, +2=pedantic page-cross notes,
    // +4=unofficial-opcode usage notes. 0 disables all warnings.
    int warnLevel = 3;
    // Command-line style preprocessor defines applied to every assembly
    // ("NAME=value,OTHER=2").
    std::string predefinedDefines;
    // Rebuild automatically whenever the source file is saved.
    bool autoRebuild = false;
    // Fast-forward while Tab is held.
    bool turboEnabled = true;

    std::string path() const;
};

class Settings {
  public:
    static AppSettings &get();
    static void load();
    static void save();
    static void pushRecent(const std::string &file);
};
