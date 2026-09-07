// Fork regression tests for stats_store.cpp: cross-device playtime
// (ApplyLocalconfigPlaytime and the migrated-bucket adoption in MergePlaytime)
// and the legacy per-app blob migration in SeedApps.
// No framework: each CHECK reports a failure with file/line; the exit code is
// the number of failures. Built and run by .github/workflows/windows-release.yml.
#include "stats_store.h"
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
    for (const auto& root : g_tempRoots) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::printf("stats_store_fork_tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed;
}
