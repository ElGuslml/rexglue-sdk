#pragma once
/**
 * @file        rex/system/achievement_cache.h
 * @brief       The achievements as files a launcher can read without the title's executable
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <cstdint>
#include <filesystem>
#include <vector>

#include <rex/system/achievement_store.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

namespace rex::system {

// A title's achievements live in its executable (the XDBF in the XEX, compressed and often encrypted), which a
// launcher should not have to read. Each time the achievements are loaded, the runtime writes them next to the
// unlocks (<dir>/<TITLEID>.toml, AchievementManager), rewriting only what changed:
// - <dir>/<TITLEID>.info.toml: the title, its languages, and per achievement its id, gamerscore, flags, image id,
//   icon, and label, description and unachieved description in every language the XDBF has (keys en, ja, de, fr,
//   es, it, ko, zh-Hant, pt, zh-Hans, pl, ru). Texts that achievements.toml overrode replace the XDBF's in the
//   language they were loaded in (`loaded_language`);
// - <dir>/<TITLEID>/<image_id>.png: the icons, the XDBF's images as they are (PNG).
// --achievements_cache_only (ReXApp) writes them and exits, for a launcher that has just installed the game.
void WriteAchievementCache(const util::XdbfGameData& db, uint32_t title_id, XLanguage loaded_language,
                           const std::vector<AchievementInfo>& achievements, const std::filesystem::path& dir);

}  // namespace rex::system
