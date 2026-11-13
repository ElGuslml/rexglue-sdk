/**
 * ReXGlue online play: the host's port opened on its router by UPnP (online_upnp).
 *
 * Friends over the internet reach the hosting PC's UDP port (online_port) only when its router
 * forwards it. A router with UPnP (an Internet Gateway Device, found by SSDP on the LAN) is asked to
 * forward it while the game runs: a mapping with a lease, renewed at half of it, deleted when the
 * game closes (and gone by itself when the lease ends after a crash). When the router's own internet
 * address is private (the internet provider's carrier-grade NAT, or a second router in front of it),
 * no mapping can make the PC reachable from the internet: that is said in the log, and nothing is
 * asked of the router.
 *
 * Only addresses inside the LAN are contacted. The router's internet address is never logged in
 * full (players post their logs).
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rex::system::online::upnp {

// An Internet gateway's WAN connection service, from its description.
struct Gateway {
  std::string control_url;   // "http://a.b.c.d:port/path"
  std::string service_type;  // "urn:schemas-upnp-org:service:WANIPConnection:1" (or :2, or WANPPP)
  std::string name;          // its friendlyName
};

// The SSDP search on the LAN: the description URLs of the Internet gateways answering within
// timeout_ms (stop, when given and set, ends the wait early).
std::vector<std::string> Discover(uint32_t timeout_ms, const std::atomic<bool>* stop = nullptr);
// A gateway's description read: its WAN IP (else PPP) connection service.
std::optional<Gateway> ReadGateway(const std::string& location);
// The router's own internet address (host order), as it reports it.
std::optional<uint32_t> ExternalAddress(const Gateway& gateway);
// The address this PC reaches the router from (host order); 0 if none.
uint32_t LocalAddressTowards(const Gateway& gateway);
// AddPortMapping, UDP: the router's port to local:port. 0 when done, the router's UPnP error code
// when refused (718: the port goes to another device already; 725: only permanent leases), -1 when
// it did not answer.
int AddPortMapping(const Gateway& gateway, uint16_t port, uint32_t local, uint32_t lease_seconds,
                   const std::string& description);
// DeletePortMapping, UDP: as AddPortMapping's result.
int DeletePortMapping(const Gateway& gateway, uint16_t port);
// 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier-grade NAT), 169.254/16, 127/8.
bool IsPrivate(uint32_t address);
// "a.x.x.x": an address's first number only, for the log.
std::string Masked(uint32_t address);

// The host's mapping: the router found, the port mapped and renewed on a thread of its own; deleted
// by Release or the destructor.
class PortMapping {
 public:
  enum class State {
    kSearching,
    kNoRouter,    // no gateway answered (UPnP off on the router, or none)
    kBehindNat,   // the router's internet address is private: nothing mapped
    kMapped,
    kRefused,     // the router answered with an error (or not at all) to AddPortMapping
    kReleased,
  };
  static constexpr uint32_t kLeaseSeconds = 3600;

  // location: a gateway's description URL, to skip the SSDP search (tests).
  PortMapping(uint16_t port, std::string description, std::string location = {});
  ~PortMapping();
  PortMapping(const PortMapping&) = delete;
  PortMapping& operator=(const PortMapping&) = delete;

  State state() const { return state_.load(); }
  // The lease the router took (0: a permanent one, the router's only kind), once mapped.
  uint32_t lease() const { return lease_.load(); }
  // Stops the thread and deletes the mapping (the game closing). Bounded by the router's answer
  // (a few seconds at most).
  void Release();

 private:
  void Run();
  bool Wait(uint32_t seconds);  // false when stopped

  uint16_t port_;
  std::string description_;
  std::string location_;
  std::atomic<State> state_{State::kSearching};
  std::atomic<uint32_t> lease_{0};
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  Gateway gateway_;
  uint32_t local_ = 0;
  std::thread thread_;
};

}  // namespace rex::system::online::upnp
