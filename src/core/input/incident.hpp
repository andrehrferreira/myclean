#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/units.hpp"

namespace myclean::input {

// The two key chords the watchdog times. Stored as "copy" and "paste".
enum class Chord { copy, paste };

[[nodiscard]] std::string_view to_string(Chord chord);
[[nodiscard]] std::optional<Chord> parse_chord(std::string_view text);

// A measured time above this is an incident.
inline constexpr std::chrono::milliseconds incident_threshold{250};
// Each SendMessageTimeout(WM_NULL) probe waits this long for an answer.
inline constexpr std::chrono::milliseconds probe_timeout{100};
// Probe rounds start this often while a chord is being watched.
inline constexpr std::chrono::milliseconds probe_period{100};
// A chord is watched at most this long: paste probing stops, a copy without update is dropped.
inline constexpr std::chrono::milliseconds watch_limit{5000};

// A process by id and image file name ("Code.exe"); the image is empty when it could not be read.
struct ProcessRef {
  std::uint32_t pid = 0;
  std::string image;

  friend bool operator==(const ProcessRef&, const ProcessRef&) = default;
};

// One clipboard format as GetUpdatedClipboardFormats lists it: never its data or size.
struct ClipboardFormat {
  std::uint32_t id = 0;
  std::string name;
  // Converted by Windows from an earlier format in the list (CF_TEXT next to CF_UNICODETEXT).
  bool synthesized = false;

  friend bool operator==(const ClipboardFormat&, const ClipboardFormat&) = default;
};

// Fills the names of the standard formats (registered formats keep the name they came with) and
// marks a format as synthesized when an earlier format of the same conversion group (text,
// bitmap, metafile) is in the list. Windows lists the formats an application set first.
[[nodiscard]] std::vector<ClipboardFormat> describe_formats(std::vector<ClipboardFormat> formats);

// True for a format whose data is typically large: bitmaps, metafiles, TIFF and the registered
// image and rich-document formats PNG, JFIF, GIF, HTML Format and Rich Text Format.
[[nodiscard]] bool is_heavy(const ClipboardFormat& format);

// The image names of the processes known to take part in copy/paste freezes on this machine
// (analysis F-029): keyboard hooks, remote desktop, global hotkeys.
[[nodiscard]] std::span<const std::string_view> known_suspects();

// The known suspects among `running` image names, compared without case, each once, in the order
// of known_suspects().
[[nodiscard]] std::vector<std::string> running_suspects(std::span<const std::string> running);

// Total CPU time since boot, in any unit: `busy` is kernel plus user time without idle time.
struct CpuTimes {
  std::uint64_t idle = 0;
  std::uint64_t busy = 0;
};

// CPU utilisation in percent between two readings; nullopt when no time passed.
[[nodiscard]] std::optional<double> cpu_percent(CpuTimes before, CpuTimes after);

// One freeze over the threshold and the context captured around it. Empty optionals and lists
// mean "none".
struct Incident {
  Chord chord = Chord::copy;
  WallTime time;  // when the key went down
  // Copy: key down to clipboard update. Paste: key down to the foreground window's first answer.
  std::chrono::milliseconds latency{};
  // Paste only: the foreground window answered no probe within watch_limit.
  bool capped = false;
  // Paste only, when the clipboard owner is in another thread than the foreground window: key
  // down to the owner's first answer (watch_limit when it never answered).
  std::optional<std::chrono::milliseconds> owner_unanswered;
  // Tick count at receipt minus the message time of the key's WM_INPUT (16 ms resolution).
  std::chrono::milliseconds input_lag{};
  std::optional<ProcessRef> foreground;
  std::optional<ProcessRef> owner;
  // A process other than the foreground one seen holding the clipboard open during the stall.
  std::optional<ProcessRef> holder;
  std::optional<ProcessRef> viewer;  // head of the legacy clipboard viewer chain
  std::vector<ClipboardFormat> formats;
  std::vector<ProcessRef> hung;  // processes with a hung top-level window
  std::vector<std::string> suspects;
  std::optional<double> cpu_percent;  // total CPU utilisation during the stall
  std::optional<int> memory_load_percent;

  friend bool operator==(const Incident&, const Incident&) = default;
};

}  // namespace myclean::input
