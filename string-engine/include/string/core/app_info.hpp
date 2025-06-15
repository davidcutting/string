#pragma once

#include <string>
#include <filesystem>

namespace String
{
struct ApplicationInfo
{
    std::string application_name;
    std::filesystem::path resources_directory;
};
}