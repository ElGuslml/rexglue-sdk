# GameInput (API 3 header)

`include/GameInput.h` is the public header of Microsoft's GameInput, from the NuGet package
[Microsoft.GameInput](https://www.nuget.org/packages/Microsoft.GameInput) 3.5.283, unmodified (the repository stores it
with LF line endings). It is under the MIT License (its own notice, at the top of the file; the package's header and
static library are MIT since version 3.5).

Why it is here: SDL reads the Xbox One instruments that are neither XInput nor HID devices (GIP guitars and drum kits
such as the PDP Riffmaster, the PDP Jaguar or the Mad Catz Stratocaster) through GameInput's raw device reports, and
only when it is built against GameInput's API 3 (`GAMEINPUT_API_VERSION >= 3`, SDL_HINT_JOYSTICK_GAMEINPUT_RAW). The
Windows SDK's own `GameInput.h` is API 0. `thirdparty/CMakeLists.txt` puts this directory first on SDL's include path,
and the SDL input driver reads those guitars' tilt with it (`src/input/sdl/gameinput_instruments.cpp`).

SDL 3.4 compiles against API 3 only (`using namespace GameInput::v3`): the package's 4.x headers would not build. At
run time GameInput is the player's: System32's `gameinput.dll` answers for API 3 when the Microsoft GameInput
redistributable (`GameInputRedist.msi`, in the same package) is installed; without it nothing changes but those
instruments go unread.
