#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

#include <string/gpu/residency_manager.hpp>

using namespace string::gpu;

namespace
{

// A GPU-free stand-in for a real streaming provider. Streams "complete" only when the test releases
// their ticket via complete_all(), so tests can drive the STREAMING -> CACHED transition
// deterministically. cost() is a flat bytes-per-detail so budget math is easy to reason about.
class FakeProvider : public residency_provider
{
public:
    explicit FakeProvider(VkDeviceSize bytes_per_detail) : bytes_per_detail_(bytes_per_detail) {}

    std::uint64_t stream(resource_id id, std::uint32_t from, std::uint32_t to) override
    {
        stream_calls.push_back({ id, from, to });
        const std::uint64_t ticket = next_ticket_++;
        pending_.insert(ticket);
        return ticket;
    }
    bool is_complete(std::uint64_t ticket) override
    {
        return ticket == 0 || pending_.find(ticket) == pending_.end();
    }
    void on_resident(resource_id id, std::uint32_t detail) override
    {
        resident_calls.push_back({ id, detail });
    }
    void evict(resource_id id, std::uint32_t from, std::uint32_t to) override
    {
        evict_calls.push_back({ id, from, to });
    }
    VkDeviceSize cost(resource_id, std::uint32_t detail) override
    {
        return static_cast<VkDeviceSize>(detail) * bytes_per_detail_;
    }

    // Mark every in-flight stream as landed on the GPU.
    void complete_all() { pending_.clear(); }

    struct Call { resource_id id; std::uint32_t from; std::uint32_t to; };
    struct Resident { resource_id id; std::uint32_t detail; };
    std::vector<Call> stream_calls;
    std::vector<Call> evict_calls;
    std::vector<Resident> resident_calls;

private:
    VkDeviceSize bytes_per_detail_;
    std::uint64_t next_ticket_ = 1;
    std::unordered_set<std::uint64_t> pending_;
};

}  // namespace

// A wanted-finer resource streams, and only flips to CACHED once its upload completes — never
// before (a not-yet-resident mip must not be treated as sampleable).
TEST(ResidencyManager, StreamsThenCachesOnCompletion)
{
    FakeProvider provider(/*bytes_per_detail=*/100);
    residency_manager mgr(/*budget=*/10'000);
    mgr.register_resource(1, provider, /*min=*/1, /*max=*/8, /*initial=*/1);

    EXPECT_EQ(mgr.status_of(1), stream_status::CACHED);
    EXPECT_EQ(mgr.resident_detail(1), 1u);

    mgr.want(1, 8, resource_priority::LAZY, /*frame=*/1);
    mgr.tick(1);
    EXPECT_EQ(mgr.status_of(1), stream_status::STREAMING);
    EXPECT_EQ(mgr.resident_detail(1), 1u);  // still coarse until the upload lands
    ASSERT_EQ(provider.stream_calls.size(), 1u);
    EXPECT_EQ(provider.stream_calls[0].from, 1u);
    EXPECT_EQ(provider.stream_calls[0].to, 8u);

    // Not complete yet -> stays streaming.
    mgr.tick(2);
    EXPECT_EQ(mgr.status_of(1), stream_status::STREAMING);

    provider.complete_all();
    mgr.tick(3);
    EXPECT_EQ(mgr.status_of(1), stream_status::CACHED);
    EXPECT_EQ(mgr.resident_detail(1), 8u);
    ASSERT_EQ(provider.resident_calls.size(), 1u);
    EXPECT_EQ(provider.resident_calls[0].detail, 8u);
}

// want() clamps the request into [min, max].
TEST(ResidencyManager, ClampsDesiredDetail)
{
    FakeProvider provider(10);
    residency_manager mgr(10'000);
    mgr.register_resource(1, provider, /*min=*/2, /*max=*/6, /*initial=*/2);

    mgr.want(1, 99, resource_priority::LAZY, 1);
    mgr.tick(1);
    provider.complete_all();
    mgr.tick(2);
    EXPECT_EQ(mgr.resident_detail(1), 6u);  // clamped to max
}

// Over budget, the least-recently-visible resource's finer detail is evicted down to its pinned
// floor; the pinned floor itself is never evicted so the resource still renders.
TEST(ResidencyManager, EvictsLeastRecentlyVisibleToFloor)
{
    FakeProvider provider(/*bytes_per_detail=*/100);
    // Budget fits one resource at detail 8 (800) plus two floors (100 each) = 1000.
    residency_manager mgr(/*budget=*/1000);
    for (resource_id id : { resource_id{1}, resource_id{2} })
    {
        mgr.register_resource(id, provider, /*min=*/1, /*max=*/8, /*initial=*/1);
    }

    // Frame 1: resource 1 seen (older), resource 2 seen (newer), both want max.
    mgr.want(1, 8, resource_priority::LAZY, 1);
    mgr.want(2, 8, resource_priority::LAZY, 2);
    mgr.tick(2);
    provider.complete_all();
    mgr.tick(3);
    // Both can't be resident at 8 (1600 > 1000). One must have been held back / evicted to floor.
    EXPECT_LE(mgr.resident_bytes(), mgr.budget());

    // Keep resource 2 visible and wanted; resource 1 goes stale. Under pressure, 1 (older) loses its
    // finer detail before 2.
    for (std::uint64_t f = 4; f < 8; ++f)
    {
        mgr.want(2, 8, resource_priority::LAZY, f);
        mgr.tick(f);
        provider.complete_all();
    }
    mgr.tick(8);
    EXPECT_EQ(mgr.resident_detail(2), 8u);
    EXPECT_EQ(mgr.resident_detail(1), 1u);  // evicted back to pinned floor
    EXPECT_LE(mgr.resident_bytes(), mgr.budget());
}

// An IMMEDIATE resource streams even when that pushes past the budget (correctness over cap).
TEST(ResidencyManager, ImmediateStreamsOverBudget)
{
    FakeProvider provider(1000);
    residency_manager mgr(/*budget=*/100);  // far too small
    mgr.register_resource(1, provider, /*min=*/1, /*max=*/4, /*initial=*/1);

    mgr.want(1, 4, resource_priority::IMMEDIATE, 1);
    mgr.tick(1);
    EXPECT_EQ(mgr.status_of(1), stream_status::STREAMING);
    provider.complete_all();
    mgr.tick(2);
    EXPECT_EQ(mgr.resident_detail(1), 4u);
}

// The per-tick stream budget spreads a burst of newly-wanted detail across frames rather than
// starting every stream in one tick.
TEST(ResidencyManager, RateLimitsStreamsPerTick)
{
    FakeProvider provider(/*bytes_per_detail=*/100);  // delta 1->8 = 700 bytes each
    residency_manager mgr(/*budget=*/1'000'000, /*max_stream_bytes_per_tick=*/1000);
    for (resource_id id : { resource_id{1}, resource_id{2}, resource_id{3} })
    {
        mgr.register_resource(id, provider, /*min=*/1, /*max=*/8, /*initial=*/1);
        mgr.want(id, 8, resource_priority::LAZY, 1);
    }

    // First tick: only one 700-byte stream fits the 1000-byte/tick cap.
    mgr.tick(1);
    EXPECT_EQ(provider.stream_calls.size(), 1u);

    // Drain and keep ticking: the deferred ones stream on later ticks.
    for (std::uint64_t f = 2; f <= 5; ++f)
    {
        provider.complete_all();
        for (resource_id id : { resource_id{1}, resource_id{2}, resource_id{3} })
            mgr.want(id, 8, resource_priority::LAZY, f);
        mgr.tick(f);
    }
    EXPECT_EQ(provider.stream_calls.size(), 3u);
    EXPECT_EQ(mgr.resident_detail(1), 8u);
    EXPECT_EQ(mgr.resident_detail(2), 8u);
    EXPECT_EQ(mgr.resident_detail(3), 8u);
}

// release() drops the request to the floor and the finer detail is evicted on the next tick.
TEST(ResidencyManager, ReleaseEvictsToFloor)
{
    FakeProvider provider(100);
    residency_manager mgr(10'000);
    mgr.register_resource(1, provider, /*min=*/1, /*max=*/8, /*initial=*/1);

    mgr.want(1, 8, resource_priority::LAZY, 1);
    mgr.tick(1);
    provider.complete_all();
    mgr.tick(2);
    ASSERT_EQ(mgr.resident_detail(1), 8u);

    mgr.release(1);
    mgr.tick(3);
    EXPECT_EQ(mgr.resident_detail(1), 1u);
    ASSERT_FALSE(provider.evict_calls.empty());
    EXPECT_EQ(provider.evict_calls.back().to, 1u);
}
