// The shipped knowledge base, the kb.json of every pack in data/packs, loaded as the service loads
// it and matched against
// image paths and signers as this machine reports them.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/rules/schema.hpp"
#include "core/trust/trust.hpp"

namespace myclean::trust {
namespace {

class KbData : public ::testing::Test {
 protected:
  KbData() {
    const packs::LoadReport report =
        packs::load_packs(packs::DiskPackFiles{}, std::filesystem::path{MYCLEAN_DATA_DIR} / "packs",
                          std::nullopt, packs::this_version());
    for (const Error& error : report.errors) {
      ADD_FAILURE() << error.message;
    }
    entries_ = packs::make_registry(report.packs).kb;
  }

  [[nodiscard]] const std::vector<rules::KbEntry>& entries() const { return entries_; }
  // Null when no entry has `id`.
  [[nodiscard]] const rules::KbEntry* find(std::string_view id) const {
    const auto found = std::ranges::find(entries_, id, &rules::KbEntry::id);
    return found == entries_.end() ? nullptr : &*found;
  }

 private:
  std::vector<rules::KbEntry> entries_;
};

TEST_F(KbData, EveryFileLoadsAndEveryEntryHasAKnownCategory) {
  const std::set<std::string_view> categories = {
      "browser",     "chat",          "cloud-sync",     "creative",  "dev-tool",
      "driver",      "game",          "launcher",       "licensing", "network",
      "oem-utility", "remote-access", "runtime",        "security",  "system",
      "updater",     "utility",       "virtualization", "vpn"};
  ASSERT_GE(entries().size(), 100U);
  for (const rules::KbEntry& entry : entries()) {
    EXPECT_TRUE(categories.contains(entry.category)) << entry.id << ": " << entry.category;
  }
}

struct Seen {
  const wchar_t* image;
  const char* signer;
  const char* entry;
};

// Images and signers as WinVerifyTrust reported them on the reference machine.
const std::vector<Seen> seen = {
    {.image = L"C:\\Program Files\\PowerToys\\modules\\KeyboardManager\\KeyboardManagerEngine\\"
              L"PowerToys.KeyboardManagerEngine.exe",
     .signer = "Microsoft Corporation",
     .entry = "windows/microsoft.powertoys.keyboard-manager"},
    {.image = L"C:\\Program Files (x86)\\AnyDesk\\AnyDesk.exe",
     .signer = "AnyDesk Software GmbH",
     .entry = "security-network/anydesk"},
    {.image = L"C:\\Users\\owner\\AppData\\Local\\Discord\\app-1.0.9259\\Discord.exe",
     .signer = "Discord Inc.",
     .entry = "electron-apps/discord"},
    {.image = L"C:\\Program Files\\ASUS\\ARMOURY CRATE Lite Service\\ArmouryCrate.Service.exe",
     .signer = "ASUSTeK COMPUTER INC.",
     .entry = "asus/armoury-crate.service"},
    {.image = L"C:\\WINDOWS\\system32\\svchost.exe",
     .signer = "Microsoft Windows Publisher",
     .entry = "windows/svchost"},
    {.image =
         L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\TextInputHost.exe",
     .signer = "Microsoft Windows",
     .entry = "windows/text-input-host"},
    {.image = L"C:\\WINDOWS\\Explorer.EXE",
     .signer = "Microsoft Windows",
     .entry = "windows/explorer"},
    {.image = L"C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
     .signer = "Microsoft Windows",
     .entry = "windows/powershell"},
    {.image = L"C:\\Windows\\System32\\wbem\\WmiPrvSE.exe",
     .signer = "Microsoft Windows",
     .entry = "windows/wmi-provider-host"},
    {.image = L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\amd3dvcache.inf_amd64_"
              L"558311a8a60226a4\\"
              L"amd3dvcacheSvc.exe",
     .signer = "Microsoft Windows Hardware Compatibility Publisher",
     .entry = "gpu/amd.3d-vcache-optimizer"},
    {.image = L"C:\\Program Files (x86)\\Kaspersky Lab\\Kaspersky 21.26\\avp.exe",
     .signer = "Kaspersky Labs GmbH",
     .entry = "security-network/kaspersky.avp-service"},
    {.image =
         L"C:\\ProgramData\\Kaspersky Lab\\AVP21.26\\Data\\webview2\\Win10\\msedgewebview2.exe",
     .signer = "Microsoft Corporation",
     .entry = "browsers/microsoft.edge-webview2"},
    {.image = L"C:\\Program Files (x86)\\Steam\\Steam.exe",
     .signer = "Valve Corp.",
     .entry = "games/valve.steam"},
};

TEST_F(KbData, ResidentAppsAndKnownSuspectsMatchTheirEntry) {
  for (const Seen& process : seen) {
    const TrustSignal signal =
        assess(process.image, Signature{.status = SignatureStatus::valid, .signer = process.signer},
               PathClass::protected_location, entries());
    const rules::KbEntry entry = signal.entry.value_or(rules::KbEntry{});
    EXPECT_EQ(entry.id, process.entry);
    EXPECT_FALSE(entry.description.empty()) << process.entry;
    EXPECT_FALSE(signal.unrecognized()) << process.entry;
  }
}

TEST_F(KbData, ClipboardHistoryIsExplained) {
  const rules::KbEntry* svchost = find("windows/svchost");
  const rules::KbEntry* panel = find("windows/text-input-host");
  ASSERT_NE(svchost, nullptr);
  ASSERT_NE(panel, nullptr);
  EXPECT_NE(svchost->description.find("clipboard history"), std::string::npos);
  EXPECT_NE(panel->description.find("clipboard history"), std::string::npos);
}

TEST_F(KbData, WindowsImageOutsideItsFolderIsUnrecognized) {
  const TrustSignal signal =
      assess(L"C:\\Users\\owner\\AppData\\Local\\Temp\\svchost.exe",
             Signature{.status = SignatureStatus::valid, .signer = "Microsoft Windows Publisher"},
             PathClass::user_writable, entries());
  EXPECT_FALSE(signal.entry.has_value());
  EXPECT_EQ(signal.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::user_writable_path}));
}

TEST_F(KbData, UnsignedLookAlikeIsUnrecognized) {
  const TrustSignal signal =
      assess(L"C:\\Program Files (x86)\\AnyDesk\\AnyDesk.exe",
             Signature{.status = SignatureStatus::none}, PathClass::protected_location, entries());
  EXPECT_FALSE(signal.entry.has_value());
  EXPECT_EQ(signal.reasons,
            (std::vector<Reason>{Reason::not_in_knowledge_base, Reason::unsigned_image}));
}

}  // namespace
}  // namespace myclean::trust
