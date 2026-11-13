/**
 * The few Steamworks flat API declarations the Steam helper uses, written from Valve's public
 * steam_api_flat.h (SDK 1.6x) so that the SDK builds without the Steamworks SDK: the functions are
 * resolved one by one from steam_api64.dll at run time, and the structures below are the callbacks'
 * (Windows: 8-byte packing, VALVE_CALLBACK_PACK_LARGE; the sizes are checked).
 */

#pragma once

#include <cstdint>

namespace rex::system::online::steam_flat {

using HSteamPipe = int32_t;
using HSteamUser = int32_t;
using SteamAPICall = uint64_t;
using SteamId = uint64_t;

struct ISteamUser;
struct ISteamFriends;
struct ISteamUtils;
struct ISteamMatchmaking;
struct ISteamNetworkingMessages;
struct ISteamNetworkingUtils;

constexpr int kSteamApiInitOk = 0;
constexpr int kResultOk = 1;
constexpr int kLobbyTypeFriendsOnly = 1;
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kFriendRelationshipFriend = 3;
constexpr int kChatRoomEnterSuccess = 1;
constexpr int kIdentityTypeSteamId = 16;
constexpr int kSendUnreliable = 0;
constexpr int kSendNoNagle = 1;
constexpr int kSendReliable = 8;
constexpr int kSendAutoRestartBrokenSession = 32;
constexpr int kConfigIceEnable = 104;  // k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable
constexpr int kConfigScopeGlobal = 1;
constexpr int kConfigTypeInt32 = 1;

#pragma pack(push, 8)
struct CallbackMsg {
  HSteamUser user;
  int callback;
  uint8_t* param;
  int param_size;
};
static_assert(sizeof(CallbackMsg) == 24);

// SteamAPICallCompleted_t
struct ApiCallCompleted {
  static constexpr int kId = 703;
  SteamAPICall call;
  int callback;
  uint32_t param_size;
};
static_assert(sizeof(ApiCallCompleted) == 16);

// SteamNetworkingIdentity
struct NetworkingIdentity {
  int type;
  int size;
  union {
    uint64_t steam_id;
    uint32_t reserved[32];
  };
};
static_assert(sizeof(NetworkingIdentity) == 136);

// SteamNetworkingMessage_t, up to what the helper reads.
struct NetworkingMessage {
  void* data;
  int size;
  uint32_t connection;
  NetworkingIdentity peer;
};

// FriendGameInfo_t
struct FriendGameInfo {
  uint64_t game_id;
  uint32_t game_ip;
  uint16_t game_port;
  uint16_t query_port;
  SteamId lobby;
};
static_assert(sizeof(FriendGameInfo) == 24);

// PersonaStateChange_t
struct PersonaStateChange {
  static constexpr int kId = 304;
  uint64_t steam_id;
  int flags;
};
// GameLobbyJoinRequested_t
struct GameLobbyJoinRequested {
  static constexpr int kId = 333;
  SteamId lobby;
  SteamId friend_id;
};
static_assert(sizeof(GameLobbyJoinRequested) == 16);
// FriendRichPresenceUpdate_t
struct FriendRichPresenceUpdate {
  static constexpr int kId = 336;
  SteamId friend_id;
  uint32_t app_id;
};
// GameRichPresenceJoinRequested_t
struct GameRichPresenceJoinRequested {
  static constexpr int kId = 337;
  SteamId friend_id;
  char connect[256];
};
static_assert(sizeof(GameRichPresenceJoinRequested) == 264);
// LobbyEnter_t
struct LobbyEnter {
  static constexpr int kId = 504;
  uint64_t lobby;
  uint32_t chat_permissions;
  bool locked;
  uint32_t response;
};
static_assert(sizeof(LobbyEnter) == 24);
// LobbyDataUpdate_t
struct LobbyDataUpdate {
  static constexpr int kId = 505;
  uint64_t lobby;
  uint64_t member;
  uint8_t success;
};
// LobbyChatUpdate_t
struct LobbyChatUpdate {
  static constexpr int kId = 506;
  uint64_t lobby;
  uint64_t user_changed;
  uint64_t making_change;
  uint32_t state_change;
};
static_assert(sizeof(LobbyChatUpdate) == 32);
// LobbyCreated_t
struct LobbyCreated {
  static constexpr int kId = 513;
  int result;
  uint64_t lobby;
};
static_assert(sizeof(LobbyCreated) == 16);
// SteamNetworkingMessagesSessionRequest_t
struct SessionRequest {
  static constexpr int kId = 1251;
  NetworkingIdentity remote;
};
static_assert(sizeof(SessionRequest) == 136);
// SteamNetworkingMessagesSessionFailed_t: its SteamNetConnectionInfo_t starts with the identity.
struct SessionFailed {
  static constexpr int kId = 1252;
  NetworkingIdentity remote;
};
#pragma pack(pop)

// The exports, as steam_api_flat.h declares them (references are pointers in the C ABI).
#define REX_STEAM_FLAT_FUNCTIONS(X)                                                                 \
  X(SteamAPI_Shutdown, void, ())                                                                    \
  X(SteamAPI_GetHSteamPipe, HSteamPipe, ())                                                         \
  X(SteamAPI_ManualDispatch_Init, void, ())                                                         \
  X(SteamAPI_ManualDispatch_RunFrame, void, (HSteamPipe))                                           \
  X(SteamAPI_ManualDispatch_GetNextCallback, bool, (HSteamPipe, CallbackMsg*))                      \
  X(SteamAPI_ManualDispatch_FreeLastCallback, void, (HSteamPipe))                                   \
  X(SteamAPI_ManualDispatch_GetAPICallResult, bool,                                                 \
    (HSteamPipe, SteamAPICall, void*, int, int, bool*))                                             \
  X(SteamAPI_ISteamUser_GetSteamID, SteamId, (ISteamUser*))                                         \
  X(SteamAPI_ISteamUser_BLoggedOn, bool, (ISteamUser*))                                             \
  X(SteamAPI_ISteamUtils_GetAppID, uint32_t, (ISteamUtils*))                                        \
  X(SteamAPI_ISteamFriends_GetPersonaName, const char*, (ISteamFriends*))                           \
  X(SteamAPI_ISteamFriends_GetFriendCount, int, (ISteamFriends*, int))                              \
  X(SteamAPI_ISteamFriends_GetFriendByIndex, SteamId, (ISteamFriends*, int, int))                   \
  X(SteamAPI_ISteamFriends_GetFriendRelationship, int, (ISteamFriends*, SteamId))                   \
  X(SteamAPI_ISteamFriends_GetFriendPersonaName, const char*, (ISteamFriends*, SteamId))            \
  X(SteamAPI_ISteamFriends_GetFriendGamePlayed, bool, (ISteamFriends*, SteamId, FriendGameInfo*))   \
  X(SteamAPI_ISteamFriends_GetFriendRichPresence, const char*,                                      \
    (ISteamFriends*, SteamId, const char*))                                                         \
  X(SteamAPI_ISteamFriends_RequestFriendRichPresence, void, (ISteamFriends*, SteamId))              \
  X(SteamAPI_ISteamFriends_SetRichPresence, bool, (ISteamFriends*, const char*, const char*))       \
  X(SteamAPI_ISteamFriends_ClearRichPresence, void, (ISteamFriends*))                               \
  X(SteamAPI_ISteamMatchmaking_CreateLobby, SteamAPICall, (ISteamMatchmaking*, int, int))           \
  X(SteamAPI_ISteamMatchmaking_JoinLobby, SteamAPICall, (ISteamMatchmaking*, SteamId))              \
  X(SteamAPI_ISteamMatchmaking_LeaveLobby, void, (ISteamMatchmaking*, SteamId))                     \
  X(SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, int, (ISteamMatchmaking*, SteamId))              \
  X(SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex, SteamId, (ISteamMatchmaking*, SteamId, int))  \
  X(SteamAPI_ISteamMatchmaking_GetLobbyData, const char*,                                           \
    (ISteamMatchmaking*, SteamId, const char*))                                                     \
  X(SteamAPI_ISteamMatchmaking_SetLobbyData, bool,                                                  \
    (ISteamMatchmaking*, SteamId, const char*, const char*))                                        \
  X(SteamAPI_ISteamMatchmaking_GetLobbyOwner, SteamId, (ISteamMatchmaking*, SteamId))               \
  X(SteamAPI_ISteamNetworkingMessages_SendMessageToUser, int,                                       \
    (ISteamNetworkingMessages*, const NetworkingIdentity*, const void*, uint32_t, int, int))        \
  X(SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel, int,                                \
    (ISteamNetworkingMessages*, int, NetworkingMessage**, int))                                     \
  X(SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser, bool,                                  \
    (ISteamNetworkingMessages*, const NetworkingIdentity*))                                         \
  X(SteamAPI_SteamNetworkingMessage_t_Release, void, (NetworkingMessage*))                          \
  X(SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess, void, (ISteamNetworkingUtils*))          \
  X(SteamAPI_ISteamNetworkingUtils_SetConfigValue, bool,                                            \
    (ISteamNetworkingUtils*, int, int, intptr_t, int, const void*))

}  // namespace rex::system::online::steam_flat
