#pragma once

#include <optional>

#include "core/events/events.hpp"
#include "platform/etw/krabs_user.hpp"

namespace myclean::platform::etw {

// Microsoft-Windows-Kernel-Process {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}.
inline constexpr GUID kernel_process_provider = {
    0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};
// Microsoft-Windows-Kernel-Network {7DD42A49-5329-4832-8DFD-43D979153A88}.
inline constexpr GUID kernel_network_provider = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
inline constexpr ULONGLONG process_keyword = 0x10;   // WINEVENT_KEYWORD_PROCESS
inline constexpr ULONGLONG network_keywords = 0x30;  // KERNEL_NETWORK_KEYWORD_IPV4 | _IPV6

// Converts the records of the two providers into core events, reading the properties by name
// through the provider's manifest (TDH), so every event version since Windows 8 works. A record
// of another event, or one whose properties do not parse, gives nullopt.

// ProcessStart (event 1). `image` is the NT device path the event carries
// ("\Device\HarddiskVolume3\Windows\System32\cmd.exe"); the event has no command line.
[[nodiscard]] std::optional<events::ProcessStart> process_start_of(
    const EVENT_RECORD& record, const krabs::schema_locator& schemas);
// ProcessStop (event 2). `image` is the file name.
[[nodiscard]] std::optional<events::ProcessStop> process_stop_of(
    const EVENT_RECORD& record, const krabs::schema_locator& schemas);
// TCP and UDP send and receive over IPv4 and IPv6 (events 10, 11, 26, 27, 42, 43, 58, 59), at
// the record's time stamp.
[[nodiscard]] std::optional<events::NetworkTransfer> network_transfer_of(
    const EVENT_RECORD& record, const krabs::schema_locator& schemas);

}  // namespace myclean::platform::etw
