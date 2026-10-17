#pragma once

#include "cpu.hpp"
#include "emulator.hpp"
#include "memory.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct CPUStateSnapshot {
    uint8_t A = 0;
    uint8_t X = 0;
    uint8_t Y = 0;
    uint16_t PC = 0;
    uint8_t SP = 0;
    uint8_t P = FLAG_U;
    uint64_t totalCycles = 0;
    uint8_t lastOpcode = 0;
    int lastCycles = 0;
    bool halted = false;
};

struct DebugMemoryDelta {
    uint16_t address = 0;
    uint8_t oldValue = 0;
    uint8_t newValue = 0;
};

struct DebugHistoryEntry {
    uint16_t pc = 0;
    uint8_t opcode = 0;
    CPUStateSnapshot before{};
    std::vector<DebugMemoryDelta> writes;
    int cycles = 0;
    std::string disassembly;
};

struct DebugTraceEntry {
    uint64_t sequence = 0;
    uint16_t pc = 0;
    uint8_t opcode = 0;
    CPUStateSnapshot before{};
    CPUStateSnapshot after{};
    int cycles = 0;
    std::string disassembly;
    std::vector<DebugMemoryDelta> writes;
};

struct DebugBreakpoint {
    uint16_t address = 0;
    bool enabled = true;
    std::string condition;
    int hitCount = 0;
    int ignoreLeft = 0;
    bool logMode = false;
    std::string logMessage;
};

struct DebugWatchpoint {
    uint16_t start = 0;
    uint16_t end = 0;
    bool enabled = true;
    bool onRead = false;
    bool onWrite = true;
};

struct DebugProfilerEntry {
    uint64_t executions = 0;
    uint64_t cycles = 0;
};

class Debugger {
  public:
    explicit Debugger(Emulator &emulator);

    void clear();

    void setHistoryEnabled(bool enabled) noexcept { historyEnabled_ = enabled; }
    bool historyEnabled() const noexcept { return historyEnabled_; }
    void setHistoryCapacity(std::size_t capacity);
    std::size_t historyCount() const noexcept { return history_.size(); }
    bool stepBack();
    bool stepForward();

    void setTraceEnabled(bool enabled) noexcept { traceEnabled_ = enabled; }
    bool traceEnabled() const noexcept { return traceEnabled_; }
    void clearTrace();
    const std::deque<DebugTraceEntry> &trace() const noexcept { return trace_; }
    bool exportTrace(const std::string &path, std::string &error) const;
    // Mesen-style per-instruction trace log (item 7)
    bool exportTraceMesen(const std::string &path, std::string &error) const;
    // Only record trace entries whose condition matches (empty = all).
    void setTraceFilter(const std::string &condition) { traceFilter_ = condition; }
    const std::string &traceFilter() const noexcept { return traceFilter_; }

    // Stepping strategies (items 39-41).
    // stepOver: a JSR is executed as one step. stepOut: run until the
    // current JSR frame returns (next RTS at depth 0). runTo: run until PC.
    uint64_t stepOver(uint64_t budget = 1000000);
    uint64_t stepOut(uint64_t budget = 10000000);
    uint64_t runTo(uint16_t address, uint64_t budget = 100000000);
    bool lastStopWasStepTarget() const noexcept { return lastStopStepTarget_; }

    struct CallFrame {
        uint16_t returnAddress = 0;     // address pushed by JSR (points at operand tail)
        uint16_t callerInstruction = 0; // start of the JSR instruction itself
        uint16_t sp = 0;
    };
    // Heuristic stack walk: scan the live stack for plausible JSR frames.
    std::vector<CallFrame> callStack() const;

    const std::deque<DebugHistoryEntry> &history() const noexcept { return history_; }

    // Seqlock-protected snapshot published periodically while running so a
    // remote-debug thread can read registers without taking the emu mutex.
    struct LockFreeSnapshot {
        static constexpr uint32_t kMagic = 0x534E4150u; // 'SNAP'
        uint32_t magic = kMagic;
        CPUStateSnapshot cpu{};
        std::array<uint8_t, 256> stackTop{}; // stack page contents
        uint64_t totalCycles = 0;
    };
    void publishSnapshot() noexcept;                               // called from emulation side
    bool tryAcquireSnapshot(LockFreeSnapshot &out) const noexcept; // wait-free read

    void setProfilerEnabled(bool enabled) noexcept { profilerEnabled_ = enabled; }
    bool profilerEnabled() const noexcept { return profilerEnabled_; }
    void clearProfiler();
    const DebugProfilerEntry &profiler(uint16_t address) const noexcept { return profiler_[address]; }

    bool addBreakpoint(uint16_t address, const std::string &condition = {});
    bool removeBreakpoint(uint16_t address);
    bool setBreakpointEnabled(uint16_t address, bool enabled);
    const std::vector<DebugBreakpoint> &breakpoints() const noexcept { return breakpoints_; }
    bool breakpointHit(uint16_t pc) const;

    void setLogSink(std::function<void(const std::string &)> sink) { logSink_ = std::move(sink); }
    bool evaluateExpression(const std::string &expr, uint64_t &value, std::string &err) const;
    int breakpointHitCount(uint16_t address) const;
    bool setBreakpointLog(uint16_t address, const std::string &message);
    bool setBreakpointIgnore(uint16_t address, int count);

    bool addWatchExpression(const std::string &expr);
    bool removeWatchExpression(std::size_t index);
    const std::vector<std::string> &watchExpressions() const noexcept { return watchExpressions_; }

    void setCoverageEnabled(bool enabled) noexcept { coverageEnabled_ = enabled; }
    bool coverageEnabled() const noexcept { return coverageEnabled_; }
    double coveragePercent(uint16_t low, uint16_t high) const noexcept;
    bool exportCoverageCsv(const std::string &path, std::string &error) const;

    // NOTE: write watchpoints only. The memory layer traces writes, not reads,
    // so onRead=true is rejected (returns false + logs via the sink) instead of
    // being silently accepted and never firing.
    bool addWatchpoint(uint16_t start, uint16_t end, bool onRead = false, bool onWrite = true);
    void clearWatchpoints();
    const std::vector<DebugWatchpoint> &watchpoints() const noexcept { return watchpoints_; }
    bool watchpointHit(const std::vector<Memory::Write> &writes) const;

    void takeSnapshot();
    void clearSnapshot();
    bool hasSnapshot() const noexcept { return snapshotValid_; }
    std::vector<DebugMemoryDelta> snapshotDiff() const;

    bool beforeInstruction();
    int step();
    uint64_t runInstructions(uint64_t budget);
    bool lastStopWasBreakpoint() const noexcept { return lastStopBreakpoint_; }
    bool lastStopWasWatchpoint() const noexcept { return lastStopWatchpoint_; }
    void clearStopReason() noexcept {
        lastStopBreakpoint_ = false;
        lastStopWatchpoint_ = false;
        lastStopStepTarget_ = false;
    }
    void afterInstruction(uint16_t pcBefore, uint8_t opcode, int cycles, const CPUStateSnapshot &before,
                          const std::string &disassembly);

    CPUStateSnapshot captureState() const;
    void restoreState(const CPUStateSnapshot &state);

    std::string statusText() const;

  private:
    Emulator &emulator_;
    bool historyEnabled_ = true;
    std::size_t historyCapacity_ = 512;
    std::deque<DebugHistoryEntry> history_;
    std::size_t historyPosition_ = 0;

    bool traceEnabled_ = false;
    std::deque<DebugTraceEntry> trace_;
    std::size_t traceCapacity_ = 50000;
    uint64_t traceSequence_ = 0;

    bool profilerEnabled_ = false;
    std::array<DebugProfilerEntry, 65536> profiler_{};

    std::vector<DebugBreakpoint> breakpoints_;
    std::vector<DebugWatchpoint> watchpoints_;

    bool snapshotValid_ = false;
    std::array<uint8_t, Memory::SIZE> snapshotMemory_{};
    CPUStateSnapshot snapshotCPU_{};

    bool lastStopBreakpoint_ = false;
    bool lastStopWatchpoint_ = false;
    bool lastStopStepTarget_ = false;
    std::string traceFilter_;
    mutable std::atomic<uint32_t> snapSeq_{0};
    LockFreeSnapshot snapBuffers_[2]{};
    std::function<void(const std::string &)> logSink_;
    std::vector<std::string> watchExpressions_;
    std::array<bool, 65536> coverage_{};
    bool coverageEnabled_ = false;
    bool evaluateCondition(const std::string &expression) const;
    // Recursive paren-descent worker for evaluateExpression(): depth-capped so
    // a hostile condition with tens of thousands of '(' cannot blow the stack.
    bool evaluateExpressionDepth(const std::string &expr, uint64_t &value, std::string &err,
                                 int recDepth) const;
    // Route a condition/parse error to logSink_, each distinct message once
    // only (a hot breakpoint must not flood the log).  Dedupe covers the last
    // kRecentErrors distinct messages so two alternating condition errors do
    // not re-log on every hit; bounded memory, oldest evicted first.
    void logErrorOnce(const std::string &msg) const;
    static std::string upper(std::string text);
    static constexpr std::size_t kRecentErrors = 8;
    mutable std::array<std::string, kRecentErrors> recentErrors_{};
    mutable std::size_t recentErrorsNext_ = 0;
};
