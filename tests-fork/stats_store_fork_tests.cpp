// Fork regression tests for stats_store.cpp: cross-device playtime
// (ApplyLocalconfigPlaytime and the migrated-bucket adoption in MergePlaytime)
// and the legacy per-app blob migration in SeedApps; plus the Player.GetUserStats
// answer in stats_handlers.cpp.
// No framework: each CHECK reports a failure with file/line; the exit code is
// the number of failures. Built and run by .github/workflows/windows-release.yml.
#include "stats_store.h"
#include "stats_handlers.h"
#include "metadata_sync.h"
#include "protobuf.h"
#include "json.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using StatsStore::DevicePlaytime;
using StatsStore::PlaytimeData;

static int g_failed = 0;
static int g_passed = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// "Own" is the platform this binary runs on; "Other" is a platform that can only
// be written by another device. The tests never depend on which OS runs them.
#ifdef _WIN32
static uint32_t& Own(DevicePlaytime& d)   { return d.windows; }
static uint32_t& Other(DevicePlaytime& d) { return d.lin; }
static const char* kOwnName = "windows";
static const char* kOtherName = "linux";
#elif defined(__APPLE__)
static uint32_t& Own(DevicePlaytime& d)   { return d.mac; }
static uint32_t& Other(DevicePlaytime& d) { return d.lin; }
static const char* kOwnName = "mac";
static const char* kOtherName = "linux";
#else
static uint32_t& Own(DevicePlaytime& d)   { return d.lin; }
static uint32_t& Other(DevicePlaytime& d) { return d.windows; }
static const char* kOwnName = "linux";
static const char* kOtherName = "windows";
#endif

static const char* kMigrated = "__migrated_localconfig";

// ---- ApplyLocalconfigPlaytime -------------------------------------------------

// The user's actual situation: the Deck's 1597 pre-tracking minutes (adopted
// from the cloud) plus 1 live minute, and Windows's own localconfig says 22.
static void ReconcileKeepsOtherDevicesMinutes() {
    PlaytimeData pt;
    Other(pt.perDevice[kMigrated]) = 1597;
    Other(pt.perDevice["steamdeck"]) = 1;
    StatsStore::ApplyLocalconfigPlaytime(1903340, pt, /*vdf*/22, /*2wks*/0);
    CHECK(pt.minutesForever == 1598);
    CHECK(pt.perDevice.count(kMigrated) == 1);
    CHECK(Other(pt.perDevice[kMigrated]) == 1597);
    CHECK(Own(pt.perDevice[kMigrated]) == 0);
    CHECK(Other(pt.perDevice["steamdeck"]) == 1);
}

// Steam's figure exceeds everything known elsewhere: the excess is ours.
static void ReconcileAddsShortfallToOwnField() {
    PlaytimeData pt;
    Other(pt.perDevice[kMigrated]) = 1597;
    Other(pt.perDevice["steamdeck"]) = 1;
    StatsStore::ApplyLocalconfigPlaytime(1903340, pt, /*vdf*/2000, /*2wks*/0);
    CHECK(Own(pt.perDevice[kMigrated]) == 402);
    CHECK(Other(pt.perDevice[kMigrated]) == 1597);
    CHECK(pt.minutesForever == 2000);
}

// Re-running with a lower Steam figure shrinks only our own field (the repair
// path upstream relies on) and never touches the other platform's field.
static void ReconcileShrinksOnlyOwnField() {
    PlaytimeData pt;
    Own(pt.perDevice[kMigrated]) = 402;
    Other(pt.perDevice[kMigrated]) = 1597;
    Other(pt.perDevice["steamdeck"]) = 1;
    StatsStore::ApplyLocalconfigPlaytime(1903340, pt, /*vdf*/1700, /*2wks*/0);
    CHECK(Own(pt.perDevice[kMigrated]) == 102);
    CHECK(Other(pt.perDevice[kMigrated]) == 1597);
    CHECK(pt.minutesForever == 1700);
}

// A bucket that only ever held our own field is still dropped once real
// device buckets cover Steam's figure (upstream behaviour preserved).
static void ReconcileErasesEmptyBucket() {
    PlaytimeData pt;
    Own(pt.perDevice[kMigrated]) = 21;
    Own(pt.perDevice["DESKTOP-A"]) = 30;
    StatsStore::ApplyLocalconfigPlaytime(1, pt, /*vdf*/10, /*2wks*/0);
    CHECK(pt.perDevice.count(kMigrated) == 0);
    CHECK(pt.minutesForever == 30);
}

// First launch on a fresh device: the whole localconfig figure is ours.
static void ReconcileCreatesBucketWhenMissing() {
    PlaytimeData pt;
    StatsStore::ApplyLocalconfigPlaytime(1, pt, /*vdf*/22, /*2wks*/7);
    CHECK(pt.perDevice.count(kMigrated) == 1);
    CHECK(Own(pt.perDevice[kMigrated]) == 22);
    CHECK(pt.minutesForever == 22);
    CHECK(pt.minutesLastTwoWeeks == 7);
}

// Our own live bucket is part of what Steam's figure already covers.
static void ReconcileCountsOwnLiveBucket() {
    PlaytimeData pt;
    Own(pt.perDevice["DESKTOP-A"]) = 60;
    Other(pt.perDevice[kMigrated]) = 1597;
    StatsStore::ApplyLocalconfigPlaytime(1, pt, /*vdf*/82, /*2wks*/0);
    CHECK(pt.perDevice.count(kMigrated) == 1);
    CHECK(Own(pt.perDevice[kMigrated]) == 0);
    CHECK(pt.minutesForever == 1657);
}

// ---- MergePlaytime via MergeAppStatsJson ------------------------------------

static std::string Dev(const char* key, uint32_t own, uint32_t other) {
    return std::string("\"") + key + "\":{\"" + kOwnName + "\":" + std::to_string(own) +
           ",\"" + kOtherName + "\":" + std::to_string(other) + "}";
}

static std::string Entry(const std::string& perDevice, uint32_t forever) {
    return "{\"crc_stats\":0,\"stats\":[],\"achievements\":[],\"playtime\":{"
           "\"minutes_forever\":" + std::to_string(forever) +
           ",\"minutes_2weeks\":0,\"last_played\":1,\"windows\":0,\"mac\":0,\"linux\":0,"
           "\"per_device\":{" + perDevice + "}}}";
}

static uint32_t Field(const Json::Value& root, const char* dev, const char* plat) {
    const Json::Value& pd = root["playtime"]["per_device"];
    if (pd.type != Json::Type::Object || !pd.has(dev)) return 0;
    return (uint32_t)pd[dev][plat].integer();
}

static bool HasDevice(const Json::Value& root, const char* dev) {
    const Json::Value& pd = root["playtime"]["per_device"];
    return pd.type == Json::Type::Object && pd.has(dev);
}

static uint32_t Forever(const Json::Value& root) {
    return (uint32_t)root["playtime"]["minutes_forever"].integer();
}

// Pull direction (base = local, incoming = cloud): the Deck's minutes arrive.
static void MergeAdoptsOtherPlatformField() {
    std::string local = Entry(Dev(kMigrated, 21, 0) + "," + Dev("steamdeck", 0, 1), 22);
    std::string cloud = Entry(Dev(kMigrated, 0, 1597) + "," + Dev("steamdeck", 0, 1), 1598);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(local, cloud));
    CHECK(Field(r, kMigrated, kOwnName) == 21);
    CHECK(Field(r, kMigrated, kOtherName) == 1597);
    CHECK(Field(r, "steamdeck", kOtherName) == 1);
    CHECK(Forever(r) == 1619);
}

// The own-platform field is reconcile-owned: another device of the same
// platform (or a stale cloud copy) must never overwrite it.
static void MergeNeverAdoptsOwnPlatformField() {
    std::string local = Entry(Dev(kMigrated, 21, 0), 21);
    std::string cloud = Entry(Dev(kMigrated, 500, 0), 500);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(local, cloud));
    CHECK(Field(r, kMigrated, kOwnName) == 21);
    CHECK(Forever(r) == 21);
}

// Other-platform fields merge by max in both orders.
static void MergeOtherPlatformFieldIsMax() {
    std::string a = Entry(Dev(kMigrated, 0, 1597), 1597);
    std::string b = Entry(Dev(kMigrated, 0, 100), 100);
    Json::Value r1 = Json::Parse(StatsStore::MergeAppStatsJson(a, b));
    Json::Value r2 = Json::Parse(StatsStore::MergeAppStatsJson(b, a));
    CHECK(Field(r1, kMigrated, kOtherName) == 1597);
    CHECK(Field(r2, kMigrated, kOtherName) == 1597);
}

// Push direction (base = cloud, incoming = local): a device that never pulled
// (other field still 0) must not erase the cloud's other-platform minutes, and
// its own field is not published.
static void MergePushKeepsCloudOtherField() {
    std::string cloud = Entry(Dev(kMigrated, 0, 1597) + "," + Dev("steamdeck", 0, 1), 1598);
    std::string local = Entry(Dev(kMigrated, 21, 0), 21);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(cloud, local));
    CHECK(Field(r, kMigrated, kOtherName) == 1597);
    CHECK(Field(r, kMigrated, kOwnName) == 0);
    CHECK(Forever(r) == 1598);
}

// A source whose migrated bucket has nothing for other platforms creates no
// bucket on the destination.
static void MergeDoesNotCreateEmptyMigratedBucket() {
    std::string dst = Entry(Dev("DESKTOP-A", 30, 0), 30);
    std::string src = Entry(Dev(kMigrated, 5, 0), 5);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(dst, src));
    CHECK(!HasDevice(r, kMigrated));
    CHECK(Forever(r) == 30);
}

// Real device buckets keep upstream's max-per-device union.
static void MergeRealDevicesUnchanged() {
    std::string a = Entry(Dev("DESKTOP-A", 10, 0), 10);
    std::string b = Entry(Dev("DESKTOP-A", 40, 0) + "," + Dev("steamdeck", 0, 5), 45);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(a, b));
    CHECK(Field(r, "DESKTOP-A", kOwnName) == 40);
    CHECK(Field(r, "steamdeck", kOtherName) == 5);
    CHECK(Forever(r) == 45);
}

// End to end: the state found on the user's Windows box (local file with the
// own field only) after one pull and one reconcile shows the Deck's hours, and
// a second reconcile changes nothing (no flapping between launches).
static void PullThenReconcileShowsDeckHours() {
    std::string local = Entry(Dev(kMigrated, 21, 0) + "," + Dev("steamdeck", 0, 1), 22);
    std::string cloud = Entry(Dev(kMigrated, 0, 1597) + "," + Dev("steamdeck", 0, 1), 1598);
    Json::Value r = Json::Parse(StatsStore::MergeAppStatsJson(local, cloud));
    PlaytimeData pt;
    Own(pt.perDevice[kMigrated]) = Field(r, kMigrated, kOwnName);
    Other(pt.perDevice[kMigrated]) = Field(r, kMigrated, kOtherName);
    Other(pt.perDevice["steamdeck"]) = Field(r, "steamdeck", kOtherName);
    StatsStore::ApplyLocalconfigPlaytime(1903340, pt, /*vdf*/22, /*2wks*/1597);
    CHECK(pt.minutesForever == 1598);
    CHECK(Other(pt.perDevice[kMigrated]) == 1597);
    StatsStore::ApplyLocalconfigPlaytime(1903340, pt, /*vdf*/22, /*2wks*/1597);
    CHECK(pt.minutesForever == 1598);
    CHECK(Other(pt.perDevice[kMigrated]) == 1597);
}

// ---- Legacy per-app blob migration (SeedApps -> MigrateLegacyBlobs) ---------
//
// Drives the real seed against in-memory cloud callbacks. The account blob knows
// app 100; apps 200 and 300 have no entry there, so they are the candidates for
// a legacy per-app blob. Each case records which apps the store still probed one
// by one and what the seed pushed back into the account blob.

using BlobMap = std::unordered_map<uint32_t, std::string>;

struct SeedRun {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint32_t> probed;   // pullLegacy calls, in order
    std::vector<uint32_t> asked;    // apps the lister was asked about
    BlobMap pushed;                 // last pushAll snapshot
    bool pushSeen = false;

    // The account blob is pushed from a detached thread; wait for it (bounded).
    bool WaitForPush() {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::seconds(10), [this] { return pushSeen; });
    }
};

static std::vector<std::filesystem::path> g_tempRoots;

// A legacy per-app entry whose minutes are unmistakable in a snapshot.
static std::string Legacy(uint32_t forever) {
    return Entry(Dev("olddevice", forever, 0), forever);
}

static std::string AccountEntry(uint32_t forever) {
    return Entry(Dev("steamdeck", 0, forever), forever);
}

static uint32_t SeededMinutes(uint32_t appId) {
    return StatsStore::Snapshot(appId).playtime.minutesForever;
}

// Runs SeedApps({100, 200, 300}) on a fresh store rooted in a temp directory.
// `listing` == nullptr means no lister is configured; otherwise the lister
// returns *listing with the given verdict. `probes` is what a per-app probe finds.
static std::shared_ptr<SeedRun> RunSeed(int caseNo, BlobMap blob, const BlobMap* listing,
                                        bool listingOk, bool listingComplete, BlobMap probes) {
    namespace fs = std::filesystem;
    auto run = std::make_shared<SeedRun>();
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
        ("cr_fork_tests_" + std::to_string(stamp) + "_" + std::to_string(caseNo));
    std::error_code ec;
    fs::create_directories(root / "steam", ec);
    g_tempRoots.push_back(root);

    StatsStore::ResetForTesting();
    StatsStore::Init((root / "cloud").string(), (root / "steam").string());
    StatsStore::SetAccountIdProvider([] { return 1397805883u; });
    StatsStore::SetCloudProvider(
        [blob](BlobMap& out) { out = blob; return true; },
        [run](const BlobMap& all) {
            std::lock_guard<std::mutex> lock(run->mtx);
            run->pushed = all;
            run->pushSeen = true;
            run->cv.notify_all();
        },
        [run, probes](uint32_t appId) -> std::string {
            {
                std::lock_guard<std::mutex> lock(run->mtx);
                run->probed.push_back(appId);
            }
            auto it = probes.find(appId);
            return it == probes.end() ? std::string() : it->second;
        },
        [](uint32_t) { return std::string(); });
    if (listing) {
        BlobMap copy = *listing;
        StatsStore::SetCloudLegacyLister(
            [run, copy, listingOk, listingComplete](const std::vector<uint32_t>& apps,
                                                    BlobMap& out, bool& complete) {
                run->asked = apps;
                out = copy;
                complete = listingComplete;
                return listingOk;
            });
    } else {
        StatsStore::SetCloudLegacyLister(nullptr);
    }
    StatsStore::SeedApps({100, 200, 300});
    return run;
}

// Google Drive's complete listing: the one app that has a legacy blob is
// migrated straight from the listing and nobody is probed individually.
static void LegacyListingCompleteMigratesWithoutProbes() {
    BlobMap listing = {{200, Legacy(777)}};
    BlobMap probes = {{300, Legacy(999)}};   // must never be consulted
    auto run = RunSeed(1, {{100, AccountEntry(50)}}, &listing, true, true, probes);
    CHECK((run->asked == std::vector<uint32_t>{200, 300}));   // never the blob's own apps
    CHECK(run->probed.empty());
    CHECK(SeededMinutes(100) == 50);
    CHECK(SeededMinutes(200) == 777);
    CHECK(SeededMinutes(300) == 0);
    CHECK(run->WaitForPush());
    CHECK(run->pushed.count(200) == 1);
    CHECK(run->pushed.count(300) == 0);
}

// A listing that cannot vouch for completeness (a lost page, a failed download)
// still migrates what it found, and every other candidate is probed exactly as
// before, so a blob the listing missed is still recovered.
static void LegacyListingIncompleteProbesTheRest() {
    BlobMap listing = {{200, Legacy(777)}};
    BlobMap probes = {{300, Legacy(999)}};
    auto run = RunSeed(2, {{100, AccountEntry(50)}}, &listing, true, false, probes);
    CHECK((run->probed == std::vector<uint32_t>{300}));
    CHECK(SeededMinutes(200) == 777);
    CHECK(SeededMinutes(300) == 999);
    CHECK(run->WaitForPush());
    CHECK(run->pushed.count(200) == 1);
    CHECK(run->pushed.count(300) == 1);
}

// A failed listing is ignored entirely, even if it returned something, and the
// seed behaves exactly like upstream: one probe per candidate.
static void LegacyListingFailedProbesEveryCandidate() {
    BlobMap listing = {{200, Legacy(777)}};   // came with "failed": must be ignored
    BlobMap probes = {{200, Legacy(555)}, {300, Legacy(999)}};
    auto run = RunSeed(3, {{100, AccountEntry(50)}}, &listing, false, true, probes);
    CHECK((run->probed == std::vector<uint32_t>{200, 300}));
    CHECK(SeededMinutes(200) == 555);
    CHECK(SeededMinutes(300) == 999);
    CHECK(run->WaitForPush());
    CHECK(run->pushed.count(200) == 1);
    CHECK(run->pushed.count(300) == 1);
}

// No lister at all (S3, local disk, an older wiring): upstream behaviour.
static void NoLegacyListerProbesEveryCandidate() {
    BlobMap probes = {{300, Legacy(999)}};
    auto run = RunSeed(4, {{100, AccountEntry(50)}}, nullptr, false, false, probes);
    CHECK((run->probed == std::vector<uint32_t>{200, 300}));
    CHECK(SeededMinutes(200) == 0);
    CHECK(SeededMinutes(300) == 999);
    CHECK(run->WaitForPush());
    CHECK(run->pushed.count(300) == 1);
}

// The account blob always wins: a stale legacy blob for an app that already
// has an account-blob entry is neither adopted nor probed for.
static void LegacyListingNeverOverridesAccountBlob() {
    BlobMap listing = {{100, Legacy(5000)}};
    auto run = RunSeed(5, {{100, AccountEntry(50)}}, &listing, true, true, {});
    CHECK(run->probed.empty());
    CHECK(SeededMinutes(100) == 50);
    CHECK(SeededMinutes(200) == 0);
    CHECK(SeededMinutes(300) == 0);
}

// --- Player.GetUserStats answers (stats_handlers.cpp) ---

static constexpr uint32_t kStatsApp = 2458860;
static constexpr uint32_t kStatsAccount = 1397805883u;
static constexpr uint64_t kStatsSteamId = 76561197960265728ull + kStatsAccount;

// The schema blob is opaque to the handler; "abc" has a well-known SHA-1, which
// also pins the hash to plain SHA-1 over the raw bytes (what Steam sends).
static const std::vector<uint8_t> kSchema = {'a', 'b', 'c'};
static const std::vector<uint8_t> kSchemaSha = {
    0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};

struct UserStatsReply {
    bool answered = false;        // false: passed through to Steam
    std::vector<uint8_t> sha;     // sha_schema (1)
    bool hasCrc = false;
    uint32_t crc = 0;             // crc_stats (2)
    bool hasSchema = false;       // schema (3)
    size_t stats = 0;             // stats (4)
};

// A fresh store for kStatsAccount holding kSchema for kStatsApp and, if
// withStats, one achievement stat. Returns the store's crc_stats.
static uint32_t SetUpStatsStore(int caseNo, bool withStats) {
    namespace fs = std::filesystem;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
        ("cr_fork_userstats_" + std::to_string(stamp) + "_" + std::to_string(caseNo));
    std::error_code ec;
    fs::create_directories(root / "steam", ec);
    g_tempRoots.push_back(root);

    StatsStore::ResetForTesting();
    StatsStore::Init((root / "cloud").string(), (root / "steam").string());
    StatsStore::SetAccountIdProvider([] { return kStatsAccount; });
    StatsStore::ResetForAccountSwitch(kStatsAccount);
    MetadataSync::syncAchievements.store(false);   // no seed to wait for
    StatsStore::SetSchema(kStatsApp, kSchema.data(), kSchema.size());
    if (!withStats)
        return StatsStore::Snapshot(kStatsApp).crcStats;
    StatsStore::SetStats(kStatsApp, {{1, 0x3}});
    return StatsStore::SetAchievement(kStatsApp, 1, 0, 1759600000);
}

// steamId 0 / sha nullptr leave the field out of the request.
static UserStatsReply AskUserStats(uint64_t steamId, const std::vector<uint8_t>* sha, uint32_t crc) {
    PB::Writer req;
    if (steamId) req.WriteVarint(1, steamId);
    req.WriteVarint(2, kStatsApp);
    if (sha) req.WriteBytes(3, sha->data(), sha->size());
    req.WriteVarint(4, crc);
    std::vector<uint8_t> reqBytes = req.Data();
    auto res = StatsHandlers::HandleGetUserStats(kStatsApp, PB::Parse(reqBytes.data(), reqBytes.size()));

    UserStatsReply r;
    std::vector<uint8_t> body = res.body.Data();
    r.answered = !body.empty();
    for (const auto& f : PB::Parse(body.data(), body.size())) {
        if (f.fieldNum == 1) r.sha.assign(f.data, f.data + f.dataLen);
        else if (f.fieldNum == 2) { r.hasCrc = true; r.crc = (uint32_t)f.varintVal; }
        else if (f.fieldNum == 3) r.hasSchema = true;
        else if (f.fieldNum == 4) ++r.stats;
    }
    return r;
}

// Same schema, same stats: nothing to send but the tokens.
static void UserStatsInSyncIsNoOp() {
    uint32_t crc = SetUpStatsStore(1, true);
    auto r = AskUserStats(kStatsSteamId, &kSchemaSha, crc);
    CHECK(r.answered);
    CHECK(r.sha == kSchemaSha);
    CHECK(r.hasCrc && r.crc == crc);
    CHECK(!r.hasSchema);
    CHECK(r.stats == 0);
}

// The 0% global-unlock-rate regression: a stats change (every unlock) must not
// re-send a schema the client already holds, or Steam reloads it mid-session.
static void UserStatsChangeKeepsKnownSchema() {
    uint32_t crc = SetUpStatsStore(2, true);
    auto r = AskUserStats(kStatsSteamId, &kSchemaSha, crc ^ 0x5a5a5a5au);
    CHECK(r.answered);
    CHECK(!r.hasSchema);
    CHECK(r.stats == 1);
    CHECK(r.hasCrc && r.crc == crc);
    CHECK(r.sha == kSchemaSha);
}

// A client without a schema gets it even when its crc already matches.
static void UserStatsMissingSchemaIsSent() {
    uint32_t crc = SetUpStatsStore(3, true);
    auto r = AskUserStats(kStatsSteamId, nullptr, crc);
    CHECK(r.answered);
    CHECK(r.hasSchema);
    CHECK(r.stats == 0);
    CHECK(r.sha == kSchemaSha);
}

// Both crcs zero (no stats yet) is not proof the client has the schema.
static void UserStatsZeroCrcStillSendsSchema() {
    uint32_t crc = SetUpStatsStore(4, false);
    auto r = AskUserStats(kStatsSteamId, nullptr, crc);
    CHECK(r.answered);
    CHECK(r.hasSchema);
    CHECK(r.hasCrc && r.crc == crc);
}

// A client holding a different schema gets ours (stats unchanged: none sent).
static void UserStatsDifferentSchemaIsReplaced() {
    uint32_t crc = SetUpStatsStore(5, true);
    std::vector<uint8_t> otherSha(20, 0xab);
    auto r = AskUserStats(kStatsSteamId, &otherSha, crc);
    CHECK(r.answered);
    CHECK(r.hasSchema);
    CHECK(r.stats == 0);
    CHECK(r.sha == kSchemaSha);
}

// Another player's stats are not ours to answer; a request without a steamid is.
static void UserStatsOtherAccountPassesThrough() {
    uint32_t crc = SetUpStatsStore(6, true);
    auto other = AskUserStats(kStatsSteamId + 1, &kSchemaSha, crc ^ 1u);
    CHECK(!other.answered);
    auto unnamed = AskUserStats(0, &kSchemaSha, crc ^ 1u);
    CHECK(unnamed.answered);
    CHECK(unnamed.stats == 1);
}

static PB::Writer StoreRequest(uint64_t account = kStatsSteamId, bool reset = false) {
    PB::Writer req, stat;
    req.WriteFixed64(1, kStatsApp);
    req.WriteFixed64(2, kStatsSteamId);
    req.WriteFixed64(3, account);
    req.WriteVarint(4, 0xdeadbeef); // the old server token is not our authority
    req.WriteVarint(5, reset);
    stat.WriteVarint(1, 1);
    stat.WriteVarint(2, 7);
    req.WriteSubmessage(6, stat);
    return req;
}

static void StoreReplyEndsOutOfDateLoop() {
    SetUpStatsStore(10, true);
    StatsHandlers::SetNamespacePredicate([](uint32_t app) { return app == kStatsApp; });
    MetadataSync::syncAchievements.store(true);
    auto req = StoreRequest();
    auto response = StatsHandlers::HandleLegacyStoreUserStats2(
        req.Data().data(), req.Size(), kStatsSteamId);
    CHECK(response.has_value());
    if (!response) return;
    const auto fields = PB::Parse(response->data(), response->size());
    CHECK(PB::FindField(fields, 2)->varintVal == 1);
    CHECK(PB::FindField(fields, 5)->varintVal == 0);
    const uint32_t crc = uint32_t(PB::FindField(fields, 3)->varintVal);
    auto saved = StatsStore::Snapshot(kStatsApp);
    CHECK(saved.crcStats == crc);
    CHECK(saved.stats[0].value == 7);
    CHECK(saved.achievements[0].bits == 7);
    CHECK(saved.achievements[0].unlockTimes[0] == 1759600000);
    CHECK(saved.achievements[0].unlockTimes[2] != 0);
    StatsStore::AppStats disk{};
    CHECK(StatsStore::LoadAppStats(kStatsApp, disk));
    CHECK(disk.crcStats == crc);
    CHECK(disk.achievements[0].unlockTimes[2] == saved.achievements[0].unlockTimes[2]);
    // Retrying the same transaction must not change unlock times / CRC.
    auto again = StatsHandlers::HandleLegacyStoreUserStats2(req.Data().data(), req.Size(), kStatsSteamId);
    CHECK(again == response);
    MetadataSync::syncAchievements.store(false); // skip seed wait in read fixture
    auto read = AskUserStats(kStatsSteamId, &kSchemaSha, crc);
    CHECK(read.answered && read.crc == crc && read.stats == 0 && !read.hasSchema);
}

static void StoreRejectsUnmanagedAndOtherAccounts() {
    const auto old = SetUpStatsStore(11, true);
    auto req = StoreRequest();
    auto send = [&](const PB::Writer& q) {
        return StatsHandlers::HandleLegacyStoreUserStats2(q.Data().data(), q.Size(), kStatsSteamId);
    };
    MetadataSync::syncAchievements.store(false);
    CHECK(!send(req));
    MetadataSync::syncAchievements.store(true);
    StatsHandlers::SetNamespacePredicate([](uint32_t) { return false; });
    CHECK(!send(req));
    StatsHandlers::SetNamespacePredicate([](uint32_t app) { return app == kStatsApp; });
    CHECK(!send(StoreRequest(kStatsSteamId + 1)));
    PB::Writer malformed = StoreRequest();
    PB::Writer invalidStat;
    invalidStat.WriteVarint(1, 9); // missing value: no partial mutation
    malformed.WriteSubmessage(6, invalidStat);
    CHECK(!send(malformed));
    CHECK(StatsStore::Snapshot(kStatsApp).crcStats == old);
    MetadataSync::syncAchievements.store(false);
}

static void StoreDiskFailureIsNotAcknowledged() {
    const uint32_t before = SetUpStatsStore(12, true);
    const auto root = g_tempRoots.back();
    const auto path = root / "cloud" / "stats" / std::to_string(kStatsAccount)
                     / (std::to_string(kStatsApp) + ".json");
    if (std::filesystem::exists(path)) std::filesystem::rename(path, path.string() + ".saved");
    std::filesystem::create_directory(path); // directory is never replaceable by atomic file rename
    MetadataSync::syncAchievements.store(true);
    auto req = StoreRequest();
    auto response = StatsHandlers::HandleLegacyStoreUserStats2(req.Data().data(), req.Size(), kStatsSteamId);
    CHECK(response.has_value());
    if (response) {
        auto fields = PB::Parse(response->data(), response->size());
        CHECK(PB::FindField(fields, 2)->varintVal == 2);
        CHECK(!PB::FindField(fields, 3));
    }
    CHECK(StatsStore::Snapshot(kStatsApp).crcStats == before);
    MetadataSync::syncAchievements.store(false);
}

static void StoreResetKeepsSchemaAndPlaytime() {
    SetUpStatsStore(13, true);
    auto before = StatsStore::Snapshot(kStatsApp);
    auto crc = StatsStore::CommitClientStats(kStatsApp, {}, true, kStatsAccount);
    CHECK(crc && *crc == 0);
    auto after = StatsStore::Snapshot(kStatsApp);
    CHECK(after.stats.empty() && after.achievements.empty());
    CHECK(after.schema == before.schema);
    CHECK(after.playtime.minutesForever == before.playtime.minutesForever);
}

static void StoreFirstUnlockUsesSchemaAndPreservesNumericStats() {
    SetUpStatsStore(14, false);
    std::vector<uint8_t> schema;
    auto field = [&](uint8_t type, const std::string& name) {
        schema.push_back(type);
        schema.insert(schema.end(), name.begin(), name.end());
        schema.push_back(0);
    };
    field(0, std::to_string(kStatsApp)); field(0, "stats"); field(0, "3");
    field(0, "bits"); field(0, "0"); field(1, "name");
    const std::string name = "PROLOGUE_COMPLETE";
    schema.insert(schema.end(), name.begin(), name.end()); schema.push_back(0);
    for (int i = 0; i < 6; ++i) schema.push_back(8);
    StatsStore::SetSchema(kStatsApp, schema.data(), schema.size());
    CHECK(!StatsStore::CommitClientStats(kStatsApp, {{3, 1}}, false, kStatsAccount + 1));
    auto crc = StatsStore::CommitClientStats(kStatsApp, {{3, 1}, {99, 123}}, false, kStatsAccount);
    CHECK(crc.has_value());
    auto saved = StatsStore::Snapshot(kStatsApp);
    CHECK(saved.achievements.size() == 1);
    CHECK(saved.achievements[0].statId == 3);
    CHECK(saved.achievements[0].names[0] == name);
    CHECK(saved.achievements[0].unlockTimes[0] != 0);
    CHECK(saved.stats.size() == 2 && saved.stats[1].value == 123);
    auto nextCrc = StatsStore::CommitClientStats(kStatsApp, {{99, 5}}, false, kStatsAccount);
    CHECK(nextCrc && *nextCrc != *crc);
    CHECK(StatsStore::Snapshot(kStatsApp).stats[1].value == 5); // genuine numeric decreases remain valid
    StatsStore::AppStats disk{};
    CHECK(StatsStore::LoadAppStats(kStatsApp, disk));
    CHECK(disk.schema == schema);
    CHECK(disk.achievements[0].unlockTimes[0] == saved.achievements[0].unlockTimes[0]);
}

int main() {
    ReconcileKeepsOtherDevicesMinutes();
    ReconcileAddsShortfallToOwnField();
    ReconcileShrinksOnlyOwnField();
    ReconcileErasesEmptyBucket();
    ReconcileCreatesBucketWhenMissing();
    ReconcileCountsOwnLiveBucket();
    MergeAdoptsOtherPlatformField();
    MergeNeverAdoptsOwnPlatformField();
    MergeOtherPlatformFieldIsMax();
    MergePushKeepsCloudOtherField();
    MergeDoesNotCreateEmptyMigratedBucket();
    MergeRealDevicesUnchanged();
    PullThenReconcileShowsDeckHours();
    LegacyListingCompleteMigratesWithoutProbes();
    LegacyListingIncompleteProbesTheRest();
    LegacyListingFailedProbesEveryCandidate();
    NoLegacyListerProbesEveryCandidate();
    LegacyListingNeverOverridesAccountBlob();
    UserStatsInSyncIsNoOp();
    UserStatsChangeKeepsKnownSchema();
    UserStatsMissingSchemaIsSent();
    UserStatsZeroCrcStillSendsSchema();
    UserStatsDifferentSchemaIsReplaced();
    UserStatsOtherAccountPassesThrough();
    StoreReplyEndsOutOfDateLoop();
    StoreRejectsUnmanagedAndOtherAccounts();
    StoreDiskFailureIsNotAcknowledged();
    StoreResetKeepsSchemaAndPlaytime();
    StoreFirstUnlockUsesSchemaAndPreservesNumericStats();
    for (const auto& root : g_tempRoots) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::printf("stats_store_fork_tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed;
}
