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
