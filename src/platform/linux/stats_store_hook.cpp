#include "stats_store_hook.h"
#include "stats_handlers.h"
#include "cloud_intercept.h"
#include "log.h"
#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace StatsStoreHook {
using SendWait = bool(*)(void*, void*, int, int, void*, uint32_t);
static SendWait g_original = nullptr;
static StatsHooks::SerializeFn g_serialize;
static StatsHooks::ParseFn g_parse;
static uint8_t* g_call = nullptr;
static uint8_t g_saved[5];
static std::atomic<bool> g_stopping{false};
static std::atomic<unsigned> g_active{0};

static bool OnStore(void* job, void* request, int login, int timeout,
                    void* response, uint32_t responseMsg) {
    struct Guard {
        Guard() { g_active.fetch_add(1); }
        ~Guard() { g_active.fetch_sub(1); }
    } guard;
    if (!g_stopping.load() && request && response && responseMsg == 821) {
        // CProtoBufMsg layout shared with the GamesPlayed observer: emsg +20,
        // MessageLite body +32. Native code has initialized both wrappers.
        const uint32_t msg = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(request) + 20)
                             & 0x7fffffff;
        void* reqBody = *reinterpret_cast<void**>(static_cast<uint8_t*>(request) + 32);
        void* respBody = *reinterpret_cast<void**>(static_cast<uint8_t*>(response) + 32);
        if ((msg == 5466 || msg == 820) && reqBody && respBody) {
            const auto bytes = g_serialize(reqBody);
            const uint32_t account = CloudIntercept::GetAccountId();
            if (!bytes.empty() && account) {
                auto reply = StatsHandlers::HandleLegacyStoreUserStats2(
                    bytes.data(), bytes.size(), 76561197960265728ull + account);
                if (reply) {
                    // On a parse failure do not forward an already committed
                    // local write to a different authority. Native job reports
                    // transport failure and retains its pending changes.
                    const bool ok = g_parse(respBody, reply->data(), reply->size());
                    LOG("[Stats] Local StoreUserStats reply emsg=%u parsed=%d", msg, ok);
                    return ok;
                }
            }
        }
    }
    return g_original(job, request, login, timeout, response, responseMsg);
}

static bool Writable(uint8_t* p) {
    const uintptr_t page = uintptr_t(p) & ~(uintptr_t(sysconf(_SC_PAGESIZE)) - 1);
    const size_t bytes = uintptr_t(p + 5) - page;
    return mprotect(reinterpret_cast<void*>(page), bytes,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool Install(uintptr_t base, size_t size, StatsHooks::SerializeFn serialize,
             StatsHooks::ParseFn parse) {
    if (g_call) return true;
    if (!serialize || !parse || sizeof(void*) != 4) return false;
    // push 821; push edi(reply); push 10; add eax,184h(request);
    // push 1; push eax; push [ebp+8](job); call SendAndWait;
    // mov ecx,eax; mov [ebp-local],al; mov eax,[ebp+8]; add esp,20h.
    // Unique call-site signature, not a hard-coded RVA or a whole-function
    // detour. Relative call target is preserved for every non-managed request.
    static const uint8_t prefix[] = {
        0x68,0x35,0x03,0,0,0x57,0x6a,0x0a,0x05,0x84,0x01,0,0,
        0x6a,0x01,0x50,0xff,0x75,0x08,0xe8
    };
    uint8_t* found = nullptr;
    for (size_t i = 0; i + 43 <= size; ++i) {
        auto* p = reinterpret_cast<uint8_t*>(base + i);
        if (memcmp(p, prefix, sizeof(prefix)) ||
            memcmp(p + 24, "\x89\xc1\x88\x85", 4) ||
            memcmp(p + 32, "\x8b\x45\x08\x83\xc4\x20", 6)) continue;
        if (found) {
            LOG("[Stats] StoreUserStats call signature ambiguous; not installing");
            return false;
        }
        found = p + 19;
    }
    if (!found) {
        LOG("[Stats] StoreUserStats call signature not found; not installing");
        return false;
    }
    int32_t rel;
    memcpy(&rel, found + 1, 4);
    auto* target = found + 5 + rel;
    if (uintptr_t(target) < base || uintptr_t(target) >= base + size ||
        memcmp(target, "\x55\x57\x56\x53\xe8", 5) || !Writable(found)) return false;
    g_original = reinterpret_cast<SendWait>(target);
    g_serialize = std::move(serialize);
    g_parse = std::move(parse);
    g_stopping.store(false);
    memcpy(g_saved, found, 5);
    rel = int32_t(uintptr_t(&OnStore) - uintptr_t(found + 5));
    memcpy(found + 1, &rel, 4);
    __builtin___clear_cache(reinterpret_cast<char*>(found), reinterpret_cast<char*>(found + 5));
    g_call = found;
    LOG("[Stats] StoreUserStats local commit hook installed at %p", found);
    return true;
}

void Remove() {
    g_stopping.store(true);
    if (g_call && Writable(g_call)) {
        memcpy(g_call, g_saved, 5);
        __builtin___clear_cache(reinterpret_cast<char*>(g_call), reinterpret_cast<char*>(g_call + 5));
    }
    for (int i = 0; i < 300 && g_active.load(); ++i) usleep(10000);
    g_call = nullptr;
}
}
