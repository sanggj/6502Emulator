#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Emulator;

namespace ines {

struct NesRom {
    uint8_t mapperId = 0;
    int prgBanks16 = 0;
    int chrBanks8 = 0;
    bool hasBattery = false;
    std::vector<uint8_t> prg;
    std::vector<uint8_t> chr;
};

bool loadFile(const std::string &path, NesRom &out, std::string &error);
bool mapInto(Emulator &emu, const NesRom &rom, std::string &error);

} // namespace ines
