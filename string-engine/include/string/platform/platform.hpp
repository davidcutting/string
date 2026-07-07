#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <experimental/propagate_const>
#include <string/platform/event.hpp>
#include <string/platform/window.hpp>
#include <string/device.hpp>

namespace String
{

enum class FileEventType : std::uint8_t
{
    CREATED,
    MODIFIED,
    DELETED
};

struct FileWatchEvent
{
    std::string file_path;
    FileEventType type;
};

using FileEventCallback = std::function<void(const FileWatchEvent&)>;

struct Version
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;  
};

struct PlatformInfo
{
    std::string application_name = "String Application";
    Version application_version = { 0, 0, 1 };
};

class Platform
{
    class Impl;
    using impl_t = std::experimental::propagate_const<std::unique_ptr<Impl>>;
    impl_t impl_;
public:
    explicit Platform(const PlatformInfo& platform_info);
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;
    
    auto create_window(const WindowInfo& window_info) -> std::unique_ptr<Window>;
    auto create_device(const DeviceInfo& device_info) -> std::unique_ptr<string::gpu::device>;

    auto read_file(const std::filesystem::path& path) -> std::span<const std::byte>;
    void write_file(const std::filesystem::path& path, const std::span<const std::byte>& data);
    void watch_file(const std::filesystem::path& path, const FileEventCallback&& callback);
    void unwatch_file(const std::filesystem::path& path);

    // NetworkManager& get_network_manager();
    // AudioManager& get_audio_manager();
};

}

/** Example usage
int main() {
    using namespace String;
    
    // No platform-specific symbols pollute the user's namespace
    // No GLFW headers needed in user code
    // No template complexity
    
    Platform platform;
    
    auto window = platform.create_window({
        .properties = {
            .title = "Clean Game Engine",
            .extent = {1920, 1080}
        },
        .key_callback = [](const KeyEvent& e) {
            std::cout << "Key: " << e.key << std::endl;
        }
    });
    
    while (!window->should_close()) {
        window->update();
        // Your game code here - no platform concerns!
    }
    
    return 0;
}
*/
