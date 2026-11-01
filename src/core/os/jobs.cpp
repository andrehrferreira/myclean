#include "core/os/jobs.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/os/process_control.hpp"

namespace myclean::os {

namespace {

constexpr std::size_t max_class_length = 32;
constexpr std::size_t nonce_length = 32;

bool class_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

std::string JobRef::object_name() const {
  return "Global\\MyClean.Job." + job_class + "." + nonce;
}

Result<void> validate_job_class(std::string_view job_class) {
  if (job_class.empty() || job_class.size() > max_class_length ||
      !std::ranges::all_of(job_class, class_char)) {
    return fail(Errc::invalid_argument,
                "job class must be 1-32 of [a-z0-9_-]: '" + std::string(job_class) + "'");
  }
  return {};
}

Result<void> validate(const JobRef& job) {
  if (Result<void> valid = validate_job_class(job.job_class); !valid) {
    return valid;
  }
  if (job.nonce.size() != nonce_length || !std::ranges::all_of(job.nonce, hex_digit)) {
    return fail(Errc::invalid_argument,
                "job nonce must be 32 lowercase hex digits: '" + job.nonce + "'");
  }
  return {};
}

Result<void> assign_tree_root(const ProcessKey& root, IJob* parent, IJob& tree) {
  if (parent != nullptr) {
    if (Result<void> assigned = parent->assign(root); !assigned) {
      return assigned;
    }
  }
  return tree.assign(root);
}

}  // namespace myclean::os
