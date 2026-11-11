#pragma once

#include <string_view>

// The intruder-scan methods of the service (security spec; core/security,
// svc::SecurityComponent). Every answer is a JSON object; times are Unix seconds (UTC), sizes
// bytes. The scan reads only: no method here closes a process or removes or disables a file,
// extension, persistence point or service. A persistence point that is a startup item names it
// in "startup_item" (the "key" of startup.list): disabling it is a plans.propose_startup plan
// that waits for the owner's approval in the tray. "Known dangerous" comes only from Microsoft
// Defender or the owner's opt-in VirusTotal lookup; everything else is at most "suspicious
// because <reasons>".
//
// The owner's settings live in %ProgramData%\MyClean\security.toml (accepted only when
// administrators alone can write to it), read at every call; both are off without it:
//   [network]
//   reverse_dns = false   # resolve the remote addresses of security.network
//   [virustotal]
//   enabled = false       # look up the SHA-256 of suspicious files (needs api_key)
//   api_key = ""
namespace myclean::ipc {

// Trust (as in telemetry.hpp): {"status", "signer", "path", "path_class", "unrecognized",
// "reasons", "description", "category"}.
//
// No parameters. Every persistence point, read now:
// {"items": [{"id": int (its number in this answer; security.defender_scan takes it),
//             "source": "run"|"run32"|"runonce"|"runonce32"|"folder"|"task"|"service"|"wmi"|
//                       "ifeo"|"winlogon"|"appinit",
//             "location": string (the key, folder or namespace it is stored in), "name",
//             "command" (what it runs, as stored), "target": string|null (the file it runs, as
//             written), "image": string|null (that file on disk; null when it does not exist),
//             "enabled": bool|null, "signer": string|null, "startup_item": string|null,
//             "trust": Trust|null, "status": "default"|"ok"|"suspicious"|"known_dangerous",
//             "reasons": [token] ("unsigned", "invalid-signature", "signer-not-listed",
//                         "user-writable-path", "not-in-knowledge-base", "target-missing",
//                         "non-default-winlogon", "appinit-dll", "ifeo-debugger",
//                         "wmi-consumer"),
//             "summary": string ("suspicious because ...", "known dangerous: ..."),
//             "verdict": null | {"source": "Microsoft Defender"|"VirusTotal", "threat":
//                                string|null, "flagged": int|null, "engines": int|null,
//                                "time": int|null},
//             "virustotal": null | {"flagged": int, "engines": int, "time": int} (the stored
//                                   lookup, also when nothing flagged it; "engines" 0: the
//                                   hash is unknown to VirusTotal)}],
//  "counts": {"default", "ok", "suspicious", "known_dangerous"},
//  "defender": {"available": bool, "error": string|null, "detections": [{"threat",
//               "resources": [string], "time": int|null}] (the newest 50 of its history)},
//  "virustotal": {"enabled": bool, "note": string|null, "queued": int (lookups waiting)},
//  "problems": [string] (sources that could not be read)}
// Defender's history is read at most every 5 minutes. With VirusTotal enabled, the SHA-256 of
// each suspicious image is queued for a lookup (one every 15 s, a result kept 7 days); only the
// hash is sent, never the file, its name or its path.
inline constexpr std::string_view security_scan_method = "security.scan";

// No parameters. The browser extensions of the owner:
// {"extensions": [{"browser": "chrome"|"edge"|"brave"|"firefox", "profile", "id", "name",
//                  "version", "installed": int|null, "origin": "store"|"external"|"policy"|
//                  "unpacked", "enabled": bool, "flag": null|"probably not installed by you"}],
//  "problems": [string]}
inline constexpr std::string_view security_extensions_method = "security.extensions";

// Parameters: hours (number, optional, default 24, at most security_max_hours). Every process
// that used the network in the last `hours` (net_minute) or holds a TCP connection, listener or
// UDP endpoint now (GetExtendedTcpTable / GetExtendedUdpTable), loopback left out:
// {"hours", "from", "to", "reverse_dns": bool, "names_pending": int, "windows_reported": bool,
//  "processes": [{"pid", "created": int|null, "image", "running": bool, "session": int|null,
//                 "visible_window": bool|null (null: unknown, the tray has not reported),
//                 "trust": Trust|null, "sent", "received", "first_seen": int|null,
//                 "last_seen": int|null, "active_minutes": int,
//                 "remotes": [{"address", "name": string|null, "port", "protocol", "sent",
//                              "received", "first_seen": int|null, "last_seen": int|null,
//                              "active_minutes", "open_now": bool}],
//                 "listening": [{"protocol", "port"}]}]}
// "name" is null while reverse DNS is off (the default: no DNS query is made) or the name is not
// resolved yet, and "" for an address without a reverse-DNS record. Names are resolved in the
// background, a few per second, and kept for 6 hours.
inline constexpr std::string_view security_network_method = "security.network";
inline constexpr double security_max_hours = 30.0 * 24;

// Parameters: item (integer, required): an "id" of the latest security.scan answer. Starts a
// Microsoft Defender scan of that item's image (MpCmdRun -Scan -ScanType 3 -File <image>
// -DisableRemediation: it reports and quarantines or deletes nothing) in the background; its
// result becomes the item's verdict in later scans. {"started": true, "image": string}. Fails
// with not_found for an id the latest answer did not list, invalid_argument for an item without
// an image on disk, and invalid_state while another scan runs.
inline constexpr std::string_view security_defender_scan_method = "security.defender_scan";

// TRAY ONLY. Parameters: windows (record, required): {"pids": [int]} (1..64 process ids), the
// processes owning a visible top-level window in the owner's session now. The tray sends them
// every security_windows_seconds, in as many calls as it needs. {}.
inline constexpr std::string_view security_windows_method = "security.windows";
inline constexpr int security_windows_seconds = 60;

}  // namespace myclean::ipc
