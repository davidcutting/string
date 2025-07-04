#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

using ResourceID = std::size_t;
using TaskID = std::size_t;
using PipelineID = std::size_t;

enum class AccessType : std::uint8_t
{
    READ,
    WRITE,
    READ_WRITE,
};

struct Resource
{
    ResourceID id;
    AccessType access_type;
};

struct Buffer
{

};

struct Image
{
};

struct Window
{
  
};

struct PipelineManager
{
    auto add_compute_pipeline() -> void;
    auto add_graphics_pipeline() -> void;
};

struct CommandRecorder
{
    void begin_rendering(const ResourceID& target);
    void end_rendering();

    void bind_pipeline(const PipelineID& pipeline);
    
    auto draw() -> CommandRecorder&;

    void blit_image(const ResourceID& source, const ResourceID& destination);
};

struct Device
{
    auto get_command_recorder() -> CommandRecorder&;
};

struct Presentation
{
    void present(const Image& image);
};

class Platform
{
    class Impl* impl_;
public:
    static auto initialize() -> Platform;
    auto create_device() -> Device;
    auto create_window() -> Window;
    auto create_presentation(const Window& window, const Device& device) -> Presentation;
};

using TaskFunction = std::function<void(CommandRecorder& recorder)>;

struct Task
{
    std::string name;
    std::vector<Resource> resources;
    std::vector<PipelineID> pipelines;
    TaskFunction task;
};

struct TaskGraph
{
    struct Node
    {
        TaskID id;
        Task task;
        std::vector<TaskID> dependencies;
    };
    std::vector<Node> nodes;
};

struct TaskDescription
{
};


int main(int argc, char* argv[])
{
    Platform platform = Platform::initialize();
    Window window = platform.create_window();
    Device device = platform.create_device();
    Presentation presentation = platform.create_presentation(window, device);

    auto& recorder = device.get_command_recorder();


    

    return EXIT_SUCCESS;
}
