// Integration tests: Emulator burn+run, savestates, iNES rejection of fake
// ROM, Klaus Dormann functional test, Memory helpers, Intel HEX round-trip.
// This file also owns main() and the per-suite summary.
#include "apu.hpp"
#include "emulator.hpp"
#include "hexfile.hpp"
#include "ines.hpp"
#include "memory.hpp"
#include "savestate.hpp"
#include "selftest_roms.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int g_core_pass = 0;
int g_core_fail = 0;

extern int g_cpu_pass, g_cpu_fail;
extern int g_asm_pass, g_asm_fail;
extern int g_plat_pass, g_plat_fail;

int run_test_cpu();
int run_test_asm();
int run_test_platform();
int run_test_core();

#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (expr) {                                                                          \
            ++g_core_pass;                                                                    \
        } else {                                                                             \
            ++g_core_fail;                                                                    \
            std::fprintf(stderr, "FAIL test_core.cpp:%d %s\n", __LINE__, #expr);              \
        }                                                                                    \
    } while (0)

namespace {

std::string tempDir() {
    const char *t = std::getenv("TEMP");
    if (t && *t) return std::string(t);
    t = std::getenv("TMP");
    if (t && *t) return std::string(t);
    return ".";
}

} // namespace

int run_test_core() {
    // ---- Emulator: assembleAndBurn + runInstructions ----
    Emulator emu;
    std::string msg;
    const std::string src =
        "        .org $8000\n"
        "start:  LDA #$05\n"
        "        STA $0200\n"
        "loop:   JMP loop\n"
        "        .org $FFFC\n"
        "        .word $8000\n";
    const bool burned = emu.assembleAndBurn(src, msg);
    CHECK(burned);
    if (!burned) std::fprintf(stderr, "  assembleAndBurn: %s\n", msg.c_str());
    if (burned) {
        CHECK(emu.hasProgram());
        CHECK(emu.cpu().PC == 0x8000); // reset vector burned at $FFFC
        emu.start();
        const uint64_t n = emu.runInstructions(1000);
        CHECK(n == 1000); // JMP loop never halts; budget reached
        CHECK(emu.memory().read(0x0200) == 5);
        CHECK(emu.cpu().A == 5);
    }

    // ---- savestate: save -> mutate -> load restores memory CRC ----
    const uint32_t crcBefore = emu.memory().crc32();
    const std::string statePath = tempDir() + "\\test_core_state.st8";
    std::string err;
    CHECK(savestate::saveToFile(emu, statePath, err));
    emu.memory().write(0x1234, 0x77);
    emu.cpu().A = 0xEE;
    CHECK(emu.memory().crc32() != crcBefore);
    const bool loaded = savestate::loadFromFile(emu, statePath, err);
    if (!loaded) std::fprintf(stderr, "  loadFromFile: %s\n", err.c_str());
    CHECK(loaded);
    CHECK(emu.memory().crc32() == crcBefore);
    CHECK(emu.memory().read(0x1234) != 0x77);
    CHECK(emu.cpu().A == 5);
    std::remove(statePath.c_str());

    // ---- savestate: NMI line/pending restored atomically (no phantom edge) ----
    emu.cpu().restoreNmiState(true, false); // line held high, edge already serviced
    CHECK(savestate::saveToFile(emu, statePath, err));
    emu.cpu().restoreNmiState(false, false); // mutate away from saved state
    CHECK(savestate::loadFromFile(emu, statePath, err));
    CHECK(emu.cpu().nmiLine());
    CHECK(!emu.cpu().nmiPending()); // driving setNmiLine() would latch a phantom NMI
    emu.cpu().restoreNmiState(false, false);
    std::remove(statePath.c_str());

    // ---- legacy v1 savestate (checksum + payload, no trailer) still loads ----
    // Mirrors the on-disk v1 Payload layout of src/savestate.cpp; checksum is
    // FNV-1a over the payload bytes only.
    struct V1Payload {
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
    V1Payload v1{};
    std::memcpy(v1.magic, "ST8\x01", 4);
    v1.version = 1;
    v1.a = 0x42;
    v1.x = 0x11;
    v1.y = 0x22;
    v1.sp = 0xFD;
    v1.p = static_cast<uint8_t>(FLAG_U | FLAG_C);
    v1.pc = 0x8000;
    v1.totalCycles = 1234;
    v1.instructionCount = 9;
    v1.decimalEnabled = 1;
    std::memcpy(v1.ram, emu.memory().data(), Memory::SIZE);
    const uint8_t *v1bytes = reinterpret_cast<const uint8_t *>(&v1);
    uint32_t v1hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < sizeof(V1Payload); ++i) {
        v1hash ^= v1bytes[i];
        v1hash *= 0x01000193u;
    }
    {
        std::ofstream vf(statePath, std::ios::binary);
        vf.write(reinterpret_cast<const char *>(&v1hash), sizeof(v1hash));
        vf.write(reinterpret_cast<const char *>(v1bytes), sizeof(V1Payload));
        CHECK(static_cast<bool>(vf));
    }
    emu.cpu().A = 0x99;
    const bool v1loaded = savestate::loadFromFile(emu, statePath, err);
    if (!v1loaded) std::fprintf(stderr, "  v1 loadFromFile: %s\n", err.c_str());
    CHECK(v1loaded);
    CHECK(emu.cpu().A == 0x42);
    CHECK(emu.cpu().PC == 0x8000);
    CHECK(emu.cpu().getTotalCycles() == 1234);
    std::remove(statePath.c_str());
    const std::string nestest = findTestDataFile("nestest.nes");
    if (nestest.empty()) {
        std::fprintf(stderr, "SKIP: nestest validation (tests\\data\\nestest.nes absent)\n");
    } else {
        // Real nestest.nes (kevtris): mapper 0, 16 KiB PRG + 8 KiB CHR.
        ines::NesRom rom;
        std::string ierr;
        const bool okRom = ines::loadFile(nestest, rom, ierr);
        if (!okRom) std::fprintf(stderr, "  ines load: %s\n", ierr.c_str());
        CHECK(okRom);
        CHECK(rom.mapperId == 0);
        CHECK(rom.prgBanks16 == 1);
        CHECK(rom.chrBanks8 == 1);
        CHECK(rom.prg.size() == 16384);
        CHECK(rom.chr.size() == 8192);
        // Full trace validation against the golden log (PC/regs/flags/cycles
        // per instruction, automated entry at $C000). First 5003 lines =
        // official-opcode section; the unofficial continuation is skipped.
        const std::string nlog = findTestDataFile("nestest.log");
        if (nlog.empty()) {
            std::fprintf(stderr, "SKIP: nestest trace (tests\\data\\nestest.log absent)\n");
        } else {
            Emulator nesEmu;
            std::string nreport;
            const bool okNest = nestestValidate(nesEmu, nestest, nlog, 5003, nreport);
            if (!okNest) std::fprintf(stderr, "  nestest report: %s\n", nreport.c_str());
            CHECK(okNest);
        }
    }

    // ---- Klaus Dormann 6502 functional test (golden CPU test) ----
    {
        const std::string bin = findTestDataFile("6502_functional_test.bin");
        if (bin.empty()) {
            // Fixture removed from the repo in commit v0.2.0. Restore
            // tests\data\6502_functional_test.bin (Klaus Dormann's image) to
            // re-enable the golden CPU validation - without it the CPU core is
            // covered by unit tests only.
            std::fprintf(stderr,
                         "SKIP: Klaus golden CPU test (tests\\data\\6502_functional_test.bin absent)\n");
        } else {
            Memory kmem;
            std::string report;
            // This image needs ~30.6M instructions to reach the $3469 success
            // trap (verified standalone: 30,646,178 steps, ~0.4 s). Spec's
            // 5M cap is too low for this build; 40M keeps the run bounded.
            const bool okKlaus = klausFunctionalTest(kmem, bin, 40000000, report);
            if (!okKlaus) std::fprintf(stderr, "  klaus report: %s\n", report.c_str());
            CHECK(okKlaus);
        }
    }

    // ---- Memory: crc32 / copyRange / search / fill ----
    {
        Memory m;
        m.reset(0x00);
        m.write(0x1000, 0xDE);
        m.write(0x1001, 0xAD);
        m.write(0x1002, 0xBE);
        m.write(0x1003, 0xEF);
        const uint32_t c1 = m.crc32(0x1000, 4);
        const std::vector<uint8_t> r = m.copyRange(0x1000, 4);
        CHECK(r.size() == 4 && r[0] == 0xDE && r[1] == 0xAD && r[2] == 0xBE && r[3] == 0xEF);
        CHECK(m.crc32(0x1000, 4) == c1); // stable

        std::vector<uint8_t> pat = {0xDE, 0xAD};
        const std::vector<uint16_t> hits = m.search(pat);
        CHECK(hits.size() == 1 && hits[0] == 0x1000);
        std::vector<uint8_t> pat2 = {0xBE, 0xEF};
        const std::vector<uint16_t> hits2 = m.search(pat2, 0x1000, 4);
        CHECK(hits2.size() == 1 && hits2[0] == 0x1002);
        std::vector<uint8_t> miss = {0x12, 0x34};
        CHECK(m.search(miss).empty());

        m.write(0x1001, 0x00);
        const uint32_t c2 = m.crc32(0x1000, 4);
        CHECK(c2 != c1);
        CHECK(m.crc32(0x1000, 4) == c2);

        m.fill(0x2000, 4, 0x55);
        const std::vector<uint8_t> rf = m.copyRange(0x2000, 4);
        CHECK(rf.size() == 4 && rf[0] == 0x55 && rf[3] == 0x55);
    }

    // ---- Intel HEX save/load round-trip ----
    {
        Memory h1;
        h1.reset(0x00);
        for (int i = 0; i < 16; ++i) h1.write(static_cast<uint16_t>(0x3000 + i), static_cast<uint8_t>(i * 17));
        const std::string hexPath = tempDir() + "\\test_core_roundtrip.hex";
        std::string herr;
        CHECK(hexfile::saveIntelHex(h1, 0x3000, 16, hexPath, herr));
        Memory h2;
        h2.reset(0x11);
        const bool hexOk = hexfile::loadIntelHex(hexPath, h2, herr);
        if (!hexOk) std::fprintf(stderr, "  loadIntelHex: %s\n", herr.c_str());
        CHECK(hexOk);
        CHECK(h2.crc32(0x3000, 16) == h1.crc32(0x3000, 16));
        std::remove(hexPath.c_str());
    }

    // ---- legacy v2 savestate: the OLD trailer layout still loads in v3 ----
    // Trailer layout before v3: u32 apuLen | apu blob | nmiLine irqLine
    // nmiPending core lastOpcode (5 bytes) | u32 lastCycles.
    {
        V1Payload v2p{};
        std::memcpy(v2p.magic, "ST8\x01", 4);
        v2p.version = 2;
        v2p.a = 0x42;
        v2p.x = 0x11;
        v2p.y = 0x22;
        v2p.sp = 0xFD;
        v2p.p = static_cast<uint8_t>(FLAG_U | FLAG_C);
        v2p.pc = 0x8000;
        v2p.totalCycles = 5555;
        v2p.instructionCount = 77;
        v2p.decimalEnabled = 1;
        std::memcpy(v2p.ram, emu.memory().data(), Memory::SIZE);

        std::vector<uint8_t> apuBlob;
        emu.apu().saveState(apuBlob);
        std::vector<uint8_t> file(sizeof(V1Payload) + 4 + apuBlob.size() + 9);
        std::memcpy(file.data(), &v2p, sizeof(V1Payload));
        std::size_t pos = sizeof(V1Payload);
        auto put32v = [&file, &pos](uint32_t x) {
            for (int i = 0; i < 4; ++i) file[pos++] = static_cast<uint8_t>((x >> (i * 8)) & 0xFFu);
        };
        put32v(static_cast<uint32_t>(apuBlob.size()));
        std::memcpy(file.data() + pos, apuBlob.data(), apuBlob.size());
        pos += apuBlob.size();
        file[pos++] = 0; // nmiLine
        file[pos++] = 0; // irqLine
        file[pos++] = 0; // nmiPending
        file[pos++] = 0; // core = NMOS6502
        file[pos++] = 0; // lastOpcode
        put32v(777);     // lastCycles (u32)
        // checksum = FNV-1a over payload + trailer
        uint32_t hash = 0x811C9DC5u;
        for (std::size_t i = 0; i < file.size(); ++i) {
            hash ^= file[i];
            hash *= 0x01000193u;
        }
        std::vector<uint8_t> disk(4 + file.size());
        for (int i = 0; i < 4; ++i) disk[static_cast<std::size_t>(i)] = static_cast<uint8_t>((hash >> (i * 8)) & 0xFFu);
        std::memcpy(disk.data() + 4, file.data(), file.size());
        {
            std::ofstream vf(statePath, std::ios::binary);
            vf.write(reinterpret_cast<const char *>(disk.data()), static_cast<std::streamsize>(disk.size()));
            CHECK(static_cast<bool>(vf));
        }
        emu.cpu().A = 0x99;
        const bool v2loaded = savestate::loadFromFile(emu, statePath, err);
        if (!v2loaded) std::fprintf(stderr, "  v2 loadFromFile: %s\n", err.c_str());
        CHECK(v2loaded);
        CHECK(emu.cpu().A == 0x42);
        CHECK(emu.cpu().getTotalCycles() == 5555);
        CHECK(emu.cpu().getCycles() == 777);
        CHECK(emu.cpu().core() == CPU::Core::NMOS6502);
        std::remove(statePath.c_str());
    }

    // ---- v3 round trip: instruction count + transient flags survive ----
    // Frame IRQ AND the external source both assert at save time: the old
    // reconstruction (irqLine && !frameIrq) lost externalIrq_ in exactly
    // this case; the flags byte now carries it as bit3.
    emu.apu().writeRegister(0x4017, 0x00); // 4-step, IRQ not inhibited
    emu.apu().tick(29829);                  // raise the frame flag (works with no audio backend)
    CHECK(emu.apu().frameIrqAsserted());
    emu.requestIrq(true); // external source too: both sources assert the CPU line
    emu.cpu().restoreTransientState(4242, /*waiting=*/true, /*suppress=*/true,
                                    /*waiIrqBypass=*/true);
    CHECK(emu.cpu().isWaitingForInterrupt());
    CHECK(savestate::saveToFile(emu, statePath, err));
    emu.cpu().restoreTransientState(0, false, false, false);
    emu.requestIrq(false);
    emu.apu().reset(); // clear both sources before the load
    emu.memory().write(0x1234, 0x5C);
    const bool v3loaded = savestate::loadFromFile(emu, statePath, err);
    if (!v3loaded) std::fprintf(stderr, "  v3 loadFromFile: %s\n", err.c_str());
    CHECK(v3loaded);
    CHECK(emu.cpu().isWaitingForInterrupt());
    CHECK(emu.cpu().suppressPollOnceActive());  // bit1 persisted (was always 0)
    CHECK(emu.cpu().waiIrqBypassActive());      // bit2 persisted (was always 0)
    CHECK(emu.apu().frameIrqAsserted());        // APU source restored from the blob
    CHECK(emu.externalIrqAsserted());           // bit3: external survives both-asserted
    CHECK(emu.cpu().irqLine());                 // line = frame OR external
    CHECK(emu.cpu().getInstructionCount() == 4242);
    CHECK(emu.memory().read(0x1234) != 0x5C);
    emu.cpu().restoreTransientState(0, false, false, false);
    emu.requestIrq(false); // release the external source for the following blocks
    emu.apu().reset();
    std::remove(statePath.c_str());

    // ---- hostile v3 APU blob: out-of-range duty/dutyStep are masked on
    // load (kDutyWave[4][8] would otherwise be indexed OOB in Apu::mix) ----
    {
        const uint32_t preCrc = emu.memory().crc32();
        V1Payload p{};
        std::memcpy(p.magic, "ST8\x01", 4);
        p.version = 3;
        p.a = 0x77;
        p.x = 0x11;
        p.y = 0x22;
        p.sp = 0xFD;
        p.p = static_cast<uint8_t>(FLAG_U | FLAG_C);
        p.pc = 0x8000;
        p.totalCycles = 4242;
        p.instructionCount = 55;
        p.decimalEnabled = 1;
        std::memcpy(p.ram, emu.memory().data(), Memory::SIZE);

        std::vector<uint8_t> apuBlob;
        emu.apu().saveState(apuBlob);
        CHECK(apuBlob.size() > 0x18u + 3u);
        // APU blob layout (src/apu.cpp saveState): 0x18 register bytes, then
        // per square: enabled, duty, dutyStep, ...
        const std::size_t kDutyOff = 0x18u + 1u; // square 0 duty
        apuBlob[kDutyOff] = 0xFF;                // hostile: valid size, bad index
        apuBlob[kDutyOff + 1] = 0xFF;            // hostile dutyStep

        std::vector<uint8_t> file(sizeof(V1Payload) + 4u + apuBlob.size() + 18u);
        std::memcpy(file.data(), &p, sizeof(V1Payload));
        std::size_t pos = sizeof(V1Payload);
        auto put32v3 = [&file, &pos](uint32_t x) {
            for (int i = 0; i < 4; ++i) file[pos++] = static_cast<uint8_t>((x >> (i * 8)) & 0xFFu);
        };
        auto put64v3 = [&file, &pos](uint64_t x) {
            for (int i = 0; i < 8; ++i) file[pos++] = static_cast<uint8_t>((x >> (i * 8)) & 0xFFu);
        };
        put32v3(static_cast<uint32_t>(apuBlob.size()));
        std::memcpy(file.data() + pos, apuBlob.data(), apuBlob.size());
        pos += apuBlob.size();
        file[pos++] = 0; // nmiLine
        file[pos++] = 0; // irqLine
        file[pos++] = 0; // nmiPending
        file[pos++] = 0; // core = NMOS6502
        file[pos++] = 0; // lastOpcode
        put32v3(999);    // lastCycles (u32)
        put64v3(55);     // instructionCount (u64)
        // transientFlags: bit0 waiting, bit1 suppressPollOnce,
        // bit2 waiIrqBypass, bit3 external IRQ source.
        file[pos++] = 0x08; // bit3 set with irqLine byte 0: external survives
        CHECK(pos == file.size());
        uint32_t hash = 0x811C9DC5u;
        for (std::size_t i = 0; i < file.size(); ++i) {
            hash ^= file[i];
            hash *= 0x01000193u;
        }
        std::vector<uint8_t> disk(4u + file.size());
        for (int i = 0; i < 4; ++i)
            disk[static_cast<std::size_t>(i)] = static_cast<uint8_t>((hash >> (i * 8)) & 0xFFu);
        std::memcpy(disk.data() + 4, file.data(), file.size());
        {
            std::ofstream vf(statePath, std::ios::binary);
            vf.write(reinterpret_cast<const char *>(disk.data()),
                     static_cast<std::streamsize>(disk.size()));
            CHECK(static_cast<bool>(vf));
        }
        emu.cpu().A = 0x99;
        emu.memory().write(0x1234, 0x66); // mutate away from the crafted payload
        const bool hostileLoaded = savestate::loadFromFile(emu, statePath, err);
        if (!hostileLoaded) std::fprintf(stderr, "  hostile APU load: %s\n", err.c_str());
        CHECK(hostileLoaded);
        CHECK(emu.cpu().A == 0x77); // CPU/memory restored from the payload
        CHECK(emu.cpu().getTotalCycles() == 4242);
        CHECK(emu.memory().crc32() == preCrc);
        CHECK(emu.externalIrqAsserted()); // flags byte bit3 wins over irqLine=0
        emu.requestIrq(false);            // release for the following blocks
        // Re-save: the masked duty/dutyStep must come back in range.
        CHECK(savestate::saveToFile(emu, statePath, err));
        {
            std::ifstream rf(statePath, std::ios::binary);
            const std::vector<uint8_t> again((std::istreambuf_iterator<char>(rf)),
                                             std::istreambuf_iterator<char>());
            const std::size_t apuOff = 4u + sizeof(V1Payload) + 4u;
            CHECK(again.size() > apuOff + kDutyOff + 1u);
            if (again.size() > apuOff + kDutyOff + 1u) {
                CHECK(again[apuOff + kDutyOff] <= 3u);      // duty range 0..3
                CHECK(again[apuOff + kDutyOff + 1] <= 7u);  // dutyStep range 0..7
            }
        }
        std::remove(statePath.c_str());
    }

    // ---- savestate: APU blob size mismatch -> live APU untouched,
    // external IRQ loads FALSE (never inferred from live APU state) ----
    // Contract: the file itself is structurally valid, so the load succeeds
    // (CPU/memory restored); only the APU half is rejected (Apu::loadState
    // false) and the external source then loads false even with bit3 set.
    {
        const uint32_t preCrc = emu.memory().crc32();
        emu.apu().writeRegister(0x4000, 0x11); // value the crafted blob carries

        V1Payload p{};
        std::memcpy(p.magic, "ST8\x01", 4);
        p.version = 3;
        p.a = 0x5B;
        p.x = 0x12;
        p.y = 0x23;
        p.sp = 0xFD;
        p.p = static_cast<uint8_t>(FLAG_U | FLAG_C);
        p.pc = 0x8000;
        p.totalCycles = 7777;
        p.instructionCount = 9;
        p.decimalEnabled = 1;
        std::memcpy(p.ram, emu.memory().data(), Memory::SIZE);

        std::vector<uint8_t> goodBlob;
        emu.apu().saveState(goodBlob);
        CHECK(goodBlob.size() > 1u);

        auto writeV3File = [&](const std::vector<uint8_t> &blob, uint8_t irqByte, uint8_t flags) {
            std::vector<uint8_t> file(sizeof(V1Payload) + 4u + blob.size() + 18u);
            std::memcpy(file.data(), &p, sizeof(V1Payload));
            std::size_t pos = sizeof(V1Payload);
            auto put32v = [&](uint32_t x) {
                for (int i = 0; i < 4; ++i) file[pos++] = static_cast<uint8_t>((x >> (i * 8)) & 0xFFu);
            };
            auto put64v = [&](uint64_t x) {
                for (int i = 0; i < 8; ++i) file[pos++] = static_cast<uint8_t>((x >> (i * 8)) & 0xFFu);
            };
            put32v(static_cast<uint32_t>(blob.size()));
            std::memcpy(file.data() + pos, blob.data(), blob.size());
            pos += blob.size();
            file[pos++] = 0;       // nmiLine
            file[pos++] = irqByte; // irqLine (legacy; never used for inference)
            file[pos++] = 0;       // nmiPending
            file[pos++] = 0;       // core = NMOS6502
            file[pos++] = 0;       // lastOpcode
            put32v(333);           // lastCycles (u32)
            put64v(9);             // instructionCount (u64)
            file[pos++] = flags;   // transientFlags
            CHECK(pos == file.size());
            uint32_t hash = 0x811C9DC5u;
            for (std::size_t i = 0; i < file.size(); ++i) {
                hash ^= file[i];
                hash *= 0x01000193u;
            }
            std::vector<uint8_t> disk(4u + file.size());
            for (int i = 0; i < 4; ++i)
                disk[static_cast<std::size_t>(i)] = static_cast<uint8_t>((hash >> (i * 8)) & 0xFFu);
            std::memcpy(disk.data() + 4, file.data(), file.size());
            std::ofstream vf(statePath, std::ios::binary);
            vf.write(reinterpret_cast<const char *>(disk.data()),
                     static_cast<std::streamsize>(disk.size()));
            CHECK(static_cast<bool>(vf));
        };

        // Truncated blob (one byte short of the expected size).
        const std::vector<uint8_t> badBlob(goodBlob.begin(), goodBlob.end() - 1);
        writeV3File(badBlob, 0, 0x08); // bit3 SET: must still NOT restore external
        emu.apu().writeRegister(0x4000, 0x7E); // live sentinel the blob must not clobber
        emu.memory().write(0x1234, 0x44);
        emu.requestIrq(true); // live external asserted before the load
        const bool badLoaded = savestate::loadFromFile(emu, statePath, err);
        if (!badLoaded) std::fprintf(stderr, "  mismatch-blob load: %s\n", err.c_str());
        CHECK(badLoaded);                             // file itself is valid: load succeeds
        CHECK(emu.cpu().A == 0x5B);                   // CPU restored from the payload
        CHECK(emu.memory().crc32() == preCrc);        // memory restored
        CHECK(emu.apu().registerValue(0x00) == 0x7E); // rejected blob: live APU untouched
        CHECK(!emu.externalIrqAsserted());            // rejected blob: external false, no inference
        emu.requestIrq(false);
        std::remove(statePath.c_str());

        // v3 file predating bit3 (flags byte 0) with irqLine=1: the external
        // source loads false instead of falling back to live-state inference.
        writeV3File(goodBlob, 1, 0x00);
        emu.requestIrq(true); // live external asserted again
        const bool legacyLoaded = savestate::loadFromFile(emu, statePath, err);
        if (!legacyLoaded) std::fprintf(stderr, "  legacy-flags load: %s\n", err.c_str());
        CHECK(legacyLoaded);
        CHECK(emu.apu().registerValue(0x00) == 0x11); // accepted blob: APU restored
        CHECK(!emu.externalIrqAsserted());            // no bit3 -> external false
        emu.requestIrq(false);
        std::remove(statePath.c_str());
    }

    // ---- savestate corruption matrix: hostile files rejected, state intact ----
    {
        emu.cpu().A = 0x5A;
        emu.memory().write(0x1234, 0xAB);
        const uint32_t sentCrc = emu.memory().crc32();
        CHECK(savestate::saveToFile(emu, statePath, err));
        std::ifstream gf(statePath, std::ios::binary);
        const std::vector<uint8_t> good((std::istreambuf_iterator<char>(gf)),
                                        std::istreambuf_iterator<char>());
        gf.close();
        CHECK(good.size() > sizeof(V1Payload) + 4u);
        auto putBytes = [&statePath](const std::vector<uint8_t> &b) {
            std::ofstream f(statePath, std::ios::binary);
            if (!b.empty()) f.write(reinterpret_cast<const char *>(b.data()),
                                    static_cast<std::streamsize>(b.size()));
        };
        auto rejected = [&](const char *what) {
            const bool ok = savestate::loadFromFile(emu, statePath, err);
            if (ok) std::fprintf(stderr, "  corruption case '%s' accepted\n", what);
            CHECK(!ok);
            CHECK(!err.empty());
            CHECK(emu.cpu().A == 0x5A); // sentinel: emulator state untouched
            CHECK(emu.memory().crc32() == sentCrc);
        };
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
        putBytes(t);
        rejected("truncated");
        t = good;
        t[4] = 'Q'; // payload magic damaged
        putBytes(t);
        rejected("bad magic");
        t = good;
        t[0] ^= 0xFF; // checksum byte flipped
        putBytes(t);
        rejected("checksum mismatch");
        putBytes(std::vector<uint8_t>(300, 0xEE));
        rejected("garbage");
        // apuLen = 0xFFFFFFFF with a VALID checksum: size math must reject
        // before any allocation or indexing.
        t = good;
        const std::size_t trailerOff = 4u + sizeof(V1Payload);
        t[trailerOff + 0] = 0xFF;
        t[trailerOff + 1] = 0xFF;
        t[trailerOff + 2] = 0xFF;
        t[trailerOff + 3] = 0xFF;
        uint32_t hash = 0x811C9DC5u;
        for (std::size_t i = 4; i < t.size(); ++i) {
            hash ^= t[i];
            hash *= 0x01000193u;
        }
        for (int i = 0; i < 4; ++i) t[static_cast<std::size_t>(i)] = static_cast<uint8_t>((hash >> (i * 8)) & 0xFFu);
        putBytes(t);
        rejected("apuLen overflow claim");
        // oversized trailer: padded far past the length cap
        t = good;
        t.insert(t.end(), 65536, 0x77);
        putBytes(t);
        rejected("oversized trailer");
        // payload.version 0 and 99 are both unsupported (only 1..3 load);
        // version sits right after checksum(4) + magic(4). Checksum is
        // recomputed so the rejection proves the version gate, not FNV.
        for (uint32_t badVer : {0u, 99u}) {
            t = good;
            t[8] = static_cast<uint8_t>(badVer & 0xFFu);
            t[9] = static_cast<uint8_t>((badVer >> 8) & 0xFFu);
            t[10] = 0;
            t[11] = 0;
            uint32_t vhash = 0x811C9DC5u;
            for (std::size_t i = 4; i < t.size(); ++i) {
                vhash ^= t[i];
                vhash *= 0x01000193u;
            }
            for (int i = 0; i < 4; ++i)
                t[static_cast<std::size_t>(i)] = static_cast<uint8_t>((vhash >> (i * 8)) & 0xFFu);
            putBytes(t);
            rejected(badVer == 0u ? "payload version 0" : "payload version 99");
        }
        std::remove(statePath.c_str());
    }

    // ---- iNES malformed matrix: every bad header fails with a message ----
    {
        const std::string romPath = tempDir() + "\\test_core_fake.nes";
        auto putFile = [&romPath](const std::vector<uint8_t> &b) {
            std::ofstream f(romPath, std::ios::binary);
            if (!b.empty()) f.write(reinterpret_cast<const char *>(b.data()),
                                    static_cast<std::streamsize>(b.size()));
        };
        auto rejected = [&](const char *what) {
            ines::NesRom r;
            std::string ie;
            const bool ok = ines::loadFile(romPath, r, ie);
            if (ok) std::fprintf(stderr, "  iNES case '%s' accepted\n", what);
            CHECK(!ok);
            CHECK(!ie.empty());
        };
        std::vector<uint8_t> hdr(16, 0);
        std::memcpy(hdr.data(), "XES\x1A", 4); // bad magic
        putFile(hdr);
        rejected("bad magic");
        putFile(std::vector<uint8_t>(8, 0x4E)); // shorter than 16-byte header
        rejected("short header");
        hdr.assign(16, 0);
        std::memcpy(hdr.data(), "NES\x1A", 4);
        hdr[4] = 0xFF; // absurd PRG count, file ~26 bytes
        hdr[5] = 0xFF;
        hdr.resize(26);
        putFile(hdr);
        rejected("absurd bank counts");
        hdr.assign(16, 0);
        std::memcpy(hdr.data(), "NES\x1A", 4);
        hdr[4] = 3;  // 48KB PRG
        hdr[6] = 0x20; // mapper 2
        putFile(hdr);
        rejected("mapper2 PRG > 32KB");
        hdr[4] = 4;  // 64KB PRG
        hdr[6] = 0x00; // mapper 0
        putFile(hdr);
        rejected("NROM PRG > 32KB");
        hdr[4] = 1;
        hdr[5] = 0;
        hdr[6] = 0x06; // battery + trainer, mapper 0, body too short
        hdr.resize(16 + 600);
        putFile(hdr);
        rejected("battery+trainer truncated");
        // valid battery+trainer NROM: trainer skipped, 16KB PRG read
        hdr.assign(16 + 512 + 16384, 0);
        std::memcpy(hdr.data(), "NES\x1A", 4);
        hdr[4] = 1;
        hdr[6] = 0x06;
        hdr[16] = 0xAA; // trainer marker: must NOT end up in PRG[0]
        putFile(hdr);
        {
            ines::NesRom r;
            std::string ie;
            const bool ok = ines::loadFile(romPath, r, ie);
            if (!ok) std::fprintf(stderr, "  valid trainer ROM: %s\n", ie.c_str());
            CHECK(ok);
            CHECK(r.hasBattery);
            CHECK(r.prg.size() == 16384);
            CHECK(r.prg[0] == 0x00); // trainer really skipped
        }
        // CHR-RAM (chrBanks8 == 0): legal, no CHR mapped, no error
        hdr.assign(16 + 16384, 0);
        std::memcpy(hdr.data(), "NES\x1A", 4);
        hdr[4] = 1;
        hdr[5] = 0;
        putFile(hdr);
        {
            ines::NesRom r;
            std::string ie;
            const bool ok = ines::loadFile(romPath, r, ie);
            if (!ok) std::fprintf(stderr, "  CHR-RAM ROM: %s\n", ie.c_str());
            CHECK(ok);
            CHECK(r.chrBanks8 == 0);
            CHECK(r.chr.empty());
        }
        std::remove(romPath.c_str());
    }

    // ---- APU frame IRQ: 4-step sequencer sets the flag; $4015 reports and
    // clears it; 5-step mode and $4017 bit 6 suppress it ----
    {
        Apu &apu = emu.apu();
        apu.reset();
        apu.setEnabled(true);
        apu.writeRegister(0x4017, 0x00); // 4-step mode, IRQ not inhibited
        apu.tick(29829);                 // crosses the 4th quarter-frame step
        uint8_t st = apu.readStatus(0x4015);
        CHECK((st & 0x40) != 0); // frame IRQ reported
        CHECK((st & 0x20) != 0); // mode mirror (4-step)
        st = apu.readStatus(0x4015);
        CHECK((st & 0x40) == 0); // clear-on-read
        CHECK((st & 0x20) == 0);
        // $4017 bit 7 = 5-step mode: no frame IRQ ever
        apu.reset();
        apu.setEnabled(true);
        apu.writeRegister(0x4017, 0x80);
        apu.tick(60000);
        CHECK((apu.readStatus(0x4015) & 0x40) == 0);
        // $4017 bit 6 inhibits the frame IRQ
        apu.reset();
        apu.setEnabled(true);
        apu.writeRegister(0x4017, 0x40);
        apu.tick(29829);
        CHECK((apu.readStatus(0x4015) & 0x40) == 0);
        // CPU line level + $4017 bit 6/7 write acknowledges a pending flag
        apu.reset();
        apu.setEnabled(true);
        apu.writeRegister(0x4017, 0x00);
        apu.tick(29829);
        CHECK(apu.frameIrqAsserted());
        apu.writeRegister(0x4017, 0xC0);
        CHECK(!apu.frameIrqAsserted());
        CHECK((apu.readStatus(0x4015) & 0x40) == 0);
        apu.setEnabled(false);
        apu.reset();
    }

    // ---- IRQ line wiring: APU frame IRQ and the external source share the
    // CPU line; the line releases only when neither source asserts ----
    {
        Apu &apu = emu.apu();
        apu.reset();
        apu.setEnabled(true);
        apu.writeRegister(0x4017, 0x00);
        emu.tickApu(29829); // tick the sequencer, then sync the IRQ line
        CHECK(emu.cpu().irqLine());
        (void)emu.memory().read(0x4015); // clears the frame IRQ flag via hook
        emu.tickApu(0);
        CHECK(!emu.cpu().irqLine());
        emu.requestIrq(true); // external source keeps the line asserted...
        (void)emu.memory().read(0x4015);
        emu.tickApu(0);
        CHECK(emu.cpu().irqLine());
        emu.requestIrq(false);
        emu.tickApu(0);
        CHECK(!emu.cpu().irqLine());
        apu.setEnabled(false);
        apu.reset();
    }

    // ---- APU io hooks sync the IRQ line BEFORE the CPU's in-step poll ----
    // The NMOS boundary poll (cpu.cpp pollIrqAtBoundary) runs INSIDE
    // CPU::step(); an instruction that clears the frame flag ($4015 read,
    // $4017 write ack) must drop irqLine_ from the hook itself, or the stale
    // high line vector's a 7-cycle IRQ hardware would not deliver.
    {
        const std::string isr =
            "        .org $9000\n"
            "isr:    INC $0201\n"
            "        JMP isr\n";
        const std::string tail =
            "        .org $FFFC\n"
            "        .word $8000\n"
            "        .org $FFFE\n"
            "        .word $9000\n";
        // (a) guard: the frame IRQ still fires when enabled and I is clear.
        Emulator e;
        std::string m;
        const std::string fireSrc =
            "        .org $8000\n"
            "start:  CLI\n"
            "        NOP\n"
            "halt:   JMP halt\n" + isr + tail;
        CHECK(e.assembleAndBurn(fireSrc, m));
        e.setCoreVariant(CPU::Core::NMOS6502);
        e.apu().writeRegister(0x4017, 0x00); // 4-step, IRQ not inhibited
        e.apu().tick(29829);                  // raise the frame flag (no audio backend)
        e.tickApu(0);                         // sync the line
        CHECK(e.cpu().irqLine());
        e.start();
        for (int i = 0; i < 6 && e.step() > 0; ++i) {
        }
        CHECK(e.memory().read(0x0201) != 0); // ISR entered: IRQ serviced with I=0
        CHECK(e.memory().read(0x0200) == 0); // halt loop never reached
        e.apu().reset();

        // (b) LDA $4015 clears the flag mid-instruction: NO IRQ afterwards.
        Emulator e2;
        const std::string clearSrc =
            "        .org $8000\n"
            "start:  CLI\n"
            "        LDA $4015\n"
            "        STA $0200\n"
            "halt:   JMP halt\n" + isr + tail;
        CHECK(e2.assembleAndBurn(clearSrc, m));
        e2.setCoreVariant(CPU::Core::NMOS6502);
        e2.apu().writeRegister(0x4017, 0x00);
        e2.apu().tick(29829);
        e2.tickApu(0);
        CHECK(e2.cpu().irqLine()); // flag asserted before the run
        e2.start();
        for (int i = 0; i < 6 && e2.step() > 0; ++i) {
        }
        CHECK(e2.memory().read(0x0200) == 0x60); // STA $4015 status (0x40|0x20) ran
        CHECK(e2.memory().read(0x0201) == 0);    // ISR never entered: no spurious IRQ
        e2.apu().reset();
    }

    // ---- frame sequencer must run WITHOUT an audio backend (no init()) ----
    // enabled_ only turns on after a successful audio-backend init; on CI /
    // audio-less machines frame IRQ and $4015 bit 6 must still report.
    {
        Apu fresh; // never init()'d: enabled_ stays false
        CHECK(!fresh.enabled());
        fresh.writeRegister(0x4017, 0x00);
        fresh.tick(29829); // must not early-return before the sequencer
        CHECK((fresh.readStatus(0x4015) & 0x40) != 0); // bit6 reports
        CHECK((fresh.readStatus(0x4015) & 0x40) == 0); // clear-on-read
    }

    // ---- registerValue: range-checked (old `& 0x1F` mask read past the
    // 0x18-byte registers_ for indices 0x18..0x1F); out of range -> 0 ----
    {
        Apu r;
        r.writeRegister(0x4000, 0x11);
        r.writeRegister(0x4008, 0xAB);
        CHECK(r.registerValue(0x00) == 0x11);
        CHECK(r.registerValue(0x08) == 0xAB); // old `& 0x1F` mask read this fine; its bug was OOB at 0x18..0x1F
        CHECK(r.registerValue(0x17) == 0x00);
        CHECK(r.registerValue(0x18) == r.registerValue(0)); // clamps, no OOB
        CHECK(r.registerValue(-1) == r.registerValue(0));
    }

    // ---- Apu::loadState: exact-size match only; mismatch leaves state ----
    {
        Apu s;
        s.writeRegister(0x4000, 0x11);
        std::vector<uint8_t> blob;
        s.saveState(blob);
        CHECK(!blob.empty());
        CHECK(s.loadState(blob)); // exact size: accepted (true)
        s.writeRegister(0x4000, 0x77); // live mutation after the save
        const std::vector<uint8_t> shortBlob(blob.begin(), blob.end() - 1);
        CHECK(!s.loadState(shortBlob));            // wrong size: rejected (false)
        CHECK(!s.loadState(std::vector<uint8_t>())); // empty: rejected (false)
        CHECK(s.registerValue(0x00) == 0x77);      // rejected blobs leave live state untouched
    }

    // ---- startWavRecord probe must not truncate an existing file ----
    // 0 samples captured -> stopWavRecord() returns early; before the append
    // probe the file was already truncated to 0 bytes at record start.
    {
        const std::string wavPath = tempDir() + "\\test_core_probe.wav";
        {
            std::ofstream f(wavPath, std::ios::binary);
            f << "PREEXISTING";
        }
        Apu w;
        CHECK(w.startWavRecord(wavPath)); // probe opens with app: existing bytes kept
        w.stopWavRecord();                // 0 samples: early return, no header write
        std::ifstream in(wavPath, std::ios::binary);
        const std::string kept((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(kept == "PREEXISTING");
        std::remove(wavPath.c_str());
        // app+binary must still CREATE a missing file (probe succeeds).
        const std::string newPath = tempDir() + "\\test_core_new_probe.wav";
        std::remove(newPath.c_str());
        CHECK(w.startWavRecord(newPath));
        w.stopWavRecord();
        CHECK(std::ifstream(newPath, std::ios::binary).good());
        std::remove(newPath.c_str());
    }

    return g_core_fail;
}

int main() {
    run_test_cpu();
    run_test_asm();
    run_test_platform();
    run_test_core();

    std::printf("test_cpu:      %d passed, %d failed\n", g_cpu_pass, g_cpu_fail);
    std::printf("test_asm:      %d passed, %d failed\n", g_asm_pass, g_asm_fail);
    std::printf("test_platform: %d passed, %d failed\n", g_plat_pass, g_plat_fail);
    std::printf("test_core:     %d passed, %d failed\n", g_core_pass, g_core_fail);

    const int totalFails = g_cpu_fail + g_asm_fail + g_plat_fail + g_core_fail;
    const int totalPass = g_cpu_pass + g_asm_pass + g_plat_pass + g_core_pass;
    if (totalFails != 0) {
        std::printf("FAILED: %d of %d checks\n", totalFails, totalPass + totalFails);
        return 1;
    }
    std::printf("All %d checks passed\n", totalPass);
    return 0;
}
