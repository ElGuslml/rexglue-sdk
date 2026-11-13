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

#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_BOOL(online_play, false, "Online",
                    "Online play between friends (experimental): the profile is signed in to Xbox "
                    "Live with an online XUID of its own (kept in its folder, online_xuid.txt), has "
                    "the multiplayer privileges and a network cable. Off: offline, as before");
REXCVAR_DEFINE_UINT32(online_port, 3074, "Online",
                      "Online play: the UDP port this PC's game takes for its friends (one port "
                      "for every player; a second instance on one PC needs another)");
REXCVAR_DEFINE_STRING(online_host, "", "Online",
                      "Online play: the friend whose game to join, as address:port (a direct "
                      "connection); empty: this PC's game is the host its friends join");
REXCVAR_DEFINE_BOOL(online_steam, false, "Online",
                    "Online play through Steam (Steam running and signed in): the Steam friends "
                    "running this game in its friends list, invites, and Valve's relays, so no port "
                    "is opened. Steam runs in rexsteam.exe, next to the game, as app 480 "
                    "(Spacewar), which Steam shows to the friends");
REXCVAR_DEFINE_BOOL(online_upnp, true, "Online",
                    "Online play, hosting: ask the router (UPnP) to forward the UDP port (online_port) "
                    "to this PC while the game runs, so friends over the internet can join; nothing is "
                    "asked when the router's own internet address is private (carrier-grade NAT), "
                    "which the log says. Off: the port is forwarded by hand, or not at all");
REXCVAR_DEFINE_STRING(online_xuid, "", "Online",
                      "The online XUID to use instead of the profile's (16 hexadecimal digits), for "
                      "two instances of a game on one PC with copies of one profile. Empty: the "
                      "profile's");

namespace rex {
namespace system {
namespace xam {

namespace {

// Online XUIDs on Xbox Live begin with 0009: the rest of the profile's is random.
constexpr uint64_t kOnlineXuidPrefix = 0x0009000000000000ull;
constexpr uint64_t kOnlineXuidPrefixMask = 0xFFFF000000000000ull;

// An online XUID written as 16 hexadecimal digits (spaces around ignored); 0 if it is not one.
uint64_t ParseOnlineXuid(std::string text) {
  while (!text.empty() && std::isspace(uint8_t(text.back()))) {
    text.pop_back();
  }
  size_t start = 0;
  while (start < text.size() && std::isspace(uint8_t(text[start]))) {
    ++start;
  }
  text = text.substr(start);
  if (text.size() != 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
    return 0;
  }
  uint64_t xuid = std::stoull(text, nullptr, 16);
  return (xuid & kOnlineXuidPrefixMask) == kOnlineXuidPrefix && xuid != kOnlineXuidPrefix ? xuid : 0;
}

// A profile name for the game (profile_name): ASCII, as an Xbox 360 gamertag and as XamUserGetName
// copies it, at most 15 characters. Latin letters with accents (U+00C0 to U+017F) lose them; other
// characters are left out.
std::string GamerName(const std::string& utf8) {
  // U+00C0 to U+017F without their accents, 32 a line; '?' where it takes two letters (kTwo) or
  // none (the signs x and /).
  static constexpr char kFolded[] =
      "AAAAAA?CEEEEIIIIDNOOOOO?OUUUUY??"
      "aaaaaa?ceeeeiiiidnooooo?ouuuuy?y"
      "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
      "GgGgHhHhIiIiIiIiIi??JjKkkLlLlLlL"
      "lLlNnNnNnnNnOoOoOo??RrRrRrSsSsSs"
      "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  static_assert(sizeof(kFolded) - 1 == 0x180 - 0xC0);
  static constexpr std::pair<uint32_t, const char*> kTwo[] = {
      {0xC6, "AE"},  {0xDE, "Th"},  {0xDF, "ss"},  {0xE6, "ae"}, {0xFE, "th"},
      {0x132, "IJ"}, {0x133, "ij"}, {0x152, "OE"}, {0x153, "oe"}};
  std::string name;
  for (size_t i = 0; i < utf8.size();) {
    // The next character's code point.
    uint32_t c = uint8_t(utf8[i]);
    size_t length = c < 0x80           ? 1
                    : (c >> 5) == 0x6  ? 2
                    : (c >> 4) == 0xE  ? 3
                    : (c >> 3) == 0x1E ? 4
                                       : 1;
    if (length > 1) {
      c &= 0x7F >> length;
      for (size_t k = 1; k < length && i + k < utf8.size(); ++k) {
        c = (c << 6) | (uint8_t(utf8[i + k]) & 0x3F);
      }
    }
    i += length;
    if (c >= 0x20 && c < 0x7F) {
      name += char(c);
    } else if (c >= 0xC0 && c < 0x180 && kFolded[c - 0xC0] != '?') {
      name += kFolded[c - 0xC0];
    } else {
      for (const auto& [code, letters] : kTwo) {
        if (code == c) {
          name += letters;
        }
      }
    }
  }
  while (!name.empty() && name.front() == ' ') {
    name.erase(name.begin());
  }
  name = name.substr(0, 15);
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  return name;
}

}  // namespace

UserProfile::UserProfile() {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  xuid_ = 0xB13EBABEBABEBABE;
  // Every profile has this XUID: a launcher's profiles are told apart by their folder
  // (profile_root), so a save bound to its owner's XUID loads in any of them.
  name_ = GamerName(REXCVAR_GET(profile_name));
  if (name_.empty()) {
    name_ = "User";
  }

  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=gfwl+live&start=195
  // https://github.com/arkem/py360/blob/master/py360/constants.py
  // XPROFILE_GAMER_YAXIS_INVERSION
  AddSetting(std::make_unique<Int32Setting>(0x10040002, 0));
  // XPROFILE_OPTION_CONTROLLER_VIBRATION
  AddSetting(std::make_unique<Int32Setting>(0x10040003, 3));
  // XPROFILE_GAMERCARD_ZONE
  AddSetting(std::make_unique<Int32Setting>(0x10040004, 0));
  // XPROFILE_GAMERCARD_REGION
  AddSetting(std::make_unique<Int32Setting>(0x10040005, 0));
  // XPROFILE_GAMERCARD_CRED
  AddSetting(std::make_unique<Int32Setting>(0x10040006, 0xFA));
  // XPROFILE_GAMERCARD_REP
  AddSetting(std::make_unique<FloatSetting>(0x5004000B, 0.0f));
  // XPROFILE_OPTION_VOICE_MUTED
  AddSetting(std::make_unique<Int32Setting>(0x1004000C, 0));
  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  AddSetting(std::make_unique<Int32Setting>(0x1004000D, 0));
  // XPROFILE_OPTION_VOICE_VOLUME
  AddSetting(std::make_unique<Int32Setting>(0x1004000E, 0x64));
  // XPROFILE_GAMERCARD_MOTTO
  AddSetting(std::make_unique<UnicodeSetting>(0x402C0011, u""));
  // XPROFILE_GAMERCARD_TITLES_PLAYED
  AddSetting(std::make_unique<Int32Setting>(0x10040012, 1));
  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040013, 0));
  // XPROFILE_GAMER_DIFFICULTY
  AddSetting(std::make_unique<Int32Setting>(0x10040015, 0));
  // XPROFILE_GAMER_CONTROL_SENSITIVITY
  AddSetting(std::make_unique<Int32Setting>(0x10040018, 0));
  // Preferred color 1
  AddSetting(std::make_unique<Int32Setting>(0x1004001D, 0xFFFF0000u));
  // Preferred color 2
  AddSetting(std::make_unique<Int32Setting>(0x1004001E, 0xFF00FF00u));
  // XPROFILE_GAMER_ACTION_AUTO_AIM
  AddSetting(std::make_unique<Int32Setting>(0x10040022, 1));
  // XPROFILE_GAMER_ACTION_AUTO_CENTER
  AddSetting(std::make_unique<Int32Setting>(0x10040023, 0));
  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040024, 0));
  // XPROFILE_GAMER_RACE_TRANSMISSION
  AddSetting(std::make_unique<Int32Setting>(0x10040026, 0));
  // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  AddSetting(std::make_unique<Int32Setting>(0x10040027, 0));
  // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040028, 0));
  // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040029, 0));
  // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040038, 0));
  // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040039, 0));

  // If we set this, games will try to get it.
  // XPROFILE_GAMERCARD_PICTURE_KEY
  AddSetting(std::make_unique<UnicodeSetting>(0x4064000F, u"gamercard_picture_key"));

  // XPROFILE_TITLE_SPECIFIC1
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFF));
  // XPROFILE_TITLE_SPECIFIC2
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFE));
  // XPROFILE_TITLE_SPECIFIC3
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFD));
}

void UserProfile::AddSetting(std::unique_ptr<Setting> setting) {
  Setting* previous_setting = setting.get();
  std::swap(settings_[setting->setting_id], previous_setting);

  if (setting->is_set && setting->is_title_specific()) {
    SaveSetting(setting.get());
  }

  if (previous_setting) {
    // replace: swap out the old setting from the owning list
    for (auto vec_it = setting_list_.begin(); vec_it != setting_list_.end(); ++vec_it) {
      if (vec_it->get() == previous_setting) {
        vec_it->swap(setting);
        break;
      }
    }
  } else {
    // new setting: add to the owning list
    setting_list_.push_back(std::move(setting));
  }
}

UserProfile::Setting* UserProfile::GetSetting(uint32_t setting_id) {
  const auto& it = settings_.find(setting_id);
  if (it == settings_.end()) {
    return nullptr;
  }
  UserProfile::Setting* setting = it->second;
  if (setting->is_title_specific()) {
    // If what we have loaded in memory isn't for the title that is running
    // right now, then load it from disk.
    if (kernel_state_->title_id() != setting->loaded_title_id) {
      LoadSetting(setting);
    }
  }
  return setting;
}

void UserProfile::LoadSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "rb");
    if (file) {
      fseek(file, 0, SEEK_END);
      uint32_t input_file_size = static_cast<uint32_t>(ftell(file));
      fseek(file, 0, SEEK_SET);

      std::vector<uint8_t> serialized_data(input_file_size);
      fread(serialized_data.data(), 1, serialized_data.size(), file);
      fclose(file);
      setting->Deserialize(serialized_data);
      setting->loaded_title_id = kernel_state_->title_id();
    }
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to load unsupported profile setting from disk");
  }
}

void UserProfile::SaveSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto serialized_setting = setting->Serialize();
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    std::filesystem::create_directories(content_dir);
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "wb");
    fwrite(serialized_setting.data(), 1, serialized_setting.size(), file);
    fclose(file);
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to save unsupported profile setting to disk");
  }
}

void UserProfile::InitializeOnline(const std::filesystem::path& folder) {
  online_xuid_ = 0;
  if (!REXCVAR_GET(online_play)) {
    return;
  }
  uint64_t xuid = 0;
  const std::string& chosen = REXCVAR_GET(online_xuid);
  if (!chosen.empty()) {
    xuid = ParseOnlineXuid(chosen);
    if (!xuid) {
      REXSYS_WARN("Online play: online_xuid \"{}\" is not an online XUID (16 hexadecimal digits "
                  "beginning 0009); the profile's is used",
                  chosen);
    }
  }
  std::filesystem::path file = folder.empty() ? std::filesystem::path() : folder / "online_xuid.txt";
  if (!xuid && !file.empty()) {
    std::ifstream in(file);
    std::string text;
    if (in && std::getline(in, text)) {
      xuid = ParseOnlineXuid(text);
      if (!xuid) {
        REXSYS_WARN("Online play: {} does not hold an online XUID; a new one is made",
                    rex::path_to_utf8(file));
      }
    }
  }
  if (!xuid) {
    // 48 random bits under the 0009 prefix: two friends' profiles never share one in practice.
    std::random_device random;
    do {
      xuid = kOnlineXuidPrefix | ((uint64_t(random()) << 16 ^ uint64_t(random())) & 0xFFFFFFFFFFFFull);
    } while (xuid == kOnlineXuidPrefix);
    if (file.empty()) {
      REXSYS_WARN("Online play: no profile folder; the online XUID {:016X} lasts this session only",
                  xuid);
    } else {
      std::ofstream out(file, std::ios::trunc);
      out << fmt::format("{:016X}\n", xuid);
      if (!out) {
        REXSYS_WARN("Online play: could not write {}; the online XUID {:016X} lasts this session only",
                    rex::path_to_utf8(file), xuid);
      }
    }
  }
  online_xuid_ = xuid;
  REXSYS_INFO("Online play: \"{}\" signed in to Xbox Live, online XUID {:016X}", name_, xuid);
}

}  // namespace xam
}  // namespace system
}  // namespace rex
