#pragma once

#include <Tracy.hpp>
#include <TracyVulkan.hpp>

#ifndef STRING_RELEASE
#define STRING_PROFILE_SCOPE(str) ZoneScopedN(str);
#define STRING_MARK_FRAME FrameMark;
#else
#define STRING_PROFILE_SCOPE(str) ((void)0)
#define STRING_MARK_FRAME ((void)0)
#endif  // STRING_RELEASE