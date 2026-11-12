/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex::system {

namespace {

// WSA errors of virtual sockets.
constexpr uint32_t kWsaEwouldblock = 10035;
constexpr uint32_t kWsaEmsgsize = 10040;
constexpr uint32_t kWsaEaddrinuse = 10048;
constexpr uint32_t kWsaEhostunreach = 10065;
// ioctlsocket's commands.
constexpr uint32_t kFionbio = 0x8004667E;
constexpr uint32_t kFionread = 0x4004667F;
// Datagrams waiting on a virtual socket at most (the oldest go first).
constexpr size_t kMaxQueued = 512;

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  // Online play: a datagram socket (VDP or UDP) is virtual, its datagrams carried between the
  // players' games; no real port is taken.
  if (type == Type::X_SOCK_DGRAM) {
    if (online::VirtualNet* net = kernel_state_->online_net()) {
      virtual_net_ = net;
      return X_STATUS_SUCCESS;
    }
  }

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (virtual_net_) {
    if (bound_) {
      virtual_net_->Unbind(this, bound_port_);
      bound_ = false;
    }
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    while (!incoming_packets_.empty()) {
      delete[] incoming_packets_.front();
      incoming_packets_.pop();
    }
    // A thread waiting in recvfrom() returns.
    incoming_packet_cv_.notify_all();
    return X_STATUS_SUCCESS;
  }
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = -1;
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }
  if (virtual_net_) {
    if (level == 0xFFFF && optname == 0x0020) {
      broadcast_socket_ = true;
    }
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (virtual_net_) {
    if (cmd == kFionbio) {
      nonblocking_ = memory::load_and_swap<uint32_t>(arg_ptr) != 0;
    } else if (cmd == kFionread) {
      std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
      uint32_t size = incoming_packets_.empty()
                          ? 0
                          : reinterpret_cast<packet*>(incoming_packets_.front())->data_len;
      memory::store_and_swap<uint32_t>(arg_ptr, size);
    }
    return X_STATUS_SUCCESS;
  }
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (virtual_net_) {
    uint16_t port = name->sin_port;
    if (bound_ || !virtual_net_->Bind(this, &port)) {
      last_error_ = kWsaEaddrinuse;
      return X_STATUS_UNSUCCESSFUL;
    }
    bound_ = true;
    bound_port_ = port;
    return X_STATUS_SUCCESS;
  }
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  if (virtual_net_) {
    std::unique_lock<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.empty()) {
      if (nonblocking_) {
        last_error_ = kWsaEwouldblock;
        return -1;
      }
      incoming_packet_cv_.wait(lock, [this] { return !incoming_packets_.empty() || !bound_; });
      if (incoming_packets_.empty()) {
        last_error_ = kWsaEwouldblock;
        return -1;
      }
    }
    packet* pkt = reinterpret_cast<packet*>(incoming_packets_.front());
    incoming_packets_.pop();
    int size = pkt->data_len;
    if (uint32_t(size) > buf_len) {
      // As a real datagram socket: the start of it, and WSAEMSGSIZE.
      std::memcpy(buf, pkt->data, buf_len);
      last_error_ = kWsaEmsgsize;
      size = -1;
    } else {
      std::memcpy(buf, pkt->data, size);
    }
    if (from) {
      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    if (from_len) {
      *from_len = sizeof(N_XSOCKADDR_IN);
    }
    delete[] reinterpret_cast<uint8_t*>(pkt);
    return size;
  }
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = nfrom.sin_port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  if (virtual_net_) {
    if (!to) {
      last_error_ = kWsaEhostunreach;
      return -1;
    }
    if (!bound_) {
      // A socket sending before bind() gets a port, as a real one.
      uint16_t port = 0;
      if (virtual_net_->Bind(this, &port)) {
        bound_ = true;
        bound_port_ = port;
      }
    }
    uint32_t dst = to->sin_addr;
    if (dst == 0xFFFFFFFF) {
      // A broadcast (system link): nobody to hear it online.
      return int(buf_len);
    }
    int sent = virtual_net_->SendTo(bound_port_, dst, to->sin_port, buf, buf_len);
    if (sent < 0) {
      last_error_ = buf_len > online::VirtualNet::kMaxDatagram ? kWsaEmsgsize : kWsaEhostunreach;
    }
    return sent;
  }
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

bool XSocket::HasPacket() {
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  return !incoming_packets_.empty();
}

void XSocket::Deliver(uint32_t src_in_addr, uint16_t src_port, const uint8_t* data, size_t size) {
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    while (incoming_packets_.size() >= kMaxQueued) {
      delete[] incoming_packets_.front();
      incoming_packets_.pop();
    }
  }
  QueuePacket(src_in_addr, src_port, data, size);
  incoming_packet_cv_.notify_one();
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
