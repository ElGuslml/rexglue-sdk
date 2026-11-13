/**
 * Online play through Steam (rex/system/online/steam_bridge.h): games with their Steam helpers on
 * threads, over a fake Steam (lobbies, friends, rich presence, messages), on this PC's loopback.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/online/session_service.h>
#include <rex/system/online/steam_bridge.h>
#include <rex/system/online/steam_link.h>
#include <rex/system/online/virtual_net.h>

#include "helper.h"

using namespace rex::system::online;
using steam_helper::Backend;

namespace {

constexpr uint32_t kTitle = 0x41560830;
constexpr uint32_t kParty = 0x23;  // GROUP_LOBBY + HOST

// Steam for the tests: users, friendships, lobbies, rich presence and messages, all in memory.
struct FakeSteam {
  struct User {
    std::string persona;
    std::set<uint64_t> friends;
    bool running = false;
    // Its game not shown to its friends (a profile whose game details are private).
    bool game_hidden = false;
    std::map<std::string, std::string> presence;
    std::deque<std::function<void(const Backend::Events&)>> events;
    std::map<int, std::deque<std::pair<uint64_t, std::vector<uint8_t>>>> inbox;
    // Sessions: the users whose messages this one takes (accepted, or written to first).
    std::set<uint64_t> sessions;
    std::map<uint64_t, std::vector<std::pair<int, std::vector<uint8_t>>>> held;
  };
  struct Lobby {
    uint64_t owner = 0;
    std::vector<uint64_t> members;
    std::map<std::string, std::string> data;
  };

  std::mutex mutex;
  std::map<uint64_t, User> users;
  std::map<uint64_t, Lobby> lobbies;
  uint64_t next_lobby = 0x0186000000000001ull;

  void AddUser(uint64_t id, const std::string& persona) {
    std::lock_guard lock(mutex);
    users[id].persona = persona;
  }
  void Befriend(uint64_t a, uint64_t b) {
    std::lock_guard lock(mutex);
    users[a].friends.insert(b);
    users[b].friends.insert(a);
  }
  void Post(uint64_t user, std::function<void(const Backend::Events&)> event) {
    users[user].events.push_back(std::move(event));
  }
  uint64_t LobbyOf(uint64_t user) {
    for (auto& [id, lobby] : lobbies) {
      if (std::find(lobby.members.begin(), lobby.members.end(), user) != lobby.members.end()) {
        return id;
      }
    }
    return 0;
  }
  void Leave(uint64_t lobby_id, uint64_t user) {
    auto it = lobbies.find(lobby_id);
    if (it == lobbies.end()) {
      return;
    }
    auto& members = it->second.members;
    members.erase(std::remove(members.begin(), members.end(), user), members.end());
    if (members.empty()) {
      lobbies.erase(it);
      return;
    }
    if (it->second.owner == user) {
      it->second.owner = members.front();
    }
    for (uint64_t member : members) {
      Post(member, [lobby_id](const Backend::Events& e) { e.lobby_changed(lobby_id); });
    }
  }
};

class FakeBackend : public Backend {
 public:
  FakeBackend(FakeSteam* steam, uint64_t id) : steam_(steam), id_(id) {}

  bool Init(std::string*) override {
    std::lock_guard lock(steam_->mutex);
    steam_->users[id_].running = true;
    return true;
  }
  void Shutdown() override {
    std::lock_guard lock(steam_->mutex);
    steam_->users[id_].running = false;
  }
  uint64_t SelfId() override { return id_; }
  std::string SelfName() override {
    std::lock_guard lock(steam_->mutex);
    return steam_->users[id_].persona;
  }
  void RunCallbacks(const Events& events) override {
    std::deque<std::function<void(const Events&)>> pending;
    {
      std::lock_guard lock(steam_->mutex);
      pending.swap(steam_->users[id_].events);
    }
    for (auto& event : pending) {
      event(events);
    }
  }
  void SetRichPresence(const char* key, const std::string& value) override {
    std::lock_guard lock(steam_->mutex);
    steam_->users[id_].presence[key] = value;
    for (uint64_t f : steam_->users[id_].friends) {
      steam_->Post(f, [](const Events& e) { e.friends_changed(); });
    }
  }
  void CreateLobby(int) override {
    std::lock_guard lock(steam_->mutex);
    uint64_t id = steam_->next_lobby++;
    steam_->lobbies[id] = {id_, {id_}, {}};
    steam_->Post(id_, [id](const Events& e) { e.lobby_entered(true, id); });
  }
  void JoinLobby(uint64_t lobby) override {
    std::lock_guard lock(steam_->mutex);
    auto it = steam_->lobbies.find(lobby);
    if (it == steam_->lobbies.end()) {
      steam_->Post(id_, [](const Events& e) { e.lobby_entered(false, 0); });
      return;
    }
    it->second.members.push_back(id_);
    for (uint64_t member : it->second.members) {
      if (member != id_) {
        steam_->Post(member, [lobby](const Events& e) { e.lobby_changed(lobby); });
      }
    }
    steam_->Post(id_, [lobby](const Events& e) { e.lobby_entered(true, lobby); });
  }
  void LeaveLobby(uint64_t lobby) override {
    std::lock_guard lock(steam_->mutex);
    steam_->Leave(lobby, id_);
  }
  uint64_t LobbyOwner(uint64_t lobby) override {
    std::lock_guard lock(steam_->mutex);
    auto it = steam_->lobbies.find(lobby);
    return it == steam_->lobbies.end() ? 0 : it->second.owner;
  }
  std::string LobbyData(uint64_t lobby, const char* key) override {
    std::lock_guard lock(steam_->mutex);
    auto it = steam_->lobbies.find(lobby);
    return it == steam_->lobbies.end() ? std::string() : it->second.data[key];
  }
  void SetLobbyData(uint64_t lobby, const char* key, const std::string& value) override {
    std::lock_guard lock(steam_->mutex);
    auto it = steam_->lobbies.find(lobby);
    if (it != steam_->lobbies.end() && it->second.owner == id_) {
      it->second.data[key] = value;
    }
  }
  bool IsLobbyMember(uint64_t lobby, uint64_t user) override {
    std::lock_guard lock(steam_->mutex);
    auto it = steam_->lobbies.find(lobby);
    return it != steam_->lobbies.end() &&
           std::find(it->second.members.begin(), it->second.members.end(), user) !=
               it->second.members.end();
  }
  bool IsFriend(uint64_t user) override {
    std::lock_guard lock(steam_->mutex);
    return steam_->users[id_].friends.count(user) != 0;
  }
  std::vector<FriendInfo> FriendsInApp() override {
    std::lock_guard lock(steam_->mutex);
    std::vector<FriendInfo> list;
    for (uint64_t f : steam_->users[id_].friends) {
      if (steam_->users[f].running && !steam_->users[f].game_hidden) {
        list.push_back({f, steam_->users[f].persona, steam_->LobbyOf(f)});
      }
    }
    return list;
  }
  std::string FriendPresence(uint64_t user, const char* key) override {
    std::lock_guard lock(steam_->mutex);
    return steam_->users[user].presence[key];
  }
  void RequestFriendPresence(uint64_t) override {}
  bool Send(uint64_t to, int channel, const uint8_t* data, size_t size, bool) override {
    std::lock_guard lock(steam_->mutex);
    auto& receiver = steam_->users[to];
    if (!receiver.running) {
      return false;
    }
    // Writing to a user opens this side's session with it.
    steam_->users[id_].sessions.insert(to);
    std::vector<uint8_t> message(data, data + size);
    if (receiver.sessions.count(id_)) {
      receiver.inbox[channel].push_back({id_, std::move(message)});
    } else {
      bool first = receiver.held[id_].empty();
      receiver.held[id_].push_back({channel, std::move(message)});
      if (first) {
        uint64_t from = id_;
        steam_->Post(to, [from](const Events& e) { e.session_requested(from); });
      }
    }
    return true;
  }
  void Receive(int channel, const ReceiveFn& receive) override {
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> messages;
    {
      std::lock_guard lock(steam_->mutex);
      messages.swap(steam_->users[id_].inbox[channel]);
    }
    for (auto& [from, message] : messages) {
      receive(from, message.data(), message.size());
    }
  }
  void AcceptSession(uint64_t user) override {
    std::lock_guard lock(steam_->mutex);
    auto& self = steam_->users[id_];
    self.sessions.insert(user);
    for (auto& [channel, message] : self.held[user]) {
      self.inbox[channel].push_back({user, std::move(message)});
    }
    self.held.erase(user);
  }

 private:
  FakeSteam* steam_;
  uint64_t id_;
};

// A game: its network, sessions and Steam bridge, its helper on a thread.
struct Game {
  Game(FakeSteam* steam, uint64_t steam_id, uint64_t xuid, const std::string& name)
      : backend(steam, steam_id) {
    uint16_t port = 0;
    auto udp = CreateUdpTransport(0, &port);
    REQUIRE(udp);
    auto owned = std::make_unique<SteamBridge>(std::move(udp), port);
    bridge = owned.get();
    net = std::make_unique<VirtualNet>(xuid, kTitle, std::move(owned));
    net->SetSelfName(name);
    sessions = std::make_unique<SessionService>(net.get(), xuid, name,
                                                [this](uint32_t) { ++invites; });
    net->Start({});
    bridge->Attach(net.get(), sessions.get());
    bridge->Start(kTitle, VirtualNet::kProtocolVersion, xuid, name,
                  [this](uint16_t game_port, uint64_t token) {
                    helper = std::thread([this, game_port, token] {
                      steam_helper::Options options;
                      options.game_port = game_port;
                      options.token = token;
                      steam_helper::Run(&backend, options, &stop_helper);
                    });
                    return true;
                  });
  }
  ~Game() {
    net->Stop();
    sessions.reset();
    stop_helper = true;
    if (helper.joinable()) {
      helper.join();
    }
  }
  FakeBackend backend;
  SteamBridge* bridge = nullptr;
  std::unique_ptr<VirtualNet> net;
  std::unique_ptr<SessionService> sessions;
  std::thread helper;
  std::atomic<bool> stop_helper = false;
  std::atomic<int> invites = 0;
};

template <typename Condition>
bool WaitFor(Condition condition, int timeout_ms = 5000) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

bool Lists(SessionService& sessions, uint64_t xuid, const std::string& name) {
  for (const Friend& f : sessions.Friends()) {
    if (f.xuid == xuid && f.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Steam: a friend invited from the friends list joins the host's party", "[online]") {
  FakeSteam steam;
  constexpr uint64_t kSteamHost = 0x0110000100000501ull, kSteamFriend = 0x0110000100000502ull;
  constexpr uint64_t kXuidHost = 0x0009000000000501ull, kXuidFriend = 0x0009000000000502ull;
  steam.AddUser(kSteamHost, "host_persona");
  steam.AddUser(kSteamFriend, "friend_persona");
  steam.Befriend(kSteamHost, kSteamFriend);

  std::atomic<int> prompts = 0;
  SessionService::SetInvitePrompt([&](const std::string& name, std::function<void(bool)> answer) {
    CHECK(name == "Host");
    ++prompts;
    answer(true);
  });

  Game host(&steam, kSteamHost, kXuidHost, "Host");
  Game friend_(&steam, kSteamFriend, kXuidFriend, "Friend");
  REQUIRE(WaitFor([&] { return host.bridge->ready() && friend_.bridge->ready(); }));
  CHECK(host.bridge->self_steam_id() == kSteamHost);

  // Each in its own lobby, not connected: Steam lists the other by its profile name.
  REQUIRE(WaitFor([&] { return Lists(*host.sessions, kXuidFriend, "Friend"); }));
  REQUIRE(WaitFor([&] { return Lists(*friend_.sessions, kXuidHost, "Host"); }));
  CHECK(host.net->ConnectedPeers().empty());

  // The host's party, the friend in its own online lobby.
  SessionInfo party, own;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party, &nonce);
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own, &nonce);

  // Invited through Steam; accepted, the friend joins the host's lobby, then its party.
  REQUIRE(host.sessions->SendInvite(kXuidFriend));
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  CHECK(prompts == 1);
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->inviter == kXuidHost);
  CHECK(invite->info.xnkid == party.xnkid);
  CHECK(friend_.net->hub() == kXuidHost);
  CHECK(friend_.net->hub_endpoint() == Endpoint::Steam(kSteamHost));
  CHECK(host.net->KnowsPeer(kXuidFriend));
  // Connected now: listed once.
  int listed = 0;
  for (const Friend& f : host.sessions->Friends()) {
    listed += f.xuid == kXuidFriend ? 1 : 0;
  }
  CHECK(listed == 1);
  // No second invite to the same party.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(friend_.invites == 1);

  SessionService::SetInvitePrompt({});
}

TEST_CASE("Steam: \"Join game\" from Steam takes a friend to the host's party", "[online]") {
  FakeSteam steam;
  constexpr uint64_t kSteamHost = 0x0110000100000601ull, kSteamFriend = 0x0110000100000602ull;
  constexpr uint64_t kXuidHost = 0x0009000000000601ull, kXuidFriend = 0x0009000000000602ull;
  steam.AddUser(kSteamHost, "host_persona");
  steam.AddUser(kSteamFriend, "friend_persona");
  steam.Befriend(kSteamHost, kSteamFriend);

  Game host(&steam, kSteamHost, kXuidHost, "Host");
  Game friend_(&steam, kSteamFriend, kXuidFriend, "Friend");
  REQUIRE(WaitFor([&] { return host.bridge->ready() && friend_.bridge->ready(); }));
  REQUIRE(WaitFor([&] {
    for (const auto& f : friend_.bridge->Friends()) {
      if (f.xuid == kXuidHost && f.lobby) {
        return true;
      }
    }
    return false;
  }));
  uint64_t host_lobby = 0;
  for (const auto& f : friend_.bridge->Friends()) {
    host_lobby = f.lobby;
  }

  SessionInfo party, own;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party, &nonce);
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own, &nonce);

  // The friend chooses "Join game" in Steam's friends list.
  {
    std::lock_guard lock(steam.mutex);
    steam.Post(kSteamFriend, [host_lobby](const Backend::Events& e) { e.join_requested(host_lobby); });
  }
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->info.xnkid == party.xnkid);
  CHECK(friend_.net->hub() == kXuidHost);
}

TEST_CASE("Steam: a friend whose game Steam does not show is listed and invited all the same", "[online]") {
  FakeSteam steam;
  constexpr uint64_t kSteamHost = 0x0110000100000701ull, kSteamFriend = 0x0110000100000702ull;
  constexpr uint64_t kXuidHost = 0x0009000000000701ull, kXuidFriend = 0x0009000000000702ull;
  steam.AddUser(kSteamHost, "host_persona");
  steam.AddUser(kSteamFriend, "friend_persona");
  steam.Befriend(kSteamHost, kSteamFriend);
  // The friend's game details are private: the host's Steam never shows it in a game.
  steam.users[kSteamFriend].game_hidden = true;

  std::atomic<int> prompts = 0;
  SessionService::SetInvitePrompt([&](const std::string& name, std::function<void(bool)> answer) {
    CHECK(name == "Host");
    ++prompts;
    answer(true);
  });

  Game host(&steam, kSteamHost, kXuidHost, "Host");
  Game friend_(&steam, kSteamFriend, kXuidFriend, "Friend");
  REQUIRE(WaitFor([&] { return host.bridge->ready() && friend_.bridge->ready(); }));

  // The friend sees the host and tells it it is here: both lists, though the host's Steam shows no game.
  REQUIRE(WaitFor([&] { return Lists(*friend_.sessions, kXuidHost, "Host"); }));
  REQUIRE(WaitFor([&] { return Lists(*host.sessions, kXuidFriend, "Friend"); }, 10000));

  // The host invites the friend from its list; the friend joins its party.
  SessionInfo party, own;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party, &nonce);
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own, &nonce);
  REQUIRE(host.sessions->SendInvite(kXuidFriend));
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  CHECK(prompts == 1);
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->info.xnkid == party.xnkid);
  CHECK(friend_.net->hub() == kXuidHost);

  SessionService::SetInvitePrompt({});
}
