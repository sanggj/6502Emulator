#pragma once

#include <cstdint>
#include <string>

class Emulator;

namespace savestate {

bool saveToFile(Emulator &emu, const std::string &path, std::string &error);
bool loadFromFile(Emulator &emu, const std::string &path, std::string &error);
std::string slotPath(int slot);

} // namespace savestate
