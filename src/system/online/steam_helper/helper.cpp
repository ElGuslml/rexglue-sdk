/**
 * ReXGlue online play through Steam: the helper process's logic (see helper.h and
 * rex/system/online/steam_link.h).
 */

#include "helper.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

#include <rex/system/online/steam_link.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::system::online::steam_helper {

namespace {

using namespace steam_link;
using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
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

constexpr int kLobbyMembers = 8;
constexpr auto kFriendsInterval = std::chrono::seconds(2);
constexpr auto kPresenceRequestInterval = std::chrono::seconds(15);
constexpr auto kCallbacksInterval = std::chrono::milliseconds(10);
constexpr auto kParentInterval = std::chrono::milliseconds(500);
constexpr auto kLobbyRetry = std::chrono::seconds(10);
// The helpers' own messages on kChannelHelper.
constexpr uint8_t kHelperInvite = 1;  // u64 lobby, the invite
// u32 title, u64 xuid, u64 lobby, str name, str persona: this game is running, told to a friend seen in this app.
// Steam does not always show a friend's game both ways (a profile whose game details are private shows as online,
// not in a game), so one side seeing the other is enough for both friends lists.
constexpr uint8_t kHelperHere = 2;
constexpr auto kHereInterval = std::chrono::seconds(5);
// A friend heard from stays listed this long after its last word.
constexpr auto kHeardFor = std::chrono::seconds(16);

struct FriendEntry {
  uint64_t steam_id = 0;
  uint64_t xuid = 0;
  std::string name;
  std::string persona;
  uint64_t lobby = 0;
  bool operator==(const FriendEntry& o) const {
    return steam_id == o.steam_id && xuid == o.xuid && name == o.name && persona == o.persona &&
           lobby == o.lobby;
  }
};

uint64_t ParseHex(const std::string& text) {
  if (text.empty() || text.size() > 16) {
    return 0;
  }
  uint64_t v = 0;
  for (char c : text) {
    int d = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : -1;
    if (d < 0) {
      return 0;
    }
    v = v << 4 | uint64_t(d);
  }
  return v;
}

std::string Hex(uint64_t v, int digits) {
  char text[24];
  std::snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(v));
  return text;
}

// "+connect_lobby <id>": the rich presence's connect, which Steam's friends list joins.
uint64_t LobbyFromConnect(const std::string& connect) {
  const std::string prefix = "+connect_lobby ";
  if (connect.compare(0, prefix.size(), prefix) != 0) {
    return 0;
  }
  return std::strtoull(connect.c_str() + prefix.size(), nullptr, 10);
}

class Helper {
 public:
  Helper(Backend* backend, const Options& options) : backend_(backend), options_(options) {}

  ~Helper() {
    if (socket_ != kNoSocket) {
      CloseNative(socket_);
    }
#if defined(_WIN32)
    if (wsa_started_) {
      WSACleanup();
    }
#endif
  }

  int Run(std::atomic<bool>* stop) {
    if (!OpenSocket()) {
      std::fprintf(stderr, "rexsteam: no loopback socket\n");
      return 2;
    }
    auto next_parent = Clock::now();
    auto next_announce = Clock::now();
    while (!done_ && !(stop && *stop)) {
      Wait(2);
      ReadGame();
      const auto now = Clock::now();
      if (!hello_ && now >= next_announce) {
        // The game learns this helper's port from it.
        next_announce = now + std::chrono::seconds(1);
        Writer w(Message::kAnnounce);
        w.U64(options_.token);
        SendGame(w);
      }
      if (started_) {
        if (now >= next_callbacks_) {
          next_callbacks_ = now + kCallbacksInterval;
          backend_->RunCallbacks(events_);
        }
        backend_->Receive(kChannelFrames, [this](uint64_t from, const uint8_t* data, size_t size) {
          Writer w(Message::kReceive);
          w.U64(options_.token);
          w.U64(from);
          w.Bytes(data, size);
          SendGame(w);
        });
        backend_->Receive(kChannelHelper, [this](uint64_t from, const uint8_t* data, size_t size) {
          OnHelperMessage(from, data, size);
        });
        if (!lobby_ && !joining_ && !creating_ && now >= lobby_retry_) {
          CreateOwnLobby();
        }
        if (now >= next_friends_ || (friends_dirty_ && now >= friends_dirty_at_)) {
          next_friends_ = now + kFriendsInterval;
          friends_dirty_ = false;
          ScanFriends(false);
        }
      }
      if (options_.parent_alive && now >= next_parent) {
        next_parent = now + kParentInterval;
        if (!options_.parent_alive()) {
          break;
        }
      }
    }
    if (started_) {
      if (lobby_) {
        backend_->LeaveLobby(lobby_);
      }
      backend_->Shutdown();
    }
    return 0;
  }

 private:
  bool OpenSocket() {
#if defined(_WIN32)
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
#if defined(_WIN32)
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
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      return false;
    }
    game_.sin_family = AF_INET;
    game_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    game_.sin_port = htons(options_.game_port);
    return true;
  }

  void Wait(int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);
    timeval timeout{0, timeout_ms * 1000};
    ::select(int(socket_) + 1, &read_set, nullptr, nullptr, &timeout);
  }

  void SendGame(const Writer& w) {
    ::sendto(socket_, reinterpret_cast<const char*>(w.out.data()), int(w.out.size()), 0,
             reinterpret_cast<const sockaddr*>(&game_), sizeof(game_));
  }

  void Log(int level, const std::string& text) {
    Writer w(Message::kLog);
    w.U64(options_.token);
    w.U8(uint8_t(level));
    w.Str(text);
    SendGame(w);
  }

  void ReadGame() {
    for (int i = 0; i < 256; ++i) {
      sockaddr_in from{};
      socklen_t from_length = sizeof(from);
      int size = ::recvfrom(socket_, reinterpret_cast<char*>(buffer_), sizeof(buffer_), 0,
                            reinterpret_cast<sockaddr*>(&from), &from_length);
      if (size <= 0) {
        return;
      }
      // Only the game that started this helper.
      if (from.sin_addr.s_addr != game_.sin_addr.s_addr || from.sin_port != game_.sin_port) {
        continue;
      }
      OnGameMessage(buffer_, size_t(size));
    }
  }

  void OnGameMessage(const uint8_t* data, size_t size) {
    Reader r{data, size};
    switch (Message(Reader::TypeOf(data, size))) {
      case Message::kHello: {
        uint32_t version = r.U32();
        uint64_t token = r.U64();
        uint32_t title = r.U32();
        uint32_t protocol = r.U32();
        uint64_t xuid = r.U64();
        std::string name = r.Str();
        if (!r.ok || token != options_.token) {
          return;
        }
        if (version != kVersion) {
          error_ = "the game and rexsteam are of different versions";
          failed_ = true;
        }
        hello_ = true;
        OnHello(title, protocol, xuid, name);
        break;
      }
      case Message::kSend: {
        uint64_t to = r.U64();
        size_t n = 0;
        const uint8_t* frame = r.Rest(&n);
        if (r.ok && started_ && n) {
          backend_->Send(to, kChannelFrames, frame, n, false);
        }
        break;
      }
      case Message::kJoinLobby: {
        uint64_t lobby = r.U64();
        if (r.ok && started_ && lobby) {
          JoinLobby(lobby, "the player accepted an invite");
        }
        break;
      }
      case Message::kInvite: {
        uint64_t to = r.U64();
        size_t n = 0;
        const uint8_t* invite = r.Rest(&n);
        if (!r.ok || !started_) {
          break;
        }
        if (!lobby_) {
          Log(1, "an invite with no lobby yet: not sent");
          break;
        }
        std::vector<uint8_t> message;
        message.push_back(kHelperInvite);
        for (int b = 0; b < 8; ++b) {
          message.push_back(uint8_t(lobby_ >> (8 * b)));
        }
        message.insert(message.end(), invite, invite + n);
        bool sent = backend_->Send(to, kChannelHelper, message.data(), message.size(), true);
        Log(sent ? 0 : 1, std::string("invite to Steam user ") + std::to_string(to) +
                              (sent ? " sent" : " not sent"));
        break;
      }
      case Message::kBye:
        done_ = true;
        break;
      default:
        break;
    }
  }

  void OnHello(uint32_t title, uint32_t protocol, uint64_t xuid, const std::string& name) {
    if (!started_ && !failed_) {
      title_ = title;
      protocol_ = protocol;
      xuid_ = xuid;
      name_ = name;
      std::string error;
      if (backend_->Init(&error)) {
        started_ = true;
        events_.lobby_entered = [this](bool ok, uint64_t lobby) { OnLobbyEntered(ok, lobby); };
        events_.lobby_changed = [this](uint64_t lobby) {
          if (lobby == lobby_) {
            ReportLobby();
          }
        };
        events_.join_requested = [this](uint64_t lobby) {
          JoinLobby(lobby, "the player chose to join in Steam");
        };
        events_.session_requested = [this](uint64_t user) {
          bool known = backend_->IsFriend(user) || (lobby_ && backend_->IsLobbyMember(lobby_, user));
          if (known) {
            backend_->AcceptSession(user);
          } else {
            Log(1, "a message from Steam user " + std::to_string(user) +
                       ", neither a friend nor in the lobby: ignored");
          }
        };
        events_.friends_changed = [this] {
          if (!friends_dirty_) {
            friends_dirty_ = true;
            friends_dirty_at_ = Clock::now() + std::chrono::milliseconds(250);
          }
        };
        backend_->SetRichPresence(kKeyTitle, Hex(title_, 8));
        backend_->SetRichPresence(kKeyXuid, Hex(xuid_, 16));
        backend_->SetRichPresence(kKeyName, name_);
        Log(0, "Steam started as app " + std::to_string(kAppId) + ", user " +
                   std::to_string(backend_->SelfId()) + " (\"" + backend_->SelfName() + "\")");
        CreateOwnLobby();
      } else {
        failed_ = true;
        error_ = error;
      }
    }
    Writer w(Message::kStatus);
    w.U64(options_.token);
    w.U8(uint8_t(started_ ? Status::kReady : Status::kFailed));
    w.U64(started_ ? backend_->SelfId() : 0);
    w.Str(started_ ? backend_->SelfName() : std::string());
    w.Str(error_);
    SendGame(w);
    if (started_) {
      // A hello again (the game restarted its network): everything once more.
      reported_lobby_ = reported_owner_ = ~0ull;
      if (lobby_) {
        ReportLobby();
      }
      ScanFriends(true);
    }
  }

  void CreateOwnLobby() {
    if (lobby_) {
      backend_->LeaveLobby(lobby_);
      lobby_ = 0;
    }
    creating_ = true;
    joining_ = 0;
    backend_->CreateLobby(kLobbyMembers);
  }

  void JoinLobby(uint64_t lobby, const char* why) {
    if (lobby == lobby_ || lobby == joining_) {
      return;
    }
    Log(0, std::string("joining lobby ") + std::to_string(lobby) + ": " + why);
    if (lobby_) {
      backend_->LeaveLobby(lobby_);
      lobby_ = 0;
    }
    creating_ = false;
    joining_ = lobby;
    backend_->JoinLobby(lobby);
  }

  void OnLobbyEntered(bool ok, uint64_t lobby) {
    if (!ok) {
      if (joining_) {
        Log(1, "could not join lobby " + std::to_string(joining_) + "; a lobby of this game's own");
        joining_ = 0;
        CreateOwnLobby();
      } else if (creating_) {
        Log(1, "could not create a Steam lobby; trying again in 10 s");
        creating_ = false;
        lobby_retry_ = Clock::now() + kLobbyRetry;
      }
      return;
    }
    if (lobby == lobby_) {
      return;
    }
    if (!creating_ && joining_ != lobby) {
      // A late answer for a lobby no longer wanted.
      backend_->LeaveLobby(lobby);
      return;
    }
    const bool created = creating_;
    creating_ = false;
    joining_ = 0;
    lobby_ = lobby;
    const std::string title = Hex(title_, 8);
    if (created) {
      backend_->SetLobbyData(lobby, kKeyTitle, title);
      backend_->SetLobbyData(lobby, kKeyProtocol, std::to_string(protocol_));
    } else if (backend_->LobbyData(lobby, kKeyTitle) != title ||
               backend_->LobbyData(lobby, kKeyProtocol) != std::to_string(protocol_)) {
      Log(1, "lobby " + std::to_string(lobby) +
                 " is another game's or another version's: left");
      CreateOwnLobby();
      return;
    }
    backend_->SetRichPresence("connect", "+connect_lobby " + std::to_string(lobby));
    Log(0, std::string(created ? "created" : "joined") + " lobby " + std::to_string(lobby));
    ReportLobby();
  }

  void ReportLobby() {
    uint64_t owner = lobby_ ? backend_->LobbyOwner(lobby_) : 0;
    if (lobby_ == reported_lobby_ && owner == reported_owner_) {
      return;
    }
    reported_lobby_ = lobby_;
    reported_owner_ = owner;
    Writer w(Message::kLobby);
    w.U64(options_.token);
    w.U64(lobby_);
    w.U64(owner);
    SendGame(w);
  }

  // A friend's game is running (kHelperHere): listed for kHeardFor even if Steam does not show its game here.
  void OnHere(uint64_t from, const uint8_t* data, size_t size) {
    if (!backend_->IsFriend(from)) {
      return;
    }
    Reader r{data, size};
    r.at = 1;
    FriendEntry entry;
    uint32_t title = r.U32();
    entry.steam_id = from;
    entry.xuid = r.U64();
    entry.lobby = r.U64();
    entry.name = r.Str();
    entry.persona = r.Str();
    if (!r.ok || title != title_ || !entry.xuid) {
      return;
    }
    Heard& heard = heard_[from];
    if (!(heard.entry == entry)) {
      if (!heard.entry.xuid) {
        Log(0, "Steam user " + std::to_string(from) + " (\"" + entry.persona + "\") is in this game");
      }
      heard.entry = entry;
      friends_dirty_ = true;
      friends_dirty_at_ = Clock::now();
    }
    heard.until = Clock::now() + kHeardFor;
  }

  // Tells a friend this game is running (throttled; at once when this game's lobby changed).
  void SayHere(uint64_t to, Clock::time_point now) {
    Said& said = said_[to];
    if (now < said.next && said.lobby == lobby_) {
      return;
    }
    said.next = now + kHereInterval;
    said.lobby = lobby_;
    Writer w(static_cast<Message>(kHelperHere));
    w.U32(title_);
    w.U64(xuid_);
    w.U64(lobby_);
    w.Str(name_);
    w.Str(backend_->SelfName());
    // Without the magic: the helpers' messages start with their type.
    backend_->Send(to, kChannelHelper, w.out.data() + 4, w.out.size() - 4, true);
  }

  void OnHelperMessage(uint64_t from, const uint8_t* data, size_t size) {
    if (size >= 1 && data[0] == kHelperHere) {
      OnHere(from, data, size);
      return;
    }
    if (size < 9 || data[0] != kHelperInvite) {
      return;
    }
    if (!backend_->IsFriend(from) && !(lobby_ && backend_->IsLobbyMember(lobby_, from))) {
      return;
    }
    uint64_t lobby = 0;
    for (int b = 0; b < 8; ++b) {
      lobby |= uint64_t(data[1 + b]) << (8 * b);
    }
    Writer w(Message::kInvited);
    w.U64(options_.token);
    w.U64(from);
    w.U64(lobby);
    w.Bytes(data + 9, size - 9);
    SendGame(w);
  }

  void ScanFriends(bool force) {
    const auto now = Clock::now();
    const std::string title = Hex(title_, 8);
    std::vector<FriendEntry> list;
    for (const Backend::FriendInfo& info : backend_->FriendsInApp()) {
      std::string their_title = backend_->FriendPresence(info.id, kKeyTitle);
      auto& requested = presence_requested_[info.id];
      if (their_title.empty()) {
        if (now >= requested) {
          requested = now + kPresenceRequestInterval;
          backend_->RequestFriendPresence(info.id);
        }
        // Its game may be this one: told, it can list this game even if it does not see it.
        SayHere(info.id, now);
        continue;
      }
      if (their_title != title) {
        continue;
      }
      SayHere(info.id, now);
      FriendEntry entry;
      entry.steam_id = info.id;
      entry.xuid = ParseHex(backend_->FriendPresence(info.id, kKeyXuid));
      entry.name = backend_->FriendPresence(info.id, kKeyName);
      entry.persona = info.persona;
      entry.lobby = info.lobby ? info.lobby
                               : LobbyFromConnect(backend_->FriendPresence(info.id, "connect"));
      if (!entry.xuid) {
        continue;
      }
      list.push_back(std::move(entry));
    }
    // The friends that said they are in this game and Steam does not show here; this game tells them it is too.
    for (auto it = heard_.begin(); it != heard_.end();) {
      if (now >= it->second.until) {
        Log(0, "Steam user " + std::to_string(it->first) + " no longer heard from");
        it = heard_.erase(it);
        continue;
      }
      SayHere(it->first, now);
      bool listed = std::any_of(list.begin(), list.end(),
                                [&](const FriendEntry& e) { return e.steam_id == it->first; });
      if (!listed) {
        list.push_back(it->second.entry);
      }
      ++it;
    }
    std::sort(list.begin(), list.end(),
              [](const FriendEntry& a, const FriendEntry& b) { return a.steam_id < b.steam_id; });
    if (!force && list == friends_) {
      return;
    }
    friends_ = list;
    Writer w(Message::kFriends);
    w.U64(options_.token);
    w.U16(uint16_t(list.size()));
    for (const FriendEntry& entry : list) {
      w.U64(entry.steam_id);
      w.U64(entry.xuid);
      w.Str(entry.name);
      w.Str(entry.persona);
      w.U64(entry.lobby);
    }
    SendGame(w);
  }

  Backend* backend_;
  Options options_;
  Backend::Events events_;
  NativeSocket socket_ = kNoSocket;
  bool wsa_started_ = false;
  sockaddr_in game_{};
  uint8_t buffer_[2048];

  bool done_ = false;
  bool hello_ = false;
  bool started_ = false;
  bool failed_ = false;
  std::string error_;
  uint32_t title_ = 0;
  uint32_t protocol_ = 0;
  uint64_t xuid_ = 0;
  std::string name_;

  uint64_t lobby_ = 0;
  uint64_t joining_ = 0;
  bool creating_ = false;
  Clock::time_point lobby_retry_{};
  uint64_t reported_lobby_ = ~0ull;
  uint64_t reported_owner_ = ~0ull;

  std::vector<FriendEntry> friends_;
  std::map<uint64_t, Clock::time_point> presence_requested_;
  struct Heard {
    FriendEntry entry;
    Clock::time_point until{};
  };
  std::map<uint64_t, Heard> heard_;
  struct Said {
    Clock::time_point next{};
    uint64_t lobby = 0;
  };
  std::map<uint64_t, Said> said_;
  Clock::time_point next_friends_{};
  Clock::time_point next_callbacks_{};
  bool friends_dirty_ = false;
  Clock::time_point friends_dirty_at_{};
};

}  // namespace

int Run(Backend* backend, const Options& options, std::atomic<bool>* stop) {
  Helper helper(backend, options);
  return helper.Run(stop);
}

}  // namespace rex::system::online::steam_helper
