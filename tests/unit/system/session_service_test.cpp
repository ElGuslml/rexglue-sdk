/**
 * Online play's sessions (rex/system/online/session_service.h): a host and a friend that joined it,
 * two networks over UDP on this PC.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/online/session_service.h>
#include <rex/system/online/virtual_net.h>

using namespace rex::system::online;

namespace {

constexpr uint32_t kTitle = 0x41560830;
constexpr uint32_t kParty = 0x23;      // GROUP_LOBBY + HOST
constexpr uint32_t kGame = 0x42D;      // GROUP_GAME + HOST + JOIN_IN_PROGRESS_DISABLED
constexpr uint32_t kJoinedParty = 0x22;

struct Player {
  Player(uint64_t id, const std::string& name) {
    uint16_t port = 0;
    auto transport = CreateUdpTransport(0, &port);
    REQUIRE(transport);
    endpoint = Endpoint::Udp(0x7F000001, port);
    net = std::make_unique<VirtualNet>(id, kTitle, std::move(transport));
    net->SetSelfName(name);
    sessions = std::make_unique<SessionService>(net.get(), id, name,
                                                [this](uint32_t) { ++invites; });
  }
  ~Player() { net->Stop(); }
  Endpoint endpoint;
  std::unique_ptr<VirtualNet> net;
  std::unique_ptr<SessionService> sessions;
  std::atomic<int> invites = 0;
};

template <typename Condition>
bool WaitFor(Condition condition, int timeout_ms = 3000) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

}  // namespace

TEST_CASE("A friend finds the host's match, by its mode", "[online]") {
  Player host(0x0009000000000101ull, "Host");
  Player friend_(0x0009000000000102ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));

  // The host's user: standard (not ranked) competitive.
  host.sessions->SetContext(0, SessionService::kContextGameType, 1);
  host.sessions->SetContext(0, SessionService::kContextGameMode, 8);
  host.sessions->SetContext(0, 3, 1);
  host.sessions->SetProperty(0, 0x1000000C, {0, 0, 0, 2});
  SessionInfo party_info, game_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  host.sessions->JoinLocal(0x1000, {0}, {false});
  host.sessions->Create(0x2000, kGame, 0, 2, 0, &game_info, &nonce);
  host.sessions->JoinLocal(0x2000, {0}, {false});
  CHECK(party_info.xnkid != game_info.xnkid);
  CHECK((party_info.xnkid[0] & 0xF0) == 0x80);
  XNADDR host_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  CHECK(std::memcmp(&game_info.host, &host_addr, sizeof(XNADDR)) == 0);

  std::vector<std::pair<uint32_t, uint32_t>> query = {{SessionService::kContextGameType, 1},
                                                      {SessionService::kContextGameMode, 8},
                                                      {3, 2}};
  // Advertised at its creation, then again with the host's user in it.
  REQUIRE(WaitFor([&] {
    auto results = friend_.sessions->Search(query, 30);
    return results.size() == 1 && results[0].filled_public == 1;
  }));
  auto found = friend_.sessions->Search(query, 30);
  // Only the match (matchmaking), not the party; one of its two slots filled.
  CHECK(found[0].info.xnkid == game_info.xnkid);
  CHECK(found[0].max_public == 2);
  CHECK(found[0].filled_public == 1);
  CHECK(found[0].owner == host.net->self());
  bool has_name = false;
  for (const auto& property : found[0].properties) {
    if (property.id == SessionService::kPropertyHostName) {
      // "Host" in UTF-16 big-endian, with its terminator.
      CHECK(property.value.size() == 10);
      CHECK(property.value[1] == 'H');
      has_name = true;
    }
  }
  CHECK(has_name);
  // Another mode (coop) or ranked: nothing.
  CHECK(friend_.sessions->Search({{SessionService::kContextGameMode, 7}}, 30).empty());
  CHECK(friend_.sessions->Search({{SessionService::kContextGameType, 0}}, 30).empty());

  // The party by its ID (an invite's search).
  auto party = friend_.sessions->FindById(party_info.xnkid);
  REQUIRE(party);
  CHECK(party->flags == kParty);

  // The match full: no longer found.
  host.sessions->JoinRemote(0x2000, {friend_.net->self()}, {false});
  REQUIRE(WaitFor([&] { return friend_.sessions->Search(query, 30).empty(); }));
  auto details = host.sessions->Details(0x2000);
  REQUIRE(details);
  CHECK(details->members.size() == 2);
  host.sessions->LeaveRemote(0x2000, {friend_.net->self()});
  REQUIRE(WaitFor([&] { return friend_.sessions->Search(query, 30).size() == 1; }));
  // Deleted: gone.
  host.sessions->Delete(0x2000);
  REQUIRE(WaitFor([&] { return friend_.sessions->Search(query, 30).empty(); }));
}

TEST_CASE("A friend in its own lobby is invited once to the host's party", "[online]") {
  Player host(0x0009000000000201ull, "Host");
  Player friend_(0x0009000000000202ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));

  SessionInfo party_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->FindById(party_info.xnkid).has_value(); }));
  // Not in its lobby yet: no invite.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(friend_.invites == 0);

  // Its own party (the online lobby): invited to the host's.
  SessionInfo own_info;
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->inviter == host.net->self());
  CHECK(invite->invitee == friend_.net->self());
  CHECK(invite->info.xnkid == party_info.xnkid);

  // The game leaves its party and joins the host's: no second invite.
  friend_.sessions->Delete(0x3000);
  SessionInfo joined = invite->info;
  friend_.sessions->Create(0x3001, kJoinedParty, 0, 8, 0, &joined, &nonce);
  CHECK(joined.xnkid == party_info.xnkid);
  host.sessions->Modify(0x1000, kParty, 8, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(friend_.invites == 1);

  // The host's party closed to invites (matchmaking): not offered.
  Player other(0x0009000000000203ull, "Other");
  other.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return other.net->hub() == host.net->self(); }));
  host.sessions->Modify(0x1000, kParty | SessionService::kInvitesDisabled, 8, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  SessionInfo other_own;
  other.sessions->Create(0x4000, kParty, 0, 8, 0, &other_own, &nonce);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(other.invites == 0);
}

TEST_CASE("The friends list, and an invite sent from it", "[online]") {
  Player host(0x0009000000000301ull, "Host");
  Player friend_(0x0009000000000302ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));

  // The friend's prompt: answered at once, yes the first time, no after.
  std::atomic<int> prompts = 0;
  std::string inviter;
  SessionService::SetInvitePrompt(
      [&](const std::string& name, std::function<void(bool)> answer) {
        inviter = name;
        answer(prompts++ == 0);
      });

  // No party yet: nothing to invite to.
  CHECK_FALSE(host.sessions->SendInvite(friend_.net->self()));
  // Not connected: not sent.
  CHECK_FALSE(host.sessions->SendInvite(0x0009000000000399ull));

  SessionInfo party_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  // The host in the friend's list, with its party.
  REQUIRE(WaitFor([&] {
    auto friends = friend_.sessions->Friends();
    return friends.size() == 1 && friends[0].has_party;
  }));
  auto friends = friend_.sessions->Friends();
  CHECK(friends[0].xuid == host.net->self());
  CHECK(friends[0].name == "Host");
  CHECK(friends[0].party.xnkid == party_info.xnkid);
  // The friend has no party: listed without one.
  auto host_friends = host.sessions->Friends();
  REQUIRE(host_friends.size() == 1);
  CHECK(host_friends[0].xuid == friend_.net->self());
  CHECK(host_friends[0].name == "Friend");
  CHECK_FALSE(host_friends[0].has_party);

  // Accepted: the game's invite notification, the host's party as the accepted invite.
  REQUIRE(host.sessions->SendInvite(friend_.net->self()));
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  CHECK(inviter == "Host");
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->inviter == host.net->self());
  CHECK(invite->info.xnkid == party_info.xnkid);
  XNADDR host_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  CHECK(std::memcmp(&invite->info.host, &host_addr, sizeof(XNADDR)) == 0);

  // Declined: nothing for the game.
  REQUIRE(host.sessions->SendInvite(friend_.net->self()));
  REQUIRE(WaitFor([&] { return prompts == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(friend_.invites == 1);

  SessionService::SetInvitePrompt({});
}

TEST_CASE("A ranked session is made a player match", "[online]") {
  Player host(0x0009000000000401ull, "Host");
  host.net->Start({});
  SessionInfo info;
  uint64_t nonce = 0;
  host.sessions->Create(0x2000, kGame | SessionService::kUsesArbitration, 0, 2, 0, &info, &nonce);
  auto details = host.sessions->Details(0x2000);
  REQUIRE(details);
  CHECK(details->flags == kGame);
}

TEST_CASE("An invite is told to the game once its gate has been open a second", "[online]") {
  Player host(0x0009000000000701ull, "Host");
  Player friend_(0x0009000000000702ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));
  std::atomic<bool> open = false;
  friend_.sessions->SetInviteGate([&] { return open.load(); });

  SessionInfo party_info, own_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->FindById(party_info.xnkid).has_value(); }));
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own_info, &nonce);
  // Accepted (the hub's party), not told while the game is loading.
  REQUIRE(WaitFor([&] { return friend_.sessions->accepted_invite().has_value(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(friend_.invites == 0);
  auto opened = std::chrono::steady_clock::now();
  open = true;
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  CHECK(std::chrono::steady_clock::now() - opened >= SessionService::kInviteGateSettle);
}

TEST_CASE("An invite whose party is gone before the game is told is dropped", "[online]") {
  Player host(0x0009000000000801ull, "Host");
  Player friend_(0x0009000000000802ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));
  std::atomic<bool> open = false;
  friend_.sessions->SetInviteGate([&] { return open.load(); });

  // The friend back in its own lobby, auto-invited to the host's new party while its gate is closed.
  SessionInfo party_info, own_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->FindById(party_info.xnkid).has_value(); }));
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->accepted_invite().has_value(); }));

  // The host leaves its online lobby: its party deleted before the gate opens.
  host.sessions->Delete(0x1000);
  REQUIRE(WaitFor([&] { return !friend_.sessions->FindById(party_info.xnkid).has_value(); }));
  open = true;
  REQUIRE(WaitFor([&] { return !friend_.sessions->accepted_invite().has_value(); }));
  std::this_thread::sleep_for(SessionService::kInviteGateSettle + std::chrono::milliseconds(300));
  CHECK(friend_.invites == 0);

  // A party of the host's again: invited, and told (the old party's invite does not stand in the way).
  SessionInfo second_info;
  host.sessions->Create(0x1001, kParty, 0, 8, 0, &second_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }, 4000));
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->info.xnkid == second_info.xnkid);
}

TEST_CASE("A friend is taken to its host's party once per join, not to the host's next parties", "[online]") {
  Player host(0x0009000000000901ull, "Host");
  Player friend_(0x0009000000000902ull, "Friend");
  host.net->Start({});
  friend_.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));

  // Joined: taken to the host's party from its own lobby.
  SessionInfo party_info, own_info;
  uint64_t nonce = 0;
  host.sessions->Create(0x1000, kParty, 0, 8, 0, &party_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->FindById(party_info.xnkid).has_value(); }));
  friend_.sessions->Create(0x3000, kParty, 0, 8, 0, &own_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.invites == 1; }));
  friend_.sessions->Delete(0x3000);
  SessionInfo joined = party_info;
  friend_.sessions->Create(0x3001, kJoinedParty, 0, 8, 0, &joined, &nonce);

  // The host leaves its party (a new one of its own at once), and the friend is back in its own lobby: not taken
  // to the host's new party.
  host.sessions->Delete(0x1000);
  SessionInfo second_info;
  host.sessions->Create(0x1001, kParty, 0, 8, 0, &second_info, &nonce);
  friend_.sessions->Delete(0x3001);
  friend_.sessions->Create(0x3002, kParty, 0, 8, 0, &own_info, &nonce);
  REQUIRE(WaitFor([&] { return friend_.sessions->FindById(second_info.xnkid).has_value(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(friend_.invites == 1);

  // The friend joins the host again (Steam's "Join game", or its address): taken to its party once more.
  friend_.net->SetHub({});
  friend_.net->SetHub(host.endpoint);
  REQUIRE(WaitFor([&] { return friend_.net->hub() == host.net->self(); }));
  REQUIRE(WaitFor([&] { return friend_.invites == 2; }, 4000));
  auto invite = friend_.sessions->accepted_invite();
  REQUIRE(invite);
  CHECK(invite->info.xnkid == second_info.xnkid);
}
