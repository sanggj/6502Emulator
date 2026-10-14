#include "savestate.hpp"
#include "apu.hpp"
#include "cpu.hpp"
#include "emulator.hpp"
#include "memory.hpp"
#include "platform.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace savestate {

namespace {

constexpr char kMagic[4] = {'S', 'T', '8', 1};
constexpr uint32_t kVersion = 2;

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
    uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// Explicit little-endian helpers for the v2 trailer (append-only fields).
void put8(std::vector<uint8_t> &v, uint64_t x) { v.push_back(static_cast<uint8_t>(x)); }
void put32(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFFu));
}
uint32_t get32(const std::vector<uint8_t> &v, std::size_t pos) {
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<uint32_t>(v[pos + static_cast<std::size_t>(i)]) << (i * 8);
    return x;
}

} // namespace

bool saveToFile(Emulator &emu, const std::string &path, std::string &error) {
    const CPU &cpu = emu.cpu();
    const Memory &mem = emu.memory();

    struct Payload {
        char magic[4];
        uint32_t version;
        uint8_t a, x, y, sp, p;
        uint16_t pc;
        uint64_t totalCycles;
        uint64_t instructionCount;
        uint8_t halted;
        uint8_t decimalEnabled;
        uint8_t running;
        uint8_t frameCounter;
        uint8_t ram[Memory::SIZE];
    };
    static_assert(sizeof(Payload) >= Memory::SIZE + 32, "payload layout mismatch");

    Payload payload{};
    std::memcpy(payload.magic, kMagic, 4);
    payload.version = kVersion;
    payload.a = cpu.A;
    payload.x = cpu.X;
    payload.y = cpu.Y;
    payload.sp = cpu.SP;
    payload.p = cpu.P;
    payload.pc = cpu.PC;
    payload.totalCycles = cpu.getTotalCycles();
    payload.instructionCount = cpu.getInstructionCount();
    payload.halted = cpu.halted() ? 1 : 0;
    payload.decimalEnabled = cpu.decimalModeIsEnabled() ? 1 : 0;
    payload.running = emu.running() ? 1 : 0;
    payload.frameCounter = emu.frameCounter();
    std::memcpy(payload.ram, mem.data(), Memory::SIZE);

    // v2 trailer appended after the payload, explicit little-endian:
    // apuLen(u32) apuBlob | nmiLine irqLine nmiPending core lastOpcode (u8)
    // | lastCycles(u32).
    // waitingForInterrupt_/suppressPollOnce_/instructionCount have no public
    // setter (and suppressPollOnce_ no getter) in cpu.hpp, so they are skipped.
    std::vector<uint8_t> apuBlob;
    emu.apu().saveState(apuBlob);
    std::vector<uint8_t> extra;
    extra.reserve(4 + apuBlob.size() + 9);
    put32(extra, apuBlob.size());
    extra.insert(extra.end(), apuBlob.begin(), apuBlob.end());
    put8(extra, cpu.nmiLine());
    put8(extra, cpu.irqLine());
    put8(extra, cpu.nmiPending());
    put8(extra, static_cast<uint8_t>(cpu.core()));
    put8(extra, cpu.lastOpcode());
    put32(extra, static_cast<uint32_t>(cpu.getCycles()));

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    std::vector<uint8_t> blob(sizeof(Payload) + extra.size());
    std::memcpy(blob.data(), &payload, sizeof(Payload));
    std::memcpy(blob.data() + sizeof(Payload), extra.data(), extra.size());
    const uint32_t checksum = fnv1a(blob.data(), blob.size());
    out.write(reinterpret_cast<const char *>(&checksum), sizeof(checksum));
    out.write(reinterpret_cast<const char *>(&payload), sizeof(Payload));
    out.write(reinterpret_cast<const char *>(extra.data()), static_cast<std::streamsize>(extra.size()));
    if (!out) {
        error = "write failed";
        return false;
    }
    error = "state saved";
    return true;
}

bool loadFromFile(Emulator &emu, const std::string &path, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }

    uint32_t storedChecksum = 0;
    in.read(reinterpret_cast<char *>(&storedChecksum), sizeof(storedChecksum));

    struct Payload {
        char magic[4];
        uint32_t version;
        uint8_t a, x, y, sp, p;
        uint16_t pc;
        uint64_t totalCycles;
        uint64_t instructionCount;
        uint8_t halted;
        uint8_t decimalEnabled;
        uint8_t running;
        uint8_t frameCounter;
        uint8_t ram[Memory::SIZE];
    };
    Payload payload{};
    in.read(reinterpret_cast<char *>(&payload), sizeof(Payload));
    if (!in || std::memcmp(payload.magic, kMagic, 4) != 0) {
        error = "not a valid savestate";
        return false;
    }
    const bool v1 = payload.version == 1;
    if (!v1 && payload.version != kVersion) {
        error = "unsupported savestate version";
        return false;
    }

    // v2 trailer is everything after the payload.
    constexpr std::streamoff kBase =
        static_cast<std::streamoff>(sizeof(storedChecksum) + sizeof(Payload));
    in.seekg(0, std::ios::end);
    const std::streamoff fileSize = in.tellg();
    if (fileSize < kBase) {
        error = "not a valid savestate";
        return false;
    }
    in.seekg(kBase, std::ios::beg);
    std::vector<uint8_t> extra(static_cast<std::size_t>(fileSize - kBase));
    if (!extra.empty()) in.read(reinterpret_cast<char *>(extra.data()),
                                 static_cast<std::streamsize>(extra.size()));
    if (!extra.empty() && !in) {
        error = "savestate read failed";
        return false;
    }
    std::vector<uint8_t> blob(sizeof(Payload) + extra.size());
    std::memcpy(blob.data(), &payload, sizeof(Payload));
    std::memcpy(blob.data() + sizeof(Payload), extra.data(), extra.size());
    if (fnv1a(blob.data(), blob.size()) != storedChecksum) {
        error = "savestate checksum mismatch";
        return false;
    }
    // v2 trailer: apuLen(u32) apuBlob | nmiLine irqLine nmiPending core
    // lastOpcode (u8) | lastCycles(u32).  v1 files have no trailer; defaults
    // below leave the live APU/interrupt-line state untouched.
    uint8_t nmiLine = 0, irqLine = 0, nmiPending = 0, core = 0xFF, lastOpcode = 0;
    uint32_t lastCycles = 0;
    bool haveTrailer = false;
    std::vector<uint8_t> apuBlob;
    if (!v1) {
        if (extra.size() < 4) {
            error = "corrupt savestate";
            return false;
        }
        const uint32_t apuLen = get32(extra, 0);
        if (extra.size() != 4u + static_cast<std::size_t>(apuLen) + 5u + 4u) {
            error = "corrupt savestate";
            return false;
        }
        const std::size_t cpuOff = 4u + apuLen;
        nmiLine = extra[cpuOff];
        irqLine = extra[cpuOff + 1];
        nmiPending = extra[cpuOff + 2];
        core = extra[cpuOff + 3];
        lastOpcode = extra[cpuOff + 4];
        lastCycles = get32(extra, cpuOff + 5);
        apuBlob.assign(extra.begin() + 4, extra.begin() + static_cast<std::streamoff>(cpuOff));
        haveTrailer = true;
    }

    CPU &cpu = emu.cpu();
    cpu.A = payload.a;
    cpu.X = payload.x;
    cpu.Y = payload.y;
    cpu.SP = payload.sp;
    cpu.P = payload.p;
    cpu.PC = payload.pc;
    cpu.setDecimalModeEnabled(payload.decimalEnabled != 0);
    cpu.restoreTiming(payload.totalCycles, lastOpcode, static_cast<int32_t>(lastCycles),
                      payload.halted != 0);
    if (core <= static_cast<uint8_t>(CPU::Core::WDC65C02))
        cpu.setCore(static_cast<CPU::Core>(core));
    if (haveTrailer) {
        cpu.setIrqLine(irqLine != 0);
        // Atomic restore: driving setNmiLine() would latch a phantom edge.
        cpu.restoreNmiState(nmiLine != 0, nmiPending != 0);
        emu.apu().loadState(apuBlob);
    }
    emu.memory().markAllDirty();
    std::memcpy(emu.memory().data(), payload.ram, Memory::SIZE);
    emu.setFrameCounter(payload.frameCounter);
    if (payload.running)
        emu.start();
    else
        emu.pause();
    error = "state loaded";
    return true;
}

std::string slotPath(int slot) {
    const std::string dir = platform::settingsDirectory() + "/states";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char file[32];
    std::snprintf(file, sizeof(file), "/slot%d.st8", slot);
    return dir + file;
}

} // namespace savestate
