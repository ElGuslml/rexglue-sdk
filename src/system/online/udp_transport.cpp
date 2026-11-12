/**
 * ReXGlue online play: the UDP transport, one real socket for every frame of the virtual network
 * (virtual_net.h).
 */

#include <rex/system/online/virtual_net.h>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::system::online {

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

class UdpTransport : public Transport {
 public:
  ~UdpTransport() override {
    if (socket_ != kNoSocket) {
      CloseNative(socket_);
    }
#if REX_PLATFORM_WIN32
    if (wsa_started_) {
      WSACleanup();
    }
#endif
  }

  bool Open(uint16_t port, uint16_t* bound_port) {
#if REX_PLATFORM_WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return false;
    }
    wsa_started_ = true;
#endif
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kNoSocket) {
      return false;
    }
#if REX_PLATFORM_WIN32
    // An ICMP "port unreachable" would make the next recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket_, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr,
             nullptr);
    u_long nonblocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonblocking);
#else
    fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK);
#endif
    int buffer = 1 << 20;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer),
               sizeof(buffer));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer),
               sizeof(buffer));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      return false;
    }
    socklen_t length = sizeof(addr);
    getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &length);
    *bound_port = ntohs(addr.sin_port);
    return true;
  }

  void SendTo(const Endpoint& to, const uint8_t* data, size_t size) override {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ipv4());
    addr.sin_port = htons(to.port());
    ::sendto(socket_, reinterpret_cast<const char*>(data), int(size), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  }

  void Poll(uint32_t timeout_ms, const Receive& receive) override {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);
    timeval timeout{0, int(timeout_ms * 1000)};
    if (::select(int(socket_) + 1, &read_set, nullptr, nullptr, &timeout) <= 0) {
      return;
    }
    // Every frame waiting, then back to the caller.
    for (int i = 0; i < 256; ++i) {
      sockaddr_in from{};
      socklen_t from_length = sizeof(from);
      int size = ::recvfrom(socket_, reinterpret_cast<char*>(buffer_), sizeof(buffer_), 0,
                            reinterpret_cast<sockaddr*>(&from), &from_length);
      if (size <= 0) {
        break;
      }
      receive(Endpoint::Udp(ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)), buffer_,
              size_t(size));
    }
  }

 private:
  NativeSocket socket_ = kNoSocket;
  bool wsa_started_ = false;
  uint8_t buffer_[2048];
};

}  // namespace

std::unique_ptr<Transport> CreateUdpTransport(uint16_t port, uint16_t* bound_port) {
  auto transport = std::make_unique<UdpTransport>();
  if (!transport->Open(port, bound_port)) {
    REXSYS_ERROR("online: could not open UDP port {}", port);
    return nullptr;
  }
  return transport;
}

}  // namespace rex::system::online
