# Notes

## Architecture

I am creating a GPU-driven Vulkan 1.3 abstraction layer.

The Instance is effectively the Vulkan context, it is used to select a physical device and create a Device, which is the primary object for Vulkan abstraction. A Device is used by all other components, as it holds the logical device and performs most of the major Vulkan functions.

The swapchain integrates VRR, handles swapchain recreation, and fully encapsulates the presentation Vulkan presentation logic.

Swapchain contains its semaphores for timing:
 - Acquire semaphores, one for each frame in flight,
 - Present semaphores, one per swapchain image,
 - Timeline semaphore, used to limit frames in flight.

The correct set of semaphores can be queried with member functions, as they are changed after every acquire call.  

At the highest level, the application interacts with a Scene which is composed of primitives. The scene is used to compose a TaskGraph, which handles various resource dependencies between Tasks. Everything is a Task, even the swapchain presentation is a Task. The TaskGraph is compiled to command buffers using a CommandRecorder.

The TaskGraph should have its Tasks declared up-front, so that it can determine the optimal run order. The recording and execution of the TaskGraph are separate steps. So one would create the TaskGraph, then compile it. It would effectively bake the required Pipeline, Resources, and Shaders. After, it could then be executed.

With TaskGraph, you can create task resource handles and names for the resources you have in your program. You can then list a series of tasks. Each task contains a list of used resources and a callback to the operations the task should perform.

A core idea of TaskGraph is that you record a high-level description of a series of operations and execute these operations later. In TaskGraph, you record tasks, compile, and later execute. The callbacks in each task are called during execution.

This “two-phase” design allows TaskGraph to optimize the operations by determining optimal synchronization automatically based on the declared resource used in each Task. In addition, TaskGraph is reusable. You can record your main render loop as a task graph and let the task graph optimize the tasks only once and then reuse the optimized execution plan every frame. All in all, this allows for automatically optimized, low CPU cost synchronization generation.

Overview of TaskGraph workflow:
 - Create tasks
 - Create resources
 - Add resources to tasks
 - Add tasks to graph
 - Complete task graph
 - Execute task graph

Task resources, namely TaskBuffer and TaskImage are assigned a bindless index on creation, which is valid for it's entire lifetime.

Additionally, I have the concept of a window and a platform. The window is a WSI abstraction that handles keyboard, mouse, and window events. The window is ultimately part of the platform and is meant to be an abstraction point to allow for support of both Windows and Linux common OS functionalities.

## Allocator



## Other stuff

- Integrations
  - volk
  - VMA
  - glTF
  - ImGUI
    - ImGuizmo
    - ImPlot

- Core
  - C++ 23 Logging Implementation
    - Use logger as abstraction to swap which logging implementation is used?
  - EventService
    - Some event queue that can process events on another thread? Add queue to main loop?

## Vulkan Discord

### McAdam - Descriptor Set Management... or not?

## Online Sources

### Writing an efficient Vulkan renderer
### Guthmann - Integrating Dear ImGui in a custom Vulkan renderer
### Vulkan Guide
### VkGuide
### Vulkan Spec
