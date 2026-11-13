/**
 * rexsteam: a game's Steam helper (rex/system/online/steam_link.h). Started by the game with
 *   rexsteam --port <the game's online port> --token <hex> --parent <the game's process ID>
 * it runs until the game says bye or ends.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "helper.h"
#include "steam_backend.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <timeapi.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path ExecutableFolder() {
#if defined(_WIN32)
  wchar_t path[MAX_PATH * 4] = {};
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
  return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

}  // namespace

int main(int argc, char** argv) {
  using namespace rex::system::online::steam_helper;
  Options options;
  unsigned long parent = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--port")) {
      options.game_port = uint16_t(std::strtoul(argv[i + 1], nullptr, 10));
    } else if (!std::strcmp(argv[i], "--token")) {
      options.token = std::strtoull(argv[i + 1], nullptr, 16);
    } else if (!std::strcmp(argv[i], "--parent")) {
      parent = std::strtoul(argv[i + 1], nullptr, 10);
    }
  }
  if (!options.game_port || !options.token) {
    std::fprintf(stderr, "usage: rexsteam --port <port> --token <hex> [--parent <pid>]\n");
    return 1;
  }
#if defined(_WIN32)
  // The loop waits 2 ms at a time.
  timeBeginPeriod(1);
  HANDLE parent_process = parent ? OpenProcess(SYNCHRONIZE, FALSE, DWORD(parent)) : nullptr;
  if (parent_process) {
    options.parent_alive = [parent_process] {
      return WaitForSingleObject(parent_process, 0) == WAIT_TIMEOUT;
    };
  }
#else
  if (parent) {
    options.parent_alive = [parent] { return kill(pid_t(parent), 0) == 0; };
  }
#endif
  auto backend = CreateSteamBackend(ExecutableFolder());
  int code = Run(backend.get(), options);
#if defined(_WIN32)
  if (parent_process) {
    CloseHandle(parent_process);
  }
  timeEndPeriod(1);
#endif
  return code;
}
