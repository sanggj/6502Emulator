#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

class SerialUploader {
  public:
    static bool upload(const std::string &portName, uint32_t baud, const std::vector<uint8_t> &bytes,
                       std::string &error);
};

// Line-oriented receive monitor (item 98): timestamps, hex/ascii rendering,
// log-to-file capture and a bounded ring of recent lines for UI display.
// All methods are safe to call when no port was ever opened (graceful).
class SerialMonitor {
  public:
    static constexpr std::size_t kRingCapacity = 1000; // last N received lines
    static constexpr std::size_t kLogCapBytes = 5u * 1024u * 1024u;

    struct Line {
        uint64_t tickMs = 0; // GetTickCount64 delta in ms since monitor creation
        std::string text;
    };

    static SerialMonitor &instance();

    bool open(const std::string &portName, uint32_t baud, std::string &error);
    void close();
    bool isOpen() const noexcept { return osPort_ != nullptr; }

    // Live baud-rate change; stores the rate even when the port is closed so
    // a later open() uses it.
    bool setBaud(uint32_t baud);
    uint32_t baud() const noexcept { return baud_; }
    const std::string &portName() const noexcept { return portName_; }

    // Display mode: false = ASCII lines, true = hex byte dump per chunk.
    void setHexMode(bool hex) noexcept { hexMode_ = hex; }
    bool hexMode() const noexcept { return hexMode_; }
    void toggleHexMode() noexcept { hexMode_ = !hexMode_; }

    void poll(); // drain OS receive buffer -> lines (+ log file)

    const std::deque<Line> &lines() const noexcept { return lines_; }
    uint64_t totalLines() const noexcept { return totalLines_; }   // monotonic
    uint64_t droppedLines() const noexcept { return droppedLines_; }
    uint64_t bytesReceived() const noexcept { return bytesReceived_; }

    // Log-to-file: append mode, stops once kLogCapBytes have been written.
    bool openLog(const std::string &path);
    void closeLog();
    bool logging() const noexcept { return logging_; }
    const std::string &logPath() const noexcept { return logPath_; }

    // Test/loopback injection: behaves exactly like bytes from the port.
    void feed(const uint8_t *data, std::size_t count);

  private:
    SerialMonitor();
    ~SerialMonitor();
    SerialMonitor(const SerialMonitor &) = delete;
    SerialMonitor &operator=(const SerialMonitor &) = delete;

    void completeLine(std::vector<uint8_t> raw);

    std::string portName_;
    uint32_t baud_ = 115200;
    bool hexMode_ = false;
    uint64_t startTickMs_ = 0;
    std::deque<Line> lines_;
    std::deque<uint8_t> pending_; // partial line across polls (ASCII mode)
    std::ofstream logFile_;
    std::string logPath_;
    bool logging_ = false;
    std::size_t logBytes_ = 0;
    uint64_t totalLines_ = 0;
    uint64_t droppedLines_ = 0;
    uint64_t bytesReceived_ = 0;
    void *osPort_ = nullptr; // HANDLE on Windows
};
