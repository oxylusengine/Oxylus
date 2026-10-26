# Render tests

These suites cover the engine side of the renderer. The render graph itself is the separate `vir` package
([oxylusengine/vir](https://github.com/oxylusengine/vir)), and its coverage lives in that repo's `tests/`. Most suites here need no Vulkan device, pipeline creation
and execution checks use stub Vulkan function pointers.

`TestHiZGPU` uses a headless Vulkan device and compares every pyramid mip with a CPU reference for
the original SPD's nearest sampling and minimum reduction, including 1622x717. It executes
the same Program twice with different source depths. Set `OX_HIZ_GPU_VALIDATION=1` to enable Vulkan
synchronization validation. It skips when a suitable Vulkan device is unavailable. On Linux, the test
keeps loaded driver modules mapped so their process globals remain visible to LeakSanitizer after
Vulkan teardown.

Nothing here parses SPIR-V. ResourceCompiler reads each entry point's layout off
Slang's reflection and stores it in the shader pack. `tests/ResourceCompiler/TestShaderLayout.cpp` checks
that reflection on small sources, and `TestEngineShaders` runs every built engine and editor pipeline
through the checks a declaration performs.

## Run

Enable `--tests=y` in the project's existing Xmake configuration, then run:

```sh
xmake build -j 32 TestRenderContext TestHiZGPU TestEngineShaders TestImageTypes TestShaderLayout TestPendingPick
xmake test 'TestRenderContext/*' 'TestHiZGPU/*' 'TestEngineShaders/*' 'TestImageTypes/*' 'TestShaderLayout/*' 'TestPendingPick/*'
xmake build -j 32 -a
```

Test targets enable ASan, UBSan and, on Linux, LeakSanitizer. Xmake supplies
`ASAN_OPTIONS=detect_container_overflow=0` because the linked engine library is not instrumented.
If process inspection is restricted, run the tests outside that restriction so LeakSanitizer can finish.

## Renderer context

`RenderContext` owns one graphics queue, one vir graph, persistent resource slot maps, a raw bindless
set and timeline-retired frame allocators. Its public header uses forward declarations; graph recording
and execution headers are included at the call sites that need their definitions.

Use `RenderContext::compile` on a Module with exported resources. It roots every `export_to`/`present`
instruction and also retains values named with `Module::keep`. Execute the resulting Program repeatedly,
updating its variables before each execution. `execute_blocking(Module&&)` uses a separate allocator
and waits on the returned ticket outside the queue lock. Following submissions wait outside that lock for
acceleration-structure query writers to retire, avoiding the queue/command-buffer lock cycle in GPU-assisted
validation. Ordinary submissions remain asynchronous. Upload jobs can instead supply their own
frame allocator to `execute(program, allocator)` and poll `is_retired(ticket)`.

Destroying a persistent resource removes it from lookups immediately. Its slot and bindless index stay
reserved until the final frame submission ticket sealed by `seal_retirements` completes; `collect_retired` then destroys
the Vulkan object and releases the slot. Callers must stop submitting programs that reference a resource
before requesting its destruction. Swapchain programs carry a generation and are rejected after resize.
Modules recorded against an old swapchain are also rejected if compilation happens after resize.

Scene passes record directly into a `vir::Module` once per `FrameShape`. Each cached `vir::Program`
keeps its variable IDs in `PreparedFrame`. Passes take the Module explicitly plus plain `FrameRecording`
data (shape, prepared variables and current graph values), and declare variables beside their uses; `bind_frame` and the pass groups' bind functions refresh
persistent resources, push constants and counts before each execution. Callbacks declared with a body
read current data when executed. Injected stages store their own variables and have an explicit bind
hook. The editor records its swapchain Program once per generation and samples the persistent targets
submitted by panels. Set `OX_DUMP_IR=1` to log newly compiled scene Programs.

`TestPendingPick` checks submission-ticket completion, prevention of readback reuse, and cancellation
of requests whose viewport submission fails. Empty 2D and 3D scenes still record the picking stages
against cleared visibility attachments. Highlight selections are rebuilt from the current scene.
`TestEngineShaders` requires both built shader packs and checks descriptor sets against the frame
binding convention, including the bindless set at index 1 and the VSM resolve resources at index 2.

`TestRenderContext` covers exported uploads, program reuse, frame retirement, wait locking, descriptor
failure cleanup, bindless bounds and delayed slot reuse, image view swizzles, swapchain generations,
shutdown with holes in the slot maps, named pipeline declarations and the raw Vulkan utility types.
It also records the original HiZ pass against its built shader layout, checking its single dispatch,
counter reset and write-to-sampled barrier. Explicit binding refreshes buffers while recorded callbacks
read current data without recording again. Setting a variable removed by compilation is a no-op;
staging allocations must be guarded with `has_variable`.

Production-pass tests record light-grid and post-processing feature combinations, early/late geometry
with both draw backends, early/late terrain, repeated point/spot mips, and multiple DDGI debug volumes. They check live
variable kinds and sizes, shared versus distinct IDs, absent feature inputs, and repeated byte updates
without changing program instructions or reallocating the variable's byte storage. These use the built
shader layouts and stub Vulkan device; they do not establish whole-scene visual parity.

For a focused light-grid microbenchmark, set `OX_FRONTEND_BENCH=1` when running `TestRenderContext`.
It reports cold record+compile time and warm byte-binding time separately. `OX_FRONTEND_DUMP=<directory>`
writes the light-grid and post-processing fixture IR into an existing directory for before/after
comparison. Sanitized debug timings are diagnostic, not release frame-time measurements.

Whole-image subresource ranges use `vir::REMAINING`, so never treat their mip or layer counts as
dispatch dimensions. Use the frame's known texture dimensions and fixed array sizes instead.
