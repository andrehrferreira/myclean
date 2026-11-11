#include "svc/security_component.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/events/events.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/security.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/security/extensions.hpp"
#include "core/security/network.hpp"
#include "core/security/persistence.hpp"
#include "core/security/scan.hpp"
#include "core/security/verdicts.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"
#include "svc/trust_component.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;

constexpr double default_hours = 24.0;

Json time_json(const std::optional<WallTime>& time) {
  return time ? Json(store::to_unix_seconds(*time)) : Json(nullptr);
}

Json text_or_null(const std::optional<std::string>& text) {
  return text ? Json(*text) : Json(nullptr);
}

// The key of a file in file_verdict: its path folded to lower case.
std::string path_key(const std::filesystem::path& image) {
  std::string key = utf8_text(image);
  std::ranges::transform(key, key.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return key;
}

Json verdict_json(const security::Verdict& verdict) {
  return Json{{"source", verdict.source},
              {"threat", verdict.threat.empty() ? Json(nullptr) : Json(verdict.threat)},
              {"flagged", verdict.flagged ? Json(*verdict.flagged) : Json(nullptr)},
              {"engines", verdict.engines ? Json(*verdict.engines) : Json(nullptr)},
              {"time", time_json(verdict.time)}};
}

Json detections_json(const std::vector<security::Detection>& history) {
  std::vector<const security::Detection*> newest;
  newest.reserve(history.size());
  for (const security::Detection& detection : history) {
    newest.push_back(&detection);
  }
  std::ranges::stable_sort(newest, [](const security::Detection* a, const security::Detection* b) {
    return a->time > b->time;
  });
  Json list = Json::array();
  for (std::size_t index = 0; index < newest.size() && index < SecurityComponent::max_detections;
       ++index) {
    list.push_back(Json{{"threat", newest[index]->threat},
                        {"resources", newest[index]->resources},
                        {"time", time_json(newest[index]->time)}});
  }
  return list;
}

}  // namespace

SecurityComponent::SecurityComponent(SecuritySources sources) : sources_(std::move(sources)) {
  if (sources_.resolver) {
    dns_ = std::make_unique<security::ReverseDns>(*sources_.resolver);
  }
}

SecurityComponent::~SecurityComponent() {
  stop();
}

Result<void> SecurityComponent::start(const Context& context) {
  store_ = &context.store();
  clock_ = &context.clock();
  const std::string& owner = context.owner_sid();
  if (sources_.startup) {
    startup_ = sources_.startup(owner);
  }
  if (sources_.persistence) {
    persistence_ = sources_.persistence(owner);
  }
  if (sources_.browsers) {
    browsers_ = sources_.browsers(owner);
  }
  if (sources_.paths) {
    paths_ = sources_.paths(owner);
  }
  for (store::RetentionRule rule :
       {store::RetentionRule{.table = "visible_window",
                             .time_column = "last_seen",
                             .max_age = store::Store::minute_retention},
        store::RetentionRule{.table = "file_verdict",
                             .time_column = "time",
                             .max_age = store::Store::minute_retention}}) {
    if (Result<void> added = store_->add_retention(std::move(rule)); !added) {
      return added;
    }
  }
  windows_ = std::make_unique<security::WindowLog>(*store_);
  if (Result<void> loaded = windows_->load(
          clock_->now() - std::chrono::hours{static_cast<int>(ipc::security_max_hours)});
      !loaded) {
    note("visible windows", loaded.error().message);
  }

  using ipc::Param;
  using ipc::ParamType;
  ipc::Dispatcher& methods = context.methods();
  Result<void> registered =
      methods
          .add(std::string(ipc::security_scan_method), {}, [this](const Json&) { return scan(); })
          .and_then([&] {
            return methods.add(std::string(ipc::security_extensions_method), {},
                               [this](const Json&) { return extensions(); });
          })
          .and_then([&] {
            return methods.add(std::string(ipc::security_network_method),
                               {Param{.name = "hours", .type = ParamType::number}},
                               [this](const Json& params) { return network(params); });
          })
          .and_then([&] {
            return methods.add(
                std::string(ipc::security_defender_scan_method),
                {Param{.name = "item", .type = ParamType::integer, .required = true}},
                [this](const Json& params) { return defender_scan(params); });
          })
          .and_then([&] {
            return methods.add(
                std::string(ipc::security_windows_method),
                {Param{
                    .name = "windows",
                    .type = ParamType::record,
                    .required = true,
                    .schema =
                        [](const Json& record) { return security::check_windows_record(record); }}},
                [this](const Json& params) { return windows(params); }, ipc::Access::tray);
          });
  if (!registered) {
    return registered;
  }
  if (sources_.period > std::chrono::milliseconds::zero()) {
    // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  }
  return {};
}

void SecurityComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  if (defender_thread_.joinable()) {
    defender_thread_.request_stop();
    defender_thread_.join();
  }
}

void SecurityComponent::run(const std::stop_token& stop) {
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(wait_mutex);
  while (!stop.stop_requested()) {
    tick();
    static_cast<void>(wake.wait_for(lock, stop, sources_.period, [] { return false; }));
  }
}

void SecurityComponent::tick() {
  const WallTime now = clock_->now();
  if (dns_) {
    dns_->resolve(now, names_per_tick);
  }
  std::filesystem::path image;
  std::string key;
  {
    const std::scoped_lock lock(mutex_);
    if (lookups_.empty() || api_key_.empty() || (next_lookup_ && now < *next_lookup_)) {
      return;
    }
    image = std::move(lookups_.front());
    lookups_.pop_front();
    queued_.erase(image);
    key = api_key_;
    next_lookup_ = now + virustotal_spacing;
  }
  look_up(image, key);
}

void SecurityComponent::look_up(const std::filesystem::path& image, const std::string& api_key) {
  if (!sources_.hasher || !sources_.virustotal || !sources_.stamps) {
    return;
  }
  const Result<trust::FileStamp> stamp = sources_.stamps->stamp(image);
  if (!stamp) {
    return;  // gone since the scan
  }
  const Result<space::Digest> digest = sources_.hasher->hash(image, std::nullopt);
  if (!digest) {
    std::string problem = "hashing ";
    problem.append(utf8_text(image)).append(": ").append(digest.error().message);
    note("VirusTotal", problem);
    return;
  }
  // Only the hash and the owner's key leave the machine.
  const Result<std::optional<security::HashReport>> report =
      sources_.virustotal->lookup(security::hex(*digest), api_key);
  if (!report) {
    note("VirusTotal", report.error().message);
    return;
  }
  note("VirusTotal", std::nullopt);
  const security::HashReport found = report->value_or(security::HashReport{});
  store_verdict(image, security::virustotal_source,
                Stored{.stamp = *stamp,
                       .detected = found.malicious > 0,
                       .flagged = found.malicious,
                       .engines = found.engines,
                       .time = clock_->now()});
}

void SecurityComponent::note(std::string_view what, const std::optional<std::string>& problem) {
  const std::scoped_lock lock(mutex_);
  const std::string key(what);
  if (!problem) {
    problems_.erase(key);
    return;
  }
  if (const auto known = problems_.find(key);
      known == problems_.end() || known->second != *problem) {
    spdlog::warn("security: {}: {}", key, *problem);
  }
  problems_.insert_or_assign(key, *problem);
}

ipc::ComponentHealth SecurityComponent::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  const std::scoped_lock lock(mutex_);
  for (const auto& [what, problem] : problems_) {
    std::string line = what;
    line.append(": ").append(problem);
    health.problems.push_back(std::move(line));
  }
  health.healthy = health.problems.empty();
  return health;
}

security::Settings SecurityComponent::settings() {
  if (!sources_.settings) {
    return {};
  }
  const Result<std::optional<std::string>> text = sources_.settings();
  if (!text) {
    note("settings", text.error().message);
    return {};
  }
  if (!*text) {
    note("settings", std::nullopt);
    return {};
  }
  Result<security::Settings> parsed = security::parse_settings(**text);
  if (!parsed) {
    note("settings", parsed.error().message);
    return {};
  }
  note("settings", std::nullopt);
  return std::move(*parsed);
}

std::vector<security::LiveProcess> SecurityComponent::live() const {
  std::vector<security::LiveProcess> processes;
  const std::shared_ptr<const telemetry::ProcessView> view =
      sources_.view ? sources_.view() : nullptr;
  if (!view) {
    return processes;
  }
  processes.reserve(view->rows.size());
  for (const telemetry::ProcessRow& row : view->rows) {
    processes.push_back(security::LiveProcess{.pid = row.info.pid,
                                              .created = row.info.created,
                                              .image = row.info.image,
                                              .session = row.info.session});
  }
  return processes;
}

Result<std::vector<security::Detection>> SecurityComponent::detections() {
  if (!sources_.defender) {
    return fail(Errc::not_found, "Microsoft Defender is not available");
  }
  const WallTime now = clock_->now();
  {
    const std::scoped_lock lock(mutex_);
    if (detections_time_ && now - *detections_time_ < defender_cache) {
      if (detections_error_) {
        return tl::unexpected(*detections_error_);
      }
      return *detections_;
    }
  }
  Result<std::vector<security::Detection>> read = sources_.defender->detections();
  const std::scoped_lock lock(mutex_);
  detections_time_ = now;
  if (!read) {
    detections_error_ = read.error();
    detections_.reset();
    return read;
  }
  detections_error_.reset();
  detections_ = *read;
  return read;
}

std::optional<SecurityComponent::Stored> SecurityComponent::stored(
    const std::filesystem::path& image, std::string_view source) const {
  if (!sources_.stamps) {
    return std::nullopt;
  }
  const Result<trust::FileStamp> stamp = sources_.stamps->stamp(image);
  if (!stamp) {
    return std::nullopt;
  }
  std::optional<Stored> found;
  static_cast<void>(store_->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(
        "SELECT size, modified, detected, threat, flagged, engines, time FROM file_verdict "
        "WHERE path = ?1 AND source = ?2");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, std::string_view{path_key(image)}).bind(2, source);
    Result<bool> row = statement->step();
    if (!row || !*row) {
      return {};
    }
    const trust::FileStamp recorded{.size = static_cast<std::uint64_t>(statement->column_int(0)),
                                    .modified = store::from_unix_seconds(statement->column_int(1))};
    // A changed file is a different file: its stored verdict no longer applies.
    if (recorded.size != stamp->size ||
        store::to_unix_seconds(recorded.modified) != store::to_unix_seconds(stamp->modified)) {
      return {};
    }
    found =
        Stored{.stamp = recorded,
               .detected = statement->column_int(2) != 0,
               .threat = statement->is_null(3) ? std::string() : statement->column_text(3),
               .flagged = statement->is_null(4)
                              ? std::nullopt
                              : std::optional(static_cast<std::uint32_t>(statement->column_int(4))),
               .engines = statement->is_null(5)
                              ? std::nullopt
                              : std::optional(static_cast<std::uint32_t>(statement->column_int(5))),
               .time = store::from_unix_seconds(statement->column_int(6))};
    return {};
  }));
  return found;
}

void SecurityComponent::store_verdict(const std::filesystem::path& image, std::string_view source,
                                      const Stored& verdict) const {
  const Result<void> written = store_->write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(R"sql(
INSERT INTO file_verdict(path, source, size, modified, detected, threat, flagged, engines, time)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
ON CONFLICT(path, source) DO UPDATE SET size = excluded.size, modified = excluded.modified,
  detected = excluded.detected, threat = excluded.threat, flagged = excluded.flagged,
  engines = excluded.engines, time = excluded.time)sql");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, std::string_view{path_key(image)})
        .bind(2, source)
        .bind(3, static_cast<std::int64_t>(verdict.stamp.size))
        .bind(4, store::to_unix_seconds(verdict.stamp.modified))
        .bind(5, std::int64_t{verdict.detected ? 1 : 0})
        .bind(9, store::to_unix_seconds(verdict.time));
    if (verdict.threat.empty()) {
      statement->bind(6, nullptr);
    } else {
      statement->bind(6, std::string_view{verdict.threat});
    }
    if (verdict.flagged) {
      statement->bind(7, std::int64_t{*verdict.flagged});
    } else {
      statement->bind(7, nullptr);
    }
    if (verdict.engines) {
      statement->bind(8, std::int64_t{*verdict.engines});
    } else {
      statement->bind(8, nullptr);
    }
    return statement->run();
  });
  if (!written) {
    spdlog::warn("security: storing the {} verdict of {}: {}", source, utf8_text(image),
                 written.error().message);
  }
}

std::optional<security::Verdict> SecurityComponent::verdict_of(
    const std::filesystem::path& image, std::span<const security::Detection> known) const {
  if (const security::Detection* detection = security::detection_of(known, image)) {
    return security::Verdict{.source = std::string(security::defender_source),
                             .threat = detection->threat,
                             .time = detection->time};
  }
  if (const std::optional<Stored> scanned = stored(image, security::defender_source);
      scanned && scanned->detected) {
    return security::Verdict{.source = std::string(security::defender_source),
                             .threat = scanned->threat,
                             .time = scanned->time};
  }
  if (const std::optional<Stored> looked = stored(image, security::virustotal_source);
      looked && looked->detected) {
    return security::Verdict{.source = std::string(security::virustotal_source),
                             .flagged = looked->flagged,
                             .engines = looked->engines,
                             .time = looked->time};
  }
  return std::nullopt;
}

std::vector<security::Finding> SecurityComponent::findings(
    std::span<const security::Detection> known, Json& problems) const {
  std::vector<startup::Item> items;
  if (startup_) {
    if (Result<startup::Snapshot> snapshot = startup_->read(); snapshot) {
      items = startup::inventory(*snapshot);
    } else {
      problems.push_back("startup items: " + snapshot.error().message);
    }
  }
  security::PersistenceSnapshot extra;
  if (persistence_) {
    extra = persistence_->read();
  }
  for (const std::string& problem : extra.problems) {
    problems.push_back(problem);
  }
  std::vector<security::Finding> found;
  for (security::Point& point : security::persistence_points(items, extra)) {
    std::optional<std::filesystem::path> image =
        paths_ && !point.target.empty() ? paths_->resolve(point.target) : std::nullopt;
    std::optional<trust::TrustSignal> signal;
    std::optional<security::Verdict> verdict;
    if (image) {
      if (sources_.image_trust) {
        if (Result<trust::TrustSignal> evaluated = sources_.image_trust(*image); evaluated) {
          signal = std::move(*evaluated);
        }
      }
      verdict = verdict_of(*image, known);
    }
    found.push_back(security::assess(std::move(point), std::move(image), std::move(signal),
                                     std::move(verdict)));
  }
  return found;
}

Json SecurityComponent::item_json(const security::Finding& finding, std::size_t id,
                                  std::vector<std::filesystem::path>& to_look_up) const {
  Json reasons = Json::array();
  for (const security::Suspicion reason : finding.reasons) {
    reasons.push_back(security::to_string(reason));
  }
  Json virustotal = nullptr;
  if (finding.image) {
    if (const std::optional<Stored> looked = stored(*finding.image, security::virustotal_source)) {
      virustotal = Json{{"flagged", looked->flagged.value_or(0)},
                        {"engines", looked->engines.value_or(0)},
                        {"time", store::to_unix_seconds(looked->time)}};
      if (clock_->now() - looked->time > virustotal_fresh) {
        to_look_up.push_back(*finding.image);  // stale: asked again
      }
    } else if (finding.status == security::Status::suspicious) {
      to_look_up.push_back(*finding.image);
    }
  }
  const security::Point& point = finding.point;
  return Json{{"id", id},
              {"source", security::to_string(point.source)},
              {"location", point.location},
              {"name", point.name},
              {"command", point.command},
              {"target", point.target.empty() ? Json(nullptr) : Json(point.target)},
              {"image", finding.image ? Json(utf8_text(*finding.image)) : Json(nullptr)},
              {"enabled", point.enabled ? Json(*point.enabled) : Json(nullptr)},
              {"signer", finding.trust && finding.trust->signature.signer
                             ? Json(*finding.trust->signature.signer)
                             : text_or_null(point.signer)},
              {"startup_item", text_or_null(point.startup_item)},
              {"trust", finding.trust ? trust_json(*finding.trust) : Json(nullptr)},
              {"status", security::to_string(finding.status)},
              {"reasons", std::move(reasons)},
              {"summary", security::summary(finding)},
              {"verdict", finding.verdict ? verdict_json(*finding.verdict) : Json(nullptr)},
              {"virustotal", std::move(virustotal)}};
}

Result<Json> SecurityComponent::scan() {
  const std::scoped_lock scanning(scan_mutex_);
  const security::Settings options = settings();
  Json problems = Json::array();
  Result<std::vector<security::Detection>> history = detections();
  std::vector<security::Finding> found =
      findings(history ? std::span<const security::Detection>(*history)
                       : std::span<const security::Detection>{},
               problems);
  Json list = Json::array();
  Json counts{{"default", 0}, {"ok", 0}, {"suspicious", 0}, {"known_dangerous", 0}};
  std::vector<std::filesystem::path> to_look_up;
  for (std::size_t index = 0; index < found.size(); ++index) {
    list.push_back(item_json(found[index], index + 1, to_look_up));
    const std::string status(security::to_string(found[index].status));
    counts[status] = counts[status].get<int>() + 1;
  }
  const std::size_t queued = remember(std::move(found), options, std::move(to_look_up));
  return Json{
      {"items", std::move(list)},
      {"counts", std::move(counts)},
      {"defender",
       {{"available", history.has_value()},
        {"error", history ? Json(nullptr) : Json(history.error().message)},
        {"detections", history ? detections_json(*history) : Json::array()}}},
      {"virustotal",
       {{"enabled", options.virustotal},
        {"note", options.virustotal_note.empty() ? Json(nullptr) : Json(options.virustotal_note)},
        {"queued", queued}}},
      {"problems", std::move(problems)}};
}

std::size_t SecurityComponent::remember(std::vector<security::Finding> found,
                                        const security::Settings& options,
                                        std::vector<std::filesystem::path> to_look_up) {
  const std::scoped_lock lock(mutex_);
  latest_ = std::move(found);
  if (!options.virustotal) {
    api_key_.clear();
    lookups_.clear();
    queued_.clear();
    return 0;
  }
  api_key_ = options.api_key;
  for (std::filesystem::path& image : to_look_up) {
    if (queued_.insert(image).second) {
      lookups_.push_back(std::move(image));
    }
  }
  return lookups_.size();
}
Result<Json> SecurityComponent::extensions() const {
  Json list = Json::array();
  Json problems = Json::array();
  if (browsers_) {
    const security::BrowserSnapshot snapshot = browsers_->read();
    std::vector<std::string> found_problems = snapshot.problems;
    for (const security::Extension& extension : security::extensions(snapshot, found_problems)) {
      list.push_back(
          Json{{"browser", security::to_string(extension.browser)},
               {"profile", extension.profile},
               {"id", extension.id},
               {"name", extension.name},
               {"version", extension.version},
               {"installed", time_json(extension.installed)},
               {"origin", security::to_string(extension.origin)},
               {"enabled", extension.enabled},
               {"flag", extension.probably_not_by_owner() ? Json("probably not installed by you")
                                                          : Json(nullptr)}});
    }
    for (const std::string& problem : found_problems) {
      problems.push_back(problem);
    }
  }
  return Json{{"extensions", std::move(list)}, {"problems", std::move(problems)}};
}

std::optional<trust::TrustSignal> SecurityComponent::talker_trust(
    const security::Talker& talker) const {
  if (talker.running && talker.created && sources_.process_trust) {
    if (Result<trust::TrustSignal> evaluated = sources_.process_trust(talker.pid, *talker.created);
        evaluated) {
      return std::move(*evaluated);
    }
  }
  const std::filesystem::path recorded{std::u8string(talker.image.begin(), talker.image.end())};
  if (recorded.is_absolute() && sources_.image_trust) {
    if (Result<trust::TrustSignal> evaluated = sources_.image_trust(recorded); evaluated) {
      return std::move(*evaluated);
    }
  }
  return std::nullopt;
}

Json SecurityComponent::talker_json(const security::Talker& talker, bool reverse_dns,
                                    WallTime now) const {
  const std::optional<trust::TrustSignal> signal = talker_trust(talker);
  Json remotes = Json::array();
  for (const security::Remote& remote : talker.remotes) {
    Json name = nullptr;
    if (reverse_dns) {
      if (std::optional<std::string> resolved = dns_->lookup(remote.address, now)) {
        name = std::move(*resolved);
      }
    }
    remotes.push_back(Json{{"address", remote.address},
                           {"name", std::move(name)},
                           {"port", remote.port},
                           {"protocol", events::to_string(remote.protocol)},
                           {"sent", remote.sent},
                           {"received", remote.received},
                           {"first_seen", time_json(remote.first_seen)},
                           {"last_seen", time_json(remote.last_seen)},
                           {"active_minutes", remote.minutes},
                           {"open_now", remote.open_now}});
  }
  Json listening = Json::array();
  for (const security::Listener& listener : talker.listening) {
    listening.push_back(
        Json{{"protocol", events::to_string(listener.protocol)}, {"port", listener.port}});
  }
  return Json{
      {"pid", talker.pid},
      {"created", time_json(talker.created)},
      {"image", signal ? utf8_text(signal->image) : talker.image},
      {"running", talker.running},
      {"session", talker.session ? Json(*talker.session) : Json(nullptr)},
      {"visible_window", talker.visible_window ? Json(*talker.visible_window) : Json(nullptr)},
      {"trust", signal ? trust_json(*signal) : Json(nullptr)},
      {"sent", talker.sent},
      {"received", talker.received},
      {"first_seen", time_json(talker.first_seen)},
      {"last_seen", time_json(talker.last_seen)},
      {"active_minutes", talker.minutes},
      {"remotes", std::move(remotes)},
      {"listening", std::move(listening)}};
}

Result<Json> SecurityComponent::network(const Json& params) {
  const double hours = params.value("hours", default_hours);
  if (!(hours > 0.0) || hours > ipc::security_max_hours) {
    return fail(Errc::invalid_argument, "hours must be more than 0 and at most 720");
  }
  const WallTime now = clock_->now();
  const WallTime from = now - std::chrono::duration_cast<WallTime::duration>(
                                  std::chrono::duration<double, std::ratio<3600>>{hours});
  Result<std::vector<security::Traffic>> history = security::traffic(*store_, from, now);
  if (!history) {
    return tl::unexpected(history.error());
  }
  Json problems = Json::array();
  std::vector<events::Connection> connections;
  if (sources_.connections) {
    if (Result<std::vector<events::Connection>> table = sources_.connections->snapshot(); table) {
      connections = std::move(*table);
    } else {
      problems.push_back("connections: " + table.error().message);
    }
  }
  const security::WindowFacts facts = windows_->facts(from);
  const bool reverse_dns = settings().reverse_dns && dns_ != nullptr;
  Json processes = Json::array();
  for (const security::Talker& talker : security::talkers(*history, connections, live(), facts)) {
    processes.push_back(talker_json(talker, reverse_dns, now));
  }
  return Json{{"hours", hours},
              {"from", store::to_unix_seconds(from)},
              {"to", store::to_unix_seconds(now)},
              {"reverse_dns", reverse_dns},
              {"names_pending", reverse_dns ? dns_->pending() : 0},
              {"windows_reported", facts.reported},
              {"processes", std::move(processes)},
              {"problems", std::move(problems)}};
}
Result<Json> SecurityComponent::defender_scan(const Json& params) {
  const std::int64_t id = params.at("item").get<std::int64_t>();
  std::filesystem::path image;
  {
    const std::scoped_lock lock(mutex_);
    if (id < 1 || static_cast<std::size_t>(id) > latest_.size()) {
      return fail(Errc::not_found,
                  "no item " + std::to_string(id) + " in the latest security.scan");
    }
    const security::Finding& finding = latest_[static_cast<std::size_t>(id) - 1];
    if (!finding.image) {
      return fail(Errc::invalid_argument, "item " + std::to_string(id) + " has no file on disk");
    }
    image = *finding.image;
  }
  if (!sources_.defender) {
    return fail(Errc::not_found, "Microsoft Defender is not available");
  }
  if (scanning_.exchange(true)) {
    return fail(Errc::invalid_state, "a Defender scan runs already");
  }
  if (defender_thread_.joinable()) {
    defender_thread_.join();  // the previous scan, finished
  }
  // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
  defender_thread_ = std::jthread([this, image](const std::stop_token& stop) {
    const Result<trust::FileStamp> stamp = sources_.stamps
                                               ? sources_.stamps->stamp(image)
                                               : Result<trust::FileStamp>(trust::FileStamp{});
    const Result<security::FileScan> scanned = sources_.defender->scan_file(image, stop);
    if (!scanned) {
      note("Defender scan", scanned.error().message);
    } else if (stamp) {
      note("Defender scan", std::nullopt);
      store_verdict(image, security::defender_source,
                    Stored{.stamp = *stamp,
                           .detected = scanned->detected,
                           .threat = scanned->threat,
                           .time = clock_->now()});
    }
    scanning_ = false;
  });
  return Json{{"started", true}, {"image", utf8_text(image)}};
}

void SecurityComponent::wait_for_defender_scan() {
  if (defender_thread_.joinable()) {
    defender_thread_.join();
  }
}

Result<Json> SecurityComponent::windows(const Json& params) {
  std::vector<std::uint32_t> pids;
  for (const Json& pid : params.at("windows").at("pids")) {
    pids.push_back(pid.get<std::uint32_t>());
  }
  windows_->report(pids, live(), clock_->now());
  return Json::object();
}

}  // namespace myclean::svc
