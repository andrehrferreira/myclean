#pragma once

#include <stop_token>
#include <string>
#include <utility>

#include "core/model/action.hpp"
#include "core/model/result.hpp"

// Removing an Appx package (apps spec, "Uninstall Through The App's Own Uninstaller After
// Approval"): the tray removes one of the owner's packages through PackageManager once the owner
// approved the plan, as Settings > Apps > Uninstall does.
namespace myclean::user_tasks {

// The owner's Appx packages. Implemented in src/tray; tests use a fake that removes nothing.
class IPackageRemover {
 public:
  virtual ~IPackageRemover() = default;
  IPackageRemover(const IPackageRemover&) = delete;
  IPackageRemover& operator=(const IPackageRemover&) = delete;
  IPackageRemover(IPackageRemover&&) = delete;
  IPackageRemover& operator=(IPackageRemover&&) = delete;

  // Whether the package `full_name` is installed for the caller.
  [[nodiscard]] virtual Result<bool> installed(const std::string& full_name) const = 0;
  // Removes it for the caller (PackageManager.RemovePackageAsync) and waits for the end.
  [[nodiscard]] virtual Result<void> remove(const std::string& full_name,
                                            const std::stop_token& stop) = 0;

 protected:
  IPackageRemover() = default;
};

// Removes one Appx package (tier ask, run in the owner's context). `package` is its full name,
// `name` its display name.
//   preview  one item "uninstall:<name>"; fails with not_found when it is not installed any more;
//   apply    removes it; the undo data records the package; it cannot be undone (the app comes
//            back from the Microsoft Store or its installer);
//   verify   metric "uninstall_entry": better when it is gone; fails while it is still installed;
//   undo     fails with not_reversible.
class AppxRemoval final : public Action {
 public:
  // `remover` must outlive the action.
  AppxRemoval(IPackageRemover& remover, std::string package, std::string name, std::stop_token stop)
      : remover_(&remover),
        package_(std::move(package)),
        name_(std::move(name)),
        stop_(std::move(stop)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IPackageRemover* remover_;
  std::string package_;
  std::string name_;
  std::stop_token stop_;
};

}  // namespace myclean::user_tasks
