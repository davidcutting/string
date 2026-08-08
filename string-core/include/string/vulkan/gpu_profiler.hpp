#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <volk.h>

namespace string::gpu { class device; }

namespace string
{

// Per-pass GPU timing via vkCmdWriteTimestamp2 pairs (brief 06). The renderer wraps each pass's
// record()/record_compute() in a begin/end timestamp at the SAME seam the Tracy GPU zones use;
// this gives an IN-GAME per-pass millisecond readout (a HUD panel) plus periodic log lines — Tracy
// stays the deep-capture tool, this is the always-on glance.
//
// Design: one VkQueryPool per frame-in-flight (a ring). Each frame the renderer resets its pool,
// then for each recorded pass phase allocates a timestamp pair (begin at TOP_OF_PIPE, end at
// BOTTOM_OF_PIPE). When a frame index is reused (its GPU work is provably complete — the renderer
// already waited on that frame's timeline value), we read the previous results back, scale by
// timestampPeriod, and fold them into a rolling average/worst keyed by pass name.
//
// Zero-cost-ish when the HUD is off: the queries still run (cheap), but stats() is only walked when
// a consumer asks. The whole thing no-ops if the device reports a zero timestampPeriod / lacks
// timestamp support.
class GpuProfiler
{
public:
    struct PassStat
    {
        std::string name;
        double avg_ms = 0.0;    // exponential rolling average
        double last_ms = 0.0;   // most recent readback
        double worst_ms = 0.0;  // decaying worst-case
    };

    GpuProfiler() = default;
    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // Create the query-pool ring. frames_in_flight pools, each with room for max_pairs pairs.
    void init(string::gpu::device& device, uint32_t frames_in_flight, uint32_t max_pairs = 64);
    void destroy(VkDevice device);

    bool enabled() const { return period_ns_ > 0.0; }

    // Start a new frame's timing: read back the pool that is about to be reused (its GPU work is
    // done), then reset that pool. Must be called after the renderer has waited on this frame index.
    void begin_frame(VkCommandBuffer cmd, uint32_t frame_index);

    // Bracket a pass phase. write_begin returns a pair-slot handle; pass it to write_end. The `name`
    // is copied into the frame's slot table so readback can attribute the result. phase is a short
    // suffix ("", " cs") to disambiguate the compute vs graphics phase of the same pass.
    void write_begin(VkCommandBuffer cmd, uint32_t frame_index, const std::string& name);
    void write_end(VkCommandBuffer cmd, uint32_t frame_index);

    // Forget every per-pass stat. For a scene switch: the pass set is replaced wholesale, so the
    // rolling averages describe passes that no longer exist and the HUD would otherwise show the two
    // scenes' passes mixed together. Also discards results still pending readback — they belong to
    // the outgoing scene (begin_frame resets each pool unconditionally, so dropping `pending` is safe).
    void reset_stats();

    // Snapshot the current rolling per-pass stats (sorted by first-seen order). Cheap copy.
    std::vector<PassStat> stats() const;

    // Sum of all passes' avg ms — the HUD's "GPU total".
    double total_avg_ms() const;

    // Process-global handle so a UI/tooling layer (the profiler HUD) can read the numbers without
    // the renderer being plumbed through the UI author. The renderer registers itself on init and
    // clears the pointer on destruction. Null before the renderer exists / after it's gone.
    static void set_global(const GpuProfiler* p);
    static const GpuProfiler* global();

private:
    struct SlotName { std::string name; };
    struct FramePool
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t used = 0;                 // pairs written this cycle
        bool pending = false;              // has results to read back before reset
        std::vector<SlotName> slot_names;  // per-pair attribution
        uint32_t open_slot = UINT32_MAX;   // slot with an unmatched write_begin
    };

    void readback(uint32_t frame_index);

    VkDevice device_ = VK_NULL_HANDLE;
    double period_ns_ = 0.0;               // ns per timestamp tick; 0 = disabled
    uint32_t max_pairs_ = 0;
    std::vector<FramePool> pools_;

    // Rolling stats keyed by name, in first-seen order for stable HUD layout.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, size_t> index_by_name_;
    std::vector<PassStat> stats_;
};

}  // namespace string
