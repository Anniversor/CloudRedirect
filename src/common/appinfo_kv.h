#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AutoCloudUtil {

// Parsed appinfo KeyValues data, independent of filesystem/platform helpers.
struct AppInfoKVNode {
    std::string key;
    std::string stringValue;
    int32_t intValue = 0;
    bool hasString = false;
    bool hasInt = false;
    std::vector<AppInfoKVNode> children;
};

} // namespace AutoCloudUtil
