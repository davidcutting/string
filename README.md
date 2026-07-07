# String

*/striNG/*
*noun.*

a set of things tied or threaded together on a thin cord.

## Introduction

Simple Tiny RenderING engine (String) is my experimentation in this space. When evaluating alternatives, I found many existing game engines shoot to be incredibly general purpose. There are a vast number of features and robust tools. I'm not interested features such as custom scripting languages, visual block coding, monolithic editors, ect. The purpose of this project is not to replace existing general purpose game engines, but instead to be a simple sandbox that can be easily modified for more specific projects. Ultimately, one could easily pick up the engine to run simulations, experiments, games, whatever. I'm aiming for things to be loosely bound such that the engine can be extended, disabled, discarded, or whatever my project calls for.

My priorities follow:
1. Aggressive deprecation. Generally, not providing long term support for releases.
2. Lack of broad hardware support. I will not attempt to target every platform, or keep optimizations for hardware older than ~5 years.
3. Intentially small set of features. Your project, many of my projects, may have different requirements. My goal is to keep this code open source and available to modify fitting whatever unique requirements exist.
4. Clarity. Aiming to utilize modern techniques and keeping up with language features to best represent the intention of the code.
5. Speed. Of course in high performance applications such as this, but ideally balancing the trade-offs of optimization with the priors.

A disclaimer; I see this as an experiment and I'm not an expert so implementation may be naive. I am more than happy to review pull requests, but expect I may be picky.

## Platforms and Versions
| Operating System  | Library (version) | Supported | Stability    |
| :---------------- | :---------------: | :-------: | :----------: |
| Linux 64-bit      | Vulkan (1.3)      | Yes       | Experimental |
| Windows 64-bit    | Vulkan (1.3)      | Yes       | Experimental |
| Mac 64-bit        | Metal/MoltenVK    | No        | Non-existent |

## Dependencies

- meson
- VulkanSDK
- glm
- glfw3
- glslang
- spdlog

## Building

I suggest you go for your favorite Linux distro, because I'm not supporting Windows or Mac currently. You'll need to install the listed dependencies to your system including the VulkanSDK.

Afterward, set up the build with Meson using `meson setup build` then `meson compile -C build`

## License

This project is MIT licensed. Third party libraries may/do have their own licensing.

## Inspiration

- Vulkan Tutorial: A Vulkan API E-book
- Vulkan Guide: vkguide.dev
- Sascha Willems, Adam Sawicki, Khronos: Vulkan-Samples
- Austin Morlan: A simple Entity Component System (ECS) [C++]
- Yan Chernikov: Hazel Game Engine
- Godot Team: The Godot Game Engine

## Handy Resources

https://github.com/vinjn/awesome-vulkan

https://www.khronos.org/registry/vulkan/specs/1.3/html/vkspec.html#introduction

### Design

http://gameprogrammingpatterns.com/contents.html

https://alextardif.com/Bindless.html

https://alextardif.com/RenderingAbstractionLayers.html

https://alain.xyz/blog/comparison-of-modern-graphics-apis

http://www.gijskaerts.com/wordpress/?p=98

https://www.gamedeveloper.com/programming/designing-a-modern-cross-platform-low-level-graphics-library

### Other Engines

https://github.com/godotengine/godot

https://github.com/OGRECave/ogre-next

https://github.com/DiligentGraphics/DiligentCore

https://github.com/Themaister/Granite

https://github.com/kcloudy0717/Kaguya

https://github.com/TheCherno/Hazel

### OpenGL

https://learnopengl.com/

### Vulkan

https://vulkan-tutorial.com/Introduction

https://github.com/KhronosGroup/Vulkan-Samples

https://zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer/

https://github.com/blurrypiano/littleVulkanEngine

https://alain.xyz/blog/raw-vulkan

https://gpuopen.com/learn/understanding-vulkan-objects

### Render Graphs

https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in

https://developer.download.nvidia.com/assets/gameworks/downloads/regular/GDC17/DX12CaseStudies_GDC2017_FINAL.pdf

https://www.khronos.org/assets/uploads/developers/library/2019-reboot-develop-blue/SEED-EA_Rapid-Innovation-Using-Modern-Graphics_Apr19.pdf

https://github.com/azhirnov/FrameGraph

https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/

### D3D

https://docs.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide

https://alain.xyz/blog/raw-directx12

### PBR and RayTracing

https://www.realtimerendering.com/raytracinggems/

https://www.pbr-book.org/3ed-2018/contents

https://github.com/RayTracing/raytracing.github.io

### Memory

https://floooh.github.io/2018/06/17/handles-vs-pointers.html

### ECS

https://github.com/skypjack/entt

