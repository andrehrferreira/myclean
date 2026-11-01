#include "core/governor/classifier.hpp"

#include <algorithm>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {

namespace {

char fold(char character) {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character - 'A' + 'a');
  }
  return character == '/' ? '\\' : character;
}

bool equal_folded(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, {}, fold, fold);
}

bool any_of(const std::vector<std::string>& criteria, auto matches) {
  return std::ranges::any_of(criteria, matches);
}

bool entry_matches(const rules::ClassMatch& entry, const ProcessFacts& facts) {
  if (!entry.images.empty() && !any_of(entry.images, [&](const std::string& pattern) {
        return image_matches(pattern, facts.image);
      })) {
    return false;
  }
  if (!entry.path_roots.empty() && !any_of(entry.path_roots, [&](const std::string& root) {
        return under_path_root(root, facts.image_path);
      })) {
    return false;
  }
  if (!entry.signers.empty() &&
      (!facts.signer || !any_of(entry.signers, [&](const std::string& signer) {
        return equal_folded(signer, *facts.signer);
      }))) {
    return false;
  }
  if (entry.command_line && (facts.command_line.empty() ||
                             !std::regex_search(facts.command_line, entry.command_line->regex))) {
    return false;
  }
  return true;
}

}  // namespace

bool image_matches(std::string_view pattern, std::string_view image) {
  // Greedy matching with one backtrack point: the last `*` seen and where its run ends.
  std::size_t at_pattern = 0;
  std::size_t at_image = 0;
  std::size_t star = std::string_view::npos;
  std::size_t star_image = 0;
  while (at_image < image.size()) {
    if (at_pattern < pattern.size() &&
        (pattern[at_pattern] == '?' || fold(pattern[at_pattern]) == fold(image[at_image]))) {
      ++at_pattern;
      ++at_image;
    } else if (at_pattern < pattern.size() && pattern[at_pattern] == '*') {
      star = at_pattern++;
      star_image = at_image;
    } else if (star != std::string_view::npos) {
      at_pattern = star + 1;
      at_image = ++star_image;
    } else {
      return false;
    }
  }
  while (at_pattern < pattern.size() && pattern[at_pattern] == '*') {
    ++at_pattern;
  }
  return at_pattern == pattern.size();
}

bool under_path_root(std::string_view root, std::string_view path) {
  while (!root.empty() && (root.back() == '\\' || root.back() == '/')) {
    root.remove_suffix(1);
  }
  if (root.empty() || path.size() < root.size() ||
      !equal_folded(root, path.substr(0, root.size()))) {
    return false;
  }
  return path.size() == root.size() || fold(path[root.size()]) == '\\';
}

Classification classify(const ProcessFacts& facts, const rules::ClassSet& classes) {
  const auto found = std::ranges::find_if(
      classes.matches, [&](const rules::ClassMatch& entry) { return entry_matches(entry, facts); });
  if (found == classes.matches.end()) {
    return {};
  }
  return {.process_class = found->process_class, .match_id = found->id};
}

rules::ProcessClass final_class(rules::ProcessClass process_class, bool detected_game) {
  if (process_class == rules::ProcessClass::system || !detected_game) {
    return process_class;
  }
  return rules::ProcessClass::game;
}

Result<void> Classifier::reload(Result<rules::ClassSet> loaded) {
  if (!loaded) {
    return tl::unexpected(std::move(loaded.error()));
  }
  classes_ = *std::move(loaded);
  return {};
}

}  // namespace myclean::governor
