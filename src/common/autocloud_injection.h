#pragma once

#include "autocloud_util.h"
#include "steam_kv_injector.h"

namespace SteamKvInjector {

inline SaveFileRule MakeSaveFileRule(const AutoCloudUtil::AutoCloudRuleNative& rule) {
    SaveFileRule result;
    // GetRules resolves the filesystem root for this OS, but keeps the original
    // cloud root/path separately. Steam applies ufs.rootoverrides itself: mixing
    // the resolved root with the original path loses addpath/pathtransforms and
    // changes the cloud filename. Keep the original pair for Steam to resolve.
    result.root = rule.cloudRoot.empty() ? rule.root : rule.cloudRoot;
    result.path = rule.path;
    result.pattern = rule.pattern;
    result.recursive = rule.recursive;
    result.platforms = rule.platforms;
    result.filterFields = rule.filterFields;
    return result;
}

// Preserve Steam's own filter representation. In particular, do not turn an
// exclude list into a broader pattern or discard sibling extension settings.
template<class FindKey, class SetString, class SetInt>
bool WriteFilterFields(void* parent, const std::vector<AutoCloudUtil::AppInfoKVNode>& fields,
                       FindKey findKey, SetString setString, SetInt setInt) {
    for (const auto& field : fields) {
        void* node = findKey(parent, field.key.c_str(), 1, nullptr);
        if (!node) return false;
        if (field.hasString) setString(node, field.stringValue.c_str());
        else if (field.hasInt) setInt(node, field.intValue);
        if (!WriteFilterFields(node, field.children, findKey, setString, setInt)) return false;
    }
    return true;
}

} // namespace SteamKvInjector
