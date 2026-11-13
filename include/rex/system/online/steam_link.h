/**
 * ReXGlue online play through Steam: the messages between a game and its Steam helper.
 *
 * Steam runs in a small process of its own, rexsteam (src/system/online/steam_helper), never in
 * the game: as app 480 (Spacewar) Steam takes over the controllers of the process that starts it
 * (Steam Input applies Spacewar's action set, and the game's pads go dead), and its overlay would
 * hook the game's renderer. The helper starts Steam, keeps a friends-only lobby, the rich presence
 * and the friends list, and carries the virtual network's frames over Steam's relay.
 *
 * The game and its helper talk over UDP on this PC's loopback: the game's online socket
 * (online_port) and the helper's. The game starts the helper with its port, a random token the
 * helper's messages carry, and its process ID (the helper ends with the game). Little-endian.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rex::system::online::steam_link {

constexpr uint8_t kMagic[4] = {'R', 'X', 'S', 'L'};
constexpr uint32_t kVersion = 1;
// Steam's networking channels: the virtual network's frames, and the helpers' own messages
// (invites).
constexpr int kChannelFrames = 0;
constexpr int kChannelHelper = 1;
// The app ID: Spacewar, Valve's example app, which any Steam account may run.
constexpr uint32_t kAppId = 480;
// The rich presence and lobby keys.
constexpr const char* kKeyTitle = "rexglue_title";  // the game's title ID, 8 hex digits
constexpr const char* kKeyXuid = "rexglue_xuid";    // the player's online XUID, 16 hex digits
constexpr const char* kKeyName = "rexglue_name";    // the player's profile name
constexpr const char* kKeyProtocol = "rexglue_protocol";

enum class Message : uint8_t {
  // The game to its helper.
  kHello = 1,      // u32 version, u64 token, u32 title, u32 protocol, u64 xuid, str name
  kSend = 2,       // u64 Steam ID, bytes: a frame
  kJoinLobby = 3,  // u64 lobby
  kInvite = 4,     // u64 Steam ID, bytes: the invite (SessionService's)
  kBye = 5,
  // The helper to the game.
  kStatus = 64,   // u64 token, u8 state (Status), u64 own Steam ID, str persona name, str error
  kReceive = 65,  // u64 token, u64 Steam ID, bytes: a frame
  kLobby = 66,    // u64 token, u64 lobby, u64 owner: the lobby this game is in now
  kFriends = 67,  // u64 token, u16 count, {u64 Steam ID, u64 XUID, str name, str persona, u64 lobby}
  kInvited = 68,  // u64 token, u64 Steam ID, u64 lobby, bytes: an invite from a friend's game
  kLog = 69,      // u64 token, u8 level (0 info, 1 warning, 2 error), str text
  kAnnounce = 70,  // u64 token: the helper is up (every second until the game's hello)
};

enum class Status : uint8_t {
  kReady = 1,
  kFailed = 2,  // Steam could not start (not running, no DLL, ...): the error says
};

struct Writer {
  std::vector<uint8_t> out;
  explicit Writer(Message type) {
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t(type));
  }
  void U8(uint8_t v) { out.push_back(v); }
  void U16(uint16_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
  }
  void U32(uint32_t v) {
    U16(uint16_t(v));
    U16(uint16_t(v >> 16));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v));
    U32(uint32_t(v >> 32));
  }
  void Str(const std::string& s) {
    uint16_t n = uint16_t(s.size() > 1024 ? 1024 : s.size());
    U16(n);
    out.insert(out.end(), s.begin(), s.begin() + n);
  }
  void Bytes(const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }
};

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at = 5;
  bool ok = true;
  // A message's type, or 0 if it is not one.
  static uint8_t TypeOf(const uint8_t* data, size_t size) {
    return size >= 5 && std::memcmp(data, kMagic, 4) == 0 ? data[4] : 0;
  }
  const uint8_t* Take(size_t n) {
    if (at + n > size) {
      ok = false;
      return nullptr;
    }
    const uint8_t* p = data + at;
    at += n;
    return p;
  }
  uint8_t U8() {
    const uint8_t* p = Take(1);
    return p ? p[0] : 0;
  }
  uint16_t U16() {
    const uint8_t* p = Take(2);
    return p ? uint16_t(p[0] | p[1] << 8) : 0;
  }
  uint32_t U32() {
    uint32_t lo = U16();
    return lo | uint32_t(U16()) << 16;
  }
  uint64_t U64() {
    uint64_t lo = U32();
    return lo | uint64_t(U32()) << 32;
  }
  std::string Str() {
    uint16_t n = U16();
    const uint8_t* p = Take(n);
    return p ? std::string(reinterpret_cast<const char*>(p), n) : std::string();
  }
  // The rest of the message.
  const uint8_t* Rest(size_t* n) {
    *n = at <= size ? size - at : 0;
    const uint8_t* p = data + at;
    at = size;
    return p;
  }
};

}  // namespace rex::system::online::steam_link
