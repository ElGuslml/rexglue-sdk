/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");
REXCVAR_DEFINE_STRING(gamepad_remap, "", "Input",
                      "Controller buttons moved to other Xbox 360 controls: sections 'type:control=input,...' "
                      "joined by ';'. type is SDL's gamepad type (xbox360, xboxone, ps3, ps4, ps5, switchpro, "
                      "joyconpair, standard...) or * for any; control is a b x y lb rb lt rt ls rs start back "
                      "guide; input is the controller's south east west north lb rb lt rt ls rs start back guide "
                      "dpup dpdown dpleft dpright misc1-6 paddle1-4 touchpad, alternatives joined by '|'. Empty: "
                      "the standard layout. Example: 'ps5:a=east,b=south'");
REXCVAR_DEFINE_STRING(gamepad_device_remap, "", "Input",
                      "As gamepad_remap, for particular controllers, applied after it: sections "
                      "'controller:control=input,...' joined by ';', a controller named by its serial number or "
                      "else its SDL GUID (as the log's OnControllerDeviceAdded lines give them). A launcher writes "
                      "it");
REXCVAR_DEFINE_BOOL(
    guitar_whammy_rest, false, "Input",
    "A guitar's whammy bar (the right stick's X axis) is at rest, -32768, until the guitar "
    "reports a position: an Xbox 360 guitar reads 0 there, half pressed, until the bar first "
    "moves. For a game that plays guitars; in another the bar at rest is the stick held left");

namespace rex::input::sdl {

namespace {

// --- gamepad_remap ---

// Physical inputs are SDL_GamepadButton bits, and these two for the triggers.
constexpr int kInputLeftTrigger = 32;
constexpr int kInputRightTrigger = 33;

constexpr uint64_t Bit(int input) { return uint64_t(1) << input; }

// The remappable Xbox 360 controls, in SDLInputDriver::ControllerState::sources order.
enum RemapControl : int { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kLS, kRS, kStart, kBack, kGuide };
constexpr const char* kControlNames[] = {"a",  "b",  "x",  "y",     "lb",   "rb",   "lt",
                                         "rt", "ls", "rs", "start", "back", "guide"};
// The guest's button for each control (the triggers are analog).
constexpr uint16_t kControlButtons[] = {
    X_INPUT_GAMEPAD_A,          X_INPUT_GAMEPAD_B,     X_INPUT_GAMEPAD_X,
    X_INPUT_GAMEPAD_Y,          X_INPUT_GAMEPAD_LEFT_SHOULDER,
    X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    0,                          0,                     X_INPUT_GAMEPAD_LEFT_THUMB,
    X_INPUT_GAMEPAD_RIGHT_THUMB, X_INPUT_GAMEPAD_START, X_INPUT_GAMEPAD_BACK,
    X_INPUT_GAMEPAD_GUIDE};
static_assert(std::size(kControlNames) == SDLInputDriver::kRemapControls);
static_assert(std::size(kControlButtons) == SDLInputDriver::kRemapControls);

struct InputName {
  const char* name;
  int input;
};
constexpr InputName kInputNames[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST},
    {"north", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", SDL_GAMEPAD_BUTTON_START},
    {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"misc1", SDL_GAMEPAD_BUTTON_MISC1},
    // SDL's mapping strings number the paddles in this order (paddle1 is the Elite's P1).
    {"paddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"paddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"paddle3", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"paddle4", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"misc2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", SDL_GAMEPAD_BUTTON_MISC3},
    {"misc4", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", SDL_GAMEPAD_BUTTON_MISC5},
    {"misc6", SDL_GAMEPAD_BUTTON_MISC6},
    {"lt", kInputLeftTrigger},
    {"rt", kInputRightTrigger},
};

// The standard layout, as the fixed table before gamepad_remap had it: the Elite's paddles press the face
// buttons, Misc1 and the PlayStation touchpad press Guide.
constexpr std::array<uint64_t, SDLInputDriver::kRemapControls> kDefaultSources = {
    Bit(SDL_GAMEPAD_BUTTON_SOUTH) | Bit(SDL_GAMEPAD_BUTTON_LEFT_PADDLE2),
    Bit(SDL_GAMEPAD_BUTTON_EAST) | Bit(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1),
    Bit(SDL_GAMEPAD_BUTTON_WEST) | Bit(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2),
    Bit(SDL_GAMEPAD_BUTTON_NORTH) | Bit(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1),
    Bit(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
    Bit(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
    Bit(kInputLeftTrigger),
    Bit(kInputRightTrigger),
    Bit(SDL_GAMEPAD_BUTTON_LEFT_STICK),
    Bit(SDL_GAMEPAD_BUTTON_RIGHT_STICK),
    Bit(SDL_GAMEPAD_BUTTON_START),
    Bit(SDL_GAMEPAD_BUTTON_BACK),
    Bit(SDL_GAMEPAD_BUTTON_GUIDE) | Bit(SDL_GAMEPAD_BUTTON_MISC1) | Bit(SDL_GAMEPAD_BUTTON_TOUCHPAD),
};

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

// Calls `f` with each trimmed piece of `s` between `separator`s.
template <typename F>
void ForEachPiece(std::string_view s, char separator, F f) {
  while (true) {
    size_t end = s.find(separator);
    f(Trim(s.substr(0, end)));
    if (end == std::string_view::npos) {
      return;
    }
    s.remove_prefix(end + 1);
  }
}

bool EqualsCI(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

// Moves the controls a section lists ("control=input|input,...") onto their inputs.
void ApplySection(std::string_view entries, std::array<uint64_t, SDLInputDriver::kRemapControls>& sources) {
  ForEachPiece(entries, ',', [&](std::string_view entry) {
    size_t equals = entry.find('=');
    if (entry.empty() || equals == std::string_view::npos) {
      return;
    }
    std::string_view control = Trim(entry.substr(0, equals));
    auto c = std::find_if(std::begin(kControlNames), std::end(kControlNames),
                          [&](const char* name) { return EqualsCI(control, name); });
    if (c == std::end(kControlNames)) {
      REXLOG_WARN("gamepad_remap: unknown control '{}'", control);
      return;
    }
    uint64_t mask = 0;
    ForEachPiece(entry.substr(equals + 1), '|', [&](std::string_view input) {
      if (input.empty()) {
        return;  // "a=": the control is not on the controller at all
      }
      auto i = std::find_if(std::begin(kInputNames), std::end(kInputNames),
                            [&](const InputName& n) { return EqualsCI(input, n.name); });
      if (i == std::end(kInputNames)) {
        REXLOG_WARN("gamepad_remap: unknown input '{}'", input);
        return;
      }
      mask |= Bit(i->input);
    });
    sources[size_t(c - std::begin(kControlNames))] = mask;
  });
}

// The sources of each control for a controller of `type`: the standard layout, then gamepad_remap's '*'
// sections, then the sections for this type, then gamepad_device_remap's section for this controller (its serial
// number's, else its GUID's). `changes` lists what differs from the standard layout.
std::array<uint64_t, SDLInputDriver::kRemapControls> RemapFor(SDL_GamepadType type, std::string_view serial,
                                                               std::string_view guid, std::string& changes) {
  std::array<uint64_t, SDLInputDriver::kRemapControls> sources = kDefaultSources;
  const std::string& text = REXCVAR_GET(gamepad_remap);
  const char* type_name = SDL_GetGamepadStringForType(type);
  for (int pass = 0; pass < 2; ++pass) {
    ForEachPiece(text, ';', [&](std::string_view section) {
      size_t colon = section.find(':');
      if (colon == std::string_view::npos) {
        return;
      }
      std::string_view section_type = Trim(section.substr(0, colon));
      bool match = pass == 0 ? section_type == "*" : (type_name && EqualsCI(section_type, type_name));
      if (!match) {
        return;
      }
      ApplySection(section.substr(colon + 1), sources);
    });
  }
  // A controller's name is cut at its section's last ':' (the entries have none; a serial number may).
  std::optional<std::string_view> by_serial, by_guid;
  ForEachPiece(REXCVAR_GET(gamepad_device_remap), ';', [&](std::string_view section) {
    size_t colon = section.rfind(':');
    if (colon == std::string_view::npos) {
      return;
    }
    std::string_view controller = Trim(section.substr(0, colon));
    if (!serial.empty() && EqualsCI(controller, serial)) {
      by_serial = section.substr(colon + 1);
    } else if (!guid.empty() && EqualsCI(controller, guid)) {
      by_guid = section.substr(colon + 1);
    }
  });
  if (by_serial || by_guid) {
    ApplySection(by_serial ? *by_serial : *by_guid, sources);
  }
  for (int c = 0; c < SDLInputDriver::kRemapControls; ++c) {
    if (sources[c] == kDefaultSources[c]) {
      continue;
    }
    std::string inputs;
    for (const InputName& n : kInputNames) {
      if (sources[c] & Bit(n.input)) {
        inputs += (inputs.empty() ? "" : "|") + std::string(n.name);
      }
    }
    changes += (changes.empty() ? "" : ",") + std::string(kControlNames[c]) + "=" + inputs;
  }
  return sources;
}

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

const char* JoystickTypeName(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_UNKNOWN:
      return "Unknown";
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      return "Gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL:
      return "Wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return "ArcadeStick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return "FlightStick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return "DancePad";
    case SDL_JOYSTICK_TYPE_GUITAR:
      return "Guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return "DrumKit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return "ArcadePad";
    case SDL_JOYSTICK_TYPE_THROTTLE:
      return "Throttle";
    default:
      return "?";
  }
}

const char* XInputSubTypeName(uint8_t s) {
  switch (s) {
    case XINPUT_DEVSUBTYPE_GAMEPAD:
      return "GAMEPAD";
    case XINPUT_DEVSUBTYPE_WHEEL:
      return "WHEEL";
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
      return "ARCADE_STICK";
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
      return "FLIGHT_STICK";
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
      return "DANCE_PAD";
    case XINPUT_DEVSUBTYPE_GUITAR:
      return "GUITAR";
    case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
      return "GUITAR_ALTERNATE";
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
      return "DRUM_KIT";
    case XINPUT_DEVSUBTYPE_GUITAR_BASS:
      return "GUITAR_BASS";
    case XINPUT_DEVSUBTYPE_ARCADE_PAD:
      return "ARCADE_PAD";
    default:
      return "?";
  }
}

// 'needle' must be lowercase ASCII.
bool NameContainsCI(const char* name, std::string_view needle) {
  if (!name || needle.empty()) {
    return false;
  }
  const std::string_view haystack(name);
  return std::search(
             haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
             }) != haystack.end();
}

// SDL_JoystickType numbering diverges from XINPUT_DEVSUBTYPE_* past value 6,
// so it cannot simply be cast.
uint8_t SdlTypeToXInputSubType(SDL_JoystickType t, const char* name) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      // SDL's XInput backend bakes the SubType into the device name, e.g.
      // "XInput Guitar #1".
      if (NameContainsCI(name, "guitar")) {
        return XINPUT_DEVSUBTYPE_GUITAR;
      }
      if (NameContainsCI(name, "drum")) {
        return XINPUT_DEVSUBTYPE_DRUM_KIT;
      }
      if (NameContainsCI(name, "wheel")) {
        return XINPUT_DEVSUBTYPE_WHEEL;
      }
      if (NameContainsCI(name, "dancepad") || NameContainsCI(name, "dance pad")) {
        return XINPUT_DEVSUBTYPE_DANCE_PAD;
      }
      if (NameContainsCI(name, "flightstick") || NameContainsCI(name, "flight stick") ||
          NameContainsCI(name, "hotas")) {
        return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
      }
      if (NameContainsCI(name, "arcadepad") || NameContainsCI(name, "arcade pad")) {
        return XINPUT_DEVSUBTYPE_ARCADE_PAD;
      }
      if (NameContainsCI(name, "arcade")) {
        return XINPUT_DEVSUBTYPE_ARCADE_STICK;
      }
      return XINPUT_DEVSUBTYPE_GAMEPAD;
    case SDL_JOYSTICK_TYPE_WHEEL:
      return XINPUT_DEVSUBTYPE_WHEEL;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return XINPUT_DEVSUBTYPE_ARCADE_STICK;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return XINPUT_DEVSUBTYPE_DANCE_PAD;
    case SDL_JOYSTICK_TYPE_GUITAR:
      return XINPUT_DEVSUBTYPE_GUITAR;
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return XINPUT_DEVSUBTYPE_DRUM_KIT;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return XINPUT_DEVSUBTYPE_ARCADE_PAD;
    default:
      return XINPUT_DEVSUBTYPE_GAMEPAD;
  }
}

uint8_t GamepadSubType(SDL_Gamepad* gamepad) {
  return SdlTypeToXInputSubType(SDL_GetJoystickType(SDL_GetGamepadJoystick(gamepad)),
                                SDL_GetGamepadName(gamepad));
}

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), SDL_GetError());
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (auto& controller : controllers_) {
      SDL_CloseGamepad(controller.sdl);
    }
    controllers_.clear();
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Polling can start before the window exists and the subsystems are up.
  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_) {
    return;
  }

  auto guard = DrainAndLock();

  for (const auto& controller : controllers_) {
    DeviceInfo info;
    info.id = controller.id;
    const char* name = SDL_GetGamepadName(controller.sdl);
    info.name = name ? name : "";
    char guid_text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller.sdl)), guid_text,
                     static_cast<int>(sizeof(guid_text)));
    info.guid = guid_text;
    const char* serial = SDL_GetGamepadSerial(controller.sdl);
    info.serial = serial ? serial : "";
    info.subtype = GamepadSubType(controller.sdl);
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT SDLInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // XInput vibration holds until the guest changes it, but SDL rumble expires,
  // and a zero duration expires on the next SDL_UpdateJoysticks. Arm it for
  // SDL's maximum instead; each call cancels the previous effect anyway.
  return SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                           vibration->right_motor_speed, kRumbleDurationMs)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDLInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(controller->state.gamepad.buttons)) |
                   AnalogToKeyfield(controller->state.gamepad))
                : uint64_t(0);
  KeystrokeState& last = controller->keystroke;

  // Handle repeating
  auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
  static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
  if (last.repeat_state == RepeatState::Waiting &&
      (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
    last.repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
    assert_true(vk != rex::ui::VirtualKey::kNone);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      auto fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        // up
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        // down
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnJoystickDeviceAddedLocked(const SDL_Event& event) {
  // Devices SDL cannot promote to a gamepad, such as some guitars and drum
  // kits, never reach the gamepad event and would otherwise vanish silently.
  const SDL_JoystickID id = event.jdevice.which;
  const char* name = SDL_GetJoystickNameForID(id);
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_text, static_cast<int>(sizeof(guid_text)));
  const bool is_gamepad = SDL_IsGamepad(id);
  REXLOG_INFO(
      "SDL JoystickAdded: \"{}\", VendorID(0x{:04X}), ProductID(0x{:04X}), GUID({}), "
      "HasGamepadMapping({})",
      name ? name : "?", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id), guid_text,
      is_gamepad);
  if (!is_gamepad) {
    REXLOG_WARN(
        "SDL JoystickAdded: \"{}\" has no gamepad mapping, so it will not be usable. Add one to "
        "the file named by --hid_mappings_file.",
        name ? name : "?");
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller)), guid_text,
                   static_cast<int>(sizeof(guid_text)));
  const SDL_JoystickType joy_type = SDL_GetJoystickType(SDL_GetGamepadJoystick(controller));
  const uint8_t subtype = GamepadSubType(controller);
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "XInputSubType({} = 0x{:02X}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X}), "
      "GUID({}), "
      "Serial({})",
      SDL_GetGamepadName(controller), JoystickTypeName(joy_type),
      static_cast<int>(SDL_GetGamepadType(controller)), XInputSubTypeName(subtype), subtype,
      SDL_GetGamepadVendor(controller), SDL_GetGamepadProduct(controller), guid_text,
      SDL_GetGamepadSerial(controller) ? SDL_GetGamepadSerial(controller) : "");

  // SDL_GetGamepadPlayerIndex is deliberately ignored: on Windows it reports
  // the XInput user index the OS assigned, which is unrelated to connection
  // order and leaves non-XInput pads unnumbered.
  ControllerState state = {};
  state.sdl = controller;
  state.id = AllocateDeviceId();
  state.state_changed = true;  // XInput starts with packet_number = 1
  std::string remap_changes;
  const char* serial = SDL_GetGamepadSerial(controller);
  state.sources = RemapFor(SDL_GetGamepadType(controller), serial ? serial : "", guid_text, remap_changes);
  if (!remap_changes.empty()) {
    REXLOG_INFO("SDL OnControllerDeviceAdded: gamepad_remap for {} (gamepad_device_remap for this one included): {}",
                SDL_GetGamepadStringForType(SDL_GetGamepadType(controller)), remap_changes);
  }
  UpdateXCapabilities(state);
  if (joy_type == SDL_JOYSTICK_TYPE_GUITAR && REXCVAR_GET(guitar_whammy_rest)) {
    state.whammy_unknown = true;
    state.state.gamepad.thumb_rx = INT16_MIN;
  }
  controllers_.push_back(state);

  const int ordinal = static_cast<int>(controllers_.size()) - 1;
  SDL_SetGamepadPlayerIndex(controller, ordinal);

  REXLOG_INFO("SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
              static_cast<uint64_t>(state.id));
  const char* mapping = SDL_GetGamepadMapping(controller);
  REXLOG_INFO("SDL Controller {}: {}", ordinal, mapping ? mapping : "<none>");
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    // Expected when the pad was never opened, e.g. it had no gamepad mapping.
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored, instance_id({}) not in use.",
                event.gdevice.which);
    return;
  }
  SDL_Gamepad* removed = controllers_.at(*idx).sdl;
  const char* removed_name = SDL_GetGamepadName(removed);
  char removed_guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(removed)), removed_guid,
                   static_cast<int>(sizeof(removed_guid)));
  REXLOG_INFO("SDL OnControllerDeviceRemoved: \"{}\", GUID({}).", removed_name ? removed_name : "?",
              removed_guid);
  SDL_CloseGamepad(removed);
  controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));

  // LEDs only. Guest user assignment does not shift, because InputSystem holds
  // each device's ordinal.
  for (size_t i = 0; i < controllers_.size(); i++) {
    SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
  }
  REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& controller = controllers_.at(*idx);
  auto& pad = controller.state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      if (controller.whammy_unknown) {
        if (event.gaxis.value == 0) {
          break;
        }
        controller.whammy_unknown = false;
        REXLOG_INFO("SDL: device {}'s whammy bar reported its first position, {}.",
                    static_cast<uint64_t>(controller.id), event.gaxis.value);
      }
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    // The triggers go through gamepad_remap, like the buttons.
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      controller.physical_triggers[0] = static_cast<uint8_t>(event.gaxis.value >> 7);
      ApplyRemap(controller);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      controller.physical_triggers[1] = static_cast<uint8_t>(event.gaxis.value >> 7);
      ApplyRemap(controller);
      break;
    default:
      assert_always();
      break;
  }
  controller.state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& controller = controllers_.at(*idx);

  if (event.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  // The controller's own button; the guest's follow from gamepad_remap (the standard layout by default).
  if (event.gbutton.down) {
    controller.physical_buttons |= Bit(event.gbutton.button);
  } else {
    controller.physical_buttons &= ~Bit(event.gbutton.button);
  }
  ApplyRemap(controller);
  controller.state_changed = true;
}

void SDLInputDriver::ApplyRemap(ControllerState& state) {
  static_assert(SDL_GAMEPAD_BUTTON_COUNT <= kInputLeftTrigger);
  auto pressed = [&state](uint64_t sources) {
    return (sources & state.physical_buttons) != 0 ||
           ((sources & Bit(kInputLeftTrigger)) && state.physical_triggers[0] > HID_SDL_TRIGG_THRES) ||
           ((sources & Bit(kInputRightTrigger)) && state.physical_triggers[1] > HID_SDL_TRIGG_THRES);
  };
  // An analog trigger keeps its travel; a button on a trigger pulls it all the way.
  auto trigger = [&state](uint64_t sources) {
    uint8_t value = 0;
    if (sources & Bit(kInputLeftTrigger)) value = std::max(value, state.physical_triggers[0]);
    if (sources & Bit(kInputRightTrigger)) value = std::max(value, state.physical_triggers[1]);
    if (sources & state.physical_buttons) value = 0xFF;
    return value;
  };

  // The D-pad is not remapped.
  uint16_t buttons = 0;
  constexpr std::pair<int, uint16_t> kDpad[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, X_INPUT_GAMEPAD_DPAD_UP},
                                                {SDL_GAMEPAD_BUTTON_DPAD_DOWN, X_INPUT_GAMEPAD_DPAD_DOWN},
                                                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_LEFT},
                                                {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, X_INPUT_GAMEPAD_DPAD_RIGHT}};
  for (auto [input, button] : kDpad) {
    if (state.physical_buttons & Bit(input)) {
      buttons |= button;
    }
  }
  for (int c = 0; c < kRemapControls; ++c) {
    if (kControlButtons[c] && pressed(state.sources[c])) {
      buttons |= kControlButtons[c];
    }
  }
  if (!REXCVAR_GET(guide_button)) {
    buttons &= ~uint16_t(X_INPUT_GAMEPAD_GUIDE);
  }
  state.state.gamepad.buttons = buttons;
  state.state.gamepad.left_trigger = trigger(state.sources[kLT]);
  state.state.gamepad.right_trigger = trigger(state.sources[kRT]);
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::FindController(DeviceId id) {
  if (id == DeviceId::kInvalid) {
    return nullptr;
  }
  for (auto& controller : controllers_) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

DeviceId SDLInputDriver::AllocateDeviceId() {
  return static_cast<DeviceId>(next_device_id_++);
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_DEBUG("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  SDL_Joystick* joystick = SDL_GetGamepadJoystick(state.sdl);
  if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  } else {
    // Backends leaving the connection state UNKNOWN still report power.
    const SDL_PowerState power_state = SDL_GetJoystickPowerInfo(joystick, nullptr);
    if (power_state == SDL_POWERSTATE_ON_BATTERY || power_state == SDL_POWERSTATE_CHARGING ||
        power_state == SDL_POWERSTATE_CHARGED) {
      cap_flags |= X_INPUT_CAPS_WIRELESS;
    }
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = XINPUT_DEVTYPE_GAMEPAD;
  c.sub_type = GamepadSubType(state.sdl);
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
