#include "core/rules/schema.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean::rules {

namespace {

constexpr std::array<std::string_view, 4> strategy_names = {"delete", "quarantine", "native",
                                                            "handler"};
constexpr std::array<std::string_view, 6> process_class_names = {"system", "game", "allow",
                                                                 "build",  "test", "default"};

}  // namespace

std::string_view to_string(Strategy strategy) {
  return detail::enum_name(strategy_names, strategy);
}

std::optional<Strategy> parse_strategy(std::string_view text) {
  return detail::enum_from_name<Strategy>(strategy_names, text);
}

std::string_view to_string(ProcessClass process_class) {
  return detail::enum_name(process_class_names, process_class);
}

std::optional<ProcessClass> parse_process_class(std::string_view text) {
  return detail::enum_from_name<ProcessClass>(process_class_names, text);
}

}  // namespace myclean::rules
