/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               bool allow_share_delete)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      allow_share_delete_(allow_share_delete) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      // Stat the exact name first: enumerating a directory of hundreds of
      // archives costs milliseconds a walk. Only casing mismatch reaches
      // the scan below.
      const auto exact_path = current_entry->host_path() / rex::to_path(part);
      rex::filesystem::FileInfo exact_info;
      if (rex::filesystem::GetInfo(exact_path, &exact_info)) {
        auto* exact_child = HostPathEntry::Create(this, current_entry, exact_path, exact_info);
        if (!exact_child) {
          return nullptr;
        }
        current_entry->children_.push_back(std::unique_ptr<Entry>(exact_child));
        current_entry = static_cast<HostPathEntry*>(exact_child);
        continue;
      }

      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    auto child = HostPathEntry::Create(this, parent_entry,
                                       parent_entry->host_path() / child_info.name, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

bool HostPathDevice::MergeOverlay(const std::filesystem::path& overlay_root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(overlay_root, ec)) {
    return false;
  }

  OverlayCounts counts;
  MergeOverlayEntry(static_cast<HostPathEntry*>(root_entry_.get()), overlay_root, counts);
  REXFS_INFO(
      "Overlay {} on {}: {} files shadow existing ones, {} files added, {} directories added",
      rex::path_to_utf8(overlay_root), mount_path(), counts.files_shadowed, counts.files_added,
      counts.directories_added);
  return true;
}

void HostPathDevice::MergeOverlayEntry(HostPathEntry* parent_entry,
                                       const std::filesystem::path& overlay_path,
                                       OverlayCounts& counts) {
  for (auto& child_info : rex::filesystem::ListFiles(overlay_path)) {
    const auto child_host_path = overlay_path / child_info.name;
    const bool is_directory = child_info.type == rex::filesystem::FileInfo::Type::kDirectory;
    auto* existing = parent_entry->GetChild(rex::path_to_utf8(child_info.name));

    // A directory the tree has already is merged into, its entry kept.
    if (existing && is_directory && (existing->attributes() & kFileAttributeDirectory)) {
      MergeOverlayEntry(static_cast<HostPathEntry*>(existing), child_host_path, counts);
      continue;
    }

    // Anything else the overlay has replaces the entry, in its place in the parent's list (a
    // directory listing keeps its order) and under its name (its casing too). A file and a
    // directory of the same name: the overlay's wins as well.
    const bool shadows = existing != nullptr;
    if (shadows) {
      child_info.name = rex::to_path(existing->name());
    }
    auto* child = HostPathEntry::Create(this, parent_entry, child_host_path, child_info);
    child->from_overlay_ = true;
    if (shadows) {
      auto it = std::find_if(parent_entry->children_.begin(), parent_entry->children_.end(),
                             [existing](const auto& entry) { return entry.get() == existing; });
      it->reset(child);
    } else {
      parent_entry->children_.push_back(std::unique_ptr<Entry>(child));
    }

    if (is_directory) {
      ++counts.directories_added;
      MergeOverlayEntry(child, child_host_path, counts);
    } else if (shadows) {
      ++counts.files_shadowed;
      REXFS_DEBUG("  {} shadowed by {}", child->path(), rex::path_to_utf8(child_host_path));
    } else {
      ++counts.files_added;
      REXFS_DEBUG("  {} added from {}", child->path(), rex::path_to_utf8(child_host_path));
    }
  }
}

}  // namespace rex::filesystem
