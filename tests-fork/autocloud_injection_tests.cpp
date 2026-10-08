// Exercise the full rule round trip: appinfo -> CR's local resolution -> KV
// injection -> Steam's root overrides. No game files or Steam process needed.
#include "autocloud_injection.h"

#include <cstdio>
#include <map>
#include <memory>

namespace Log { void Write(const char*, ...) {} }

using namespace AutoCloudUtil;
static int passed = 0, failed = 0;
#define CHECK(cond) do { if (cond) ++passed; else { ++failed; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static AutoCloudRuleNative Rule(const char* root, const char* path) {
    AutoCloudRuleNative r;
    r.root = r.cloudRoot = root;
    r.path = r.resolvedPath = path;
    r.pattern = "*.nson";
    r.recursive = true;
    return r;
}

static AutoCloudRuleNative RoundTrip(
    const AutoCloudRuleNative& original,
    const std::vector<AutoCloudRootOverrideNative>& overrides,
    AutoCloudEffectivePlatform scanPlatform,
    AutoCloudEffectivePlatform steamPlatform) {
    auto scanned = original;
    ApplyRootOverridesForPlatform(scanned, overrides, scanPlatform);
    const auto injected = SteamKvInjector::MakeSaveFileRule(scanned);
    // The logical cloud filename and matching rules must not change even when
    // the directory on the current machine is different.
    CHECK(injected.root == original.root);
    CHECK(injected.path == original.path);
    CHECK(injected.pattern == original.pattern);
    CHECK(injected.recursive == original.recursive);
    CHECK(injected.platforms == original.platforms);
    auto steamRule = Rule(injected.root.c_str(), injected.path.c_str());
    ApplyRootOverridesForPlatform(steamRule, overrides, steamPlatform);
    return steamRule;
}

static void SummerClover() {
    const auto original = Rule("gameinstall", "SummerClover/Data/Saves");
    AutoCloudRootOverrideNative ov;
    ov.root = "GameInstall"; // Steam compares roots without case sensitivity.
    ov.os = "Windows";
    ov.osCompare = "=";
    ov.useInstead = "WinAppDataLocalLow";
    ov.addPath = "Connection";
    const auto result = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                                 AutoCloudEffectivePlatform::Windows);
    CHECK(result.root == "WinAppDataLocalLow");
    CHECK(result.resolvedPath == "Connection/SummerClover/Data/Saves");

    // A Proton scan must publish the same logical root/path as Windows; Steam
    // then applies the Windows override in its compatdata prefix.
    const auto proton = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Linux,
                                 AutoCloudEffectivePlatform::Windows);
    CHECK(proton.root == result.root);
    CHECK(proton.resolvedPath == result.resolvedPath);
}

static void PlainRulesStayUnchanged() {
    for (const auto& original : {
            Rule("WinSavedGames", "Sandfall/Expedition33"),
            Rule("WinSavedGames", "CLE/Sora_no_Kiseki_the_1st/savedata"),
            Rule("gameinstall", "savedata"), Rule("LinuxXdgDataHome", "Example/Saves")}) {
        const auto result = RoundTrip(original, {}, AutoCloudEffectivePlatform::Windows,
                                     AutoCloudEffectivePlatform::Windows);
        CHECK(result.root == original.root);
        CHECK(result.resolvedPath == original.path);
    }
}

static void OverrideVariants() {
    auto original = Rule("WinAppDataLocal", "Game/Saves/{Steam3AccountID}");
    original.pattern = "*.sav";
    original.recursive = false;
    original.platforms = 1;
    AutoCloudRootOverrideNative ov;
    ov.root = original.root;
    ov.os = "Windows";
    ov.osCompare = "=";
    ov.useInstead = "WinAppDataRoaming";
    auto result = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                           AutoCloudEffectivePlatform::Windows);
    CHECK(result.root == "WinAppDataRoaming");
    CHECK(result.resolvedPath == original.path); // Root-only override.

    ov.useInstead.clear(); // addpath without changing roots must apply once.
    ov.addPath = "Publisher\\";
    result = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                      AutoCloudEffectivePlatform::Windows);
    CHECK(result.root == original.root);
    CHECK(result.resolvedPath == "Publisher/Game/Saves/{Steam3AccountID}");

    ov.pathTransforms = {{"Saves", "WinSaves"}};
    ov.useInstead = "WinAppDataLocalLow";
    result = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                      AutoCloudEffectivePlatform::Windows);
    CHECK(result.root == "WinAppDataLocalLow");
    CHECK(result.resolvedPath == "Publisher/Game/WinSaves/{Steam3AccountID}");
    CHECK(ExpandAutoCloudPathTokens(result.resolvedPath, 1234) ==
          "Publisher/Game/WinSaves/1234");

    original.path.clear();
    original.resolvedPath.clear();
    result = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                      AutoCloudEffectivePlatform::Windows);
    CHECK(result.resolvedPath == "Publisher");
}

static void NativeLinuxAndNonmatchingOverrides() {
    const auto original = Rule("WinSavedGames", "Game/WinSaves");
    AutoCloudRootOverrideNative ov;
    ov.root = original.root;
    ov.os = "Linux";
    ov.osCompare = "=";
    ov.useInstead = "LinuxXdgDataHome";
    ov.addPath = "Publisher";
    ov.pathTransforms = {{"WinSaves", "LinuxSaves"}};
    const auto native = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Linux,
                                 AutoCloudEffectivePlatform::Linux);
    CHECK(native.root == "LinuxXdgDataHome");
    CHECK(native.resolvedPath == "Publisher/Game/LinuxSaves");
    const auto win = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                              AutoCloudEffectivePlatform::Windows);
    CHECK(win.root == original.root);
    CHECK(win.resolvedPath == original.path);
    ov.root = "GameInstall";
    ov.os = "Windows";
    const auto other = RoundTrip(original, {ov}, AutoCloudEffectivePlatform::Windows,
                                AutoCloudEffectivePlatform::Windows);
    CHECK(other.root == original.root);
    CHECK(other.resolvedPath == original.path);
}

struct TestKv {
    std::string value;
    int32_t number = 0;
    std::map<std::string, std::unique_ptr<TestKv>> children;
};

static void FiltersKeepTheirOriginalRepresentation() {
    auto rule = Rule("WinAppDataLocalLow", "Game/Saves");
    AppInfoKVNode exclude;
    exclude.key = "exclude";
    AppInfoKVNode log, temp;
    log.key = "0"; log.hasString = true; log.stringValue = "*.log";
    temp.key = "2"; temp.hasString = true; temp.stringValue = "temp/*";
    exclude.children = {log, temp};
    AppInfoKVNode siblings;
    siblings.key = "siblings";
    siblings.hasString = true;
    siblings.stringValue = "jpg png";
    rule.filterFields = {exclude, siblings};
    const auto findKey = [](void* parent, const char* name, uint8_t, void*) -> void* {
        auto& p = static_cast<TestKv*>(parent)->children[name];
        if (!p) p = std::make_unique<TestKv>();
        return p.get();
    };
    const auto setString = [](void* node, const char* value) {
        static_cast<TestKv*>(node)->value = value;
    };
    const auto setInt = [](void* node, int32_t value) {
        static_cast<TestKv*>(node)->number = value;
    };
    auto injected = SteamKvInjector::MakeSaveFileRule(rule);
    CHECK(injected.filterFields.size() == 2);
    TestKv output;
    CHECK(SteamKvInjector::WriteFilterFields(&output, injected.filterFields,
                                            findKey, setString, setInt));
    CHECK(output.children.size() == 2);
    if (output.children.count("exclude") && output.children.count("siblings")) {
        const auto& actual = *output.children.at("exclude");
        CHECK(actual.children.size() == 2);
        CHECK(actual.children.count("0") == 1);
        CHECK(actual.children.count("2") == 1);
        if (actual.children.count("0") && actual.children.count("2")) {
            CHECK(actual.children.at("0")->value == "*.log");
            CHECK(actual.children.at("2")->value == "temp/*");
        }
        CHECK(output.children.at("siblings")->value == "jpg png");
    } else CHECK(false);

    // A scalar exclude must remain a scalar, not be rewritten into a list.
    exclude.children.clear();
    exclude.hasString = true;
    exclude.stringValue = "*.tmp";
    rule.filterFields = {exclude};
    injected = SteamKvInjector::MakeSaveFileRule(rule);
    TestKv scalar;
    CHECK(SteamKvInjector::WriteFilterFields(&scalar, injected.filterFields,
                                            findKey, setString, setInt));
    CHECK(scalar.children.count("exclude") == 1);
    if (scalar.children.count("exclude")) {
        CHECK(scalar.children.at("exclude")->value == "*.tmp");
        CHECK(scalar.children.at("exclude")->children.empty());
    }
    // Rules without optional filters must not gain any.
    TestKv empty;
    CHECK(SteamKvInjector::WriteFilterFields(&empty, {}, findKey, setString, setInt));
    CHECK(empty.children.empty());
    const auto failFind = [](void*, const char*, uint8_t, void*) -> void* { return nullptr; };
    CHECK(!SteamKvInjector::WriteFilterFields(&empty, injected.filterFields,
                                             failFind, setString, setInt));
}

int main() {
    SummerClover();
    PlainRulesStayUnchanged();
    OverrideVariants();
    NativeLinuxAndNonmatchingOverrides();
    FiltersKeepTheirOriginalRepresentation();
    // Callers without an explicit cloud root retain their existing root.
    auto legacy = Rule("WinSavedGames", "Example");
    legacy.cloudRoot.clear();
    CHECK(SteamKvInjector::MakeSaveFileRule(legacy).root == "WinSavedGames");
    std::printf("AutoCloud injection: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
