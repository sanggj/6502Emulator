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

// Independent ADC/SBC reference computed from the specification, not from
// cpu.cpp: binary via plain integer math (V from slope/signed-range), and
// decimal via explicit nibble-wise BCD correction (low digit +6 if >9 with
// its carry feeding the high digit, high digit +6 if >9).  Validated over
// all 256x256x2 inputs for both cores; N/Z on 65C02 decimal come from the
// BCD result, on NMOS (and for V, always) from the binary result.
void refAdc(int a, int b, int cin, bool dec, bool c02, uint8_t &r, bool &C, bool &Z,
            bool &N, bool &V) {
    const int sum = a + b + cin;
    const uint8_t bin = static_cast<uint8_t>(sum & 0xFF);
    C = sum > 0xFF;
    Z = bin == 0;
    N = (bin & 0x80) != 0;
    V = (((a ^ bin) & (b ^ bin) & 0x80) != 0);
    r = bin;
    if (dec) {
        int lo = (a & 0x0F) + (b & 0x0F) + cin;
        if (lo > 9) lo += 6;
        int hi = (a >> 4) + (b >> 4) + (lo >> 4);
        if (hi > 9) hi += 6;
        r = static_cast<uint8_t>(((hi & 0x0F) << 4) | (lo & 0x0F));
        C = hi > 0x0F;
        if (c02) {
            Z = r == 0;
            N = (r & 0x80) != 0;
        }
    }
}

void refSbc(int a, int b, int cin, bool dec, bool c02, uint8_t &r, bool &C, bool &Z,
            bool &N, bool &V) {
    const int borrow = cin ? 0 : 1;
    const int diff = a - b - borrow;
    const uint8_t bin = static_cast<uint8_t>(diff & 0xFF);
    C = diff >= 0;
    Z = bin == 0;
    N = (bin & 0x80) != 0;
    const int sdiff = static_cast<int>(static_cast<int8_t>(static_cast<uint8_t>(a))) -
                      static_cast<int>(static_cast<int8_t>(static_cast<uint8_t>(b))) - borrow;
    V = sdiff < -128 || sdiff > 127; // signed overflow, not the XOR shortcut
    r = bin;
    if (dec) {
        int lo = (a & 0x0F) - (b & 0x0F) - borrow;
        int hi = (a >> 4) - (b >> 4);
        if (lo < 0) {
            lo += 10; // decimal borrow from the low digit
            hi -= 1;
        }
        if (hi < 0) hi += 10;
        r = static_cast<uint8_t>(((hi & 0x0F) << 4) | (lo & 0x0F));
        if (c02) {
            Z = r == 0;
            N = (r & 0x80) != 0;
        }
    }
}

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

    // ---- NMOS deferred-interrupt quirk: CLI/SEI/PLP I change hidden for one instruction ----
    mem.reset(0x00);
    cpu.setCore(NMOS);
    cpu.setDecimalModeEnabled(true);
    mem.write(0xFFFA, 0x00);
    mem.write(0xFFFB, 0xA0); // NMI -> $A000
    mem.write(0xFFFE, 0x00);
    mem.write(0xFFFF, 0x90); // IRQ -> $9000

    // (a) IRQ pending + I set: the instruction AFTER CLI runs before vectoring
    poke(mem, 0x8000, {0x58, 0xEA, 0xA9, 0x42}); // CLI; NOP; LDA #$42
    cpu.SP = 0xFD;
    cpu.P = FLAG_U | FLAG_I;
    cpu.A = 0x77;
    cpu.setIrqLine(true);
    uint64_t irqT0 = cpu.getTotalCycles();
    CHECK(exec(cpu, 0x8000) == 2); // CLI
    CHECK(cpu.PC == 0x8001);
    // the instruction after CLI executes (2), then the IRQ sequence (7)
    // follows it in the same boundary - no vectoring before the NOP
    CHECK(exec(cpu, 0x8001) == 9);
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.getCycles() == 9);
    CHECK(cpu.getTotalCycles() - irqT0 == 11); // 2 + 2 + 7
    CHECK(cpu.A == 0x77); // LDA #$42 at $8002 never ran
    CHECK(mem.read(0x01FD) == 0x80);           // pushed PC = $8002 (the un-run LDA)
    CHECK(mem.read(0x01FC) == 0x02);
    CHECK((mem.read(0x01FB) & FLAG_I) == 0);   // pushed P already shows CLI's I=0

    // (b) IRQ pending + I clear: SEI runs first, the IRQ vectors after it
    // (the poll saw the old I value)
    cpu.setIrqLine(false);
    poke(mem, 0x8100, {0x78}); // SEI
    cpu.SP = 0xFD;
    cpu.P = FLAG_U;
    cpu.setIrqLine(true);
    irqT0 = cpu.getTotalCycles();
    CHECK(exec(cpu, 0x8100) == 9); // SEI (2) + interrupt sequence (7)
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.getTotalCycles() - irqT0 == 9);
    CHECK((mem.read(0x01FB) & FLAG_I) != 0); // pushed P has I set by SEI
    CHECK(mem.read(0x01FC) == 0x01);         // pushed PC = $8101 (SEI executed)
    CHECK(mem.read(0x01FD) == 0x81);
    cpu.setIrqLine(false);

    // (d) RTI is in the deferred class with PLP: the restored I is hidden
    // from the poll for one instruction on NMOS.  Start I=0, RTI restores
    // I=1 with an IRQ pending -> the poll still sees the instruction-start
    // I=0 and vectors right after RTI (pushed P shows the restored I=1).
    poke(mem, 0x8500, {0x40}); // RTI
    mem.write(0x01FB, FLAG_U | FLAG_I); // restored P: I=1
    mem.write(0x01FC, 0x00);            // restored PCL
    mem.write(0x01FD, 0x86);            // restored PCH -> PC = $8600
    cpu.SP = 0xFA;
    cpu.P = FLAG_U; // I clear at instruction start
    cpu.setIrqLine(true);
    irqT0 = cpu.getTotalCycles();
    CHECK(exec(cpu, 0x8500) == 13); // RTI (6) + interrupt sequence (7)
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.getTotalCycles() - irqT0 == 13);
    CHECK((mem.read(0x01FB) & FLAG_I) != 0); // nested IRQ pushed the restored I=1
    CHECK(mem.read(0x01FC) == 0x00);         // pushed PC = $8600 (RTI's target)
    CHECK(mem.read(0x01FD) == 0x86);
    cpu.setIrqLine(false);

    // (e) RTI restoring I=0 with IRQ pending: the poll uses the start-of-
    // instruction I=1, so vectoring is delayed one instruction (same class
    // as CLI) - the next instruction runs before the IRQ.
    poke(mem, 0x8600, {0xA9, 0x42}); // LDA #$42
    mem.write(0x01FB, FLAG_U);       // restored P: I=0
    mem.write(0x01FC, 0x00);
    mem.write(0x01FD, 0x86); // restored PC = $8600
    cpu.SP = 0xFA;
    cpu.P = FLAG_U | FLAG_I; // I set at instruction start
    cpu.A = 0x77;
    cpu.setIrqLine(true);
    CHECK(exec(cpu, 0x8500) == 6); // RTI alone: no vectoring yet (deferred)
    CHECK(cpu.PC == 0x8600);
    CHECK(exec(cpu, 0x8600) == 9); // LDA (2) + IRQ (7)
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.A == 0x42); // the LDA ran before vectoring
    cpu.setIrqLine(false);

    // (c) NMI is not deferred and ignores I: edge serviced at the boundary
    poke(mem, 0x8300, {0xA9, 0x11}); // LDA #$11
    cpu.SP = 0xFD;
    cpu.P = FLAG_U | FLAG_I; // I set - must not block the NMI
    cpu.A = 0x77;
    cpu.setNmiLine(true);
    CHECK(exec(cpu, 0x8300) == 7); // serviced before the instruction
    CHECK(cpu.PC == 0xA000);
    CHECK(cpu.A == 0x77);
    CHECK(cpu.getCycles() == 7);
    CHECK(!cpu.nmiPending());
    cpu.setNmiLine(false);

    // 65C02 kept as-is: the CLI I change is effective immediately (WDC), so
    // the IRQ vectors right after CLI, before the following NOP.
    cpu.setCore(C02);
    poke(mem, 0x8340, {0x58, 0xEA}); // CLI; NOP
    cpu.SP = 0xFD;
    cpu.P = FLAG_U | FLAG_I;
    cpu.A = 0x77;
    cpu.setIrqLine(true);
    CHECK(exec(cpu, 0x8340) == 2); // CLI
    CHECK(exec(cpu, 0x8341) == 7); // IRQ vectors before the NOP (no NMOS delay)
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.A == 0x77);
    cpu.setIrqLine(false);

    // 65C02 RTI unchanged (WDC: the restored I takes effect for the very
    // next poll): with I=0 restored and IRQ pending, the IRQ preempts the
    // following instruction - no NMOS one-instruction delay.
    poke(mem, 0x8600, {0xA9, 0x42}); // LDA #$42
    mem.write(0x01FB, FLAG_U);       // restored P: I=0
    mem.write(0x01FC, 0x00);
    mem.write(0x01FD, 0x86); // restored PC = $8600
    cpu.SP = 0xFA;
    cpu.P = FLAG_U | FLAG_I;
    cpu.A = 0x77;
    cpu.setIrqLine(true);
    CHECK(exec(cpu, 0x8500) == 6); // RTI
    CHECK(exec(cpu, 0x8600) == 7); // IRQ preempts the LDA (poll before instruction)
    CHECK(cpu.PC == 0x9000);
    CHECK(cpu.A == 0x77); // LDA never ran
    cpu.setIrqLine(false);
    cpu.setCore(NMOS);

    // ---- RMW write count: NMOS double write, 65C02 single write ----
    poke(mem, 0x8400, {0xEE, 0x00, 0x03}); // INC $0300
    mem.write(0x0300, 0x41);
    cpu.P = FLAG_U;
    CHECK(exec(cpu, 0x8400) == 6);
    CHECK(mem.read(0x0300) == 0x42);
    CHECK(mem.lastWrites().size() == 2);
    CHECK(mem.lastWrites()[0].address == 0x0300);
    CHECK(mem.lastWrites()[0].oldValue == 0x41);
    CHECK(mem.lastWrites()[0].newValue == 0x41); // dummy write of the old value
    CHECK(mem.lastWrites()[1].address == 0x0300);
    CHECK(mem.lastWrites()[1].newValue == 0x42);
    mem.write(0x0300, 0x41);
    cpu.setCore(C02);
    CHECK(exec(cpu, 0x8400) == 6);
    CHECK(mem.read(0x0300) == 0x42);
    CHECK(mem.lastWrites().size() == 1); // 65C02: single write cycle
    CHECK(mem.lastWrites()[0].address == 0x0300);
    CHECK(mem.lastWrites()[0].oldValue == 0x41);
    CHECK(mem.lastWrites()[0].newValue == 0x42);
    cpu.setCore(NMOS);

    // ---- JAM halts with PC advanced and cycles charged ----
    poke(mem, 0x8500, {0x02});
    cpu.clearHalt();
    cpu.SP = 0xFD;
    cpu.P = FLAG_U;
    const uint64_t jamT0 = cpu.getTotalCycles();
    CHECK(exec(cpu, 0x8500) == 2); // fetch cycles charged (was 0)
    CHECK(cpu.halted());
    CHECK(cpu.PC == 0x8501); // PC advanced past the opcode
    CHECK(cpu.getCycles() == 2);
    CHECK(cpu.getTotalCycles() - jamT0 == 2);
    CHECK(cpu.step() == 0); // halted CPU: step()==0 stays the caller stop signal
    cpu.clearHalt();

    // ---- reset costs exactly 7 cycles ----
    mem.write(0xFFFC, 0x34);
    mem.write(0xFFFD, 0x12);
    cpu.reset();
    CHECK(cpu.PC == 0x1234);
    CHECK(cpu.getCycles() == 7);
    CHECK(cpu.getTotalCycles() == 7);

    // ---- isIllegal: core-aware overload; original signature unchanged ----
    CHECK(CPU::isIllegal(0x04)); // old call compiles, NMOS view
    CHECK(CPU::isIllegal(0x04, NMOS));
    CHECK(!CPU::isIllegal(0x04, C02)); // TSB zp
    CHECK(!CPU::isIllegal(0x12, C02)); // ORA (zp)
    CHECK(!CPU::isIllegal(0x34, C02)); // BIT zp,X
    CHECK(!CPU::isIllegal(0x64, C02)); // STZ zp
    CHECK(!CPU::isIllegal(0x89, C02)); // BIT #imm
    CHECK(!CPU::isIllegal(0xCB, C02)); // WAI
    CHECK(!CPU::isIllegal(0x80, C02)); // BRA
    CHECK(CPU::isIllegal(0x03, C02));  // SLO stays illegal everywhere
    CHECK(CPU::isIllegal(0x02, C02));  // JAM stays illegal everywhere

    // ---- exhaustive ADC/SBC: all 256x256x2 inputs per (core, mode, op),
    // checked against the independent reference above ----
    {
        const CPU::Core kCores[2] = {NMOS, C02};
        int adcFail[2][2] = {};
        int sbcFail[2][2] = {};
        long long refCases = 0;
        cpu.setDecimalModeEnabled(true);
        for (int ci = 0; ci < 2; ++ci) {
            cpu.setCore(kCores[ci]);
            for (int opi = 0; opi < 2; ++opi) {
                const bool isAdc = opi == 0;
                mem.write(0x8000, isAdc ? uint8_t(0x69) : uint8_t(0xE9));
                for (int b = 0; b < 256; ++b) {
                    mem.write(0x8001, static_cast<uint8_t>(b));
                    for (int a = 0; a < 256; ++a) {
                        for (int cin = 0; cin < 2; ++cin) {
                            for (int dec = 0; dec < 2; ++dec) {
                                cpu.A = static_cast<uint8_t>(a);
                                cpu.P = static_cast<uint8_t>(FLAG_U | (dec ? FLAG_D : 0) |
                                                             (cin ? FLAG_C : 0));
                                cpu.PC = 0x8000;
                                cpu.step();
                                uint8_t r;
                                bool C, Z, N, V;
                                if (isAdc)
                                    refAdc(a, b, cin, dec != 0, ci != 0, r, C, Z, N, V);
                                else
                                    refSbc(a, b, cin, dec != 0, ci != 0, r, C, Z, N, V);
                                ++refCases;
                                const bool ok = cpu.A == r && flag(cpu, FLAG_C) == C &&
                                                 flag(cpu, FLAG_Z) == Z && flag(cpu, FLAG_N) == N &&
                                                 flag(cpu, FLAG_V) == V;
                                if (!ok) {
                                    int &f = isAdc ? adcFail[ci][dec] : sbcFail[ci][dec];
                                    if (++f == 1)
                                        std::fprintf(stderr,
                                                     "first fail: core=%d %s dec=%d a=%02X b=%02X "
                                                     "cin=%d got %02X/%d%d%d%d want %02X/%d%d%d%d\n",
                                                     ci, isAdc ? "ADC" : "SBC", dec, a, b, cin,
                                                     cpu.A, flag(cpu, FLAG_C), flag(cpu, FLAG_Z),
                                                     flag(cpu, FLAG_N), flag(cpu, FLAG_V), r, C, Z, N, V);
                                }
                            }
                        }
                    }
                }
            }
        }
        cpu.setCore(NMOS);
        CHECK(refCases == 1048576LL);
        CHECK(adcFail[0][0] == 0); // NMOS binary
        CHECK(adcFail[0][1] == 0); // NMOS decimal
        CHECK(adcFail[1][0] == 0); // 65C02 binary
        CHECK(adcFail[1][1] == 0); // 65C02 decimal
        CHECK(sbcFail[0][0] == 0);
        CHECK(sbcFail[0][1] == 0);
        CHECK(sbcFail[1][0] == 0);
        CHECK(sbcFail[1][1] == 0);
    }

    // ---- cycle-count table across addressing modes ----
    cpu.SP = 0xFD;
    cpu.X = 0x10;
    cpu.Y = 0x10;
    poke(mem, 0x8600, {0xEA});             // implied
    CHECK(exec(cpu, 0x8600) == 2);
    poke(mem, 0x8600, {0xA9, 0x01});       // immediate
    CHECK(exec(cpu, 0x8600) == 2);
    poke(mem, 0x8600, {0xA5, 0x10});       // zero page
    CHECK(exec(cpu, 0x8600) == 3);
    poke(mem, 0x8600, {0xB5, 0x10});       // zero page,X
    CHECK(exec(cpu, 0x8600) == 4);
    poke(mem, 0x8600, {0xAD, 0x10, 0x03}); // absolute
    CHECK(exec(cpu, 0x8600) == 4);
    poke(mem, 0x8600, {0xBD, 0x00, 0x01}); // absolute,X, no page cross
    CHECK(exec(cpu, 0x8600) == 4);
    poke(mem, 0x8600, {0xBD, 0xF0, 0x01}); // absolute,X page cross (+1)
    CHECK(exec(cpu, 0x8600) == 5);
    poke(mem, 0x8600, {0xB9, 0x00, 0x01}); // absolute,Y, no page cross
    CHECK(exec(cpu, 0x8600) == 4);
    poke(mem, 0x8600, {0xB9, 0xF0, 0x01}); // absolute,Y page cross (+1)
    CHECK(exec(cpu, 0x8600) == 5);
    poke(mem, 0x8600, {0xA1, 0x30});       // (indirect,X)
    CHECK(exec(cpu, 0x8600) == 6);
    mem.write(0x0030, 0x00);
    mem.write(0x0031, 0x02);
    poke(mem, 0x8600, {0xB1, 0x30});       // (indirect),Y, no page cross
    CHECK(exec(cpu, 0x8600) == 5);
    mem.write(0x0030, 0xF0);
    mem.write(0x0031, 0x02);
    poke(mem, 0x8600, {0xB1, 0x30});       // (indirect),Y page cross (+1)
    CHECK(exec(cpu, 0x8600) == 6);
    poke(mem, 0x8600, {0x6C, 0x00, 0x03}); // JMP (indirect)
    CHECK(exec(cpu, 0x8600) == 5);
    poke(mem, 0x8600, {0x30, 0x10});       // branch untaken
    cpu.P = FLAG_U;
    CHECK(exec(cpu, 0x8600) == 2);
    poke(mem, 0x8600, {0x30, 0x10});       // branch taken
    cpu.P = FLAG_U | FLAG_N;
    CHECK(exec(cpu, 0x8600) == 3);
    poke(mem, 0x86FD, {0x30, 0x01});       // branch taken, page cross (+1)
    cpu.P = FLAG_U | FLAG_N;
    CHECK(exec(cpu, 0x86FD) == 4);
    poke(mem, 0x8600, {0xFE, 0x00, 0x02}); // INC abs,X: dummy read, fixed 7
    CHECK(exec(cpu, 0x8600) == 7);
    poke(mem, 0x8600, {0xDE, 0xF0, 0x01}); // DEC abs,X: dummy read, fixed 7
    CHECK(exec(cpu, 0x8600) == 7);
    poke(mem, 0x86E0, {0x68});             // PLA
    cpu.SP = 0x00;
    mem.write(0x0101, 0x55);
    CHECK(exec(cpu, 0x86E0) == 4);
    CHECK(cpu.SP == 0x01);
    CHECK(cpu.A == 0x55);
    poke(mem, 0x86F0, {0x40});             // RTI: 6 cycles + exact PC/P restore
    cpu.SP = 0xFC;
    mem.write(0x01FD, FLAG_U | FLAG_C);
    mem.write(0x01FE, 0x56);
    mem.write(0x01FF, 0x34);
    CHECK(exec(cpu, 0x86F0) == 6);
    CHECK(cpu.PC == 0x3456);
    CHECK(cpu.SP == 0xFF);
    CHECK(flag(cpu, FLAG_C));
    // JSR 6 / RTS 6 / PHA 3 / BRK 7 / BRK-65C02 8 asserted above (cycle
    // counts + totalCycles section and the BRK/RTI section).

    // ---- branch offsets: all 256 signed displacements assemble to target ----
    cpu.P = FLAG_U; // Z clear -> BNE always taken
    int branchBad = 0;
    for (int off = 0; off < 256; ++off) {
        mem.write(0x8700, 0xD0);
        mem.write(0x8701, static_cast<uint8_t>(off));
        cpu.PC = 0x8700;
        cpu.step();
        const uint16_t want =
            static_cast<uint16_t>(0x8702 + off - ((off & 0x80) ? 0x100 : 0));
        if (cpu.PC != want) ++branchBad;
    }
    CHECK(branchBad == 0);
    mem.write(0x8700, 0xD0);
    mem.write(0x8701, 0x80); // -128 boundary
    cpu.PC = 0x8700;
    cpu.step();
    CHECK(cpu.PC == 0x8682);
    mem.write(0x8701, 0x7F); // +127 boundary
    cpu.PC = 0x8700;
    cpu.step();
    CHECK(cpu.PC == 0x8781);

    // ---- stack: PHP/PLP wrap SP $00 -> $FF across the $0100 page ----
    poke(mem, 0x8790, {0x08, 0x28}); // PHP; PLP
    cpu.SP = 0x00;
    cpu.P = FLAG_U | FLAG_C | FLAG_N;
    CHECK(exec(cpu, 0x8790) == 3);
    CHECK(cpu.SP == 0xFF); // wrapped to the top of the page
    CHECK((mem.read(0x0100) & FLAG_B) != 0); // PHP pushed B set
    CHECK((mem.read(0x0100) & FLAG_C) != 0);
    CHECK(exec(cpu, 0x8791) == 4);
    CHECK(cpu.SP == 0x00);
    CHECK(flag(cpu, FLAG_C));
    CHECK(flag(cpu, FLAG_N));

    // ---- NMI vs IRQ priority: both pending, NMI wins ----
    mem.write(0xFFFA, 0x00);
    mem.write(0xFFFB, 0xA0); // NMI -> $A000
    mem.write(0xFFFE, 0x00);
    mem.write(0xFFFF, 0x90); // IRQ -> $9000
    poke(mem, 0x87A0, {0xEA}); // NOP
    cpu.SP = 0xFD;
    cpu.P = FLAG_U; // I clear, so the IRQ would be serviceable
    cpu.A = 0x77;
    cpu.setNmiLine(true);
    cpu.setIrqLine(true);
    CHECK(exec(cpu, 0x87A0) == 7);
    CHECK(cpu.PC == 0xA000); // NMI vector, not $9000
    CHECK(cpu.A == 0x77);    // no instruction executed
    CHECK(!cpu.nmiPending());
    cpu.setNmiLine(false);
    cpu.setIrqLine(false);

    return g_cpu_fail;
}
