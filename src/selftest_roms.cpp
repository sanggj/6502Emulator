#include "selftest_roms.hpp"
#include "cpu.hpp"
#include "emulator.hpp"
#include "ines.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

std::string findTestDataFile(const std::string &name) {
    const char *dirs[] = {"", "tests\\data\\", "tests/data/", "..\\tests\\data\\", "../tests/data/"};
    for (const char *dir : dirs) {
        const std::string path = std::string(dir) + name;
        std::ifstream in(path, std::ios::binary);
        if (in) return path;
    }
    (void)0;
#ifdef _WIN32
    // Also try relative to the executable directory.
    char exe[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string base(exe);
        const std::size_t slash = base.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string root = base.substr(0, slash) + "\\..";
            for (const char *rel : {"\\tests\\data\\", "/tests/data/"}) {
                const std::string path = root + rel + name;
                std::ifstream in(path, std::ios::binary);
                if (in) return path;
            }
        }
    }
#endif
    return {};
}

bool klausFunctionalTest(Memory &memory, const std::string &binPath, unsigned maxInstructions,
                         std::string &report, uint16_t start, uint16_t successTrap) {
    std::ifstream in(binPath, std::ios::binary);
    if (!in) {
        report = "cannot open functional test image: " + binPath;
        return false;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (image.size() != 65536) {
        report = "unexpected image size " + std::to_string(image.size()) + " (want 65536)";
        return false;
    }
    std::vector<uint8_t> bytes(65536);
    std::copy(image.begin(), image.end(), bytes.begin());
    memory.reset(0x00);
    std::string err;
    if (!memory.loadBytes(bytes, 0x0000, err)) {
        report = err;
        return false;
    }
    // The binary is a full 64K image including the trap code and vectors.
    CPU cpu(memory); // standalone CPU; caller may reuse its own instead
    cpu.setDecimalModeEnabled(true);
    cpu.PC = start;
    cpu.SP = 0xFD;
    cpu.A = cpu.X = cpu.Y = 0;
    cpu.P = 0x24;

    uint16_t prev1 = 0xFFFF, prev2 = 0xFFFE;
    unsigned executed = 0;
    while (executed < maxInstructions) {
        if (cpu.step() == 0) break;
        ++executed;
        if (cpu.PC == prev1 && prev1 == prev2) break; // trapped in tight loop
        prev2 = prev1;
        prev1 = cpu.PC;
    }
    std::ostringstream o;
    o << "executed " << executed << " instructions, trapped at $" << std::hex << std::uppercase << cpu.PC;
    if (cpu.halted()) o << " (CPU jammed)";
    report = o.str();
    return !cpu.halted() && cpu.PC == successTrap && executed < maxInstructions;
}

namespace {

struct NestestLine {
    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, p = 0, sp = 0;
    uint64_t cyc = 0;
};

bool parseNestestLine(const std::string &line, NestestLine &out) {
    if (line.size() < 4) return false;
    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    auto hex2 = [&](std::size_t pos) -> int {
        if (pos + 1 >= line.size()) return -1;
        const int hi = hexDigit(line[pos]), lo = hexDigit(line[pos + 1]);
        return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
    };
    // PC is four separate hex digits at the start of the line.
    const int d0 = hexDigit(line[0]), d1 = hexDigit(line[1]);
    const int d2 = hexDigit(line[2]), d3 = hexDigit(line[3]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) return false;
    out.pc = static_cast<uint16_t>((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
    std::size_t regPos = line.find("A:");
    if (regPos == std::string::npos) return false;
    const int ra = hex2(regPos + 2);
    regPos = line.find("X:", regPos);
    const int rx = regPos == std::string::npos ? -1 : hex2(regPos + 2);
    regPos = line.find("Y:", regPos);
    const int ry = regPos == std::string::npos ? -1 : hex2(regPos + 2);
    regPos = line.find("P:", regPos);
    const int rp = regPos == std::string::npos ? -1 : hex2(regPos + 2);
    regPos = line.find("SP:", regPos);
    const int rs = regPos == std::string::npos ? -1 : hex2(regPos + 3);
    regPos = line.find("CYC:", regPos);
    if (ra < 0 || rx < 0 || ry < 0 || rp < 0 || rs < 0 || regPos == std::string::npos) return false;
    out.a = static_cast<uint8_t>(ra);
    out.x = static_cast<uint8_t>(rx);
    out.y = static_cast<uint8_t>(ry);
    out.p = static_cast<uint8_t>(rp);
    out.sp = static_cast<uint8_t>(rs);
    out.cyc = std::strtoull(line.c_str() + regPos + 4, nullptr, 10);
    return true;
}

} // namespace

bool nestestValidate(Emulator &emu, const std::string &romPath, const std::string &logPath, int maxLines,
                     std::string &report) {
    ines::NesRom rom;
    std::string err;
    if (!ines::loadFile(romPath, rom, err)) {
        report = err;
        return false;
    }
    if (!ines::mapInto(emu, rom, err)) {
        report = err;
        return false;
    }
    std::ifstream log(logPath);
    if (!log) {
        report = "cannot open nestest.log: " + logPath;
        return false;
    }
    emu.cpu().PC = 0xC000; // automated mode entry
    emu.cpu().SP = 0xFD;
    emu.cpu().P = 0x24;
    emu.cpu().A = emu.cpu().X = emu.cpu().Y = 0;
    emu.cpu().clearHalt();
    // nestest.log counts the 7 reset cycles in its CYC column.
    emu.cpu().restoreTiming(7, 0, 0, false);
    // The reference log generator ran with decimal mode disabled; our decimal
    // implementation is validated separately by the Klaus Dormann suite.
    emu.cpu().setDecimalModeEnabled(false);

    std::ostringstream diffs;
    int compared = 0;
    int mismatches = 0;
    std::string line;
    while (compared < maxLines && std::getline(log, line)) {
        NestestLine want;
        if (!parseNestestLine(line, want)) continue;
        CPU &c = emu.cpu();
        bool ok = true;
        auto diff = [&](const char *what, unsigned gotV, unsigned wantV) {
            if (gotV != wantV) {
                ok = false;
                diffs << "line " << (compared + 1) << " PC=$" << std::hex << std::uppercase << want.pc << ": "
                      << what << " got $" << gotV << " want $" << wantV << '\n';
            }
        };
        diff("PC", c.PC, want.pc);
        diff("A", c.A, want.a);
        diff("X", c.X, want.x);
        diff("Y", c.Y, want.y);
        diff("P", c.P & (FLAG_C | FLAG_Z | FLAG_I | FLAG_D | FLAG_B | FLAG_V | FLAG_N),
             want.p & (FLAG_C | FLAG_Z | FLAG_I | FLAG_D | FLAG_B | FLAG_V | FLAG_N));
        diff("SP", c.SP, want.sp);
        diff("CYC", static_cast<unsigned>(c.getTotalCycles()), static_cast<unsigned>(want.cyc));
        ++compared;
        if (!ok) {
            ++mismatches;
            if (mismatches >= 5) break; // keep reports bounded
        }
        if (emu.step() <= 0) {
            diffs << "CPU halted at line " << (compared + 1) << '\n';
            ++mismatches;
            break;
        }
    }
    std::ostringstream o;
    o << "nestest: compared " << compared << " lines, " << mismatches << " mismatch(es)";
    if (!diffs.str().empty()) o << "\n" << diffs.str();
    report = o.str();
    return mismatches == 0 && compared >= std::min(maxLines, 100);
}
