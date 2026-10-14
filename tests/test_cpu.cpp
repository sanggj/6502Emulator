// CPU core unit tests: flags, decimal mode, branches, stack, interrupts,
// cycle counts, disassembly helpers.
#include "cpu.hpp"
#include "memory.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

int g_cpu_pass = 0;
int g_cpu_fail = 0;

#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (expr) {                                                                          \
            ++g_cpu_pass;                                                                    \
        } else {                                                                             \
            ++g_cpu_fail;                                                                    \
            std::fprintf(stderr, "FAIL test_cpu.cpp:%d %s\n", __LINE__, #expr);              \
        }                                                                                    \
    } while (0)

namespace {

void poke(Memory &m, uint16_t at, std::initializer_list<uint8_t> bytes) {
    uint16_t a = at;
    for (uint8_t b : bytes) m.write(a++, b);
}

int exec(CPU &c, uint16_t at) {
    c.PC = at;
    return c.step();
}

bool flag(const CPU &c, uint8_t f) { return (c.P & f) != 0; }

} // namespace

int run_test_cpu() {
    Memory mem;
    CPU cpu(mem);
    const CPU::Core NMOS = CPU::Core::NMOS6502;
    const CPU::Core C02 = CPU::Core::WDC65C02;

    // ---- reset vector fetch ----
    mem.reset(0x00);
    mem.write(0xFFFC, 0x34);
    mem.write(0xFFFD, 0x12);
    cpu.reset();
    CHECK(cpu.PC == 0x1234);
    CHECK(cpu.SP == 0xFD);
    CHECK(flag(cpu, FLAG_I));

    // ---- LDA flags ----
    mem.reset(0x00);
    cpu.setCore(NMOS);
    poke(mem, 0x8000, {0xA9, 0x00});
    cpu.P = FLAG_U;
    cpu.A = 0xFF;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x00);
    CHECK(flag(cpu, FLAG_Z));
    CHECK(!flag(cpu, FLAG_N));
    poke(mem, 0x8000, {0xA9, 0x80});
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x80);
    CHECK(flag(cpu, FLAG_N));
    CHECK(!flag(cpu, FLAG_Z));
    CHECK(!flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_V));

    // ---- ADC ----
    poke(mem, 0x8000, {0x69, 0x50});
    cpu.A = 0x50;
    cpu.P = FLAG_U; // C=0, D=0
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0xA0);
    CHECK(flag(cpu, FLAG_V));
    CHECK(flag(cpu, FLAG_N));
    CHECK(!flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_Z));

    poke(mem, 0x8000, {0x69, 0x01});
    cpu.A = 0xFF;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x00);
    CHECK(flag(cpu, FLAG_C));
    CHECK(flag(cpu, FLAG_Z));
    CHECK(!flag(cpu, FLAG_V));
    CHECK(!flag(cpu, FLAG_N));

    poke(mem, 0x8100, {0x38}); // SEC
    exec(cpu, 0x8100);
    CHECK(flag(cpu, FLAG_C));
    poke(mem, 0x8100, {0x69, 0x00});
    cpu.A = 0x00;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0x01);
    CHECK(!flag(cpu, FLAG_C)); // carry consumed

    // ---- SBC ----
    poke(mem, 0x8100, {0x38});
    exec(cpu, 0x8100);
    poke(mem, 0x8100, {0xE9, 0x01});
    cpu.A = 0x00;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0xFF);
    CHECK(!flag(cpu, FLAG_C)); // borrow occurred
    CHECK(flag(cpu, FLAG_N));
    CHECK(!flag(cpu, FLAG_Z));

    poke(mem, 0x8100, {0x38});
    exec(cpu, 0x8100);
    poke(mem, 0x8100, {0xE9, 0x01});
    cpu.A = 0x50;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0x4F);
    CHECK(flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_V));

    poke(mem, 0x8100, {0x38});
    exec(cpu, 0x8100);
    poke(mem, 0x8100, {0xE9, 0x01});
    cpu.A = 0x80;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0x7F);
    CHECK(flag(cpu, FLAG_V));
    CHECK(flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_N));

    // ---- AND / EOR / ORA ----
    poke(mem, 0x8000, {0x29, 0x3C});
    cpu.A = 0xF0;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x30);
    CHECK(!flag(cpu, FLAG_Z));
    CHECK(!flag(cpu, FLAG_N));
    poke(mem, 0x8000, {0x29, 0x00});
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x00);
    CHECK(flag(cpu, FLAG_Z));

    poke(mem, 0x8000, {0x49, 0x0F});
    cpu.A = 0xFF;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0xF0);
    CHECK(flag(cpu, FLAG_N));

    poke(mem, 0x8000, {0x09, 0x80});
    cpu.A = 0x00;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x80);
    CHECK(flag(cpu, FLAG_N));
    poke(mem, 0x8000, {0x09, 0x00});
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x80);
    CHECK(flag(cpu, FLAG_N));
    poke(mem, 0x8000, {0x29, 0xF0});
    cpu.A = 0x80;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x80);
    CHECK(!flag(cpu, FLAG_Z));
    CHECK(flag(cpu, FLAG_N));

    // ---- decimal mode: NMOS ----
    cpu.setCore(NMOS);
    cpu.setDecimalModeEnabled(true);
    poke(mem, 0x8000, {0x69, 0x01});
    cpu.A = 0x99;
    cpu.P = FLAG_U | FLAG_D;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x00); // BCD 99 + 01 = 100
    CHECK(flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_Z)); // NMOS: Z from binary result 0x9A
    CHECK(flag(cpu, FLAG_N));  // NMOS: N from binary result 0x9A

    poke(mem, 0x8100, {0x38}); // SEC (no borrow)
    cpu.P = FLAG_U | FLAG_D;
    exec(cpu, 0x8100);
    poke(mem, 0x8100, {0xE9, 0x90});
    cpu.A = 0x50;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0x60); // BCD 50 - 90 = 60 (borrow)
    CHECK(!flag(cpu, FLAG_C));
    CHECK(flag(cpu, FLAG_N));  // NMOS: N from binary result 0xC0
    CHECK(flag(cpu, FLAG_V));  // NMOS: V from binary result
    CHECK(!flag(cpu, FLAG_Z));

    // ---- decimal mode: 65C02 ----
    cpu.setCore(C02);
    poke(mem, 0x8000, {0x69, 0x01});
    cpu.A = 0x99;
    cpu.P = FLAG_U | FLAG_D;
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x00);
    CHECK(flag(cpu, FLAG_C));
    CHECK(flag(cpu, FLAG_Z)); // 65C02: Z from BCD result
    CHECK(!flag(cpu, FLAG_N)); // 65C02: N from BCD result

    poke(mem, 0x8100, {0x38});
    cpu.P = FLAG_U | FLAG_D;
    exec(cpu, 0x8100);
    poke(mem, 0x8100, {0xE9, 0x90});
    cpu.A = 0x50;
    exec(cpu, 0x8100);
    CHECK(cpu.A == 0x60);
    CHECK(!flag(cpu, FLAG_C));
    CHECK(!flag(cpu, FLAG_N)); // 65C02: N from BCD result 0x60
    CHECK(flag(cpu, FLAG_V)); // V is binary-derived on both cores (0x50-0x90)
    CHECK(!flag(cpu, FLAG_Z));

    // decimal disabled -> plain binary math
    cpu.setDecimalModeEnabled(false);
    poke(mem, 0x8000, {0x69, 0x01});
    cpu.A = 0x99;
    cpu.P = FLAG_U | FLAG_D; // D set but decimal engine off
    exec(cpu, 0x8000);
    CHECK(cpu.A == 0x9A);
    CHECK(!flag(cpu, FLAG_C));
    CHECK(flag(cpu, FLAG_N));
    cpu.setDecimalModeEnabled(true);

    // ---- 65C02 decimal mode takes one extra cycle (NMOS does not) ----
    poke(mem, 0x8040, {0x69, 0x01});
    cpu.setCore(C02);
    cpu.P = FLAG_U | FLAG_D;
    cpu.A = 0x01;
    CHECK(exec(cpu, 0x8040) == 3); // 2 base + 1 decimal penalty
    cpu.setCore(NMOS);
    cpu.P = FLAG_U | FLAG_D;
    cpu.A = 0x01;
    CHECK(exec(cpu, 0x8040) == 2); // NMOS: no decimal penalty
    cpu.setCore(NMOS);

    // ---- branch cycles: untaken / taken / page cross ----
    cpu.setCore(NMOS);
    poke(mem, 0x8100, {0xD0, 0x10});
    cpu.P = FLAG_U | FLAG_Z;
    exec(cpu, 0x8100);
    CHECK(cpu.getCycles() == 2);
    CHECK(cpu.PC == 0x8102);

    cpu.P = FLAG_U; // Z clear -> taken
    exec(cpu, 0x8100);
    CHECK(cpu.getCycles() == 3);
    CHECK(cpu.PC == 0x8112);

    poke(mem, 0x81FD, {0xD0, 0x01}); // target $8200 crosses page
    cpu.P = FLAG_U;
    exec(cpu, 0x81FD);
    CHECK(cpu.getCycles() == 4);
    CHECK(cpu.PC == 0x8200);

    // ---- JMP ($xxFF) page-wrap bug ----
    poke(mem, 0x8000, {0x6C, 0xFF, 0x12});
    mem.write(0x12FF, 0x00);
    mem.write(0x1300, 0x40); // correct target high byte ($4000)
    mem.write(0x1200, 0x34); // wrapped target high byte ($3400)
    cpu.setCore(NMOS);
    exec(cpu, 0x8000);
    CHECK(cpu.PC == 0x3400); // NMOS wraps within page
    cpu.setCore(C02);
    exec(cpu, 0x8000);
    CHECK(cpu.PC == 0x4000); // 65C02 reads $12FF/$1300
    cpu.setCore(NMOS);

    // ---- stack push/pop with SP wrap ----
    mem.reset(0x00);
    poke(mem, 0x8000, {0x48, 0x68}); // PHA, PLA
    cpu.SP = 0x00;
    cpu.A = 0x42;
    cpu.P = FLAG_U;
    exec(cpu, 0x8000);
    CHECK(cpu.SP == 0xFF);
    CHECK(mem.read(0x0100) == 0x42);
    exec(cpu, 0x8001);
    CHECK(cpu.SP == 0x00);
    CHECK(cpu.A == 0x42);
    CHECK(mem.read(0x0100) == 0x42);

    // ---- BRK / RTI ----
    mem.reset(0x00);
    poke(mem, 0x8000, {0x00, 0xEA}); // BRK + padding
    poke(mem, 0x9000, {0x40});       // RTI handler
    mem.write(0xFFFE, 0x00);
    mem.write(0xFFFF, 0x90);
    cpu.setCore(NMOS);
    cpu.SP = 0xFD;
    cpu.P = FLAG_U;
    int brkCycles = exec(cpu, 0x8000);
    CHECK(brkCycles == 7);
    CHECK(cpu.getCycles() == 7);
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.SP == 0xFA);
    CHECK(flag(cpu, FLAG_I));
    CHECK((mem.read(0x01FB) & FLAG_B) != 0); // B flag pushed
    CHECK(mem.read(0x01FC) == 0x02);         // return address $8002
    CHECK(mem.read(0x01FD) == 0x80);
    exec(cpu, 0x9000);
    CHECK(cpu.PC == 0x8002);
    CHECK(cpu.SP == 0xFD);
    CHECK(!flag(cpu, FLAG_I));
    // 65C02: BRK takes 8 cycles
    cpu.setCore(C02);
    cpu.SP = 0xFD;
    cpu.P = FLAG_U;
    cpu.PC = 0x8000;
    CHECK(cpu.step() == 8);
    cpu.setCore(NMOS);

    // ---- cycle counts + totalCycles accumulation ----
    mem.reset(0x00);
    poke(mem, 0x8200, {0xA9, 0x05});         // LDA #imm: 2
    poke(mem, 0x8300, {0x8D, 0x00, 0x02});   // STA abs: 4
    poke(mem, 0x8400, {0xE8});               // INX: 2
    poke(mem, 0x8401, {0xEA});               // NOP: 2
    poke(mem, 0x8500, {0x48});               // PHA: 3
    poke(mem, 0x8600, {0x20, 0x00, 0x90});   // JSR $9000: 6
    poke(mem, 0x9000, {0x60});               // RTS: 6

    uint64_t t0 = cpu.getTotalCycles();
    exec(cpu, 0x8200);
    CHECK(cpu.getCycles() == 2);
    CHECK(cpu.getTotalCycles() - t0 == 2);
    t0 = cpu.getTotalCycles();
    exec(cpu, 0x8300);
    CHECK(cpu.getCycles() == 4);
    CHECK(cpu.getTotalCycles() - t0 == 4);
    t0 = cpu.getTotalCycles();
    exec(cpu, 0x8400);
    CHECK(cpu.getCycles() == 2);
    CHECK(cpu.getTotalCycles() - t0 == 2);
    exec(cpu, 0x8401);
    CHECK(cpu.getCycles() == 2);
    exec(cpu, 0x8500);
    CHECK(cpu.getCycles() == 3);
    t0 = cpu.getTotalCycles();
    exec(cpu, 0x8600);
    CHECK(cpu.getCycles() == 6);
    CHECK(cpu.getTotalCycles() - t0 == 6);
    CHECK(cpu.PC == 0x9000);
    t0 = cpu.getTotalCycles();
    exec(cpu, 0x9000);
    CHECK(cpu.getCycles() == 6);
    CHECK(cpu.getTotalCycles() - t0 == 6);
    CHECK(cpu.PC == 0x8603);

    // ---- disassembly helpers ----
    mem.reset(0x00);
    poke(mem, 0x8400, {0xA9, 0x05});
    poke(mem, 0x8402, {0x8D, 0x00, 0x02});
    std::string d = cpu.disassembleOne(0x8400);
    CHECK(d.find("LDA") != std::string::npos);
    CHECK(d.find("$05") != std::string::npos);
    d = cpu.disassembleOne(0x8402);
    CHECK(d.find("STA") != std::string::npos);
    CHECK(std::string(CPU::opcodeName(0xA9)) == "LDA");
    CHECK(std::string(CPU::opcodeName(0x8D)) == "STA");
    CHECK(CPU::instructionLength(0xA9) == 2);
    CHECK(CPU::instructionLength(0xAD) == 3);
    CHECK(CPU::instructionLength(0xEA) == 1);
    CHECK(!CPU::isIllegal(0xA9));
    CHECK(!CPU::isIllegal(0xEA));
    CHECK(CPU::isIllegal(0x02));
    CHECK(CPU::isIllegal(0x03));

    return g_cpu_fail;
}
