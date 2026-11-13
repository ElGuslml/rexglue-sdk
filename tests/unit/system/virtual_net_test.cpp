/**
 * Online play's virtual network (rex/system/online/virtual_net.h): peers over real UDP sockets on
 * this PC (loopback), as two or three game processes would be.
 */

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/online/virtual_net.h>

using namespace rex::system::online;

namespace {

constexpr uint32_t kTitle = 0x41560830;

struct Received {
  uint32_t src_in_addr = 0;
  uint16_t src_port = 0;
  std::vector<uint8_t> data;
};

class FakeSocket : public PacketSink {
 public:
  void Deliver(uint32_t src_in_addr, uint16_t src_port, const uint8_t* data, size_t size) override {
    std::lock_guard lock(mutex);
    received.push_back({src_in_addr, src_port, std::vector<uint8_t>(data, data + size)});
  }
  size_t count() {
    std::lock_guard lock(mutex);
    return received.size();
  }
  std::mutex mutex;
  std::vector<Received> received;
};

// A peer: its network on a free UDP port of this PC.
struct TestPeer {
  TestPeer(uint64_t id, uint32_t title = kTitle) {
    uint16_t port = 0;
    auto transport = CreateUdpTransport(0, &port);
    REQUIRE(transport);
    endpoint = Endpoint::Udp(0x7F000001, port);
    net = std::make_unique<VirtualNet>(id, title, std::move(transport));
  }
  Endpoint endpoint;
  std::unique_ptr<VirtualNet> net;
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

TEST_CASE("An endpoint is parsed from address:port", "[online]") {
  Endpoint e = Endpoint::Parse("192.168.1.20:3074");
  CHECK(e.valid());
  CHECK(e.ipv4() == 0xC0A80114);
  CHECK(e.port() == 3074);
  CHECK(e.ToString() == "192.168.1.20:3074");
  CHECK_FALSE(Endpoint::Parse("192.168.1:3074").valid());
  CHECK_FALSE(Endpoint::Parse("192.168.1.20").valid());
  CHECK_FALSE(Endpoint::Parse("192.168.1.300:3074").valid());
}

TEST_CASE("A peer's XNADDR becomes its IN_ADDR and back", "[online]") {
  TestPeer a(0x0009000000000A01ull);
  XNADDR own{};
  a.net->GetTitleXnAddr(&own);
  uint32_t in_addr = 0;
  REQUIRE(a.net->XnAddrToInAddr(own, nullptr, &in_addr) == 0);
  CHECK(in_addr == a.net->self_in_addr());
  CHECK((in_addr >> 24) == 10);

  // Another peer's address, before it ever sent anything.
  TestPeer b(0x0009000000000B02ull);
  XNADDR other{};
  b.net->GetTitleXnAddr(&other);
  uint32_t other_in_addr = 0;
  REQUIRE(a.net->XnAddrToInAddr(other, nullptr, &other_in_addr) == 0);
  CHECK(other_in_addr != in_addr);
  XNADDR back{};
  REQUIRE(a.net->InAddrToXnAddr(other_in_addr, &back, nullptr) == 0);
  CHECK(std::memcmp(back.abOnline, other.abOnline, sizeof(back.abOnline)) == 0);

  // Not one of ours.
  XNADDR zero{};
  CHECK(a.net->XnAddrToInAddr(zero, nullptr, &in_addr) != 0);
}

TEST_CASE("Datagrams reach a peer's virtual port, also through the hub", "[online]") {
  TestPeer host(0x0009000000000001ull);
  TestPeer b(0x0009000000000002ull);
  TestPeer c(0x0009000000000003ull);
  host.net->Start({});
  b.net->Start(host.endpoint);
  c.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return b.net->hub() == host.net->self(); }));
  REQUIRE(WaitFor([&] { return c.net->hub() == host.net->self(); }));

  FakeSocket host_socket, b_socket, c_socket;
  uint16_t port = 1000;
  REQUIRE(host.net->Bind(&host_socket, &port));
  port = 1000;
  REQUIRE(b.net->Bind(&b_socket, &port));
  port = 1000;
  REQUIRE(c.net->Bind(&c_socket, &port));
  port = 1000;
  FakeSocket second;
  CHECK_FALSE(host.net->Bind(&second, &port));  // taken

  XNADDR host_addr{}, b_addr{}, c_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  b.net->GetTitleXnAddr(&b_addr);
  c.net->GetTitleXnAddr(&c_addr);

  // B to the host.
  uint32_t host_in_b = 0;
  REQUIRE(b.net->XnAddrToInAddr(host_addr, nullptr, &host_in_b) == 0);
  const uint8_t game_data[] = {0x00, 0x03, 'G', 'H', '!'};
  CHECK(b.net->SendTo(1000, host_in_b, 1000, game_data, sizeof(game_data)) == sizeof(game_data));
  REQUIRE(WaitFor([&] { return host_socket.count() == 1; }));
  uint32_t b_in_host = 0;
  REQUIRE(host.net->XnAddrToInAddr(b_addr, nullptr, &b_in_host) == 0);
  CHECK(host_socket.received[0].src_in_addr == b_in_host);
  CHECK(host_socket.received[0].src_port == 1000);
  CHECK(host_socket.received[0].data == std::vector<uint8_t>(game_data, game_data + 5));

  // B to C: neither knows where the other is; the host forwards.
  uint32_t c_in_b = 0;
  REQUIRE(b.net->XnAddrToInAddr(c_addr, nullptr, &c_in_b) == 0);
  CHECK(b.net->SendTo(1000, c_in_b, 1000, game_data, sizeof(game_data)) == sizeof(game_data));
  REQUIRE(WaitFor([&] { return c_socket.count() == 1; }));
  uint32_t b_in_c = 0;
  REQUIRE(c.net->XnAddrToInAddr(b_addr, nullptr, &b_in_c) == 0);
  CHECK(c_socket.received[0].src_in_addr == b_in_c);

  // A connection to C from B, through the hub.
  CHECK(b.net->Connect(c_in_b) == 0);
  REQUIRE(WaitFor([&] { return b.net->GetConnectStatus(c_in_b) == kConnectConnected; }));

  // To one's own address: delivered locally.
  CHECK(host.net->SendTo(1000, host.net->self_in_addr(), 1000, game_data, sizeof(game_data)) ==
        sizeof(game_data));
  REQUIRE(WaitFor([&] { return host_socket.count() == 2; }));

  // To nobody.
  CHECK(b.net->SendTo(1000, 0x0A0B0C0D, 1000, game_data, sizeof(game_data)) == -1);

  host.net->Unbind(&host_socket, 1000);
  port = 1000;
  CHECK(host.net->Bind(&second, &port));
  c.net->Stop();
  b.net->Stop();
  host.net->Stop();
}

TEST_CASE("A peer in two sessions is two addresses, as XNet's security associations", "[online]") {
  TestPeer host(0x0009000000000031ull);
  TestPeer b(0x0009000000000032ull);
  host.net->Start({});
  b.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return b.net->hub() == host.net->self(); }));
  FakeSocket host_socket;
  uint16_t port = 1000;
  REQUIRE(host.net->Bind(&host_socket, &port));

  XNADDR host_addr{}, b_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  b.net->GetTitleXnAddr(&b_addr);
  const uint8_t party[kXnKidSize] = {0x81, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t match[kXnKidSize] = {0x82, 2, 2, 2, 2, 2, 2, 2};
  uint32_t party_in_b = 0, match_in_b = 0;
  REQUIRE(b.net->XnAddrToInAddr(host_addr, party, &party_in_b) == 0);
  REQUIRE(b.net->XnAddrToInAddr(host_addr, match, &match_in_b) == 0);
  CHECK(party_in_b != match_in_b);
  uint8_t kid[kXnKidSize] = {};
  XNADDR back{};
  REQUIRE(b.net->InAddrToXnAddr(match_in_b, &back, kid) == 0);
  CHECK(std::memcmp(kid, match, kXnKidSize) == 0);
  CHECK(std::memcmp(back.abOnline, host_addr.abOnline, sizeof(back.abOnline)) == 0);

  // A datagram in each session: two sources at the host, its own addresses for B in each.
  const uint8_t data[] = {0, 1, 'x'};
  CHECK(b.net->SendTo(1000, party_in_b, 1000, data, sizeof(data)) == sizeof(data));
  CHECK(b.net->SendTo(1000, match_in_b, 1000, data, sizeof(data)) == sizeof(data));
  REQUIRE(WaitFor([&] { return host_socket.count() == 2; }));
  uint32_t b_party_in_host = 0, b_match_in_host = 0;
  REQUIRE(host.net->XnAddrToInAddr(b_addr, party, &b_party_in_host) == 0);
  REQUIRE(host.net->XnAddrToInAddr(b_addr, match, &b_match_in_host) == 0);
  CHECK(host_socket.received[0].src_in_addr == b_party_in_host);
  CHECK(host_socket.received[1].src_in_addr == b_match_in_host);
  CHECK(b_party_in_host != b_match_in_host);
  b.net->Stop();
  host.net->Stop();
}

TEST_CASE("A QoS lookup gets the target's listen data and round trip", "[online]") {
  TestPeer host(0x0009000000000011ull);
  TestPeer b(0x0009000000000012ull);
  host.net->Start({});
  b.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return b.net->hub() == host.net->self(); }));

  std::array<uint8_t, kXnKidSize> kid = {0x80, 1, 2, 3, 4, 5, 6, 7};
  std::array<uint8_t, kXnKidSize> other_kid = {0x80, 9, 9, 9, 9, 9, 9, 9};
  const uint8_t data[] = {0x01};
  host.net->QosListen(kid.data(), data, 1, kQosListenEnable | kQosListenSetData);

  XNADDR host_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  std::mutex mutex;
  std::vector<std::pair<size_t, QosResult>> results;
  b.net->QosLookup({{host_addr, kid}, {host_addr, other_kid}}, 4,
                   [&](size_t index, const QosResult& result) {
                     std::lock_guard lock(mutex);
                     results.push_back({index, result});
                   });
  REQUIRE(WaitFor([&] {
    std::lock_guard lock(mutex);
    return results.size() == 2;
  }));
  for (auto& [index, result] : results) {
    if (index == 0) {
      CHECK((result.flags & kQosComplete));
      CHECK((result.flags & kQosTargetContacted));
      CHECK_FALSE((result.flags & kQosTargetDisabled));
      CHECK((result.flags & kQosDataReceived));
      CHECK(result.data == std::vector<uint8_t>{0x01});
      CHECK(result.probes_recv == 4);
      CHECK(result.up_bits_per_sec > 0);
    } else {
      // Not listening for that session: complete, never contacted.
      CHECK(result.flags == kQosComplete);
    }
  }

  // A session's key registered without listening (a party): contacted, no data.
  std::array<uint8_t, kXnKidSize> party_kid = {0x80, 7, 7, 7, 7, 7, 7, 7};
  const uint8_t party_key[kXnKeySize] = {};
  host.net->RegisterKey(party_kid.data(), party_key);
  results.clear();
  b.net->QosLookup({{host_addr, party_kid}}, 1, [&](size_t index, const QosResult& result) {
    std::lock_guard lock(mutex);
    results.push_back({index, result});
  });
  REQUIRE(WaitFor([&] {
    std::lock_guard lock(mutex);
    return results.size() == 1;
  }));
  CHECK(results[0].second.flags == (kQosComplete | kQosTargetContacted));

  // Listening turned off: the target answers as disabled.
  host.net->QosListen(kid.data(), nullptr, 0, kQosListenDisable);
  results.clear();
  b.net->QosLookup({{host_addr, kid}}, 2, [&](size_t index, const QosResult& result) {
    std::lock_guard lock(mutex);
    results.push_back({index, result});
  });
  REQUIRE(WaitFor([&] {
    std::lock_guard lock(mutex);
    return results.size() == 1;
  }));
  CHECK((results[0].second.flags & kQosTargetDisabled));
  b.net->Stop();
  host.net->Stop();
}

TEST_CASE("A peer of another game is refused", "[online]") {
  TestPeer host(0x0009000000000021ull);
  TestPeer stranger(0x0009000000000022ull, 0x415607F7);
  host.net->Start({});
  stranger.net->Start(host.endpoint);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(stranger.net->hub() == 0);
  stranger.net->Stop();
  host.net->Stop();
}

TEST_CASE("A connection the peer's game closes is lost here, its other session's not", "[online]") {
  TestPeer host(0x0009000000000041ull);
  TestPeer b(0x0009000000000042ull);
  host.net->Start({});
  b.net->Start(host.endpoint);
  REQUIRE(WaitFor([&] { return b.net->hub() == host.net->self(); }));
  XNADDR host_addr{}, b_addr{};
  host.net->GetTitleXnAddr(&host_addr);
  b.net->GetTitleXnAddr(&b_addr);
  const uint8_t party[kXnKidSize] = {0x81, 4, 4, 4, 4, 4, 4, 4};
  const uint8_t match[kXnKidSize] = {0x82, 5, 5, 5, 5, 5, 5, 5};
  uint32_t party_in_b = 0, match_in_b = 0, b_party_in_host = 0, b_match_in_host = 0;
  REQUIRE(b.net->XnAddrToInAddr(host_addr, party, &party_in_b) == 0);
  REQUIRE(b.net->XnAddrToInAddr(host_addr, match, &match_in_b) == 0);
  REQUIRE(host.net->XnAddrToInAddr(b_addr, party, &b_party_in_host) == 0);
  REQUIRE(host.net->XnAddrToInAddr(b_addr, match, &b_match_in_host) == 0);
  CHECK(b.net->GetConnectStatus(match_in_b) == kConnectConnected);

  // The host's game leaves the match: its address for B there, unregistered.
  CHECK(host.net->UnregisterInAddr(b_match_in_host) == 0);
  REQUIRE(WaitFor([&] { return b.net->GetConnectStatus(match_in_b) == kConnectLost; }));
  CHECK(b.net->GetConnectStatus(party_in_b) == kConnectConnected);

  // The party's key unregistered: every connection of it.
  CHECK(host.net->UnregisterKey(party) == 0);
  REQUIRE(WaitFor([&] { return b.net->GetConnectStatus(party_in_b) == kConnectLost; }));

  // B's game lets the match go too, then joins a session with that key again: a new connection with an address of
  // its own, while the old address stays lost (a game may still hold it for the dead connection).
  CHECK(b.net->UnregisterInAddr(match_in_b) == 0);
  uint32_t again = 0;
  REQUIRE(b.net->XnAddrToInAddr(host_addr, match, &again) == 0);
  CHECK(again != match_in_b);
  CHECK(b.net->GetConnectStatus(again) == kConnectConnected);
  CHECK(b.net->GetConnectStatus(match_in_b) == kConnectLost);
  // Nothing is sent on the released address; the new one reaches the host, at an address of B's new there too.
  FakeSocket host_socket;
  uint16_t port = 1000;
  REQUIRE(host.net->Bind(&host_socket, &port));
  const uint8_t data[] = {0, 1, 'x'};
  b.net->SendTo(1000, match_in_b, 1000, data, sizeof(data));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(host_socket.count() == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));  // past kReopenAfter
  CHECK(b.net->SendTo(1000, again, 1000, data, sizeof(data)) == sizeof(data));
  REQUIRE(WaitFor([&] { return host_socket.count() == 1; }));
  CHECK(host_socket.received[0].src_in_addr != b_match_in_host);
  CHECK(host.net->GetConnectStatus(b_match_in_host) == kConnectLost);
  b.net->Stop();
  host.net->Stop();
}
