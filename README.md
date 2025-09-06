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
