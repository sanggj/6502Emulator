#include "emulator.hpp"
#include "apu.hpp"

#include <algorithm>
#include <fstream>

Emulator::Emulator() : memory_{}, cpu_(memory_), apu_(new Apu()) {
    memory_.reset(0x00);
    cpu_.reset();
    Memory &mem = memory_;
    mem.setIoWriteHook([&mem, this](uint16_t address, uint8_t value) {
        if (address <= 0x4017) {
            apu_->writeRegister(address, value);
            // $4017 bit 6/7 write acks the frame IRQ mid-instruction; re-sync
            // now so the CPU's in-step boundary poll (pollIrqAtBoundary) sees
            // the released line instead of a stale-high one.
            syncIrqLine();
        }
        (void)mem;
    });
    mem.setIoReadHook([this](uint16_t address) -> uint8_t {
        if (address == 0x4015) {
            const uint8_t status = apu_->readStatus(address);
            // Reading $4015 clears the frame flag mid-instruction; same
            // reason as the write hook above.
            syncIrqLine();
            return status;
        }
        return 0;
    });
}

bool Emulator::hasProgram() const noexcept {
    return program_.byteCount > 0;
}

Emulator::~Emulator() = default;

Apu &Emulator::apu() noexcept {
    return *apu_;
}

void Emulator::tickApu(int cpuCycles) noexcept {
    if (cpuCycles > 0) apu_->tick(static_cast<uint64_t>(cpuCycles));
    syncIrqLine();
}

// The CPU IRQ line is shared between the external `irq` command and the APU
// frame IRQ: assert while either source asserts, release only when neither
// does. Called after every APU tick (frame IRQ) and on external requests.
void Emulator::syncIrqLine() noexcept {
    cpu_.setIrqLine(externalIrq_ || apu_->frameIrqAsserted());
}

void Emulator::requestIrq(bool asserted) noexcept {
    externalIrq_ = asserted;
    syncIrqLine();
}

bool Emulator::assembleAndBurn(const std::string &source, std::string &message) {
    AssembledProgram assembled;
    std::vector<AssemblerError> errors;
    if (!assembler_.assemble(source, assembled, errors)) {
        message = "Assembly failed (" + std::to_string(errors.size()) + " error" +
                  (errors.size() == 1 ? "" : "s") + "):\n";
        for (const auto &error : errors) {
            message += "line " + std::to_string(error.line) + ": " + error.message + "\n";
        }
        return false;
    }
    memory_.reset(0x00);
    for (std::size_t i = 0; i < assembled.memory.size(); ++i) {
        if (assembled.written[i]) memory_.write(static_cast<uint16_t>(i), assembled.memory[i]);
    }
    source_ = source;
    program_ = assembled;
    cpu_.reset();
    cpu_.clearHalt();
    running_ = false;
    message = "Burned " + std::to_string(assembled.byteCount) + " bytes; entry=$";
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "%04X", assembled.entryPoint);
    message += buf;
    return true;
}

void Emulator::reset() {
    pause();
    cpu_.clearHalt();
    cpu_.reset(); // CPU reset releases the IRQ line (historic behavior)
    externalIrq_ = false;
    apu_->reset();
    syncIrqLine(); // APU frame IRQ cleared by apu reset; keep line truthful
}

int Emulator::step() {
    if (cpu_.halted()) return 0;
    const int cycles = cpu_.step();
    if (cycles > 0) apu_->tick(static_cast<uint64_t>(cycles));
    syncIrqLine();
    if (cycles == 0) running_ = false;
    return cycles;
}

bool Emulator::debugStepInternal() {
    return step() > 0;
}

uint64_t Emulator::runInstructions(uint64_t instructionBudget) {
    if (!running_) return 0;
    uint64_t executed = 0;
    while (executed < instructionBudget && !cpu_.halted()) {
        if (cpu_.step() == 0) {
            running_ = false;
            break;
        }
        ++executed;
    }
    return executed;
}

bool Emulator::loadBinaryFile(const std::string &file, uint16_t address, std::string &message) {
    std::vector<uint8_t> bytes;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        message = "Could not open " + file;
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    if (!memory_.loadBytes(bytes, address, message)) return false;
    memory_.write(0xFFFC, static_cast<uint8_t>(address));
    memory_.write(0xFFFD, static_cast<uint8_t>(address >> 8));
    cpu_.reset();
    return true;
}

bool Emulator::saveBinaryFile(const std::string &file, uint16_t start, uint16_t end,
                              std::string &message) const {
    if (start > end) {
        message = "start address must be <= end address";
        return false;
    }
    const auto bytes = memory_.copyRange(start, static_cast<std::size_t>(end - start) + 1);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        message = "Could not create " + file;
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        message = "Write failed";
        return false;
    }
    message = "Saved " + std::to_string(bytes.size()) + " bytes";
    return true;
}
