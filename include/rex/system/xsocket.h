#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <condition_variable>
#include <cstring>
#include <queue>

#include <rex/math.h>
#include <rex/system/online/virtual_net.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::countof(sa_data));
    return *this;
  }

  uint16_t address_family;
  char sa_data[14];
};

struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;

  // Always big-endian!
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  uint16_t sin_family;
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

class XSocket : public XObject, public online::PacketSink {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  // The SOCKET the game is given for a socket object: its handle less XObject::kHandleBase, a small
  // positive number (4, 8, ...), as a console's XNet numbers its sockets; the socket functions take it
  // back with HandleFor. Kernel handles (0xF8......) are negative as signed numbers, and Demonware 2.3
  // takes a negative socket for a failed socket() (Guitar Hero: Warriors of Rock's bdSocket::create):
  // its network layer never had a socket.
  static constexpr uint32_t GuestNumber(X_HANDLE handle) { return handle - XObject::kHandleBase; }
  // The handle of the socket the game names; anything that is not a socket number (INVALID_SOCKET,
  // 0) gives a handle no object has.
  static constexpr X_HANDLE HandleFor(uint32_t guest_number) {
    return guest_number != 0 && guest_number < (0u - XObject::kHandleBase)
               ? guest_number + XObject::kHandleBase
               : 0;
  }

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(N_XSOCKADDR* name, int name_len);
  X_STATUS Bind(N_XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  object_ref<XSocket> Accept(N_XSOCKADDR* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
               uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to, uint32_t to_len);

  struct packet {
    // These values are in network byte order.
    rex::be<uint16_t> src_port;
    rex::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len);

  // Online play: a datagram socket of the virtual network (online::VirtualNet) instead of a real
  // one: bound to a virtual port, its datagrams carried to and from the peers. Without online play,
  // a VDP socket is virtual with no network: bound, its datagrams go nowhere and none come.
  bool is_virtual() const { return virtual_; }
  // The WSA error of a virtual socket's last failed call (WSAEWOULDBLOCK when nothing is waiting).
  uint32_t last_error() const { return last_error_; }
  // A virtual socket with a datagram waiting (select).
  bool HasPacket();
  void Deliver(uint32_t src_in_addr, uint16_t src_port, const uint8_t* data, size_t size) override;

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  uint64_t native_handle_ = -1;

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;

  std::unique_ptr<rex::thread::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;

  bool virtual_ = false;
  online::VirtualNet* virtual_net_ = nullptr;  // null for a virtual socket with no network
  bool nonblocking_ = false;
  uint32_t last_error_ = 0;
  std::condition_variable incoming_packet_cv_;
};

}  // namespace rex::system
