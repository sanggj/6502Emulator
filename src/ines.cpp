#include "ines.hpp"
#include "cpu.hpp"
#include "emulator.hpp"
#include "memory.hpp"

#include <cstring>
#include <fstream>

namespace ines {

bool loadFile(const std::string &path, NesRom &out, std::string &error) {
    out = NesRom{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    uint8_t header[16]{};
    in.read(reinterpret_cast<char *>(header), 16);
    if (in.gcount() != 16) {
        error = "file too small";
        return false;
    }
    if (std::memcmp(header, "NES\x1A", 4) != 0) {
        error = "missing iNES signature";
        return false;
    }
    if ((header[7] & 0x0Cu) != 0 || header[12] != 0 || header[13] != 0 || header[14] != 0 ||
        header[15] != 0) {
        error = "NES 2.0 headers not supported";
        return false;
    }
    out.prgBanks16 = header[4];
    out.chrBanks8 = header[5];
    out.hasBattery = (header[6] & 0x02u) != 0;
    out.mapperId = static_cast<uint8_t>((header[6] >> 4) | (header[7] & 0xF0u));
    if (out.prgBanks16 == 0) {
        error = "no PRG banks";
        return false;
    }

    const std::size_t prgSize = static_cast<std::size_t>(out.prgBanks16) * 16384;
    const std::size_t chrSize = static_cast<std::size_t>(out.chrBanks8) * 8192;
    // Trainer (512 bytes) sits between header and PRG when header[6] bit 2 is set.
    const std::size_t trainerSize = (header[6] & 0x04u) != 0 ? 512u : 0u;
    in.seekg(0, std::ios::end);
    const std::streamoff total = in.tellg();
    if (total < static_cast<std::streamoff>(16 + trainerSize + prgSize + chrSize)) {
        error = "truncated ROM file";
        return false;
    }
    in.seekg(16 + static_cast<std::streamoff>(trainerSize), std::ios::beg);
    out.prg.resize(prgSize);
    in.read(reinterpret_cast<char *>(out.prg.data()), static_cast<std::streamsize>(prgSize));
    if (chrSize > 0) {
        out.chr.resize(chrSize);
        in.read(reinterpret_cast<char *>(out.chr.data()), static_cast<std::streamsize>(chrSize));
    }
    error = "rom loaded: mapper " + std::to_string(out.mapperId) + ", " + std::to_string(out.prgBanks16) +
            "x16KB PRG";
    return true;
}

bool mapInto(Emulator &emu, const NesRom &rom, std::string &error) {
    if (rom.mapperId == 1 || rom.mapperId > 3) {
        error = "unsupported mapper " + std::to_string(rom.mapperId) + " (NROM/UNROM/CNROM only)";
        return false;
    }
    Memory &mem = emu.memory();
    mem.reset(0x00);
    if (rom.prg.size() == 16384) {
        for (int copy = 0; copy < 2; ++copy)
            for (std::size_t i = 0; i < rom.prg.size(); ++i)
                mem.write(static_cast<uint16_t>(0x8000 + copy * 16384 + i), rom.prg[i]);
    } else {
        for (std::size_t i = 0; i < rom.prg.size() && 0x8000 + i <= 0xFFFF; ++i)
            mem.write(static_cast<uint16_t>(0x8000 + i), rom.prg[i]);
    }
    emu.memory().markAllDirty();
    emu.cpu().reset();
    error = "ROM mapped at $8000";
    return true;
}

} // namespace ines
