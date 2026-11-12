/**
 * ReXGlue online play: the XNet structures as the guest sees them (big-endian fields), shared by
 * the XNet exports (xam_net.cpp) and the virtual network (virtual_net.h).
 */

#pragma once

#include <cstdint>

#include <rex/types.h>

namespace rex::system::online {

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
// IN_ADDRs are kept as their numeric value (a.b.c.d = 0xAABBCCDD), as a guest register holds them.
struct XNADDR {
  rex::be<uint32_t> ina;          // IP address (zero if not static/DHCP)
  rex::be<uint32_t> inaOnline;    // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
};
static_assert(sizeof(XNADDR) == 36);

// A session's key ID and key (XNetCreateKey, XNetRegisterKey).
constexpr size_t kXnKidSize = 8;
constexpr size_t kXnKeySize = 16;

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
};
static_assert(sizeof(XNQOSINFO) == 24);

struct XNQOS {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

// XNetGetTitleXnAddr's result.
constexpr uint32_t kXnAddrPending = 0x00;
constexpr uint32_t kXnAddrNone = 0x01;
constexpr uint32_t kXnAddrStatic = 0x04;
constexpr uint32_t kXnAddrDhcp = 0x08;
constexpr uint32_t kXnAddrGateway = 0x20;
constexpr uint32_t kXnAddrDns = 0x40;
constexpr uint32_t kXnAddrOnline = 0x80;

// XNetGetConnectStatus.
constexpr uint32_t kConnectIdle = 0;
constexpr uint32_t kConnectPending = 1;
constexpr uint32_t kConnectConnected = 2;
constexpr uint32_t kConnectLost = 3;

// XNQOSINFO::flags.
constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;
constexpr uint8_t kQosTargetDisabled = 0x04;
constexpr uint8_t kQosDataReceived = 0x08;

// XNetQosListen's flags.
constexpr uint32_t kQosListenEnable = 0x01;
constexpr uint32_t kQosListenDisable = 0x02;
constexpr uint32_t kQosListenSetData = 0x04;
constexpr uint32_t kQosListenSetBitsPerSec = 0x08;
constexpr uint32_t kQosListenRelease = 0x10;

}  // namespace rex::system::online
