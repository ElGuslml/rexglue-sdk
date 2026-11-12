/**
 * ReXGlue online play: Xbox Live's sessions between friends, without Xbox Live.
 *
 * The XSession calls of each game are kept here (create, join, leave, modify, start, end, delete);
 * a game's hosted sessions (those of its peer network, not a single player's) are advertised to the
 * peers it is connected to (virtual_net.h), with their info (key ID, key, host's XNADDR), slots,
 * and the host user's contexts and properties. A search answers from the sessions the connected
 * peers advertise (a friend's game hosting); a search by ID finds one of them.
 *
 * Invites: a game that joined a host by its address (online_host) is invited once to each party
 * the host opens (a session with presence and invites), when it is itself in its online lobby (a
 * party of its own): the game's own invite path then leaves its lobby and joins the host's, as when
 * a player accepts a friend's invite on the console.
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <rex/system/online/xnet_types.h>

namespace rex::system::online {

class VirtualNet;

// XSESSION_INFO without the guest's layout.
struct SessionInfo {
  std::array<uint8_t, kXnKidSize> xnkid{};
  std::array<uint8_t, kXnKeySize> xnkey{};
  XNADDR host{};
};

// A user property (XUSER_PROPERTY): its id gives its type (the high nibble); the value's bytes
// big-endian as the guest wrote them (ints), UTF-16 big-endian with the terminator (strings).
struct SessionProperty {
  uint32_t id = 0;
  std::vector<uint8_t> value;
};

struct AdvertisedSession {
  SessionInfo info;
  uint32_t flags = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint32_t filled_public = 0;
  uint32_t filled_private = 0;
  bool started = false;
  uint64_t owner = 0;  // the peer that hosts it
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<SessionProperty> properties;
};

// A session member (XSESSION_MEMBER).
struct SessionMember {
  uint64_t xuid = 0;
  uint32_t user_index = 0xFE;  // a local user's index, or XUSER_INDEX_NONE (remote)
  bool is_private = false;
};

// One of this game's sessions (XSessionGetDetails).
struct LocalSession {
  uint32_t obj = 0;
  uint32_t flags = 0;
  uint32_t user_index = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint64_t nonce = 0;
  SessionInfo info;
  bool host = false;
  bool started = false;
  std::vector<SessionMember> members;
};

// An accepted invite (XINVITE_INFO).
struct AcceptedInvite {
  uint32_t user_index = 0;
  uint64_t invitee = 0;
  uint64_t inviter = 0;
  SessionInfo info;
};

class SessionService {
 public:
  // XSESSION_CREATE flags.
  static constexpr uint32_t kHost = 0x1;
  static constexpr uint32_t kUsesPresence = 0x2;
  static constexpr uint32_t kUsesStats = 0x4;
  static constexpr uint32_t kUsesMatchmaking = 0x8;
  static constexpr uint32_t kUsesArbitration = 0x10;
  static constexpr uint32_t kUsesPeerNetwork = 0x20;
  static constexpr uint32_t kInvitesDisabled = 0x100;
  static constexpr uint32_t kJoinViaPresenceDisabled = 0x200;
  static constexpr uint32_t kJoinInProgressDisabled = 0x400;
  // X_CONTEXT_GAME_TYPE, X_CONTEXT_GAME_MODE; the host's name (X_PROPERTY_GAMER_HOSTNAME).
  static constexpr uint32_t kContextGameType = 0x800A;
  static constexpr uint32_t kContextGameMode = 0x800B;
  static constexpr uint32_t kPropertyHostName = 0x40008109;

  // invite: raises the console's "invite accepted" notification for a user (the network thread).
  SessionService(VirtualNet* net, uint64_t self_xuid, std::string self_name,
                 std::function<void(uint32_t user_index)> invite);

  // A user's contexts and properties (XUserSetContext, XUserSetProperty): a session advertises its
  // host user's.
  void SetContext(uint32_t user_index, uint32_t id, uint32_t value);
  void SetProperty(uint32_t user_index, uint32_t id, const std::vector<uint8_t>& value);

  // XSessionCreate: a host session gets a new info (written to *info) and nonce; a joining one
  // (no kHost) takes the info given (the session found).
  void Create(uint32_t obj, uint32_t flags, uint32_t user_index, uint32_t max_public,
              uint32_t max_private, SessionInfo* info, uint64_t* nonce);
  void Delete(uint32_t obj);
  void JoinLocal(uint32_t obj, const std::vector<uint32_t>& user_indices,
                 const std::vector<bool>& is_private);
  void JoinRemote(uint32_t obj, const std::vector<uint64_t>& xuids,
                  const std::vector<bool>& is_private);
  void LeaveLocal(uint32_t obj, const std::vector<uint32_t>& user_indices);
  void LeaveRemote(uint32_t obj, const std::vector<uint64_t>& xuids);
  void Modify(uint32_t obj, uint32_t flags, uint32_t max_public, uint32_t max_private);
  void Start(uint32_t obj);
  void End(uint32_t obj);
  std::optional<LocalSession> Details(uint32_t obj) const;

  // XSessionSearchEx: open sessions the connected peers host, matching the query's game type,
  // game mode and title game mode contexts where both have them; at most `max_results`.
  std::vector<AdvertisedSession> Search(const std::vector<std::pair<uint32_t, uint32_t>>& contexts,
                                        uint32_t max_results) const;
  // XSessionSearchByID.
  std::optional<AdvertisedSession> FindById(const std::array<uint8_t, kXnKidSize>& xnkid) const;

  // XInviteGetAcceptedInfo: the invite last accepted.
  std::optional<AcceptedInvite> accepted_invite() const;

  // Decoding of an advertisement (tests).
  static std::vector<AdvertisedSession> DecodeSessions(const uint8_t* data, size_t size,
                                                       uint64_t owner);

 private:
  struct UserValues {
    std::map<uint32_t, uint32_t> contexts;
    std::map<uint32_t, std::vector<uint8_t>> properties;
  };

  void Advertise(uint64_t to);  // 0: every connected peer
  std::vector<uint8_t> EncodeSessions() const;
  void OnControl(uint64_t peer, const uint8_t* data, size_t size);
  void OnPeer(uint64_t peer, bool connected);
  // A joiner in its own lobby is invited to the hub's open party (the lock held); true: call
  // invite_ once the lock is released.
  bool MaybeInvite();
  uint64_t LocalUserXuid(uint32_t user_index) const;
  static bool IsAdvertised(const LocalSession& session);

  VirtualNet* net_;
  const uint64_t self_xuid_;
  const std::string self_name_;
  std::function<void(uint32_t)> invite_;

  mutable std::mutex mutex_;
  std::map<uint32_t, UserValues> users_;
  std::map<uint32_t, LocalSession> sessions_;
  std::map<uint64_t, std::vector<AdvertisedSession>> remote_;  // by the peer that hosts them
  std::set<std::array<uint8_t, kXnKidSize>> invited_;
  std::optional<AcceptedInvite> accepted_invite_;
};

}  // namespace rex::system::online
