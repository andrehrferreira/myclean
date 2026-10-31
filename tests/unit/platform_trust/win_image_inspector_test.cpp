#include "platform/trust/win_image_inspector.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/trust/trust.hpp"

// Against real files of this machine: Windows images signed through a catalog or embedded, this
// test executable (built here, never signed) and a modified copy of a signed image.
namespace myclean::platform {
namespace {

using std::filesystem::path;

path system32() {
  return path{L"C:\\Windows\\System32"};
}

path this_executable() {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return buffer;
}

// A fresh folder under %TEMP% for the current test, removed afterwards.
class TempFolder {
 public:
  TempFolder() : path_(std::filesystem::temp_directory_path() / "myclean-platform-trust") {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  TempFolder(TempFolder&&) = delete;
  TempFolder& operator=(TempFolder&&) = delete;
  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const path& get() const { return path_; }

 private:
  path path_;
};

TEST(WinImageInspector, CatalogSignedSystemFileIsSignedByMicrosoftWindows) {
  // notepad.exe and cmd.exe carry no embedded signature: only a Windows catalog lists them.
  const WinImageInspector inspector;
  for (const path& image : {system32() / L"notepad.exe", system32() / L"cmd.exe"}) {
    const Result<trust::Signature> signature = inspector.verify(image);
    ASSERT_TRUE(signature.has_value()) << signature.error().message;
    EXPECT_EQ(signature->status, trust::SignatureStatus::valid) << image;
    EXPECT_EQ(signature->signer, "Microsoft Windows") << image;
  }
}

TEST(WinImageInspector, EmbeddedSignatureIsSignedWithItsSigner) {
  // svchost.exe of this Windows build is signed in the file itself.
  const WinImageInspector inspector;
  const Result<trust::Signature> signature = inspector.verify(system32() / L"svchost.exe");
  ASSERT_TRUE(signature.has_value()) << signature.error().message;
  EXPECT_EQ(signature->status, trust::SignatureStatus::valid);
  // The Windows build decides which of Microsoft's two Windows certificates signs it.
  EXPECT_TRUE(signature->signer == "Microsoft Windows Publisher" ||
              signature->signer == "Microsoft Windows")
      << signature->signer.value_or("no signer");
}

TEST(WinImageInspector, UnsignedImageHasNoSigner) {
  const WinImageInspector inspector;
  const Result<trust::Signature> signature = inspector.verify(this_executable());
  ASSERT_TRUE(signature.has_value()) << signature.error().message;
  EXPECT_EQ(signature->status, trust::SignatureStatus::none);
  EXPECT_FALSE(signature->signer.has_value());
}

TEST(WinImageInspector, ModifiedSignedImageIsInvalidNotSigned) {
  // curl.exe has an embedded signature; one byte of its code is changed in a copy.
  const TempFolder folder;
  const path copy = folder.get() / L"tampered.bin";
  std::filesystem::copy_file(system32() / L"curl.exe", copy);
  {
    std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(0x1000);
    const char byte = static_cast<char>(file.get());
    file.seekp(0x1000);
    file.put(static_cast<char>(byte ^ 0x5A));
    ASSERT_TRUE(file.good());
  }
  const WinImageInspector inspector;
  const Result<trust::Signature> signature = inspector.verify(copy);
  ASSERT_TRUE(signature.has_value()) << signature.error().message;
  EXPECT_EQ(signature->status, trust::SignatureStatus::invalid);
  EXPECT_FALSE(signature->signer.has_value());
}

TEST(WinImageInspector, StampIsSizeAndLastWriteTime) {
  const TempFolder folder;
  const path file = folder.get() / L"stamp.bin";
  const auto written = std::chrono::system_clock::now();
  std::ofstream(file, std::ios::binary) << "abc";
  const WinImageInspector inspector;
  const Result<trust::FileStamp> stamp = inspector.stamp(file);
  ASSERT_TRUE(stamp.has_value()) << stamp.error().message;
  EXPECT_EQ(stamp->size, 3U);
  EXPECT_LT(std::chrono::abs(stamp->modified - written), std::chrono::minutes{1});

  std::ofstream(file, std::ios::binary | std::ios::app) << "de";
  const Result<trust::FileStamp> changed = inspector.stamp(file);
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->size, 5U);
}

TEST(WinImageInspector, MissingFileIsNotFound) {
  const WinImageInspector inspector;
  const path missing = system32() / L"myclean-no-such-image.exe";
  ASSERT_FALSE(inspector.stamp(missing).has_value());
  EXPECT_EQ(inspector.stamp(missing).error().code, Errc::not_found);
  EXPECT_EQ(inspector.verify(missing).error().code, Errc::not_found);
}

TEST(UserWritableRoots, ProfilesAndWindowsTempAreUserWritableProgramFilesIsNot) {
  const Result<std::vector<path>> roots = user_writable_roots();
  ASSERT_TRUE(roots.has_value()) << roots.error().message;
  std::wstring profile(MAX_PATH, L'\0');
  profile.resize(::GetEnvironmentVariableW(L"USERPROFILE", profile.data(), MAX_PATH));
  ASSERT_FALSE(profile.empty());
  using trust::PathClass;
  EXPECT_EQ(trust::classify_path(path{profile} / L"AppData\\Local\\Foo\\foo.exe", *roots),
            PathClass::user_writable);
  EXPECT_EQ(trust::classify_path(path{profile} / L"Downloads\\setup.exe", *roots),
            PathClass::user_writable);
  EXPECT_EQ(trust::classify_path(L"C:\\Users\\Public\\x.exe", *roots), PathClass::user_writable);
  EXPECT_EQ(trust::classify_path(L"C:\\Windows\\Temp\\x.exe", *roots), PathClass::user_writable);
  EXPECT_EQ(trust::classify_path(L"C:\\Program Files\\Foo\\foo.exe", *roots),
            PathClass::protected_location);
  EXPECT_EQ(trust::classify_path(system32() / L"svchost.exe", *roots),
            PathClass::protected_location);
}

TEST(TrustEvaluator, RealImagesEndToEnd) {
  const WinImageInspector inspector;
  const Result<std::vector<path>> roots = user_writable_roots();
  ASSERT_TRUE(roots.has_value());
  trust::TrustEvaluator evaluator(inspector, *roots);
  const Result<trust::TrustSignal> unknown = evaluator.evaluate(this_executable(), {});
  ASSERT_TRUE(unknown.has_value()) << unknown.error().message;
  EXPECT_TRUE(unknown->unrecognized());
  EXPECT_EQ(unknown->reasons.at(1), trust::Reason::unsigned_image);
}

}  // namespace
}  // namespace myclean::platform
