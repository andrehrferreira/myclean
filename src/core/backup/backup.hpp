#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/backup/repos.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/space/scanner.hpp"

// Backup detection (backup spec): every Windows backup source, the third-party backup products,
// the backup artifacts on the backup volumes, the last-backup age and, when no backup exists, a
// recommendation that uses the owner's own drives. Read-only: nothing here configures, starts or
// changes a backup. The facts come from IBackupSystem (src/platform/backup); the values each one
// reads are those of spike X8 (docs/spikes/X8-backups.md).
namespace myclean::backup {

// File History of the owner: configured when its Config1.xml exists.
struct FileHistoryFacts {
  bool configured = false;
  std::string target;              // the TargetUrl of Config1.xml; empty when unreadable
  std::optional<WallTime> newest;  // the newest file time of its Configuration folder
};

struct RestorePoint {
  WallTime created;
  std::string description;
};

struct ShadowCopy {
  std::string volume;  // the shadowed volume (\\?\Volume{...}\)
  WallTime created;
};

// One OneDrive account of the owner.
struct OneDriveAccount {
  std::string account;                         // the Accounts subkey: Personal, Business1, ...
  std::filesystem::path folder;                // its UserFolder
  std::vector<std::string> protected_folders;  // Desktop, Documents, Pictures under `folder`
  std::optional<WallTime> sign_in_error;       // SignInErrorStateStartTimestamp, when set
};

struct Volume {
  std::filesystem::path root;  // "K:\"
  std::string label;           // "Backup 1"
};

// What Windows holds about backups, as the OS stores it. Implemented by src/platform/backup;
// tests use fakes. Each read fails on its own (access denied, WMI error), and a failure is
// reported as "could not be checked" for that source only.
class IBackupSystem {
 public:
  virtual ~IBackupSystem() = default;
  IBackupSystem(const IBackupSystem&) = delete;
  IBackupSystem& operator=(const IBackupSystem&) = delete;
  IBackupSystem(IBackupSystem&&) = delete;
  IBackupSystem& operator=(IBackupSystem&&) = delete;

  [[nodiscard]] virtual Result<FileHistoryFacts> file_history() const = 0;
  // Whether `wbadmin get versions` lists a system image version (judged by its exit code).
  [[nodiscard]] virtual Result<bool> image_versions() const = 0;
  [[nodiscard]] virtual Result<std::vector<RestorePoint>> restore_points() const = 0;
  [[nodiscard]] virtual Result<std::vector<ShadowCopy>> shadow_copies() const = 0;
  [[nodiscard]] virtual Result<std::vector<OneDriveAccount>> onedrive() const = 0;
  // The DisplayName of every Uninstall entry (HKLM 64-bit and 32-bit, the owner's HKCU).
  [[nodiscard]] virtual Result<std::vector<std::string>> installed_programs() const = 0;
  // The volumes with a drive letter, with their labels.
  [[nodiscard]] virtual Result<std::vector<Volume>> volumes() const = 0;

 protected:
  IBackupSystem() = default;
};

enum class SourceKind {
  file_history,    // "file_history"
  system_image,    // "system_image"
  restore_points,  // "restore_points"
  shadow_copies,   // "shadow_copies"
  onedrive,        // "onedrive"
  third_party,     // "third_party"
  backup_drives,   // "backup_drives": the artifacts on the backup volumes
};
[[nodiscard]] std::string_view to_string(SourceKind kind);

enum class Status {
  found,      // "found"
  not_found,  // "not_found"
  unknown,    // "could_not_check": the read failed; `detail` holds the error
};
[[nodiscard]] std::string_view to_string(Status status);

// What a backup artifact on a backup volume is.
enum class ArtifactKind {
  macrium_image,   // "macrium_image": *.mrimg
  veeam_backup,    // "veeam_backup": *.vbk
  acronis_backup,  // "acronis_backup": *.tibx, *.tib
  system_image,    // "system_image": a WindowsImageBackup folder
  file_history,    // "file_history": a FileHistory folder
  manual_copy,     // "manual_copy": a top-level folder named "backup" with a date
  dump,            // "dump": *.dump, *_gitlab_backup.tar, *.bak
};
[[nodiscard]] std::string_view to_string(ArtifactKind kind);

struct Artifact {
  ArtifactKind kind = ArtifactKind::dump;
  std::filesystem::path path;
  WallTime newest;  // the file's last write; a folder's newest entry; a manual copy's date
};

struct Source {
  SourceKind kind = SourceKind::file_history;
  Status status = Status::not_found;
  std::string location;             // where it stores its copies; empty when unknown or none
  std::optional<WallTime> newest;   // the newest copy; nullopt when none or unknown
  std::size_t copies = 0;           // restore points, shadow copies, products, artifacts
  std::string detail;               // the error, or what is known beyond the fields
  std::vector<std::string> names;   // third_party: the products found
  std::vector<Artifact> artifacts;  // backup_drives and system_image: what was found
  // Whether its copies count toward the last-backup age: not restore points and shadow copies,
  // which live on the volume they protect.
  [[nodiscard]] bool counts() const;
};

// The third-party products MyClean recognizes by a case-insensitive part of their DisplayName.
[[nodiscard]] std::span<const std::string_view> backup_products();

// The product of `display_name`, nullopt when it is none of backup_products().
[[nodiscard]] std::optional<std::string_view> backup_product(std::string_view display_name);

// The date written in a folder name such as "Backup-24-09-2026", "backup_2026-09-24" or
// "Backup 20260924" (day-month-year with '-', '_' or '.', year-month-day, or eight digits
// year-month-day), as midnight UTC; nullopt when it holds none.
[[nodiscard]] std::optional<WallTime> name_date(std::string_view name);

// The volumes whose label contains "backup" (any case): where artifacts are looked for.
[[nodiscard]] std::vector<Volume> backup_volumes(std::span<const Volume> volumes);

struct ScanLimits {
  int max_depth = 3;                 // folder levels below a volume root
  std::size_t max_folders = 20'000;  // folders listed per volume
};

// The artifacts on the volume `root`, walked with `reader` down to limits.max_depth: a reparse
// point is never entered and no file is opened (a cloud placeholder is judged by its name and
// times). WindowsImageBackup and FileHistory folders, and a top-level folder whose name holds
// "backup" and a date (a manual copy, dated by name_date, else its creation time), are
// artifacts and not entered; the WindowsImageBackup folder at the root is left to image_folder.
// `.vhdx` and `.wim` files are not artifacts. Folders that cannot be listed are skipped and counted
// in `unreadable`.
struct ArtifactScan {
  std::vector<Artifact> artifacts;
  std::size_t unreadable = 0;
  bool truncated = false;  // stopped at limits.max_folders
};
[[nodiscard]] ArtifactScan scan_artifacts(const space::IDirectoryReader& reader,
                                          const std::filesystem::path& root,
                                          const ScanLimits& limits, const std::stop_token& stop);

// The WindowsImageBackup folder of volume `root`, as an artifact dated by its newest entry two
// levels down (WindowsImageBackup\<computer>\<backup set>); nullopt when there is none.
[[nodiscard]] std::optional<Artifact> image_folder(const space::IDirectoryReader& reader,
                                                   const std::filesystem::path& root);

// The newest copy among the sources that count, and its source.
struct LastBackup {
  SourceKind source = SourceKind::file_history;
  std::string name;      // "File History", "Macrium Reflect image", "manual copy"
  std::string location;  // where that copy is; empty when unknown
  WallTime newest;
};

// A drive named, or left out, by the recommendation.
struct DriveAdvice {
  std::string drive;   // "K:"
  std::string reason;  // why it is left out; empty when named
};

struct Recommendation {
  std::string text;                   // the steps, naming only the drives in `named`
  std::vector<std::string> named;     // "K:", "L:"
  std::vector<DriveAdvice> left_out;  // backup-volume or HDD candidates that are not healthy
};

// The recommendation when no backup exists: File History to the healthy HDD (or backup volume)
// with the most free space, a monthly system image to the second one (or the same one when
// there is only one), naming only drives that exist and are healthy; a drive that is not
// healthy is left out with its health note. Candidates are the HDDs and the volumes whose label
// contains "backup", other than `system_root`. With no healthy candidate, the text says to
// attach a drive and names none.
[[nodiscard]] Recommendation recommend(std::span<const placement::TargetVolume> volumes,
                                       std::span<const Volume> labels,
                                       const std::filesystem::path& system_root);

struct Report {
  WallTime checked;
  std::vector<Source> sources;  // one per SourceKind, in its order
  std::optional<LastBackup> last;
  // A source that counts was found (OneDrive folder backup has no copy time, so a backup can
  // exist with `last` empty).
  bool backup_exists = false;
  std::optional<Recommendation> recommendation;  // only when no backup exists
  std::optional<RepoReport> repositories;        // nullopt without git
};

// What check() reads.
struct Probes {
  const IBackupSystem* system = nullptr;
  const space::IDirectoryReader* reader = nullptr;
  const placement::IDriveFacts* drives = nullptr;  // null: no recommendation
  std::filesystem::path system_root{L"C:\\"};
  const IRepoGit* git = nullptr;  // null: no repository check
  std::vector<std::filesystem::path> workspace_roots;
  std::vector<std::wstring> skip;  // folder names never entered (data/workspaces.toml)
  RepoLimits repo_limits;
};

// Checks every source (backup spec, "Windows Backup Source Detection", "Third-Party Tools And
// Backup Drive Artifacts", "Last-Backup Age", "Recommendation When No Backup Exists"):
//  - file_history: found when configured (location its target, newest its time);
//  - system_image: the WindowsImageBackup folders of every volume; when none, found when
//    image_versions() says wbadmin lists one (location unknown), unknown when it fails;
//  - restore_points, shadow_copies: how many and the newest; never counted;
//  - onedrive: found when an account protects a known folder (no copy time);
//  - third_party: the backup products installed (no copy time of their own);
//  - backup_drives: scan_artifacts over backup_volumes().
// The last backup is the newest copy among the sources that count; with no source that counts
// found, the report holds recommend() over the drives. With git, the repositories of the
// workspace roots are checked too (find_repositories, check_repositories).
[[nodiscard]] Report check(const Probes& probes, WallTime now, const std::stop_token& stop,
                           const ScanLimits& limits = {});

// The report as JSON (the backups.check answer, core/ipc/backup.hpp), times Unix seconds; ages in
// whole days before `now`.
[[nodiscard]] nlohmann::json report_json(const Report& report, WallTime now);

// The metric of the backup-age alert (data/alerts.toml, rule backup.age): the whole days since
// the last backup, entity "newest", infinite when no backup exists. A backup without a copy time
// (OneDrive folder backup alone) reads nothing.
inline constexpr std::string_view age_metric = "backup.age_days";
[[nodiscard]] std::vector<alerts::Reading> age_readings(const Report& report, WallTime now);

}  // namespace myclean::backup
