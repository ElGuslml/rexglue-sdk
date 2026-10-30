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
| 40 | XMA: a packet's skip count carries into the next input buffer. In a multichannel file each stream's packets are interleaved with the others' and the skip says how many to pass; when it runs past the end of the buffer, the stream goes on that many packets into the next one instead of at its first packet, which belongs to another stream when several contexts share the buffers (Guitar Hero: Metallica's songs: only the first stem of each file played after the first block). Packets where no frame starts are also passed by their skip |
| 41 | `gamepad_subtype`: the XINPUT_DEVSUBTYPE reported for a plain gamepad and the keyboard's emulated one (6 guitar, 7 alternate guitar, 8 drum kit, 11 bass guitar), so that a music game gives it an instrument's part with the same XInput controls (frets on A, B, Y, X and LB, strumming on the D-pad); a device of another kind keeps its own; 0 keeps the gamepad |
| 42 | Code generator: `non_volatile_vectors_as_local` (true by default). With `non_volatile_as_local`, false keeps the non-volatile vector registers (v14-v31, v64-v127) in the context, and their `__savevmx_N` / `__restvmx_N` helpers save and restore them there, for code whose compiler passes values to its callees in them outside the calling convention (Guitar Hero: Metallica's sets v92 and calls a function whose callees read it: as local variables it was unset there). The other registers stay local variables |
| 43 | `signin_ui_cancel`: `XamShowSigninUI` acts as the system's sign-in UI closed without a sign-in (`XN_SYS_UI` on, then off), as when the player cancels it on the console, instead of reporting a sign-in change of user 0 and the UI off. Only user 0 has a profile, so a game that asks another controller to sign in waited for that sign-in forever (Guitar Hero: Metallica, Start pressed on a second controller at the title: an empty screen; now its "not signed in" message, with "continue without saving"); false keeps the old notifications |
| 44 | `install_content`: the executable installs an Xbox 360 content package (an STFS file: LIVE, PIRS or CON) as its title's downloadable content and quits without starting the game, as `achievements_cache_only` does (no window, the executable loaded in tool mode for its title ID): `ContentManager::InstallContent` extracts it into the user data's `0000000000000000/<title ID>/00000002/<package's file name>/` and writes its header last. The package's own title ID is not checked (Guitar Hero: Metallica reads Guitar Hero III's "Death Magnetic"). For launchers |
| 45 | `mnk_tap_target`: the keyboard's pad also presses that control, briefly (`mnk_tap_hold_ms`, 30), when one of `mnk_tap_sources` is pressed, the sources pressed within `mnk_tap_window_ms` (20) of the first making one press, which carries them all and comes when the window ends; the target is then released at least 20 ms, so that a game reading the pad once a frame sees each press. `SetTapGate` lets the game allow it only in some states. A music game's strum on each fret (Guitar Hero: Metallica: the frets A, B, Y, X, LB and the D-pad, only in a song, as its menus move with the D-pad); empty keeps the pad as it is |
| 46 | SDL input: a guitar's whammy bar (the right stick's X axis, -32768 at rest) is at rest until the guitar reports a position other than 0. An Xbox 360 guitar reads 0 there, half pressed, until the bar first moves, and SDL sends no motion while the value does not change, so a game saw the bar half pressed until the player moved it (Guitar Hero: Metallica, whose bar rests at -0.76 of the axis: the whammy at 43 % on every sustain). Guitars only (SDL_JOYSTICK_TYPE_GUITAR); the log names the first position reported |
| 47 | `profile_root`, `profile_name`: a launcher's profiles. With `profile_root`, the signed-in profile's content (that of a XUID other than 0: saves, and their headers), its settings in each game (the title-specific profile settings, in `<profile_root>/<title>/profile/`, no longer under the profile's name, so a rename keeps them) and its achievement unlocks (`<profile_root>/achievements/<title>.toml`) go to that folder; downloadable content (XUID 0), the caches and the achievements' list stay in the user data folder. `profile_name` is the name the game gets (XamUserGetName, the sign-in info, the gamertag): ASCII as an Xbox 360 gamertag, accents folded (U+00C0 to U+017F), at most 15 characters. Every profile keeps the XUID 0xB13EBABEBABEBABE, so a save bound to its owner loads in any profile. Both empty (the default) keep everything as it was |
| 48 | `guitar_whammy_rest` (false by default): change 46's whammy bar at rest only where a game asks for it. In a game that does not play guitars, a guitar left connected is one more controller, and its bar at rest is its right stick held left: Ghost Recon Advanced Warfighter's camera turned on its own from the start (before change 46, from the bar's first move). Guitar Hero: Metallica sets it |
| 49 | Paths in UTF-8 and UTF-16, not in the ANSI code page: `profile_root` is UTF-8 (`rex::to_path`; `std::filesystem::path` from a `std::string` read it as ANSI, so a profile under a Windows user's name with letters outside it went to a wrong folder), and the achievements' files are read through their wide path and logged in UTF-8 (`toml::parse_file(path.string())` and `path.string()` in the logs failed or threw for such a folder). Checked with a profile folder named in Cyrillic and with an Ñ |
| 50 | Several controllers, one profile, as on the console. `signin_guests` (false by default): users 1-3 with a controller are signed in as guests of the profile (XUSER_SIGNIN_INFO's guest flag, guest number and sponsor 0, "Name (1)", a XUID of their own), read their settings unset (the title's defaults), write them without keeping them, and cannot create or delete content (every user's content is the profile's here). `InputSystem::SetPrimaryPressWindow` (`rex::input::SetPrimaryPressWindow`), which a game opens on its title screen: a controller that presses a button there becomes user 0, the profile's, and user 0's controller takes its user; the game sees the press on user 0. Guitar Hero: Metallica turns both on |
| 51 | `guitar_subtype` (0 by default): the XINPUT_DEVSUBTYPE reported for a guitar, as `gamepad_subtype` is for a gamepad. SDL gives every guitar one joystick type (SDL_JOYSTICK_TYPE_GUITAR, for XInput's subtypes 6, 7 and 11 alike), so the SDL driver reported them all as XINPUT_DEVSUBTYPE_GUITAR (6), Rock Band's; Guitar Hero's guitars report XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE (7) on the console. Guitar Hero III's engine (Guitar Hero: Aerosmith) takes only 7 as a guitar (IsGuitarController: its device type 8) and calls a 6 an unsupported peripheral, asking for a Guitar Hero guitar or a controller before its title screen. Guitar Hero: Aerosmith sets 7 (and `gamepad_subtype` 7). A gamepad changed by `gamepad_subtype` is not changed again |
| 52 | The achievement cache for launchers (change 38) also writes the title's own icon, the XDBF's image 0x8000 (the one the console's dashboard shows), as `<TITLEID>/title.png` beside the achievements' icons, as it is in the XDBF. For a launcher's picture of the game where the disc has no `nxeart` package with it (Kameo, GRAW, POTCAWE) |
| 53 | XMA: an output buffer whose read and write offsets meet is full only if the decoder wrote into it. `XmaContext::Work` cleared `output_buffer_valid` whenever they met, also when it wrote nothing because the input had run out after the guest drained the buffer; the guest then reads it as full. FMOD Ex 4.06 (Guitar Hero: Aerosmith) calls a sound's data finished when both input buffers are empty and the output buffer is empty and valid; it took the last frame again at each mix up to the sound's declared length instead, so each loop of a streamed sound (the crowd's ambience, 8-9 s) ended in about 85 ms of one frame repeated, heard as a stutter |
| 54 | XMA1 packets: a frame whose 15-bit length straddles the packet's end is decoded. `GetPacketInfo` counted a packet's frames only while 15 bits were left, so such a frame was not counted (XMA2 packets give the count in their header, which covered it): the frame before it was taken for the packet's last and the decoder went on at the next packet's first frame, dropping it. About one frame in 800, at different places in each stream: Guitar Hero: Aerosmith's songs (FMOD Ex 4.06, a stream per stem) drifted apart, Make It's band stem 150 ms ahead of its guitar at the end, until a pause set them together again. Nothing changes for XMA2 packets |
| 55 | USB microphones: `usb_microphone_1` and `usb_microphone_2` (empty by default: nothing runs) give the game's two USB microphones a recording device of the host each (a device's name as the system lists it, or `default`), `usb_microphone_*_channel` the channel of it the game hears (0 = all of them mixed). The XDK's USB microphone library (the USB Audio Class microphones of Guitar Hero and Rock Band) finds a microphone with `XamUserGetDeviceContext(index, 4)` and drives it with `MicDeviceRequest`, which were a stub: the state (code 0), open (1), the volume control's transfers (2), the sample rate (4), reads of 1 ms packets (7) and the rates (8), each ending in the request's completion routine, called from a worker thread (`rex/audio/usb_microphone.h`, `AudioSystem::usb_microphones`). A read ends when SDL has recorded its packets, 16-bit little-endian at 48 kHz; what the game did not ask for in time is dropped down to two reads' worth. The volume control is fixed (its range is 0 to 0 dB): Guitar Hero turns its microphone down to the bottom of its range at a song's start, as its Logitech microphone needs, and how far a PC microphone should go down depends on the microphone, so `usb_microphone_*_gain` (dB, 0 by default) is the player's. A device that goes away ends its reads with an error and is a new device to the game when it comes back. Guitar Hero: Metallica |
| 56 | `gamepad_device_subtypes` (empty by default): the XINPUT_DEVSUBTYPE reported for particular gamepads, in place of `gamepad_subtype`: `controller=subtype` entries, a controller named by its serial number (SDL's HIDAPI controllers, a DualSense's) or else its SDL GUID, `keyboard` for the keyboard's emulated pad; 1 is a gamepad. A device of another kind (a real guitar) keeps its own. The SDL driver's `DeviceInfo` has the controller's serial, and the log's OnControllerDeviceAdded lines give it. Guitar Hero: Metallica's launcher writes it: a music game gives a plain gamepad the vocals (its singer, with a microphone, change 55) while `gamepad_subtype` makes the other gamepads guitars |
| 57 | A gamepad, or the keyboard's pad, that the game is told is a guitar (`gamepad_subtype`, `gamepad_device_subtypes`) plays as one. `gamepad_tap_target` and `gamepad_tap_sources` (empty by default) are change 45's strum on each fret for gamepads, with the keyboard's timing (`mnk_tap_window_ms`, `mnk_tap_hold_ms`) and its game's gate (`SetTapGate`); the logic is one class for both (`rex/input/tap_strum.h`), one state per gamepad. `gamepad_whammy_rest` (false by default): its right stick's X axis is the whammy bar, at rest (-32768) while the stick is centred and fully pressed at either side, past a tenth of its travel; a stick's value was the bar held half way (Guitar Hero: Metallica's bar rests at -0.76 of the axis: a centred stick is 43 % pressed). The user's whammy bars, a real guitar's too, are merged by the one pressed furthest (the merge's larger magnitude would take a bar at rest), and which controller is in the player's hands is judged on the stick's own value. Guitar Hero: Metallica sets the sources and the whammy; its launcher writes the target |
| 58 | Input read on several threads at once. `InputSystem` enumerates the devices again on every read (`RefreshDevices` clears its device list and fills it again), and nothing guarded that list (upstream's since `26d657d`): two reads at once freed the same `DeviceInfo` strings twice. Guitar Hero: Metallica closed now and then with a heap corruption (0xc0000374 in `RtlFreeHeap`, under `RefreshDevices`, from `XamInputGetState` on its main thread), as another of its threads asks who is signed in, which with `signin_guests` looks for a guest's controller (`UserHasDevice`); the quit prompt reads the controllers on the UI thread, a vibration may come from any. GRAW's dumps show the same failure (a free from the runtime finding the heap corrupt). A recursive mutex guards the device list, the assignment and the users' state, taken before the drivers' own locks; the host input filter runs outside it. A unit test reads from five threads at once: it crashed in every run before |
| 59 | `gamepad_device_remap` (empty by default): change 36's `gamepad_remap` for particular controllers, applied after the sections of their type: sections `controller:control=input,...` joined by `;`, a controller named as in `gamepad_device_subtypes` (its serial number, else its SDL GUID; the name is cut at the section's last `:`, as a serial number may hold one). One layout per type of controller made an Xbox 360 guitar's and an Xbox 360 controller's the same (both are SDL's `xbox360`): swapping a guitar's yellow and blue frets swapped the controller's Y and X. The log's `gamepad_remap for` line includes the controller's own section. A launcher writes it |
| 60 | Whammy bars that rest elsewhere. `gamepad_whammy_calibration` (empty by default): a launcher's calibration of a controller's bar, `controller=axis:rest:full` entries (a controller named as in `gamepad_device_remap`; axis `lx ly rx ry lt rt`; the axis's SDL values at rest and all the way): the right stick's X is then that axis, from -32768 at rest to 32767. And with `gamepad_whammy_rest` (change 57), a gamepad's right stick's X found resting at -32768 (still for a second, the first time; `DeviceInfo::whammy_bar`) is a bar already and is not folded as a stick: a guitar that reports itself as a gamepad (a CRKD guitar in its Xbox 360 mode may) had its bar at rest read as the stick pushed all the way, the whammy always pressed. The log says what the axis was found to be. A unit test plays a gamepad as a guitar both ways |

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
