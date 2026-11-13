/**
 * The Steam helper's Steam (steam_backend.cpp): steam_api64.dll (libsteam_api.so) in `folder`.
 */

#pragma once

#include <filesystem>
#include <memory>

#include "helper.h"

namespace rex::system::online::steam_helper {

std::unique_ptr<Backend> CreateSteamBackend(const std::filesystem::path& folder);

}  // namespace rex::system::online::steam_helper
