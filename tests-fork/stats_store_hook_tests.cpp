// Run the actual Linux call-site installer and dispatch on 32-bit x86. A small
// executable image stands in for Steam and jumps into the test SLSsteam.so.
// The old 2.6.6.2 installer rejects that jump, reproducing the regression.
#include "stats_store_hook_validation.h"
#ifndef CR_SOURCE_UNDER_TEST
#define CR_SOURCE_UNDER_TEST "../src/platform/linux/stats_store_hook.cpp"
#endif
#include CR_SOURCE_UNDER_TEST
#include <dlfcn.h>
#include <cstdarg>
#include <array>

static int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
static uint32_t account = 42;
static bool managed = true, parseOk = true;
static int stores = 0, parses = 0;
namespace Log {
void Write(const char* fmt, ...) {
    va_list args; va_start(args, fmt); std::vfprintf(stderr, fmt, args); va_end(args);
    std::fputc('\n', stderr);
}
}
namespace CloudIntercept { uint32_t GetAccountId() { return account; } }
namespace StatsHandlers {
std::optional<std::vector<uint8_t>> HandleLegacyStoreUserStats2(
    const uint8_t* body, size_t size, uint64_t steamId) {
    ++stores;
    CHECK(size == 2 && body[0] == 0x44 && body[1] == 0x55);
    CHECK(steamId == 76561197960265728ull + account);
    if (!managed) return std::nullopt; // unmanaged game / disabled feature
    return std::vector<uint8_t>{1, 2, 3};
}
}

static void ValidationCases() {
    using namespace StatsStoreHook::Validation;
    const uint8_t native[] = {0x55,0x57,0x56,0x53,0xe8};
    CHECK(ClassifyEntry(0x1000, native, 5, [](uint32_t) { return false; }) == EntryKind::Native);
    CHECK(ClassifyEntry(0x1000, native, 4, [](uint32_t) { return true; }) == EntryKind::Unsupported);
    uint8_t jump[] = {0xe9, 0, 0, 0, 0};
    uint32_t rel = 0x2000u - (0x1000u + 5u);
    std::memcpy(jump + 1, &rel, 4);
    CHECK(JumpDestination(0x1000, jump, 5) == 0x2000);
    CHECK(ClassifyEntry(0x1000, jump, 5, [](uint32_t a) { return a == 0x2000; }) == EntryKind::SlsDetour);
    CHECK(ClassifyEntry(0x1000, jump, 5, [](uint32_t) { return false; }) == EntryKind::Unsupported);
    rel = 0xc0000000u - (0xf0000000u + 5u);
    std::memcpy(jump + 1, &rel, 4);
    CHECK(JumpDestination(0xf0000000, jump, 5) == 0xc0000000u);
    CHECK(!JumpDestination(0, native, 5));
    CHECK(!JumpDestination(0, jump, 4));
    CHECK(IsSlsMapping("r-xp", "/home/deck/.local/share/SLSsteam/SLSsteam.so"));
    CHECK(IsSlsMapping("r-xp", "/home/deck/SLSsteam.so (deleted)"));
    CHECK(!IsSlsMapping("rw-p", "/home/deck/SLSsteam.so"));
    CHECK(!IsSlsMapping("r-xp", "/home/deck/other.so"));
    CHECK(!IsSlsMapping("r-xp", "/home/deck/not-SLSsteam.so"));
    CHECK(!IsSlsMapping("r-xp", "/home/deck/SLSsteam.so.extra"));
}

int main(int argc, char** argv) {
    ValidationCases();
    if (argc != 2 || sizeof(void*) != 4) {
        std::fprintf(stderr, "Requires a 32-bit build and the SLS test fixture path\n");
        return 1;
    }
    void* fixture = dlopen(argv[1], RTLD_NOW);
    if (!fixture) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto sls = reinterpret_cast<StatsStoreHook::SendWait>(dlsym(fixture, "SlsTestSendWait"));
    auto count = reinterpret_cast<unsigned(*)()>(dlsym(fixture, "SlsTestCallCount"));
    CHECK(sls && count);
    if (!sls || !count) return 1;
    auto* image = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (image == MAP_FAILED) return 1;
    std::memset(image, 0x90, 4096);
    const uint8_t site[] = {
        0x68,0x35,0x03,0,0,0x57,0x6a,0x0a,0x05,0x84,0x01,0,0,
        0x6a,0x01,0x50,0xff,0x75,0x08,0xe8,0,0,0,0,
        0x89,0xc1,0x88,0x85,0x63,0xff,0xff,0xff,0x8b,0x45,0x08,0x83,0xc4,0x20
    };
    std::memcpy(image + 16, site, sizeof(site));
    uint8_t* call = image + 16 + 19;
    uint8_t* entry = image + 256;
    int32_t rel = int32_t(uintptr_t(entry) - uintptr_t(call + 5));
    std::memcpy(call + 1, &rel, 4);
    entry[0] = 0xe9;
    rel = int32_t(uintptr_t(sls) - uintptr_t(entry + 5));
    std::memcpy(entry + 1, &rel, 4);
    std::array<uint8_t,5> savedCall{}, savedEntry{};
    std::memcpy(savedCall.data(), call, 5);
    std::memcpy(savedEntry.data(), entry, 5);
    auto serialize = [](void*) { return std::vector<uint8_t>{0x44, 0x55}; };
    auto parse = [](void*, const uint8_t* data, size_t size) {
        ++parses; CHECK(size == 3 && data[0] == 1 && data[2] == 3); return parseOk;
    };
    const bool installed = StatsStoreHook::Install(uintptr_t(image), 4096, serialize, parse);
    CHECK(installed); // This is the regression: 2.6.6.2 returns false here.
    if (installed) {
        CHECK(std::memcmp(entry, savedEntry.data(), 5) == 0); // never rewrite SLS's entry
        std::memcpy(&rel, call + 1, 4);
        auto invoke = reinterpret_cast<StatsStoreHook::SendWait>(uintptr_t(call + 5) + rel);
        CHECK(invoke == &StatsStoreHook::OnStore);
        alignas(void*) uint8_t req[48]{}, resp[48]{};
        uint32_t msg = 5466; std::memcpy(req + 20, &msg, 4);
        void* dummy = req; std::memcpy(req + 32, &dummy, sizeof(dummy));
        dummy = resp; std::memcpy(resp + 32, &dummy, sizeof(dummy));
        CHECK(invoke(nullptr, req, 1, 10, resp, 821));
        CHECK(stores == 1 && parses == 1 && count() == 0);
        managed = false;
        CHECK(invoke(nullptr, req, 1, 10, resp, 821));
        CHECK(count() == 1); // pass-through traverses the ORIGINAL entry, then SLS
        managed = true; parseOk = false;
        CHECK(!invoke(nullptr, req, 1, 10, resp, 821));
        CHECK(count() == 1); // never forward an already committed write after parse failure
        parseOk = true; account = 0;
        CHECK(invoke(nullptr, req, 1, 10, resp, 821));
        CHECK(count() == 2);
        account = 42; msg = 5410; std::memcpy(req + 20, &msg, 4);
        CHECK(invoke(nullptr, req, 1, 10, resp, 821));
        CHECK(count() == 3); // non-stats messages unchanged
        CHECK(StatsStoreHook::g_active.load() == 0);
        StatsStoreHook::Remove();
        CHECK(std::memcmp(call, savedCall.data(), 5) == 0);
        CHECK(std::memcmp(entry, savedEntry.data(), 5) == 0);
    }
    // Reject a jump into another executable module, with no partial patch.
    rel = int32_t(uintptr_t(&std::puts) - uintptr_t(entry + 5));
    std::memcpy(entry + 1, &rel, 4);
    CHECK(!StatsStoreHook::Install(uintptr_t(image), 4096, serialize, parse));
    CHECK(std::memcmp(call, savedCall.data(), 5) == 0);
    munmap(image, 4096);
    dlclose(fixture);
    std::printf("stats_store_hook_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
