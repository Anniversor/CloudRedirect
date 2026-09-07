#include "metadata_sync.h"

namespace MetadataSync {

std::atomic<bool> steamToolsPresent{false};
std::atomic<bool> syncLuas{false};
// Default OFF: WIP opt-in features; the user enables them.
std::atomic<bool> syncAchievements{false};
std::atomic<bool> syncPlaytime{false};
// Retired: schema fetching conflicted with non-SteamTools unlock clients. Kept as a
// no-op flag so config parsing and gate checks compile; SchemaFetchEnabled() is always false.
std::atomic<bool> schemaFetch{false};
// Fork: local answers for the achievements page (see metadata_sync.h).
std::atomic<bool> answerUserStats{true};

}
