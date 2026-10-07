#pragma once
#include "stats_hooks.h"

namespace StatsStoreHook {
// Patch only the StoreUserStats job's call to send-and-wait. Keeping the rest
// of the native job intact preserves pending-change cleanup and game callbacks.
bool Install(uintptr_t base, size_t size, StatsHooks::SerializeFn serialize,
             StatsHooks::ParseFn parse);
void Remove();
}
