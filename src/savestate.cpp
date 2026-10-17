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
// Savestate format compatibility rules (dispatched on payload.version):
//   v1: checksum + payload only, no trailer (empty trailer still loads).
//   v2: payload + apuLen(u32) + apu blob + nmiLine irqLine nmiPending core
//       lastOpcode (u8) + lastCycles(u32).
//   v3: v2 layout + instructionCount(u64) + transientFlags(u8), where
//       transientFlags bit layout: bit0 = waitingForInterrupt,
//       bit1 = suppressPollOnce, bit2 = waiIrqBypass (applied through
//       CPU::restoreTransientState), bit3 = external IRQ source (Emulator
//       requestIrq / terminal `irq` command). Files written before bit1-3
//       existed carry zeros there and load as "all clear"; the external
//       source then falls back to the old irqLine && !frameIrq inference.
// Versions 1, 2 and 3 all load (same kVersion = 3: the extended bits are a
// compatible extension of the v3 flags byte, not a layout change); newer or
// unknown versions are rejected. Every
// failure path (bad magic, unsupported version, truncation, oversized or
// corrupt trailer, checksum mismatch) returns false BEFORE any emulator
// mutation - all parsing happens into locals first.
constexpr uint32_t kVersion = 3;

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
void put64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFFu));
}
uint32_t get32(const std::vector<uint8_t> &v, std::size_t pos) {
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<uint32_t>(v[pos + static_cast<std::size_t>(i)]) << (i * 8);
    return x;
}
uint64_t get64(const std::vector<uint8_t> &v, std::size_t pos) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i)
        x |= static_cast<uint64_t>(v[pos + static_cast<std::size_t>(i)]) << (i * 8);
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

    // v3 trailer appended after the payload, explicit little-endian:
    // apuLen(u32) apuBlob | nmiLine irqLine nmiPending core lastOpcode (u8)
    // | lastCycles(u32) | instructionCount(u64) | transientFlags(u8).
    std::vector<uint8_t> apuBlob;
    emu.apu().saveState(apuBlob);
    std::vector<uint8_t> extra;
    extra.reserve(4 + apuBlob.size() + 9 + 8 + 1);
    put32(extra, apuBlob.size());
    extra.insert(extra.end(), apuBlob.begin(), apuBlob.end());
    put8(extra, cpu.nmiLine());
    put8(extra, cpu.irqLine());
    put8(extra, cpu.nmiPending());
    put8(extra, static_cast<uint8_t>(cpu.core()));
    put8(extra, cpu.lastOpcode());
    put32(extra, static_cast<uint32_t>(cpu.getCycles()));
    put64(extra, cpu.getInstructionCount());
    // transientFlags: bit0 waitingForInterrupt, bit1 suppressPollOnce,
    // bit2 waiIrqBypass (CPU getters), bit3 external IRQ source (Emulator).
    const uint8_t transientFlags =
        static_cast<uint8_t>((cpu.isWaitingForInterrupt() ? 0x01u : 0x00u) |
                             (cpu.suppressPollOnceActive() ? 0x02u : 0x00u) |
                             (cpu.waiIrqBypassActive() ? 0x04u : 0x00u) |
                             (emu.externalIrqAsserted() ? 0x08u : 0x00u));
    put8(extra, transientFlags);

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
    if (payload.version < 1 || payload.version > kVersion) {
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
    // Bound the trailer BEFORE allocating: apuLen comes from the file, so
    // never size anything from it. Fixed per-version generous cap (NOT a live
    // APU probe): 64KB APU blob + 4 (apuLen) + 9 (v2 tail) + 9 (v3 tail).
    // A legitimate trailer is ~4 + 100 + 18 bytes; anything past the cap is a
    // hostile/corrupt file (would otherwise allocate fileSize - kBase), and
    // the size equality check below still pins apuLen exactly.
    constexpr std::streamoff kMaxExtra = 65536 + 4 + 9 + 9;
    if (fileSize - kBase > kMaxExtra) {
        error = "corrupt savestate: trailer exceeds size cap";
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
    // v2/v3 trailer: apuLen(u32) apuBlob | nmiLine irqLine nmiPending core
    // lastOpcode (u8) | lastCycles(u32), v3 additionally:
    // instructionCount(u64) | transientFlags(u8).  v1 files have no trailer;
    // defaults below leave the live APU/interrupt-line state untouched.
    // All trailer parsing happens into locals: a corrupt trailer returns
    // false before the first emulator mutation.
    uint8_t nmiLine = 0, irqLine = 0, nmiPending = 0, core = 0xFF, lastOpcode = 0;
    uint32_t lastCycles = 0;
    uint64_t savedInstrCount = 0;
    uint8_t transientFlags = 0;
    bool haveTrailer = false;
    std::vector<uint8_t> apuBlob;
    if (!v1) {
        if (extra.size() < 4) {
            error = "corrupt savestate";
            return false;
        }
        const uint32_t apuLen = get32(extra, 0);
        // Size math stays in size_t (64-bit): evaluating 4u + apuLen + 9u in
        // uint32 would wrap for apuLen near 0xFFFFFFFF and pass a bogus
        // check; with size_t the equality against the real file size bounds
        // apuLen long before any indexing below.
        const std::size_t cpuOff = 4u + static_cast<std::size_t>(apuLen);
        const std::size_t tail = 9u + (payload.version >= 3 ? 9u : 0u);
        if (extra.size() != cpuOff + tail) {
            error = "corrupt savestate";
            return false;
        }
        nmiLine = extra[cpuOff];
        irqLine = extra[cpuOff + 1];
        nmiPending = extra[cpuOff + 2];
        core = extra[cpuOff + 3];
        lastOpcode = extra[cpuOff + 4];
        lastCycles = get32(extra, cpuOff + 5);
        if (payload.version >= 3) {
            savedInstrCount = get64(extra, cpuOff + 9);
            transientFlags = extra[cpuOff + 17];
        }
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
    // Instruction count + transient flags. v3 carries them in the trailer;
    // v1/v2 stored no transient flags, so they are restored as their (usual)
    // false defaults - waiting/suppress/waiIrqBypass are only transiently
    // true (during WAI, or for one instruction after a taken branch).
    if (haveTrailer && payload.version >= 3) {
        cpu.restoreTransientState(savedInstrCount, (transientFlags & 0x01u) != 0,
                                  (transientFlags & 0x02u) != 0, (transientFlags & 0x04u) != 0);
    } else {
        cpu.restoreTransientState(payload.instructionCount, false, false, false);
    }
    if (haveTrailer) {
        // Order matters: the APU blob restores the frame-IRQ flag first, then
        // the external source is re-applied via requestIrq (which ORs both).
        // bit3 is authoritative for extended saves; older v1/v2/v3 files have
        // it clear and keep the historical irqLine && !frameIrq inference.
        emu.apu().loadState(apuBlob);
        const bool external = (transientFlags & 0x08u) != 0 ||
                              (irqLine != 0 && !emu.apu().frameIrqAsserted());
        emu.requestIrq(external);
        // Atomic restore: driving setNmiLine() would latch a phantom edge.
        cpu.restoreNmiState(nmiLine != 0, nmiPending != 0);
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
