#include <gtest/gtest.h>

#include <algorithm>

#include <string/vulkan/frame_graph.hpp>

using namespace String;

namespace
{
// A no-op body; these tests exercise compile()'s ordering/enable/fallback logic, not recording.
RecordFn nop() { return [](string::gpu::command_recorder&, std::uint16_t) {}; }

bool has(const std::vector<string::gpu::resource_id>& v, string::gpu::resource_id r)
{
    return std::find(v.begin(), v.end(), r) != v.end();
}

const CompiledPass* find_pass(const CompiledFrame& f, std::string_view name)
{
    for (const auto& p : f.passes)
        if (p.name == name) return &p;
    return nullptr;
}
}  // namespace

// All passes enabled: the fluent layer lowers to the same stable topo order the planner produces
// for the classic depth -> cull -> shade chain (parity with render_graph_test's expectation).
TEST(FrameGraphTest, AllEnabledLowersToStableOrder)
{
    FrameGraph fg;
    ImageHandle depth = fg.image("depth");
    BufferHandle grid = fg.buffer("lightgrid");
    ImageHandle color = fg.image("color");

    fg.pass("depth").depth(depth).raster(nop());
    fg.pass("cull").read(depth.id, Access::DepthRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
                   .writes(grid).compute(nop());
    fg.pass("shade").reads(grid).color(color).raster(nop());

    CompiledFrame f = fg.compile();
    ASSERT_EQ(f.passes.size(), 3u);
    EXPECT_EQ(f.passes[0].name, "depth");
    EXPECT_EQ(f.passes[1].name, "cull");
    EXPECT_EQ(f.passes[2].name, "shade");
    // No fallbacks when every producer is live.
    for (const auto& p : f.passes) EXPECT_TRUE(p.fallback_reads.empty());
}

// A pass with no .toggle() is always enabled.
TEST(FrameGraphTest, NoToggleMeansAlwaysEnabled)
{
    FrameGraph fg;
    ImageHandle color = fg.image("color");
    fg.pass("only").color(color).raster(nop());
    EXPECT_EQ(fg.compile().passes.size(), 1u);
}

// Disabling a PRODUCER does not skip its OPTIONAL consumer: the consumer survives and the read is
// reported as a fallback (the executor will bind a neutral resource). This is the core
// graceful-degrade behaviour — toggle shadows off, the lit pass still runs.
TEST(FrameGraphTest, DisabledProducerGivesOptionalConsumerAFallback)
{
    bool cull_enabled = false;
    FrameGraph fg;
    ImageHandle depth = fg.image("depth");
    BufferHandle grid = fg.buffer("lightgrid");
    ImageHandle color = fg.image("color");

    fg.pass("depth").depth(depth).raster(nop());
    fg.pass("cull").toggle(&cull_enabled)
                   .read(depth.id, Access::DepthRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
                   .writes(grid).compute(nop());
    fg.pass("shade").reads(grid).color(color).raster(nop());  // optional read of grid

    CompiledFrame f = fg.compile();
    EXPECT_EQ(f.passes.size(), 2u);            // cull dropped
    EXPECT_FALSE(find_pass(f, "cull"));
    const CompiledPass* shade = find_pass(f, "shade");
    ASSERT_TRUE(shade);
    EXPECT_TRUE(has(shade->fallback_reads, grid.id));   // grid falls back to neutral
    // The fallback read must NOT appear as a real usage (no ordering edge against an unproduced
    // resource).
    for (const auto& u : shade->usages) EXPECT_NE(u.resource, grid.id);
}

// A .requires() read of a disabled producer transitively skips the consumer too (no sensible
// neutral for an essential input).
TEST(FrameGraphTest, RequiredReadOfDisabledProducerCascades)
{
    bool cull_enabled = false;
    FrameGraph fg;
    BufferHandle grid = fg.buffer("lightgrid");
    ImageHandle color = fg.image("color");

    fg.pass("cull").toggle(&cull_enabled).writes(grid).compute(nop());
    fg.pass("shade").requires_(grid).color(color).raster(nop());  // grid is essential

    CompiledFrame f = fg.compile();
    EXPECT_TRUE(f.passes.empty());   // both gone: cull disabled, shade cascaded
}

// The cascade is transitive: A(off) -> B requires A's output -> C requires B's output. All drop.
TEST(FrameGraphTest, RequiredCascadeIsTransitive)
{
    bool a_enabled = false;
    FrameGraph fg;
    BufferHandle ra = fg.buffer("a_out");
    BufferHandle rb = fg.buffer("b_out");
    ImageHandle color = fg.image("color");

    fg.pass("A").toggle(&a_enabled).writes(ra).compute(nop());
    fg.pass("B").requires_(ra).writes(rb).compute(nop());
    fg.pass("C").requires_(rb).color(color).raster(nop());

    EXPECT_TRUE(fg.compile().passes.empty());
}

// An optional read of an EXTERNAL input (a resource no pass writes — an imported texture/buffer)
// is never a fallback: external inputs are always available.
TEST(FrameGraphTest, ExternalInputIsNeverAFallback)
{
    FrameGraph fg;
    BufferHandle external = fg.import_buffer(4242);   // nothing writes this
    ImageHandle color = fg.image("color");

    fg.pass("shade").reads(external).color(color).raster(nop());

    CompiledFrame f = fg.compile();
    ASSERT_EQ(f.passes.size(), 1u);
    EXPECT_TRUE(f.passes[0].fallback_reads.empty());
    // external input still appears as a real usage.
    bool seen = false;
    for (const auto& u : f.passes[0].usages) seen |= (u.resource == external.id);
    EXPECT_TRUE(seen);
}

// Re-enabling the producer restores the full graph (toggle is re-evaluated each compile — the
// persistent-plan invalidation model reruns compile on toggle).
TEST(FrameGraphTest, ReenablingProducerRestoresConsumerEdge)
{
    bool cull_enabled = false;
    FrameGraph fg;
    BufferHandle grid = fg.buffer("lightgrid");
    ImageHandle color = fg.image("color");
    fg.pass("cull").toggle(&cull_enabled).writes(grid).compute(nop());
    fg.pass("shade").reads(grid).color(color).raster(nop());

    EXPECT_EQ(fg.compile().passes.size(), 1u);   // cull off -> shade survives w/ fallback
    cull_enabled = true;
    CompiledFrame f = fg.compile();
    ASSERT_EQ(f.passes.size(), 2u);
    const CompiledPass* shade = find_pass(f, "shade");
    ASSERT_TRUE(shade);
    EXPECT_TRUE(shade->fallback_reads.empty());   // grid now produced -> real edge, no fallback
    EXPECT_EQ(f.passes[0].name, "cull");          // producer ordered before consumer
    EXPECT_EQ(f.passes[1].name, "shade");
}
