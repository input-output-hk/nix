#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nix::detail {

constexpr int64_t emptyAutoGCCooldownSeconds = 60;

struct EmptyAutoGCCacheRecord
{
    int64_t completedAt;
    uint64_t freeAfter;
    std::string identity;
};

struct EmptyAutoGCCacheDecision
{
    bool reusable;
    std::string_view reason;
    uint64_t ageSeconds;
    uint64_t freeThreshold;
};

/** Decide whether a previous successful empty auto-GC is recent enough to reuse. */
constexpr EmptyAutoGCCacheDecision canReuseEmptyAutoGC(
    const EmptyAutoGCCacheRecord & record,
    int64_t now,
    uint64_t currentFree,
    std::string_view identity) noexcept
{
    if (std::string_view{record.identity} != identity)
        return {false, "identity-mismatch", 0, 0};

    if (now < 0 || record.completedAt < 0)
        return {false, "invalid-time", 0, 0};

    if (now < record.completedAt)
        return {false, "clock-backwards", 0, 0};

    const auto age = static_cast<uint64_t>(now - record.completedAt);
    if (age >= static_cast<uint64_t>(emptyAutoGCCooldownSeconds))
        return {false, "expired", age, 0};

    /* Compute floor(freeAfter * 97 / 100) without overflowing uint64_t. */
    const auto threshold = (record.freeAfter / 100) * 97 + ((record.freeAfter % 100) * 97) / 100;
    if (currentFree <= threshold)
        return {false, "free-space-dropped", age, threshold};

    return {true, "recent-empty", age, threshold};
}

} // namespace nix::detail
