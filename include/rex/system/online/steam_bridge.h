/**
 * ReXGlue online play through Steam (online_steam): the game's side of its Steam helper
 * (rex/system/online/steam_link.h).
 *
 * A Transport for the virtual network: frames to a Steam endpoint (Endpoint::Steam, a Steam ID) go
 * to the helper, which sends them over Steam's relay; the others go out of the UDP socket it wraps,
 * the same socket that talks to the helper on this PC's loopback. So a game reaches its friends
 * through Steam and still takes direct players (online_host) at once.
 *
 * Every game keeps a friends-only Steam lobby; the lobby's owner is the hub its members join (the
 * virtual network's hub, as online_host's). Joining a friend's lobby (an invite accepted, Steam's
 * "Join game") makes its owner this game's hub; the sessions' own invite to the hub's party follows.
 * The friends list is the virtual network's peers and the Steam friends running this game; an
 * invite to a Steam friend not connected goes through the helpers, and accepted, the friend's game
 * joins the inviter's lobby and then the party.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/system/online/virtual_net.h>

namespace rex::system::online {

class SessionService;

class SteamBridge : public Transport {
 public:
  struct Friend {
    uint64_t steam_id = 0;
    uint64_t xuid = 0;
    std::string name;     // the friend's profile name (its gamertag in the game)
    std::string persona;  // its Steam name
    uint64_t lobby = 0;
  };
  // Starts the helper for this game's port and token; false if it could not. Tests run helpers
  // on threads; the default runs rexsteam next to the executable.
  using Launcher = std::function<bool(uint16_t port, uint64_t token)>;

  // inner: the UDP transport; port: its port.
  SteamBridge(std::unique_ptr<Transport> inner, uint16_t port);
  ~SteamBridge() override;

  // Connects the bridge to the network (the lobby's owner as its hub) and the sessions (Steam
  // friends, invites); then starts the helper.
  void Attach(VirtualNet* net, SessionService* sessions);
  bool Start(uint32_t title, uint32_t protocol, uint64_t xuid, const std::string& name,
             Launcher launcher = {});

  // Transport.
  void SendTo(const Endpoint& to, const uint8_t* data, size_t size) override;
  void Poll(uint32_t timeout_ms, const Receive& receive) override;

  bool ready() const;
  uint64_t self_steam_id() const;
  // The Steam friends running this game now.
  std::vector<Friend> Friends() const;
  // Sends an invite (SessionService's) to a Steam friend by its XUID; false if it is not one.
  bool Invite(uint64_t xuid, const std::vector<uint8_t>& invite);
  void JoinLobby(uint64_t lobby);

 private:
  void OnHelper(const uint8_t* data, size_t size, const Endpoint& from, const Receive& receive);
  void SendHelper(const std::vector<uint8_t>& message);
  void SendHello();

  std::unique_ptr<Transport> inner_;
  const uint16_t port_;
  uint64_t token_ = 0;
  VirtualNet* net_ = nullptr;
  SessionService* sessions_ = nullptr;

  mutable std::mutex mutex_;
  Endpoint helper_;
  bool started_ = false;
  bool ready_ = false;
  bool failed_ = false;
  uint32_t title_ = 0;
  uint32_t protocol_ = 0;
  uint64_t xuid_ = 0;
  std::string name_;
  uint64_t self_steam_id_ = 0;
  uint64_t hub_owner_ = 0;  // the lobby's owner when it is another game: this game's hub
  uint64_t lobby_ = 0;
  std::vector<Friend> friends_;
  int64_t next_hello_ms_ = 0;
};

}  // namespace rex::system::online
