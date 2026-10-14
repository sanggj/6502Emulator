#pragma once

#include "assembler.hpp"
#include "cpu.hpp"
#include "memory.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Apu;

class Emulator {
  public:
    Emulator();
    ~Emulator();

    bool assembleAndBurn(const std::string &source, std::string &message);
    void reset();
    int step();
    uint64_t runInstructions(uint64_t instructionBudget);
    bool debugStepInternal();
    bool beforeStepBreakpoint() const noexcept { return false; }
    void pause() noexcept { running_ = false; }
    void start() noexcept { running_ = true; }
    bool running() const noexcept { return running_; }

    bool loadBinaryFile(const std::string &file, uint16_t address, std::string &message);
    bool saveBinaryFile(const std::string &file, uint16_t start, uint16_t end, std::string &message) const;
    bool hasProgram() const noexcept;

    CPU &cpu() noexcept { return cpu_; }
    const CPU &cpu() const noexcept { return cpu_; }
    Memory &memory() noexcept { return memory_; }
    const Memory &memory() const noexcept { return memory_; }
    const AssembledProgram &program() const noexcept { return program_; }
    const std::string &source() const noexcept { return source_; }

    Apu &apu() noexcept;
    void tickApu(int cpuCycles) noexcept;
    void setFrameCounter(uint8_t value) noexcept { frameCounter_ = value; }
    uint8_t frameCounter() const noexcept { return frameCounter_; }

    // Interrupt line control (edge/level modeled inside the CPU).
    void requestNmi() noexcept { cpu_.setNmiLine(true); }
    void clearNmi() noexcept { cpu_.setNmiLine(false); }
    void requestIrq(bool asserted) noexcept { cpu_.setIrqLine(asserted); }
    void setCoreVariant(CPU::Core core) noexcept { cpu_.setCore(core); }
    CPU::Core coreVariant() const noexcept { return cpu_.core(); }

  private:
    Memory memory_;
    CPU cpu_;
    Assembler assembler_;
    AssembledProgram program_;
    std::string source_;
    bool running_ = false;
    uint8_t frameCounter_ = 0;
    std::unique_ptr<Apu> apu_;
};
