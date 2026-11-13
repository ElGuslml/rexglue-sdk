/**
 * ReXGlue online play through Steam: the helper process's logic (see rex/system/online/steam_link.h).
 *
 * Separate from Steam itself (Backend) so that the unit tests can run two helpers on a fake Steam.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::system::online::steam_helper {

// What the helper needs of Steam: the real one (steam_backend.cpp, through steam_api64.dll) or a
// test's. Called on the helper's thread only.
class Backend {
 public:
  struct Events {
    // A lobby created or joined (ok), or the attempt failed.
    std::function<void(bool ok, uint64_t lobby)> lobby_entered;
    // A lobby's members or owner changed.
    std::function<void(uint64_t lobby)> lobby_changed;
    // The player chose to join a friend's lobby in Steam (an invite, "Join game").
    std::function<void(uint64_t lobby)> join_requested;
    // A user's first message to this one: accept it or not.
    std::function<void(uint64_t user)> session_requested;
    // A friend's state or rich presence changed.
    std::function<void()> friends_changed;
  };
  struct FriendInfo {
    uint64_t id = 0;
    std::string persona;
    uint64_t lobby = 0;  // the lobby it is in, if Steam says
  };

  virtual ~Backend() = default;
  // Starts Steam as kAppId; false and *error if it cannot.
  virtual bool Init(std::string* error) = 0;
  virtual void Shutdown() = 0;
  virtual uint64_t SelfId() = 0;
  virtual std::string SelfName() = 0;
  virtual void RunCallbacks(const Events& events) = 0;
  virtual void SetRichPresence(const char* key, const std::string& value) = 0;
  virtual void CreateLobby(int max_members) = 0;
  virtual void JoinLobby(uint64_t lobby) = 0;
  virtual void LeaveLobby(uint64_t lobby) = 0;
  virtual uint64_t LobbyOwner(uint64_t lobby) = 0;
  virtual std::string LobbyData(uint64_t lobby, const char* key) = 0;
  virtual void SetLobbyData(uint64_t lobby, const char* key, const std::string& value) = 0;
  virtual bool IsLobbyMember(uint64_t lobby, uint64_t user) = 0;
  virtual bool IsFriend(uint64_t user) = 0;
  // The friends running this app now.
  virtual std::vector<FriendInfo> FriendsInApp() = 0;
  virtual std::string FriendPresence(uint64_t user, const char* key) = 0;
  virtual void RequestFriendPresence(uint64_t user) = 0;
  virtual bool Send(uint64_t to, int channel, const uint8_t* data, size_t size, bool reliable) = 0;
  using ReceiveFn = std::function<void(uint64_t from, const uint8_t* data, size_t size)>;
  // Every message waiting on the channel.
  virtual void Receive(int channel, const ReceiveFn& receive) = 0;
  virtual void AcceptSession(uint64_t user) = 0;
};

struct Options {
  uint16_t game_port = 0;  // the game's online socket, on 127.0.0.1
  uint64_t token = 0;      // carried by every message to the game
  // False once the game has ended (none: never checked).
  std::function<bool()> parent_alive;
};

// Runs the helper until the game says bye or ends (or *stop). The process's exit code.
int Run(Backend* backend, const Options& options, std::atomic<bool>* stop = nullptr);

}  // namespace rex::system::online::steam_helper
