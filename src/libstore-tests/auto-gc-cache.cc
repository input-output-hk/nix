#include <gtest/gtest.h>

#include <limits>

#include "../libstore/auto-gc-cache.hh"

namespace nix::detail {

TEST(EmptyAutoGCCache, ReusesRecentResultWithinFreeSpaceBand)
{
    const EmptyAutoGCCacheRecord record{
        .completedAt = 100,
        .freeAfter = 100'000,
        .identity = "local-store-a",
    };

    const auto decision = canReuseEmptyAutoGC(record, 159, 97'001, "local-store-a");
    EXPECT_TRUE(decision.reusable);
    EXPECT_EQ(decision.reason, "recent-empty");
    EXPECT_EQ(decision.ageSeconds, 59);
    EXPECT_EQ(decision.freeThreshold, 97'000);
}

TEST(EmptyAutoGCCache, ExpiresAtSixtySeconds)
{
    const EmptyAutoGCCacheRecord record{.completedAt = 100, .freeAfter = 100'000, .identity = "local-store-a"};

    const auto decision = canReuseEmptyAutoGC(record, 160, 100'000, "local-store-a");
    EXPECT_FALSE(decision.reusable);
    EXPECT_EQ(decision.reason, "expired");
}

TEST(EmptyAutoGCCache, RejectsClockMovingBackwards)
{
    const EmptyAutoGCCacheRecord record{.completedAt = 100, .freeAfter = 100'000, .identity = "local-store-a"};

    const auto decision = canReuseEmptyAutoGC(record, 99, 100'000, "local-store-a");
    EXPECT_FALSE(decision.reusable);
    EXPECT_EQ(decision.reason, "clock-backwards");
}

TEST(EmptyAutoGCCache, RejectsInvalidNegativeTimestamp)
{
    const EmptyAutoGCCacheRecord record{.completedAt = -1, .freeAfter = 100'000, .identity = "local-store-a"};

    const auto decision = canReuseEmptyAutoGC(record, 101, 100'000, "local-store-a");
    EXPECT_FALSE(decision.reusable);
    EXPECT_EQ(decision.reason, "invalid-time");
}

TEST(EmptyAutoGCCache, RequiresMoreThanNinetySevenPercentFree)
{
    const EmptyAutoGCCacheRecord record{.completedAt = 100, .freeAfter = 100'000, .identity = "local-store-a"};

    const auto atThreshold = canReuseEmptyAutoGC(record, 101, 97'000, "local-store-a");
    EXPECT_FALSE(atThreshold.reusable);
    EXPECT_EQ(atThreshold.reason, "free-space-dropped");

    const auto belowThreshold = canReuseEmptyAutoGC(record, 101, 96'999, "local-store-a");
    EXPECT_FALSE(belowThreshold.reusable);
    EXPECT_EQ(belowThreshold.reason, "free-space-dropped");
}

TEST(EmptyAutoGCCache, DoesNotReuseAcrossStoreOrConfigIdentity)
{
    const EmptyAutoGCCacheRecord record{.completedAt = 100, .freeAfter = 100'000, .identity = "local-store-a"};

    const auto decision = canReuseEmptyAutoGC(record, 101, 100'000, "local-store-b");
    EXPECT_FALSE(decision.reusable);
    EXPECT_EQ(decision.reason, "identity-mismatch");
}

TEST(EmptyAutoGCCache, ThresholdArithmeticDoesNotOverflow)
{
    const EmptyAutoGCCacheRecord record{
        .completedAt = 100,
        .freeAfter = std::numeric_limits<uint64_t>::max(),
        .identity = "local-store-a",
    };

    const auto decision = canReuseEmptyAutoGC(record, 101, std::numeric_limits<uint64_t>::max(), "local-store-a");
    EXPECT_TRUE(decision.reusable);
}

} // namespace nix::detail
