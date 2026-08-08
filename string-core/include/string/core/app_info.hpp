#pragma once

#include <string>
#include <filesystem>

namespace string
{
struct ApplicationInfo
{
    std::string application_name;
    std::filesystem::path executable_directory;
    std::filesystem::path resources_directory;
    std::filesystem::path user_config_directory;
    std::filesystem::path user_cache_directory;
};
}