#include "core/safety/paths.hpp"

#include <filesystem>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::safety {
namespace {

using std::filesystem::path;

TEST(NormalizePath, ProducesOneSpellingPerPath) {
  const path expected = L"C:\\Windows\\System32\\drivers";
  for (const path& spelling : {
           path{L"C:\\Windows\\System32\\drivers"},
           path{L"c:\\Windows\\System32\\drivers"},
           path{L"C:/Windows/System32/drivers/"},
           path{L"\\\\?\\C:\\Windows\\System32\\drivers"},
           path{L"\\??\\C:\\Windows\\System32\\drivers"},
           path{L"C:\\Windows\\\\System32\\.\\drivers\\\\"},
           path{L"C:\\Windows\\Temp\\..\\System32\\drivers"},
           path{L"C:\\..\\..\\Windows\\System32\\drivers"},
           path{L"C:\\Windows.\\System32 \\drivers. ."},
       }) {
    EXPECT_EQ(normalize_path(spelling).value(), expected) << to_utf8(spelling);
  }
  EXPECT_EQ(normalize_path(L"d:\\").value(), path{L"D:\\"});
  EXPECT_EQ(normalize_path(L"D:\\data\\..").value(), path{L"D:\\"});
}

TEST(NormalizePath, RejectsEverythingButAbsoluteDrivePaths) {
  for (const path& spelling : {
           path{L""},
           path{L"Temp\\a.txt"},
           path{L"C:Temp\\a.txt"},
           path{L"\\Temp\\a.txt"},
           path{L"\\\\server\\share\\a.txt"},
           path{L"\\\\?\\UNC\\server\\share\\a.txt"},
           path{L"\\\\.\\C:\\a.txt"},
           path{L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\a.txt"},
           path{L"C:\\Temp\\a.txt:hidden"},
           path{L"C:\\Temp\\*.txt"},
           path{L"C:\\Temp\\. .\\a.txt"},
       }) {
    const Result<path> normal = normalize_path(spelling);
    ASSERT_FALSE(normal.has_value()) << to_utf8(spelling);
    EXPECT_EQ(normal.error().code, Errc::invalid_argument);
  }
}

}  // namespace
}  // namespace myclean::safety
