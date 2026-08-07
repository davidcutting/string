#include <cstdlib>
#include <string/vulkan/frame_graph.hpp>

#include <string/gpu/pass_context.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/core/logger.hpp>

#include <algorithm>
#include <cmath>
#include <queue>

namespace string
{

// ================================================================================================
// authoring
// ================================================================================================

VkPipelineStageFlags2 pass_spec::main_stage(pass_kind kind)
{
    switch (kind)
    {
        case pass_kind::compute:  return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case pass_kind::transfer: return VK_PIPELINE_STAGE_2_COPY_BIT;
        case pass_kind::raster:   break;
    }
    return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
}

pass_spec& pass_spec::add(const resource_use& u, bool required)
{
    building_.uses.push_back(u);
    building_.required.push_back(required);
    return *this;
}

// The pass KIND is only known at the terminal raster()/compute()/transfer(), but reads and writes are
// declared before it. A stage of 0 means "whatever this pass's main stage turns out to be" and is
// resolved here — declaring .reads(x).compute(fn) must give COMPUTE_SHADER, not the raster default.
// Uses declared with an explicit stage keep it.
void pass_spec::resolve_stages()
{
    const VkPipelineStageFlags2 main = main_stage(building_.kind);
    for (resource_use& u : building_.uses)
    {
        if (u.stage != 0) continue;
        // Some accesses have exactly ONE stage they can happen at — an indirect-parameter fetch is
        // always DRAW_INDIRECT, an index fetch always INDEX_INPUT. Deriving those from the access
        // means an indirect draw's declaration needs no stage mask in pass code, which is the last
        // place they would otherwise have survived.
        switch (u.how)
        {
            case access::indirect_read: u.stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT; break;
            case access::index_read:    u.stage = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT; break;
            case access::vertex_read:   u.stage = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT; break;
            case access::transfer_read:
            case access::transfer_write: u.stage = VK_PIPELINE_STAGE_2_COPY_BIT; break;
            default:                    u.stage = main; break;
        }
    }
}

pass_spec& pass_spec::reads(gpu::image_view v)
{
    return add({ v, {}, access::sampled_read, 0 }, false);
}
pass_spec& pass_spec::reads(gpu::buffer b)
{
    return add({ {}, b, access::storage_read, 0 }, false);
}
pass_spec& pass_spec::reads(gpu::image_view v, access how, VkPipelineStageFlags2 stage)
{
    return add({ v, {}, how, stage }, false);
}
pass_spec& pass_spec::reads(gpu::buffer b, access how, VkPipelineStageFlags2 stage)
{
    return add({ {}, b, how, stage }, false);
}

pass_spec& pass_spec::requires_(gpu::image_view v)
{
    return add({ v, {}, access::sampled_read, 0 }, true);
}
pass_spec& pass_spec::requires_(gpu::buffer b)
{
    return add({ {}, b, access::storage_read, 0 }, true);
}

pass_spec& pass_spec::writes(gpu::image_view v)
{
    return add({ v, {}, access::storage_image_write, 0 }, false);
}
pass_spec& pass_spec::writes(gpu::buffer b)
{
    return add({ {}, b, access::storage_write, 0 }, false);
}
pass_spec& pass_spec::writes(gpu::image_view v, access how, VkPipelineStageFlags2 stage)
{
    return add({ v, {}, how, stage }, false);
}
pass_spec& pass_spec::writes(gpu::buffer b, access how, VkPipelineStageFlags2 stage)
{
    return add({ {}, b, how, stage }, false);
}

pass_spec& pass_spec::color(gpu::image_view v)
{
    return add({ v, {}, access::color_write, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT }, false);
}
pass_spec& pass_spec::depth(gpu::image_view v)
{
    return add({ v, {}, access::depth_write, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT }, false);
}
pass_spec& pass_spec::depth_read(gpu::image_view v)
{
    return add({ v, {}, access::depth_read, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT }, false);
}

void pass_spec::raster(record_fn fn)
{
    building_.kind = pass_kind::raster;
    resolve_stages();
    building_.record = std::move(fn);
    graph_.add(std::move(building_));
}
void pass_spec::compute(record_fn fn)
{
    building_.kind = pass_kind::compute;
    resolve_stages();
    building_.record = std::move(fn);
    graph_.add(std::move(building_));
}
void pass_spec::transfer(record_fn fn)
{
    building_.kind = pass_kind::transfer;
    resolve_stages();
    building_.record = std::move(fn);
    graph_.add(std::move(building_));
}

// ================================================================================================
// resource declaration
// ================================================================================================

gpu::image frame_graph::use_persistent(const persistent_image_info& info)
{
    image_record r;
    r.name = info.name;
    r.transient = false;
    r.swapchain = info.swapchain;
    r.initial_layout = info.initial_layout;
    r.per_frame = info.physical.size() > 1;
    r.physical = info.physical;
    r.neutral = info.neutral;
    images_.push_back(std::move(r));
    return gpu::image{ static_cast<std::uint32_t>(images_.size() - 1) };
}

gpu::buffer frame_graph::use_persistent(const persistent_buffer_info& info)
{
    buffer_record r;
    r.name = info.name;
    r.transient = false;
    r.per_frame = info.physical.size() > 1;
    r.physical = info.physical;
    buffers_.push_back(std::move(r));
    return gpu::buffer{ static_cast<std::uint32_t>(buffers_.size() - 1) };
}

void frame_graph::set_images(gpu::image h, std::span<const gpu::resource_id> physical)
{
    image_record& r = images_[h.index];
    r.physical.assign(physical.begin(), physical.end());
    r.per_frame = physical.size() > 1;
}

void frame_graph::set_buffers(gpu::buffer h, std::span<const gpu::resource_id> physical)
{
    buffer_record& r = buffers_[h.index];
    r.physical.assign(physical.begin(), physical.end());
    r.per_frame = physical.size() > 1;
}

gpu::image frame_graph::image(const transient_image_info& info)
{
    image_record r;
    r.name = info.name;
    r.transient = true;
    r.per_frame = info.per_frame;
    r.info = info;
    r.neutral = info.neutral;
    images_.push_back(std::move(r));
    return gpu::image{ static_cast<std::uint32_t>(images_.size() - 1) };
}

gpu::buffer frame_graph::buffer(const transient_buffer_info& info)
{
    buffer_record r;
    r.name = info.name;
    r.transient = true;
    r.per_frame = info.per_frame;
    r.info = info;
    buffers_.push_back(std::move(r));
    return gpu::buffer{ static_cast<std::uint32_t>(buffers_.size() - 1) };
}

std::vector<pass_info> frame_graph::passes() const
{
    std::vector<pass_info> out;
    out.reserve(passes_.size());
    for (const pass_decl& p : passes_)
        out.push_back({ p.name, p.kind, p.lane, !p.enabled || p.enabled() });
    return out;
}

// ================================================================================================
// compile
// ================================================================================================

namespace
{

// The descriptor type a handle needs follows from HOW the pass declared it. That is the whole reason
// pass code never names a descriptor type and never receives a slot from anywhere else.
gpu::descriptor_type descriptor_for(access how)
{
    switch (how)
    {
        case access::storage_image_read:
        case access::storage_image_write:
            return gpu::descriptor_type::STORAGE_IMAGE;
        case access::storage_read:
        case access::storage_write:
        case access::vertex_read:
        case access::index_read:
        case access::indirect_read:
            return gpu::descriptor_type::STORAGE_BUFFER;
        default:
            return gpu::descriptor_type::TEXTURE;
    }
}

// Two uses collide only if they name the same resource AND their subresource ranges overlap. This is
// what makes a mip chain orderable: hiz.build's write of mip N+1 does not collide with its read of
// mip N, so the graph derives the chain rather than serialising the whole image.
bool ranges_overlap(const gpu::image_view& a, const gpu::image_view& b)
{
    const auto span = [](std::uint32_t base, std::uint32_t count, std::uint32_t& lo, std::uint32_t& hi) {
        lo = base;
        hi = (count == gpu::image_view::all) ? ~std::uint32_t{ 0 } : base + count;
    };
    std::uint32_t amip_lo, amip_hi, bmip_lo, bmip_hi, alay_lo, alay_hi, blay_lo, blay_hi;
    span(a.base_mip, a.mip_count, amip_lo, amip_hi);
    span(b.base_mip, b.mip_count, bmip_lo, bmip_hi);
    span(a.base_layer, a.layer_count, alay_lo, alay_hi);
    span(b.base_layer, b.layer_count, blay_lo, blay_hi);
    return a.img == b.img && amip_lo < bmip_hi && bmip_lo < amip_hi
        && alay_lo < blay_hi && blay_lo < alay_hi;
}

bool collides(const resource_use& a, const resource_use& b)
{
    if (a.is_image() != b.is_image()) return false;
    if (a.is_image()) return ranges_overlap(a.img, b.img);
    return a.buf.valid() && a.buf == b.buf;
}

}  // namespace

compiled_frame frame_graph::compile(String::engine_context& ctx, VkExtent2D viewport)
{
    compiled_frame frame;
    frame.graph_ = this;
    frame.ctx_ = &ctx;
    frame.viewport_ = viewport;

    frame.materialize(ctx, viewport);

    // --- ordering ------------------------------------------------------------------------------
    // An edge runs producer -> consumer wherever two passes' declared uses collide and at least one
    // is a write. Kahn with a min-index priority queue, so among ready passes the earliest AUTHORED
    // one runs first — authoring order is the tiebreak, which makes the derived order predictable
    // and stable rather than an artefact of hash iteration.
    const auto n = static_cast<std::uint32_t>(passes_.size());
    std::vector<std::vector<std::uint32_t>> adjacency(n);
    std::vector<std::uint32_t> indegree(n, 0);

    for (std::uint32_t i = 0; i < n; ++i)
    {
        for (std::uint32_t j = i + 1; j < n; ++j)
        {
            bool edge = false;
            for (const resource_use& a : passes_[i].uses)
            {
                for (const resource_use& b : passes_[j].uses)
                {
                    if (!collides(a, b)) continue;
                    if (!is_write(a.how) && !is_write(b.how)) continue;   // read/read is free
                    edge = true;
                    break;
                }
                if (edge) break;
            }
            if (edge)
            {
                adjacency[i].push_back(j);
                ++indegree[j];
            }
        }
    }

    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
    for (std::uint32_t i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.push(i);

    frame.order_.reserve(n);
    while (!ready.empty())
    {
        const std::uint32_t i = ready.top();
        ready.pop();
        frame.order_.push_back(i);
        for (std::uint32_t j : adjacency[i])
            if (--indegree[j] == 0) ready.push(j);
    }
    if (frame.order_.size() != n)
        throw std::runtime_error("frame_graph::compile: cycle in the declared dependencies");

    frame.seed_persistent_layouts();
    frame.bind_declared_slots();
    frame.derive_groups();
    frame.alive_.assign(n, true);

    for (std::uint32_t i = 0; i < frame.order_.size(); ++i)
        STRING_LOG_INFO("[order] {} {}", i, passes_[frame.order_[i]].name);
    STRING_LOG_INFO("[graph] compiled {} passes, {} groups, {} KiB transients", n,
                    frame.groups_.size(), frame.transient_bytes_ / 1024);
    return frame;
}

// Allocate every transient, mint the neutral fallbacks, and register each declared sub-resource
// slice as its own resource so bind() can give it a descriptor slot.
void compiled_frame::materialize(String::engine_context& ctx, VkExtent2D viewport)
{
    transient_bytes_ = 0;

    const auto slots = static_cast<std::uint32_t>(ctx.frames_in_flight);

    for (frame_graph::image_record& r : graph_->images_)
    {
        if (r.transient)
        {
            const transient_image_info& ti = r.info;
            gpu::image_info info{};
            // Viewport-scaled extents round UP, and must. A half-res target at an odd viewport
            // width is dispatched as (width + 1) / 2 by every consumer that walks it; truncating
            // here would leave the image one texel short of the grid writing into it, which is an
            // out-of-bounds storage write on every odd-sized window.
            const auto scaled = [&](std::uint32_t v) {
                const float f = static_cast<float>(v) * ti.viewport_scale;
                return std::max(1u, static_cast<std::uint32_t>(std::ceil(f - 1e-4f)));
            };
            info.extent = ti.viewport_scaled
                ? VkExtent3D{ scaled(viewport.width), scaled(viewport.height), 1 }
                : ti.extent;
            info.format = ti.format;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = ti.usage;
            info.aspect_flags = ti.aspect;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.allocation_flags = 0;
            info.mip_levels = ti.mip_levels;
            info.samples = ti.samples;
            info.cube = ti.cube;
            info.sampler = ti.sampler;

            const std::uint32_t count = r.per_frame ? slots : 1;
            r.physical.clear();
            for (std::uint32_t s = 0; s < count; ++s)
                r.physical.push_back(ctx.allocator.create_resource(info));
        }
    }

    // Neutral fallbacks. A resource needs one iff some pass WRITES it (so it can be toggled off) and
    // some pass OPTIONALLY reads it (so degrade applies). A 1x1 image of the same format and sampler
    // is enough: the consumer samples it exactly as it would the real resource and gets the value
    // that cancels — 1.0 for a shadow or AO term, black for an additive one.
    const auto degradable = [&](std::uint32_t image_index) {
        // Only a transient can degrade — see producer_alive(). A persistent keeps its contents when
        // its producer is skipped, so minting a fallback for one would be actively wrong.
        if (!graph_->images_[image_index].transient) return false;
        bool written = false;
        bool optionally_read = false;
        for (const pass_decl& p : graph_->passes_)
        {
            for (std::size_t u = 0; u < p.uses.size(); ++u)
            {
                const resource_use& use = p.uses[u];
                if (!use.is_image() || use.img.img.index != image_index) continue;
                if (is_write(use.how)) written = true;
                else if (!p.required[u]) optionally_read = true;
            }
        }
        return written && optionally_read;
    };

    const auto image_count = static_cast<std::uint32_t>(graph_->images_.size());
    for (std::uint32_t i = 0; i < image_count; ++i)
    {
        frame_graph::image_record& r = graph_->images_[i];
        if (r.fallback.valid() || !degradable(i)) continue;

        VkFormat format = r.transient ? r.info.format : VK_FORMAT_UNDEFINED;
        gpu::sampler_info sampler = r.transient ? r.info.sampler : gpu::sampler_info{};
        if (!r.transient && !r.physical.empty())
        {
            const gpu::allocated_image& src = ctx.allocator.get_image(r.physical[0]);
            format = src.format;
        }
        if (format == VK_FORMAT_UNDEFINED) continue;

        const VkImageAspectFlags aspect = aspect_of(format);
        gpu::image_info info{};
        info.extent = { 1, 1, 1 };
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.aspect_flags = aspect;
        info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
        info.allocation_flags = 0;
        info.sampler = sampler;

        frame_graph::image_record fb;
        fb.name = r.name + ".neutral";
        fb.transient = true;
        fb.neutral = r.neutral;
        fb.physical.push_back(ctx.allocator.create_resource(info));
        fb.info.format = format;
        graph_->images_.push_back(std::move(fb));
        r.fallback = gpu::image{ static_cast<std::uint32_t>(graph_->images_.size() - 1) };
        pending_fallbacks_.push_back(r.fallback);
    }

    for (frame_graph::buffer_record& r : graph_->buffers_)
    {
        if (!r.transient) continue;
        gpu::buffer_info info{};
        info.size = r.info.bytes_for ? r.info.bytes_for(viewport) : r.info.bytes;
        info.usage = r.info.usage;
        info.memory_usage = r.info.memory_usage;
        info.allocation_flags = r.info.allocation_flags;

        const std::uint32_t count = r.per_frame ? slots : 1;
        r.physical.clear();
        for (std::uint32_t s = 0; s < count; ++s)
            r.physical.push_back(ctx.allocator.create_resource(info));
        transient_bytes_ += info.size * count;
    }
}

// Tell the tracker what layout each persistent's backing is ALREADY in. Without this the first
// graph touch of a construction-uploaded texture sources from UNDEFINED, which discards it.
// Bind every declared resource into the bindless table ONCE, at compile, for every frame slot.
//
// This has to happen before any recording: the set is not UPDATE_AFTER_BIND, so a bind issued while
// a command buffer holding it is open invalidates that buffer. The graph is the only thing that
// knows the full declared set up front, which is exactly what makes doing it here possible — pass
// code binding lazily at record time is what it replaces.
void compiled_frame::bind_declared_slots()
{
    if (std::getenv("STRING_TRACE_MAP"))
        for (const frame_graph::image_record& rec : graph_->images_)
            for (gpu::resource_id pid : rec.physical)
                if (pid != 0) STRING_LOG_INFO("[map] {} vk={:#x}", rec.name,
                    reinterpret_cast<std::uintptr_t>(ctx_->allocator.get_image(pid).image));
    const auto slots = static_cast<std::uint32_t>(ctx_->frames_in_flight);
    for (const pass_decl& p : graph_->passes_)
    {
        for (const resource_use& u : p.uses)
        {
            // Attachments and transfers need no descriptor at all — a colour write is not something a
            // shader indexes. Binding them would be meaningless, and for a BUFFER declared with a
            // transfer access it is actively wrong: descriptor_for() would answer TEXTURE and the
            // table would look the buffer id up in the image map.
            if (u.how == access::color_write || u.how == access::depth_write
                || u.how == access::depth_read || u.how == access::present
                || u.how == access::transfer_read || u.how == access::transfer_write) continue;
            // The KIND of the resource decides the descriptor family; the access only refines it
            // within images. An image is never a storage BUFFER and a buffer is never a texture.
            const gpu::descriptor_type type = u.is_image() ? descriptor_for(u.how)
                                                           : gpu::descriptor_type::STORAGE_BUFFER;
            // A use naming neither an image nor a buffer is a pass declaring an UNDECLARED handle —
            // a resource the app forgot to create. Skip it here; the pass will resolve slot 0 and the
            // missing declaration shows up as a visibly wrong read rather than a crash in compile.
            if (!u.is_image() && !u.buf.valid()) continue;
            if (u.is_image() && graph_->images_[u.img.img.index].swapchain) continue;
            // Creating the slice here, not lazily at record time, is the point: create_view() inserts
            // into the allocator's image map, and any reference taken before that insert dangles
            // afterwards. Doing it all at compile means the map is immutable while a frame records.
            for (std::uint32_t s = 0; s < slots; ++s)
            {
                const gpu::resource_id id = u.is_image() ? resolve_view(u.img, s)
                                                         : physical(u.buf, s);
                if (id != 0) ctx_->descriptor_table.bind(id, type);
            }
        }
    }
    // The neutral fallbacks too — a degraded read resolves to one of these and must find a slot.
    for (const frame_graph::image_record& r : graph_->images_)
    {
        if (!r.fallback.valid()) continue;
        const gpu::resource_id id = physical(r.fallback, 0);
        if (id != 0) ctx_->descriptor_table.bind(id, gpu::descriptor_type::TEXTURE);
    }
}

void compiled_frame::seed_persistent_layouts()
{
    for (const frame_graph::image_record& r : graph_->images_)
    {
        if (r.transient || r.initial_layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
        for (gpu::resource_id id : r.physical)
        {
            if (id == 0) continue;
            const gpu::allocated_image& img = ctx_->allocator.get_image(id);
            const VkImageAspectFlags aspect = aspect_of(img.format);
            states_.track(img.image, aspect, img.mip_levels, img.array_layers);
            states_.set_layout(img.image, r.initial_layout);
        }
    }
}

void compiled_frame::derive_groups()
{
    groups_.clear();

    // An attachment slot, plus its resolve target when the pass declared one. A pass that declares
    // TWO writes of the same kind where one is multisampled and one is not has declared a resolve —
    // the multisampled image is the attachment, the single-sample one is what it resolves into. That
    // is derivable from the declarations alone, so there is no `.resolves_to()` verb and no marker
    // access: the retired DepthResolve marker existed only because nothing looked at sample counts.
    // TWO cases share the shape "more than one write of the same kind", and the SAMPLE COUNT is what
    // distinguishes them:
    //   - exactly one multisampled and one single-sample -> a RESOLVE;
    //   - otherwise                                      -> MRT, and every one is a real attachment.
    // An earlier version collapsed the second case to found[0], which silently DROPPED attachments.
    // That is worse than failing: the pass looks like it rendered.
    struct attachment { std::vector<gpu::image_view> targets; std::optional<gpu::image_view> resolve; };
    const auto attachment_of = [this](const pass_decl& p, access want) -> attachment {
        std::vector<gpu::image_view> found;
        for (const resource_use& u : p.uses)
            if (u.is_image() && u.how == want) found.push_back(u.img);
        if (found.size() < 2) return { found, std::nullopt };

        std::optional<gpu::image_view> multi, single;
        bool ambiguous = false;
        for (const gpu::image_view& v : found)
        {
            std::optional<gpu::image_view>& slot =
                samples_of(v.img) > VK_SAMPLE_COUNT_1_BIT ? multi : single;
            if (slot) ambiguous = true;
            slot = v;
        }
        if (multi && single && !ambiguous) return { { *multi }, single };
        return { found, std::nullopt };
    };

    for (std::uint32_t idx = 0; idx < order_.size(); ++idx)
    {
        const pass_decl& p = graph_->passes_[order_[idx]];
        if (p.kind != pass_kind::raster)
            continue;

        const attachment color = attachment_of(p, access::color_write);
        attachment depth = attachment_of(p, access::depth_write);
        const bool writes_depth = !depth.targets.empty();
        if (!writes_depth) depth = attachment_of(p, access::depth_read);

        const auto first_of = [](const std::vector<gpu::image_view>& v) {
            return v.empty() ? std::optional<gpu::image_view>{} : std::optional<gpu::image_view>{ v.front() };
        };

        // Extend the open group when this pass draws into the same attachments as the previous one.
        if (!groups_.empty())
        {
            render_group& open = groups_.back();
            const bool contiguous = open.first + open.count == idx;
            const bool same = open.colors == color.targets && open.depth == first_of(depth.targets);
            if (contiguous && same)
            {
                ++open.count;
                // The resolve belongs to the LAST pass writing the attachment, not the first: an
                // earlier pass in the group storing to the resolve target would publish a
                // half-drawn frame.
                if (color.resolve) open.color_resolve = color.resolve;
                if (depth.resolve) open.depth_resolve = depth.resolve;
                open.depth_writes = open.depth_writes || writes_depth;
                continue;
            }
        }

        render_group g;
        g.first = idx;
        g.count = 1;
        g.colors = color.targets;
        g.color = first_of(color.targets);
        g.depth = first_of(depth.targets);
        g.color_resolve = color.resolve;
        g.depth_resolve = depth.resolve;
        g.depth_writes = writes_depth;

        // Multiview, derived: a layered attachment slice means every one of its layers is rendered in
        // this single pass. Six layers is a cubemap capture. Nothing declares a view mask; the slice
        // the pass already declared says it.
        std::uint32_t layers = 1;
        for (const gpu::image_view& v : g.colors)
        {
            const std::uint32_t n = v.layer_count == gpu::image_view::all
                ? graph_->images_[v.img.index].info.array_layers : v.layer_count;
            layers = std::max(layers, n);
        }
        if (layers > 1) g.view_mask = (1u << layers) - 1u;

        groups_.push_back(g);
    }

    // The RESOLVE belongs to the LAST group that writes the attachment, not the group whose pass
    // happened to declare the pair. Everything drawn after that group — debug lines, the UI overlay —
    // renders into the same multisampled target, and resolving early would publish the frame before
    // they existed: the scene would appear and the overlays would silently vanish.
    for (std::size_t gi = 0; gi < groups_.size(); ++gi)
    {
        if (!groups_[gi].color_resolve) continue;
        std::size_t last = gi;
        for (std::size_t k = gi + 1; k < groups_.size(); ++k)
            if (groups_[k].color == groups_[gi].color) last = k;
        if (last != gi)
        {
            groups_[last].color_resolve = groups_[gi].color_resolve;
            groups_[gi].color_resolve.reset();
        }
    }

    // Clear/store facts, derived from each attachment's position in its own lifetime across the
    // frame: the first group that writes an attachment clears it, later groups load it, and the last
    // one stores (or resolves). Nothing about this is declared by a pass.
    for (std::size_t gi = 0; gi < groups_.size(); ++gi)
    {
        render_group& g = groups_[gi];
        const auto first_use = [&](const std::optional<gpu::image_view>& v) {
            if (!v) return false;
            for (std::size_t k = 0; k < gi; ++k)
                if (groups_[k].color == v || groups_[k].depth == v) return false;
            return true;
        };
        const auto last_use = [&](const std::optional<gpu::image_view>& v) {
            if (!v) return false;
            for (std::size_t k = gi + 1; k < groups_.size(); ++k)
                if (groups_[k].color == v || groups_[k].depth == v) return false;
            return true;
        };
        g.clears_color = first_use(g.color);
        g.clears_depth = first_use(g.depth);
        g.last_color = last_use(g.color);
    }
}

void compiled_frame::resize(String::engine_context& ctx, VkExtent2D viewport)
{
    // Destroy and re-create only what is sized from the viewport. The DECLARATIONS are untouched,
    // so there is nothing to re-author and nothing to re-plan — the order, the groups and the
    // derived barriers are all still correct against the new backing.
    viewport_ = viewport;

    for (frame_graph::image_record& r : graph_->images_)
    {
        if (!r.transient || !r.info.viewport_scaled) continue;
        for (gpu::resource_id id : r.physical) ctx.allocator.destroy_resource(id);
        r.physical.clear();
    }
    for (frame_graph::buffer_record& r : graph_->buffers_)
    {
        if (!r.transient || !r.info.bytes_for) continue;
        for (gpu::resource_id id : r.physical) ctx.allocator.destroy_resource(id);
        r.physical.clear();
    }

    materialize(ctx, viewport);
    states_.clear();
}

// ================================================================================================
// execute
// ================================================================================================

void compiled_frame::compute_survivors()
{
    const auto n = static_cast<std::uint32_t>(graph_->passes_.size());
    alive_.assign(n, true);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        const pass_decl& p = graph_->passes_[i];
        if (p.enabled && !p.enabled()) alive_[i] = false;
    }

    // A pass whose .requires() read has no surviving producer drops too, transitively. Optional
    // reads never propagate a skip — they bind the resource's neutral fallback, which is what makes
    // toggling a producer off degrade gracefully instead of cascading into a black frame.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            if (!alive_[i]) continue;
            const pass_decl& p = graph_->passes_[i];
            for (std::size_t u = 0; u < p.uses.size(); ++u)
            {
                if (!p.required[u] || is_write(p.uses[u].how)) continue;
                bool produced = false;
                bool produced_by_anyone = false;
                for (std::uint32_t j = 0; j < n; ++j)
                {
                    for (const resource_use& w : graph_->passes_[j].uses)
                    {
                        if (!is_write(w.how) || !collides(w, p.uses[u])) continue;
                        produced_by_anyone = true;
                        if (alive_[j]) produced = true;
                    }
                }
                if (produced_by_anyone && !produced)
                {
                    alive_[i] = false;
                    changed = true;
                    break;
                }
            }
        }
    }
}

bool compiled_frame::ran(std::uint32_t order_index) const
{
    return order_index < order_.size() && alive_[order_[order_index]];
}

gpu::resource_id compiled_frame::physical(gpu::image h, std::uint32_t slot) const
{
    if (!h.valid() || h.index >= graph_->images_.size()) return 0;
    const frame_graph::image_record& r = graph_->images_[h.index];
    if (r.swapchain) return swapchain_;
    if (r.physical.empty()) return 0;
    return r.physical[r.per_frame ? slot % r.physical.size() : 0];
}

gpu::resource_id compiled_frame::physical(gpu::buffer h, std::uint32_t slot) const
{
    if (!h.valid() || h.index >= graph_->buffers_.size()) return 0;
    const frame_graph::buffer_record& r = graph_->buffers_[h.index];
    if (r.physical.empty()) return 0;
    return r.physical[r.per_frame ? slot % r.physical.size() : 0];
}

gpu::resource_allocator& compiled_frame::allocator() const { return ctx_->allocator; }

void compiled_frame::note_external_layout(gpu::image h, VkImageLayout layout)
{
    const frame_graph::image_record& r = graph_->images_[h.index];
    for (gpu::resource_id id : r.physical)
    {
        if (id == 0) continue;
        const gpu::allocated_image& img = ctx_->allocator.get_image(id);
        const VkImageAspectFlags aspect = aspect_of(img.format);
        states_.track(img.image, aspect, img.mip_levels, img.array_layers);
        states_.set_layout(img.image, layout);
    }
}

VkSampleCountFlagBits compiled_frame::samples_of(gpu::image h) const
{
    const frame_graph::image_record& r = graph_->images_[h.index];
    if (r.transient) return r.info.samples;
    // A persistent image's sample count is a property of the backing its owner allocated, which the
    // allocator records alongside mip_levels and array_layers.
    if (r.physical.empty()) return VK_SAMPLE_COUNT_1_BIT;
    return ctx_->allocator.get_image(r.physical[0]).samples;
}

gpu::resource_id compiled_frame::resolve_view(gpu::image_view v, std::uint32_t slot) const
{
    const gpu::resource_id base = physical(v.img, slot);
    if (base == 0 || v.whole_image()) return base;

    const slice_key key{ v.img.index, v.base_mip, v.mip_count, v.base_layer, v.layer_count, slot };
    if (const auto it = slices_.find(key); it != slices_.end()) return it->second;

    const gpu::allocated_image& src = ctx_->allocator.get_image(base);
    gpu::resource_allocator::view_range range;
    range.aspect = aspect_of(src.format);
    range.base_mip = v.base_mip;
    range.mip_count = v.mip_count == gpu::image_view::all ? src.mip_levels : v.mip_count;
    range.base_layer = v.base_layer;
    range.layer_count = v.layer_count == gpu::image_view::all ? src.array_layers : v.layer_count;
    range.view_type = range.layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

    const gpu::resource_id id = ctx_->allocator.create_view(base, range);
    slices_.emplace(key, id);
    return id;
}

namespace
{

}  // namespace

std::uint32_t compiled_frame::slot_for(gpu::image_view v, std::uint32_t pass_index,
                                       std::uint32_t slot) const
{
    const pass_decl& p = graph_->passes_[pass_index];
    // Match the SLICE, not just the image. A pass that reads mip m-1 and writes mip m declares two
    // uses of one image with different access — matching on the image alone collapses both to
    // whichever was declared first, so a storage write would resolve to a sampled descriptor.
    // Fall back to an image-level match only when no slice matches exactly.
    gpu::descriptor_type type = gpu::descriptor_type::TEXTURE;
    bool matched = false;
    // (Image path: descriptor_for only ever answers TEXTURE or STORAGE_IMAGE for image accesses.)
    for (const resource_use& u : p.uses)
    {
        if (!u.is_image() || !(u.img == v)) continue;
        type = descriptor_for(u.how);
        matched = true;
        break;
    }
    if (!matched)
        for (const resource_use& u : p.uses)
            if (u.is_image() && u.img.img == v.img) { type = descriptor_for(u.how); break; }

    gpu::image_view target = v;
    // Graceful degrade: if nothing alive produced this resource, the consumer transparently gets the
    // neutral substitute declared with it. The pass does not branch and does not know.
    if (!producer_alive(v.img) && graph_->images_[v.img.index].fallback.valid())
        target = graph_->images_[v.img.index].fallback.whole();

    const gpu::resource_id id = resolve_view(target, slot);
    if (id == 0) return 0;
    // LOOKUP ONLY. See the note on the other slot_for overload: binding here writes the descriptor
    // set mid-recording and invalidates the command buffer that already bound it.
    return ctx_->descriptor_table.get_binding_slot(id, type);
}

// The slot under a SPECIFIC access, for a pass that declared the same image two ways. The access is
// the disambiguator because it is what the pass already said; nothing new is being declared here.
std::uint32_t compiled_frame::slot_for(gpu::image_view v, access how, std::uint32_t pass_index,
                                       std::uint32_t slot) const
{
    const gpu::descriptor_type type = descriptor_for(how);
    gpu::image_view target = v;
    if (!producer_alive(v.img) && graph_->images_[v.img.index].fallback.valid())
        target = graph_->images_[v.img.index].fallback.whole();

    const gpu::resource_id id = resolve_view(target, slot);
    if (id == 0) return 0;
    // LOOKUP ONLY — never bind here. Binding writes the descriptor set, and the set is not
    // UPDATE_AFTER_BIND, so touching it while a command buffer that has already bound it is
    // recording invalidates that command buffer outright. Every declared resource is bound once at
    // compile (bind_declared_slots) precisely so this path cannot do it.
    return ctx_->descriptor_table.get_binding_slot(id, type);
}

std::uint32_t compiled_frame::slot_for(gpu::buffer h, std::uint32_t pass_index,
                                       std::uint32_t slot) const
{
    (void)pass_index;
    const gpu::resource_id id = physical(h, slot);
    if (id == 0) return 0;
    return ctx_->descriptor_table.get_binding_slot(id, gpu::descriptor_type::STORAGE_BUFFER);
}

// Is this resource produced by a pass that survived the frame's conditionals? A resource nothing
// writes at all is an external input and always counts as produced.
//
// PERSISTENT resources always count as produced, whatever happened to their writers this frame.
// Degrade exists because a TRANSIENT's contents are frame-scoped: if its producer did not run, there
// is genuinely nothing in it, so a consumer must be given a neutral value instead. A persistent's
// contents survive the frame, so "the producer was skipped" means "not updated", not "invalid" —
// substituting a fallback there would blank a resource that holds perfectly good data. The UI glyph
// atlas is the case that proves it: its upload pass is toggled off on every frame the atlas is not
// dirty, which is nearly all of them.
bool compiled_frame::producer_alive(gpu::image h) const
{
    if (!graph_->images_[h.index].transient) return true;

    bool written_by_anyone = false;
    for (std::uint32_t i = 0; i < graph_->passes_.size(); ++i)
    {
        for (const resource_use& u : graph_->passes_[i].uses)
        {
            if (!u.is_image() || !(u.img.img == h) || !is_write(u.how)) continue;
            written_by_anyone = true;
            if (alive_[i]) return true;
        }
    }
    return !written_by_anyone;
}

// Derive and emit every barrier one pass's declarations imply, against tracked state. This is the
// ONLY place image layout/access transitions originate for graph work: a pass declared what it
// touches, so nothing is left for pass code to hand-roll.
void compiled_frame::barrier_for(VkCommandBuffer cmd, const pass_decl& p, std::uint32_t slot,
                                 bool skip_attachments)
{
    for (const resource_use& u : p.uses)
    {
        if (u.is_image())
        {
            // Attachments are transitioned once before the render pass opens, not per pass inside it.
            const bool attachment = u.how == access::color_write || u.how == access::depth_write
                                 || u.how == access::depth_read;
            if (attachment && skip_attachments) continue;

            // The swapchain is the one image the allocator does not own: its backing is latched from
            // the acquire, so it has no allocator record to look up. Everything about it that a
            // barrier needs is already in hand.
            if (graph_->images_[u.img.img.index].swapchain)
            {
                if (swapchain_image_ == VK_NULL_HANDLE) continue;
                subresource sw;
                states_.track(swapchain_image_, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
                states_.transition(cmd, swapchain_image_, sw, u.how, u.stage);
                continue;
            }

            const gpu::resource_id id = physical(u.img.img, slot);
            if (id == 0) continue;
            const gpu::allocated_image& img = ctx_->allocator.get_image(id);

            subresource sub;
            sub.aspect = aspect_of(img.format);
            sub.base_mip = u.img.base_mip;
            sub.mip_count = u.img.mip_count == gpu::image_view::all ? img.mip_levels : u.img.mip_count;
            sub.base_layer = u.img.base_layer;
            sub.layer_count = u.img.layer_count == gpu::image_view::all ? img.array_layers : u.img.layer_count;

            states_.track(img.image, sub.aspect, img.mip_levels, img.array_layers);
            states_.transition(cmd, img.image, sub, u.how, u.stage);
        }
        else if (u.buf.valid())
        {
            const gpu::resource_id id = physical(u.buf, slot);
            if (id != 0) states_.buffer_access(id, u.how, u.stage);
        }
    }
}

VkImageAspectFlags compiled_frame::aspect_of(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Framework-opens. The passes declared their attachments; the group's position in each attachment's
// lifetime decides load/store, and a multisampled attachment paired with a 1-sample image of the
// same extent resolves into it. No pass hand-codes a clear, a load, or an MSAA resolve chain.
void compiled_frame::open_group(VkCommandBuffer cmd, const render_group& g, std::uint32_t slot,
                                VkExtent2D frame_extent)
{
    std::vector<VkRenderingAttachmentInfo> colors;
    VkRenderingAttachmentInfo depth{};
    bool has_depth = false;

    // The render area is the ATTACHMENT's extent, not the frame's. A shadow cascade group draws into
    // a fixed 2048x2048 map that has nothing to do with the window size; sizing the render area,
    // viewport and scissor from the frame viewport would rasterise only its top-left corner. The
    // graph knows every attachment's real extent, so it derives this rather than making each pass
    // set a viewport that would still leave renderArea wrong.
    VkExtent2D extent = frame_extent;
    for (const std::optional<gpu::image_view>& v : { g.color, g.depth })
    {
        if (!v) continue;
        const gpu::resource_id id = physical(v->img, slot);
        if (id == 0) continue;
        if (graph_->images_[v->img.index].swapchain) break;   // late-latched: the frame extent is right
        const VkExtent3D e = ctx_->allocator.get_image(id).extent;
        extent = { e.width, e.height };
        break;
    }

    for (const gpu::image_view& cv : g.colors)
    {
        const gpu::resource_id id = physical(cv.img, slot);
        if (id == 0) continue;
        const frame_graph::image_record& rec = graph_->images_[cv.img.index];

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        // A SLICE declared as an attachment must be bound as that slice's own view. A cube image's
        // default view is a CUBE view, which is not a valid render target — resolving through the
        // declaration is what makes a layered capture renderable at all.
        color.imageView = rec.swapchain ? swapchain_view_
                                        : ctx_->allocator.get_image(resolve_view(cv, slot)).view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = g.clears_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // The clear value is the resource's declared NEUTRAL. It already means "the value that
        // cancels" for degrade, and that is exactly what an attachment should clear to — the probe
        // capture's normal-depth target needs {0,0,0,-1} as its sky-miss sentinel, not black.
        color.clearValue.color = rec.neutral;
        if (g.color_resolve && &cv == &g.colors.front())
        {
            const gpu::resource_id rid = physical(g.color_resolve->img, slot);
            if (rid != 0)
            {
                color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                color.resolveImageView = ctx_->allocator.get_image(rid).view;
                color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        colors.push_back(color);
    }

    if (g.depth)
    {
        const gpu::resource_id id = physical(g.depth->img, slot);
        if (id != 0)
        {
            const gpu::allocated_image& img = ctx_->allocator.get_image(id);
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = ctx_->allocator.get_image(resolve_view(*g.depth, slot)).view;
            depth.imageLayout = g.depth_writes ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                               : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depth.loadOp = g.clears_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = { 0.0f, 0 };   // reverse-Z: far = 0
            if (g.depth_resolve)
            {
                const gpu::resource_id rid = physical(g.depth_resolve->img, slot);
                if (rid != 0)
                {
                    // Reverse-Z: MIN keeps the FARTHEST sample, the conservative choice for a HiZ
                    // occluder pyramid built from this depth.
                    depth.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
                    depth.resolveImageView = ctx_->allocator.get_image(rid).view;
                    depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                }
            }
            has_depth = true;
        }
    }

    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = { { 0, 0 }, extent },
        // layerCount and viewMask are mutually exclusive in dynamic rendering: with a view mask the
        // layers come from the mask. Both are DERIVED from the declared slice.
        .layerCount = g.view_mask ? 0u : 1u,
        .viewMask = g.view_mask,
        .colorAttachmentCount = static_cast<uint32_t>(colors.size()),
        .pColorAttachments = colors.empty() ? nullptr : colors.data(),
        .pDepthAttachment = has_depth ? &depth : nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(extent.width),
                                  static_cast<float>(extent.height), 0.0f, 1.0f };
    const VkRect2D scissor = { { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

// Clear each fallback to its declared neutral, once, the first time the frame runs. It happens here
// rather than at compile because a clear is a recorded command and compile records nothing — and it
// is one-shot rather than per-frame because a 1x1 constant never changes.
// Note: descriptor_for() is defined in the anonymous namespace above.
void compiled_frame::init_fallbacks(VkCommandBuffer cmd)
{
    if (pending_fallbacks_.empty()) return;

    for (gpu::image h : pending_fallbacks_)
    {
        const frame_graph::image_record& r = graph_->images_[h.index];
        if (r.physical.empty()) continue;
        const gpu::allocated_image& img = ctx_->allocator.get_image(r.physical[0]);
        const VkImageAspectFlags aspect = aspect_of(img.format);

        subresource sub;
        sub.aspect = aspect;
        states_.track(img.image, aspect, 1, 1);
        states_.transition(cmd, img.image, sub, access::transfer_write,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, /*discard=*/true);

        const VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            // The neutral for a depth resource is carried in the same float slot; for a shadow map
            // that is 1.0 — fully lit, which is what "no shadow pass ran" must look like.
            const VkClearDepthStencilValue value{ r.neutral.float32[0], 0 };
            vkCmdClearDepthStencilImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &value, 1, &range);
        }
        else
        {
            vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &r.neutral, 1, &range);
        }

        states_.transition(cmd, img.image, sub, access::sampled_read,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                               | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    pending_fallbacks_.clear();
}

void compiled_frame::execute(const execute_info& info)
{
    swapchain_ = info.swapchain;
    swapchain_image_ = info.swapchain_image;
    swapchain_view_ = info.swapchain_view;

    compute_survivors();

    VkCommandBuffer cmd = info.rec.vk();
    const std::uint32_t slot = info.frame_slot;

    init_fallbacks(cmd);

    std::size_t group_i = 0;
    std::uint32_t idx = 0;
    while (idx < order_.size())
    {
        const bool group_starts_here = group_i < groups_.size() && groups_[group_i].first == idx;
        if (group_starts_here)
        {
            const render_group& g = groups_[group_i];

            // If every pass in the group is skipped this frame, do not open the render pass at all.
            // Barriers are only emitted for LIVE passes, so opening it anyway binds attachments that
            // were never transitioned — the render pass then expects a layout nothing established.
            bool any_alive = false;
            for (std::uint32_t k = 0; k < g.count && !any_alive; ++k)
                any_alive = alive_[order_[g.first + k]];
            if (!any_alive) { idx += g.count; ++group_i; continue; }
            if (std::getenv("STRING_TRACE_PASS"))
                STRING_LOG_INFO("[group] first={} count={}", g.first, g.count);

            // Every barrier the whole group needs, derived and emitted BEFORE the render pass opens —
            // a barrier inside dynamic rendering is illegal, which is precisely why grouping is the
            // graph's job and not something a pass can be asked to get right.
            for (std::uint32_t k = 0; k < g.count; ++k)
            {
                const std::uint32_t pi = order_[g.first + k];
                if (!alive_[pi]) continue;
                barrier_for(cmd, graph_->passes_[pi], slot, /*skip_attachments=*/true);
            }
            for (std::uint32_t k = 0; k < g.count; ++k)
            {
                const std::uint32_t pi = order_[g.first + k];
                if (!alive_[pi]) continue;
                for (const resource_use& u : graph_->passes_[pi].uses)
                {
                    const bool attachment = u.is_image()
                        && (u.how == access::color_write || u.how == access::depth_write
                            || u.how == access::depth_read);
                    if (!attachment) continue;
                    // When ANY pass in the group writes depth, the attachment must be in the
                    // writable layout when the render pass opens. A later depth_read in the same
                    // group would otherwise transition it to READ_ONLY and win, leaving the bound
                    // layout contradicting the group's own attachment description. The write covers
                    // the reader anyway — ATTACHMENT_OPTIMAL permits the depth test.
                    if (u.how == access::depth_read && g.depth_writes) continue;
                    const bool is_swap = graph_->images_[u.img.img.index].swapchain;
                    if (is_swap && swapchain_image_ == VK_NULL_HANDLE) continue;
                    const gpu::resource_id id = is_swap ? 0 : physical(u.img.img, slot);
                    if (!is_swap && id == 0) continue;
                    VkImage vk_img = is_swap ? swapchain_image_ : ctx_->allocator.get_image(id).image;
                    subresource sub;
                    const std::uint32_t mips = is_swap ? 1u : ctx_->allocator.get_image(id).mip_levels;
                    const std::uint32_t layers = is_swap ? 1u : ctx_->allocator.get_image(id).array_layers;
                    sub.aspect = is_swap ? VK_IMAGE_ASPECT_COLOR_BIT
                                         : aspect_of(ctx_->allocator.get_image(id).format);
                    sub.mip_count = 1;
                    sub.layer_count = layers;
                    states_.track(vk_img, sub.aspect, mips, layers);
                    // A cleared attachment discards: no need to preserve contents nobody will read.
                    const bool discard = (u.how == access::color_write && g.clears_color)
                                      || (u.how == access::depth_write && g.clears_depth);
                    states_.transition(cmd, vk_img, sub, u.how, u.stage, discard);
                }
            }
            // The resolve targets are written by the render pass itself, at EndRendering, so the
            // graph transitions them like any other attachment write. This is what the retired
            // Access::DepthResolve marker and the tracker's seed() back door were both standing in
            // for: a write the tracker "could not observe" was only unobservable because the
            // framework did not own the resolve.
            for (const std::optional<gpu::image_view>& rv : { g.color_resolve, g.depth_resolve })
            {
                if (!rv) continue;
                const gpu::resource_id id = physical(rv->img, slot);
                if (id == 0) continue;
                const gpu::allocated_image& img = ctx_->allocator.get_image(id);
                const VkImageAspectFlags aspect = aspect_of(img.format);
                subresource sub;
                sub.aspect = aspect;
                sub.layer_count = img.array_layers;
                states_.track(img.image, aspect, img.mip_levels, img.array_layers);
                // A depth resolve completes in the LATE_FRAGMENT_TESTS stage, not COLOR_ATTACHMENT_OUTPUT —
                // the depth access flags are not even legal at the colour stage.
                const bool is_depth = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
                states_.transition(cmd, img.image, sub,
                                   is_depth ? access::depth_write : access::color_write,
                                   is_depth ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                                            : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   /*discard=*/true);
            }
            states_.flush_buffers(cmd);

            open_group(cmd, g, slot, info.extent);
            for (std::uint32_t k = 0; k < g.count; ++k)
            {
                const std::uint32_t pi = order_[g.first + k];
                if (!alive_[pi]) continue;
                pass_context pc{ info.rec, slot, info.extent };
                pc.frame = this;
                pc.pass_index = pi;
                graph_->passes_[pi].record(pc);
            }
            vkCmdEndRendering(cmd);

            idx += g.count;
            ++group_i;
            continue;
        }

        const std::uint32_t pi = order_[idx];
        const pass_decl& p = graph_->passes_[pi];
        if (std::getenv("STRING_TRACE_PASS")) STRING_LOG_INFO("[pass] {}", p.name);
        if (alive_[pi])
        {
            // An async-placed pass records onto the async lane's recorder when the hardware exposed
            // one; otherwise it records here. Same graph, same declarations, different placement.
            const bool async = p.lane == pass_lane::async && info.async_rec != nullptr;
            gpu::command_recorder& rec = async ? *info.async_rec : info.rec;
            VkCommandBuffer target = rec.vk();

            barrier_for(target, p, slot, /*skip_attachments=*/false);
            states_.flush_buffers(target);

            pass_context pc{ rec, slot, info.extent };
            pc.frame = this;
            pc.pass_index = pi;
            p.record(pc);
        }
        ++idx;
    }

    // The presentable image's final transition is derived like any other: a declared Present access
    // against whatever state the frame left it in.
    if (swapchain_image_ != VK_NULL_HANDLE)
    {
        subresource sub;
        sub.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        states_.track(swapchain_image_, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        states_.transition(cmd, swapchain_image_, sub, access::present,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }

    // Tracked state deliberately CARRIES across the frame boundary. An image's layout is a property
    // of the image, not of the frame that last touched it, so forgetting it at the boundary would
    // both re-emit redundant transitions and — the real defect — make a genuine cross-frame
    // dependency underivable. `hiz_depth_ring_` is exactly that case: GTAO reprojection reads the
    // PREVIOUS frame slot's resolved depth, and the two hand-rolled barriers that currently service
    // it (geometry_pass.cpp:1947, renderer.cpp:1437) exist only because nothing modelled it. Per-slot
    // backing means each slot's state is tracked against its own VkImage, so the graph derives it.
    // Only a resize, which destroys and recreates backing, invalidates this (see resize()).
}

// ================================================================================================
// pass_context — resolution, and the single place graceful degrade becomes visible to a shader
// ================================================================================================

gpu::resource_id pass_context::id(gpu::image h) const { return frame->physical(h, frame_slot); }
gpu::resource_id pass_context::id(gpu::buffer h) const { return frame->physical(h, frame_slot); }

VkImage pass_context::vk_image(gpu::image_view v) const
{
    const gpu::resource_id r = frame->physical(v.img, frame_slot);
    return r == 0 ? VK_NULL_HANDLE : frame->allocator().get_image(r).image;
}

VkImageView pass_context::view(gpu::image_view v) const
{
    const gpu::resource_id r = frame->resolve_view(v, frame_slot);
    return r == 0 ? VK_NULL_HANDLE : frame->allocator().get_image(r).view;
}

VkDeviceAddress pass_context::address(gpu::buffer h) const
{
    const gpu::resource_id r = frame->physical(h, frame_slot);
    return r == 0 ? 0 : frame->allocator().get_buffer(r).device_address;
}

void* pass_context::mapped(gpu::buffer h) const
{
    const gpu::resource_id r = frame->physical(h, frame_slot);
    return r == 0 ? nullptr : frame->allocator().get_buffer(r).allocation_info.pMappedData;
}

std::uint32_t pass_context::slot(gpu::image_view v) const
{
    return frame->slot_for(v, pass_index, frame_slot);
}

std::uint32_t pass_context::slot(gpu::image_view v, access how) const
{
    return frame->slot_for(v, how, pass_index, frame_slot);
}

std::uint32_t pass_context::slot(gpu::buffer h) const
{
    return frame->slot_for(h, pass_index, frame_slot);
}

}  // namespace string
