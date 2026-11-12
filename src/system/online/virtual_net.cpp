/**
 * ReXGlue online play: the virtual network under XNet (see virtual_net.h).
 */

#include <rex/system/online/virtual_net.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <fmt/format.h>

#include <rex/logging.h>

namespace rex::system::online {

namespace {

// A frame: "RXNP", version, type, flags, 0, source peer, destination peer (0: whoever receives it,
// for a hello to an address), then the type's payload. Little-endian.
constexpr uint8_t kMagic[4] = {'R', 'X', 'N', 'P'};
constexpr size_t kHeaderSize = 24;
constexpr uint8_t kFlagForwarded = 0x01;

// The XNADDR's mark of one of ours: abOnline begins with it, then the peer id (big-endian).
constexpr uint8_t kXnAddrMagic[4] = {'R', 'X', 'N', 'P'};

// WSA errors the XNet calls return.
constexpr uint32_t kWsaEinval = 10022;

// QoS: the bandwidth reported (nothing measures it; a broadband connection), the time a target has
// to answer, the time between probes.
constexpr uint32_t kQosBitsPerSec = 4000000;
constexpr auto kQosTimeout = std::chrono::milliseconds(2000);
constexpr auto kQosProbeInterval = std::chrono::milliseconds(50);
// Connections: a pending connect pings this often and gives up after kConnectTimeout; a connected
// peer is pinged every kKeepAlive and lost after kLostAfter without a frame.
constexpr auto kConnectPing = std::chrono::milliseconds(250);
constexpr auto kConnectTimeout = std::chrono::seconds(10);
constexpr auto kKeepAlive = std::chrono::seconds(2);
constexpr auto kLostAfter = std::chrono::seconds(15);
constexpr auto kHelloInterval = std::chrono::seconds(1);

void Put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  Put16(out, uint16_t(v));
  Put16(out, uint16_t(v >> 16));
}
void Put64(std::vector<uint8_t>& out, uint64_t v) {
  Put32(out, uint32_t(v));
  Put32(out, uint32_t(v >> 32));
}

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at = 0;
  bool ok = true;
  bool Has(size_t n) {
    if (at + n > size) {
      ok = false;
      return false;
    }
    return true;
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    if (!Has(2)) return 0;
    uint16_t v = uint16_t(data[at] | data[at + 1] << 8);
    at += 2;
    return v;
  }
  uint32_t U32() {
    uint32_t lo = U16();
    return lo | uint32_t(U16()) << 16;
  }
  uint64_t U64() {
    uint64_t lo = U32();
    return lo | uint64_t(U32()) << 32;
  }
  const uint8_t* Bytes(size_t n) {
    if (!Has(n)) return nullptr;
    const uint8_t* p = data + at;
    at += n;
    return p;
  }
  size_t Left() const { return at <= size ? size - at : 0; }
};

uint64_t Mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return x;
}

// A peer's virtual IN_ADDR: 10.x.y.z from its id (the same on every PC while it is free), never .0
// or .255.
uint32_t HashInAddr(uint64_t id, uint32_t salt) {
  uint32_t host = uint32_t(Mix(id + salt * 0x9E3779B97F4A7C15ull)) & 0x00FFFFFF;
  uint8_t last = uint8_t(host);
  if (last == 0 || last == 255) {
    host ^= 0x01;
  }
  return 0x0A000000 | host;
}

void MakeXnAddr(uint64_t id, uint32_t in_addr, XNADDR* addr) {
  std::memset(addr, 0, sizeof(*addr));
  addr->ina = in_addr;
  addr->inaOnline = in_addr;
  addr->wPortOnline = VirtualNet::kXnAddrPort;
  uint64_t mac = Mix(id);
  addr->abEnet[0] = 0x02;  // locally administered
  for (int i = 1; i < 6; ++i) {
    addr->abEnet[i] = uint8_t(mac >> (8 * i));
  }
  std::memcpy(addr->abOnline, kXnAddrMagic, 4);
  for (int i = 0; i < 8; ++i) {
    addr->abOnline[4 + i] = uint8_t(id >> (56 - 8 * i));
  }
}

// The peer id an XNADDR of ours carries, 0 if it is not one.
uint64_t XnAddrPeer(const XNADDR& addr) {
  if (std::memcmp(addr.abOnline, kXnAddrMagic, 4) != 0) {
    return 0;
  }
  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) {
    id = id << 8 | addr.abOnline[4 + i];
  }
  return id;
}

std::string Hex(const uint8_t* data, size_t size) {
  std::string text;
  for (size_t i = 0; i < size; ++i) {
    text += fmt::format("{:02X}", data[i]);
  }
  return text;
}

}  // namespace

std::string Endpoint::ToString() const {
  uint32_t ip = ipv4();
  return fmt::format("{}.{}.{}.{}:{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                     port());
}

Endpoint Endpoint::Parse(const std::string& text) {
  unsigned a, b, c, d, port;
  char tail;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail) != 5 || a > 255 ||
      b > 255 || c > 255 || d > 255 || port == 0 || port > 65535) {
    return {};
  }
  return Udp(a << 24 | b << 16 | c << 8 | d, uint16_t(port));
}

VirtualNet::VirtualNet(uint64_t self, uint32_t title_id, std::unique_ptr<Transport> transport)
    : self_(self),
      title_id_(title_id),
      self_in_addr_(HashInAddr(self, 0)),
      transport_(std::move(transport)) {}

VirtualNet::~VirtualNet() {
  Stop();
}

void VirtualNet::Start(const Endpoint& hub) {
  if (running_) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    hub_endpoint_ = hub;
  }
  running_ = true;
  thread_ = std::thread([this] { Run(); });
}

void VirtualNet::Stop() {
  if (!running_) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, peer] : peers_) {
      if (peer.endpoint.valid()) {
        SendFrameTo(peer.endpoint, id, FrameType::kBye, nullptr, 0);
      }
    }
  }
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void VirtualNet::Run() {
  while (running_) {
    transport_->Poll(10, [this](const Endpoint& from, const uint8_t* data, size_t size) {
      OnFrame(from, data, size);
    });
    Tick();
  }
}

uint32_t VirtualNet::InAddrFor(uint64_t id) const {
  for (uint32_t salt = 0;; ++salt) {
    uint32_t in_addr = HashInAddr(id, salt);
    auto it = in_addrs_.find(in_addr);
    if (in_addr != self_in_addr_ && (it == in_addrs_.end() || it->second == id)) {
      return in_addr;
    }
  }
}

VirtualNet::Peer& VirtualNet::PeerFor(uint64_t id) {
  auto it = peers_.find(id);
  if (it != peers_.end()) {
    return it->second;
  }
  Peer& peer = peers_[id];
  peer.id = id;
  peer.in_addr = InAddrFor(id);
  in_addrs_[peer.in_addr] = id;
  REXSYS_DEBUG("online: peer {:016X} is {}.{}.{}.{} here", id, peer.in_addr >> 24,
               (peer.in_addr >> 16) & 0xFF, (peer.in_addr >> 8) & 0xFF, peer.in_addr & 0xFF);
  return peer;
}

VirtualNet::Peer* VirtualNet::PeerByInAddr(uint32_t in_addr) {
  auto it = in_addrs_.find(in_addr);
  if (it == in_addrs_.end()) {
    return nullptr;
  }
  return &peers_[it->second];
}

void VirtualNet::SendFrameTo(const Endpoint& to, uint64_t dst, FrameType type,
                             const uint8_t* payload, size_t size) {
  std::vector<uint8_t> frame;
  frame.reserve(kHeaderSize + size);
  frame.insert(frame.end(), kMagic, kMagic + 4);
  frame.push_back(uint8_t(kProtocolVersion));
  frame.push_back(uint8_t(type));
  frame.push_back(0);
  frame.push_back(0);
  Put64(frame, self_);
  Put64(frame, dst);
  if (size) {
    frame.insert(frame.end(), payload, payload + size);
  }
  transport_->SendTo(to, frame.data(), frame.size());
}

void VirtualNet::SendFrame(uint64_t to, FrameType type, const uint8_t* payload, size_t size) {
  auto it = peers_.find(to);
  if (it != peers_.end() && it->second.endpoint.valid()) {
    SendFrameTo(it->second.endpoint, to, type, payload, size);
  } else if (hub_endpoint_.valid()) {
    // Through the hub, which forwards it.
    SendFrameTo(hub_endpoint_, to, type, payload, size);
  } else {
    ++frames_dropped_;
  }
}

void VirtualNet::OnFrame(const Endpoint& from, const uint8_t* data, size_t size) {
  if (size < kHeaderSize || std::memcmp(data, kMagic, 4) != 0) {
    return;
  }
  Reader r{data, size, 4};
  uint8_t version = r.U8();
  auto type = FrameType(r.U8());
  uint8_t flags = r.U8();
  r.U8();
  uint64_t src = r.U64();
  uint64_t dst = r.U64();
  if (version != kProtocolVersion) {
    REXSYS_WARN("online: a frame of protocol {} from {} (this build: {}) ignored", version,
                from.ToString(), kProtocolVersion);
    return;
  }
  if (src == self_ || src == 0) {
    return;
  }
  const auto now = Clock::now();
  std::lock_guard lock(mutex_);

  if (dst != 0 && dst != self_) {
    // For another peer: forward it if we know where it is (the hub).
    if (!(flags & kFlagForwarded)) {
      Peer& sender = PeerFor(src);
      sender.endpoint = from;
      sender.heard = now;
    }
    auto it = peers_.find(dst);
    if (it != peers_.end() && it->second.endpoint.valid() && !(it->second.endpoint == from)) {
      std::vector<uint8_t> copy(data, data + size);
      copy[6] |= kFlagForwarded;
      transport_->SendTo(it->second.endpoint, copy.data(), copy.size());
      ++frames_forwarded_;
    } else {
      ++frames_dropped_;
    }
    return;
  }

  Peer& peer = PeerFor(src);
  if (!(flags & kFlagForwarded)) {
    peer.endpoint = from;
  }
  peer.heard = now;
  if (peer.status != kConnectConnected) {
    REXSYS_INFO("online: peer {:016X} connected{}", src,
                (flags & kFlagForwarded) ? " (through the hub)" : "");
    peer.status = kConnectConnected;
  }

  switch (type) {
    case FrameType::kHello: {
      uint32_t protocol = r.U32();
      uint32_t title = r.U32();
      if (!r.ok || title != title_id_) {
        REXSYS_WARN("online: {} ({:016X}) plays title {:08X}, not this game; refused",
                    from.ToString(), src, title);
        SendFrameTo(from, src, FrameType::kBye, nullptr, 0);
        peer.status = kConnectLost;
        return;
      }
      REXSYS_INFO("online: peer {:016X} joined from {} (protocol {})", src, from.ToString(),
                  protocol);
      std::vector<uint8_t> payload;
      Put32(payload, kProtocolVersion);
      Put32(payload, title_id_);
      SendFrameTo(from, src, FrameType::kHelloAck, payload.data(), payload.size());
      break;
    }
    case FrameType::kHelloAck:
      if (from == hub_endpoint_ && hub_ != src) {
        hub_ = src;
        REXSYS_INFO("online: joined the host {:016X} at {}", src, from.ToString());
      }
      break;
    case FrameType::kData: {
      uint16_t src_port = r.U16();
      uint16_t dst_port = r.U16();
      if (!r.ok) {
        return;
      }
      auto it = sockets_.find(dst_port);
      if (it == sockets_.end()) {
        ++frames_dropped_;
        return;
      }
      it->second->Deliver(peer.in_addr, src_port, data + r.at, r.Left());
      break;
    }
    case FrameType::kPing:
      SendFrame(src, FrameType::kPong, data + r.at, r.Left());
      break;
    case FrameType::kPong:
      break;
    case FrameType::kQosProbe: {
      std::array<uint8_t, kXnKidSize> xnkid{};
      const uint8_t* kid = r.Bytes(kXnKidSize);
      uint32_t seq = r.U32();
      uint64_t t0 = r.U64();
      if (!r.ok) {
        return;
      }
      std::memcpy(xnkid.data(), kid, kXnKidSize);
      std::vector<uint8_t> payload(kid, kid + kXnKidSize);
      Put32(payload, seq);
      Put64(payload, t0);
      auto it = qos_listen_.find(xnkid);
      // 0: not listening for that session; 1: listening; 2: listening disabled.
      uint8_t state = it == qos_listen_.end() ? 0 : it->second.enabled ? 1 : 2;
      payload.push_back(state);
      const std::vector<uint8_t> empty;
      const auto& listen_data = state == 1 ? it->second.data : empty;
      Put16(payload, uint16_t(listen_data.size()));
      payload.insert(payload.end(), listen_data.begin(), listen_data.end());
      SendFrame(src, FrameType::kQosReply, payload.data(), payload.size());
      break;
    }
    case FrameType::kQosReply: {
      const uint8_t* kid = r.Bytes(kXnKidSize);
      uint32_t seq = r.U32();
      uint64_t t0 = r.U64();
      uint8_t state = r.U8();
      uint16_t data_len = r.U16();
      const uint8_t* reply_data = r.Bytes(data_len);
      if (!r.ok) {
        return;
      }
      (void)seq;
      auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch() - std::chrono::microseconds(t0));
      for (auto& lookup : qos_lookups_) {
        for (size_t i = 0; i < lookup.targets.size(); ++i) {
          QosTarget& target = lookup.targets[i];
          if (target.done || target.peer != src ||
              std::memcmp(target.xnkid.data(), kid, kXnKidSize) != 0) {
            continue;
          }
          target.answered = true;
          target.listening = state;
          target.rtts.push_back(uint32_t(std::max<int64_t>(0, rtt.count())));
          target.data.assign(reply_data, reply_data + data_len);
          if (target.rtts.size() >= lookup.probes || state != 1) {
            FinishQosTarget(lookup, i);
          }
        }
      }
      break;
    }
    case FrameType::kBye:
      REXSYS_INFO("online: peer {:016X} left", src);
      peer.status = kConnectLost;
      peer.endpoint = {};
      if (src == hub_) {
        hub_ = 0;
      }
      break;
  }
}

void VirtualNet::FinishQosTarget(QosLookupState& lookup, size_t index) {
  QosTarget& target = lookup.targets[index];
  if (target.done) {
    return;
  }
  target.done = true;
  QosResult result;
  result.flags = kQosComplete;
  result.probes_xmit = uint16_t(target.sent);
  result.probes_recv = uint16_t(target.rtts.size());
  if (target.answered && target.listening != 0) {
    result.flags |= kQosTargetContacted;
    if (target.listening == 2) {
      result.flags |= kQosTargetDisabled;
    } else if (!target.data.empty()) {
      result.flags |= kQosDataReceived;
      result.data = target.data;
    }
    std::vector<uint32_t> rtts = target.rtts;
    std::sort(rtts.begin(), rtts.end());
    result.rtt_min_ms = uint16_t(std::min<uint32_t>(rtts.front(), 0xFFFF));
    result.rtt_med_ms = uint16_t(std::min<uint32_t>(rtts[rtts.size() / 2], 0xFFFF));
    result.up_bits_per_sec = kQosBitsPerSec;
    result.down_bits_per_sec = kQosBitsPerSec;
  }
  REXSYS_DEBUG("online: QoS of {:016X}: flags {:02X}, {} of {} probes, rtt {} ms", target.peer,
               result.flags, result.probes_recv, result.probes_xmit, result.rtt_min_ms);
  auto done = lookup.done;
  // Called after the lock is released (Tick).
  pending_qos_.push_back([done, index, result] { done(index, result); });
}

void VirtualNet::Tick() {
  const auto now = Clock::now();
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard lock(mutex_);
    if (hub_endpoint_.valid() && hub_ == 0 && now - hello_sent_ >= kHelloInterval) {
      hello_sent_ = now;
      std::vector<uint8_t> payload;
      Put32(payload, kProtocolVersion);
      Put32(payload, title_id_);
      SendFrameTo(hub_endpoint_, 0, FrameType::kHello, payload.data(), payload.size());
    }
    for (auto& [id, peer] : peers_) {
      if (peer.status == kConnectPending) {
        if (now - peer.connect_started >= kConnectTimeout) {
          REXSYS_WARN("online: peer {:016X} did not answer; connection lost", id);
          peer.status = kConnectLost;
        } else if (now - peer.pinged >= kConnectPing) {
          peer.pinged = now;
          std::vector<uint8_t> payload;
          Put32(payload, ++ping_seq_);
          SendFrame(id, FrameType::kPing, payload.data(), payload.size());
        }
      } else if (peer.status == kConnectConnected) {
        if (now - peer.heard >= kLostAfter) {
          REXSYS_WARN("online: nothing from peer {:016X} for {} s; connection lost", id,
                      std::chrono::duration_cast<std::chrono::seconds>(kLostAfter).count());
          peer.status = kConnectLost;
        } else if (now - peer.pinged >= kKeepAlive) {
          peer.pinged = now;
          std::vector<uint8_t> payload;
          Put32(payload, ++ping_seq_);
          SendFrame(id, FrameType::kPing, payload.data(), payload.size());
        }
      }
    }
    for (auto& lookup : qos_lookups_) {
      bool probe_round = now - lookup.last_probe >= kQosProbeInterval;
      if (probe_round) {
        lookup.last_probe = now;
      }
      for (size_t i = 0; i < lookup.targets.size(); ++i) {
        QosTarget& target = lookup.targets[i];
        if (target.done) {
          continue;
        }
        if (now - lookup.started >= kQosTimeout) {
          FinishQosTarget(lookup, i);
          continue;
        }
        if (probe_round && target.sent < lookup.probes) {
          std::vector<uint8_t> payload(target.xnkid.begin(), target.xnkid.end());
          Put32(payload, target.sent++);
          Put64(payload, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      now.time_since_epoch())
                                      .count()));
          SendFrame(target.peer, FrameType::kQosProbe, payload.data(), payload.size());
        }
      }
    }
    qos_lookups_.erase(std::remove_if(qos_lookups_.begin(), qos_lookups_.end(),
                                      [](const QosLookupState& lookup) {
                                        return std::all_of(
                                            lookup.targets.begin(), lookup.targets.end(),
                                            [](const QosTarget& t) { return t.done; });
                                      }),
                       qos_lookups_.end());
    callbacks.swap(pending_qos_);
  }
  for (auto& callback : callbacks) {
    callback();
  }
}

void VirtualNet::GetTitleXnAddr(XNADDR* addr) const {
  MakeXnAddr(self_, self_in_addr_, addr);
}

uint32_t VirtualNet::XnAddrToInAddr(const XNADDR& addr, uint32_t* in_addr) {
  uint64_t id = XnAddrPeer(addr);
  if (!id) {
    return kWsaEinval;
  }
  if (id == self_) {
    *in_addr = self_in_addr_;
    return 0;
  }
  std::lock_guard lock(mutex_);
  *in_addr = PeerFor(id).in_addr;
  return 0;
}

uint32_t VirtualNet::InAddrToXnAddr(uint32_t in_addr, XNADDR* addr) {
  if (in_addr == self_in_addr_) {
    MakeXnAddr(self_, self_in_addr_, addr);
    return 0;
  }
  std::lock_guard lock(mutex_);
  Peer* peer = PeerByInAddr(in_addr);
  if (!peer) {
    return kWsaEinval;
  }
  MakeXnAddr(peer->id, peer->in_addr, addr);
  return 0;
}

uint32_t VirtualNet::UnregisterInAddr(uint32_t in_addr) {
  // The peer stays known (its frames keep arriving while the game plays with it).
  return 0;
}

uint32_t VirtualNet::Connect(uint32_t in_addr) {
  if (in_addr == self_in_addr_) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  Peer* peer = PeerByInAddr(in_addr);
  if (!peer) {
    return kWsaEinval;
  }
  if (peer->status != kConnectConnected) {
    peer->status = kConnectPending;
    peer->connect_started = Clock::now();
    peer->pinged = {};
  }
  return 0;
}

uint32_t VirtualNet::GetConnectStatus(uint32_t in_addr) {
  if (in_addr == self_in_addr_) {
    return kConnectConnected;
  }
  std::lock_guard lock(mutex_);
  Peer* peer = PeerByInAddr(in_addr);
  return peer ? peer->status : kConnectIdle;
}

uint32_t VirtualNet::RegisterKey(const uint8_t* xnkid, const uint8_t* xnkey) {
  std::array<uint8_t, kXnKidSize> kid{};
  std::array<uint8_t, kXnKeySize> key{};
  std::memcpy(kid.data(), xnkid, kXnKidSize);
  if (xnkey) {
    std::memcpy(key.data(), xnkey, kXnKeySize);
  }
  std::lock_guard lock(mutex_);
  keys_[kid] = key;
  REXSYS_DEBUG("online: key {} registered", Hex(xnkid, kXnKidSize));
  return 0;
}

uint32_t VirtualNet::UnregisterKey(const uint8_t* xnkid) {
  std::array<uint8_t, kXnKidSize> kid{};
  std::memcpy(kid.data(), xnkid, kXnKidSize);
  std::lock_guard lock(mutex_);
  keys_.erase(kid);
  return 0;
}

uint32_t VirtualNet::QosListen(const uint8_t* xnkid, const uint8_t* data, uint32_t size,
                               uint32_t flags) {
  std::array<uint8_t, kXnKidSize> kid{};
  std::memcpy(kid.data(), xnkid, kXnKidSize);
  std::lock_guard lock(mutex_);
  if (flags & kQosListenRelease) {
    qos_listen_.erase(kid);
    return 0;
  }
  QosListenEntry& entry = qos_listen_[kid];
  if (flags & kQosListenEnable) {
    entry.enabled = true;
  }
  if (flags & kQosListenDisable) {
    entry.enabled = false;
  }
  if ((flags & kQosListenSetData) && data) {
    entry.data.assign(data, data + size);
  }
  REXSYS_DEBUG("online: QoS listen {} flags {:02X}: {}, {} bytes", Hex(xnkid, kXnKidSize), flags,
               entry.enabled ? "on" : "off", entry.data.size());
  return 0;
}

void VirtualNet::QosLookup(
    const std::vector<std::pair<XNADDR, std::array<uint8_t, kXnKidSize>>>& targets,
    uint32_t probes, QosDone done) {
  QosLookupState lookup;
  lookup.id = next_qos_id_++;
  lookup.probes = std::clamp<uint32_t>(probes, 1, 8);
  lookup.done = std::move(done);
  lookup.started = Clock::now();
  for (const auto& [addr, xnkid] : targets) {
    QosTarget target;
    target.peer = XnAddrPeer(addr);
    target.xnkid = xnkid;
    lookup.targets.push_back(target);
  }
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < lookup.targets.size(); ++i) {
    if (!lookup.targets[i].peer || lookup.targets[i].peer == self_) {
      FinishQosTarget(lookup, i);
    }
  }
  qos_lookups_.push_back(std::move(lookup));
}

bool VirtualNet::Bind(PacketSink* sink, uint16_t* port) {
  std::lock_guard lock(mutex_);
  if (*port == 0) {
    for (uint32_t tries = 0; tries < 16384; ++tries) {
      uint16_t candidate = next_port_;
      next_port_ = next_port_ == 65535 ? 49152 : next_port_ + 1;
      if (!sockets_.count(candidate)) {
        *port = candidate;
        break;
      }
    }
    if (*port == 0) {
      return false;
    }
  } else if (sockets_.count(*port)) {
    return false;
  }
  sockets_[*port] = sink;
  REXSYS_DEBUG("online: a socket bound to virtual port {}", *port);
  return true;
}

void VirtualNet::Unbind(PacketSink* sink, uint16_t port) {
  std::lock_guard lock(mutex_);
  auto it = sockets_.find(port);
  if (it != sockets_.end() && it->second == sink) {
    sockets_.erase(it);
  }
}

int VirtualNet::SendTo(uint16_t src_port, uint32_t dst_in_addr, uint16_t dst_port,
                       const uint8_t* data, size_t size) {
  if (size > kMaxDatagram) {
    return -1;
  }
  std::lock_guard lock(mutex_);
  if (dst_in_addr == self_in_addr_ || dst_in_addr == 0x7F000001) {
    auto it = sockets_.find(dst_port);
    if (it != sockets_.end()) {
      it->second->Deliver(self_in_addr_, src_port, data, size);
    }
    return int(size);
  }
  Peer* peer = PeerByInAddr(dst_in_addr);
  if (!peer) {
    ++frames_dropped_;
    return -1;
  }
  std::vector<uint8_t> payload;
  payload.reserve(4 + size);
  Put16(payload, src_port);
  Put16(payload, dst_port);
  payload.insert(payload.end(), data, data + size);
  SendFrame(peer->id, FrameType::kData, payload.data(), payload.size());
  return int(size);
}

bool VirtualNet::KnowsPeer(uint64_t peer) const {
  std::lock_guard lock(mutex_);
  auto it = peers_.find(peer);
  return it != peers_.end() && it->second.status == kConnectConnected;
}

uint64_t VirtualNet::hub() const {
  std::lock_guard lock(mutex_);
  return hub_;
}

}  // namespace rex::system::online
