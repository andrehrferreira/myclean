#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/telemetry/process_table.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::placement::testing {

inline std::string upper(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](char character) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  });
  return result;
}

inline std::filesystem::path path_of(std::string_view utf8) {
  return std::filesystem::path{std::u8string(utf8.begin(), utf8.end())};
}

// HKCU\Environment of an owner whose profile is C:\Users\owner. Names are case-insensitive.
class FakeUserEnvironment final : public IUserEnvironment {
 public:
  [[nodiscard]] Result<std::optional<EnvValue>> get(std::string_view name) const override {
    const auto found = variables_.find(upper(name));
    return found == variables_.end() ? std::optional<EnvValue>{} : found->second;
  }
  [[nodiscard]] Result<void> set(std::string_view name, const EnvValue& value) override {
    if (Result<void> allowed = before_write(name); !allowed) {
      return allowed;
    }
    variables_[upper(name)] = value;
    return {};
  }
  [[nodiscard]] Result<void> remove(std::string_view name) override {
    if (Result<void> allowed = before_write(name); !allowed) {
      return allowed;
    }
    variables_.erase(upper(name));
    return {};
  }
  [[nodiscard]] Result<std::filesystem::path> expand(std::string_view text) const override {
    std::string result(text);
    for (const auto& [name, value] : expansions_) {
      for (std::size_t at = upper(result).find(name); at != std::string::npos;
           at = upper(result).find(name, at + value.size())) {
        result.replace(at, name.size(), value);
      }
    }
    return path_of(result);
  }
  [[nodiscard]] Result<void> announce() override {
    ++announcements_;
    return {};
  }

  void put(std::string_view name, std::string text, bool expandable = false) {
    variables_[upper(name)] = EnvValue{.text = std::move(text), .expandable = expandable};
  }
  [[nodiscard]] std::optional<EnvValue> value(std::string_view name) const { return *get(name); }
  [[nodiscard]] const std::map<std::string, EnvValue>& all() const { return variables_; }
  [[nodiscard]] int announcements() const { return announcements_; }
  [[nodiscard]] int writes() const { return writes_; }
  // Makes every later write of `name` fail with access_denied.
  void fail_on(std::string_view name) { failing_.insert(upper(name)); }
  // Runs before every write: a test can inspect the world at the moment a setting switches.
  void on_write(std::function<void(std::string_view)> hook) { hook_ = std::move(hook); }

 private:
  Result<void> before_write(std::string_view name) {
    if (hook_) {
      hook_(name);
    }
    if (failing_.contains(upper(name))) {
      return fail(Errc::access_denied, "injected failure writing " + std::string(name));
    }
    ++writes_;
    return {};
  }

  std::map<std::string, EnvValue> variables_;
  std::map<std::string, std::string> expansions_ = {
      {"%USERPROFILE%", R"(C:\Users\owner)"},
      {"%LOCALAPPDATA%", R"(C:\Users\owner\AppData\Local)"},
      {"%SYSTEMDRIVE%", "C:"}};
  std::set<std::string> failing_;
  std::function<void(std::string_view)> hook_;
  int announcements_ = 0;
  int writes_ = 0;
};

class FakeTextFiles final : public ITextFiles {
 public:
  [[nodiscard]] Result<std::optional<std::string>> read(
      const std::filesystem::path& path) const override {
    const auto found = files_.find(key(path));
    return found == files_.end() ? std::optional<std::string>{} : found->second;
  }
  [[nodiscard]] Result<void> write(const std::filesystem::path& path,
                                   std::string_view text) override {
    files_[key(path)] = std::string(text);
    return {};
  }
  [[nodiscard]] Result<void> remove(const std::filesystem::path& path) override {
    files_.erase(key(path));
    return {};
  }

  [[nodiscard]] bool exists(const std::filesystem::path& path) const {
    return files_.contains(key(path));
  }

 private:
  static std::wstring key(const std::filesystem::path& path) {
    std::wstring text = path.native();
    std::ranges::transform(text, text.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return text;
  }

  std::map<std::wstring, std::string> files_;
};

// Copies inside a FakeFileSystem. `skip_one_file()` leaves the last file out, as a copy that
// silently lost a file would.
class FakeTreeCopier final : public ITreeCopier {
 public:
  explicit FakeTreeCopier(fakes::FakeFileSystem& file_system) : file_system_(&file_system) {}

  [[nodiscard]] Result<TreeTotals> copy_tree(const std::filesystem::path& from,
                                             const std::filesystem::path& to) override {
    ++copies_;
    if (failure_) {
      return fail(*failure_, "injected copy failure");
    }
    if (!file_system_->exists(to)) {
      file_system_->add_directory(to);
    }
    TreeTotals totals;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> folders{{from, to}};
    std::vector<std::pair<std::filesystem::path, Bytes>> files;
    while (!folders.empty()) {
      const auto [source, destination] = folders.back();
      folders.pop_back();
      const std::vector<os::DirEntry> entries = file_system_->enumerate(source).value();
      for (const os::DirEntry& entry : entries) {
        if (entry.stat.attributes.reparse_point) {
          continue;
        }
        const std::filesystem::path target = destination / entry.path.filename();
        if (entry.stat.kind == os::EntryKind::directory) {
          file_system_->add_directory(target);
          folders.emplace_back(entry.path, target);
        } else {
          files.emplace_back(target, entry.stat.size);
        }
      }
    }
    if (skip_one_ && !files.empty()) {
      files.pop_back();
    }
    for (const auto& [target, size] : files) {
      file_system_->add_file(target, size);
      ++totals.files;
      totals.bytes += size;
    }
    return totals;
  }

  void skip_one_file() { skip_one_ = true; }
  void fail_with(Errc code) { failure_ = code; }
  [[nodiscard]] int copies() const { return copies_; }

 private:
  fakes::FakeFileSystem* file_system_;
  bool skip_one_ = false;
  std::optional<Errc> failure_;
  int copies_ = 0;
};

class FakeDriveFacts final : public IDriveFacts {
 public:
  [[nodiscard]] Result<std::vector<TargetVolume>> volumes() const override {
    if (failure_) {
      return tl::unexpected(*failure_);
    }
    return volumes_;
  }

  std::vector<TargetVolume>& list() { return volumes_; }
  // Makes every later volumes() fail with `error`: the volumes cannot be read.
  void fail_with(Error error) { failure_ = std::move(error); }
  TargetVolume& at(std::wstring_view root) {
    return *std::ranges::find_if(volumes_,
                                 [&](const TargetVolume& volume) { return volume.root == root; });
  }

 private:
  std::vector<TargetVolume> volumes_;
  std::optional<Error> failure_;
};

// Volumes like the owner's machine (docs/analysis/myclean-planning/01-context-and-measurements.md).
inline std::vector<TargetVolume> owner_volumes() {
  using namespace literals;
  const auto volume = [](const wchar_t* root, std::uint64_t size_gb, std::uint64_t free_gb,
                         DriveClass drive_class) {
    return TargetVolume{.root = root,
                        .size = Bytes{size_gb << 30U},
                        .free = Bytes{free_gb << 30U},
                        .drive_class = drive_class,
                        .healthy = true};
  };
  return {volume(L"C:\\", 952, 79, DriveClass::nvme),
          volume(L"D:\\", 3726, 1024, DriveClass::nvme),
          volume(L"E:\\", 954, 159, DriveClass::usb),
          volume(L"F:\\", 1863, 246, DriveClass::nvme),
          volume(L"G:\\", 1863, 334, DriveClass::nvme),
          volume(L"K:\\", 9314, 5136, DriveClass::hdd),
          volume(L"L:\\", 9314, 5947, DriveClass::hdd),
          volume(L"W:\\", 3726, 1757, DriveClass::nvme_qlc)};
}

class FakeSeekPenalty final : public ISeekPenalty {
 public:
  [[nodiscard]] std::optional<bool> seek_penalty(std::uint32_t disk_number) const override {
    const auto found = penalties.find(disk_number);
    return found == penalties.end() ? std::nullopt : std::optional<bool>{found->second};
  }

  std::map<std::uint32_t, bool> penalties;
};

// Records every command; `on_run` simulates its effect, `exit_codes` its exit code by text.
class FakeProcessRunner final : public IProcessRunner {
 public:
  [[nodiscard]] Result<CommandResult> run(const Command& command) override {
    commands.push_back(command.text());
    const auto code = exit_codes.find(command.text());
    if (code != exit_codes.end() && code->second != 0) {
      return CommandResult{.exit_code = code->second, .output = "Access is denied.\r\n"};
    }
    if (on_run) {
      on_run(command.text());
    }
    return CommandResult{};
  }

  std::vector<std::string> commands;
  std::map<std::string, std::uint32_t> exit_codes;
  std::function<void(const std::string&)> on_run;
};

// Registry keys by their text (HKCU\Software\...), case-insensitive, with their values.
class FakeRegistry final : public IRegistry {
 public:
  [[nodiscard]] Result<std::optional<RegistryValue>> read(std::string_view key,
                                                          std::string_view name) const override {
    const auto found = keys_.find(upper(key));
    if (found == keys_.end()) {
      return std::optional<RegistryValue>{};
    }
    const auto value = found->second.values.find(upper(name));
    return value == found->second.values.end() ? std::optional<RegistryValue>{} : value->second;
  }
  [[nodiscard]] Result<void> write(std::string_view key, std::string_view name,
                                   const RegistryValue& value) override {
    if (failing_) {
      return fail(Errc::access_denied, "injected failure writing " + std::string(name));
    }
    put(key, name, value);
    return {};
  }
  [[nodiscard]] Result<void> remove(std::string_view key, std::string_view name) override {
    if (const auto found = keys_.find(upper(key)); found != keys_.end()) {
      found->second.values.erase(upper(name));
    }
    return {};
  }
  [[nodiscard]] Result<std::vector<std::string>> subkeys(std::string_view key) const override {
    const std::string prefix = upper(key) + "\\";
    std::vector<std::string> names;
    for (const auto& [folded, entry] : keys_) {
      if (folded.starts_with(prefix) && folded.find('\\', prefix.size()) == std::string::npos) {
        names.push_back(entry.text.substr(prefix.size()));
      }
    }
    return names;
  }

  void put(std::string_view key, std::string_view name, RegistryValue value) {
    Key& entry = keys_[upper(key)];
    entry.text = std::string(key);
    entry.values[upper(name)] = std::move(value);
  }
  // Deletes `key` with its values, as `wsl --unregister` does with a distribution's key.
  void drop(std::string_view key) { keys_.erase(upper(key)); }
  [[nodiscard]] bool has(std::string_view key) const { return keys_.contains(upper(key)); }
  // Makes every later write fail with access_denied.
  void fail_writes() { failing_ = true; }

 private:
  struct Key {
    std::string text;
    std::map<std::string, RegistryValue> values;
  };

  std::map<std::string, Key> keys_;
  bool failing_ = false;
};

// A process table holding processes with the images `images`.
class FakeProcessTable final : public telemetry::IProcessTable {
 public:
  [[nodiscard]] Result<std::vector<telemetry::ProcessInfo>> snapshot() override {
    std::vector<telemetry::ProcessInfo> processes;
    processes.reserve(images.size());
    for (const std::string& image : images) {
      processes.push_back(telemetry::ProcessInfo{.image = image});
    }
    return processes;
  }
  [[nodiscard]] unsigned logical_processors() const override { return 1; }

  std::vector<std::string> images;
};

class FakeSystemSpace final : public ISystemSpace {
 public:
  [[nodiscard]] Result<HibernationState> hibernation() const override { return hibernation_state; }
  [[nodiscard]] Result<PageFileState> page_files() const override { return page_file_state; }
  [[nodiscard]] Result<void> set_page_files(const PageFileConfig& config) override {
    written.push_back(config);
    page_file_state.config = config;
    return {};
  }
  [[nodiscard]] Result<std::vector<ShadowStorage>> shadow_storage() const override {
    return shadows;
  }

  HibernationState hibernation_state;
  PageFileState page_file_state;
  std::vector<PageFileConfig> written;
  std::vector<ShadowStorage> shadows;
};

}  // namespace myclean::placement::testing
