#include "core/rules/data_files.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/parse_support.hpp"

namespace myclean::rules {

namespace {

Errc code_of(const std::error_code& error) {
  if (error == std::errc::no_such_file_or_directory) {
    return Errc::not_found;
  }
  if (error == std::errc::permission_denied) {
    return Errc::access_denied;
  }
  return Errc::io_error;
}

}  // namespace

Result<std::vector<std::filesystem::path>> DiskDataFiles::list(
    const std::filesystem::path& dir) const {
  std::error_code error;
  std::filesystem::directory_iterator entry(dir, error);
  std::vector<std::filesystem::path> files;
  while (!error && entry != std::filesystem::directory_iterator()) {
    std::error_code ignored;  // an entry that vanished meanwhile is simply not listed
    if (entry->is_regular_file(ignored)) {
      files.push_back(entry->path());
    }
    entry.increment(error);
  }
  if (error) {
    return fail(code_of(error), detail::utf8(dir) + ": cannot be listed: " + error.message());
  }
  return files;
}

Result<std::string> DiskDataFiles::read(const std::filesystem::path& file) const {
  const std::ifstream stream(file, std::ios::binary);
  if (!stream) {
    std::error_code ignored;
    return fail(std::filesystem::exists(file, ignored) ? Errc::io_error : Errc::not_found,
                detail::utf8(file) + ": cannot be opened");
  }
  std::ostringstream content;
  content << stream.rdbuf();
  if (stream.bad()) {
    return fail(Errc::io_error, detail::utf8(file) + ": cannot be read");
  }
  return content.str();
}

}  // namespace myclean::rules
