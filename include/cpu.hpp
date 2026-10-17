#ifndef CPU_HPP
#define CPU_HPP

#include "memory.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Status flags - use FLAG_ prefix to avoid Windows macro conflicts
constexpr uint8_t FLAG_C = 0x01;
constexpr uint8_t FLAG_Z = 0x02;
constexpr uint8_t FLAG_I = 0x04;
constexpr uint8_t FLAG_D = 0x08;
constexpr uint8_t FLAG_B = 0x10;
constexpr uint8_t FLAG_U = 0x20; // unused
constexpr uint8_t FLAG_V = 0x40;
constexpr uint8_t FLAG_N = 0x80;

class CPU {
  public:
    enum class Core : uint8_t { NMOS6502, WDC65C02 };

    CPU(Memory &memory);
    ~CPU();

    void reset();
    void irq(); // assert IRQ line (level sensitive; sampled when I flag clear)
    void nmi(); // assert NMI line (edge triggered; latched)
    void setNmiLine(bool asserted) noexcept;
    void setIrqLine(bool asserted) noexcept;
    bool nmiLine() const noexcept { return nmiLine_; }
    bool irqLine() const noexcept { return irqLine_; }
    bool nmiPending() const noexcept { return nmiPending_; }
    // Savestate restore: set line+pending atomically, bypassing the edge
    // detector (driving setNmiLine() would latch a phantom NMI).
    void restoreNmiState(bool asserted, bool pending) noexcept {
        nmiLine_ = asserted;
        nmiPending_ = pending;
    }
    // Savestate restore: transient execution state not covered by
    // restoreTiming()/restoreNmiState() (v2 trailer fields).
    void restoreTransientState(uint64_t instrCount, bool waiting, bool suppressPoll,
                               bool waiIrqBypass) noexcept {
        instructionCount = instrCount;
        waitingForInterrupt_ = waiting;
        suppressPollOnce_ = suppressPoll;
        waiIrqBypass_ = waiIrqBypass;
    }
    int step(); // returns cycles used

    // Registers
    uint8_t A, X, Y;
    uint16_t PC;
    uint8_t SP;
    uint8_t P; // status flags

    // Disassembly
    void disassemble(uint16_t address, int count) const;

    // Getter for cycles
    int getCycles() const noexcept { return lastCycles; }
    uint64_t getTotalCycles() const noexcept { return totalCycles; }
    uint64_t getInstructionCount() const noexcept { return instructionCount; }
    uint8_t lastOpcode() const noexcept { return lastOpcodeValue; }
    bool halted() const noexcept { return haltedFlag; }
    bool isWaitingForInterrupt() const noexcept { return waitingForInterrupt_; }
    bool suppressPollOnceActive() const noexcept { return suppressPollOnce_; }
    bool waiIrqBypassActive() const noexcept { return waiIrqBypass_; }
    void setDecimalModeEnabled(bool enabled) noexcept { decimalModeEnabled = enabled; }
    bool decimalModeIsEnabled() const noexcept { return decimalModeEnabled; }
    void setCore(Core core) noexcept;
    Core core() const noexcept { return coreVariant_; }
    static bool coreIs65C02(Core c) noexcept { return c == Core::WDC65C02; }
    void clearHalt() noexcept {
        haltedFlag = false;
        waitingForInterrupt_ = false;
    }
    void restoreTiming(uint64_t total, uint8_t opcode, int last, bool halted) noexcept {
        totalCycles = total;
        lastOpcodeValue = opcode;
        lastCycles = last;
        haltedFlag = halted;
    }
    std::string disassembleText(uint16_t address, int count) const;
    std::string disassembleOne(uint16_t address) const;
    std::string disassembleOneAt(uint16_t address, uint8_t &length, int &cycles, bool &isPC) const;
    // Mesen-style cycle-by-cycle trace line for the instruction at `address`.
    std::string mesenTraceLine(uint16_t address) const;
    static std::string flagsToString(uint8_t p);
    static const char *opcodeName(uint8_t opcode) noexcept;
    static uint8_t instructionLength(uint8_t opcode) noexcept;
    int instructionCycles(uint8_t opcode, uint16_t address) const noexcept;
    // opcode-only form = NMOS table; the Core overload also knows the
    // opcodes the 65C02 redefines (BRA/STZ/TSB/TRB/WAI/...).
    static bool isIllegal(uint8_t opcode) noexcept;
    static bool isIllegal(uint8_t opcode, Core core) noexcept;

  private:
    Memory &mem;
    int cycles;
    int lastCycles;
    uint64_t totalCycles;
    uint64_t instructionCount;
    uint8_t lastOpcodeValue;
    bool haltedFlag;
    bool decimalModeEnabled;
    Core coreVariant_ = Core::NMOS6502;

    // Interrupt lines: NMI is edge-triggered (falling edge on the real bus,
    // modelled here as a rising edge of the request), IRQ is level-sensitive.
    bool nmiLine_ = false;
    bool irqLine_ = false;
    bool nmiPending_ = false;          // NMI edge detected but not serviced yet
    bool suppressPollOnce_ = false;    // taken branches delay polling one instruction
    bool waitingForInterrupt_ = false; // 65C02 WAI state
    bool waiIrqBypass_ = false;        // IRQ waking WAI is serviced despite I=1
    bool decimalC02Penalty_ = false;   // 65C02 decimal ADC/SBC takes one extra cycle

    bool interruptWakeup() const noexcept { return nmiPending_ || irqLine_; }

    // Predecoded instruction cache (item 54): operand bytes for the
    // instruction at PC, validated against per-64-byte dirty-page epochs so
    // self-modifying code invalidates precisely.
    struct Decoded {
        uint64_t pageEpochLo = 0;
        uint64_t pageEpochHi = 0;
        uint8_t opcode = 0xEA;
        uint8_t length = 1;
        uint8_t b[3] = {0xEA, 0, 0};
        bool valid = false;
    };
    std::vector<Decoded> decodeCache_;
    uint32_t epochGuard_ = 0;
    Decoded decoded_;
    uint8_t operandCursor_ = 0;
    uint8_t rmwOld_ = 0;

    const Decoded &decodedAt(uint16_t pc);
    uint8_t pc8();
    uint16_t pc16();

    // Flag helpers
    void setFlag(uint8_t flag, bool set);
    bool getFlag(uint8_t flag) const;

    // Memory access
    uint8_t readByte(uint16_t addr);
    void writeByte(uint16_t addr, uint8_t val);
    void writeRmwByte(uint16_t addr, uint8_t newVal); // double-write side effects
    uint16_t readWord(uint16_t addr);
    uint16_t readWordZeroPage(uint8_t addr);
    uint16_t readWordJmpBug(uint16_t addr);
    uint8_t readPC();
    uint16_t readPC16();

    // Dummy read performed by indexed addressing on a possibly-unfixed
    // address before the final access (visible to I/O devices).
    uint8_t dummyReadThen(uint16_t unfixedAddr, uint16_t finalAddr);
    uint8_t readIndexedWithDummy(uint16_t base, uint16_t indexed);
    uint8_t readRmwOperand(uint16_t addr);

    // Interrupt service shared by NMI/IRQ/BRK hijack paths.
    void serviceInterrupt(bool fromNmi);

    // NMOS IRQ poll for the boundary after an instruction (see cpu.cpp).
    int pollIrqAtBoundary(uint8_t opcode, bool iAtStart) noexcept;

    // 65C02 extension dispatch; returns cycles, or -1 when the opcode is not
    // a 65C02 extension and should fall through to the common decoder.
    int stepC02(uint8_t opcode);
    uint8_t effectiveLength(uint8_t opcode) const;
    const char *effectiveName(uint8_t opcode) const;

    // Arithmetic helpers
    uint8_t adc(uint8_t a, uint8_t b);
    uint8_t sbc(uint8_t a, uint8_t b);
};

#endif // CPU_HPP
