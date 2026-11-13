/**
 * ReXGlue online play: the host's port opened on its router by UPnP (rex/system/online/upnp.h).
 */

#include <rex/system/online/upnp.h>

#include <rex/logging.h>
#include <rex/platform.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::system::online::upnp {

namespace {

#if REX_PLATFORM_WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
void CloseNative(NativeSocket s) {
  closesocket(s);
}
void SetNonBlocking(NativeSocket s, bool on) {
  u_long value = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &value);
}
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
void CloseNative(NativeSocket s) {
  close(s);
}
void SetNonBlocking(NativeSocket s, bool on) {
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

// Winsock for this file's sockets (counted: the UDP transport has its own).
void StartSockets() {
#if REX_PLATFORM_WIN32
  static const bool started = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)started;
#endif
}

constexpr uint32_t kHttpTimeoutMs = 2500;

// Whether the socket becomes readable (or writable) within timeout_ms.
bool WaitSocket(NativeSocket s, bool write, uint32_t timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval timeout{int(timeout_ms / 1000), int((timeout_ms % 1000) * 1000)};
  return ::select(int(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout) > 0;
}

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return text;
}

// "http://host[:port]/path" split; false if it is not one.
bool SplitUrl(const std::string& url, std::string* host, uint16_t* port, std::string* path) {
  const std::string scheme = "http://";
  if (Lower(url.substr(0, scheme.size())) != scheme) {
    return false;
  }
  const size_t slash = url.find('/', scheme.size());
  const std::string authority =
      url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
  *path = slash == std::string::npos ? "/" : url.substr(slash);
  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos) {
    *host = authority;
    *port = 80;
  } else {
    *host = authority.substr(0, colon);
    *port = uint16_t(std::atoi(authority.c_str() + colon + 1));
  }
  return !host->empty() && *port != 0;
}

// The host part of an http URL as an IPv4 address (host order); 0 if it is not one.
uint32_t UrlAddress(const std::string& url) {
  std::string host, path;
  uint16_t port = 0;
  if (!SplitUrl(url, &host, &port, &path)) {
    return 0;
  }
  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
    return 0;
  }
  return ntohl(addr.s_addr);
}

// A chunked body put together.
std::string Dechunk(const std::string& body) {
  std::string out;
  size_t at = 0;
  while (at < body.size()) {
    const size_t line_end = body.find("\r\n", at);
    if (line_end == std::string::npos) {
      break;
    }
    const size_t size = std::strtoul(body.substr(at, line_end - at).c_str(), nullptr, 16);
    if (size == 0) {
      break;
    }
    out += body.substr(line_end + 2, size);
    at = line_end + 2 + size + 2;
  }
  return out;
}

struct HttpResponse {
  int status = 0;
  std::string body;
};

// One HTTP/1.1 request to a LAN address, "Connection: close"; nullopt when it did not answer.
std::optional<HttpResponse> Http(const std::string& url, const std::string& method,
                                 const std::string& extra_headers, const std::string& body) {
  std::string host, path;
  uint16_t port = 0;
  if (!SplitUrl(url, &host, &port, &path)) {
    return std::nullopt;
  }
  StartSockets();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
    return std::nullopt;
  }
  sockaddr_in to{};
  std::memcpy(&to, found->ai_addr, sizeof(to));
  freeaddrinfo(found);

  NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) {
    return std::nullopt;
  }
  SetNonBlocking(s, true);
  ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  if (!WaitSocket(s, true, kHttpTimeoutMs)) {
    CloseNative(s);
    return std::nullopt;
  }
  int error = 0;
  socklen_t error_size = sizeof(error);
  getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_size);
  if (error != 0) {
    CloseNative(s);
    return std::nullopt;
  }
  SetNonBlocking(s, false);

  std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                        "\r\n" + extra_headers + "Content-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < request.size()) {
    if (!WaitSocket(s, true, kHttpTimeoutMs)) {
      CloseNative(s);
      return std::nullopt;
    }
    const int n = ::send(s, request.data() + sent, int(request.size() - sent), 0);
    if (n <= 0) {
      CloseNative(s);
      return std::nullopt;
    }
    sent += size_t(n);
  }
  std::string reply;
  char buffer[4096];
  while (reply.size() < (1u << 20) && WaitSocket(s, false, kHttpTimeoutMs)) {
    const int n = ::recv(s, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }
    reply.append(buffer, size_t(n));
  }
  CloseNative(s);

  const size_t header_end = reply.find("\r\n\r\n");
  if (reply.compare(0, 5, "HTTP/") != 0 || header_end == std::string::npos) {
    return std::nullopt;
  }
  HttpResponse response;
  const size_t space = reply.find(' ');
  response.status = std::atoi(reply.c_str() + space + 1);
  const std::string headers = Lower(reply.substr(0, header_end));
  response.body = reply.substr(header_end + 4);
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    response.body = Dechunk(response.body);
  }
  return response;
}

// The text of the first <tag>...</tag> (or <prefix:tag>) at or after from; empty if none.
std::string Element(const std::string& xml, const std::string& tag, size_t from = 0) {
  size_t at = from;
  while ((at = xml.find(tag, at)) != std::string::npos) {
    const size_t after = at + tag.size();
    size_t open = at;  // back over a namespace prefix to the '<'
    if (open > 0 && xml[open - 1] == ':') {
      --open;
      while (open > 0 && (std::isalnum(uint8_t(xml[open - 1])) || xml[open - 1] == '_' ||
                          xml[open - 1] == '-')) {
        --open;
      }
    }
    if (open > 0 && xml[open - 1] == '<' && after < xml.size() &&
        (xml[after] == '>' || xml[after] == ' ' || xml[after] == '\t')) {
      const size_t text = xml.find('>', after);
      const size_t close = text == std::string::npos ? text : xml.find("</", text);
      if (close == std::string::npos) {
        return {};
      }
      return xml.substr(text + 1, close - text - 1);
    }
    at = after;
  }
  return {};
}

std::string Trim(std::string text) {
  while (!text.empty() && std::isspace(uint8_t(text.back()))) {
    text.pop_back();
  }
  size_t start = 0;
  while (start < text.size() && std::isspace(uint8_t(text[start]))) {
    ++start;
  }
  return text.substr(start);
}

// A SOAP action on the gateway's service: the response, or nullopt when it did not answer.
std::optional<HttpResponse> Soap(const Gateway& gateway, const std::string& action,
                                 const std::string& arguments) {
  const std::string body =
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
      action + " xmlns:u=\"" + gateway.service_type + "\">" + arguments + "</u:" + action +
      "></s:Body></s:Envelope>\r\n";
  const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" +
                              gateway.service_type + "#" + action + "\"\r\n";
  return Http(gateway.control_url, "POST", headers, body);
}

// A SOAP response's result: 0, the UPnP error code, or -1.
int SoapResult(const std::optional<HttpResponse>& response) {
  if (!response) {
    return -1;
  }
  if (response->status == 200) {
    return 0;
  }
  const std::string code = Element(response->body, "errorCode");
  return code.empty() ? -1 : std::atoi(code.c_str());
}

std::string Dotted(uint32_t address) {
  return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 0xFF) + "." +
         std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

std::string EscapeXml(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

std::vector<std::string> Discover(uint32_t timeout_ms, const std::atomic<bool>* stop) {
  std::vector<std::string> locations;
  StartSockets();
  NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    return locations;
  }
  unsigned char ttl = 2;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
  for (const char* target : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
    const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n") +
                               "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + target + "\r\n\r\n";
    ::sendto(s, search.data(), int(search.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buffer[2048];
  while (!(stop && stop->load())) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) {
      break;
    }
    if (!WaitSocket(s, false, uint32_t(std::min<long long>(left, 200)))) {
      continue;
    }
    const int n = ::recv(s, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
      continue;
    }
    const std::string reply(buffer, size_t(n));
    const std::string lower = Lower(reply);
    const size_t at = lower.find("\r\nlocation:");
    if (at == std::string::npos) {
      continue;
    }
    const size_t value = at + 11;
    const size_t line_end = reply.find("\r\n", value);
    const std::string location = Trim(reply.substr(value, line_end - value));
    // Only routers inside the LAN.
    const uint32_t address = UrlAddress(location);
    if (!address || !IsPrivate(address)) {
      continue;
    }
    bool known = false;
    for (const auto& l : locations) {
      known |= l == location;
    }
    if (!known) {
      locations.push_back(location);
    }
  }
  CloseNative(s);
  return locations;
}

std::optional<Gateway> ReadGateway(const std::string& location) {
  const auto response = Http(location, "GET", "", "");
  if (!response || response->status != 200) {
    return std::nullopt;
  }
  const std::string& xml = response->body;
  Gateway gateway;
  gateway.name = Trim(Element(xml, "friendlyName"));
  std::string base = Trim(Element(xml, "URLBase"));
  if (base.empty()) {
    base = location;
  }
  std::string host, path;
  uint16_t port = 0;
  if (!SplitUrl(base, &host, &port, &path)) {
    return std::nullopt;
  }
  const std::string root = "http://" + host + ":" + std::to_string(port);
  for (const char* kind : {"WANIPConnection", "WANPPPConnection"}) {
    size_t at = 0;
    while ((at = xml.find("<service>", at)) != std::string::npos) {
      const size_t end = xml.find("</service>", at);
      if (end == std::string::npos) {
        break;
      }
      const std::string service = xml.substr(at, end - at);
      at = end;
      const std::string type = Trim(Element(service, "serviceType"));
      const std::string control = Trim(Element(service, "controlURL"));
      if (type.find(kind) == std::string::npos || control.empty()) {
        continue;
      }
      gateway.service_type = type;
      if (Lower(control.substr(0, 7)) == "http://") {
        gateway.control_url = control;
      } else if (control[0] == '/') {
        gateway.control_url = root + control;
      } else {
        const size_t last = path.rfind('/');
        gateway.control_url = root + path.substr(0, last + 1) + control;
      }
      return gateway;
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> ExternalAddress(const Gateway& gateway) {
  const auto response = Soap(gateway, "GetExternalIPAddress", "");
  if (!response || response->status != 200) {
    return std::nullopt;
  }
  const std::string text = Trim(Element(response->body, "NewExternalIPAddress"));
  in_addr addr{};
  if (text.empty() || inet_pton(AF_INET, text.c_str(), &addr) != 1 || addr.s_addr == 0) {
    return std::nullopt;
  }
  return ntohl(addr.s_addr);
}

uint32_t LocalAddressTowards(const Gateway& gateway) {
  std::string host, path;
  uint16_t port = 0;
  const uint32_t router = UrlAddress(gateway.control_url);
  if (!router || !SplitUrl(gateway.control_url, &host, &port, &path)) {
    return 0;
  }
  StartSockets();
  NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    return 0;
  }
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(router);
  to.sin_port = htons(port);
  uint32_t local = 0;
  if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in self{};
    socklen_t size = sizeof(self);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&self), &size) == 0) {
      local = ntohl(self.sin_addr.s_addr);
    }
  }
  CloseNative(s);
  return local;
}

int AddPortMapping(const Gateway& gateway, uint16_t port, uint32_t local, uint32_t lease_seconds,
                   const std::string& description) {
  const std::string arguments =
      "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
      "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
      "</NewInternalPort><NewInternalClient>" + Dotted(local) +
      "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" +
      EscapeXml(description) + "</NewPortMappingDescription><NewLeaseDuration>" +
      std::to_string(lease_seconds) + "</NewLeaseDuration>";
  return SoapResult(Soap(gateway, "AddPortMapping", arguments));
}

int DeletePortMapping(const Gateway& gateway, uint16_t port) {
  const std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
                                std::to_string(port) +
                                "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
  return SoapResult(Soap(gateway, "DeletePortMapping", arguments));
}

bool IsPrivate(uint32_t a) {
  const uint32_t b1 = a >> 24, b2 = (a >> 16) & 0xFF;
  return b1 == 10 || b1 == 127 || (b1 == 172 && b2 >= 16 && b2 <= 31) || (b1 == 192 && b2 == 168) ||
         (b1 == 100 && b2 >= 64 && b2 <= 127) || (b1 == 169 && b2 == 254);
}

std::string Masked(uint32_t address) {
  return std::to_string(address >> 24) + ".x.x.x";
}

PortMapping::PortMapping(uint16_t port, std::string description, std::string location)
    : port_(port), description_(std::move(description)), location_(std::move(location)) {
  thread_ = std::thread([this] { Run(); });
}

PortMapping::~PortMapping() {
  Release();
}

bool PortMapping::Wait(uint32_t seconds) {
  std::unique_lock lock(mutex_);
  return !cv_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stop_.load(); });
}

void PortMapping::Run() {
  std::vector<std::string> locations;
  if (!location_.empty()) {
    locations.push_back(location_);
  } else {
    locations = Discover(3000, &stop_);
  }
  std::optional<Gateway> gateway;
  for (const auto& location : locations) {
    if (stop_) {
      return;
    }
    if ((gateway = ReadGateway(location))) {
      break;
    }
  }
  if (stop_) {
    return;
  }
  if (!gateway) {
    REXSYS_INFO("online: UPnP: no router answered{} (UPnP off on it, or none): friends over the "
                "internet need its UDP port {} forwarded to this PC by hand",
                locations.empty() ? "" : " with a WAN connection", port_);
    state_ = State::kNoRouter;
    return;
  }
  const std::string name = gateway->name.empty() ? std::string("the router") : "\"" + gateway->name + "\"";
  const auto external = ExternalAddress(*gateway);
  if (external && IsPrivate(*external)) {
    REXSYS_WARN("online: UPnP: {}'s internet address is private ({}): the internet provider shares "
                "one address among its customers (carrier-grade NAT), or another router is in front "
                "of it. Friends cannot join this PC from the internet, with the port forwarded or "
                "not: one of them hosts, or Steam (online_steam), or a virtual network (ZeroTier, "
                "Tailscale, Radmin VPN)",
                name, Masked(*external));
    state_ = State::kBehindNat;
    return;
  }
  const uint32_t local = LocalAddressTowards(*gateway);
  if (!local) {
    REXSYS_WARN("online: UPnP: this PC's address towards {} not found; nothing mapped", name);
    state_ = State::kRefused;
    return;
  }
  uint32_t lease = kLeaseSeconds;
  int result = AddPortMapping(*gateway, port_, local, lease, description_);
  if (result == 725) {  // OnlyPermanentLeasesSupported
    lease = 0;
    result = AddPortMapping(*gateway, port_, local, lease, description_);
  }
  if (result != 0) {
    if (result == 718) {
      REXSYS_WARN("online: UPnP: {} already forwards UDP port {} to another device (another PC "
                  "hosting?): friends over the internet reach that one; choose another port "
                  "(online_port) on one of them",
                  name, port_);
    } else {
      REXSYS_WARN("online: UPnP: {} {} the forwarding of UDP port {}{}: friends over the internet "
                  "need it forwarded to this PC by hand",
                  name, result < 0 ? "did not answer" : "refused", port_,
                  result < 0 ? std::string() : " (UPnP error " + std::to_string(result) + ")");
    }
    state_ = State::kRefused;
    return;
  }
  {
    std::lock_guard lock(mutex_);
    gateway_ = *gateway;
    local_ = local;
  }
  lease_ = lease;
  state_ = State::kMapped;
  REXSYS_INFO("online: UPnP: {} forwards UDP port {} to this PC ({}:{}){}; friends over the "
              "internet join its {} address{}",
              name, port_, Dotted(local), port_,
              lease ? ", renewed every " + std::to_string(lease / 120) + " minutes" : std::string(),
              external ? "public" : "internet",
              external ? " (" + Masked(*external) + "), port " + std::to_string(port_) : std::string());
  // Renewed at half the lease while the game hosts (a permanent one needs nothing).
  uint32_t wait = lease / 2;
  while (lease && Wait(wait)) {
    const int renewed = AddPortMapping(*gateway, port_, local, lease, description_);
    if (renewed != 0) {
      REXSYS_WARN("online: UPnP: renewing the forwarding of UDP port {} failed ({}); tried again "
                  "in a minute",
                  port_, renewed);
      wait = 60;
    } else {
      wait = lease / 2;
    }
  }
}

void PortMapping::Release() {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (state_ == State::kMapped) {
    const int result = DeletePortMapping(gateway_, port_);
    if (result == 0) {
      REXSYS_INFO("online: UPnP: UDP port {} no longer forwarded", port_);
    } else {
      REXSYS_WARN("online: UPnP: the forwarding of UDP port {} could not be removed ({}); it ends "
                  "with its lease",
                  port_, result);
    }
    state_ = State::kReleased;
  }
}

}  // namespace rex::system::online::upnp
