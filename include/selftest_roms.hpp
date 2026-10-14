#pragma once

#include <cstdint>
#include <string>

class Memory;
class Emulator;

// Klaus Dormann 6502_functional_test runner (item 4).
// Loads the standard 64 KiB binary image, starts at `start` (default $0400)
// and runs until the PC is trapped in a tight loop. Success requires the
// trap address to equal `successTrap` (default $3469).
bool klausFunctionalTest(Memory &memory, const std::string &binPath, unsigned maxInstructions,
                         std::string &report, uint16_t start = 0x0400, uint16_t successTrap = 0x3469);

// nestest.nes automated validation (item 5). Maps the ROM, starts the CPU at
// $C000 (automated mode) and compares PC/registers/flags/cycles against
// nestest.log for up to `maxLines` entries.
bool nestestValidate(Emulator &emu, const std::string &romPath, const std::string &logPath, int maxLines,
                     std::string &report);

// Locate a data file: tries CWD then the given fallback directories.
std::string findTestDataFile(const std::string &name);
