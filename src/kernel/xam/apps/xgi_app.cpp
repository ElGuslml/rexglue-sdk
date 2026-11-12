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

#include <rex/kernel/xam/apps/xgi_app.h>

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/online/session_service.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

namespace {

using online::AdvertisedSession;
using online::SessionInfo;
using online::SessionService;

// XSESSION_INFO in guest memory: XNKID, the host's XNADDR, XNKEY (60 bytes).
// XSESSION_SEARCHRESULT (92 bytes), XUSER_CONTEXT (8), XUSER_PROPERTY (24),
// XSESSION_LOCAL_DETAILS (128), XSESSION_MEMBER (16).
constexpr uint32_t kSearchResultSize = 92;
constexpr uint32_t kContextSize = 8;
constexpr uint32_t kPropertySize = 24;
constexpr uint32_t kLocalDetailsSize = 128;
constexpr uint32_t kMemberSize = 16;
// XSESSION_STATE.
constexpr uint32_t kStateLobby = 0;
constexpr uint32_t kStateInGame = 2;

void ReadSessionInfo(const uint8_t* p, SessionInfo* info) {
  std::memcpy(info->xnkid.data(), p, online::kXnKidSize);
  std::memcpy(&info->host, p + 8, sizeof(online::XNADDR));
  std::memcpy(info->xnkey.data(), p + 44, online::kXnKeySize);
}

void WriteSessionInfo(uint8_t* p, const SessionInfo& info) {
  std::memcpy(p, info.xnkid.data(), online::kXnKidSize);
  std::memcpy(p + 8, &info.host, sizeof(online::XNADDR));
  std::memcpy(p + 44, info.xnkey.data(), online::kXnKeySize);
}

// XSESSION_SEARCHRESULT_HEADER and the results, their contexts and properties after them, as many
// as fit in the buffer.
uint32_t WriteSearchResults(memory::Memory* memory, uint32_t results_ptr, uint32_t buffer_size,
                            const std::vector<AdvertisedSession>& sessions) {
  uint8_t* base = memory->TranslateVirtual<uint8_t*>(results_ptr);
  uint32_t count = 0;
  // The results first, then the variable data.
  uint32_t data_at = 8 + kSearchResultSize * uint32_t(sessions.size());
  for (const AdvertisedSession& session : sessions) {
    uint32_t needed = uint32_t(session.contexts.size()) * kContextSize +
                      uint32_t(session.properties.size()) * kPropertySize;
    for (const auto& property : session.properties) {
      needed += uint32_t(property.value.size() + 3) & ~3u;
    }
    if (data_at + needed > buffer_size) {
      break;
    }
    uint8_t* result = base + 8 + kSearchResultSize * count;
    std::memset(result, 0, kSearchResultSize);
    WriteSessionInfo(result, session.info);
    uint32_t open_public =
        session.max_public > session.filled_public ? session.max_public - session.filled_public : 0;
    uint32_t open_private = session.max_private > session.filled_private
                                ? session.max_private - session.filled_private
                                : 0;
    memory::store_and_swap<uint32_t>(result + 60, open_public);
    memory::store_and_swap<uint32_t>(result + 64, open_private);
    memory::store_and_swap<uint32_t>(result + 68, session.filled_public);
    memory::store_and_swap<uint32_t>(result + 72, session.filled_private);
    memory::store_and_swap<uint32_t>(result + 76, uint32_t(session.properties.size()));
    memory::store_and_swap<uint32_t>(result + 80, uint32_t(session.contexts.size()));
    uint32_t properties_at = data_at;
    uint32_t contexts_at = properties_at + uint32_t(session.properties.size()) * kPropertySize;
    uint32_t values_at = contexts_at + uint32_t(session.contexts.size()) * kContextSize;
    memory::store_and_swap<uint32_t>(result + 84, results_ptr + properties_at);
    memory::store_and_swap<uint32_t>(result + 88, results_ptr + contexts_at);
    for (size_t i = 0; i < session.contexts.size(); ++i) {
      uint8_t* context = base + contexts_at + kContextSize * i;
      memory::store_and_swap<uint32_t>(context, session.contexts[i].first);
      memory::store_and_swap<uint32_t>(context + 4, session.contexts[i].second);
    }
    for (size_t i = 0; i < session.properties.size(); ++i) {
      const auto& value = session.properties[i].value;
      uint8_t* property = base + properties_at + kPropertySize * i;
      std::memset(property, 0, kPropertySize);
      uint32_t id = session.properties[i].id;
      memory::store_and_swap<uint32_t>(property, id);
      // XUSER_DATA: its type (the id's high nibble), then the value at +8.
      uint8_t type = uint8_t(id >> 28);
      property[8] = type;
      if (type == 4 || type == 6) {
        // A string or binary: its size and a pointer to it.
        std::memcpy(base + values_at, value.data(), value.size());
        memory::store_and_swap<uint32_t>(property + 16, uint32_t(value.size()));
        memory::store_and_swap<uint32_t>(property + 20, results_ptr + values_at);
        values_at += uint32_t(value.size() + 3) & ~3u;
      } else {
        // Big-endian as the guest gave it (4 or 8 bytes).
        std::memcpy(property + 16, value.data(), std::min<size_t>(value.size(), 8));
      }
    }
    data_at = values_at;
    ++count;
  }
  memory::store_and_swap<uint32_t>(base + 0, count);
  memory::store_and_swap<uint32_t>(base + 4, count ? results_ptr + 8 : 0);
  return count;
}

std::vector<bool> ReadBools(memory::Memory* memory, uint32_t ptr, uint32_t count) {
  std::vector<bool> values(count, false);
  if (ptr) {
    for (uint32_t i = 0; i < count; ++i) {
      values[i] = memory::load_and_swap<uint32_t>(memory->TranslateVirtual(ptr + 4 * i)) != 0;
    }
  }
  return values;
}

}  // namespace

// Online play: the session messages kept by the session service, the guest's structures filled.
// false: not one of them (the switch below answers as without online play).
bool XgiApp::DispatchOnline(uint32_t message, uint32_t buffer_ptr, uint32_t,
                            X_HRESULT* result) {
  SessionService* sessions = kernel_state_->online_sessions();
  if (!sessions) {
    return false;
  }
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  auto u32 = [&](uint32_t offset) { return memory::load_and_swap<uint32_t>(buffer + offset); };
  *result = X_E_SUCCESS;
  switch (message) {
    case 0x000B0006: {  // XUserSetContextEx
      sessions->SetContext(u32(0), u32(16), u32(20));
      return false;  // logged below
    }
    case 0x000B0007: {  // XUserSetPropertyEx
      uint32_t size = u32(20);
      uint32_t value_ptr = u32(24);
      std::vector<uint8_t> value;
      if (value_ptr && size) {
        auto* p = memory_->TranslateVirtual<uint8_t*>(value_ptr);
        value.assign(p, p + size);
      }
      sessions->SetProperty(u32(0), u32(16), value);
      return false;
    }
    case 0x000B0010: {  // XSessionCreate
      uint32_t obj = u32(0);
      uint32_t flags = u32(4);
      uint32_t info_ptr = u32(20);
      uint32_t nonce_ptr = u32(24);
      SessionInfo info;
      if (info_ptr) {
        ReadSessionInfo(memory_->TranslateVirtual<uint8_t*>(info_ptr), &info);
      }
      uint64_t nonce = 0;
      sessions->Create(obj, flags, u32(16), u32(8), u32(12), &info, &nonce);
      if (info_ptr) {
        WriteSessionInfo(memory_->TranslateVirtual<uint8_t*>(info_ptr), info);
      }
      if (nonce_ptr) {
        memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), nonce);
      }
      return true;
    }
    case 0x000B0011: {  // XSessionDelete
      sessions->Delete(u32(0));
      return true;
    }
    case 0x000B0012:    // XSessionJoinLocal / XSessionJoinRemote
    case 0x000B0013: {  // XSessionLeaveLocal / XSessionLeaveRemote
      uint32_t obj = u32(0);
      uint32_t count = u32(4);
      uint32_t xuids_ptr = u32(8);
      uint32_t indices_ptr = u32(12);
      uint32_t private_ptr = message == 0x000B0012 ? u32(16) : 0;
      std::vector<bool> is_private = ReadBools(memory_, private_ptr, count);
      if (xuids_ptr) {
        std::vector<uint64_t> xuids(count);
        for (uint32_t i = 0; i < count; ++i) {
          xuids[i] = memory::load_and_swap<uint64_t>(memory_->TranslateVirtual(xuids_ptr + 8 * i));
        }
        if (message == 0x000B0012) {
          sessions->JoinRemote(obj, xuids, is_private);
        } else {
          sessions->LeaveRemote(obj, xuids);
        }
      } else if (indices_ptr) {
        std::vector<uint32_t> indices(count);
        for (uint32_t i = 0; i < count; ++i) {
          indices[i] =
              memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(indices_ptr + 4 * i));
        }
        if (message == 0x000B0012) {
          sessions->JoinLocal(obj, indices, is_private);
        } else {
          sessions->LeaveLocal(obj, indices);
        }
      }
      return true;
    }
    case 0x000B0014: {  // XSessionStart
      sessions->Start(u32(0));
      return true;
    }
    case 0x000B0015: {  // XSessionEnd
      sessions->End(u32(0));
      return true;
    }
    case 0x000B0018: {  // XSessionModify
      sessions->Modify(u32(0), u32(4), u32(8), u32(12));
      return true;
    }
    case 0x000B001B: {  // XSessionSearchByID: user, XNKID, buffer size, results
      std::array<uint8_t, online::kXnKidSize> xnkid{};
      std::memcpy(xnkid.data(), buffer + 4, online::kXnKidSize);
      uint32_t buffer_size = u32(12);
      uint32_t results_ptr = u32(16);
      std::vector<AdvertisedSession> found;
      if (auto session = sessions->FindById(xnkid)) {
        found.push_back(*session);
      }
      uint32_t count = results_ptr ? WriteSearchResults(memory_, results_ptr, buffer_size, found) : 0;
      REXKRNL_INFO("XSessionSearchByID: {} found", count);
      return true;
    }
    case 0x000B001C: {  // XSessionSearchEx
      uint32_t num_results = u32(8);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t ctx_ptr = u32(20);
      uint32_t buffer_size = u32(24);
      uint32_t results_ptr = u32(28);
      std::vector<std::pair<uint32_t, uint32_t>> contexts;
      for (uint32_t i = 0; ctx_ptr && i < num_ctx; ++i) {
        auto* context = memory_->TranslateVirtual<uint8_t*>(ctx_ptr + kContextSize * i);
        contexts.push_back({memory::load_and_swap<uint32_t>(context),
                            memory::load_and_swap<uint32_t>(context + 4)});
      }
      std::vector<AdvertisedSession> found = sessions->Search(contexts, num_results);
      uint32_t count = results_ptr ? WriteSearchResults(memory_, results_ptr, buffer_size, found) : 0;
      REXKRNL_INFO("XSessionSearchEx: {} found", count);
      return true;
    }
    case 0x000B001A: {  // XSessionArbitrationRegister: no arbitration between friends, nobody registered
      uint32_t results_size = u32(20);
      uint32_t results_ptr = u32(24);
      if (results_ptr && results_size >= 8) {
        auto* results = memory_->TranslateVirtual<uint8_t*>(results_ptr);
        std::memset(results, 0, 8);
      }
      return true;
    }
    case 0x000B001D: {  // XSessionGetDetails
      uint32_t obj = u32(0);
      uint32_t details_size = u32(4);
      uint32_t details_ptr = u32(8);
      auto session = sessions->Details(obj);
      if (!session || !details_ptr) {
        return false;
      }
      auto* details = memory_->TranslateVirtual<uint8_t*>(details_ptr);
      std::memset(details, 0, std::min(details_size, kLocalDetailsSize));
      uint32_t members = uint32_t(session->members.size());
      uint32_t returned =
          details_size >= kLocalDetailsSize
              ? std::min<uint32_t>(members, (details_size - kLocalDetailsSize) / kMemberSize)
              : 0;
      uint32_t filled_public = 0, filled_private = 0;
      for (const auto& member : session->members) {
        ++(member.is_private ? filled_private : filled_public);
      }
      memory::store_and_swap<uint32_t>(details + 0, session->host ? session->user_index : 0xFE);
      memory::store_and_swap<uint32_t>(details + 12, session->flags);
      memory::store_and_swap<uint32_t>(details + 16, session->max_public);
      memory::store_and_swap<uint32_t>(details + 20, session->max_private);
      memory::store_and_swap<uint32_t>(
          details + 24, session->max_public > filled_public ? session->max_public - filled_public : 0);
      memory::store_and_swap<uint32_t>(
          details + 28,
          session->max_private > filled_private ? session->max_private - filled_private : 0);
      memory::store_and_swap<uint32_t>(details + 32, members);
      memory::store_and_swap<uint32_t>(details + 36, returned);
      memory::store_and_swap<uint32_t>(details + 40, session->started ? kStateInGame : kStateLobby);
      memory::store_and_swap<uint64_t>(details + 48, session->nonce);
      WriteSessionInfo(details + 56, session->info);
      memory::store_and_swap<uint32_t>(details + 124, returned ? details_ptr + kLocalDetailsSize : 0);
      for (uint32_t i = 0; i < returned; ++i) {
        uint8_t* member = details + kLocalDetailsSize + kMemberSize * i;
        memory::store_and_swap<uint64_t>(member, session->members[i].xuid);
        memory::store_and_swap<uint32_t>(member + 8, session->members[i].user_index);
        memory::store_and_swap<uint32_t>(member + 12, session->members[i].is_private ? 1 : 0);
      }
      return true;
    }
  }
  return false;
}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  X_HRESULT online_result;
  if (DispatchOnline(message, buffer_ptr, buffer_length, &online_result)) {
    return online_result;
  }
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);
      bool is_local = xuid_array_ptr == 0;

      REXKRNL_DEBUG("{}({:08X}, {}, {}, {:08X}, {:08X})",
                    is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                    user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
