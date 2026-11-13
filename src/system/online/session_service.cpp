/**
 * ReXGlue online play: Xbox Live's sessions between friends (see session_service.h).
 */

#include <rex/system/online/session_service.h>

#include <algorithm>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/system/online/virtual_net.h>

namespace rex::system::online {

namespace {

// The sessions' channel on the virtual network, and its messages.
constexpr uint8_t kChannel = 1;
constexpr uint8_t kMessageSessions = 1;  // the sender's hosted sessions, all of them
constexpr uint8_t kMessageRequest = 2;   // asks for the receiver's
constexpr uint8_t kMessageInvite = 3;    // an invite to the sender's party: its info, the sender's name

std::mutex invite_prompt_mutex;
SessionService::InvitePrompt invite_prompt;

void Put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  Put16(out, uint16_t(v));
  Put16(out, uint16_t(v >> 16));
}

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at = 0;
  bool ok = true;
  const uint8_t* Bytes(size_t n) {
    if (at + n > size) {
      ok = false;
      return nullptr;
    }
    const uint8_t* p = data + at;
    at += n;
    return p;
  }
  uint8_t U8() {
    const uint8_t* p = Bytes(1);
    return p ? p[0] : 0;
  }
  uint16_t U16() {
    const uint8_t* p = Bytes(2);
    return p ? uint16_t(p[0] | p[1] << 8) : 0;
  }
  uint32_t U32() {
    uint32_t lo = U16();
    return lo | uint32_t(U16()) << 16;
  }
};

std::string KidText(const std::array<uint8_t, kXnKidSize>& kid) {
  std::string text;
  for (uint8_t b : kid) {
    text += fmt::format("{:02X}", b);
  }
  return text;
}

// UTF-16 big-endian with its terminator, as a guest's unicode property.
std::vector<uint8_t> Utf16Be(const std::string& text) {
  std::vector<uint8_t> out;
  for (unsigned char c : text) {
    out.push_back(0);
    out.push_back(c);
  }
  out.push_back(0);
  out.push_back(0);
  return out;
}

}  // namespace

SessionService::SessionService(VirtualNet* net, uint64_t self_xuid, std::string self_name,
                               std::function<void(uint32_t)> invite)
    : net_(net), self_xuid_(self_xuid), self_name_(std::move(self_name)), invite_(std::move(invite)) {
  net_->SetControlHandler(kChannel, [this](uint64_t peer, const uint8_t* data, size_t size) {
    OnControl(peer, data, size);
  });
  net_->SetPeerHandler([this](uint64_t peer, bool connected) { OnPeer(peer, connected); });
}

uint64_t SessionService::LocalUserXuid(uint32_t user_index) const {
  // As XamUserGetSigninInfo gives them: the profile's online XUID, a guest's from it.
  return user_index == 0 ? self_xuid_ : self_xuid_ ^ (uint64_t(user_index) << 32);
}

bool SessionService::IsAdvertised(const LocalSession& session) {
  // A host's sessions of a peer network (a party, a match); not a single player's.
  return session.host && (session.flags & kUsesPeerNetwork);
}

void SessionService::SetContext(uint32_t user_index, uint32_t id, uint32_t value) {
  std::lock_guard lock(mutex_);
  users_[user_index].contexts[id] = value;
}

void SessionService::SetProperty(uint32_t user_index, uint32_t id,
                                 const std::vector<uint8_t>& value) {
  std::lock_guard lock(mutex_);
  users_[user_index].properties[id] = value;
}

void SessionService::Create(uint32_t obj, uint32_t flags, uint32_t user_index, uint32_t max_public,
                            uint32_t max_private, SessionInfo* info, uint64_t* nonce) {
  std::random_device random;
  if (flags & kUsesArbitration) {
    // Ranked matches are not offered between friends (there is no arbitration service): a ranked session is a
    // player match.
    REXSYS_WARN("online: session {:08X} asked for arbitration (ranked); made a player match", obj);
    flags &= ~kUsesArbitration;
  }
  LocalSession session;
  session.obj = obj;
  session.flags = flags;
  session.user_index = user_index;
  session.max_public = max_public;
  session.max_private = max_private;
  session.host = (flags & kHost) != 0;
  session.nonce = uint64_t(random()) << 32 | random();
  if (session.host) {
    // A new session: its key ID (XNET_XNKID_ONLINE_PEER) and key, this game as its host.
    for (auto& b : session.info.xnkid) {
      b = uint8_t(random());
    }
    session.info.xnkid[0] = uint8_t(0x80 | (session.info.xnkid[0] & 0x0F));
    for (auto& b : session.info.xnkey) {
      b = uint8_t(random());
    }
    net_->GetTitleXnAddr(&session.info.host);
    *info = session.info;
  } else {
    // Joining the session the game found (search, invite): its info as given.
    session.info = *info;
  }
  *nonce = session.nonce;
  REXSYS_INFO("online: session {:08X} created ({}), flags {:08X}, {} + {} slots, id {}", obj,
              session.host ? "host" : "joined", flags, max_public, max_private,
              KidText(session.info.xnkid));
  bool invite;
  {
    std::lock_guard lock(mutex_);
    sessions_[obj] = session;
    invite = MaybeInvite();
  }
  if (IsAdvertised(session)) {
    Advertise(0);
  }
  if (invite) {
    invite_(0);
  }
}

void SessionService::Delete(uint32_t obj) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    advertised = IsAdvertised(it->second);
    REXSYS_INFO("online: session {:08X} deleted (id {})", obj, KidText(it->second.info.xnkid));
    sessions_.erase(it);
  }
  if (advertised) {
    Advertise(0);
  }
}

void SessionService::JoinLocal(uint32_t obj, const std::vector<uint32_t>& user_indices,
                               const std::vector<bool>& is_private) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    for (size_t i = 0; i < user_indices.size(); ++i) {
      uint64_t xuid = LocalUserXuid(user_indices[i]);
      auto& members = it->second.members;
      if (std::none_of(members.begin(), members.end(),
                       [&](const SessionMember& m) { return m.xuid == xuid; })) {
        members.push_back({xuid, user_indices[i], i < is_private.size() && is_private[i]});
      }
    }
    advertised = IsAdvertised(it->second);
  }
  if (advertised) {
    Advertise(0);
  }
}

void SessionService::JoinRemote(uint32_t obj, const std::vector<uint64_t>& xuids,
                                const std::vector<bool>& is_private) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    for (size_t i = 0; i < xuids.size(); ++i) {
      auto& members = it->second.members;
      if (std::none_of(members.begin(), members.end(),
                       [&](const SessionMember& m) { return m.xuid == xuids[i]; })) {
        members.push_back({xuids[i], 0xFE, i < is_private.size() && is_private[i]});
        REXSYS_INFO("online: {:016X} joined session {:08X}", xuids[i], obj);
      }
    }
    advertised = IsAdvertised(it->second);
  }
  if (advertised) {
    Advertise(0);
  }
}

void SessionService::LeaveLocal(uint32_t obj, const std::vector<uint32_t>& user_indices) {
  std::vector<uint64_t> xuids;
  for (uint32_t user_index : user_indices) {
    xuids.push_back(LocalUserXuid(user_index));
  }
  LeaveRemote(obj, xuids);
}

void SessionService::LeaveRemote(uint32_t obj, const std::vector<uint64_t>& xuids) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    auto& members = it->second.members;
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](const SessionMember& m) {
                                   return std::find(xuids.begin(), xuids.end(), m.xuid) !=
                                          xuids.end();
                                 }),
                  members.end());
    advertised = IsAdvertised(it->second);
  }
  if (advertised) {
    Advertise(0);
  }
}

void SessionService::Modify(uint32_t obj, uint32_t flags, uint32_t max_public,
                            uint32_t max_private) {
  bool advertised = false;
  bool invite = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    // The flags that can change (invites, joining); the rest stay the creation's.
    constexpr uint32_t kModifiable = 0xF00;
    it->second.flags = (it->second.flags & ~kModifiable) | (flags & kModifiable);
    it->second.max_public = max_public;
    it->second.max_private = max_private;
    advertised = IsAdvertised(it->second);
    invite = MaybeInvite();
  }
  if (advertised) {
    Advertise(0);
  }
  if (invite) {
    invite_(0);
  }
}

void SessionService::Start(uint32_t obj) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    it->second.started = true;
    advertised = IsAdvertised(it->second);
  }
  if (advertised) {
    Advertise(0);
  }
}

void SessionService::End(uint32_t obj) {
  bool advertised = false;
  {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(obj);
    if (it == sessions_.end()) {
      return;
    }
    it->second.started = false;
    advertised = IsAdvertised(it->second);
  }
  if (advertised) {
    Advertise(0);
  }
}

std::optional<LocalSession> SessionService::Details(uint32_t obj) const {
  std::lock_guard lock(mutex_);
  auto it = sessions_.find(obj);
  if (it == sessions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<AdvertisedSession> SessionService::Search(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts, uint32_t max_results) const {
  std::vector<AdvertisedSession> results;
  std::lock_guard lock(mutex_);
  for (const auto& [peer, sessions] : remote_) {
    for (const AdvertisedSession& session : sessions) {
      if (!(session.flags & kUsesMatchmaking)) {
        continue;
      }
      if (session.started && (session.flags & kJoinInProgressDisabled)) {
        continue;
      }
      uint32_t filled = session.filled_public;
      if (session.max_public <= filled) {
        continue;
      }
      // Ranked or not, and the game mode, where both say.
      bool matches = true;
      for (const auto& [id, value] : contexts) {
        if (id != kContextGameType && id != kContextGameMode) {
          continue;
        }
        for (const auto& [session_id, session_value] : session.contexts) {
          if (session_id == id && session_value != value) {
            matches = false;
          }
        }
      }
      if (matches && results.size() < max_results) {
        results.push_back(session);
      }
    }
  }
  return results;
}

std::optional<AdvertisedSession> SessionService::FindById(
    const std::array<uint8_t, kXnKidSize>& xnkid) const {
  std::lock_guard lock(mutex_);
  for (const auto& [peer, sessions] : remote_) {
    for (const AdvertisedSession& session : sessions) {
      if (session.info.xnkid == xnkid) {
        return session;
      }
    }
  }
  return std::nullopt;
}

std::optional<AcceptedInvite> SessionService::accepted_invite() const {
  std::lock_guard lock(mutex_);
  return accepted_invite_;
}

std::vector<uint8_t> SessionService::EncodeSessions() const {
  std::vector<uint8_t> out;
  out.push_back(kMessageSessions);
  std::vector<const LocalSession*> advertised;
  for (const auto& [obj, session] : sessions_) {
    if (IsAdvertised(session)) {
      advertised.push_back(&session);
    }
  }
  Put16(out, uint16_t(advertised.size()));
  for (const LocalSession* session : advertised) {
    out.insert(out.end(), session->info.xnkid.begin(), session->info.xnkid.end());
    out.insert(out.end(), session->info.xnkey.begin(), session->info.xnkey.end());
    auto* host = reinterpret_cast<const uint8_t*>(&session->info.host);
    out.insert(out.end(), host, host + sizeof(XNADDR));
    uint32_t filled_public = 0, filled_private = 0;
    for (const SessionMember& member : session->members) {
      ++(member.is_private ? filled_private : filled_public);
    }
    Put32(out, session->flags);
    Put32(out, session->max_public);
    Put32(out, session->max_private);
    Put32(out, filled_public);
    Put32(out, filled_private);
    out.push_back(session->started ? 1 : 0);
    // The host user's contexts and properties, and the host's name.
    auto user = users_.find(session->user_index);
    std::vector<std::pair<uint32_t, uint32_t>> contexts;
    std::vector<SessionProperty> properties;
    if (user != users_.end()) {
      contexts.assign(user->second.contexts.begin(), user->second.contexts.end());
      for (const auto& [id, value] : user->second.properties) {
        properties.push_back({id, value});
      }
    }
    properties.push_back({kPropertyHostName, Utf16Be(self_name_)});
    Put16(out, uint16_t(contexts.size()));
    for (const auto& [id, value] : contexts) {
      Put32(out, id);
      Put32(out, value);
    }
    Put16(out, uint16_t(properties.size()));
    for (const SessionProperty& property : properties) {
      Put32(out, property.id);
      Put16(out, uint16_t(property.value.size()));
      out.insert(out.end(), property.value.begin(), property.value.end());
    }
  }
  return out;
}

std::vector<AdvertisedSession> SessionService::DecodeSessions(const uint8_t* data, size_t size,
                                                              uint64_t owner) {
  std::vector<AdvertisedSession> sessions;
  Reader r{data, size};
  if (r.U8() != kMessageSessions) {
    return sessions;
  }
  uint16_t count = r.U16();
  for (uint16_t i = 0; i < count && r.ok; ++i) {
    AdvertisedSession session;
    session.owner = owner;
    const uint8_t* kid = r.Bytes(kXnKidSize);
    const uint8_t* key = r.Bytes(kXnKeySize);
    const uint8_t* host = r.Bytes(sizeof(XNADDR));
    if (!r.ok) {
      break;
    }
    std::memcpy(session.info.xnkid.data(), kid, kXnKidSize);
    std::memcpy(session.info.xnkey.data(), key, kXnKeySize);
    std::memcpy(&session.info.host, host, sizeof(XNADDR));
    session.flags = r.U32();
    session.max_public = r.U32();
    session.max_private = r.U32();
    session.filled_public = r.U32();
    session.filled_private = r.U32();
    session.started = r.U8() != 0;
    uint16_t contexts = r.U16();
    for (uint16_t k = 0; k < contexts && r.ok; ++k) {
      uint32_t id = r.U32();
      uint32_t value = r.U32();
      session.contexts.push_back({id, value});
    }
    uint16_t properties = r.U16();
    for (uint16_t k = 0; k < properties && r.ok; ++k) {
      SessionProperty property;
      property.id = r.U32();
      uint16_t length = r.U16();
      const uint8_t* value = r.Bytes(length);
      if (value) {
        property.value.assign(value, value + length);
      }
      session.properties.push_back(std::move(property));
    }
    if (r.ok) {
      sessions.push_back(std::move(session));
    }
  }
  return sessions;
}

void SessionService::Advertise(uint64_t to) {
  std::vector<uint8_t> message;
  {
    std::lock_guard lock(mutex_);
    message = EncodeSessions();
  }
  std::vector<uint64_t> peers = to ? std::vector<uint64_t>{to} : net_->ConnectedPeers();
  for (uint64_t peer : peers) {
    net_->SendControl(peer, kChannel, message.data(), message.size());
  }
}

void SessionService::OnControl(uint64_t peer, const uint8_t* data, size_t size) {
  if (!size) {
    return;
  }
  if (data[0] == kMessageRequest) {
    Advertise(peer);
    return;
  }
  if (data[0] == kMessageInvite) {
    OnInvite(peer, data, size);
    return;
  }
  if (data[0] != kMessageSessions) {
    return;
  }
  std::vector<AdvertisedSession> sessions = DecodeSessions(data, size, peer);
  bool invite;
  {
    std::lock_guard lock(mutex_);
    REXSYS_DEBUG("online: {:016X} hosts {} session(s)", peer, sessions.size());
    remote_[peer] = std::move(sessions);
    invite = MaybeAcceptPending() || MaybeInvite();
  }
  if (invite) {
    invite_(0);
  }
}

void SessionService::OnPeer(uint64_t peer, bool connected) {
  if (!connected) {
    std::lock_guard lock(mutex_);
    remote_.erase(peer);
    return;
  }
  // Ours to it, and its to us.
  Advertise(peer);
  uint8_t request = kMessageRequest;
  net_->SendControl(peer, kChannel, &request, 1);
  // Joining an inviter's group through its hub: the inviter, reached through the hub, too.
  uint64_t inviter = 0;
  {
    std::lock_guard lock(mutex_);
    if (pending_invite_ && pending_invite_->inviter != peer && peer == net_->hub()) {
      inviter = pending_invite_->inviter;
    }
  }
  if (inviter) {
    net_->SendControl(inviter, kChannel, &request, 1);
  }
}

void SessionService::SetInvitePrompt(InvitePrompt prompt) {
  std::lock_guard lock(invite_prompt_mutex);
  invite_prompt = std::move(prompt);
}

std::vector<Friend> SessionService::Friends() const {
  std::vector<Friend> friends;
  std::vector<uint64_t> peers = net_->ConnectedPeers();
  std::lock_guard lock(mutex_);
  for (uint64_t peer : peers) {
    Friend f;
    f.xuid = peer;
    f.name = net_->PeerName(peer);
    auto remote = remote_.find(peer);
    if (remote != remote_.end()) {
      for (const AdvertisedSession& session : remote->second) {
        if ((session.flags & kUsesPresence) && !(session.flags & kInvitesDisabled)) {
          f.has_party = true;
          f.party = session.info;
          break;
        }
      }
    }
    friends.push_back(std::move(f));
  }
  if (external_.list) {
    for (Friend& f : external_.list()) {
      bool listed = f.xuid == self_xuid_ ||
                    std::any_of(friends.begin(), friends.end(),
                                [&](const Friend& known) { return known.xuid == f.xuid; });
      if (!listed) {
        friends.push_back(std::move(f));
      }
    }
  }
  return friends;
}

void SessionService::SetExternalFriends(ExternalFriends friends) {
  std::lock_guard lock(mutex_);
  external_ = std::move(friends);
}

bool SessionService::SendInvite(uint64_t invitee) {
  std::vector<uint8_t> message;
  {
    std::lock_guard lock(mutex_);
    // The party this game is in: a session with presence and a peer network, hosted or joined.
    const LocalSession* party = nullptr;
    for (const auto& [obj, session] : sessions_) {
      if ((session.flags & kUsesPresence) && (session.flags & kUsesPeerNetwork)) {
        party = &session;
        if (session.host) {
          break;
        }
      }
    }
    if (!party) {
      REXSYS_WARN("online: an invite to {:016X} with no party to invite to", invitee);
      return false;
    }
    message.push_back(kMessageInvite);
    message.insert(message.end(), party->info.xnkid.begin(), party->info.xnkid.end());
    message.insert(message.end(), party->info.xnkey.begin(), party->info.xnkey.end());
    auto* host = reinterpret_cast<const uint8_t*>(&party->info.host);
    message.insert(message.end(), host, host + sizeof(XNADDR));
    std::string name = self_name_.substr(0, 255);
    message.push_back(uint8_t(name.size()));
    message.insert(message.end(), name.begin(), name.end());
  }
  std::vector<uint64_t> peers = net_->ConnectedPeers();
  if (std::find(peers.begin(), peers.end(), invitee) == peers.end()) {
    std::function<bool(uint64_t, const std::vector<uint8_t>&)> external;
    {
      std::lock_guard lock(mutex_);
      external = external_.invite;
    }
    // Through Steam: the inviter's XUID, then the message.
    std::vector<uint8_t> carried;
    for (int b = 0; b < 8; ++b) {
      carried.push_back(uint8_t(self_xuid_ >> (8 * b)));
    }
    carried.insert(carried.end(), message.begin(), message.end());
    if (external && external(invitee, carried)) {
      REXSYS_INFO("online: invited {:016X} through Steam", invitee);
      return true;
    }
    REXSYS_WARN("online: an invite to {:016X}, who is not connected", invitee);
    return false;
  }
  net_->SendControl(invitee, kChannel, message.data(), message.size());
  REXSYS_INFO("online: invited \"{}\" ({:016X})", net_->PeerName(invitee), invitee);
  return true;
}

bool SessionService::ParseInvite(const uint8_t* data, size_t size, SessionInfo* party,
                                 std::string* name) {
  Reader r{data, size};
  if (r.U8() != kMessageInvite) {
    return false;
  }
  const uint8_t* kid = r.Bytes(kXnKidSize);
  const uint8_t* key = r.Bytes(kXnKeySize);
  const uint8_t* host = r.Bytes(sizeof(XNADDR));
  uint8_t name_length = r.U8();
  const uint8_t* text = r.Bytes(name_length);
  if (!r.ok) {
    return false;
  }
  std::memcpy(party->xnkid.data(), kid, kXnKidSize);
  std::memcpy(party->xnkey.data(), key, kXnKeySize);
  std::memcpy(&party->host, host, sizeof(XNADDR));
  name->assign(reinterpret_cast<const char*>(text), name_length);
  return true;
}

void SessionService::AskAboutInvite(const std::string& inviter, AcceptedInvite invite,
                                    std::function<void(const AcceptedInvite&)> accepted) {
  InvitePrompt prompt;
  {
    std::lock_guard lock(invite_prompt_mutex);
    prompt = invite_prompt;
  }
  REXSYS_INFO("online: \"{}\" ({:016X}) invites this game to its party", inviter, invite.inviter);
  if (!prompt) {
    REXSYS_INFO("online: no invite prompt: the invite is declined");
    return;
  }
  prompt(inviter, [invite, accepted = std::move(accepted)](bool accept) {
    if (!accept) {
      REXSYS_INFO("online: the invite from {:016X} declined", invite.inviter);
      return;
    }
    accepted(invite);
  });
}

void SessionService::OnInvite(uint64_t peer, const uint8_t* data, size_t size) {
  AcceptedInvite invite;
  std::string inviter;
  if (!ParseInvite(data, size, &invite.info, &inviter)) {
    return;
  }
  invite.user_index = 0;
  invite.invitee = LocalUserXuid(0);
  invite.inviter = peer;
  AskAboutInvite(inviter, invite, [this](const AcceptedInvite& invite) {
    {
      std::lock_guard lock(mutex_);
      accepted_invite_ = invite;
      invited_.insert(invite.info.xnkid);
      pending_invite_.reset();
    }
    REXSYS_INFO("online: the invite from {:016X} accepted: the game joins its party", invite.inviter);
    invite_(invite.user_index);
  });
}

void SessionService::OnExternalInvite(const uint8_t* data, size_t size,
                                      std::function<void()> join) {
  if (size < 8) {
    return;
  }
  uint64_t inviter_xuid = 0;
  for (int b = 0; b < 8; ++b) {
    inviter_xuid |= uint64_t(data[b]) << (8 * b);
  }
  AcceptedInvite invite;
  std::string inviter;
  if (!ParseInvite(data + 8, size - 8, &invite.info, &inviter)) {
    return;
  }
  invite.user_index = 0;
  invite.invitee = LocalUserXuid(0);
  invite.inviter = inviter_xuid;
  AskAboutInvite(inviter, invite, [this, join = std::move(join)](const AcceptedInvite& invite) {
    bool known;
    {
      std::lock_guard lock(mutex_);
      invited_.insert(invite.info.xnkid);
      known = KnowsSession(invite.info.xnkid);
      if (known) {
        accepted_invite_ = invite;
        pending_invite_.reset();
      } else {
        pending_invite_ = invite;
        pending_since_ = std::chrono::steady_clock::now();
      }
    }
    if (known) {
      REXSYS_INFO("online: the invite from {:016X} accepted: the game joins its party",
                  invite.inviter);
      invite_(invite.user_index);
      return;
    }
    REXSYS_INFO("online: the invite from {:016X} accepted: joining its group first",
                invite.inviter);
    join();
    // Already connected to the inviter (one group): ask for its sessions now.
    std::vector<uint64_t> peers = net_->ConnectedPeers();
    if (std::find(peers.begin(), peers.end(), invite.inviter) != peers.end()) {
      uint8_t request = kMessageRequest;
      net_->SendControl(invite.inviter, kChannel, &request, 1);
    }
  });
}

bool SessionService::KnowsSession(const std::array<uint8_t, kXnKidSize>& xnkid) const {
  for (const auto& [peer, sessions] : remote_) {
    for (const AdvertisedSession& session : sessions) {
      if (session.info.xnkid == xnkid) {
        return true;
      }
    }
  }
  return false;
}

bool SessionService::MaybeAcceptPending() {
  if (!pending_invite_) {
    return false;
  }
  if (std::chrono::steady_clock::now() - pending_since_ > kPendingInviteTimeout) {
    REXSYS_WARN("online: the party of the invite from {:016X} was never found; the invite is dropped",
                pending_invite_->inviter);
    pending_invite_.reset();
    return false;
  }
  if (!KnowsSession(pending_invite_->info.xnkid)) {
    return false;
  }
  accepted_invite_ = pending_invite_;
  pending_invite_.reset();
  REXSYS_INFO("online: the invite from {:016X}: its party found, the game joins it",
              accepted_invite_->inviter);
  return true;
}

bool SessionService::MaybeInvite() {
  if (pending_invite_) {
    // Joining an inviter's party, not the hub's.
    return false;
  }
  const uint64_t hub = net_->hub();
  if (!net_->hub_endpoint().valid() || !hub) {
    return false;
  }
  // In its own online lobby: a party it hosts.
  bool in_lobby = std::any_of(sessions_.begin(), sessions_.end(), [](const auto& entry) {
    const LocalSession& s = entry.second;
    return s.host && (s.flags & kUsesPresence) && (s.flags & kUsesPeerNetwork);
  });
  if (!in_lobby) {
    return false;
  }
  auto remote = remote_.find(hub);
  if (remote == remote_.end()) {
    return false;
  }
  for (const AdvertisedSession& party : remote->second) {
    if (!(party.flags & kUsesPresence) || (party.flags & kInvitesDisabled) ||
        invited_.count(party.info.xnkid)) {
      continue;
    }
    // Not one this game already joined.
    bool joined = std::any_of(sessions_.begin(), sessions_.end(), [&](const auto& entry) {
      return entry.second.info.xnkid == party.info.xnkid;
    });
    if (joined) {
      continue;
    }
    invited_.insert(party.info.xnkid);
    AcceptedInvite invite;
    invite.user_index = 0;
    invite.invitee = LocalUserXuid(0);
    invite.inviter = hub;
    invite.info = party.info;
    accepted_invite_ = invite;
    REXSYS_INFO("online: invited to \"{}\"'s party (id {}): the game joins it",
                net_->PeerName(hub), KidText(party.info.xnkid));
    return true;
  }
  return false;
}

}  // namespace rex::system::online
