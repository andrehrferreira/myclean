#include "cli/report_command.hpp"

#include <ostream>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "core/ipc/reports.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;

std::string dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string text_of(const Json& value, const char* key) {
  const Json member = value.value(key, Json());
  return member.is_string() ? member.get<std::string>() : dump(member);
}

// The answer of reports.get or reports.compare as text.
std::string render(const Json& reply) {
  if (!reply.value("found", false)) {
    std::string text = text_of(reply, "message") + "\n";
    const Json available = reply.value("available", Json::array());
    if (!available.empty()) {
      text += "Months that have one:";
      for (const Json& month : available) {
        text += " " + month.get<std::string>();
      }
      text += "\n";
    }
    return text;
  }
  std::string text = text_of(reply, "markdown");
  if (reply.contains("files")) {
    text += "Markdown: " + text_of(reply.at("files"), "markdown") +
            "\nHTML: " + text_of(reply.at("files"), "html") + "\n";
  }
  return text;
}

// The answer of reports.list as text.
std::string list_text(const Json& reply) {
  const Json reports = reply.value("reports", Json::array());
  std::string text = "Monthly reports, newest first:\n";
  if (reports.empty()) {
    text += "  none yet\n";
  }
  for (const Json& report : reports) {
    const Json against = report.value("against", Json());
    text += text_of(report, "month") + "  " +
            (against.is_string() ? "compared with " + against.get<std::string>()
                                 : std::string("the first report")) +
            "\n    Markdown: " + text_of(report.value("files", Json::object()), "markdown") +
            "\n    HTML: " + text_of(report.value("files", Json::object()), "html") + "\n";
  }
  return text;
}

}  // namespace

int report_command(std::span<const std::string> words, bool json, const mcp::Caller& call,
                   std::ostream& out, std::ostream& err) {
  mcp::Reply reply = tl::unexpected(mcp::CallError{});
  std::string (*text)(const Json&) = &render;
  if (words.size() == 1 && words[0] == "list") {
    reply = call(ipc::reports_list_method, Json::object());
    text = &list_text;
  } else if (words.size() == 2 && words[0] == "get") {
    reply = call(ipc::reports_get_method, Json{{"month", words[1]}});
  } else if (words.size() == 3 && words[0] == "compare") {
    reply = call(ipc::reports_compare_method, Json{{"a", words[1]}, {"b", words[2]}});
  } else {
    err << "myclean: report: list | get <YYYY-MM> | compare <YYYY-MM> <YYYY-MM>\n";
    return exit_usage;
  }
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
  out << (json ? dump(*reply) + "\n" : text(*reply));
  return exit_ok;
}

}  // namespace myclean::cli
