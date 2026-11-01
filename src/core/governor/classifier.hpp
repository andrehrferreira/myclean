#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"
#include "core/rules/schema.hpp"

// Process classification (governor spec, "Process Classification"): a starting process against
// the ordered class entries of the knowledge packs (rules::ClassSet, from packs::Registry), the
// first matching entry wins and an unmatched process is `default` (rules::ProcessClass::normal).
namespace myclean::governor {

// What is known about a process when it starts. Text is UTF-8.
struct ProcessFacts {
  std::string image;                  // file name only ("cl.exe")
  std::string image_path;             // full path of the image ("C:\Program Files\...\cl.exe")
  std::string command_line;           // empty when it could not be read
  std::optional<std::string> signer;  // the verified signer's name; absent when unsigned
};

struct Classification {
  rules::ProcessClass process_class = rules::ProcessClass::normal;
  std::string match_id;  // "<pack>/<case>" of the entry that matched; empty for `default`
};

// `image` matches `pattern`, ignoring ASCII case; `*` matches any run of characters (also none)
// and `?` exactly one.
[[nodiscard]] bool image_matches(std::string_view pattern, std::string_view image);

// `path` is `root` or lies below it, ignoring ASCII case, at a folder boundary: root "E:\Games"
// takes "E:\Games\a.exe" but not "E:\GamesOld\a.exe". `/` counts as `\`; a trailing separator of
// the root is ignored.
[[nodiscard]] bool under_path_root(std::string_view root, std::string_view path);

// The first entry of `classes` whose every criterion matches `facts` (a list criterion matches
// when any element does): image names by image_matches, path roots by under_path_root, signers
// by equality ignoring ASCII case, the command line by a search with the entry's Pattern. A
// criterion the process has no data for (no signer, an unreadable command line) does not match.
[[nodiscard]] Classification classify(const ProcessFacts& facts, const rules::ClassSet& classes);

// The class the governor acts on, given the game detector's verdict for the process: `system`
// stays `system`; otherwise a detected game is `game`, whatever the game-mode override says.
[[nodiscard]] rules::ProcessClass final_class(rules::ProcessClass process_class,
                                              bool detected_game);

// The classes in effect. A reload that failed (a malformed classes.toml, rejected whole by
// rules::parse_class_file) keeps the previous classes.
class Classifier {
 public:
  explicit Classifier(rules::ClassSet classes) : classes_(std::move(classes)) {}

  // Takes `loaded` when it holds a class set; otherwise returns its error and changes nothing.
  Result<void> reload(Result<rules::ClassSet> loaded);

  [[nodiscard]] Classification classify(const ProcessFacts& facts) const {
    return governor::classify(facts, classes_);
  }
  [[nodiscard]] const rules::ClassSet& classes() const { return classes_; }

 private:
  rules::ClassSet classes_;
};

}  // namespace myclean::governor
