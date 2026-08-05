#include <string/gpu/vk_check.hpp>

#include <string/core/logger.hpp>

#include <vector>

namespace string::gpu
{

const char* vk_result_name(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS:                     return "VK_SUCCESS";
        case VK_NOT_READY:                   return "VK_NOT_READY";
        case VK_TIMEOUT:                     return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:              return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:    return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:  return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:           return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:     return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_OUT_OF_DATE_KHR:       return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:      return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_UNKNOWN:               return "VK_ERROR_UNKNOWN";
        default:                             return "VkResult";
    }
}

namespace
{
VkDevice g_fault_device = VK_NULL_HANDLE;
bool g_has_device_fault = false;

const char* fault_access_name(VkDeviceFaultAddressTypeEXT t)
{
    switch (t)
    {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:            return "INVALID READ";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:           return "INVALID WRITE";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:         return "INVALID EXECUTE";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "IP unknown";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "IP invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:   return "IP fault";
        default:                                                       return "none";
    }
}

// Ask the driver what actually faulted. Two-call idiom: sizes first, then the arrays.
void log_device_fault()
{
    if (g_fault_device == VK_NULL_HANDLE || !g_has_device_fault)
    {
        STRING_LOG_CRITICAL("  (no VK_EXT_device_fault on this driver — cannot report the faulting "
                            "address; run with the Vulkan SDK's validation layers for more)");
        return;
    }

    VkDeviceFaultCountsEXT counts{ VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    if (vkGetDeviceFaultInfoEXT(g_fault_device, &counts, nullptr) != VK_SUCCESS) return;

    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{ VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    if (vkGetDeviceFaultInfoEXT(g_fault_device, &counts, &info) != VK_SUCCESS) return;

    STRING_LOG_CRITICAL("  fault: {}", info.description);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i)
    {
        const VkDeviceFaultAddressInfoEXT& a = addresses[i];
        // The precise address is reportedAddress +/- addressPrecision; the low bits are unreliable.
        STRING_LOG_CRITICAL("  {} at GPU address 0x{:x} (precision 0x{:x})",
                            fault_access_name(a.addressType),
                            static_cast<unsigned long long>(a.reportedAddress),
                            static_cast<unsigned long long>(a.addressPrecision));
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i)
    {
        STRING_LOG_CRITICAL("  vendor: {} (code 0x{:x}, data 0x{:x})", vendors[i].description,
                            static_cast<unsigned long long>(vendors[i].vendorFaultCode),
                            static_cast<unsigned long long>(vendors[i].vendorFaultData));
    }
}
}  // namespace

void vk_set_fault_device(VkDevice device, bool has_device_fault)
{
    g_fault_device = device;
    g_has_device_fault = has_device_fault;
}

bool vk_report(VkResult result, const char* what)
{
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        return true;
    }

    if (is_device_lost(result))
    {
        // CRITICAL so it survives a release build, and spelled out because whoever hits this is
        // usually looking at a black screen with no debugger. A device loss is the GPU faulting or
        // being reset out from under us (on Windows, the TDR watchdog after a ~2s hang); the driver
        // recovers but every Vulkan object we hold is dead, so there is no continuing from here.
        STRING_LOG_CRITICAL("GPU DEVICE LOST at {} ({}). The graphics driver reset or the GPU "
                            "faulted — all Vulkan objects are now invalid. On Windows check Event "
                            "Viewer > Windows Logs > System for Event ID 4101 (\"display driver "
                            "stopped responding and has recovered\") to confirm a driver reset.",
                            what, vk_result_name(result));
        log_device_fault();
        return false;
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return false;   // routine: the swapchain needs rebuilding, callers already handle it
    }

    STRING_LOG_ERROR("Vulkan call failed at {}: {} ({})", what, vk_result_name(result),
                     static_cast<int>(result));
    return false;
}

}  // namespace string::gpu
