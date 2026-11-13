/**
 * ReXGlue online play: the virtual network under XNet (online_play).
 *
 * Every game process in an online match is a peer, known by its id: its profile's online XUID.
 * A peer's XNADDR carries that id (abOnline: "RXNP" and the id), never a real address. As XNet's
 * secure addresses, an IN_ADDR stands for a peer and a session's key ID together (a security
 * association): each process gives every (peer, XNKID) pair it meets a virtual IN_ADDR (10.x.y.z) of
 * its own, and every datagram carries its key ID, so a game that talks to one host in two sessions
 * (Guitar Hero: Metallica's party and match, both on port 1000) sees two connections. The game's datagram sockets are virtual: a socket bound to a port receives what
 * peers send to that port, and sendto() to a peer's IN_ADDR reaches it through one transport (a
 * real UDP port, or later Steam): frames between peers, with the game's datagram unchanged inside.
 *
 * Routing: a peer that joined by an address (online_host) knows only that peer, the hub; frames to
 * any other peer go through the hub, which forwards them. So the players connect to the host and
 * the game's peer-to-peer traffic between members still reaches every member.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/system/online/xnet_types.h>

namespace rex::system::online {

// Where a transport sends a frame: an IPv4 address and port (UDP: ip << 16 | port, 48 bits), or a
// Steam user (its Steam ID; an account's has bits above the 48th set: steam_bridge.h).
struct Endpoint {
  uint64_t value = 0;
  bool valid() const { return value != 0; }
  bool operator==(const Endpoint& other) const { return value == other.value; }
  bool operator<(const Endpoint& other) const { return value < other.value; }
  static Endpoint Udp(uint32_t ipv4, uint16_t port) { return {uint64_t(ipv4) << 16 | port}; }
  static Endpoint Steam(uint64_t steam_id) { return {steam_id}; }
  bool steam() const { return (value >> 48) != 0; }
  uint64_t steam_id() const { return value; }
  uint32_t ipv4() const { return uint32_t(value >> 16); }
  uint16_t port() const { return uint16_t(value); }
  std::string ToString() const;
  // "a.b.c.d:port"; invalid if it is not one.
  static Endpoint Parse(const std::string& text);
};

// Carries frames between endpoints (one real UDP socket, or later Steam).
class Transport {
 public:
  using Receive = std::function<void(const Endpoint& from, const uint8_t* data, size_t size)>;
  virtual ~Transport() = default;
  virtual void SendTo(const Endpoint& to, const uint8_t* data, size_t size) = 0;
  // Waits up to timeout_ms for frames and passes each to `receive`.
  virtual void Poll(uint32_t timeout_ms, const Receive& receive) = 0;
};

// A virtual socket bound to a port: receives the datagrams peers send to it.
class PacketSink {
 public:
  virtual ~PacketSink() = default;
  // src_in_addr: the sender's virtual IN_ADDR in this process; may be called on the network thread.
  virtual void Deliver(uint32_t src_in_addr, uint16_t src_port, const uint8_t* data, size_t size) = 0;
};

// One target's QoS lookup result (XNQOSINFO without the guest's pointers).
struct QosResult {
  uint8_t flags = 0;
  uint16_t probes_xmit = 0;
  uint16_t probes_recv = 0;
  uint16_t rtt_min_ms = 0;
  uint16_t rtt_med_ms = 0;
  uint32_t up_bits_per_sec = 0;
  uint32_t down_bits_per_sec = 0;
  std::vector<uint8_t> data;
};

class VirtualNet {
 public:
  // Frames carried by the transport; peers of other builds are refused by version.
  static constexpr uint32_t kProtocolVersion = 2;
  // A game datagram's largest size (VDP: 2-byte length + data + voice; Guitar Hero: 1200).
  static constexpr size_t kMaxDatagram = 1264;
  // The port an XNADDR reports (wPortOnline); not a real port.
  static constexpr uint16_t kXnAddrPort = 3074;

  // self: this process's peer id (its profile's online XUID); title_id: a peer of another game is
  // refused.
  VirtualNet(uint64_t self, uint32_t title_id, std::unique_ptr<Transport> transport);
  ~VirtualNet();

  // Starts the network thread; hub: the peer to join (online_host), or none to be one.
  void Start(const Endpoint& hub);
  void Stop();
  // Another hub (a Steam lobby joined, or its owner changed), or none to be one: the peers of the
  // old group are told goodbye and lost; the new hub is greeted.
  void SetHub(const Endpoint& hub);

  uint64_t self() const { return self_; }
  uint32_t self_in_addr() const { return self_in_addr_; }

  // XNet.
  void GetTitleXnAddr(XNADDR* addr) const;
  // 0, or a WSA error (WSAEINVAL: not one of our XNADDRs). xnkid: the session's key ID (null: none).
  uint32_t XnAddrToInAddr(const XNADDR& addr, const uint8_t* xnkid, uint32_t* in_addr);
  // The peer's XNADDR and the association's key ID (xnkid may be null).
  uint32_t InAddrToXnAddr(uint32_t in_addr, XNADDR* addr, uint8_t* xnkid);
  uint32_t UnregisterInAddr(uint32_t in_addr);
  uint32_t Connect(uint32_t in_addr);
  uint32_t GetConnectStatus(uint32_t in_addr);
  uint32_t RegisterKey(const uint8_t* xnkid, const uint8_t* xnkey);
  uint32_t UnregisterKey(const uint8_t* xnkid);
  uint32_t QosListen(const uint8_t* xnkid, const uint8_t* data, uint32_t size, uint32_t flags);
  // Probes each target (an XNADDR and its session's key ID) `probes` times; `done` is called on the
  // network thread once for each target, with its index, when it answered or timed out.
  using QosDone = std::function<void(size_t index, const QosResult& result)>;
  void QosLookup(const std::vector<std::pair<XNADDR, std::array<uint8_t, kXnKidSize>>>& targets,
                 uint32_t probes, QosDone done);

  // Sockets. Bind: *port 0 takes a free one; false if the port is taken.
  bool Bind(PacketSink* sink, uint16_t* port);
  void Unbind(PacketSink* sink, uint16_t port);
  // The datagram's size, or -1 if dst_in_addr is not a peer (WSAEHOSTUNREACH).
  int SendTo(uint16_t src_port, uint32_t dst_in_addr, uint16_t dst_port, const uint8_t* data,
             size_t size);

  // Whether a peer has been heard from (tests, logs).
  bool KnowsPeer(uint64_t peer) const;
  uint64_t hub() const;
  // The hub this game joins (online_host), invalid when it hosts.
  Endpoint hub_endpoint() const;

  // The player's name, sent in the hello (a peer's is known once it said hello).
  void SetSelfName(const std::string& name);
  std::string PeerName(uint64_t peer) const;
  // The peers connected now.
  std::vector<uint64_t> ConnectedPeers() const;

  // Control messages of the services above the network (the sessions), on numbered channels;
  // handlers and the peer handler are called on the network thread, outside the network's lock.
  using ControlHandler = std::function<void(uint64_t peer, const uint8_t* data, size_t size)>;
  void SetControlHandler(uint8_t channel, ControlHandler handler);
  void SendControl(uint64_t peer, uint8_t channel, const uint8_t* data, size_t size);
  // A peer connected (true: it said hello or answered) or was lost (false).
  using PeerHandler = std::function<void(uint64_t peer, bool connected)>;
  void SetPeerHandler(PeerHandler handler);

 private:
  enum class FrameType : uint8_t {
    kHello = 1,
    kHelloAck = 2,
    kData = 3,
    kPing = 4,
    kPong = 5,
    kQosProbe = 6,
    kQosReply = 7,
    kBye = 8,
    kControl = 9,
  };
  using Clock = std::chrono::steady_clock;

  struct Peer {
    uint64_t id = 0;
    Endpoint endpoint;  // known when it sent us a frame itself (not forwarded)
    uint32_t status = kConnectIdle;
    Clock::time_point heard{};
    Clock::time_point connect_started{};
    Clock::time_point pinged{};
    std::string name;
  };
  struct QosListenEntry {
    bool enabled = false;
    std::vector<uint8_t> data;
  };
  struct QosTarget {
    uint64_t peer = 0;
    std::array<uint8_t, kXnKidSize> xnkid{};
    uint32_t sent = 0;
    std::vector<uint32_t> rtts;
    bool answered = false;
    // The target's answer: 0 an unknown session, 1 listening, 2 listening disabled, 3 the key
    // registered without listening.
    uint8_t listening = 0;
    std::vector<uint8_t> data;
    bool done = false;
  };
  struct QosLookupState {
    uint32_t id = 0;
    uint32_t probes = 0;
    std::vector<QosTarget> targets;
    QosDone done;
    Clock::time_point started{};
    Clock::time_point last_probe{};
  };

  void Run();
  void Tick();
  void OnFrame(const Endpoint& from, const uint8_t* data, size_t size);
  void SendFrame(uint64_t to, FrameType type, const uint8_t* payload, size_t size);
  void SendFrameTo(const Endpoint& to, uint64_t dst, FrameType type, const uint8_t* payload,
                   size_t size);
  using KeyId = std::array<uint8_t, kXnKidSize>;
  // A security association: a peer in a session.
  struct Association {
    uint64_t peer = 0;
    KeyId xnkid{};
  };
  Peer& PeerFor(uint64_t id);
  // The association's IN_ADDR here, made the first time.
  uint32_t InAddrFor(uint64_t peer, const KeyId& xnkid);
  const Association* AssociationByInAddr(uint32_t in_addr) const;
  void FinishQosTarget(QosLookupState& lookup, size_t index);
  void SetStatus(Peer& peer, uint32_t status);
  std::vector<uint8_t> HelloPayload() const;
  void RunCallbacks();

  const uint64_t self_;
  const uint32_t title_id_;
  const uint32_t self_in_addr_;
  std::unique_ptr<Transport> transport_;
  std::thread thread_;
  std::atomic<bool> running_ = false;

  mutable std::mutex mutex_;
  Endpoint hub_endpoint_;
  uint64_t hub_ = 0;
  Clock::time_point hello_sent_{};
  std::map<uint64_t, Peer> peers_;
  std::map<uint32_t, Association> in_addrs_;
  std::map<std::pair<uint64_t, KeyId>, uint32_t> association_in_addrs_;
  std::map<uint16_t, PacketSink*> sockets_;
  std::map<std::array<uint8_t, kXnKidSize>, std::array<uint8_t, kXnKeySize>> keys_;
  std::map<std::array<uint8_t, kXnKidSize>, QosListenEntry> qos_listen_;
  std::vector<QosLookupState> qos_lookups_;
  // Callbacks to run once the lock is released (QoS results, control messages, peer events).
  std::vector<std::function<void()>> pending_callbacks_;
  std::string self_name_;
  std::map<uint8_t, ControlHandler> control_handlers_;
  PeerHandler peer_handler_;
  uint32_t next_qos_id_ = 1;
  uint16_t next_port_ = 49152;
  uint32_t ping_seq_ = 0;
  uint64_t frames_forwarded_ = 0;
  uint64_t frames_dropped_ = 0;
};

// The UDP transport: one real socket on `port` (0: any free port).
std::unique_ptr<Transport> CreateUdpTransport(uint16_t port, uint16_t* bound_port);

}  // namespace rex::system::online
