// Win32Registry against HKCU\Software\MyClean-it\registry-<test>: the only registry key these
// tests write, created and deleted by the test itself. Every other key is only read.

#include "platform/settings/registry.hpp"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {
namespace {

using placement::RegistryType;
using placement::RegistryValue;

constexpr const char* non_ascii_folder = "D:\\wsl\\Ubunt\xC3\xBC";  // u with diaeresis, UTF-8

// HKCU\Software\MyClean-it\registry-<test>, deleted with everything below it before and after.
class RegistrySandbox {
 public:
  RegistrySandbox()
      : path_(std::string(R"(Software\MyClean-it\registry-)") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name()),
        wide_(path_.begin(), path_.end()) {
    remove();
  }
  RegistrySandbox(const RegistrySandbox&) = delete;
  RegistrySandbox& operator=(const RegistrySandbox&) = delete;
  RegistrySandbox(RegistrySandbox&&) = delete;
  RegistrySandbox& operator=(RegistrySandbox&&) = delete;
  ~RegistrySandbox() {
    remove();
    ::RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\MyClean-it)");  // only when it is empty
  }

  // "HKCU\Software\MyClean-it\registry-<test>"
  [[nodiscard]] std::string key() const { return "HKCU\\" + path_; }

 private:
  void remove() const { ::RegDeleteTreeW(HKEY_CURRENT_USER, wide_.c_str()); }

  std::string path_;  // ASCII, as the test names are
  std::wstring wide_;
};

TEST(Win32Registry, WritesReadsAndDeletesStringAndDwordValuesInTheSandbox) {
  const RegistrySandbox sandbox;
  Win32Registry registry;
  const std::string key = sandbox.key() + R"(\Lxss\{0f8c33a1})";

  EXPECT_EQ(registry.read(key, "BasePath").value(), std::nullopt);  // no key yet
  ASSERT_TRUE(registry.write(key, "BasePath", RegistryValue{.text = non_ascii_folder}).has_value());
  ASSERT_TRUE(
      registry
          .write(key, "Home",
                 RegistryValue{.text = R"(%USERPROFILE%\x)", .type = RegistryType::expandable})
          .has_value());
  ASSERT_TRUE(
      registry.write(key, "DefaultUid", RegistryValue{.text = "1000", .type = RegistryType::dword})
          .has_value());

  EXPECT_EQ(registry.read(key, "BasePath").value(), (RegistryValue{.text = non_ascii_folder}));
  EXPECT_EQ(registry.read(key, "Home").value(),
            (RegistryValue{.text = R"(%USERPROFILE%\x)", .type = RegistryType::expandable}));
  EXPECT_EQ(registry.read(key, "DefaultUid").value(),
            (RegistryValue{.text = "1000", .type = RegistryType::dword}));
  EXPECT_EQ(
      registry.write(key, "DefaultUid", RegistryValue{.text = "x", .type = RegistryType::dword})
          .error()
          .code,
      Errc::invalid_argument);

  ASSERT_TRUE(registry.remove(key, "BasePath").has_value());
  EXPECT_EQ(registry.read(key, "BasePath").value(), std::nullopt);
  EXPECT_TRUE(registry.remove(key, "BasePath").has_value());  // already gone
  EXPECT_TRUE(registry.remove(sandbox.key() + R"(\missing)", "x").has_value());
}

TEST(Win32Registry, ListsSubkeysAndReadsHklmWithoutWritingIt) {
  const RegistrySandbox sandbox;
  Win32Registry registry;
  const std::string lxss = sandbox.key() + R"(\Lxss)";
  EXPECT_TRUE(registry.subkeys(lxss).value().empty());
  for (const std::string guid : {"{a}", "{b}"}) {
    ASSERT_TRUE(registry
                    .write(std::string(lxss).append("\\").append(guid), "DistributionName",
                           RegistryValue{.text = guid})
                    .has_value());
  }
  EXPECT_EQ(registry.subkeys(lxss).value(), (std::vector<std::string>{"{a}", "{b}"}));

  const Result<std::optional<RegistryValue>> root =
      registry.read(R"(HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion)", "SystemRoot");
  ASSERT_TRUE(root.has_value()) << root.error().message;
  ASSERT_TRUE(root->has_value());
  EXPECT_EQ(root->value_or(RegistryValue{.type = RegistryType::dword}).type, RegistryType::string);
  EXPECT_FALSE(registry.subkeys(R"(HKLM\SOFTWARE\Microsoft)").value().empty());
  EXPECT_EQ(registry.read(R"(HKCR\x)", "y").error().code, Errc::invalid_argument);
}

}  // namespace
}  // namespace myclean::platform
