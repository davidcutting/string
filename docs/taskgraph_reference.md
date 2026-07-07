# TaskGraph reference (render-graph design vision)

Salvaged from a commented-out block in `string-engine/include/string/vulkan/resource.hpp`
(moved here 2026-07-06 during the Vulkan homogenization pass). This is a Daxa-style
TaskGraph / ResourceManager / streaming sketch — the **Stage 3 render-graph target**, not
current code. Kept as a reference for the resource-usage + transient-management API the
graph will grow into. See the staged plan (homogenize -> typed usage -> state tracking ->
graph) and `render_graph.hpp` (the working planner half: toposort + lifetimes).

```cpp
struct Resource
{
    std::span<std::byte> data;
    uint64_t last_task_used;
    std::atomic<uint8_t> ref_count;
    ResourceType resource_type;
    MemoryType memory_type;
    StreamStatus stream_status;
    ResourcePriority priority;
};

ResourceID generate_resource_id(const std::string& asset_path, const std::string& sub_resource = "");

class ResourceManager
{
public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    // Returns a non-owning reference
    Resource& get_resource(const ResourceID& id);

    size_t get_resource_count() const;

    void want_resource(const ResourceID& id, const ResourcePriority& priority, const MemoryType& memory_type, const ResourceType& resource_type);
    void unwant_resource(const ResourceID& id);

    // Call to clean up old resources, potentially also defragment?
    void collect_garbage(uint64_t safe_timeline_value);

private:
    // Use EnTT instead?
    std::unordered_map<ResourceID, Resource> resource_registry_;
    std::mutex registry_mutex_;

    std::vector<ResourceID> cleanup_queue_;
    std::mutex cleanup_mutex_;
};

struct TaskBuffer
{
    std::vector<ResourceID> initial_buffers;  // For persistent resources
    ResourceID transient_buffer = 0;          // For transient resources
    std::string name;
    MemoryType memory_type = MemoryType::GPU_LOCAL;
    
    // Runtime state - managed by TaskGraph
    ResourceID current_buffer = 0;
    
    void set_buffers(std::span<ResourceID> buffers) {
        if (!buffers.empty()) {
            current_buffer = buffers[0];
        }
    }
};

struct TaskImage
{
    std::vector<ResourceID> initial_images;
    ResourceID transient_image = 0;
    bool swapchain_image = false;
    std::string name;
    MemoryType memory_type = MemoryType::GPU_LOCAL;
    
    // Runtime state
    ResourceID current_image = 0;
    
    void set_images(std::span<ResourceID> images) {
        if (!images.empty()) {
            current_image = images[0];
        }
    }
};

class ITask
{
public:
    ITask(const std::string& name) : name_(name) {}
    virtual ~ITask() = default;
    const std::string& get_name() const { return name_; }
protected:
    std::string name_;
};

class Task : public ITask
{
public:
    Task(const std::string& name) : ITask(name) {}
    virtual ~Task() = default;

    // Called by TaskGraph during complete() phase to analyze resource usage
    virtual void callback_on_complete(TaskGraph& task_graph) {}
    
    // Called during execute() phase
    virtual void callback_on_execute(TaskGraph& task_graph) = 0;
};

struct TaskBufferAttachmentInfo : TaskBufferAttachment
{
    TaskBufferView view = {};
    TaskBufferView translated_view = {};
    std::span<BufferId const> ids = {};
};

struct TaskBlasAttachmentInfo : TaskBlasAttachment
{
    TaskBlasView view = {};
    TaskBlasView translated_view = {};
    std::span<BlasId const> ids = {};
};

struct TaskTlasAttachmentInfo : TaskTlasAttachment
{
    TaskTlasView view = {};
    TaskTlasView translated_view = {};
    std::span<TlasId const> ids = {};
};

struct TaskImageAttachmentInfo : TaskImageAttachment
{
    TaskImageView view = {};
    TaskImageView translated_view = {};
    ImageLayout layout = {};
    std::span<ImageId const> ids = {};
    std::span<ImageViewId const> view_ids = {};
};

struct TaskInterface
{
    Device& device;
    CommandRecorder & recorder;
    std::span<TaskAttachmentInfo const> attachment_infos = {};
    // optional:
    TransferMemoryPool * allocator = {};
    std::span<std::byte const> attachment_shader_blob = {};
    std::string_view task_name = {};
    usize task_index = {};

    auto get(TaskBufferAttachmentIndex index) const -> TaskBufferAttachmentInfo const &;
    auto get(TaskBufferView view) const -> TaskBufferAttachmentInfo const &;
    auto get(TaskBlasAttachmentIndex index) const -> TaskBlasAttachmentInfo const &;
    auto get(TaskBlasView view) const -> TaskBlasAttachmentInfo const &;
    auto get(TaskTlasAttachmentIndex index) const -> TaskTlasAttachmentInfo const &;
    auto get(TaskTlasView view) const -> TaskTlasAttachmentInfo const &;
    auto get(TaskImageAttachmentIndex index) const -> TaskImageAttachmentInfo const &;
    auto get(TaskImageView view) const -> TaskImageAttachmentInfo const &;
    auto get(usize index) const -> TaskAttachmentInfo const &;

    auto info(TaskIndexOrView auto tresource, u32 array_index = 0) const
    {
        return this->device.info(this->get(tresource).ids[array_index]);
    }
    auto image_view_info(TaskImageIndexOrView auto timage, u32 array_index = 0) const -> Optional<ImageViewInfo>
    {
        return this->device.image_view_info(this->get(timage).view_ids[array_index]);
    }
    auto device_address(TaskBufferBlasOrTlasIndexOrView auto tresource, u32 array_index = 0) const -> Optional<DeviceAddress>
    {
        return this->device.device_address(this->get(tresource).ids[array_index]);
    }
    auto buffer_host_address(TaskBufferIndexOrView auto tbuffer, u32 array_index = 0) const -> Optional<std::byte *>
    {
        return this->device.buffer_host_address(this->get(tbuffer).ids[array_index]);
    }
    auto id(TaskIndexOrView auto tresource, u32 index = 0)
    {
        return this->get(tresource).ids[index];
    }
    auto view(TaskImageIndexOrView auto timg, u32 index = 0)
    {
        return this->get(timg).view_ids[index];
    }
};

enum class TaskGraphState
{
    BUILDING,     // Adding tasks and resources
    COMPILED,     // complete() called, ready to execute
    EXECUTING     // execute() in progress
};

struct TaskGraphInfo
{
    // config
};

struct TaskGraph
{
    TaskGraph() = default;

    TaskGraph(TaskGraphInfo const & info);
    ~TaskGraph();

    void use_persistent_buffer(TaskBuffer const & buffer);
    void use_persistent_blas(TaskBlas const & blas);
    void use_persistent_tlas(TaskTlas const & tlas);
    void use_persistent_image(TaskImage const & image);

    auto create_transient_buffer(TaskTransientBufferInfo const & info) -> TaskBufferView;
    auto create_transient_image(TaskTransientImageInfo const & info) -> TaskImageView;

    auto transient_buffer_info(TaskBufferView const & transient) -> TaskTransientBufferInfo const &;
    auto transient_image_info(TaskImageView const & transient) -> TaskTransientImageInfo const &;

    void clear_buffer(TaskBufferClearInfo const & info);
    void clear_image(TaskImageClearInfo const & info);

    void copy_buffer_to_buffer(TaskBufferCopyInfo const & info);
    void copy_image_to_image(TaskImageCopyInfo const & info);

    void add_task(InlineTaskInfo const & inline_task_info)
    {
        add_task(std::make_unique<InlineTask>(inline_task_info));
    }

    void conditional(TaskGraphConditionalInfo const & conditional_info);
    void submit(TaskSubmitInfo const & info);
    void present(TaskPresentInfo const & info);

    // TODO: make move only. Return ExecutableTaskGraph.
    void complete(TaskCompleteInfo const & info);

    void execute(ExecutionInfo const & info);

    auto get_debug_string() -> std::string;
    auto get_transient_memory_size() -> daxa::usize;
};

class Scene
{
public:
    Scene() = default;
    
    void load_open_usd(const std::string& file_path);
    void load_gltf(const std::string& file_path);
    
private:
    entt::registry registry_;
    std::string scene_name_;
};
```
