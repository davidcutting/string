#pragma once

// Profiling is opt-in via -DSTRING_PROFILE (requires the Tracy client + headers).
// When disabled the macros compile to no-ops so Tracy is not a build dependency.
#if defined(STRING_PROFILE) && !defined(STRING_RELEASE)
#include <Tracy.hpp>
#include <TracyVulkan.hpp>
#define STRING_PROFILE_SCOPE(str) ZoneScopedN(str);
#define STRING_MARK_FRAME FrameMark;
#else
// Call sites are written without a trailing ';' (it lives inside the macro),
// so the no-op form must terminate the statement itself.
#define STRING_PROFILE_SCOPE(str) ((void)0);
#define STRING_MARK_FRAME ((void)0);
#endif
