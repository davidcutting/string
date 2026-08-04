#pragma once

// Profiling is opt-in via -DSTRING_PROFILE (requires the Tracy client + headers).
// When disabled the macros compile to no-ops so Tracy is not a build dependency.
//
// Macro discipline (keep it): call sites are written WITHOUT a trailing ';' — the terminating
// ';' lives inside every macro, and the no-op fallback must therefore terminate its own
// statement too. Every macro below has both an enabled and a no-op form.
#if defined(STRING_PROFILE) && !defined(STRING_RELEASE)
// TracyVulkan.hpp needs the Vulkan symbols (VkDevice, PFN_vkGetCalibratedTimestampsEXT, ...) in
// scope before it is included. The engine loads Vulkan through volk, so pull it in here — every
// TU that includes this header (even non-GPU ones) then compiles TracyVulkan.hpp cleanly.
#include <volk.h>

// Use the tracy/-prefixed include path: it works with both the meson wrap (which exports the
// `public/` dir, so both <Tracy.hpp> and <tracy/Tracy.hpp> resolve) AND the nixpkgs `tracy`
// package (CMake config exports include/tracy, so only the tracy/-prefixed form resolves).
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

// --- CPU zones --------------------------------------------------------------
// Named scoped zone: STRING_PROFILE_SCOPE("Acquire") — literal name, no per-frame cost.
#define STRING_PROFILE_SCOPE(str) ZoneScopedN(str);
// Dynamic zone name (runtime string, e.g. a pass's debug_name()). ZoneScoped opens the zone;
// ZoneName attaches the text. Both must run, hence the block.
#define STRING_PROFILE_SCOPE_DYNAMIC(str_ptr, len) \
    ZoneScoped; ZoneName(str_ptr, len);
// Frame boundary marker (call once per presented frame).
#define STRING_MARK_FRAME FrameMark;

// Name the calling thread in the Tracy timeline (call once, early, on that thread).
#define STRING_PROFILE_THREAD(name) tracy::SetThreadName(name);

// Plot a numeric value over time (frame ms, resident bytes, draw count, ...).
#define STRING_PROFILE_PLOT(name, value) TracyPlot(name, value);
// One-off log message on the timeline.
#define STRING_PROFILE_MESSAGE(str) TracyMessageL(str);

// --- GPU zones --------------------------------------------------------------
// A Tracy Vulkan context handle type, so engine code can hold one without leaking Tracy into
// headers that don't want it. The context is created/destroyed via the macros below.
#define STRING_PROFILE_GPU_CONTEXT_TYPE TracyVkCtx

// Create a (calibrated if available) GPU context on a queue. Needs a one-time command buffer to
// probe the timestamp period. Assigns to `dst`.
#define STRING_PROFILE_GPU_CONTEXT_CREATE(dst, phys, dev, queue, cmd) \
    dst = TracyVkContextCalibrated(phys, dev, queue, cmd, \
        vkGetPhysicalDeviceCalibrateableTimeDomainsEXT, vkGetCalibratedTimestampsEXT);
// Non-calibrated fallback (device lacks VK_EXT_calibrated_timestamps).
#define STRING_PROFILE_GPU_CONTEXT_CREATE_BASIC(dst, phys, dev, queue, cmd) \
    dst = TracyVkContext(phys, dev, queue, cmd);
#define STRING_PROFILE_GPU_CONTEXT_DESTROY(ctx) if (ctx) { TracyVkDestroy(ctx); }
// Name the GPU context in the Tracy UI.
#define STRING_PROFILE_GPU_CONTEXT_NAME(ctx, name, len) if (ctx) { TracyVkContextName(ctx, name, len); }

// A GPU zone scoped to the given command buffer (literal name).
#define STRING_PROFILE_GPU_ZONE(ctx, cmd, name) TracyVkZone(ctx, cmd, name);
// GPU zone with a runtime name — `cstr` must be a NUL-terminated const char* (Tracy strlen's it).
// e.g. STRING_PROFILE_GPU_ZONE_DYNAMIC(ctx, cmd, pass->debug_name().c_str())
#define STRING_PROFILE_GPU_ZONE_DYNAMIC(ctx, cmd, cstr) \
    TracyVkZoneTransient(ctx, STRING_PROFILE_ZONE_VAR(__LINE__), cmd, cstr, true);
#define STRING_PROFILE_ZONE_VAR_CAT(a, b) a##b
#define STRING_PROFILE_ZONE_VAR(line) STRING_PROFILE_ZONE_VAR_CAT(_string_gpu_zone_, line)

// Collect timestamps for the ctx into `cmd` (call once per frame, on a recording buffer).
#define STRING_PROFILE_GPU_COLLECT(ctx, cmd) TracyVkCollect(ctx, cmd);

#else
// ---- No-op fallbacks (profiling off): each terminates its own statement -----
#define STRING_PROFILE_SCOPE(str) ((void)0);
#define STRING_PROFILE_SCOPE_DYNAMIC(str_ptr, len) ((void)0);
#define STRING_MARK_FRAME ((void)0);
#define STRING_PROFILE_THREAD(name) ((void)0);
#define STRING_PROFILE_PLOT(name, value) ((void)0);
#define STRING_PROFILE_MESSAGE(str) ((void)0);

// GPU context handle collapses to a nullable pointer so engine code can still declare/hold one.
#define STRING_PROFILE_GPU_CONTEXT_TYPE void*
#define STRING_PROFILE_GPU_CONTEXT_CREATE(dst, phys, dev, queue, cmd) (dst) = nullptr;
#define STRING_PROFILE_GPU_CONTEXT_CREATE_BASIC(dst, phys, dev, queue, cmd) (dst) = nullptr;
#define STRING_PROFILE_GPU_CONTEXT_DESTROY(ctx) ((void)(ctx));
#define STRING_PROFILE_GPU_CONTEXT_NAME(ctx, name, len) ((void)0);
#define STRING_PROFILE_GPU_ZONE(ctx, cmd, name) ((void)0);
#define STRING_PROFILE_GPU_ZONE_DYNAMIC(ctx, cmd, cstr) ((void)0);
#define STRING_PROFILE_GPU_COLLECT(ctx, cmd) ((void)0);
#endif
