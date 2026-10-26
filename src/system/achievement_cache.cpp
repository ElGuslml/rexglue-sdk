/**
 * @file        system/achievement_cache.cpp
 * @brief       The achievements as files for launchers (see rex/system/achievement_cache.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/system/achievement_cache.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace rex::system {
namespace {

// XLanguage 1-12 -> the key of its texts in the cache.
constexpr const char* kLanguageKeys[] = {"",   "en", "ja",      "de", "fr",      "es", "it",
                                         "ko", "zh-Hant", "pt", "zh-Hans", "pl", "ru"};

std::string Escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  for (char c : text) {
    unsigned char ch = static_cast<unsigned char>(c);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (ch < 0x20 || ch == 0x7F) {
      out += fmt::format("\\u{:04X}", ch);
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

bool SameContent(const std::filesystem::path& path, const void* data, size_t size) {
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) != size || ec) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  std::string existing((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return existing.size() == size && std::equal(existing.begin(), existing.end(), static_cast<const char*>(data));
}

// Written as a temporary file and renamed, so that a launcher never reads half a file.
bool WriteIfChanged(const std::filesystem::path& path, const void* data, size_t size) {
  if (SameContent(path, data, size)) {
    return true;
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(static_cast<const char*>(data), std::streamsize(size))) {
      REXSYS_WARN("Achievement cache: cannot write {}", rex::path_to_utf8(temporary));
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    REXSYS_WARN("Achievement cache: cannot replace {}: {}", rex::path_to_utf8(path), ec.message());
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

}  // namespace

void WriteAchievementCache(const util::XdbfGameData& db, uint32_t title_id, XLanguage loaded_language,
                           const std::vector<AchievementInfo>& achievements, const std::filesystem::path& dir) {
  if (!db.is_valid() || dir.empty()) {
    return;
  }
  std::error_code ec;
  const std::string title_hex = fmt::format("{:08X}", title_id);
  const std::filesystem::path icon_dir = dir / title_hex;
  std::filesystem::create_directories(icon_dir, ec);
  if (ec) {
    REXSYS_WARN("Achievement cache: cannot create {}: {}", rex::path_to_utf8(icon_dir), ec.message());
    return;
  }

  // The languages the XDBF has: those it has the title's name in (as XdbfGameData::GetExistingLanguage).
  std::vector<XLanguage> languages;
  for (uint32_t l = 1; l < uint32_t(XLanguage::kMaxLanguages); ++l) {
    if (!db.title(XLanguage(l)).empty()) {
      languages.push_back(XLanguage(l));
    }
  }
  const XLanguage default_language = db.default_language();
  if (std::find(languages.begin(), languages.end(), default_language) == languages.end() &&
      uint32_t(default_language) < uint32_t(XLanguage::kMaxLanguages)) {
    languages.insert(languages.begin(), default_language);
  }
  const XLanguage loaded = db.GetExistingLanguage(loaded_language);
  auto key = [](XLanguage language) { return kLanguageKeys[uint32_t(language)]; };

  std::string content = fmt::format(
      "# The achievements of title {}, for launchers: written by the ReXGlue runtime (recomp branch, RECOMP.md)\n"
      "# when it loads them, if they changed. The unlocks are in {}.toml, the icons in the folder {}.\n\n",
      title_hex, title_hex, title_hex);
  content += fmt::format("title_id = \"{}\"\n", title_hex);
  content += fmt::format("title = {}\n", Escape(db.title()));
  content += fmt::format("default_language = \"{}\"\n", key(default_language));
  content += "languages = [";
  for (size_t i = 0; i < languages.size(); ++i) {
    content += fmt::format("{}\"{}\"", i ? ", " : "", key(languages[i]));
  }
  content += "]\n";

  const auto entries = db.GetAchievements();
  std::vector<uint32_t> icons_written;
  for (const AchievementInfo& achievement : achievements) {
    auto entry = std::find_if(entries.begin(), entries.end(),
                              [&](const util::XdbfAchievementTableEntry& e) { return uint32_t(e.id) == achievement.id; });

    // The icon: the XDBF's image, as it is.
    std::string icon;
    if (achievement.image_id != 0) {
      bool written = std::find(icons_written.begin(), icons_written.end(), achievement.image_id) != icons_written.end();
      if (!written) {
        auto block = db.GetEntry(util::XdbfSection::kImage, achievement.image_id);
        if (block && block.size > 0 &&
            WriteIfChanged(icon_dir / fmt::format("{}.png", achievement.image_id), block.buffer, block.size)) {
          icons_written.push_back(achievement.image_id);
          written = true;
        }
      }
      if (written) {
        icon = fmt::format("{}/{}.png", title_hex, achievement.image_id);
      }
    }

    content += "\n[[achievements]]\n";
    content += fmt::format("id = {}\n", achievement.id);
    content += fmt::format("gamerscore = {}\n", achievement.gamerscore);
    content += fmt::format("flags = {}\n", achievement.flags);
    content += fmt::format("image_id = {}\n", achievement.image_id);
    if (!icon.empty()) {
      content += fmt::format("icon = \"{}\"\n", icon);
    }

    // The texts per language. The loaded language's come from the manager when achievements.toml overrode them,
    // or when the achievement is not in the XDBF at all.
    struct Texts {
      std::string label, description, unachieved;
    };
    auto texts_in = [&](XLanguage language) -> Texts {
      bool from_xdbf = entry != entries.end();
      if (from_xdbf && language == loaded) {
        from_xdbf = db.GetStringTableEntry(loaded, entry->label_id) == achievement.label;
      }
      if (!from_xdbf) {
        return {achievement.label, achievement.description, achievement.unachieved_description};
      }
      return {db.GetStringTableEntry(language, entry->label_id), db.GetStringTableEntry(language, entry->description_id),
              db.GetStringTableEntry(language, entry->unachieved_id)};
    };
    std::vector<std::pair<XLanguage, Texts>> per_language;
    for (XLanguage language : languages) {
      if (entry == entries.end() && language != loaded) {
        continue;
      }
      per_language.emplace_back(language, texts_in(language));
    }
    if (per_language.empty()) {
      per_language.emplace_back(loaded, texts_in(loaded));
    }
    const char* tables[] = {"label", "description", "unachieved_description"};
    for (int field = 0; field < 3; ++field) {
      content += fmt::format("[achievements.{}]\n", tables[field]);
      for (const auto& [language, texts] : per_language) {
        const std::string& text = field == 0 ? texts.label : field == 1 ? texts.description : texts.unachieved;
        content += fmt::format("{} = {}\n", key(language), Escape(text));
      }
    }
  }

  const std::filesystem::path info_path = dir / (title_hex + ".info.toml");
  bool unchanged = SameContent(info_path, content.data(), content.size());
  if (!unchanged && WriteIfChanged(info_path, content.data(), content.size())) {
    REXSYS_INFO("Achievement cache: {} achievements, {} icons, {} languages in {}", achievements.size(),
                icons_written.size(), languages.size(), rex::path_to_utf8(dir));
  }
}

}  // namespace rex::system
