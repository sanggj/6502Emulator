// Platform layer tests: UTF conversion, binary file IO, monotonic clock,
// tr() translation table.
#include "platform.hpp"
#include "strings.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int g_plat_pass = 0;
int g_plat_fail = 0;

#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (expr) {                                                                          \
            ++g_plat_pass;                                                                    \
        } else {                                                                             \
            ++g_plat_fail;                                                                    \
            std::fprintf(stderr, "FAIL test_platform.cpp:%d %s\n", __LINE__, #expr);          \
        }                                                                                    \
    } while (0)

namespace {

std::string tempDir() {
    const char *t = std::getenv("TEMP");
    if (t && *t) return std::string(t);
    t = std::getenv("TMP");
    if (t && *t) return std::string(t);
    return ".";
}

} // namespace

int run_test_platform() {
    // ---- UTF-8 <-> UTF-16 round-trip ----
    const std::string utf8 = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xE2\x9C\x93";
    const std::wstring wide = platform::utf8ToWide(utf8);
    CHECK(!wide.empty());
    CHECK(platform::wideToUtf8(wide) == utf8);
    CHECK(platform::utf8ToWide(platform::wideToUtf8(L"abc\xE9")) == std::wstring(L"abc\xE9"));
    CHECK(platform::wideToUtf8(platform::utf8ToWide("plain ascii")) == "plain ascii");
    CHECK(platform::utf8ToWide("").empty());

    // ---- binary file round-trip in temp dir ----
    const std::string path = tempDir() + "\\test_platform_rw.bin";
    std::vector<uint8_t> out;
    for (int i = 0; i < 256; ++i) out.push_back(static_cast<uint8_t>(i));
    CHECK(platform::writeFileBinary(path, out.data(), out.size()));
    CHECK(platform::fileExists(path));
    std::vector<uint8_t> in;
    CHECK(platform::readFileBinary(path, in));
    CHECK(in.size() == out.size());
    CHECK(in == out);
    std::remove(path.c_str());
    CHECK(!platform::fileExists(path));
    CHECK(!platform::fileExists(tempDir() + "\\test_platform_missing_42.bin"));

    // ---- steadyMillis monotonic non-decreasing ----
    uint64_t prev = platform::steadyMillis();
    bool monotonic = true;
    for (int i = 0; i < 500; ++i) {
        const uint64_t now = platform::steadyMillis();
        if (now < prev) monotonic = false;
        prev = now;
    }
    CHECK(monotonic);
    platform::sleepMillis(2);
    CHECK(platform::steadyMillis() >= prev);

    // ---- strings::tr ----
    const std::string title = tr("app.title");
    CHECK(!title.empty());
    const std::string unknown = tr("no.such.key");
    CHECK(!unknown.empty());

    return g_plat_fail;
}
