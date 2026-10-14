#pragma once

// Thin platform abstraction (item 81): everything OS-specific the IDE needs,
// declared once so a Linux/mac port only has to reimplement this header.
// The Win32 implementation lives in platform.cpp; new code should prefer
// these entry points over raw Win32 calls.

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

// ---- strings -------------------------------------------------------------
// UTF-8 <-> UTF-16 conversion. Both are lossless for valid input; invalid
// sequences decode to U+FFFD rather than failing.
std::wstring utf8ToWide(const std::string& text);
std::string wideToUtf8(const std::wstring& text);

// ---- paths / files -------------------------------------------------------
// Directory for ALL generated files (INI, saves, screenshots): the folder
// that contains the executable (portable/single-folder mode).
std::string settingsDirectory();
// Directory of the running executable itself (no trailing slash).
std::string executableDirectory();
// Read a whole file as bytes. Returns false on any error; never throws.
bool readFileBinary(const std::string& utf8Path, std::vector<uint8_t>& out);
// Write bytes atomically-ish (direct write). Returns false on failure.
bool writeFileBinary(const std::string& utf8Path, const void* data, std::size_t size);
// True when the file exists.
bool fileExists(const std::string& utf8Path);
// Prefix a Windows path for long-path (>260 chars) safety (item 84).
std::string longPathSafe(const std::string& utf8Path);

// ---- dialogs -------------------------------------------------------------
struct OpenDialogResult {
    bool confirmed = false;
    std::string pathUtf8;
};
OpenDialogResult openFileDialog(void* parentHwnd, const char* filter, const char* title);
OpenDialogResult saveFileDialog(void* parentHwnd, const char* filter, const char* title);

// ---- time ----------------------------------------------------------------
// High-resolution monotonic milliseconds.
uint64_t steadyMillis();
void sleepMillis(int ms);

} // namespace platform
