# The `recomp` branch

This branch is the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) at its **nightly of 2026-10-02**
(`nightly-20261002-bd833a2a`, the `development` branch at `bd833a2`: v0.10.0 and 24 commits) with the changes that
Gustavo Garcia's recompilation projects need, one commit each. The games use it as their
`thirdparty/rexglue-sdk` submodule, pinned to a commit of this branch:

- KameoRecomp (Kameo: Elements of Power)
- GRAWRecomp (Ghost Recon Advanced Warfighter)
- POTCAWERecompVulkan (Pirates of the Caribbean: At World's End)
- GHMetalRecomp (Guitar Hero: Metallica)

and their renderer, [NativeXRenderer](https://github.com/ElGuslml/NativeXRenderer), which hooks a game's Direct3D
calls and draws on the host instead of emulating the Xbox 360 GPU.

Until 2026-10-21 these changes were `git format-patch` series that every game applied at configure time: 25 shared
ones in each game's `patches/` and 8 in NativeXRenderer's `sdk_patches/`. They are the commits of this branch now,
in the same order, with the same messages. Each commit message explains its change.

On 2026-10-21 the branch moved from v0.10.0 (`c94f5eb`; its last commit there is tagged `recomp-v0.10.0`) to the
nightly. Of the upstream commits, those that touch these changes:
- the window code: fullscreen is borderless at the desktop mode, and `fullscreen_exclusive` switches the display
  mode instead. Change 5 is gone: a game sets `fullscreen_exclusive = !borderless`;
- the input code: dead zones, vibration, UI input blockers with a read for the UI (`GetStateForUI`), the buttons
  held when a dialog closes masked until released. Change 34 is built on them;
- the code generator (`stwcx.` as `std::atomic` compare-and-swap, fences for `sync`/`lwsync`/`eieio`, unsigned
  register types): every game regenerates its code;
- the log: no size rotation (no `name.1.log` files), a directory budget instead.

## General fixes and options

None of them is specific to one game; the options they add are off by default.

| # | Change |
|---|---|
| 1 | Keep host FP exceptions masked when the game updates the FPSCR |
| 2 | Invalidate GPU copies of newly allocated physical memory |
| 3 | Code generator: `vpkd3d128` lost the sign when the destination aliases the source |
| 4 | `gpu_depth_clamp`: clamp depth instead of clipping in Z, like Xenos (D3D12 and Vulkan) |
| 5 | Removed on the nightly: `fullscreen_borderless_window` (upstream's fullscreen is borderless; `fullscreen_exclusive` switches the mode) |
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
| 36 | `gamepad_remap`: a controller's buttons and triggers moved to other Xbox 360 controls, per SDL gamepad type (`ps5:a=east,b=south`; `*` for any), applied by the SDL driver; empty keeps the standard layout. The games' launchers edit it |
| 37 | Dear ImGui's textures made on request (`ImGuiBackendFlags_RendererHasTextures`): fonts are rasterized at any size they are drawn at, and a renderer that draws the dialogs with its own immediate drawer gets the font atlas made with it. Barlow (SIL OFL 1.1, `resources/fonts`) embedded for the SDK's dialogs, a common dialog style (`DialogStyle`, scaled with the screen's height) and language (`SetOverlayLanguage`), and the quit prompt drawn in them |
| 38 | Achievements: the toast is a card at the top center (the icon, "ACHIEVEMENT UNLOCKED" in the dialogs' language, the name and description, the gamerscore, a bar for the time left) and the F7 list a panel with the totals, the progress and each achievement's unlock date, in the dialogs' style; texts in the dialogs' language from the title's XDBF; icons made with the ImGuiDrawer's current immediate drawer (a renderer that draws the dialogs itself shows them); the F7 list no longer asks for continuous painting. A cache for launchers next to the unlocks (`<TITLEID>.info.toml` with every language, the icons as PNG), and `achievements_cache_only`, which writes it and quits |
| 39 | `audio_callback_min_interval_us`: a client's render callback is called at least that many microseconds after its last call returned, instead of twice in a row when the host device takes two frames at once (WASAPI's 10 ms period); the console calls it every 5333 us. Games that poll the render position miss the frames of a burst (Guitar Hero: Metallica's FMOD mixed one block in a dozen and played noise); 0 keeps the calls as soon as a frame is free |

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

## For the games

Features a game turns on from its own code (a `[[midasm_hook]]` that calls them). They do nothing until it does.

| # | Change |
|---|---|
| 34 | A quit prompt the game offers on its title screen (`rex/ui/overlay/quit_prompt.h`): B asks whether to quit, in the game's language, A quits and B cancels, and the game does not see those presses (`InputSystem::SetHostInputFilter`, upstream's UI input blockers); `quit_prompt` turns it off |
| 35 | The quit prompt can also be offered with a condition the game registers once, asked when a press of B begins (`SetQuitPromptCondition`), for a game whose screens change on events rather than in code that runs every frame; and its language set by the game (`SetQuitPromptLanguage`), for a game that asks for its language in its own menu |

## Working on this branch

- **A change**: commit it on `recomp` with a message that explains it (author
  `Gustavo Garcia <44309076+ElGuslml@users.noreply.github.com>`), add it to the tables above, push, then move each
  game's `thirdparty/rexglue-sdk` submodule to the new commit and test it there.
- **Debug instrumentation** never goes on this branch: keep it on a local branch on top of it.
- **A new upstream release or nightly**: tag the branch's last commit (as `recomp-v0.10.0`), so that the games'
  older commits keep their submodule, rebase `recomp` onto the new upstream commit and resolve the conflicts once,
  for every game, then force-push it.
- **Patch files** for someone who wants them on another checkout of the SDK:
  `git format-patch bd833a2..recomp`.
