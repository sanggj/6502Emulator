#include "platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#else
#include <chrono>
#include <thread>
#endif

#include <fstream>

#ifdef _WIN32

namespace platform {

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string settingsDirectory() {
    // Portable mode: every generated file (INI, savestates, screenshots,
    // WAV captures) lives in ONE folder - the one containing the .exe.
    // Nothing is written to %APPDATA%.
    return executableDirectory();
}

std::string executableDirectory() {
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    const std::size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash);
    else dir = ".";
    return dir;
}

std::string longPathSafe(const std::string& utf8Path) {
    if (utf8Path.size() >= 2 && utf8Path[0] == '\\' && utf8Path[1] == '\\') {
        // UNC path: \\?\UNC\server\share\...
        return "\\\\?\\UNC\\" + utf8Path.substr(2);
    }
    if (!utf8Path.empty() && utf8Path[1] == ':') return "\\\\?\\" + utf8Path;
    return utf8Path; // relative paths are handled by the CWD
}

bool readFileBinary(const std::string& utf8Path, std::vector<uint8_t>& out) {
    // Long-path safe open via the wide API.
    const std::wstring wide = utf8ToWide(longPathSafe(utf8Path));
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) { CloseHandle(handle); return false; }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = size.QuadPart == 0 ||
                    (ReadFile(handle, out.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
                     read == static_cast<DWORD>(size.QuadPart));
    CloseHandle(handle);
    return ok != FALSE;
}

bool writeFileBinary(const std::string& utf8Path, const void* data, std::size_t size) {
    const std::wstring wide = utf8ToWide(longPathSafe(utf8Path));
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = size == 0 || WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(handle);
    return ok != FALSE && written == size;
}

bool fileExists(const std::string& utf8Path) {
    const std::wstring wide = utf8ToWide(utf8Path);
    DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

OpenDialogResult openFileDialog(void* parentHwnd, const char* filter, const char* title) {
    OpenDialogResult result;
    OPENFILENAMEW ofn{};
    wchar_t buffer[MAX_PATH * 2]{};
    static std::wstring filterStorage;
    filterStorage = utf8ToWide(filter ? filter : "All Files\0*.*\0");
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(parentHwnd);
    ofn.lpstrFilter = filterStorage.c_str();
    ofn.lpstrTitle = nullptr;
    const std::wstring titleWide = utf8ToWide(title ? title : "");
    if (!titleWide.empty()) ofn.lpstrTitle = titleWide.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>((sizeof(buffer) / sizeof(wchar_t)) - 1);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    result.confirmed = GetOpenFileNameW(&ofn) != FALSE;
    if (result.confirmed) result.pathUtf8 = wideToUtf8(buffer);
    return result;
}

OpenDialogResult saveFileDialog(void* parentHwnd, const char* filter, const char* title) {
    OpenDialogResult result;
    OPENFILENAMEW ofn{};
    wchar_t buffer[MAX_PATH * 2]{};
    static std::wstring filterStorage;
    filterStorage = utf8ToWide(filter ? filter : "All Files\0*.*\0");
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(parentHwnd);
    ofn.lpstrFilter = filterStorage.c_str();
    const std::wstring titleWide = utf8ToWide(title ? title : "");
    if (!titleWide.empty()) ofn.lpstrTitle = titleWide.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>((sizeof(buffer) / sizeof(wchar_t)) - 1);
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    result.confirmed = GetSaveFileNameW(&ofn) != FALSE;
    if (result.confirmed) result.pathUtf8 = wideToUtf8(buffer);
    return result;
}

uint64_t steadyMillis() {
    return static_cast<uint64_t>(GetTickCount64());
}

void sleepMillis(int ms) {
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
}

} // namespace platform

#else // !defined(_WIN32)

namespace platform {

std::wstring utf8ToWide(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (unsigned char c : text) out.push_back(static_cast<wchar_t>(c)); // ASCII-safe stub
    return out;
}
std::string wideToUtf8(const std::wstring& text) {
    return std::string(text.begin(), text.end());
}
std::string settingsDirectory() { return "."; }
std::string executableDirectory() { return "."; }
std::string longPathSafe(const std::string& p) { return p; }
bool readFileBinary(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
bool writeFileBinary(const std::string& path, const void* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}
bool fileExists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}
OpenDialogResult openFileDialog(void*, const char*, const char*) { return {}; }
OpenDialogResult saveFileDialog(void*, const char*, const char*) { return {}; }
uint64_t steadyMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
void sleepMillis(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace platform

#endif
