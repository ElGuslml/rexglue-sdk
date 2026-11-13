/**
 * The host's port opened on its router by UPnP (rex/system/online/upnp.h), against a fake router:
 * an HTTP server on this PC's loopback answering as an Internet Gateway Device.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/system/online/upnp.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace rex::system::online::upnp;

namespace {

#if REX_PLATFORM_WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
void CloseNative(NativeSocket s) {
  closesocket(s);
}
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
void CloseNative(NativeSocket s) {
  close(s);
}
#endif

std::string Between(const std::string& text, const std::string& open, const std::string& close) {
  const size_t a = text.find(open);
  if (a == std::string::npos) {
    return {};
  }
  const size_t b = text.find(close, a + open.size());
  return b == std::string::npos ? std::string() : text.substr(a + open.size(), b - a - open.size());
}

// A router for the tests: its description (the WAN connection's control URL relative to it, a PPP
// connection listed first, the body chunked) and the WAN IP connection's actions.
class FakeRouter {
 public:
  std::string external = "203.0.113.7";
  bool only_permanent = false;  // 725 for a lease that is not 0
  bool conflict = false;        // 718

  struct Mapping {
    std::string client;
    uint32_t lease;
    std::string description;
  };

  FakeRouter() {
#if REX_PLATFORM_WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t size = sizeof(addr);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &size);
    port_ = ntohs(addr.sin_port);
    ::listen(listener_, 8);
    thread_ = std::thread([this] { Serve(); });
  }
  ~FakeRouter() {
    stop_ = true;
    thread_.join();
    CloseNative(listener_);
  }

  std::string location() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/igd/rootDesc.xml";
  }
  std::map<std::string, Mapping> mappings() {
    std::lock_guard lock(mutex_);
    return mappings_;
  }
  std::vector<std::string> actions() {
    std::lock_guard lock(mutex_);
    return actions_;
  }

 private:
  void Serve() {
    while (!stop_) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(listener_, &set);
      timeval timeout{0, 50000};
      if (::select(int(listener_) + 1, &set, nullptr, nullptr, &timeout) <= 0) {
        continue;
      }
      NativeSocket client = ::accept(listener_, nullptr, nullptr);
      if (client == kNoSocket) {
        continue;
      }
      std::string request;
      char buffer[4096];
      while (true) {
        const int n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
          break;
        }
        request.append(buffer, size_t(n));
        const size_t end = request.find("\r\n\r\n");
        if (end != std::string::npos) {
          const std::string length = Between(request, "Content-Length: ", "\r\n");
          if (request.size() >= end + 4 + size_t(std::atoi(length.c_str()))) {
            break;
          }
        }
      }
      const std::string reply = Answer(request);
      ::send(client, reply.data(), int(reply.size()), 0);
      CloseNative(client);
    }
  }

  static std::string Reply(int status, const std::string& body, bool chunked = false) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Internal Server Error") +
                       "\r\nContent-Type: text/xml\r\nConnection: close\r\n";
    if (!chunked) {
      return head + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    const size_t half = body.size() / 2;
    auto chunk = [](const std::string& part) {
      char size[16];
      std::snprintf(size, sizeof(size), "%zx", part.size());
      return std::string(size) + "\r\n" + part + "\r\n";
    };
    return head + "Transfer-Encoding: chunked\r\n\r\n" + chunk(body.substr(0, half)) + chunk(body.substr(half)) +
           "0\r\n\r\n";
  }
  static std::string Fault(int code) {
    return Reply(500, "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>" + std::to_string(code) +
                          "</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
  }

  std::string Answer(const std::string& request) {
    if (request.rfind("GET /igd/rootDesc.xml", 0) == 0) {
      return Reply(200,
                   "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                   "<friendlyName>Test Gateway</friendlyName><deviceList><device><deviceList><device>"
                   "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1"
                   "</serviceType><controlURL>/ctl/PPPConn</controlURL></service>"
                   "<service>\n  <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
                   "  <controlURL>ctl/IPConn</controlURL>\n</service></serviceList>"
                   "</device></deviceList></device></deviceList></device></root>",
                   true);
    }
    if (request.rfind("POST /igd/ctl/IPConn", 0) != 0) {
      return Reply(404, "");
    }
    const std::string action = Between(request, "#", "\"");
    std::lock_guard lock(mutex_);
    actions_.push_back(action);
    if (action == "GetExternalIPAddress") {
      return Reply(200,
                   "<s:Envelope><s:Body><u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:"
                   "service:WANIPConnection:1\"><NewExternalIPAddress>" +
                       external + "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>");
    }
    const std::string key = Between(request, "<NewProtocol>", "<") + " " + Between(request, "<NewExternalPort>", "<");
    if (action == "AddPortMapping") {
      const uint32_t lease = uint32_t(std::atoi(Between(request, "<NewLeaseDuration>", "<").c_str()));
      if (conflict) {
        return Fault(718);
      }
      if (only_permanent && lease != 0) {
        return Fault(725);
      }
      mappings_[key] = {Between(request, "<NewInternalClient>", "<") + ":" +
                            Between(request, "<NewInternalPort>", "<"),
                        lease, Between(request, "<NewPortMappingDescription>", "<")};
      return Reply(200, "<s:Envelope><s:Body><u:AddPortMappingResponse/></s:Body></s:Envelope>");
    }
    if (action == "DeletePortMapping") {
      if (!mappings_.erase(key)) {
        return Fault(714);
      }
      return Reply(200, "<s:Envelope><s:Body><u:DeletePortMappingResponse/></s:Body></s:Envelope>");
    }
    return Fault(401);
  }

  NativeSocket listener_ = kNoSocket;
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::map<std::string, Mapping> mappings_;
  std::vector<std::string> actions_;
};

// The mapping's state once it is no longer searching (5 s at most).
PortMapping::State Settled(const PortMapping& mapping) {
  for (int i = 0; i < 500 && mapping.state() == PortMapping::State::kSearching; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return mapping.state();
}

}  // namespace

TEST_CASE("UPnP: private addresses, masked for the log", "[online][upnp]") {
  CHECK(IsPrivate(0x0A141E28));   // 10.20.30.40
  CHECK(IsPrivate(0xC0A80101));   // 192.168.1.1
  CHECK(IsPrivate(0xAC100001));   // 172.16.0.1
  CHECK(IsPrivate(0x64460102));   // 100.70.1.2, carrier-grade NAT
  CHECK(IsPrivate(0x7F000001));   // 127.0.0.1
  CHECK_FALSE(IsPrivate(0xCB007107));  // 203.0.113.7
  CHECK_FALSE(IsPrivate(0xAC200001));  // 172.32.0.1
  CHECK_FALSE(IsPrivate(0x64800001));  // 100.128.0.1
  CHECK(Masked(0xCB007107) == "203.x.x.x");
}

TEST_CASE("UPnP: a gateway's description and its WAN connection", "[online][upnp]") {
  FakeRouter router;
  const auto gateway = ReadGateway(router.location());
  REQUIRE(gateway);
  CHECK(gateway->name == "Test Gateway");
  CHECK(gateway->service_type == "urn:schemas-upnp-org:service:WANIPConnection:1");
  CHECK(gateway->control_url == router.location().substr(0, router.location().rfind('/') + 1) + "ctl/IPConn");
  const auto external = ExternalAddress(*gateway);
  REQUIRE(external);
  CHECK(*external == 0xCB007107);
  CHECK(LocalAddressTowards(*gateway) == 0x7F000001);
}

TEST_CASE("UPnP: the host's port mapped while it hosts, removed when it closes", "[online][upnp]") {
  FakeRouter router;
  PortMapping mapping(3074, "Game online play", router.location());
  REQUIRE(Settled(mapping) == PortMapping::State::kMapped);
  CHECK(mapping.lease() == PortMapping::kLeaseSeconds);
  auto mappings = router.mappings();
  REQUIRE(mappings.count("UDP 3074") == 1);
  CHECK(mappings["UDP 3074"].client == "127.0.0.1:3074");
  CHECK(mappings["UDP 3074"].lease == PortMapping::kLeaseSeconds);
  CHECK(mappings["UDP 3074"].description == "Game online play");
  mapping.Release();
  CHECK(mapping.state() == PortMapping::State::kReleased);
  CHECK(router.mappings().empty());
  mapping.Release();  // twice: nothing more
  CHECK(router.actions().back() == "DeletePortMapping");
}

TEST_CASE("UPnP: a router taking only permanent leases gets one", "[online][upnp]") {
  FakeRouter router;
  router.only_permanent = true;
  {
    PortMapping mapping(3074, "Game online play", router.location());
    REQUIRE(Settled(mapping) == PortMapping::State::kMapped);
    CHECK(mapping.lease() == 0);
    CHECK(router.mappings()["UDP 3074"].lease == 0);
  }  // the destructor removes it
  CHECK(router.mappings().empty());
  const auto actions = router.actions();
  CHECK(std::count(actions.begin(), actions.end(), std::string("AddPortMapping")) == 2);
}

TEST_CASE("UPnP: behind carrier-grade NAT nothing is asked of the router", "[online][upnp]") {
  FakeRouter router;
  router.external = "100.70.1.2";
  PortMapping mapping(3074, "Game online play", router.location());
  CHECK(Settled(mapping) == PortMapping::State::kBehindNat);
  mapping.Release();
  for (const auto& action : router.actions()) {
    CHECK(action == "GetExternalIPAddress");
  }
  CHECK(router.mappings().empty());
}

TEST_CASE("UPnP: a port another device has, and no router", "[online][upnp]") {
  {
    FakeRouter router;
    router.conflict = true;
    PortMapping mapping(3074, "Game online play", router.location());
    CHECK(Settled(mapping) == PortMapping::State::kRefused);
    mapping.Release();
    CHECK(router.actions().back() == "AddPortMapping");  // no delete: nothing was mapped
  }
  std::string gone;
  {
    FakeRouter router;
    gone = router.location();
  }
  PortMapping mapping(3074, "Game online play", gone);
  CHECK(Settled(mapping) == PortMapping::State::kNoRouter);
}
