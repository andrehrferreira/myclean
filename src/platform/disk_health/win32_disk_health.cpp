#include "platform/disk_health/win32_disk_health.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: winioctl.h and ntddscsi.h need its types
#include <winioctl.h>
#include <ntddscsi.h>
// clang-format on

#include "core/health/reading.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

using health::Bus;
using health::DriveProbe;

constexpr std::uint32_t max_disks = 64;
constexpr std::size_t block_size = 512;
constexpr std::uint8_t nvme_health_log_page = 0x02;

// ATA SMART READ DATA (ACS-3): feature D0h, command B0h, LBA mid/high 4Fh/C2h.
constexpr std::uint8_t smart_read_data = 0xD0;
constexpr std::uint8_t smart_command = 0xB0;
constexpr std::uint8_t smart_lba_mid = 0x4F;
constexpr std::uint8_t smart_lba_high = 0xC2;
constexpr std::uint8_t smart_device = 0xA0;  // device register: obsolete bits set, LBA off
constexpr ULONG pass_through_timeout_s = 5;

std::string error_text(DWORD error) {
  return error == ERROR_ACCESS_DENIED
             ? std::format("needs administrator rights (Win32 error {})", error)
             : std::format("Win32 error {}", error);
}

// Sends one IOCTL. Returns 0 on success, else the Win32 error.
DWORD control(HANDLE device, DWORD code, void* in, std::size_t in_size, void* out,
              std::size_t out_size, DWORD& returned) {
  returned = 0;
  if (::DeviceIoControl(device, code, in, static_cast<DWORD>(in_size), out,
                        static_cast<DWORD>(out_size), &returned, nullptr) == FALSE) {
    return ::GetLastError();
  }
  return 0;
}

template <typename T>
T read_struct(std::span<const std::byte> buffer, std::size_t offset = 0) {
  T value{};
  std::memcpy(&value, buffer.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

// The trimmed ASCII string at `offset` of a descriptor; empty when the offset is 0.
std::string text_at(std::span<const std::byte> buffer, DWORD offset) {
  if (offset == 0 || offset >= buffer.size()) {
    return {};
  }
  std::string text;
  for (const std::byte character : buffer.subspan(offset)) {
    if (character == std::byte{0}) {
      break;
    }
    text.push_back(static_cast<char>(character));
  }
  const std::size_t first = text.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

Bus bus_of(STORAGE_BUS_TYPE type) {
  switch (type) {
    case BusTypeNvme:
      return Bus::nvme;
    case BusTypeAta:
    case BusTypeSata:
      return Bus::ata;
    case BusTypeUsb:
      return Bus::usb;
    default:
      return Bus::other;
  }
}

// Model, serial, firmware and bus from the storage device descriptor.
DWORD read_identity(HANDLE device, DriveProbe& probe) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  std::vector<std::byte> buffer(4096);
  DWORD returned = 0;
  if (const DWORD error = control(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                  buffer.data(), buffer.size(), returned)) {
    return error;
  }
  const std::span<const std::byte> descriptor = std::span<const std::byte>{buffer}.first(returned);
  if (descriptor.size() < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    return ERROR_INVALID_DATA;
  }
  const auto header = read_struct<STORAGE_DEVICE_DESCRIPTOR>(descriptor);
  const std::string vendor = text_at(descriptor, header.VendorIdOffset);
  const std::string product = text_at(descriptor, header.ProductIdOffset);
  probe.model = vendor.empty() ? product : vendor + " " + product;
  probe.serial = text_at(descriptor, header.SerialNumberOffset);
  probe.firmware = text_at(descriptor, header.ProductRevisionOffset);
  probe.bus = bus_of(header.BusType);
  return 0;
}

std::optional<std::int64_t> read_temperature(HANDLE device) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceTemperatureProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_TEMPERATURE_DATA_DESCRIPTOR descriptor{};
  DWORD returned = 0;
  if (control(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
              sizeof(descriptor), returned) != 0 ||
      returned < sizeof(descriptor) || descriptor.InfoCount == 0) {
    return std::nullopt;
  }
  return descriptor.TemperatureInfo[0].Temperature;
}

// The NVMe SMART / Health Information log page through the storage stack.
DWORD read_nvme_health_log(HANDLE device, std::vector<std::uint8_t>& log) {
  const std::size_t request_offset = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
  const std::size_t reply_offset = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
  std::vector<std::byte> buffer(request_offset + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
                                block_size);
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProtocolSpecificProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_PROTOCOL_SPECIFIC_DATA request{};
  request.ProtocolType = ProtocolTypeNvme;
  request.DataType = NVMeDataTypeLogPage;
  request.ProtocolDataRequestValue = nvme_health_log_page;
  request.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
  request.ProtocolDataLength = block_size;
  std::memcpy(buffer.data(), &query, request_offset);
  std::memcpy(std::span{buffer}.subspan(request_offset).data(), &request, sizeof(request));
  DWORD returned = 0;
  if (const DWORD error = control(device, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(),
                                  buffer.size(), buffer.data(), buffer.size(), returned)) {
    return error;
  }
  const std::span<const std::byte> reply = std::span<const std::byte>{buffer}.first(returned);
  if (reply.size() < reply_offset + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)) {
    return ERROR_INVALID_DATA;
  }
  const auto data = read_struct<STORAGE_PROTOCOL_SPECIFIC_DATA>(reply, reply_offset);
  const std::size_t start = reply_offset + data.ProtocolDataOffset;
  if (data.ProtocolDataLength < block_size || start + block_size > reply.size()) {
    return ERROR_INVALID_DATA;
  }
  log.resize(block_size);
  std::memcpy(log.data(), reply.subspan(start, block_size).data(), block_size);
  return 0;
}

// SMART READ DATA through the disk driver; needs no access rights.
DWORD read_smart_data(HANDLE device, std::vector<std::uint8_t>& data) {
  SENDCMDINPARAMS in{};
  in.cBufferSize = READ_ATTRIBUTE_BUFFER_SIZE;
  in.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
  in.irDriveRegs.bSectorCountReg = 1;
  in.irDriveRegs.bSectorNumberReg = 1;
  in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
  in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
  in.irDriveRegs.bDriveHeadReg = smart_device;
  in.irDriveRegs.bCommandReg = SMART_CMD;
  const std::size_t data_offset = offsetof(SENDCMDOUTPARAMS, bBuffer);
  std::vector<std::byte> out(data_offset + READ_ATTRIBUTE_BUFFER_SIZE);
  DWORD returned = 0;
  if (const DWORD error = control(device, SMART_RCV_DRIVE_DATA_EX, &in, sizeof(in), out.data(),
                                  out.size(), returned)) {
    return error;
  }
  const auto header = read_struct<SENDCMDOUTPARAMS>(out);
  if (returned < out.size() || header.DriverStatus.bDriverError != 0) {
    return ERROR_INVALID_DATA;
  }
  data.resize(block_size);
  std::memcpy(data.data(), std::span{out}.subspan(data_offset, block_size).data(), block_size);
  return 0;
}

std::optional<bool> read_predicts_failure(HANDLE device) {
  STORAGE_PREDICT_FAILURE result{};
  DWORD returned = 0;
  if (control(device, IOCTL_STORAGE_PREDICT_FAILURE, nullptr, 0, &result, sizeof(result),
              returned) != 0) {
    return std::nullopt;
  }
  return result.PredictFailure != 0;
}

// SMART READ DATA through a USB bridge with SCSI/ATA Translation: ATA PASS-THROUGH(16), PIO
// data-in. IOCTL_SCSI_PASS_THROUGH requires a read/write handle, so this needs elevation.
std::string read_sat_smart_data(const std::wstring& path, std::vector<std::uint8_t>& data) {
  const wil::unique_hfile device{::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
  if (!device) {
    return "SAT pass-through " + error_text(::GetLastError());
  }
  struct Request {
    SCSI_PASS_THROUGH header;
    std::array<UCHAR, 32> sense;
    std::array<UCHAR, block_size> data;
  };
  Request request{};
  request.header.Length = sizeof(SCSI_PASS_THROUGH);
  request.header.CdbLength = 16;
  request.header.SenseInfoLength = static_cast<UCHAR>(request.sense.size());
  request.header.DataIn = SCSI_IOCTL_DATA_IN;
  request.header.DataTransferLength = block_size;
  request.header.TimeOutValue = pass_through_timeout_s;
  request.header.DataBufferOffset = offsetof(Request, data);
  request.header.SenseInfoOffset = offsetof(Request, sense);
  const std::array<UCHAR, 16> cdb{
      0x85,  // ATA PASS-THROUGH(16)
      0x08,  // protocol 4: PIO data-in
      0x0E,  // data from the device, length in blocks, taken from the sector count
      0,    smart_read_data, 0, 1, 0, 0, 0, smart_lba_mid, 0, smart_lba_high, 0, smart_command, 0};
  std::memcpy(std::data(request.header.Cdb), cdb.data(), cdb.size());
  DWORD returned = 0;
  if (const DWORD error = control(device.get(), IOCTL_SCSI_PASS_THROUGH, &request, sizeof(request),
                                  &request, sizeof(request), returned)) {
    return "SAT pass-through " + error_text(error);
  }
  if (request.header.ScsiStatus != 0) {
    return std::format("the bridge rejected SAT pass-through (SCSI status 0x{:02x})",
                       request.header.ScsiStatus);
  }
  data.assign(request.data.begin(), request.data.end());
  return {};
}

void append(std::string& problem, std::string_view text) {
  problem += problem.empty() ? "" : "; ";
  problem += text;
}

DriveProbe probe_disk(std::uint32_t number, HANDLE device, const std::wstring& path) {
  DriveProbe probe{.disk_number = number};
  if (const DWORD error = read_identity(device, probe)) {
    probe.model = std::format("PhysicalDrive{}", number);
    probe.problem = "device descriptor: " + error_text(error);
    return probe;
  }
  probe.temperature_c = read_temperature(device);
  std::string problem;
  const auto try_nvme = [&] {
    if (const DWORD error = read_nvme_health_log(device, probe.nvme_health_log)) {
      append(problem, "NVMe health log: " + error_text(error));
    }
  };
  const auto try_smart = [&] {
    if (const DWORD error = read_smart_data(device, probe.smart_data)) {
      append(problem, "SMART read: " + error_text(error));
    }
  };
  switch (probe.bus) {
    case Bus::nvme:
      try_nvme();
      break;
    case Bus::ata:
      try_smart();
      probe.predicts_failure = read_predicts_failure(device);
      break;
    case Bus::usb:
      try_nvme();
      if (probe.nvme_health_log.empty()) {
        try_smart();
      }
      if (probe.nvme_health_log.empty() && probe.smart_data.empty()) {
        if (const std::string sat = read_sat_smart_data(path, probe.smart_data); !sat.empty()) {
          append(problem, sat);
        } else {
          probe.smart_via_bridge = true;
          problem.clear();
        }
      }
      if (probe.nvme_health_log.empty() && probe.smart_data.empty()) {
        problem = "not readable over USB: " + problem;
      }
      break;
    case Bus::other:
      problem = "no health reader for this bus type";
      break;
  }
  if (probe.nvme_health_log.empty() && probe.smart_data.empty()) {
    if (Result<health::ReliabilityCounters> counters = reliability_counters(number)) {
      probe.reliability = *counters;
    } else {
      append(problem, "reliability counters: " + counters.error().message);
    }
  }
  probe.problem = std::move(problem);
  return probe;
}

}  // namespace

Result<std::vector<DriveProbe>> Win32DiskHealthSource::probe() const {
  DWORD previous_mode = 0;
  ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
  const auto restore = wil::scope_exit([&] { ::SetThreadErrorMode(previous_mode, nullptr); });

  std::vector<DriveProbe> probes;
  for (std::uint32_t number = 0; number < max_disks; ++number) {
    const std::wstring path = std::format(L"\\\\.\\PhysicalDrive{}", number);
    // No access rights: enough for every query IOCTL, and needs no elevation.
    const wil::unique_hfile device{::CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!device) {
      const DWORD error = ::GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        probes.push_back(DriveProbe{.disk_number = number,
                                    .model = std::format("PhysicalDrive{}", number),
                                    .problem = "cannot open the disk: " + error_text(error)});
      }
      continue;
    }
    probes.push_back(probe_disk(number, device.get(), path));
  }
  if (probes.empty()) {
    return fail(Errc::not_found, "no physical disk could be opened");
  }
  return probes;
}

}  // namespace myclean::platform
