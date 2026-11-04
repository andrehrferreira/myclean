#include "platform/proc/nt_process_table.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <winternl.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::platform {

namespace {

constexpr int system_process_information = 5;
constexpr long status_info_length_mismatch = static_cast<long>(0xC0000004L);
constexpr std::size_t initial_buffer = std::size_t{1} << 20U;
constexpr std::size_t buffer_margin = std::size_t{64} << 10U;

// FILETIME units.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

// SYSTEM_PROCESS_INFORMATION with the fields winternl.h leaves "reserved" named.
struct ProcessRecord {
  ULONG next_entry_offset;
  ULONG number_of_threads;
  LARGE_INTEGER working_set_private_size;
  ULONG hard_fault_count;
  ULONG number_of_threads_high_watermark;
  ULONGLONG cycle_time;
  LARGE_INTEGER create_time;
  LARGE_INTEGER user_time;
  LARGE_INTEGER kernel_time;
  UNICODE_STRING image_name;
  LONG base_priority;
  ULONG_PTR unique_process_id;
  ULONG_PTR inherited_from_unique_process_id;
  ULONG handle_count;
  ULONG session_id;
  ULONG_PTR unique_process_key;
  SIZE_T peak_virtual_size;
  SIZE_T virtual_size;
  ULONG page_fault_count;
  SIZE_T peak_working_set_size;
  SIZE_T working_set_size;
  SIZE_T quota_peak_paged_pool_usage;
  SIZE_T quota_paged_pool_usage;
  SIZE_T quota_peak_non_paged_pool_usage;
  SIZE_T quota_non_paged_pool_usage;
  SIZE_T pagefile_usage;
  SIZE_T peak_pagefile_usage;
  SIZE_T private_page_count;
  LARGE_INTEGER read_operation_count;
  LARGE_INTEGER write_operation_count;
  LARGE_INTEGER other_operation_count;
  LARGE_INTEGER read_transfer_count;
  LARGE_INTEGER write_transfer_count;
  LARGE_INTEGER other_transfer_count;
};
static_assert(sizeof(ProcessRecord) == sizeof(SYSTEM_PROCESS_INFORMATION));
// Field offsets are checked at run time by the smoke tests (PIDs, names, counters); clang
// does not accept MSVC's offsetof in a constant expression.

// Ready threads wait for a processor (KTHREAD_STATE Ready), as the "Processor Queue Length"
// counter counts them.
constexpr ULONG thread_state_ready = 1;

// The threads of a process follow its record.
std::uint32_t ready_threads(std::span<const std::byte> threads, ULONG count) {
  std::uint32_t ready = 0;
  for (std::size_t offset = 0;
       count > 0 && offset + sizeof(SYSTEM_THREAD_INFORMATION) <= threads.size();
       offset += sizeof(SYSTEM_THREAD_INFORMATION), --count) {
    ULONG state = 0;
    std::memcpy(
        &state,
        threads.subspan(offset + offsetof(SYSTEM_THREAD_INFORMATION, ThreadState), sizeof(state))
            .data(),
        sizeof(state));
    ready += state == thread_state_ready ? 1U : 0U;
  }
  return ready;
}

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

std::uint64_t unsigned_of(const LARGE_INTEGER& value) {
  return value.QuadPart > 0 ? static_cast<std::uint64_t>(value.QuadPart) : 0U;
}

// FILETIME units: 100 ns since 1601-01-01.
WallTime wall_time_of(const LARGE_INTEGER& file_time) {
  constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
  return WallTime{
      std::chrono::duration_cast<WallTime::duration>(Ticks{file_time.QuadPart - unix_epoch})};
}

telemetry::ProcessInfo info_of(const ProcessRecord& record) {
  const std::wstring_view name{record.image_name.Buffer,
                               record.image_name.Length / sizeof(wchar_t)};
  return telemetry::ProcessInfo{
      .pid = static_cast<std::uint32_t>(record.unique_process_id),
      .parent_pid = static_cast<std::uint32_t>(record.inherited_from_unique_process_id),
      .created = wall_time_of(record.create_time),
      .image = record.unique_process_id == 0 ? std::string("System Idle Process") : utf8(name),
      .cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
          Ticks{record.user_time.QuadPart + record.kernel_time.QuadPart}),
      .private_bytes = Bytes{record.private_page_count},
      .working_set = Bytes{record.working_set_size},
      .private_working_set = Bytes{unsigned_of(record.working_set_private_size)},
      .io_bytes = unsigned_of(record.read_transfer_count) +
                  unsigned_of(record.write_transfer_count) +
                  unsigned_of(record.other_transfer_count),
      .handles = record.handle_count,
      .threads = record.number_of_threads,
      .session = record.session_id,
  };
}

}  // namespace

NtProcessTable::NtProcessTable() : buffer_(initial_buffer) {
  if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
    query_ = reinterpret_cast<QueryFunction>(::GetProcAddress(ntdll, "NtQuerySystemInformation"));
  }
}

Result<std::vector<telemetry::ProcessInfo>> NtProcessTable::snapshot() {
  if (query_ == nullptr) {
    return fail(Errc::not_found, "NtQuerySystemInformation is not exported by ntdll.dll");
  }
  ULONG returned = 0;
  long status = status_info_length_mismatch;
  // The table grows between calls while processes start; retry a few times with room to spare.
  for (int attempt = 0; attempt < 4 && status == status_info_length_mismatch; ++attempt) {
    status = query_(system_process_information, buffer_.data(), static_cast<ULONG>(buffer_.size()),
                    &returned);
    if (status == status_info_length_mismatch) {
      buffer_.resize(std::max<std::size_t>(returned, buffer_.size()) + buffer_margin);
    }
  }
  if (status < 0) {
    return fail(Errc::io_error, std::format("NtQuerySystemInformation(SystemProcessInformation) "
                                            "failed with NTSTATUS {:#010x}",
                                            static_cast<unsigned long>(status)));
  }

  const std::span<const std::byte> table = std::span<const std::byte>{buffer_}.first(returned);
  std::vector<telemetry::ProcessInfo> processes;
  for (std::size_t offset = 0; offset + sizeof(ProcessRecord) <= table.size();) {
    ProcessRecord record{};
    std::memcpy(&record, table.subspan(offset, sizeof(record)).data(), sizeof(record));
    telemetry::ProcessInfo info = info_of(record);
    const std::size_t threads = offset + sizeof(record);
    const std::size_t end =
        record.next_entry_offset == 0 ? table.size() : offset + record.next_entry_offset;
    if (end > threads && end <= table.size()) {
      info.ready_threads =
          ready_threads(table.subspan(threads, end - threads), record.number_of_threads);
    }
    processes.push_back(std::move(info));
    if (record.next_entry_offset == 0) {
      break;
    }
    offset += record.next_entry_offset;
  }
  return processes;
}

unsigned NtProcessTable::logical_processors() const {
  return ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

}  // namespace myclean::platform
