#include "cli/security_command.hpp"

#include <charconv>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "core/ipc/security.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string text_of(const Json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return value.is_null() ? "n/a" : dump(value);
}

std::string scan_text(const Json& reply) {
  std::string text;
  for (const Json& item : reply.value("items", Json::array())) {
    text += "[" + text_of(item.at("id")) + "] " + text_of(item.at("source")) + "  " +
            text_of(item.at("name")) + "\n    " + text_of(item.at("location")) + "\n    runs " +
            text_of(item.at("command")) + "\n    " + text_of(item.at("summary")) + "\n";
  }
  const Json defender = reply.value("defender", Json::object());
  if (!defender.value("available", false)) {
    text += "Microsoft Defender: " + text_of(defender.value("error", Json())) + "\n";
  }
  for (const Json& detection : defender.value("detections", Json::array())) {
    text += "Defender detected " + text_of(detection.at("threat")) + "\n";
  }
  for (const Json& problem : reply.value("problems", Json::array())) {
    text += "not read: " + text_of(problem) + "\n";
  }
  return text;
}

std::string extensions_text(const Json& reply) {
  std::string text;
  for (const Json& extension : reply.value("extensions", Json::array())) {
    text += text_of(extension.at("browser")) + "/" + text_of(extension.at("profile")) + "  " +
            text_of(extension.at("name")) + " (" + text_of(extension.at("id")) + ")  " +
            text_of(extension.at("origin"));
    if (!extension.at("flag").is_null()) {
      text += "  " + text_of(extension.at("flag"));
    }
    text += "\n";
  }
  return text;
}

std::string network_text(const Json& reply) {
  std::string text;
  for (const Json& process : reply.value("processes", Json::array())) {
    const Json& window = process.at("visible_window");
    text += text_of(process.at("image")) + " (pid " + text_of(process.at("pid")) + ")  sent " +
            text_of(process.at("sent")) + " B, received " + text_of(process.at("received")) + " B" +
            (window.is_boolean() && !window.get<bool>() ? ", no visible window" : std::string()) +
            "\n";
    for (const Json& remote : process.at("remotes")) {
      const Json& name = remote.at("name");
      text += "    " + text_of(remote.at("address")) + ":" + text_of(remote.at("port")) +
              (name.is_string() && !name.get<std::string>().empty() ? " " + name.get<std::string>()
                                                                    : std::string()) +
              "\n";
    }
  }
  return text;
}

int show(const mcp::Reply& reply, bool json, std::ostream& out, std::ostream& err,
         std::string (*render)(const Json&)) {
  if (!reply) {
    if (json) {
      out << dump(Json{{"error", reply.error().message}}) << '\n';
    } else {
      err << "myclean: "
          << (reply.error().reached ? reply.error().message
                                    : mcp::unavailable_text(reply.error().message))
          << '\n';
    }
    return exit_unavailable;
  }
  out << (json ? dump(*reply) + "\n" : render(*reply));
  return exit_ok;
}

std::string started_text(const Json& reply) {
  return "Defender scan started for " + text_of(reply.value("image", Json())) +
         "; its result shows in the next `myclean security scan`\n";
}

}  // namespace

int security_command(std::span<const std::string> words, const std::string* hours, bool json,
                     const mcp::Caller& call, std::ostream& out, std::ostream& err) {
  if (words.size() == 1 && words[0] == "scan") {
    return show(call(ipc::security_scan_method, Json::object()), json, out, err, &scan_text);
  }
  if (words.size() == 1 && words[0] == "extensions") {
    return show(call(ipc::security_extensions_method, Json::object()), json, out, err,
                &extensions_text);
  }
  if (words.size() == 1 && words[0] == "network") {
    Json params = Json::object();
    if (hours != nullptr) {
      double value = 0.0;
      const char* const last = std::to_address(hours->end());
      const auto [end, error] = std::from_chars(hours->data(), last, value);
      if (error != std::errc{} || end != last) {
        err << "myclean: --hours: not a number: " << *hours << '\n';
        return exit_usage;
      }
      params["hours"] = value;
    }
    return show(call(ipc::security_network_method, params), json, out, err, &network_text);
  }
  if (words.size() == 2 && words[0] == "defender-scan") {
    std::int64_t id = 0;
    const char* const last = std::to_address(words[1].end());
    const auto [end, error] = std::from_chars(words[1].data(), last, id);
    if (error != std::errc{} || end != last || id < 1) {
      err << "myclean: security defender-scan: not an item id: " << words[1] << '\n';
      return exit_usage;
    }
    return show(call(ipc::security_defender_scan_method, Json{{"item", id}}), json, out, err,
                &started_text);
  }
  err << "myclean: security: scan | extensions | network [--hours <n>] | defender-scan <id>\n";
  return exit_usage;
}

}  // namespace myclean::cli
