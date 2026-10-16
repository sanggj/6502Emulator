#include "cpu.hpp"
#include "assembler.hpp"
#include <array>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

enum : uint8_t {
    ILL_SLO,
    ILL_RLA,
    ILL_SRE,
    ILL_RRA,
    ILL_SAX,
    ILL_LAX,
    ILL_DCP,
    ILL_ISC,
    ILL_NOP,
    ILL_ANC,
    ILL_ALR,
    ILL_ARR,
    ILL_SBX,
    ILL_XAA,
    ILL_JAM,
    ILL_SHY,
    ILL_SHX,
    ILL_SHA,
    ILL_TAS,
    ILL_LAS
};
enum : uint8_t { M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY, M_IMP };

struct IllegalInfo {
    uint8_t kind;
    uint8_t mode;
    uint8_t base;
    bool page;
};

const std::array<IllegalInfo, 256> kIllegal = [] {
    std::array<IllegalInfo, 256> t{};
    for (auto &e : t) {
        e.kind = 0xFF;
        e.mode = M_IMP;
        e.base = 0;
        e.page = false;
    }
    auto set = [&](std::initializer_list<uint16_t> ops, uint8_t kind, uint8_t mode, uint8_t base, bool page) {
        for (uint16_t op : ops) t[op] = {kind, mode, base, page};
    };
    const uint8_t rmwModes[7][2] = {{M_IZX, 8}, {M_ZP, 5},  {M_ABS, 6}, {M_IZY, 8},
                                    {M_ZPX, 6}, {M_ABY, 7}, {M_ABX, 7}};
    using L = std::initializer_list<uint16_t>;
    const L slo = {0x03, 0x07, 0x0F, 0x13, 0x17, 0x1B, 0x1F};
    const L rla = {0x23, 0x27, 0x2F, 0x33, 0x37, 0x3B, 0x3F};
    const L sre = {0x43, 0x47, 0x4F, 0x53, 0x57, 0x5B, 0x5F};
    const L rra = {0x63, 0x67, 0x6F, 0x73, 0x77, 0x7B, 0x7F};
    const L dcp = {0xC3, 0xC7, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF};
    const L isc = {0xE3, 0xE7, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF};
    const uint8_t kinds[6] = {ILL_SLO, ILL_RLA, ILL_SRE, ILL_RRA, ILL_DCP, ILL_ISC};
    const L *lists[6] = {&slo, &rla, &sre, &rra, &dcp, &isc};
    for (int g = 0; g < 6; ++g) {
        int i = 0;
        for (uint16_t op : *lists[g]) {
            set({op}, kinds[g], rmwModes[i][0], rmwModes[i][1], false);
            ++i;
        }
    }
    set({0x83}, ILL_SAX, M_IZX, 6, false);
    set({0x87}, ILL_SAX, M_ZP, 3, false);
    set({0x8F}, ILL_SAX, M_ABS, 4, false);
    set({0x97}, ILL_SAX, M_ZPY, 4, false);
    set({0xA3}, ILL_LAX, M_IZX, 6, false);
    set({0xA7}, ILL_LAX, M_ZP, 3, false);
    set({0xAF}, ILL_LAX, M_ABS, 4, false);
    set({0xB3}, ILL_LAX, M_IZY, 5, true);
    set({0xB7}, ILL_LAX, M_ZPY, 4, false);
    set({0xBF}, ILL_LAX, M_ABY, 4, true);
    set({0xAB}, ILL_LAX, M_IMM, 2, false);
    set({0x0B, 0x2B}, ILL_ANC, M_IMM, 2, false);
    set({0x4B}, ILL_ALR, M_IMM, 2, false);
    set({0x6B}, ILL_ARR, M_IMM, 2, false);
    set({0xCB}, ILL_SBX, M_IMM, 2, false);
    set({0x8B}, ILL_XAA, M_IMM, 2, false);
    set({0x9C}, ILL_SHY, M_ABX, 5, false);
    set({0x9E}, ILL_SHX, M_ABY, 5, false);
    set({0x93}, ILL_SHA, M_IZY, 6, false);
    set({0x9F}, ILL_SHA, M_ABY, 5, false);
    set({0x9B}, ILL_TAS, M_ABY, 5, false);
    set({0xBB}, ILL_LAS, M_ABY, 4, true);
    set({0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2}, ILL_JAM, M_IMP, 2, false);
    set({0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA}, ILL_NOP, M_IMP, 2, false);
    set({0x80, 0x82, 0xC2, 0xE2, 0x89}, ILL_NOP, M_IMM, 2, false);
    set({0x04, 0x44, 0x64}, ILL_NOP, M_ZP, 3, false);
    set({0x14, 0x34, 0x54, 0x74, 0xD4, 0xF4}, ILL_NOP, M_ZPX, 4, false);
    set({0x0C}, ILL_NOP, M_ABS, 4, false);
    set({0x1C, 0x3C, 0x5C, 0x7C, 0xDC, 0xFC}, ILL_NOP, M_ABX, 4, true);
    return t;
}();

const Assembler::OpcodeInfo *findOpcodeByByte(uint8_t op) {
    const std::size_t n = Assembler::opcodeTableSize();
    const Assembler::OpcodeInfo *table = Assembler::opcodeTable();
    for (std::size_t i = 0; i < n; ++i)
        if (table[i].opcode == op) return &table[i];
    return nullptr;
}

} // namespace

CPU::CPU(Memory &memory)
    : A(0), X(0), Y(0), PC(0), SP(0xFD), P(0x24), mem(memory), cycles(0), lastCycles(0), totalCycles(0),
      instructionCount(0), lastOpcodeValue(0), haltedFlag(false), decimalModeEnabled(true) {
    decodeCache_.resize(Memory::SIZE);
}

CPU::~CPU() {}

void CPU::setCore(Core core) noexcept {
    coreVariant_ = core;
    for (auto &d : decodeCache_) d.valid = false;
}

void CPU::setNmiLine(bool asserted) noexcept {
    if (asserted && !nmiLine_) nmiPending_ = true; // edge detect
    nmiLine_ = asserted;
}

void CPU::setIrqLine(bool asserted) noexcept {
    irqLine_ = asserted;
}

void CPU::irq() {
    setIrqLine(true);
}
void CPU::nmi() {
    setNmiLine(true);
}
void CPU::setFlag(uint8_t flag, bool set) {
    if (set)
        P |= flag;
    else
        P &= ~flag;
}

bool CPU::getFlag(uint8_t flag) const {
    return (P & flag) != 0;
}

uint8_t CPU::readByte(uint16_t addr) {
    return mem.read(addr);
}

void CPU::writeByte(uint16_t addr, uint8_t val) {
    mem.write(addr, val);
}

void CPU::writeRmwByte(uint16_t addr, uint8_t newVal) {
    if (coreVariant_ == Core::WDC65C02) {
        // WDC 65C02: RMW instructions issue a single write cycle (NMOS
        // writes the old value first, then the new one).
        writeByte(addr, newVal);
        return;
    }
    mem.writeRmw(addr, rmwOld_, newVal);
}

uint8_t CPU::dummyReadThen(uint16_t unfixedAddr, uint16_t finalAddr) {
    if (unfixedAddr != finalAddr) readByte(unfixedAddr); // visible to I/O devices
    return readByte(finalAddr);
}

uint8_t CPU::readIndexedWithDummy(uint16_t base, uint16_t indexed) {
    const bool crossed = ((base ^ indexed) & 0xFF00u) != 0;
    return crossed ? dummyReadThen(static_cast<uint16_t>((base & 0xFF00u) | (indexed & 0xFFu)), indexed)
                   : readByte(indexed);
}

uint16_t CPU::readWord(uint16_t addr) {
    uint16_t lo = readByte(addr);
    uint16_t hi = readByte(static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint16_t CPU::readWordZeroPage(uint8_t addr) {
    const uint8_t next = static_cast<uint8_t>(addr + 1u);
    return static_cast<uint16_t>(readByte(addr) | (static_cast<uint16_t>(readByte(next)) << 8));
}

uint16_t CPU::readWordJmpBug(uint16_t addr) {
    const uint16_t hiAddr = static_cast<uint16_t>((addr & 0xFF00u) | ((addr + 1u) & 0x00FFu));
    return static_cast<uint16_t>((readByte(hiAddr) << 8) | readByte(addr));
}

uint8_t CPU::readPC() {
    uint8_t val = readByte(PC);
    PC++;
    return val;
}

uint16_t CPU::readPC16() {
    uint16_t lo = readByte(PC);
    PC++;
    uint16_t hi = readByte(PC);
    PC++;
    return (hi << 8) | lo;
}

void CPU::reset() {
    PC = readWord(0xFFFC);
    SP = 0xFD;
    P = 0x24;
    A = X = Y = 0;
    cycles = 0;
    lastCycles = 0;
    totalCycles = 0;
    instructionCount = 0;
    lastOpcodeValue = 0;
    haltedFlag = false;
    nmiLine_ = false;
    irqLine_ = false;
    nmiPending_ = false;
    suppressPollOnce_ = false;
    waitingForInterrupt_ = false;
    waiIrqBypass_ = false;
    for (auto &d : decodeCache_) d.valid = false;
    P |= FLAG_U;
    // Reset sequence costs 7 cycles on the real bus (RMI/RES fetch).
    lastCycles = 7;
    totalCycles = 7;
}

void CPU::serviceInterrupt(bool fromNmi) {
    // Hardware sequence: push PCH, PCL, P (with B cleared), then fetch the
    // vector LAST. If an NMI edge is detected during the pushes, an IRQ
    // (or software BRK) sequence is "hijacked" and vectors through $FFFA.
    writeByte(0x0100 + SP, static_cast<uint8_t>((PC >> 8) & 0xFF));
    SP--;
    writeByte(0x0100 + SP, static_cast<uint8_t>(PC & 0xFF));
    SP--;
    const uint8_t pushedP = static_cast<uint8_t>((P & ~FLAG_B) | FLAG_U);
    writeByte(0x0100 + SP, pushedP);
    SP--;
    setFlag(FLAG_I, true);
    uint16_t vector = 0xFFFE;
    if (fromNmi) {
        nmiPending_ = false;
        vector = 0xFFFA;
    } else if (nmiPending_) {
        nmiPending_ = false; // hijack: IRQ sequence taken over by NMI
        vector = 0xFFFA;
    }
    waiIrqBypass_ = false;
    PC = readWord(vector);
    lastCycles = 7;
    totalCycles += 7;
}

// Predecoded instruction cache: decode the instruction at pc once and reuse
// the operand bytes until one of the touched 64-byte pages is written.
const CPU::Decoded &CPU::decodedAt(uint16_t pc) {
    Decoded &d = decodeCache_[pc];
    const uint64_t lo = mem.pageEpoch(pc);
    const uint64_t hi = mem.pageEpoch(static_cast<uint16_t>((pc + 2) & 0xFFFF));
    if (d.valid && d.pageEpochLo == lo && d.pageEpochHi == hi) return d;
    // Guard against pathological epoch aliasing with periodic full refresh.
    if (++epochGuard_ == 0u)
        for (auto &e : decodeCache_) e.valid = false;
    d.opcode = mem.read(pc);
    d.length = effectiveLength(d.opcode);
    d.b[0] = d.length > 1 ? mem.read(static_cast<uint16_t>(pc + 1)) : 0;
    d.b[1] = d.length > 2 ? mem.read(static_cast<uint16_t>(pc + 2)) : 0;
    d.b[2] = 0;
    d.pageEpochLo = lo;
    d.pageEpochHi = hi;
    d.valid = true;
    return d;
}

uint8_t CPU::pc8() {
    if (operandCursor_ < decoded_.length - 1) {
        const uint8_t v = decoded_.b[operandCursor_++];
        ++PC;
        return v;
    }
    return CPU::readPC();
}

uint16_t CPU::pc16() {
    const uint8_t lo = pc8();
    const uint8_t hi = pc8();
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

int CPU::step() {
    if (haltedFlag) return 0; // halted CPU executes nothing; step()==0 stays the caller stop signal
    // 65C02 WAI: idle at 2 cycles per poll until a line wakes the core.
    // An IRQ that wakes WAI is serviced even with I=1 (WDC documentation).
    if (waitingForInterrupt_) {
        lastCycles = 2;
        totalCycles += 2;
        if (!interruptWakeup()) return 2;
        waitingForInterrupt_ = false;
        const bool fromNmi = nmiPending_;
        serviceInterrupt(fromNmi);
        return 7;
    }

    // Interrupt polling happens at the instruction boundary. A taken branch
    // discards the poll result (NMOS bug), deferring the interrupt by one
    // instruction; suppressPollOnce_ models that delay.
    const bool pollDiscarded = suppressPollOnce_ && coreVariant_ == Core::NMOS6502;
    suppressPollOnce_ = false;
    if (!pollDiscarded && nmiPending_) {
        ++instructionCount;
        serviceInterrupt(true);
        return 7;
    }
    // 65C02 polls IRQ here with the current I (WDC: an I change from CLI
    // takes effect immediately).  NMOS polls after the instruction instead,
    // so CLI/SEI/PLP I changes are hidden from the poller for one
    // instruction (NMOS deferred-interrupt quirk) - see pollIrqAtBoundary.
    if (coreVariant_ != Core::NMOS6502 && !pollDiscarded && irqLine_ && (P & FLAG_I) == 0) {
        ++instructionCount;
        serviceInterrupt(false);
        return 7;
    }
    const bool iAtStart = (P & FLAG_I) != 0; // snapshot for the end-of-instruction IRQ poll

    mem.beginWriteTrace();
    decimalC02Penalty_ = false;
    int cyclesUsed = 0;
    decoded_ = decodedAt(PC);
    operandCursor_ = 0;
    rmwOld_ = 0;
    uint8_t opcode = readByte(PC);
    lastOpcodeValue = opcode;
    PC++;
    uint16_t addr;
    uint8_t val;
    uint8_t reg;
    uint16_t temp;
    uint16_t base;
    uint16_t effective;
    uint8_t newVal; // declare at top of function

    // WDC 65C02 extensions (INI toggle / setCore): handled before the NMOS
    // decoder so re-encoded opcodes (e.g. $80 BRA vs NOP #imm) take priority.
    if (coreVariant_ == Core::WDC65C02) {
        const int handled = stepC02(opcode);
        if (handled >= 0) {
            P |= FLAG_U;
            ++instructionCount;
            lastCycles = handled;
            totalCycles += static_cast<uint64_t>(handled);
            return handled;
        }
    }

    switch (opcode) {
    // ---- ADC ----
    case 0x69: // Immediate
        val = pc8();
        A = adc(A, val);
        cyclesUsed += 2;
        break;
    case 0x65: // Zero Page
        addr = pc8();
        val = readByte(addr);
        A = adc(A, val);
        cyclesUsed += 3;
        break;
    case 0x75: // Zero Page X
        addr = pc8() + X;
        addr &= 0xFF;
        val = readByte(addr);
        A = adc(A, val);
        cyclesUsed += 4;
        break;
    case 0x6D: // Absolute
        addr = pc16();
        val = readByte(addr);
        A = adc(A, val);
        cyclesUsed += 4;
        break;
    case 0x7D: // Absolute X
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        val = readIndexedWithDummy(base, addr);
        A = adc(A, val);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x79: // Absolute Y
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        val = readIndexedWithDummy(base, addr);
        A = adc(A, val);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x61: // Indexed Indirect
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        val = readByte(addr);
        A = adc(A, val);
        cyclesUsed += 6;
        break;
    case 0x71: // Indirect Indexed
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        val = readIndexedWithDummy(addr, effective);
        A = adc(A, val);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- AND ----
    case 0x29:
        val = pc8();
        A &= val;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0x25:
        addr = pc8();
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 3;
        break;
    case 0x35:
        addr = pc8() + X;
        addr &= 0xFF;
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x2D:
        addr = pc16();
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x3D:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x39:
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x21:
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        A &= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 6;
        break;
    case 0x31:
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        A &= readByte(effective);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- ASL ----
    case 0x0A: // Accumulator
        setFlag(FLAG_C, A & 0x80);
        A <<= 1;
        A &= 0xFF;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0x06:
        addr = pc8();
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x80);
        val <<= 1;
        val &= 0xFF;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 5;
        break;
    case 0x16:
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x80);
        val <<= 1;
        val &= 0xFF;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0x0E:
        addr = pc16();
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x80);
        val <<= 1;
        val &= 0xFF;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0x1E:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x80);
        val <<= 1;
        val &= 0xFF;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 7;
        break;
        // ---- BCC ----
    case 0x90:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (!getFlag(FLAG_C)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BCS ----
    case 0xB0:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (getFlag(FLAG_C)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BEQ ----
    case 0xF0:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (getFlag(FLAG_Z)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BIT ----
    case 0x24:
        addr = pc8();
        val = readByte(addr);
        setFlag(FLAG_Z, (A & val) == 0);
        setFlag(FLAG_N, val & 0x80);
        setFlag(FLAG_V, val & 0x40);
        cyclesUsed += 3;
        break;
    case 0x2C:
        addr = pc16();
        val = readByte(addr);
        setFlag(FLAG_Z, (A & val) == 0);
        setFlag(FLAG_N, val & 0x80);
        setFlag(FLAG_V, val & 0x40);
        cyclesUsed += 4;
        break;
        // ---- BMI ----
    case 0x30:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (getFlag(FLAG_N)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BNE ----
    case 0xD0:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (!getFlag(FLAG_Z)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BPL ----
    case 0x10:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (!getFlag(FLAG_N)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BRK ----
    case 0x00:
        PC++; // skip padding
        writeByte(0x0100 + SP, (PC >> 8) & 0xFF);
        SP--;
        writeByte(0x0100 + SP, PC & 0xFF);
        SP--;
        writeByte(0x0100 + SP, static_cast<uint8_t>(P | FLAG_B | FLAG_U));
        SP--;
        setFlag(FLAG_I, true);
        // NMI hijack: an NMI edge detected during the BRK pushes redirects
        // the vector fetch to $FFFA (B flag remains pushed as set).
        if (nmiPending_) {
            nmiPending_ = false;
            PC = readWord(0xFFFA);
        } else
            PC = readWord(0xFFFE);
        // BRK: 8 cycles on 65C02, 7 on NMOS.
        cyclesUsed += (coreVariant_ == Core::WDC65C02) ? 8 : 7;
        break;
        // ---- BVC ----
    case 0x50:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (!getFlag(FLAG_V)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- BVS ----
    case 0x70:
        temp = static_cast<uint16_t>(static_cast<int8_t>(pc8()));
        cyclesUsed = 2;
        if (getFlag(FLAG_V)) {
            const uint16_t before = PC;
            PC = static_cast<uint16_t>(PC + temp);
            // taken: +1 cycle, +1 more when the target crosses a page
            cyclesUsed += ((before & 0xFF00u) != (PC & 0xFF00u)) ? 2 : 1;
            // NMOS quirk: a taken branch's interrupt poll is discarded,
            // so the following instruction executes before servicing.
            if (coreVariant_ == Core::NMOS6502) suppressPollOnce_ = true;
        }
        break;
        // ---- CLC ----
    case 0x18:
        setFlag(FLAG_C, false);
        cyclesUsed += 2;
        break;
        // ---- CLD ----
    case 0xD8:
        setFlag(FLAG_D, false);
        cyclesUsed += 2;
        break;
        // ---- CLI ----
    case 0x58:
        setFlag(FLAG_I, false);
        cyclesUsed += 2;
        break;
        // ---- CLV ----
    case 0xB8:
        setFlag(FLAG_V, false);
        cyclesUsed += 2;
        break;
        // ---- CMP ----
    case 0xC9:
        val = pc8();
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 2;
        break;
    case 0xC5:
        addr = pc8();
        val = readByte(addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 3;
        break;
    case 0xD5:
        addr = pc8() + X;
        addr &= 0xFF;
        val = readByte(addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 4;
        break;
    case 0xCD:
        addr = pc16();
        val = readByte(addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 4;
        break;
    case 0xDD:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        val = readIndexedWithDummy(base, addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xD9:
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        val = readIndexedWithDummy(base, addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xC1:
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        val = readByte(addr);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 6;
        break;
    case 0xD1:
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        val = readIndexedWithDummy(addr, effective);
        reg = A - val;
        setFlag(FLAG_C, A >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- CPX ----
    case 0xE0:
        val = pc8();
        reg = X - val;
        setFlag(FLAG_C, X >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 2;
        break;
    case 0xE4:
        addr = pc8();
        val = readByte(addr);
        reg = X - val;
        setFlag(FLAG_C, X >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 3;
        break;
    case 0xEC:
        addr = pc16();
        val = readByte(addr);
        reg = X - val;
        setFlag(FLAG_C, X >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 4;
        break;
        // ---- CPY ----
    case 0xC0:
        val = pc8();
        reg = Y - val;
        setFlag(FLAG_C, Y >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 2;
        break;
    case 0xC4:
        addr = pc8();
        val = readByte(addr);
        reg = Y - val;
        setFlag(FLAG_C, Y >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 3;
        break;
    case 0xCC:
        addr = pc16();
        val = readByte(addr);
        reg = Y - val;
        setFlag(FLAG_C, Y >= val);
        setFlag(FLAG_Z, reg == 0);
        setFlag(FLAG_N, reg & 0x80);
        cyclesUsed += 4;
        break;
        // ---- DEC ----
    case 0xC6:
        addr = pc8();
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ - 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 5;
        break;
    case 0xD6:
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ - 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0xCE:
        addr = pc16();
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ - 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0xDE:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ - 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 7;
        break;
        // ---- DEX ----
    case 0xCA:
        X--;
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 2;
        break;
        // ---- DEY ----
    case 0x88:
        Y--;
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 2;
        break;
        // ---- EOR ----
    case 0x49:
        val = pc8();
        A ^= val;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0x45:
        addr = pc8();
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 3;
        break;
    case 0x55:
        addr = pc8() + X;
        addr &= 0xFF;
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x4D:
        addr = pc16();
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x5D:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x59:
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x41:
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        A ^= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 6;
        break;
    case 0x51:
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        A ^= readByte(effective);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- INC ----
    case 0xE6:
        addr = pc8();
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ + 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 5;
        break;
    case 0xF6:
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ + 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0xEE:
        addr = pc16();
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ + 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 6;
        break;
    case 0xFE:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = readByte(addr);
        val = static_cast<uint8_t>(rmwOld_ + 1);
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, val & 0x80);
        cyclesUsed += 7;
        break;
        // ---- INX ----
    case 0xE8:
        X++;
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 2;
        break;
        // ---- INY ----
    case 0xC8:
        Y++;
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 2;
        break;
        // ---- JMP ----
    case 0x4C:
        PC = pc16();
        cyclesUsed += 3;
        break;
    case 0x6C:
        addr = pc16();
        // NMOS $xxFF page-wrap bug; 65C02 reads (addr+1)&0xFFFF.
        PC = (coreVariant_ == Core::WDC65C02) ? readWord(addr) : readWordJmpBug(addr);
        cyclesUsed += 5;
        break;
        // ---- JSR ----
    case 0x20:
        addr = pc16();
        PC--;
        writeByte(0x0100 + SP, (PC >> 8) & 0xFF);
        SP--;
        writeByte(0x0100 + SP, PC & 0xFF);
        SP--;
        PC = addr;
        cyclesUsed += 6;
        break;
        // ---- LDA ----
    // ... all modes covered above.

    // ---- LSR ----
    case 0x4A:
        setFlag(FLAG_C, A & 0x01);
        A >>= 1;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, false);
        cyclesUsed += 2;
        break;
    case 0x46:
        addr = pc8();
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x01);
        val >>= 1;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, false);
        cyclesUsed += 5;
        break;
    case 0x56:
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x01);
        val >>= 1;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, false);
        cyclesUsed += 6;
        break;
    case 0x4E:
        addr = pc16();
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x01);
        val >>= 1;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, false);
        cyclesUsed += 6;
        break;
    case 0x5E:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = val = readByte(addr);
        setFlag(FLAG_C, val & 0x01);
        val >>= 1;
        writeRmwByte(addr, val);
        setFlag(FLAG_Z, val == 0);
        setFlag(FLAG_N, false);
        cyclesUsed += 7;
        break;
        // ---- NOP ----
    case 0xEA:
        cyclesUsed += 2;
        break;
        // ---- ORA ----
    case 0x09:
        val = pc8();
        A |= val;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0x05:
        addr = pc8();
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 3;
        break;
    case 0x15:
        addr = pc8() + X;
        addr &= 0xFF;
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x0D:
        addr = pc16();
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x1D:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x19:
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0x01:
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        A |= readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 6;
        break;
    case 0x11:
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        A |= readByte(effective);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- PHA, PHP, PLA, PLP ----
    case 0x48: // PHA
        writeByte(0x0100 + SP, A);
        SP--;
        cyclesUsed += 3;
        break;
    case 0x68: // PLA
        SP++;
        A = readByte(0x0100 + SP);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0x08: // PHP
        writeByte(0x0100 + SP, static_cast<uint8_t>(P | FLAG_B | FLAG_U));
        SP--;
        cyclesUsed += 3;
        break;
    case 0x28: // PLP
        SP++;
        P = readByte(0x0100 + SP);
        P = static_cast<uint8_t>((P & ~FLAG_B) | FLAG_U);
        cyclesUsed += 4;
        break;
        // ---- ROL ----
    case 0x2A: {
        val = A;
        A = (A << 1) | (getFlag(FLAG_C) ? 1 : 0);
        setFlag(FLAG_C, val & 0x80);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    }
    case 0x26: {
        addr = pc8();
        rmwOld_ = val = readByte(addr);
        newVal = (val << 1) | (getFlag(FLAG_C) ? 1 : 0);
        setFlag(FLAG_C, val & 0x80);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 5;
        break;
    }
    case 0x36: {
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = val = readByte(addr);
        newVal = (val << 1) | (getFlag(FLAG_C) ? 1 : 0);
        setFlag(FLAG_C, val & 0x80);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 6;
        break;
    }
    case 0x2E: {
        addr = pc16();
        rmwOld_ = val = readByte(addr);
        newVal = (val << 1) | (getFlag(FLAG_C) ? 1 : 0);
        setFlag(FLAG_C, val & 0x80);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 6;
        break;
    }
    case 0x3E: {
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = val = readByte(addr);
        newVal = (val << 1) | (getFlag(FLAG_C) ? 1 : 0);
        setFlag(FLAG_C, val & 0x80);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 7;
        break;
    }

    // ---- ROR ----
    case 0x6A: {
        val = A;
        A = (A >> 1) | (getFlag(FLAG_C) ? 0x80 : 0);
        setFlag(FLAG_C, val & 0x01);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    }
    case 0x66: {
        addr = pc8();
        rmwOld_ = val = readByte(addr);
        newVal = (val >> 1) | (getFlag(FLAG_C) ? 0x80 : 0);
        setFlag(FLAG_C, val & 0x01);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 5;
        break;
    }
    case 0x76: {
        addr = pc8() + X;
        addr &= 0xFF;
        rmwOld_ = val = readByte(addr);
        newVal = (val >> 1) | (getFlag(FLAG_C) ? 0x80 : 0);
        setFlag(FLAG_C, val & 0x01);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 6;
        break;
    }
    case 0x6E: {
        addr = pc16();
        rmwOld_ = val = readByte(addr);
        newVal = (val >> 1) | (getFlag(FLAG_C) ? 0x80 : 0);
        setFlag(FLAG_C, val & 0x01);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 6;
        break;
    }
    case 0x7E: {
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy unfixed-address cycle
        rmwOld_ = val = readByte(addr);
        newVal = (val >> 1) | (getFlag(FLAG_C) ? 0x80 : 0);
        setFlag(FLAG_C, val & 0x01);
        writeRmwByte(addr, newVal);
        setFlag(FLAG_Z, newVal == 0);
        setFlag(FLAG_N, newVal & 0x80);
        cyclesUsed += 7;
        break;
    }

    // ---- RTI ----
    case 0x40:
        SP++;
        P = readByte(0x0100 + SP);
        P = static_cast<uint8_t>((P & ~FLAG_B) | FLAG_U);
        SP++;
        PC = readByte(0x0100 + SP);
        SP++;
        PC |= readByte(0x0100 + SP) << 8;
        cyclesUsed += 6;
        break;
        // ---- RTS ----
    case 0x60:
        SP++;
        PC = readByte(0x0100 + SP);
        SP++;
        PC |= readByte(0x0100 + SP) << 8;
        PC++;
        cyclesUsed += 6;
        break;
        // ---- SBC ----
    case 0xE9: // Immediate
        val = pc8();
        A = sbc(A, val);
        cyclesUsed += 2;
        break;
    case 0xE5:
        addr = pc8();
        val = readByte(addr);
        A = sbc(A, val);
        cyclesUsed += 3;
        break;
    case 0xF5:
        addr = pc8() + X;
        addr &= 0xFF;
        val = readByte(addr);
        A = sbc(A, val);
        cyclesUsed += 4;
        break;
    case 0xED:
        addr = pc16();
        val = readByte(addr);
        A = sbc(A, val);
        cyclesUsed += 4;
        break;
    case 0xFD:
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        val = readIndexedWithDummy(base, addr);
        A = sbc(A, val);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xF9:
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        val = readIndexedWithDummy(base, addr);
        A = sbc(A, val);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xE1:
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        val = readByte(addr);
        A = sbc(A, val);
        cyclesUsed += 6;
        break;
    case 0xF1:
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        val = readIndexedWithDummy(addr, effective);
        A = sbc(A, val);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- SEC ----
    case 0x38:
        setFlag(FLAG_C, true);
        cyclesUsed += 2;
        break;
        // ---- SED ----
    case 0xF8:
        setFlag(FLAG_D, true);
        cyclesUsed += 2;
        break;
        // ---- SEI ----
    case 0x78:
        setFlag(FLAG_I, true);
        cyclesUsed += 2;
        break;
        // ---- STA, STX, STY (all modes) ----
    // Already handled earlier, but we need to cover all modes.
    // Actually we have STA, STX, STY already but let's ensure they are present.
    // I'll add them here for completeness.
    // Actually they were not in the original switch - I need to add them.
    // Let's add them now.

    // ---- STA ----
    case 0x85: // Zero Page
        addr = pc8();
        writeByte(addr, A);
        cyclesUsed += 3;
        break;
    case 0x95: // Zero Page X
        addr = pc8() + X;
        addr &= 0xFF;
        writeByte(addr, A);
        cyclesUsed += 4;
        break;
    case 0x8D: // Absolute
        addr = pc16();
        writeByte(addr, A);
        cyclesUsed += 4;
        break;
    case 0x9D: // Absolute X
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy read cycle
        writeByte(addr, A);
        cyclesUsed += 5;
        break;
    case 0x99: // Absolute Y
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy read cycle
        writeByte(addr, A);
        cyclesUsed += 5;
        break;
    case 0x81: // Indexed Indirect
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        writeByte(addr, A);
        cyclesUsed += 6;
        break;
    case 0x91: // Indirect Indexed
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        {
            const uint16_t unfixed = static_cast<uint16_t>(addr + Y);
            readByte(unfixed);
            addr = unfixed;
        }
        writeByte(addr, A);
        cyclesUsed += 6;
        break;
        // ---- STX ----
    case 0x86: // Zero Page
        addr = pc8();
        writeByte(addr, X);
        cyclesUsed += 3;
        break;
    case 0x96: // Zero Page Y
        addr = pc8() + Y;
        addr &= 0xFF;
        writeByte(addr, X);
        cyclesUsed += 4;
        break;
    case 0x8E: // Absolute
        addr = pc16();
        writeByte(addr, X);
        cyclesUsed += 4;
        break;
        // ---- STY ----
    case 0x84: // Zero Page
        addr = pc8();
        writeByte(addr, Y);
        cyclesUsed += 3;
        break;
    case 0x94: // Zero Page X
        addr = pc8() + X;
        addr &= 0xFF;
        writeByte(addr, Y);
        cyclesUsed += 4;
        break;
    case 0x8C: // Absolute
        addr = pc16();
        writeByte(addr, Y);
        cyclesUsed += 4;
        break;
        // ---- LDA ----
    case 0xA9: // Immediate
        A = pc8();
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0xA5: // Zero Page
        addr = pc8();
        A = readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 3;
        break;
    case 0xB5: // Zero Page X
        addr = pc8() + X;
        addr &= 0xFF;
        A = readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0xAD: // Absolute
        addr = pc16();
        A = readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 4;
        break;
    case 0xBD: // Absolute X
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        A = readIndexedWithDummy(base, addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xB9: // Absolute Y
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        A = readIndexedWithDummy(base, addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
    case 0xA1: // Indexed Indirect
        addr = static_cast<uint8_t>(pc8() + X);
        addr = readWordZeroPage(static_cast<uint8_t>(addr));
        A = readByte(addr);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 6;
        break;
    case 0xB1: // Indirect Indexed
        base = pc8();
        addr = readWordZeroPage(static_cast<uint8_t>(base));
        effective = static_cast<uint16_t>(addr + Y);
        A = readByte(effective);
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += static_cast<int>(5 + ((addr & 0xFF00u) != (effective & 0xFF00u)));
        break;
        // ---- LDX ----
    case 0xA2: // Immediate
        X = pc8();
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 2;
        break;
    case 0xA6: // Zero Page
        addr = pc8();
        X = readByte(addr);
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 3;
        break;
    case 0xB6: // Zero Page Y
        addr = pc8() + Y;
        addr &= 0xFF;
        X = readByte(addr);
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 4;
        break;
    case 0xAE: // Absolute
        addr = pc16();
        X = readByte(addr);
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 4;
        break;
    case 0xBE: // Absolute Y
        base = pc16();
        addr = static_cast<uint16_t>(base + Y);
        X = readIndexedWithDummy(base, addr);
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
        // ---- LDY ----
    case 0xA0: // Immediate
        Y = pc8();
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 2;
        break;
    case 0xA4: // Zero Page
        addr = pc8();
        Y = readByte(addr);
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 3;
        break;
    case 0xB4: // Zero Page X
        addr = pc8() + X;
        addr &= 0xFF;
        Y = readByte(addr);
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 4;
        break;
    case 0xAC: // Absolute
        addr = pc16();
        Y = readByte(addr);
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 4;
        break;
    case 0xBC: // Absolute X
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        Y = readIndexedWithDummy(base, addr);
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += static_cast<int>(4 + ((base & 0xFF00u) != (addr & 0xFF00u)));
        break;
        // ---- TAX, TAY, TSX, TXA, TXS, TYA ----
    case 0xAA: // TAX
        X = A;
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 2;
        break;
    case 0xA8: // TAY
        Y = A;
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed += 2;
        break;
    case 0xBA: // TSX
        X = SP;
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed += 2;
        break;
    case 0x8A: // TXA
        A = X;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
    case 0x9A: // TXS
        SP = X;
        cyclesUsed += 2;
        break;
    case 0x98: // TYA
        A = Y;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed += 2;
        break;
        // ---- Unofficial / illegal opcodes ----
    default: {
        const IllegalInfo &il = kIllegal[opcode];
        if (il.kind == 0xFF || il.kind == ILL_JAM) {
            // JAM/unknown: PC has already advanced past the opcode (JAM is a
            // 1-byte, no-operand form per kIllegal/assembler). Charge the
            // fetch cycles and fall through to the common tail so stepping
            // reports progress instead of a 0-cycle stall.
            haltedFlag = true;
            cyclesUsed = il.base ? static_cast<int>(il.base) : 2;
            break;
        }
        bool page = false;
        switch (il.mode) {
        case M_IMM:
            val = pc8();
            break;
        case M_ZP:
            addr = pc8();
            break;
        case M_ZPX:
            addr = static_cast<uint16_t>((pc8() + X) & 0xFF);
            break;
        case M_ZPY:
            addr = static_cast<uint16_t>((pc8() + Y) & 0xFF);
            break;
        case M_ABS:
            addr = pc16();
            break;
        case M_ABX:
            base = pc16();
            addr = static_cast<uint16_t>(base + X);
            page = (base ^ addr) & 0xFF00;
            break;
        case M_ABY:
            base = pc16();
            addr = static_cast<uint16_t>(base + Y);
            page = (base ^ addr) & 0xFF00;
            break;
        case M_IZX: {
            const uint8_t zp = static_cast<uint8_t>(pc8() + X);
            addr = readWordZeroPage(zp);
        } break;
        case M_IZY: {
            const uint8_t zp = pc8();
            base = readWordZeroPage(zp);
            addr = static_cast<uint16_t>(base + Y);
            page = (base ^ addr) & 0xFF00;
        } break;
        default:
            break;
        }
        auto rdMem = [&]() -> uint8_t {
            rmwOld_ = (il.mode == M_IMM) ? val : readByte(addr);
            return rmwOld_;
        };
        auto wrMem = [&](uint8_t v, bool doubleWrite = false) {
            if (il.mode == M_IMM || il.mode == M_IMP) return;
            if (doubleWrite)
                writeRmwByte(addr, v); // RMW: old value written first
            else
                writeByte(addr, v);
        };
        switch (il.kind) {
        case ILL_SLO: {
            uint8_t v = rdMem();
            setFlag(FLAG_C, v & 0x80);
            v = static_cast<uint8_t>(v << 1);
            wrMem(v, true);
            A |= v;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        case ILL_RLA: {
            uint8_t v = rdMem();
            const uint8_t cIn = getFlag(FLAG_C) ? 1u : 0u;
            setFlag(FLAG_C, v & 0x80);
            v = static_cast<uint8_t>((v << 1) | cIn);
            wrMem(v, true);
            A &= v;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        case ILL_SRE: {
            uint8_t v = rdMem();
            setFlag(FLAG_C, v & 0x01);
            v = static_cast<uint8_t>(v >> 1);
            wrMem(v, true);
            A ^= v;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        case ILL_RRA: {
            uint8_t v = rdMem();
            const uint8_t cIn = getFlag(FLAG_C) ? 0x80u : 0u;
            const bool newC = (v & 0x01) != 0;
            v = static_cast<uint8_t>((v >> 1) | cIn);
            wrMem(v, true);
            A = adc(A, v);
            setFlag(FLAG_C, newC);
        } break;
        case ILL_SAX:
            wrMem(static_cast<uint8_t>(A & X));
            break;
        case ILL_LAX: {
            const uint8_t v = rdMem();
            A = v;
            X = v;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        case ILL_DCP: {
            uint8_t v = static_cast<uint8_t>(rdMem() - 1);
            wrMem(v, true);
            const uint16_t t2 = static_cast<uint16_t>(A) - static_cast<uint16_t>(v);
            setFlag(FLAG_C, t2 < 0x100);
            setFlag(FLAG_Z, (t2 & 0xFF) == 0);
            setFlag(FLAG_N, t2 & 0x80);
        } break;
        case ILL_ISC: {
            uint8_t v = static_cast<uint8_t>(rdMem() + 1);
            wrMem(v, true);
            A = sbc(A, v);
        } break;
        case ILL_NOP:
            break;
        case ILL_ANC:
            A &= val;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
            setFlag(FLAG_C, A & 0x80);
            break;
        case ILL_ALR:
            A &= val;
            setFlag(FLAG_C, A & 0x01);
            A = static_cast<uint8_t>(A >> 1);
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
            break;
        case ILL_ARR: {
            const uint8_t t3 = static_cast<uint8_t>(A & val);
            setFlag(FLAG_V, (((t3 >> 7) ^ ((t3 >> 6) & 1u)) != 0));
            A = static_cast<uint8_t>((t3 >> 1) | (getFlag(FLAG_C) ? 0x80 : 0));
            setFlag(FLAG_C, (t3 >> 6) & 1);
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        case ILL_SBX: {
            const uint16_t t4 = static_cast<uint16_t>(A & X) - static_cast<uint16_t>(val);
            X = static_cast<uint8_t>(t4 & 0xFF);
            setFlag(FLAG_C, t4 < 0x100);
            setFlag(FLAG_Z, X == 0);
            setFlag(FLAG_N, X & 0x80);
        } break;
        case ILL_XAA:
            A = static_cast<uint8_t>(X & val);
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
            break;
        case ILL_SHY:
            wrMem(static_cast<uint8_t>(Y & ((addr >> 8) + 1)));
            break;
        case ILL_SHX:
            wrMem(static_cast<uint8_t>(X & ((addr >> 8) + 1)));
            break;
        case ILL_SHA:
            wrMem(static_cast<uint8_t>((A & X) & ((addr >> 8) + 1)));
            break;
        case ILL_TAS:
            SP = static_cast<uint8_t>(A & X);
            wrMem(static_cast<uint8_t>((A & X) & ((addr >> 8) + 1)));
            break;
        case ILL_LAS: {
            const uint8_t v = static_cast<uint8_t>(rdMem() & SP);
            A = v;
            X = v;
            SP = v;
            setFlag(FLAG_Z, A == 0);
            setFlag(FLAG_N, A & 0x80);
        } break;
        default:
            break;
        }
        cyclesUsed += static_cast<int>(il.base) + (page ? 1 : 0);
        break;
    }
    }

    if (decimalC02Penalty_) {
        cyclesUsed += 1; // 65C02 decimal ADC/SBC penalty (set inside adc/sbc)
        decimalC02Penalty_ = false;
    }
    P |= FLAG_U;
    totalCycles += static_cast<uint64_t>(cyclesUsed);
    cyclesUsed += pollIrqAtBoundary(opcode, iAtStart); // may append the 7-cycle IRQ sequence
    lastCycles = cyclesUsed;
    return cyclesUsed;
}

// NMOS IRQ poll for the boundary after an instruction.  The I flag written
// by CLI/SEI/PLP/RTI is invisible to the interrupt poller for one
// instruction on NMOS silicon, so those four opcodes poll with the I value
// snapshotted at instruction start (RTI pulls P off the stack, same
// one-instruction delay class as PLP); every other opcode polls the current
// I (identical to the old poll-at-next-step behaviour).  65C02 keeps the
// pre-instruction poll in step() (WDC: RTI I takes effect immediately), so
// this runs on NMOS only.  NMI is polled at the start of step() and is
// never deferred.
int CPU::pollIrqAtBoundary(uint8_t opcode, bool iAtStart) noexcept {
    if (coreVariant_ != Core::NMOS6502) return 0;
    if (haltedFlag) return 0;
    if (suppressPollOnce_) return 0; // taken branch: discard this poll too (flag stays for the NMI poll)
    if (!irqLine_) return 0;
    bool pollI = (P & FLAG_I) != 0;
    if (opcode == 0x58 || opcode == 0x78 || opcode == 0x28 || opcode == 0x40) // CLI / SEI / PLP / RTI
        pollI = iAtStart;
    if (pollI) return 0;
    ++instructionCount;
    serviceInterrupt(false);
    return 7;
}

// ---- WDC 65C02 extension set (enabled via setCore / INI cpuCore) ----
uint8_t CPU::readRmwOperand(uint16_t a) {
    rmwOld_ = readByte(a);
    return rmwOld_;
}

uint8_t CPU::effectiveLength(uint8_t op) const {
    if (coreVariant_ == Core::WDC65C02 && op == 0xCB) return 1; // WAI (NMOS SBX imm is 2)
    return instructionLength(op);
}

const char *CPU::effectiveName(uint8_t op) const {
    if (coreVariant_ != Core::WDC65C02) return opcodeName(op);
    switch (op) {
    case 0x80:
        return "BRA";
    case 0x1A:
        return "INA";
    case 0x3A:
        return "DEA";
    case 0x5A:
        return "PHY";
    case 0x7A:
        return "PLY";
    case 0xDA:
        return "PHX";
    case 0xFA:
        return "PLX";
    case 0x04:
        return "TSB";
    case 0x0C:
        return "TSB";
    case 0x14:
        return "TRB";
    case 0x1C:
        return "TRB";
    case 0x64:
    case 0x74:
    case 0x9C:
    case 0x9E:
        return "STZ";
    case 0x89:
    case 0x34:
    case 0x3C:
        return "BIT";
    case 0x7C:
        return "JMP";
    case 0xCB:
        return "WAI";
    case 0xDB:
        return "STP";
    default:
        return opcodeName(op);
    }
}

int CPU::stepC02(uint8_t opcode) {
    int cyclesUsed = 0;
    uint16_t addr;
    uint16_t base;
    switch (opcode) {
    case 0x80: { // BRA - always taken
        const int16_t offset = static_cast<int8_t>(pc8());
        const uint16_t before = PC;
        PC = static_cast<uint16_t>(PC + offset);
        cyclesUsed = ((before & 0xFF00u) != (PC & 0xFF00u)) ? 4 : 3;
        break;
    }
    case 0x1A: // INA
        ++A;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed = 2;
        break;
    case 0x3A: // DEA
        --A;
        setFlag(FLAG_Z, A == 0);
        setFlag(FLAG_N, A & 0x80);
        cyclesUsed = 2;
        break;
    case 0x5A: // PHY
        writeByte(static_cast<uint16_t>(0x0100 + SP), Y);
        SP--;
        cyclesUsed = 3;
        break;
    case 0x7A: // PLY
        SP++;
        Y = readByte(static_cast<uint16_t>(0x0100 + SP));
        setFlag(FLAG_Z, Y == 0);
        setFlag(FLAG_N, Y & 0x80);
        cyclesUsed = 4;
        break;
    case 0xDA: // PHX
        writeByte(static_cast<uint16_t>(0x0100 + SP), X);
        SP--;
        cyclesUsed = 3;
        break;
    case 0xFA: // PLX
        SP++;
        X = readByte(static_cast<uint16_t>(0x0100 + SP));
        setFlag(FLAG_Z, X == 0);
        setFlag(FLAG_N, X & 0x80);
        cyclesUsed = 4;
        break;
    case 0x04:   // TSB zp
    case 0x0C: { // TSB abs
        addr = (opcode == 0x04) ? pc8() : pc16();
        const uint8_t v = readRmwOperand(addr);
        setFlag(FLAG_Z, (A & v) == 0);
        writeRmwByte(addr, static_cast<uint8_t>(v | A));
        cyclesUsed = (opcode == 0x04) ? 5 : 6;
        break;
    }
    case 0x14:   // TRB zp
    case 0x1C: { // TRB abs
        addr = (opcode == 0x14) ? pc8() : pc16();
        const uint8_t v = readRmwOperand(addr);
        setFlag(FLAG_Z, (A & v) == 0);
        writeRmwByte(addr, static_cast<uint8_t>(v & static_cast<uint8_t>(~A)));
        cyclesUsed = (opcode == 0x14) ? 5 : 6;
        break;
    }
    case 0x64: // STZ zp
        addr = pc8();
        writeByte(addr, 0);
        cyclesUsed = 3;
        break;
    case 0x74: // STZ zp,X
        addr = static_cast<uint16_t>((pc8() + X) & 0xFF);
        writeByte(addr, 0);
        cyclesUsed = 4;
        break;
    case 0x9C: // STZ abs
        addr = pc16();
        writeByte(addr, 0);
        cyclesUsed = 4;
        break;
    case 0x9E: // STZ abs,X
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        readByte(static_cast<uint16_t>((base & 0xFF00u) | (addr & 0xFFu))); // dummy read cycle
        writeByte(addr, 0);
        cyclesUsed = 5;
        break;
    case 0x89: // BIT #imm (Z only)
        setFlag(FLAG_Z, (A & pc8()) == 0);
        cyclesUsed = 2;
        break;
    case 0x34:   // BIT zp,X
    case 0x3C: { // BIT abs,X
        if (opcode == 0x34) {
            addr = static_cast<uint16_t>((pc8() + X) & 0xFF);
            const uint8_t v = readByte(addr);
            setFlag(FLAG_Z, (A & v) == 0);
            setFlag(FLAG_N, v & 0x80);
            setFlag(FLAG_V, v & 0x40);
            cyclesUsed = 4;
        } else {
            base = pc16();
            addr = static_cast<uint16_t>(base + X);
            const uint8_t v = readIndexedWithDummy(base, addr);
            setFlag(FLAG_Z, (A & v) == 0);
            setFlag(FLAG_N, v & 0x80);
            setFlag(FLAG_V, v & 0x40);
            cyclesUsed = ((base ^ addr) & 0xFF00u) ? 5 : 4;
        }
        break;
    }
    case 0x7C: // JMP (abs,X) - no page-wrap bug on 65C02
        base = pc16();
        addr = static_cast<uint16_t>(base + X);
        PC = readWord(addr);
        cyclesUsed = 6;
        break;
    case 0xCB: // WAI
        waitingForInterrupt_ = true;
        cyclesUsed = 2;
        break;
    case 0xDB: // STP
        haltedFlag = true;
        cyclesUsed = 2;
        break;
    default:
        return -1;
    }
    return cyclesUsed;
}

std::string CPU::mesenTraceLine(uint16_t address) const {
    uint8_t len = 0;
    int cyc = 0;
    bool isPC = false;
    const std::string dis = disassembleOneAt(address, len, cyc, isPC);
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    out << std::setw(4) << static_cast<unsigned>(address) << ':' << dis;
    out << std::left << std::setfill(' ') << std::setw(46 - static_cast<int>(dis.size())) << ' ';
    out << std::right << std::hex << std::uppercase << std::setfill('0');
    out << "A:" << std::setw(2) << static_cast<unsigned>(A) << " X:" << std::setw(2)
        << static_cast<unsigned>(X) << " Y:" << std::setw(2) << static_cast<unsigned>(Y)
        << " P:" << std::setw(2) << static_cast<unsigned>(P) << " SP:" << std::setw(2)
        << static_cast<unsigned>(SP);
    out << std::dec << " CYC:" << totalCycles;
    return out.str();
}
// Helper: ADC
uint8_t CPU::adc(uint8_t a, uint8_t b) {
    const uint8_t carryIn = getFlag(FLAG_C) ? 1u : 0u;
    const uint16_t binary = static_cast<uint16_t>(a) + static_cast<uint16_t>(b) + carryIn;
    const uint8_t binaryResult = static_cast<uint8_t>(binary & 0xFFu);
    setFlag(FLAG_V, ((~(a ^ b) & (a ^ binaryResult) & 0x80u) != 0));
    if (decimalModeEnabled && getFlag(FLAG_D)) {
        // BCD nibble correction from the spec: low digit +6 if >9, its carry
        // feeds the high digit, high digit +6 if >9.  (A byte-level
        // ">0x99" threshold agreed only for valid BCD; it diverged on 972
        // invalid-digit inputs.)
        int lo = static_cast<int>(a & 0x0Fu) + static_cast<int>(b & 0x0Fu) + carryIn;
        if (lo > 9) lo += 6;
        int hi = static_cast<int>(a >> 4) + static_cast<int>(b >> 4) + (lo >> 4);
        if (hi > 9) hi += 6;
        const uint8_t bcdResult = static_cast<uint8_t>(((hi & 0x0F) << 4) | (lo & 0x0F));
        setFlag(FLAG_C, hi > 0x0F);
        if (coreVariant_ == Core::WDC65C02) {
            // 65C02: N/Z come from the BCD-adjusted result (NMOS: binary);
            // decimal mode costs one extra cycle on 65C02.
            decimalC02Penalty_ = true;
            setFlag(FLAG_Z, bcdResult == 0);
            setFlag(FLAG_N, (bcdResult & 0x80u) != 0);
        } else {
            setFlag(FLAG_Z, binaryResult == 0);
            setFlag(FLAG_N, (binaryResult & 0x80u) != 0);
        }
        return bcdResult;
    }
    setFlag(FLAG_C, binary > 0xFFu);
    setFlag(FLAG_Z, binaryResult == 0);
    setFlag(FLAG_N, (binaryResult & 0x80u) != 0);
    return binaryResult;
}

uint8_t CPU::sbc(uint8_t a, uint8_t b) {
    const int borrow = getFlag(FLAG_C) ? 0 : 1;
    const int binarySigned = static_cast<int>(a) - static_cast<int>(b) - borrow;
    const uint8_t binaryResult = static_cast<uint8_t>(binarySigned & 0xFF);
    setFlag(FLAG_V, (((a ^ b) & (a ^ binaryResult) & 0x80u) != 0));
    setFlag(FLAG_C, binarySigned >= 0);
    setFlag(FLAG_Z, binaryResult == 0);
    setFlag(FLAG_N, (binaryResult & 0x80u) != 0);
    if (decimalModeEnabled && getFlag(FLAG_D)) {
        int low = static_cast<int>(a & 0x0Fu) - static_cast<int>(b & 0x0Fu) - borrow;
        int high = static_cast<int>(a & 0xF0u) - static_cast<int>(b & 0xF0u);
        if (low < 0) {
            low -= 6;
            high -= 0x10;
        }
        if (high < 0) high -= 0x60;
        const uint8_t bcdResult = static_cast<uint8_t>((high & 0xF0) | (low & 0x0F));
        if (coreVariant_ == Core::WDC65C02) {
            // 65C02: N/Z come from the BCD result; V stays binary-derived on
            // both cores (6502.org decimal_mode). Plus the +1 cycle penalty.
            decimalC02Penalty_ = true;
            setFlag(FLAG_Z, bcdResult == 0);
            setFlag(FLAG_N, (bcdResult & 0x80u) != 0);
        }
        return bcdResult;
    }
    return binaryResult;
}

std::string CPU::flagsToString(uint8_t p) {
    const char names[8] = {'N', 'V', '-', 'B', 'D', 'I', 'Z', 'C'};
    std::string out;
    out.reserve(8);
    for (int bit = 7; bit >= 0; --bit) {
        if (bit == 5)
            out.push_back('-');
        else {
            const char c = names[7 - bit];
            out.push_back((p & static_cast<uint8_t>(1u << bit))
                              ? c
                              : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

const char *CPU::opcodeName(uint8_t opcode) noexcept {
    const Assembler::OpcodeInfo *info = findOpcodeByByte(opcode);
    return info ? info->name : "???";
}

bool CPU::isIllegal(uint8_t opcode) noexcept {
    const uint8_t kind = kIllegal[opcode].kind;
    return opcode != 0xEA && kind != 0xFF;
}

bool CPU::isIllegal(uint8_t opcode, Core core) noexcept {
    if (!isIllegal(opcode)) return false;
    if (core != Core::WDC65C02) return true;
    // Opcodes the 65C02 redefines although the NMOS core treats them as
    // illegal (WDC W65C02S set: BRA, INA/DEA, PHX/PLX/PHY/PLY, TSB/TRB,
    // STZ, BIT #/zp,X/abs,X, JMP (abs,X), WAI/STP, 2-byte NOPs and the
    // (zp)-indirect group).
    switch (opcode) {
    case 0x04:
    case 0x0C: // TSB
    case 0x12: // ORA (zp)
    case 0x14:
    case 0x1C: // TRB
    case 0x1A:
    case 0x3A: // INC A / DEC A
    case 0x32: // AND (zp)
    case 0x34:
    case 0x3C: // BIT zp,X / abs,X
    case 0x52: // EOR (zp)
    case 0x5A: // PHY
    case 0x64:
    case 0x74: // STZ zp / zp,X
    case 0x72: // ADC (zp)
    case 0x7A: // PLY
    case 0x7C: // JMP (abs,X)
    case 0x80: // BRA
    case 0x82:
    case 0xC2:
    case 0xE2: // 2-byte NOP
    case 0x89: // BIT #imm
    case 0x92: // STA (zp)
    case 0x9C:
    case 0x9E: // STZ abs / abs,X
    case 0xB2: // LDA (zp)
    case 0xCB: // WAI
    case 0xD2: // CMP (zp)
    case 0xDA: // PHX
    case 0xDB: // STP
    case 0xF2: // SBC (zp)
    case 0xFA: // PLX
        return false;
    default:
        return true;
    }
}

uint8_t CPU::instructionLength(uint8_t opcode) noexcept {
    const Assembler::OpcodeInfo *info = findOpcodeByByte(opcode);
    return info ? info->bytes : 1;
}

int CPU::instructionCycles(uint8_t opcode, uint16_t address) const noexcept {
    const Assembler::OpcodeInfo *info = findOpcodeByByte(opcode);
    if (!info) {
        const IllegalInfo &il = kIllegal[opcode];
        return il.kind == 0xFF ? 0 : static_cast<int>(il.base);
    }
    int cycles = info->cycles;
    if (info->pagePenalty && info->bytes == 3) {
        const uint8_t lo = mem.read(static_cast<uint16_t>(address + 1));
        const uint8_t hi = mem.read(static_cast<uint16_t>(address + 2));
        const uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        const uint8_t idx = (info->mode == Assembler::AddrMode::AbsoluteX) ? X : Y;
        if (((base ^ static_cast<uint16_t>(base + idx)) & 0xFF00u) != 0) ++cycles;
    }
    if ((opcode & 0x1Fu) == 0x10u) { // branches: 1 extra when taken, 2 when page crossed
        const int8_t offset = static_cast<int8_t>(mem.read(static_cast<uint16_t>(address + 1)));
        const bool taken = true; // worst-case hint used for UI only
        if (taken)
            cycles += ((static_cast<uint16_t>(address + 2) ^ static_cast<uint16_t>(address + 2 + offset)) &
                       0xFF00u) != 0
                          ? 2
                          : 1;
    }
    return cycles;
}

std::string CPU::disassembleOne(uint16_t address) const {
    uint8_t len = 0;
    int cycles = 0;
    bool isPC = false;
    return disassembleOneAt(address, len, cycles, isPC);
}

std::string CPU::disassembleOneAt(uint16_t address, uint8_t &length, int &cycles, bool &isPC) const {
    const uint8_t op = mem.read(address);
    const Assembler::OpcodeInfo *info = findOpcodeByByte(op);
    length = effectiveLength(op);
    cycles = instructionCycles(op, address);
    isPC = (address == PC);
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    out << std::setw(2) << static_cast<unsigned>(op);

    // 65C02 re-encodings need their own operand formatting.
    if (coreVariant_ == Core::WDC65C02) {
        const uint8_t b1 = mem.read(static_cast<uint16_t>(address + 1));
        const uint8_t b2 = mem.read(static_cast<uint16_t>(address + 2));
        switch (op) {
        case 0x80: {
            const uint16_t target = static_cast<uint16_t>(address + 2 + static_cast<int8_t>(b1));
            out << ' ' << std::left << std::setw(4) << "BRA" << ' ' << "$" << std::setw(4)
                << static_cast<unsigned>(target);
            return out.str();
        }
        case 0x7C:
            out << ' ' << std::left << std::setw(4) << "JMP" << ' ' << "($" << std::setw(4)
                << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8)) << ",X)";
            return out.str();
        case 0x1A:
        case 0x3A:
        case 0x5A:
        case 0x7A:
        case 0xDA:
        case 0xFA:
        case 0xCB:
        case 0xDB:
            out << ' ' << std::left << std::setw(4) << effectiveName(op) << ' ';
            return out.str();
        default:
            if (op == 0x89 || op == 0x04 || op == 0x14 || op == 0x64 || op == 0x74 || op == 0x34 ||
                op == 0x0C || op == 0x1C || op == 0x9C || op == 0x9E || op == 0x3C) {
                out << ' ' << std::left << std::setw(4) << effectiveName(op) << ' ';
                switch (op) {
                case 0x89:
                    out << "#$" << std::setw(2) << static_cast<unsigned>(b1);
                    break;
                case 0x04:
                case 0x14:
                case 0x64:
                case 0x74:
                case 0x34:
                    out << "$" << std::setw(2) << static_cast<unsigned>(b1);
                    if (op != 0x64 && op != 0x04) out << ((op == 0x34) ? ",X" : "");
                    break;
                case 0x9E:
                    out << "$" << std::setw(4) << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8))
                        << ",X";
                    break;
                default:
                    out << "$" << std::setw(4)
                        << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8));
                    break;
                }
                return out.str();
            }
            break;
        }
    }

    if (!info) {
        out << " ???";
        return out.str();
    }
    out << ' ' << std::left << std::setw(4) << info->name << std::right << ' ';
    const uint8_t b1 = mem.read(static_cast<uint16_t>(address + 1));
    const uint8_t b2 = mem.read(static_cast<uint16_t>(address + 2));
    switch (info->mode) {
    case Assembler::AddrMode::Immediate:
        out << "#$" << std::setw(2) << static_cast<unsigned>(b1);
        break;
    case Assembler::AddrMode::ZeroPage:
        out << "$" << std::setw(2) << static_cast<unsigned>(b1);
        break;
    case Assembler::AddrMode::ZeroPageX:
        out << "$" << std::setw(2) << static_cast<unsigned>(b1) << ",X";
        break;
    case Assembler::AddrMode::ZeroPageY:
        out << "$" << std::setw(2) << static_cast<unsigned>(b1) << ",Y";
        break;
    case Assembler::AddrMode::Absolute:
        out << "$" << std::setw(4) << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8));
        break;
    case Assembler::AddrMode::AbsoluteX:
        out << "$" << std::setw(4) << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8)) << ",X";
        break;
    case Assembler::AddrMode::AbsoluteY:
        out << "$" << std::setw(4) << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8)) << ",Y";
        break;
    case Assembler::AddrMode::Indirect:
        out << "($" << std::setw(4) << static_cast<unsigned>(b1 | (static_cast<unsigned>(b2) << 8)) << ")";
        break;
    case Assembler::AddrMode::IndexedIndirect:
        out << "($";
        out << std::setw(2) << static_cast<unsigned>(b1) << ",X)";
        break;
    case Assembler::AddrMode::IndirectIndexed:
        out << "($" << std::setw(2) << static_cast<unsigned>(b1) << "),Y";
        break;
    case Assembler::AddrMode::Relative: {
        const uint16_t target = static_cast<uint16_t>(address + 2 + static_cast<int8_t>(b1));
        out << "$" << std::setw(4) << static_cast<unsigned>(target);
        break;
    }
    case Assembler::AddrMode::Accumulator:
        out << "A";
        break;
    default:
        break;
    }
    return out.str();
}

std::string CPU::disassembleText(uint16_t address, int count) const {
    std::ostringstream out;
    uint16_t pc = address;
    for (int i = 0; i < count; ++i) {
        out << disassembleOne(pc) << "\n";
        const uint8_t op = mem.read(pc);
        std::size_t size = 1;
        for (std::size_t j = 0; j < Assembler::opcodeTableSize(); ++j)
            if (Assembler::opcodeTable()[j].opcode == op) {
                size = Assembler::opcodeTable()[j].bytes;
                break;
            }
        pc = static_cast<uint16_t>(pc + size);
    }
    return out.str();
}

void CPU::disassemble(uint16_t address, int count) const {
    for (int i = 0; i < count; i++) {
        uint8_t opcode = mem.read(address);
        std::cout << std::hex << std::setw(4) << std::setfill('0') << address << ": ";
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)opcode << " ";
        address++;
    }
}
