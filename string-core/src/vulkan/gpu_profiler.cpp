#include <string/vulkan/gpu_profiler.hpp>

#include <algorithm>

#include <string/gpu/device.hpp>

namespace string
{

GpuProfiler::~GpuProfiler()
{
    // Pools are destroyed explicitly via destroy() while the device is still alive; nothing to do.
}

void GpuProfiler::init(string::gpu::device& device, uint32_t frames_in_flight, uint32_t max_pairs)
{
    device_ = device.get_device();
    const VkPhysicalDeviceLimits limits = device.get_physical_device_limits();
    period_ns_ = static_cast<double>(limits.timestampPeriod);
    if (period_ns_ <= 0.0)
    {
        // Device can't timestamp — leave disabled (enabled() == false); the renderer skips writes.
        return;
    }
    max_pairs_ = max_pairs;
    pools_.resize(frames_in_flight);
    for (FramePool& fp : pools_)
    {
        const VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = max_pairs_ * 2,
            .pipelineStatistics = 0,
        };
        vkCreateQueryPool(device_, &info, nullptr, &fp.pool);
        fp.slot_names.resize(max_pairs_);
    }
}

void GpuProfiler::destroy(VkDevice device)
{
    for (FramePool& fp : pools_)
        if (fp.pool != VK_NULL_HANDLE) vkDestroyQueryPool(device, fp.pool, nullptr);
    pools_.clear();
    period_ns_ = 0.0;
}

void GpuProfiler::begin_frame(gpu::command_recorder& rec, uint32_t frame_index)
{
    if (!enabled() || frame_index >= pools_.size()) return;
    FramePool& fp = pools_[frame_index];
    // The renderer already waited on this frame index's timeline value, so last cycle's queries for
    // this pool are complete — read them before resetting.
    if (fp.pending) readback(frame_index);
    rec.reset_query_pool(fp.pool, 0, max_pairs_ * 2);
    fp.used = 0;
    fp.open_slot = UINT32_MAX;
    fp.pending = false;
}

void GpuProfiler::reset_stats()
{
    const std::lock_guard lock(mutex_);
    index_by_name_.clear();
    stats_.clear();
    for (FramePool& fp : pools_)
    {
        fp.pending = false;
        fp.open_slot = UINT32_MAX;
    }
}

void GpuProfiler::write_begin(gpu::command_recorder& rec, uint32_t frame_index, const std::string& name)
{
    if (!enabled() || frame_index >= pools_.size()) return;
    FramePool& fp = pools_[frame_index];
    if (fp.used >= max_pairs_) return;  // ring full this frame — skip (very rare, generous cap)
    const uint32_t slot = fp.used;
    fp.slot_names[slot].name = name;
    fp.open_slot = slot;
    rec.write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, fp.pool, slot * 2);
}

void GpuProfiler::write_end(gpu::command_recorder& rec, uint32_t frame_index)
{
    if (!enabled() || frame_index >= pools_.size()) return;
    FramePool& fp = pools_[frame_index];
    if (fp.open_slot == UINT32_MAX) return;
    const uint32_t slot = fp.open_slot;
    rec.write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, fp.pool, slot * 2 + 1);
    fp.open_slot = UINT32_MAX;
    fp.used = slot + 1;
    fp.pending = true;
}

void GpuProfiler::readback(uint32_t frame_index)
{
    FramePool& fp = pools_[frame_index];
    if (fp.used == 0) return;
    std::vector<uint64_t> raw(fp.used * 2);
    const VkResult r = vkGetQueryPoolResults(
        device_, fp.pool, 0, fp.used * 2, raw.size() * sizeof(uint64_t), raw.data(),
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return;

    std::lock_guard lock(mutex_);
    for (uint32_t s = 0; s < fp.used; ++s)
    {
        const uint64_t begin = raw[s * 2];
        const uint64_t end = raw[s * 2 + 1];
        if (end <= begin) continue;  // wraparound / unwritten
        const double ms = double(end - begin) * period_ns_ * 1e-6;
        const std::string& name = fp.slot_names[s].name;

        auto it = index_by_name_.find(name);
        size_t idx;
        if (it == index_by_name_.end())
        {
            idx = stats_.size();
            index_by_name_.emplace(name, idx);
            stats_.push_back(PassStat{ name, ms, ms, ms });
            continue;
        }
        idx = it->second;
        PassStat& st = stats_[idx];
        st.last_ms = ms;
        st.avg_ms = st.avg_ms * 0.9 + ms * 0.1;          // ~10-frame EMA
        st.worst_ms = std::max(st.worst_ms * 0.995, ms);  // slowly-decaying peak
    }
}

std::vector<GpuProfiler::PassStat> GpuProfiler::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

double GpuProfiler::total_avg_ms() const
{
    std::lock_guard lock(mutex_);
    double total = 0.0;
    for (const PassStat& s : stats_) total += s.avg_ms;
    return total;
}

namespace { const GpuProfiler* g_global = nullptr; }
void GpuProfiler::set_global(const GpuProfiler* p) { g_global = p; }
const GpuProfiler* GpuProfiler::global() { return g_global; }

}  // namespace string
