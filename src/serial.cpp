#include "serial.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>

namespace {

std::string toHexPairs(const std::vector<uint8_t> &bytes) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i) out += ' ';
        const uint8_t b = bytes[i];
        out += digits[(b >> 4) & 0xFu];
        out += digits[b & 0xFu];
    }
    return out;
}

} // namespace

#ifdef _WIN32

bool SerialUploader::upload(const std::string &portName, uint32_t baud, const std::vector<uint8_t> &bytes,
                            std::string &error) {
    if (portName.empty()) {
        error = "Serial port is empty";
        return false;
    }
    std::string device = "\\\\.\\" + portName;
    HANDLE port = CreateFileA(device.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (port == INVALID_HANDLE_VALUE) {
        error = "Could not open serial port " + portName;
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port, &dcb)) {
        error = "GetCommState failed";
        CloseHandle(port);
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    if (!SetCommState(port, &dcb)) {
        error = "SetCommState failed";
        CloseHandle(port);
        return false;
    }
    COMMTIMEOUTS timeouts{};
    timeouts.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(port, &timeouts);

    DWORD written = 0;
    const BYTE *ptr = bytes.data();
    DWORD remaining = static_cast<DWORD>(bytes.size());
    while (remaining) {
        DWORD chunk = 0;
        if (!WriteFile(port, ptr, remaining, &chunk, nullptr)) {
            error = "Serial write failed";
            CloseHandle(port);
            return false;
        }
        if (chunk == 0) {
            error = "Serial write made no progress";
            CloseHandle(port);
            return false;
        }
        ptr += chunk;
        remaining -= chunk;
        written += chunk;
    }
    CloseHandle(port);
    error = "Uploaded " + std::to_string(written) + " bytes to " + portName;
    return true;
}

SerialMonitor::SerialMonitor() : startTickMs_(GetTickCount64()) {}

SerialMonitor::~SerialMonitor() {
    closeLog();
    close();
}

SerialMonitor &SerialMonitor::instance() {
    static SerialMonitor mon;
    return mon;
}

bool SerialMonitor::open(const std::string &portName, uint32_t baudRate, std::string &error) {
    if (portName.empty()) {
        error = "Serial port is empty";
        return false;
    }
    close(); // reopen cleanly if already open
    baud_ = baudRate;
    HANDLE port = CreateFileA(("\\\\.\\" + portName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (port == INVALID_HANDLE_VALUE) {
        osPort_ = nullptr;
        error = "Could not open serial port " + portName;
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port, &dcb)) {
        CloseHandle(port);
        osPort_ = nullptr;
        error = "GetCommState failed";
        return false;
    }
    dcb.BaudRate = baud_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    if (!SetCommState(port, &dcb)) {
        CloseHandle(port);
        osPort_ = nullptr;
        error = "SetCommState failed";
        return false;
    }
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 20; // short-gap reads: poll() never blocks long
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(port, &timeouts)) {
        CloseHandle(port);
        osPort_ = nullptr;
        error = "SetCommTimeouts failed";
        return false;
    }
    osPort_ = port;
    portName_ = portName;
    error = "Opened " + portName + " @" + std::to_string(baud_);
    return true;
}

void SerialMonitor::close() {
    if (osPort_) {
        CloseHandle(static_cast<HANDLE>(osPort_));
        osPort_ = nullptr;
    }
}

bool SerialMonitor::setBaud(uint32_t newBaud) {
    baud_ = newBaud;
    if (!osPort_) return false;
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    HANDLE port = static_cast<HANDLE>(osPort_);
    if (!GetCommState(port, &dcb)) return false;
    dcb.BaudRate = newBaud;
    return SetCommState(port, &dcb) != 0;
}

void SerialMonitor::completeLine(std::vector<uint8_t> raw) {
    Line line;
    line.tickMs = GetTickCount64() - startTickMs_;
    if (hexMode_)
        line.text = toHexPairs(raw);
    else {
        line.text.reserve(raw.size());
        for (uint8_t b : raw)
            if (b != '\r') line.text += static_cast<char>(b >= 32 && b < 127 ? b : '.');
    }
    ++totalLines_;
    lines_.push_back(std::move(line));
    while (lines_.size() > kRingCapacity) {
        lines_.pop_front();
        ++droppedLines_;
    }
    if (logging_ && logFile_.is_open()) {
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "[+%llu ms] ",
                      static_cast<unsigned long long>(lines_.back().tickMs));
        logFile_ << stamp << lines_.back().text << '\n';
        logBytes_ += lines_.back().text.size() + std::strlen(stamp) + 1;
        if (logBytes_ >= kLogCapBytes) closeLog(); // 5 MB cap reached
    }
}

void SerialMonitor::poll() {
    if (!osPort_) return;
    uint8_t buf[2048];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(static_cast<HANDLE>(osPort_), buf, sizeof(buf), &got, nullptr) || got == 0) break;
        feed(buf, got);
        if (got < sizeof(buf)) break; // drained
    }
}

void SerialMonitor::feed(const uint8_t *data, std::size_t count) {
    if (!data || count == 0) return;
    bytesReceived_ += count;

    if (hexMode_) { // one dump line per chunk keeps the hex view responsive
        completeLine(std::vector<uint8_t>(data, data + count));
        pending_.clear();
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        pending_.push_back(data[i]);
        if (pending_.size() >= 4096 || data[i] == '\n') {
            completeLine(std::vector<uint8_t>(pending_.begin(), pending_.end()));
            pending_.clear();
        }
    }
}

bool SerialMonitor::openLog(const std::string &path) {
    closeLog();
    logFile_.open(path, std::ios::app);
    if (!logFile_) return false;
    logPath_ = path;
    logging_ = true;
    logBytes_ = 0;
    return true;
}

void SerialMonitor::closeLog() {
    if (logFile_.is_open()) logFile_.close();
    logging_ = false;
}

#endif // _WIN32

#ifndef _WIN32

bool SerialUploader::upload(const std::string &, uint32_t, const std::vector<uint8_t> &, std::string &error) {
    error = "Serial upload is Windows-only in this build";
    return false;
}

SerialMonitor::SerialMonitor() : startTickMs_(0) {}
SerialMonitor::~SerialMonitor() { closeLog(); }
SerialMonitor &SerialMonitor::instance() {
    static SerialMonitor mon;
    return mon;
}
bool SerialMonitor::open(const std::string &, uint32_t baudRate, std::string &error) {
    error = "Serial monitor is Windows-only in this build";
    return false;
}
void SerialMonitor::close() {}
bool SerialMonitor::setBaud(uint32_t newBaud) {
    baud_ = newBaud;
    return false;
}
void SerialMonitor::completeLine(std::vector<uint8_t> raw) {
    Line line;
    line.tickMs = 0;
    if (hexMode_)
        line.text = toHexPairs(raw);
    else {
        line.text.reserve(raw.size());
        for (uint8_t b : raw)
            if (b != '\r') line.text += static_cast<char>(b >= 32 && b < 127 ? b : '.');
    }
    ++totalLines_;
    lines_.push_back(std::move(line));
    while (lines_.size() > kRingCapacity) lines_.pop_front();
}
void SerialMonitor::poll() {}
void SerialMonitor::feed(const uint8_t *data, std::size_t count) {
    if (!data || count == 0) return;
    bytesReceived_ += count;
    if (hexMode_) {
        completeLine(std::vector<uint8_t>(data, data + count));
        pending_.clear();
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        pending_.push_back(data[i]);
        if (pending_.size() >= 4096 || data[i] == '\n') {
            completeLine(std::vector<uint8_t>(pending_.begin(), pending_.end()));
            pending_.clear();
        }
    }
}
bool SerialMonitor::openLog(const std::string &path) {
    closeLog();
    logFile_.open(path, std::ios::app);
    if (!logFile_) return false;
    logPath_ = path;
    logging_ = true;
    logBytes_ = 0;
    return true;
}
void SerialMonitor::closeLog() {
    if (logFile_.is_open()) logFile_.close();
    logging_ = false;
}

#endif // !_WIN32
