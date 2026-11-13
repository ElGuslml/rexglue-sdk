/**
 * The Steam helper's Steam: steam_api64.dll next to rexsteam.exe, its flat API resolved at run time
 * (steam_flat.h), callbacks by manual dispatch.
 */

#include "steam_backend.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <rex/system/online/steam_link.h>

#include "steam_flat.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace rex::system::online::steam_helper {

namespace {

using namespace steam_flat;

std::string Text(const char* s) {
  return s ? std::string(s) : std::string();
}

NetworkingIdentity IdentityOf(uint64_t steam_id) {
  NetworkingIdentity identity{};
  identity.type = kIdentityTypeSteamId;
  identity.size = 8;
  identity.steam_id = steam_id;
  return identity;
}

class SteamBackend final : public Backend {
 public:
  explicit SteamBackend(std::filesystem::path folder) : folder_(std::move(folder)) {}

  bool Init(std::string* error) override {
#if defined(_WIN32)
    const std::string app = std::to_string(steam_link::kAppId);
    SetEnvironmentVariableA("SteamAppId", app.c_str());
    SetEnvironmentVariableA("SteamGameId", app.c_str());
    const auto dll = folder_ / "steam_api64.dll";
    module_ = LoadLibraryExW(dll.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto symbol = [this](const char* name) {
      return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module_), name));
    };
#else
    const std::string app = std::to_string(steam_link::kAppId);
    setenv("SteamAppId", app.c_str(), 1);
    setenv("SteamGameId", app.c_str(), 1);
    const auto dll = folder_ / "libsteam_api.so";
    module_ = dlopen(dll.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto symbol = [this](const char* name) { return dlsym(module_, name); };
#endif
    if (!module_) {
      *error = "no " + dll.filename().string() + " next to rexsteam";
      return false;
    }
#define REX_RESOLVE(name, ret, args)                                    \
  name = reinterpret_cast<ret(*) args>(symbol(#name));                  \
  if (!name) {                                                          \
    *error = std::string(dll.filename().string()) + " has no " + #name; \
    return false;                                                       \
  }
    REX_STEAM_FLAT_FUNCTIONS(REX_RESOLVE)
#undef REX_RESOLVE
    // The interfaces' accessors carry their version; the newest this code knows first.
    auto accessor = [&](std::initializer_list<const char*> names) -> void* {
      for (const char* name : names) {
        if (auto fn = reinterpret_cast<void* (*)()>(symbol(name))) {
          return fn();
        }
      }
      return nullptr;
    };
    // SteamAPI_InitFlat (1.58 and later) says why it failed; SteamAPI_Init before it.
    auto init_flat = reinterpret_cast<int (*)(char*)>(symbol("SteamAPI_InitFlat"));
    auto init_old = reinterpret_cast<bool (*)()>(symbol("SteamAPI_Init"));
    if (init_flat) {
      char message[1024] = {};
      int result = init_flat(message);
      if (result != kSteamApiInitOk) {
        *error = message[0] ? std::string("Steam: ") + message
                            : "Steam did not start (is it running and signed in?)";
        return false;
      }
    } else if (!init_old || !init_old()) {
      *error = "Steam did not start (is it running and signed in?)";
      return false;
    }
    initialized_ = true;
    SteamAPI_ManualDispatch_Init();
    pipe_ = SteamAPI_GetHSteamPipe();
    user_ = static_cast<ISteamUser*>(
        accessor({"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021"}));
    friends_ = static_cast<ISteamFriends*>(
        accessor({"SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017"}));
    utils_ = static_cast<ISteamUtils*>(
        accessor({"SteamAPI_SteamUtils_v011", "SteamAPI_SteamUtils_v010"}));
    matchmaking_ =
        static_cast<ISteamMatchmaking*>(accessor({"SteamAPI_SteamMatchmaking_v009"}));
    messages_ = static_cast<ISteamNetworkingMessages*>(
        accessor({"SteamAPI_SteamNetworkingMessages_SteamAPI_v002"}));
    networking_ = static_cast<ISteamNetworkingUtils*>(
        accessor({"SteamAPI_SteamNetworkingUtils_SteamAPI_v004"}));
    if (!pipe_ || !user_ || !friends_ || !utils_ || !matchmaking_ || !messages_ || !networking_) {
      *error = "this steam_api64.dll lacks an interface rexsteam needs";
      return false;
    }
    if (SteamAPI_ISteamUtils_GetAppID(utils_) != steam_link::kAppId) {
      *error = "Steam started as another app";
      return false;
    }
    if (!SteamAPI_ISteamUser_BLoggedOn(user_)) {
      *error = "Steam is offline";
      return false;
    }
    self_ = SteamAPI_ISteamUser_GetSteamID(user_);
    // Only Valve's relays (no ICE): no player learns another's IP address.
    int32_t no_ice = 0;
    SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking_, kConfigIceEnable, kConfigScopeGlobal,
                                                  0, kConfigTypeInt32, &no_ice);
    SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(networking_);
    return true;
  }

  void Shutdown() override {
    if (initialized_) {
      SteamAPI_ISteamFriends_ClearRichPresence(friends_);
      SteamAPI_Shutdown();
      initialized_ = false;
    }
    // The module stays loaded: Steam's overlay is not unhooked by its shutdown.
  }

  uint64_t SelfId() override { return self_; }
  std::string SelfName() override { return Text(SteamAPI_ISteamFriends_GetPersonaName(friends_)); }

  void RunCallbacks(const Events& events) override {
    SteamAPI_ManualDispatch_RunFrame(pipe_);
    CallbackMsg msg{};
    while (SteamAPI_ManualDispatch_GetNextCallback(pipe_, &msg)) {
      if (msg.callback == ApiCallCompleted::kId && msg.param) {
        ApiCallCompleted done;
        std::memcpy(&done, msg.param, sizeof(done));
        std::vector<uint8_t> result(done.param_size);
        bool failed = false;
        if (SteamAPI_ManualDispatch_GetAPICallResult(pipe_, done.call, result.data(),
                                                     int(result.size()), done.callback, &failed)) {
          Dispatch(done.callback, failed ? nullptr : result.data(), result.size(), done.call,
                   events);
        }
      } else {
        Dispatch(msg.callback, msg.param, size_t(msg.param_size), 0, events);
      }
      SteamAPI_ManualDispatch_FreeLastCallback(pipe_);
    }
  }

  void SetRichPresence(const char* key, const std::string& value) override {
    SteamAPI_ISteamFriends_SetRichPresence(friends_, key, value.c_str());
  }

  void CreateLobby(int max_members) override {
    create_call_ = SteamAPI_ISteamMatchmaking_CreateLobby(matchmaking_, kLobbyTypeFriendsOnly,
                                                          max_members);
  }
  void JoinLobby(uint64_t lobby) override {
    join_call_ = SteamAPI_ISteamMatchmaking_JoinLobby(matchmaking_, lobby);
  }
  void LeaveLobby(uint64_t lobby) override {
    SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking_, lobby);
  }
  uint64_t LobbyOwner(uint64_t lobby) override {
    return SteamAPI_ISteamMatchmaking_GetLobbyOwner(matchmaking_, lobby);
  }
  std::string LobbyData(uint64_t lobby, const char* key) override {
    return Text(SteamAPI_ISteamMatchmaking_GetLobbyData(matchmaking_, lobby, key));
  }
  void SetLobbyData(uint64_t lobby, const char* key, const std::string& value) override {
    SteamAPI_ISteamMatchmaking_SetLobbyData(matchmaking_, lobby, key, value.c_str());
  }
  bool IsLobbyMember(uint64_t lobby, uint64_t user) override {
    int count = SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(matchmaking_, lobby);
    for (int i = 0; i < count; ++i) {
      if (SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(matchmaking_, lobby, i) == user) {
        return true;
      }
    }
    return false;
  }
  bool IsFriend(uint64_t user) override {
    return SteamAPI_ISteamFriends_GetFriendRelationship(friends_, user) ==
           kFriendRelationshipFriend;
  }

  std::vector<FriendInfo> FriendsInApp() override {
    std::vector<FriendInfo> list;
    int count = SteamAPI_ISteamFriends_GetFriendCount(friends_, kFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
      SteamId id = SteamAPI_ISteamFriends_GetFriendByIndex(friends_, i, kFriendFlagImmediate);
      FriendGameInfo game{};
      if (!SteamAPI_ISteamFriends_GetFriendGamePlayed(friends_, id, &game)) {
        continue;
      }
      // A CGameID of an app: the app ID in its low 24 bits, type 0.
      if ((game.game_id & 0xFFFFFF) != steam_link::kAppId || ((game.game_id >> 24) & 0xFF) != 0) {
        continue;
      }
      FriendInfo info;
      info.id = id;
      info.persona = Text(SteamAPI_ISteamFriends_GetFriendPersonaName(friends_, id));
      info.lobby = game.lobby;
      list.push_back(std::move(info));
    }
    return list;
  }
  std::string FriendPresence(uint64_t user, const char* key) override {
    return Text(SteamAPI_ISteamFriends_GetFriendRichPresence(friends_, user, key));
  }
  void RequestFriendPresence(uint64_t user) override {
    SteamAPI_ISteamFriends_RequestFriendRichPresence(friends_, user);
  }

  bool Send(uint64_t to, int channel, const uint8_t* data, size_t size, bool reliable) override {
    NetworkingIdentity identity = IdentityOf(to);
    int flags = kSendAutoRestartBrokenSession | (reliable ? kSendReliable : kSendNoNagle);
    return SteamAPI_ISteamNetworkingMessages_SendMessageToUser(messages_, &identity, data,
                                                               uint32_t(size), flags,
                                                               channel) == kResultOk;
  }

  void Receive(int channel, const ReceiveFn& receive) override {
    NetworkingMessage* messages[64];
    for (int round = 0; round < 16; ++round) {
      int count = SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(messages_, channel,
                                                                             messages, 64);
      for (int i = 0; i < count; ++i) {
        NetworkingMessage* m = messages[i];
        if (m->peer.type == kIdentityTypeSteamId && m->data && m->size > 0) {
          receive(m->peer.steam_id, static_cast<const uint8_t*>(m->data), size_t(m->size));
        }
        SteamAPI_SteamNetworkingMessage_t_Release(m);
      }
      if (count < 64) {
        break;
      }
    }
  }

  void AcceptSession(uint64_t user) override {
    NetworkingIdentity identity = IdentityOf(user);
    SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(messages_, &identity);
  }

 private:
  template <typename T>
  static bool As(const uint8_t* param, size_t size, T* out) {
    if (!param || size < sizeof(T)) {
      return false;
    }
    std::memcpy(out, param, sizeof(T));
    return true;
  }

  void Dispatch(int id, const uint8_t* param, size_t size, SteamAPICall call,
                const Events& events) {
    switch (id) {
      case LobbyCreated::kId: {
        LobbyCreated created{};
        bool ok = As(param, size, &created) && created.result == kResultOk;
        if (call && call == create_call_) {
          create_call_ = 0;
          events.lobby_entered(ok, ok ? created.lobby : 0);
        }
        break;
      }
      case LobbyEnter::kId: {
        LobbyEnter enter{};
        if (!As(param, size, &enter)) {
          if (call && call == join_call_) {
            join_call_ = 0;
            events.lobby_entered(false, 0);
          }
          break;
        }
        bool ok = enter.response == kChatRoomEnterSuccess;
        if (call && call == join_call_) {
          join_call_ = 0;
          events.lobby_entered(ok, enter.lobby);
        } else if (!call && ok) {
          // The broadcast copy (also sent for a lobby this user created): the helper ignores a
          // lobby it is already in.
          events.lobby_entered(true, enter.lobby);
        }
        break;
      }
      case LobbyChatUpdate::kId: {
        LobbyChatUpdate update{};
        if (As(param, size, &update)) {
          events.lobby_changed(update.lobby);
        }
        break;
      }
      case LobbyDataUpdate::kId: {
        LobbyDataUpdate update{};
        if (As(param, size, &update)) {
          events.lobby_changed(update.lobby);
        }
        break;
      }
      case GameLobbyJoinRequested::kId: {
        GameLobbyJoinRequested request{};
        if (As(param, size, &request) && request.lobby) {
          events.join_requested(request.lobby);
        }
        break;
      }
      case GameRichPresenceJoinRequested::kId: {
        GameRichPresenceJoinRequested request{};
        if (As(param, size, &request)) {
          request.connect[sizeof(request.connect) - 1] = 0;
          const std::string connect = request.connect;
          const std::string prefix = "+connect_lobby ";
          if (connect.compare(0, prefix.size(), prefix) == 0) {
            uint64_t lobby = std::strtoull(connect.c_str() + prefix.size(), nullptr, 10);
            if (lobby) {
              events.join_requested(lobby);
            }
          }
        }
        break;
      }
      case SessionRequest::kId: {
        SessionRequest request{};
        if (As(param, size, &request) && request.remote.type == kIdentityTypeSteamId) {
          events.session_requested(request.remote.steam_id);
        }
        break;
      }
      case PersonaStateChange::kId:
      case FriendRichPresenceUpdate::kId:
        events.friends_changed();
        break;
      default:
        break;
    }
  }

  std::filesystem::path folder_;
  void* module_ = nullptr;
  bool initialized_ = false;
  HSteamPipe pipe_ = 0;
  ISteamUser* user_ = nullptr;
  ISteamFriends* friends_ = nullptr;
  ISteamUtils* utils_ = nullptr;
  ISteamMatchmaking* matchmaking_ = nullptr;
  ISteamNetworkingMessages* messages_ = nullptr;
  ISteamNetworkingUtils* networking_ = nullptr;
  uint64_t self_ = 0;
  SteamAPICall create_call_ = 0;
  SteamAPICall join_call_ = 0;

#define REX_DECLARE(name, ret, args) ret(*name) args = nullptr;
  REX_STEAM_FLAT_FUNCTIONS(REX_DECLARE)
#undef REX_DECLARE
};

}  // namespace

std::unique_ptr<Backend> CreateSteamBackend(const std::filesystem::path& folder) {
  return std::make_unique<SteamBackend>(folder);
}

}  // namespace rex::system::online::steam_helper
