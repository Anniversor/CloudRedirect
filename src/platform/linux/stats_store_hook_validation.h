#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace StatsStoreHook::Validation {
enum class EntryKind { Unsupported, Native, SlsDetour };

// A 32-bit near jump wraps modulo 2^32, including jumps from Steam's high
// mapping into a module mapped below it. Never dereference this destination.
inline std::optional<uint32_t> JumpDestination(uint32_t address,
                                              const uint8_t* bytes, size_t size) {
    if (size < 5 || bytes[0] != 0xe9) return std::nullopt;
    uint32_t relative;
    std::memcpy(&relative, bytes + 1, sizeof(relative));
    return address + 5u + relative;
}

inline bool IsSlsMapping(std::string_view permissions, std::string_view path) {
    constexpr std::string_view deleted = " (deleted)";
    if (path.ends_with(deleted)) path.remove_suffix(deleted.size());
    return permissions.size() >= 3 && permissions[2] == 'x' &&
           path.ends_with("/SLSsteam.so");
}

template<class IsSlsExecutable>
EntryKind ClassifyEntry(uint32_t address, const uint8_t* bytes, size_t size,
                       IsSlsExecutable isSlsExecutable) {
    if (size >= 5 && std::memcmp(bytes, "\x55\x57\x56\x53\xe8", 5) == 0)
        return EntryKind::Native;
    auto destination = JumpDestination(address, bytes, size);
    if (destination && isSlsExecutable(*destination)) return EntryKind::SlsDetour;
    return EntryKind::Unsupported;
}
}
