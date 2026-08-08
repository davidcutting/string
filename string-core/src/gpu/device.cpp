#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string/gpu/device.hpp>
#include <string/gpu/vk_check.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/queue.hpp>

#include <volk.h>

namespace string::gpu
{

device::device(driver& driver, std::shared_ptr<string::Window> window)
: window_(std::move(window))
, driver_(driver)
, surface_(window_->create_surface(driver_.get_instance()))
{
    select_physical_device();
    create_logical_device();
}

device::~device()
{
    if (device_ != VK_NULL_HANDLE)
        vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(driver_.get_instance(), surface_, nullptr);
}

bool device::check_device_extension_support(const VkPhysicalDevice& device) {
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extensions.data());

    std::set<std::string> required_extensions(device_extensions.begin(), device_extensions.end());

    for (const auto& extension : available_extensions) {
        required_extensions.erase(extension.extensionName);
    }

    return required_extensions.empty();
}

bool device::is_device_suitable(const VkPhysicalDevice& device) {
    queue_family_indices indices = get_queue_families(device);

    bool extensions_supported = check_device_extension_support(device);

    bool swap_chain_adequate = false;
    if (extensions_supported) {
        swap_chain_support_details swapChainSupport = get_swap_chain_support(device);
        swap_chain_adequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    // Set up feature query
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features{};
    robustness2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    robustness2_features.pNext = nullptr;

    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{};
    dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic_rendering_features.pNext = &robustness2_features;

    // Task + mesh shader stages (brief 03 meshlet pipeline). Queried here, required below.
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features{};
    mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    mesh_shader_features.pNext = &dynamic_rendering_features;

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2_features{};
    extended_dynamic_state2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    extended_dynamic_state2_features.pNext = &mesh_shader_features;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT extended_dynamic_state3_features{};
    extended_dynamic_state3_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    extended_dynamic_state3_features.pNext = &extended_dynamic_state2_features;

    VkPhysicalDeviceVulkan12Features vulkan12_features{};
    vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12_features.pNext = &extended_dynamic_state2_features;
    vulkan12_features.bufferDeviceAddress = true;
    vulkan12_features.descriptorBindingPartiallyBound = true;
    vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = true;

    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.pNext = &vulkan12_features;

    VkPhysicalDeviceFeatures2 supported_features{};
    supported_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_features.pNext = &vulkan13_features;

    // Query what features are supported (the structs above are filled in by the driver).
    vkGetPhysicalDeviceFeatures2(device, &supported_features);

    // Bindless requires runtime-sized descriptor arrays, non-uniform indexing into them,
    // partially-bound sets, and update-after-bind for the descriptor types the global table
    // exposes (storage buffers + sampled images). These are what the descriptor_table relies
    // on; a device missing any of them cannot drive the renderer.
    const bool has_bindless_features = vulkan12_features.runtimeDescriptorArray == VK_TRUE
        && vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE
        && vulkan12_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE
        && vulkan12_features.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE
        && vulkan12_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE
        && vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE
        && vulkan12_features.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE
        && vulkan12_features.descriptorBindingVariableDescriptorCount == VK_TRUE
        // nullDescriptor: required so a released bindless slot can be nulled rather than left
        // dangling at a destroyed resource (descriptor_table::unbind).
        && robustness2_features.nullDescriptor == VK_TRUE;

    // And the arrays must be large enough for the table's advertised capacities.
    VkPhysicalDeviceDescriptorIndexingProperties indexing_props{};
    indexing_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 device_props{};
    device_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    device_props.pNext = &indexing_props;
    vkGetPhysicalDeviceProperties2(device, &device_props);

    const bool has_bindless_capacity =
        indexing_props.maxDescriptorSetUpdateAfterBindSampledImages >= descriptor_table::MAX_BINDLESS_IMAGES
        && indexing_props.maxDescriptorSetUpdateAfterBindStorageBuffers >= descriptor_table::MAX_BINDLESS_BUFFERS;

    // Check if desired features are supported
    bool has_desired_features = supported_features.features.samplerAnisotropy
        && supported_features.features.multiDrawIndirect
        && supported_features.features.drawIndirectFirstInstance
        // BC (block-compression) texture formats: cooked assets load as KTX2 transcoded to BC7,
        // so a device that can't sample BC formats can't render the scene. Desktop-baseline.
        && supported_features.features.textureCompressionBC == VK_TRUE
        && vulkan13_features.dynamicRendering == VK_TRUE
        && vulkan13_features.synchronization2 == VK_TRUE
        && vulkan13_features.shaderDemoteToHelperInvocation == VK_TRUE
        && vulkan12_features.timelineSemaphore == VK_TRUE
        && vulkan12_features.bufferDeviceAddress == VK_TRUE
        // GPU-driven draw generation (brief 03b): vkCmdDrawMeshTasksIndirectCountEXT reads the
        // draw count from a GPU buffer the cull compute writes.
        && vulkan12_features.drawIndirectCount == VK_TRUE
        && extended_dynamic_state2_features.extendedDynamicState2 == VK_TRUE
        // Meshlet pipeline (brief 03): both stages required, no fallback path.
        && mesh_shader_features.taskShader == VK_TRUE
        && mesh_shader_features.meshShader == VK_TRUE
        && has_bindless_features
        && has_bindless_capacity;

    return indices.can_render() && extensions_supported && swap_chain_adequate && has_desired_features;
}

void device::select_physical_device()
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(driver_.get_instance(), &device_count, nullptr);
    if (device_count == 0) {
        throw std::runtime_error("Failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(driver_.get_instance(), &device_count, devices.data());

    for (const auto& device : devices) {
        if (is_device_suitable(device)) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            // Some laptops, like one of my own, have both an integrated GPU and a discrete
            // one. It is not always the case that the discrete GPU is selected or prioritized
            // without this check.
            // TODO(DCut): Handle multiple discrete GPU, or utilize both discrete and integrated
            if (props.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device_ = device;
                break;
            }
            // Fallback to integrated
            if (props.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                physical_device_ = device;
                break;
            }
            // Fallback to use whatever device available
            physical_device_ = device;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU!");
    }

    // Cache the selected device's immutable data once; every later query reuses these.
    vkGetPhysicalDeviceProperties(physical_device_, &properties_);
    queue_family_indices_ = get_queue_families(physical_device_);
    build_capability_table();
    STRING_LOG_INFO("device name: {}", properties_.deviceName);
}

// Brief 04e M1: enumerate what the selected device's queue families ACTUALLY offer — family
// flags, per-family queue counts, present support, timestamp validity. This is the single source
// the lane set (and later the graph's placement policy) is derived from; nothing downstream
// re-probes or assumes a hardware shape.
void device::build_capability_table()
{
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());

    family_caps_.clear();
    for (uint32_t i = 0; i < family_count; ++i)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present);
        family_caps_.push_back(queue_family_caps{
            .index = i,
            .flags = families[i].queueFlags,
            .queue_count = families[i].queueCount,
            .present_support = present == VK_TRUE,
            .timestamps = families[i].timestampValidBits > 0,
        });
        STRING_LOG_INFO(
            "queue family {}: count={} flags={:#x} present={} timestamps={}",
            i, families[i].queueCount, families[i].queueFlags,
            present == VK_TRUE, families[i].timestampValidBits > 0);
    }
}

// How many queues the lane plan wants from a family. Graphics/present/transfer families carry one
// lane each; a DEDICATED compute family carries up to four async-compute lanes (RDNA3 shape). The
// count is clamped to what the family exposes — fewer queues means fewer lanes exist.
static uint32_t desired_queue_count(const queue_family_caps& caps)
{
    const bool graphics = caps.flags & VK_QUEUE_GRAPHICS_BIT;
    const bool compute = caps.flags & VK_QUEUE_COMPUTE_BIT;
    if (!graphics && compute)
        return std::min(caps.queue_count, 4u);
    return std::min(caps.queue_count, 1u);
}

// Brief 04e M1: retrieve every created queue and name it as a submission lane. Lane existence is
// capability-derived: "main" always (graphics is required), "async-compute-N" per dedicated
// compute queue, "transfer" only when a dedicated transfer family exists.
void device::build_submission_lanes()
{
    lanes_.clear();
    const queue_family_indices& indices = queue_family_indices_;

    const auto caps_of = [&](uint32_t family) -> const queue_family_caps& {
        return family_caps_[family];
    };
    const auto add_lane = [&](std::string name, queue_type type, uint32_t family, uint32_t index) {
        submission_lane lane{
            .name = std::move(name),
            .type = type,
            .queue_family_index = family,
            .queue_index = index,
            .vk_queue = VK_NULL_HANDLE,
            .timestamps = caps_of(family).timestamps,
        };
        vkGetDeviceQueue(device_, family, index, &lane.vk_queue);
        lanes_.push_back(std::move(lane));
    };

    add_lane("main", queue_type::GRAPHICS, indices.graphics_family.value(), 0);
    if (indices.compute_family.has_value())
    {
        const uint32_t count = desired_queue_count(caps_of(indices.compute_family.value()));
        for (uint32_t i = 0; i < count; ++i)
            add_lane("async-compute-" + std::to_string(i), queue_type::COMPUTE,
                     indices.compute_family.value(), i);
    }
    if (indices.transfer_family.has_value())
        add_lane("transfer", queue_type::TRANSFER, indices.transfer_family.value(), 0);

    std::string lane_names;
    for (const submission_lane& lane : lanes_)
        lane_names += (lane_names.empty() ? "" : ", ") + lane.name;
    STRING_LOG_INFO("submission lanes: {}", lane_names);
}

const submission_lane* device::lane(std::string_view name) const
{
    for (const submission_lane& lane : lanes_)
        if (lane.name == name) return &lane;
    return nullptr;
}

void device::create_logical_device()
{
    const queue_family_indices& indices = queue_family_indices_;

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    // Only request families the device actually reports. compute/transfer are optional
    // (many GPUs expose no dedicated compute or transfer family), so calling .value()
    // unconditionally would throw on them. graphics is guaranteed by is_device_suitable().
    std::set<uint32_t> uniqueQueueFamilies;
    for (const std::optional<uint32_t>& family : {
             indices.graphics_family,
             indices.compute_family,
             indices.transfer_family,
             indices.present_family })
    {
        if (family.has_value())
            uniqueQueueFamilies.insert(family.value());
    }

    // Brief 04e M1: create the FULL useful queue set once, per the capability table — one queue
    // for graphics/present/transfer families, up to four for a dedicated compute family. Equal
    // priorities: relative queue priority is a scheduler hint we deliberately don't play with.
    static constexpr float queue_priorities[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        // clang-format off
        VkDeviceQueueCreateInfo queue_create_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queueFamily,
            .queueCount = desired_queue_count(family_caps_[queueFamily]),
            .pQueuePriorities = queue_priorities,
        };
        // clang-format on
        queueCreateInfos.push_back(queue_create_info);
    }

    // Only enable the features we want, and there are many:
    // nullDescriptor makes a VK_NULL_HANDLE descriptor legal: reads return zero, writes are
    // discarded. descriptor_table::unbind writes one into every released image slot so the set
    // never carries a view of a destroyed resource (see descriptor_allocator.cpp).
    // Diagnostic-only; chained ahead of robustness2 and harmless when the extension is absent.
    VkPhysicalDeviceFaultFeaturesEXT fault_features{};
    fault_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    fault_features.deviceFault = VK_TRUE;

    VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features{};
    robustness2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    robustness2_features.nullDescriptor = VK_TRUE;

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2_features{};
    extended_dynamic_state2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    extended_dynamic_state2_features.pNext = &robustness2_features;
    extended_dynamic_state2_features.extendedDynamicState2 = VK_TRUE;

    // Task + mesh shader stages (brief 03 meshlet pipeline). vkCmdDrawMeshTasksEXT + the
    // [shader("amplification")]/[shader("mesh")] Slang stages need both enabled.
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features{};
    mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    mesh_shader_features.pNext = &extended_dynamic_state2_features;
    mesh_shader_features.taskShader = VK_TRUE;
    mesh_shader_features.meshShader = VK_TRUE;
    // Brief 09b: the probe capture renders all 6 cube faces of a probe in ONE multiview render pass
    // with a mesh-shader pipeline — that combination needs multiviewMeshShader (separate from the
    // core Vulkan 1.1 `multiview` feature, which only covers vertex-pipeline multiview).
    mesh_shader_features.multiviewMeshShader = VK_TRUE;

    VkPhysicalDeviceVulkan11Features vulkan11_features{};
    vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11_features.pNext = &mesh_shader_features;
    // Slang lowers SV_VertexID/SV_InstanceID relative to gl_BaseVertex/gl_BaseInstance,
    // which declares the SPIR-V DrawParameters capability.
    vulkan11_features.shaderDrawParameters = VK_TRUE;
    // Brief 09b: VK_KHR_multiview (core in 1.1) — the probe capture renders all 6 cube faces in a
    // SINGLE render pass (viewMask=0x3F), the mesh shader selecting the per-face view-projection via
    // SV_ViewID. Removes the per-face BeginRendering + CPU draw loop that made capture laggy.
    vulkan11_features.multiview = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12_features{};
    vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12_features.pNext = &vulkan11_features;
    vulkan12_features.timelineSemaphore = VK_TRUE;
    vulkan12_features.bufferDeviceAddress = VK_TRUE;
    // GPU-driven draw generation (brief 03b): vkCmdDrawMeshTasksIndirectCountEXT reads its draw
    // count from a GPU buffer written by the draw-cull compute.
    vulkan12_features.drawIndirectCount = VK_TRUE;
    // Scalar block layout: lets the 3D vertex shader's buffer_reference Vertex struct pack to match
    // the tightly-packed C++ string::Vertex (vec3/vec2 mix) instead of std430's vec3->vec4 padding.
    vulkan12_features.scalarBlockLayout = VK_TRUE;
    vulkan12_features.descriptorBindingPartiallyBound = VK_TRUE;
    vulkan12_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
    // Bindless: unbounded descriptor arrays in shaders + (non-)uniform indexing into them.
    vulkan12_features.runtimeDescriptorArray = VK_TRUE;
    vulkan12_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vulkan12_features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    vulkan12_features.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;

    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.pNext = &vulkan12_features;
    vulkan13_features.synchronization2 = VK_TRUE;
    vulkan13_features.dynamicRendering = VK_TRUE;
    // GLSL `discard` compiles to OpDemoteToHelperInvocation under the vulkan1.3 target env
    // (used by the UI fragment shader), which requires this feature.
    vulkan13_features.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceFeatures enabled_device_features{};
    enabled_device_features.samplerAnisotropy = VK_TRUE;
    // GPU-driven draws: multiDrawIndirect issues many draws from one indirect buffer in a single
    // command; drawIndirectFirstInstance lets each indirect command carry a non-zero firstInstance
    // (used as the per-draw index the vertex shader reads via gl_InstanceIndex).
    enabled_device_features.multiDrawIndirect = VK_TRUE;
    enabled_device_features.drawIndirectFirstInstance = VK_TRUE;
    // Sampling BC7 (transcoded from cooked KTX2 textures) requires this feature enabled;
    // is_device_suitable() already rejects any device that doesn't support it.
    enabled_device_features.textureCompressionBC = VK_TRUE;
    // Brief 09b: the probe capture/relight/debug shaders access buffer_reference pointers (vertex,
    // meshlet, draw, probe offset/active buffers), which Slang lowers to 64-bit PhysicalStorageBuffer
    // addresses and declares the SPIR-V Int64 capability. Without this the validation layer flags
    // every probe vkCreateShaderModule (Int64 declared but shaderInt64 not enabled) and the behavior
    // is technically UB even where the driver tolerates it.
    enabled_device_features.shaderInt64 = VK_TRUE;

    // clang-format off
    VkPhysicalDeviceFeatures2 enabled_device_features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan13_features,
        .features = enabled_device_features
    };
    // clang-format on

    // Optional (profiler-only) extension: VK_EXT_calibrated_timestamps lets the Tracy GPU context
    // correlate host and device clocks. It is NOT in the hard-required device_extensions list — we
    // probe for it and append only if the driver offers it, so a device without it still creates.
    std::vector<const char*> enabled_extensions(device_extensions.begin(), device_extensions.end());
    {
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available(ext_count);
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, available.data());
        for (const auto& ext : available)
        {
            if (std::string(ext.extensionName) == VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
            {
                enabled_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
                calibrated_timestamps_enabled_ = true;
            }
            // VK_EXT_device_fault: on VK_ERROR_DEVICE_LOST this reports the FAULTING GPU ADDRESS and
            // access type, turning "the device died somewhere" into a specific address. Optional —
            // not every driver implements it — and diagnostic-only, so it never gates creation.
            if (std::string(ext.extensionName) == VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
            {
                enabled_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                device_fault_enabled_ = true;
            }
        }
    }

    // Chain the fault feature only once we know the extension is enabled — feeding a driver a
    // feature struct for an extension it does not have is invalid. Probed above, chained here.
    if (device_fault_enabled_)
    {
        robustness2_features.pNext = &fault_features;
    }

    // clang-format off
    VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_device_features2,
        .flags = 0,
        .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
        .pQueueCreateInfos = queueCreateInfos.data(),
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
        .pEnabledFeatures = nullptr // Data is passed through pNext instead
    };
    // clang-format on

    // NO device layers. VkDeviceCreateInfo::enabledLayerCount must be 0 (VUID-...-12384): device
    // layers have been non-functional since Vulkan 1.0 and are deprecated. The validation layer is
    // enabled at INSTANCE creation (driver.cpp), which is what actually takes effect; setting it
    // here only produced a validation error of its own on every startup.

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create logical device!");
    }

    volkLoadDevice(device_);

    // Hand the fault reporter the device, so a DEVICE_LOST anywhere can name the faulting address.
    // After volkLoadDevice: vkGetDeviceFaultInfoEXT is a device-level entry point.
    ::string::gpu::vk_set_fault_device(device_, device_fault_enabled_);
    STRING_LOG_INFO("GPU fault reporting (VK_EXT_device_fault): {}",
                    device_fault_enabled_ ? "available" : "NOT available on this driver");

    // The queue set now exists; expose it as named lanes (1:1, capability-derived).
    build_submission_lanes();
}

swap_chain_support_details device::get_swap_chain_support(const VkPhysicalDevice& physical_device)
{
    swap_chain_support_details details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface_, &details.capabilities);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &format_count, nullptr);

    if (format_count != 0) {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &format_count, details.formats.data());
    }

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &present_mode_count, nullptr);

    if (present_mode_count != 0) {
        details.presentModes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &present_mode_count, details.presentModes.data());
    }

    return details;
}

swap_chain_support_details device::get_swap_chain_support()
{
    return get_swap_chain_support(physical_device_);
}

queue_family_indices device::get_queue_families(const VkPhysicalDevice& physical_device)
{
    queue_family_indices indices;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queueFamilies.data());

    for (uint32_t i = 0; i < queue_family_count; i++)
    {
        const auto& queueFamily = queueFamilies[i];

        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphics_family = i;
        }
        if ((queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            indices.compute_family = i;
        }

        // Check if this is a dedicated transfer queue
        if ((queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            indices.transfer_family = i;
        }

        // Present support
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface_, &presentSupport);
        if (presentSupport) {
            indices.present_family = i;
        }
    }

    return indices;
}

queue_family_indices device::get_queue_families()
{
    return queue_family_indices_;
}

VkFormat device::get_format_support(const std::vector<VkFormat>& candidates, const VkImageTiling& tiling, const VkFormatFeatureFlags& features)
{
    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
            return format;
        else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
            return format;
    }

    throw std::runtime_error("Failed to find supported format!");
}

uint32_t device::get_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

VkPhysicalDeviceLimits device::get_physical_device_limits()
{
    return properties_.limits;
}

VkFormat device::get_depth_format() {
    // clang-format off
    return get_format_support(
        {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT
        },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    // clang-format on
}

VkSurfaceKHR device::get_surface() const
{
    return surface_;
}

VkDevice& device::get_device()
{
    return device_;
}

auto device::get_physical_device() -> VkPhysicalDevice&
{
    return physical_device_;
}

auto device::get_queue(queue_type type) -> queue
{
    const queue_family_indices& indices = queue_family_indices_;

    queue queue = {
        .queue_family_index = 0,
        .type = type,
        .queue = VK_NULL_HANDLE,
    };

    switch (type)
    {
        case queue_type::GRAPHICS:
        {
            if (indices.graphics_family.has_value())
            {
                STRING_LOG_DEBUG("Creating graphics queue.");
                queue.queue_family_index = indices.graphics_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return queue;
            }
            break;
        }
        case queue_type::COMPUTE:
        {
            if (indices.compute_family.has_value())
            {
                STRING_LOG_DEBUG("Creating compute queue.");
                queue.queue_family_index = indices.compute_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return queue;
            }
            break;
        }
        case queue_type::TRANSFER:
        {
            if (indices.transfer_family.has_value())
            {
                STRING_LOG_DEBUG("Creating transfer queue.");
                queue.queue_family_index = indices.transfer_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return queue;
            }
            break;
        }
        case queue_type::PRESENT:
        {
            if (indices.present_family.has_value())
            {
                STRING_LOG_DEBUG("Creating present queue.");
                queue.queue_family_index = indices.present_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return queue;
            }
            break;
        }
        default:
            throw std::runtime_error("Attempt to get queue type which is not supported!");   
    }

    throw std::runtime_error("Failed to get queue!");
}

#include <string.h>

bool device::are_validation_layer_supported()
{
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    for (const char* layer_name : validation_layers) {
        bool layer_found = false;

        for (const auto& layerProperties : available_layers) {
            if (strcmp(layer_name, layerProperties.layerName) == 0) {
                layer_found = true;
                break;
            }
        }

        if (!layer_found) {
            return false;
        }
    }

    return true;
}

}