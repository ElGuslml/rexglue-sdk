/**
 * ReXGlue online play through Steam: the game's side of its Steam helper (see steam_bridge.h).
 */

#include <rex/system/online/steam_bridge.h>

#include <algorithm>
#include <chrono>
#include <random>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/online/session_service.h>
#include <rex/system/online/steam_link.h>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

namespace rex::system::online {

namespace {

using namespace steam_link;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr uint32_t kLoopback = 0x7F000001;

bool LaunchRexsteam(uint16_t port, uint64_t token) {
#if REX_PLATFORM_WIN32
  wchar_t path[MAX_PATH * 4] = {};
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  std::wstring folder(path, n);
  folder = folder.substr(0, folder.find_last_of(L"\\/") + 1);
  std::wstring exe = folder + L"rexsteam.exe";
  const std::string token_text = fmt::format("{:016X}", token);
  std::wstring command = L"\"" + exe + L"\" --port " + std::to_wstring(port) + L" --token " +
                         std::wstring(token_text.begin(), token_text.end()) + L" --parent " +
                         std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, folder.c_str(), &startup, &process)) {
    REXSYS_ERROR("Online play: rexsteam.exe could not start (error {}); Steam is off",
                 GetLastError());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  (void)port;
  (void)token;
  REXSYS_ERROR("Online play: Steam is not supported on this platform yet");
  return false;
#endif
}

}  // namespace

SteamBridge::SteamBridge(std::unique_ptr<Transport> inner, uint16_t port)
    : inner_(std::move(inner)), port_(port) {
  std::random_device random;
  token_ = (uint64_t(random()) << 32) | random();
}

SteamBridge::~SteamBridge() {
  std::lock_guard lock(mutex_);
  if (helper_.valid()) {
    Writer w(Message::kBye);
    inner_->SendTo(helper_, w.out.data(), w.out.size());
  }
}

void SteamBridge::Attach(VirtualNet* net, SessionService* sessions) {
  net_ = net;
  sessions_ = sessions;
  SessionService::ExternalFriends external;
  external.list = [this] {
    std::vector<online::Friend> list;
    for (const Friend& f : Friends()) {
      online::Friend entry;
      entry.xuid = f.xuid;
      entry.name = f.name.empty() ? f.persona : f.name;
      list.push_back(std::move(entry));
    }
    return list;
  };
  external.invite = [this](uint64_t xuid, const std::vector<uint8_t>& invite) {
    return Invite(xuid, invite);
  };
  sessions_->SetExternalFriends(std::move(external));
}

bool SteamBridge::Start(uint32_t title, uint32_t protocol, uint64_t xuid, const std::string& name,
                        Launcher launcher) {
  {
    std::lock_guard lock(mutex_);
    title_ = title;
    protocol_ = protocol;
    xuid_ = xuid;
    name_ = name;
  }
  bool started = launcher ? launcher(port_, token_) : LaunchRexsteam(port_, token_);
  std::lock_guard lock(mutex_);
  started_ = started;
  return started;
}

void SteamBridge::SendHelper(const std::vector<uint8_t>& message) {
  if (helper_.valid()) {
    inner_->SendTo(helper_, message.data(), message.size());
  }
}

void SteamBridge::SendHello() {
  Writer w(Message::kHello);
  w.U32(kVersion);
  w.U64(token_);
  w.U32(title_);
  w.U32(protocol_);
  w.U64(xuid_);
  w.Str(name_);
  SendHelper(w.out);
  next_hello_ms_ = NowMs() + 1000;
}

void SteamBridge::SendTo(const Endpoint& to, const uint8_t* data, size_t size) {
  if (!to.steam()) {
    inner_->SendTo(to, data, size);
    return;
  }
  std::lock_guard lock(mutex_);
  if (!ready_) {
    return;
  }
  Writer w(Message::kSend);
  w.U64(to.steam_id());
  w.Bytes(data, size);
  SendHelper(w.out);
}

void SteamBridge::Poll(uint32_t timeout_ms, const Receive& receive) {
  inner_->Poll(timeout_ms, [&](const Endpoint& from, const uint8_t* data, size_t size) {
    if (from.ipv4() == kLoopback && !from.steam() && Reader::TypeOf(data, size) != 0) {
      OnHelper(data, size, from, receive);
    } else {
      receive(from, data, size);
    }
  });
  std::lock_guard lock(mutex_);
  if (started_ && helper_.valid() && !ready_ && !failed_ && NowMs() >= next_hello_ms_) {
    SendHello();
  }
}

void SteamBridge::OnHelper(const uint8_t* data, size_t size, const Endpoint& from,
                           const Receive& receive) {
  Reader r{data, size};
  auto type = Message(Reader::TypeOf(data, size));
  if (r.U64() != token_ || !r.ok) {
    return;
  }
  std::unique_lock lock(mutex_);
  if (!helper_.valid()) {
    if (type != Message::kAnnounce) {
      return;
    }
    helper_ = from;
  } else if (!(from == helper_)) {
    return;
  }
  switch (type) {
    case Message::kAnnounce:
      if (!ready_ && !failed_) {
        SendHello();
      }
      break;
    case Message::kStatus: {
      auto status = Status(r.U8());
      uint64_t self = r.U64();
      std::string persona = r.Str();
      std::string error = r.Str();
      if (!r.ok || ready_ || failed_) {
        break;
      }
      if (status == Status::kReady) {
        ready_ = true;
        self_steam_id_ = self;
        REXSYS_INFO("Online play: Steam is on, as \"{}\" ({})", persona, self);
      } else {
        failed_ = true;
        REXSYS_WARN("Online play: Steam is off: {}", error);
      }
      break;
    }
    case Message::kReceive: {
      uint64_t steam_id = r.U64();
      size_t n = 0;
      const uint8_t* frame = r.Rest(&n);
      if (r.ok && n) {
        lock.unlock();
        receive(Endpoint::Steam(steam_id), frame, n);
      }
      break;
    }
    case Message::kLobby: {
      uint64_t lobby = r.U64();
      uint64_t owner = r.U64();
      if (!r.ok) {
        break;
      }
      lobby_ = lobby;
      uint64_t hub = (owner && owner != self_steam_id_) ? owner : 0;
      REXSYS_INFO("Online play: in Steam lobby {} ({})", lobby,
                  hub ? fmt::format("its owner {} is the host", owner)
                      : std::string("this game's own"));
      if (hub == hub_owner_) {
        break;
      }
      hub_owner_ = hub;
      lock.unlock();
      if (net_) {
        net_->SetHub(hub ? Endpoint::Steam(hub) : Endpoint{});
      }
      break;
    }
    case Message::kFriends: {
      uint16_t count = r.U16();
      std::vector<Friend> list;
      for (uint16_t i = 0; i < count && r.ok; ++i) {
        Friend f;
        f.steam_id = r.U64();
        f.xuid = r.U64();
        f.name = r.Str();
        f.persona = r.Str();
        f.lobby = r.U64();
        if (r.ok) {
          list.push_back(std::move(f));
        }
      }
      if (!r.ok) {
        break;
      }
      if (list.size() != friends_.size()) {
        REXSYS_INFO("Online play: {} Steam friend(s) in this game", list.size());
      }
      friends_ = std::move(list);
      break;
    }
    case Message::kInvited: {
      uint64_t steam_id = r.U64();
      uint64_t lobby = r.U64();
      size_t n = 0;
      const uint8_t* invite = r.Rest(&n);
      if (!r.ok || !n || !sessions_) {
        break;
      }
      std::vector<uint8_t> copy(invite, invite + n);
      lock.unlock();
      REXSYS_INFO("Online play: an invite from Steam user {} (lobby {})", steam_id, lobby);
      sessions_->OnExternalInvite(copy.data(), copy.size(), [this, lobby] { JoinLobby(lobby); });
      break;
    }
    case Message::kLog: {
      uint8_t level = r.U8();
      std::string text = r.Str();
      if (!r.ok) {
        break;
      }
      if (level >= 2) {
        REXSYS_ERROR("Steam: {}", text);
      } else if (level == 1) {
        REXSYS_WARN("Steam: {}", text);
      } else {
        REXSYS_INFO("Steam: {}", text);
      }
      break;
    }
    default:
      break;
  }
}

bool SteamBridge::ready() const {
  std::lock_guard lock(mutex_);
  return ready_;
}

uint64_t SteamBridge::self_steam_id() const {
  std::lock_guard lock(mutex_);
  return self_steam_id_;
}

std::vector<SteamBridge::Friend> SteamBridge::Friends() const {
  std::lock_guard lock(mutex_);
  return friends_;
}

bool SteamBridge::Invite(uint64_t xuid, const std::vector<uint8_t>& invite) {
  std::lock_guard lock(mutex_);
  if (!ready_) {
    return false;
  }
  auto it = std::find_if(friends_.begin(), friends_.end(),
                         [xuid](const Friend& f) { return f.xuid == xuid; });
  if (it == friends_.end()) {
    return false;
  }
  Writer w(Message::kInvite);
  w.U64(it->steam_id);
  w.Bytes(invite.data(), invite.size());
  SendHelper(w.out);
  return true;
}

void SteamBridge::JoinLobby(uint64_t lobby) {
  std::lock_guard lock(mutex_);
  if (!ready_ || lobby == lobby_) {
    return;
  }
  Writer w(Message::kJoinLobby);
  w.U64(lobby);
  SendHelper(w.out);
}

}  // namespace rex::system::online
