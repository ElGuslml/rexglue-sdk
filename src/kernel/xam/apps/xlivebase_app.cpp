/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/online/session_service.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // XFriendsCreateEnumerator through XMsgInProcessCall's argument list (16 bytes each, the value
      // big-endian at +8): pointers to the user index, the first friend and the count, then the
      // buffer size and the handle to return. Online play: the peers connected now, each an
      // XONLINE_FRIEND online and playing this game, joinable with the party it hosts.
      auto* sessions = kernel_state_->online_sessions();
      if (!sessions || !buffer_length) {
        REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                      buffer_length);
        return X_E_FAIL;
      }
      auto* args = memory_->TranslateVirtual<uint8_t*>(buffer_length);
      auto arg = [&](int i) { return uint32_t(memory::load_and_swap<uint64_t>(args + 16 * i + 8)); };
      auto deref = [&](uint32_t ptr) {
        return ptr < 0x10000 ? ptr : memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(ptr));
      };
      uint32_t user_index = deref(arg(0));
      uint32_t start = deref(arg(1));
      uint32_t count = std::max<uint32_t>(deref(arg(2)), 1);
      uint32_t buffer_size_ptr = arg(3);
      uint32_t handle_ptr = arg(4);
      constexpr uint32_t kFriendSize = 196;  // XONLINE_FRIEND
      auto e = make_object<XStaticUntypedEnumerator>(kernel_state_, count, kFriendSize);
      auto result = e->Initialize(user_index, 0xFC, 0x58020, 0x58021, 0);
      if (XFAILED(result)) {
        return X_E_FAIL;
      }
      auto friends = sessions->Friends();
      for (size_t i = start; i < friends.size(); ++i) {
        const auto& f = friends[i];
        uint8_t* item = e->AppendItem();
        std::memset(item, 0, kFriendSize);
        memory::store_and_swap<uint64_t>(item + 0, f.xuid);
        std::memcpy(item + 8, f.name.data(), std::min<size_t>(f.name.size(), 15));
        // XONLINE_FRIENDSTATE: online, playing, joinable (with a party open); the user state online.
        uint32_t state = 0x1 | 0x2 | (f.has_party ? 0x10 : 0);
        memory::store_and_swap<uint32_t>(item + 24, state);
        if (f.has_party) {
          std::memcpy(item + 28, f.party.xnkid.data(), online::kXnKidSize);
        }
        memory::store_and_swap<uint32_t>(item + 36, kernel_state_->title_id());
      }
      if (buffer_size_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr),
                                         count * kFriendSize);
      }
      if (handle_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr), e->handle());
      }
      REXKRNL_INFO("XFriendsCreateEnumerator({}): {} friend(s) connected", user_index,
                   friends.size() > start ? friends.size() - start : 0);
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      // XInviteGetAcceptedInfo through XMsgInProcessCall: the second argument is the call's
      // argument list (16 bytes each, the value big-endian at +8): a pointer to the user index,
      // then the XINVITE_INFO to fill (Guitar Hero: Metallica's sub_8254E648).
      auto* sessions = kernel_state_->online_sessions();
      auto invite = sessions ? sessions->accepted_invite() : std::nullopt;
      if (!invite || !buffer_length) {
        REXKRNL_DEBUG(
            "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}): no invite",
            buffer_ptr, buffer_length);
        return X_E_FAIL;
      }
      auto* args = memory_->TranslateVirtual<uint8_t*>(buffer_length);
      uint32_t user_arg = uint32_t(memory::load_and_swap<uint64_t>(args + 8));
      uint32_t user_index =
          user_arg < 4 ? user_arg
                       : memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_arg));
      uint32_t info_ptr = uint32_t(memory::load_and_swap<uint64_t>(args + 24));
      if (!info_ptr || user_index != invite->user_index) {
        REXKRNL_WARN("XInviteGetAcceptedInfo: user {} (the invite is user {}'s), info {:08X}",
                     user_index, invite->user_index, info_ptr);
        return X_E_FAIL;
      }
      // XINVITE_INFO: invitee, inviter, title, the host's XSESSION_INFO, from a game invite.
      auto* info = memory_->TranslateVirtual<uint8_t*>(info_ptr);
      std::memset(info, 0, 84);
      memory::store_and_swap<uint64_t>(info + 0, invite->invitee);
      memory::store_and_swap<uint64_t>(info + 8, invite->inviter);
      memory::store_and_swap<uint32_t>(info + 16, kernel_state_->title_id());
      std::memcpy(info + 20, invite->info.xnkid.data(), online::kXnKidSize);
      std::memcpy(info + 28, &invite->info.host, sizeof(online::XNADDR));
      std::memcpy(info + 64, invite->info.xnkey.data(), online::kXnKeySize);
      memory::store_and_swap<uint32_t>(info + 80, 1);
      REXKRNL_INFO("XInviteGetAcceptedInfo({}): the invite from {:016X}", user_index,
                   invite->inviter);
      return X_E_SUCCESS;
    }
    case 0x00050002: {
      // An XLive web request (XRL): the buffer holds the request object, whose +0 is the method's
      // ID and +24 / +28 the serialized arguments and their size. Online play answers method 1411
      // (0x583), the game invite (Guitar Hero: Metallica's friends list, sub_8254E778): {user index,
      // count, the invitees' XUIDs, the text's size, the text in UTF-16}: each invitee connected now
      // gets an invite to this game's party.
      auto* sessions = kernel_state_->online_sessions();
      uint32_t request = buffer_ptr ? memory::load_and_swap<uint32_t>(buffer) : 0;
      if (!sessions || !request) {
        break;
      }
      auto* req = memory_->TranslateVirtual<uint8_t*>(request);
      uint32_t method = memory::load_and_swap<uint32_t>(req + 0);
      if (method != 0x583) {
        REXKRNL_DEBUG("XLive request, method {} ({:#x}): unanswered", method, method);
        break;
      }
      uint32_t body_ptr = memory::load_and_swap<uint32_t>(req + 24);
      uint32_t body_size = memory::load_and_swap<uint32_t>(req + 28);
      if (!body_ptr || body_size < 8) {
        return X_E_FAIL;
      }
      auto* body = memory_->TranslateVirtual<uint8_t*>(body_ptr);
      uint32_t count = memory::load_and_swap<uint32_t>(body + 4);
      uint32_t sent = 0;
      for (uint32_t i = 0; i < count && 8 + 8 * (i + 1) <= body_size; ++i) {
        uint64_t xuid = memory::load_and_swap<uint64_t>(body + 8 + 8 * i);
        sent += sessions->SendInvite(xuid) ? 1 : 0;
      }
      REXKRNL_INFO("XInviteSend: {} of {} invitee(s) reached", sent, count);
      return sent ? X_E_SUCCESS : X_E_FAIL;
    }
    case 0x0005800E: {
      // XUserMuteListQuery: {user index, XUID at +8, the answer at +16}; nobody is muted. A game
      // asks it for every remote talker each frame (Guitar Hero: Metallica's sub_8254FF00).
      if (buffer_ptr) {
        memory::store_and_swap<uint32_t>(buffer + 16, 0);
      }
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
