# Oxylus Engine
![Logo](https://i.imgur.com/4JpO3vl.png)     
[![CI](https://img.shields.io/github/actions/workflow/status/Hatrickek/OxylusEngine/xmake.yaml?&style=for-the-badge&logo=cmake&logoColor=orange&labelColor=black)](https://github.com/Hatrickek/OxylusEngine/actions/workflows/xmake.yaml)
[![Discord](https://img.shields.io/discord/1364938544736370820?style=for-the-badge&logo=discord&logoColor=orange&label=Discord&link=https%3A%2F%2Fdiscord.gg%2FcbQDJrWszk)](https://discord.gg/cbQDJrWszk)
## About   
Oxylus is a simple yet powerful data-driven game engine built in C++ with a focus on developer productivity and performance. It is free and will be open-source forever!

Be aware that Oxylus is still in it's early stages of development. Some important features and documentation might be missing. Oxylus will have many API breaking changes both on C++ and Lua. Only use if you're okay with these.

## Design Goals
- **Powerful**: Full support for both 2D and 3D development
- **Intuitive**: Beginner-friendly yet endlessly adaptable for power users
- **Data-Driven**: Built on a "true" Entity Component System (ECS) for efficient data handling
- **Modular**: Use only the parts you need, swap out the rest
- **Fast**: Optimized for speed with parallel processing where possible
- **Code-First**: The editor is optional, build entire games programmatically or use the editor as a productivity aid

## Feature Highlights 
- Modular Vulkan renderer built using [vuk](https://github.com/martty/vuk) with modern rendering features:
	- Meshlet Rendering
		- Occlusion, frustum and triangle culling
		- Automatic LOD generation and selection
	- GI with [Brixelizer](https://gpuopen.com/fidelityfx-brixelizer/)
	- [GT-VBAO](https://cdrinmatane.github.io/posts/ssaovb-code/)
	- [SSSR](https://gpuopen.com/fidelityfx-sssr/)
	- [AMD FSR 3](https://gpuopen.com/fidelityfx-super-resolution-3/)
	- Virtual Directional, Spot and Point Light Shadows
	- Physically-based sky and atmosphere
	- Physically-based Bloom, Depth Of Field, HDR, Tonemapping, Auto Exposure, Chromatic Aberration, Film Grain, Vignette, Sharpen and various other post-processing effects.
	- 2D Rendering
		- Animated sprites
		- Tilemaps
  - Advanced Particle System
- Multithreaded physics with [Jolt](https://github.com/jrouwe/JoltPhysics).
- Extensive Lua scripting with [flecs](https://github.com/SanderMertens/flecs) events and systems.
  - Can write the whole game without ever touching C++, except app initalization and custom rendering.
- A featureful editor built with [Dear ImGui](https://github.com/ocornut/imgui) to aid the development process. 
- 3D Audio with [miniaudio](https://github.com/mackron/miniaudio)
- Networking with [enet](https://github.com/zpl-c/enet)

## Building
Windows, Linux and Mac (with MoltenVK) is supported.

### Requirements
- [Xmake](https://xmake.io)
- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home).
- A compiler that supports C++23.   
### Steps
- To configure the project run:
  - `xmake f --toolchain=clang --runtimes=c++_static -m debug`
	- Change `--toolchain=` for the toolchain you want to use. 
      - ex: `clang-cl` for Windows, `nix-clang` for nixos, `mac-clang` for macOS. Check `xmake/toolchains.lua` for details. 
	- Pick a mode `-m debug, release, dist`
	- Optionals:
      - `--lua_bindings` Compile lua bindings (`true` by default)
      - `--profile` Enable tracy profiler (`false` by default)
      - `--tests` Enable tests. (`false` by default)
- To build the project run:
	- `xmake build`
- To run the editor with xmake run:
  - `xmake r OxylusEditor`

