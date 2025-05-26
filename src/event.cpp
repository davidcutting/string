#include <vulkan/vulkan_core.h>
#include <string/core/event.hpp>

namespace String {

void EventManager::update()
{
    dispatcher_.update();
}

}  // namespace String
