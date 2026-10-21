# The `recomp` branch

This branch is the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) at **v0.10.0** (`c94f5eb`) with the
changes that Gustavo Garcia's recompilation projects need, one commit each. The games use it as their
`thirdparty/rexglue-sdk` submodule, pinned to a commit of this branch:

- KameoRecomp (Kameo: Elements of Power)
- GRAWRecomp (Ghost Recon Advanced Warfighter)
- POTCAWERecompVulkan (Pirates of the Caribbean: At World's End)

and their renderer, [NativeXRenderer](https://github.com/ElGuslml/NativeXRenderer), which hooks a game's Direct3D
calls and draws on the host instead of emulating the Xbox 360 GPU.

Until 2026-10-21 these changes were `git format-patch` series that every game applied at configure time: 25 shared
ones in each game's `patches/` and 8 in NativeXRenderer's `sdk_patches/`. They are the commits of this branch now,
in the same order, with the same messages. Each commit message explains its change.

## General fixes and options

None of them is specific to one game; the options they add are off by default.

| # | Change |
|---|---|
| 1 | Keep host FP exceptions masked when the game updates the FPSCR |
| 2 | Invalidate GPU copies of newly allocated physical memory |
| 3 | Code generator: `vpkd3d128` lost the sign when the destination aliases the source |
| 4 | `gpu_depth_clamp`: clamp depth instead of clipping in Z, like Xenos (D3D12 and Vulkan) |
| 5 | `fullscreen_borderless_window` option |
| 6 | `gpu_backend` option to choose D3D12 or Vulkan |
| 7 | `XGetLanguage` honors `user_language` |
| 8 | No busy waiting when there is no work (timer queue, presenter repaints) |
| 9 | `gpu_async_pipelines_no_frame_wait`: do not stall the frame on new pipelines |
| 10–13 | Faster register and shader-constant writes from the command processor |
| 14 | Log the guest context on unhandled guest access violations |
| 15 | `gpu_guest_fps_limit`: optional frame rate cap for the game |
| 16 | `XamUserReadProfileSettings` no longer writes past the game's buffer (title-specific settings larger than their nominal size) |
| 17 | `render_target_skip_transfers_on_overwrite`: no EDRAM ownership transfer into a render target that a single triangle, rectangle or quad fully overwrites |
| 18 | `render_target_draw_clears`: a draw that writes constant values to a rectangle of its render targets is done as a clear of the render targets owning that EDRAM, never through the alias |
| 19 | `d3d12_lazy_resolve`: color resolves kept as host render target copies and textures filled from them by copying, the shared memory written only on demand |
| 20 | Occlusion queries: the end pairs with the begin whatever its address and in every EDRAM tile, so host queries are no longer turned off at the first query (D3D12 and Vulkan) |
| 21 | SPIR-V: cube maps sampled at a LOD computed in the shader, not with gradients (NVIDIA's Vulkan driver returned garbage in one pixel of each quad), and the LOD bias scale kept finite |
| 22 | Code generator: with `non_volatile_as_local` the elided `__savegprlr_N` still stores the return address in the frame's LR slot, which the guest's stack walks read |
| 23 | `WAIT_REG_MEM` polls every 100 µs with the high-resolution timer instead of sleeping 1 ms |
| 24 | CMake: rexruntime and the GPU plugins are copied next to the executable on every build, not only when it links again |
| 25 | Code generator: a `share_registers` function (an SEH funclet, a setjmp caller) keeps its registers across calls when the others are local variables |

## For NativeXRenderer

Interfaces NativeXRenderer plugs into. They change nothing while it is off. The numbers in parentheses are its
decisions (`native_x_renderer/docs/decisions.md`).

| # | Change |
|---|---|
| 26 | An optional draw observer in the command processor (the library's observer compares its rebuilt state with the command processor's registers) |
| 27 | A switch to turn host rendering off: the command processor keeps the guest's GPU in sync without drawing |
| 28 | A sync-only consumer can answer occlusion queries (D32) |
| 29 | A renderer that presents itself can read the guest's gamma ramp (D36) |
| 30 | The SPIR-V translator can revert a fractional resolution scale |
| 31 | A renderer on its own thread can hold the command processor's signals to the game (D71) |
| 32 | `SharedMemory::RequestRanges` keeps its merged ranges between calls (D84) |
| 33 | The guest's swaps are counted, and a renderer is told when one executes (D86) |

## Working on this branch

- **A change**: commit it on `recomp` with a message that explains it (author
  `Gustavo Garcia <44309076+ElGuslml@users.noreply.github.com>`), add it to the tables above, push, then move each
  game's `thirdparty/rexglue-sdk` submodule to the new commit and test it there.
- **Debug instrumentation** never goes on this branch: keep it on a local branch on top of it.
- **A new upstream release**: rebase `recomp` onto the new tag and resolve the conflicts once, for every game.
- **Patch files** for someone who wants them on another checkout of the SDK:
  `git format-patch c94f5eb..recomp`.
