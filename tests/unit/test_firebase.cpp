// The Firebase connector: Crashlytics and Performance Monitoring BigQuery
// exports read from a local directory into the signal store.
//
// The fixtures under fixtures/intelligence/firebase are synthetic -- invented
// values in the shape of the documented export schemas (see the README there)
// -- so every number below can be worked out by hand from them.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "adapters/firebase/firebase.hpp"
#include "adapters/intelligence/builtin.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
using mpi::json::Value;
using mpi::signals::SignalRecord;

std::string fixture(const std::string& name) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir != nullptr ? dir : "fixtures") + "/intelligence/firebase/" + name;
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A throwaway store and export directory, removed when the test ends.
struct Env {
  std::string root;
  std::string exports;
  mpi::signals::SignalStore store;

  explicit Env(const Value& extra_settings = Value::object()) : root(make_root()), store(root + "/sessions") {
    exports = root + "/exports";
    fs::create_directories(exports);
    mpi::intelligence::register_builtin_connectors();
    mpi::signals::ProjectWorkspace w;
    w.id = "shop";
    w.name = "Shop";
    std::string err;
    MPI_CHECK_MSG(store.save_workspace(w, &err), err);
    mpi::signals::ConnectorConfig c;
    c.id = "firebase-main";
    c.provider = "firebase";
    c.settings = Value::object();
    c.settings.set("export_dir", Value::string(exports));
    for (const auto& [k, v] : extra_settings.members()) c.settings.set(k, v);
    MPI_CHECK_MSG(store.save_connector("shop", c, &err), err);
  }
  ~Env() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  static std::string make_root() {
    std::string tmpl = (fs::temp_directory_path() / "devx-firebase-XXXXXX").string();
    const char* d = ::mkdtemp(tmpl.data());
    return d != nullptr ? std::string(d) : std::string();
  }

  void add(const std::string& fixture_name, const std::string& as = "") {
    const std::string name = as.empty() ? fs::path(fixture_name).filename().string() : as;
    fs::copy_file(fixture(fixture_name), exports + "/" + name, fs::copy_options::overwrite_existing);
  }

  Value sync() {
    return mpi::signals::sync_connector(store, "shop", "firebase-main", mpi::signals::RunOptions{});
  }

  std::vector<SignalRecord> all() { return store.all_signals("shop"); }
};

std::string str(const Value& o, const char* key) {
  const Value* v = o.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

std::int64_t integer(const Value& o, const char* key) {
  const Value* v = o.find(key);
  return v != nullptr && v->is_number() ? v->as_int() : -1;
}

double real(const Value& o, const char* key) {
  const Value* v = o.find(key);
  return v != nullptr && v->is_number() ? v->as_double() : -1.0;
}

bool notes_mention(const Value& result, const std::string& needle) {
  const Value* notes = result.find("notes");
  if (notes == nullptr) return false;
  for (const auto& n : notes->items()) {
    if (n.as_string().find(needle) != std::string::npos) return true;
  }
  return false;
}

const char* kIssueA = "6a1f0c2e9b8d4f7a8c3e5d1b2a4f6e90";
const char* kIssueB = "b70d5c3a1e2f4b6d8a9c0e1f2a3b4c5d";
const char* kIssueC = "c39e7f1a2b3c4d5e6f708192a3b4c5d6";
const char* kIssueD = "d4e5f60718293a4b5c6d7e8f90a1b2c3";

const SignalRecord* find(const std::vector<SignalRecord>& all, const std::string& external_id) {
  for (const auto& r : all) {
    if (r.external_id == external_id) return &r;
  }
  return nullptr;
}

const SignalRecord* find_perf(const std::vector<SignalRecord>& all, const std::string& event_name,
                              const std::string& method = "") {
  for (const auto& r : all) {
    if (r.kind == "metric" && str(r.attributes, "event_name") == event_name &&
        str(r.attributes, "http_method") == method) {
      return &r;
    }
  }
  return nullptr;
}

std::string crash_id(const char* issue, const char* version_build) {
  return std::string("crash:") + issue + ":" + version_build;
}

}  // namespace

MPI_TEST(firebase_contract_is_local_only, {}) {
  auto c = mpi::intelligence::make_firebase_connector();
  MPI_CHECK_EQ(c->info().provider, std::string("firebase"));
  MPI_CHECK_EQ(c->info().display_name, std::string("Firebase (BigQuery export)"));
  MPI_CHECK(c->info().credential_env.empty() && c->info().credential_service.empty());
  MPI_CHECK_EQ(c->info().egress, std::string("Nothing: reads export files on this Mac"));
  const auto caps = c->capabilities();
  MPI_CHECK(!caps.network);
  MPI_CHECK(caps.crashes && caps.issues && caps.metrics && caps.incremental_pull);
}

MPI_TEST(firebase_timestamps_in_every_export_spelling, {}) {
  using mpi::intelligence::firebase::parse_timestamp_us;
  const std::int64_t want = 1791422144LL * 1000000LL;  // 2026-10-08T01:15:44Z
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08 01:15:44.123456 UTC")), want + 123456);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08 01:15:44 UTC")), want);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08T01:15:44Z")), want);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08T01:15:44.5Z")), want + 500000);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08T08:15:44+07:00")), want);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("2026-10-08T08:45:44+0730")), want);
  MPI_CHECK_EQ(*parse_timestamp_us(Value::integer(1791422144)), want);              // seconds
  MPI_CHECK_EQ(*parse_timestamp_us(Value::integer(1791422144000)), want);           // ms
  MPI_CHECK_EQ(*parse_timestamp_us(Value::integer(1791422144000000)), want);        // us
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("1791422144000000")), want);       // INT64 as a string
  MPI_CHECK_EQ(*parse_timestamp_us(Value::string("1.791422144E9")), want);          // tabledata float seconds
  MPI_CHECK_MSG(!parse_timestamp_us(Value::string("2026-10-08 01:15:44 PST")),
                "a zone that is not UTC is refused, never read as UTC");
  MPI_CHECK(!parse_timestamp_us(Value::string("yesterday")));
  MPI_CHECK(!parse_timestamp_us(Value::null()));
}

MPI_TEST(firebase_nearest_rank_is_a_sample_never_an_interpolation, {}) {
  using mpi::intelligence::firebase::nearest_rank;
  std::vector<double> xs;
  for (int i = 1; i <= 100; i++) xs.push_back(i);
  MPI_CHECK(nearest_rank(xs, 50) == 50);
  MPI_CHECK(nearest_rank(xs, 95) == 95);
  MPI_CHECK(nearest_rank(xs, 99) == 99);
  MPI_CHECK(nearest_rank(xs, 100) == 100);
  MPI_CHECK(nearest_rank({10, 20, 30, 40}, 50) == 20);
  MPI_CHECK(nearest_rank({7}, 95) == 7);
}

MPI_TEST(firebase_crashlytics_aggregates_per_issue_version_and_build, {}) {
  Env e;
  e.add("crashlytics-batch.jsonl");
  e.add("crashlytics-query.json");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("complete"));
  MPI_CHECK_EQ(integer(r, "records_written"), static_cast<std::int64_t>(5));
  MPI_CHECK_EQ(integer(r, "raw_written"), static_cast<std::int64_t>(2));
  MPI_CHECK_MSG(notes_mention(r, "counted once"), "a repeated event_id is reported");
  const auto all = e.all();
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(5));

  // Issue A on 2.4.0 (2401): four distinct events across two files (one row
  // repeats an event), on three installations.
  const SignalRecord* a = find(all, crash_id(kIssueA, "2.4.0+2401"));
  MPI_CHECK(a != nullptr);
  if (a == nullptr) return;
  MPI_CHECK_EQ(a->kind, std::string("crash"));
  MPI_CHECK_EQ(*a->severity, std::string("fatal"));
  MPI_CHECK_EQ(a->provider, std::string("firebase"));
  MPI_CHECK_EQ(a->connector_id, std::string("firebase-main"));
  MPI_CHECK_EQ(*a->title, std::string("java.lang.IllegalStateException — CheckoutViewModel.submit"));
  MPI_CHECK_EQ(integer(a->attributes, "events"), static_cast<std::int64_t>(4));
  MPI_CHECK_EQ(integer(a->attributes, "affected_installations"), static_cast<std::int64_t>(3));
  MPI_CHECK_EQ(str(a->attributes, "error_type"), std::string("FATAL"));
  MPI_CHECK_EQ(a->occurred_at, std::string("2026-10-01T08:15:44Z"));
  MPI_CHECK_EQ(str(a->attributes, "first_event_at"), std::string("2026-10-01T08:15:44Z"));
  MPI_CHECK_EQ(str(a->attributes, "last_event_at"), std::string("2026-10-04T07:30:00Z"));
  MPI_CHECK_EQ(*a->release.version, std::string("2.4.0"));
  MPI_CHECK_EQ(*a->release.build_number, std::string("2401"));
  MPI_CHECK_EQ(*a->release.bundle_or_package_id, std::string("com.example.shop"));
  MPI_CHECK(a->release.source == mpi::signals::FactSource::kProvider);
  MPI_CHECK(a->basis == mpi::signals::EvidenceBasis::kProviderAttributed);
  MPI_CHECK_EQ(a->release.key(), std::string("com.example.shop@2.4.0+2401"));
  const Value* bf = a->attributes.find("blame_frame");
  MPI_CHECK(bf != nullptr && integer(*bf, "line") == 88 && str(*bf, "file") == "CheckoutViewModel.kt");
  const Value* frames = a->attributes.find("frames");
  MPI_CHECK(frames != nullptr && frames->size() == 3);
  if (frames != nullptr && frames->size() == 3) MPI_CHECK(frames->items()[0].find("blamed")->as_bool());
  const Value* ex = a->attributes.find("exception");
  MPI_CHECK(ex != nullptr && str(*ex, "message") == "cart is empty" &&
            str(*ex, "type") == "java.lang.IllegalStateException");
  MPI_CHECK_EQ(str(a->attributes, "platform"), std::string("ANDROID"));
  const Value* files = a->attributes.find("source_files");
  MPI_CHECK(files != nullptr && files->size() == 2);
  MPI_CHECK_EQ(a->raw_ref, std::string("raw/firebase/exports/crashlytics-batch.jsonl"));
  MPI_CHECK(e.store.read_raw("shop", a->raw_ref, 0, 16).ok);

  // The same issue on another build is another signal, with its own id.
  const SignalRecord* a2 = find(all, crash_id(kIssueA, "2.4.1+2410"));
  MPI_CHECK(a2 != nullptr && a2->id != a->id);
  if (a2 != nullptr) MPI_CHECK_EQ(integer(a2->attributes, "events"), static_cast<std::int64_t>(1));

  const SignalRecord* b = find(all, crash_id(kIssueB, "2.4.0+2401"));
  MPI_CHECK(b != nullptr);
  if (b != nullptr) {
    MPI_CHECK_EQ(b->kind, std::string("issue"));
    MPI_CHECK_EQ(*b->severity, std::string("error"));
    MPI_CHECK_EQ(str(b->attributes, "error_type"), std::string("NON_FATAL"));
    // "+07:00" is honoured: 12:30 there is 05:30 UTC.
    MPI_CHECK_EQ(str(b->attributes, "first_event_at"), std::string("2026-10-02T05:30:00Z"));
    MPI_CHECK_EQ(str(b->attributes, "last_event_at"), std::string("2026-10-02T11:00:00Z"));
    const Value* os = b->attributes.find("os_versions");
    MPI_CHECK(os != nullptr && os->size() == 2);
  }

  const SignalRecord* c = find(all, crash_id(kIssueC, "2.4.0+2401"));
  MPI_CHECK(c != nullptr);
  if (c != nullptr) {
    MPI_CHECK_EQ(c->kind, std::string("crash"));
    MPI_CHECK_EQ(*c->severity, std::string("fatal"));
    MPI_CHECK_EQ(str(c->attributes, "error_type"), std::string("ANR"));
    MPI_CHECK_EQ(c->occurred_at, std::string("2026-10-02T13:00:00Z"));
  }

  // iOS, with only the deprecated is_fatal and a crashed thread.
  const SignalRecord* d = find(all, crash_id(kIssueD, "1.0.0+77"));
  MPI_CHECK(d != nullptr);
  if (d != nullptr) {
    MPI_CHECK_EQ(d->kind, std::string("crash"));
    MPI_CHECK_EQ(*d->title, std::string("EXC_BAD_ACCESS — KERN_INVALID_ADDRESS"));
    MPI_CHECK_EQ(str(*d->attributes.find("exception"), "type"), std::string("SIGSEGV"));
    MPI_CHECK_EQ(*d->release.bundle_or_package_id, std::string("com.example.other"));
  }

  const auto cursor = e.store.cursor("shop", "firebase-main");
  MPI_CHECK_EQ(cursor.watermark, std::string("2026-10-04T07:30:00Z"));
  const Value* seen = cursor.extra.find("files");
  MPI_CHECK(seen != nullptr && seen->size() == 2);
}

MPI_TEST(firebase_personal_data_never_reaches_a_signal, {}) {
  Env e;
  e.add("crashlytics-batch.jsonl");
  e.add("performance.ndjson");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("complete"));
  int files = 0;
  for (const auto& entry : fs::recursive_directory_iterator(e.store.workspace_dir("shop") + "/signals")) {
    if (!entry.is_regular_file()) continue;
    const std::string body = slurp(entry.path().string());
    files++;
    for (const char* pii : {"jane.fixture@example.com", "Jane Fixture", "user-8812", "other.person",
                            "cart-7731-private", "cart_token", "checkout_open", "opened checkout",
                            "user_email", "Viettel"}) {
      MPI_CHECK_MSG(body.find(pii) == std::string::npos,
                    entry.path().filename().string() + " holds '" + pii + "'");
    }
  }
  MPI_CHECK(files >= 10);
}

MPI_TEST(firebase_performance_percentiles_are_exact, {}) {
  Env e;
  e.add("performance.ndjson");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("complete"));
  const auto all = e.all();
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(5));

  // 1..100 ms, shuffled in the file.
  const SignalRecord* start = find_perf(all, "_app_start");
  MPI_CHECK(start != nullptr);
  if (start == nullptr) return;
  const Value& a = start->attributes;
  MPI_CHECK_EQ(start->kind, std::string("metric"));
  MPI_CHECK(!start->severity.has_value());
  MPI_CHECK_EQ(str(a, "unit"), std::string("ms"));
  MPI_CHECK_EQ(integer(a, "samples"), static_cast<std::int64_t>(100));
  MPI_CHECK_EQ(integer(a, "p50"), static_cast<std::int64_t>(50));
  MPI_CHECK_EQ(integer(a, "p90"), static_cast<std::int64_t>(90));
  MPI_CHECK_EQ(integer(a, "p95"), static_cast<std::int64_t>(95));
  MPI_CHECK_EQ(integer(a, "p99"), static_cast<std::int64_t>(99));
  MPI_CHECK_EQ(integer(a, "min"), static_cast<std::int64_t>(1));
  MPI_CHECK_EQ(integer(a, "max"), static_cast<std::int64_t>(100));
  MPI_CHECK_EQ(*start->title, std::string("_app_start p95 95 ms"));
  MPI_CHECK_EQ(start->occurred_at, std::string("2026-10-05T00:00:01Z"));
  MPI_CHECK_EQ(start->external_id.rfind("perf:", 0), static_cast<std::size_t>(0));
  MPI_CHECK_EQ(*start->release.version, std::string("2.4.0"));
  MPI_CHECK_EQ(*start->release.build_number, std::string("2401"));
  MPI_CHECK_MSG(!start->release.bundle_or_package_id.has_value(),
                "Performance rows name no app; none is invented");
  MPI_CHECK(start->basis == mpi::signals::EvidenceBasis::kProviderAttributed);

  // 100..1000 ms; eight 2xx out of ten answered requests.
  const SignalRecord* get = find_perf(all, "api.example.com/v1/**", "GET");
  MPI_CHECK(get != nullptr);
  if (get != nullptr) {
    MPI_CHECK_EQ(*get->title, std::string("GET api.example.com/v1/** p95 1.0 s"));
    MPI_CHECK_EQ(integer(get->attributes, "p50"), static_cast<std::int64_t>(500));
    MPI_CHECK_NEAR(real(get->attributes, "success_rate"), 0.8, 1e-12);
    const Value* codes = get->attributes.find("response_codes");
    MPI_CHECK(codes != nullptr && integer(*codes, "200") == 8 && integer(*codes, "404") == 1 &&
              integer(*codes, "500") == 1);
    MPI_CHECK_EQ(integer(get->attributes, "response_payload_bytes_p50"), static_cast<std::int64_t>(5000));
  }
  const SignalRecord* post = find_perf(all, "api.example.com/v1/**", "POST");
  MPI_CHECK(post != nullptr && get != nullptr && post->id != get->id);
  if (post != nullptr) MPI_CHECK_NEAR(real(post->attributes, "success_rate"), 1.0, 1e-12);

  const SignalRecord* screen = find_perf(all, "_st_MainActivity");
  MPI_CHECK(screen != nullptr);
  if (screen != nullptr) {
    const Value* slow = screen->attributes.find("slow_frame_ratio");
    const Value* frozen = screen->attributes.find("frozen_frame_ratio");
    MPI_CHECK(slow != nullptr && frozen != nullptr);
    if (slow != nullptr && frozen != nullptr) {
      MPI_CHECK_NEAR(real(*slow, "p50"), 0.2, 1e-12);
      MPI_CHECK_NEAR(real(*slow, "p95"), 0.4, 1e-12);
      MPI_CHECK_NEAR(real(*frozen, "p50"), 0.0, 1e-12);
      MPI_CHECK_NEAR(real(*frozen, "p95"), 0.1, 1e-12);
    }
  }

  const SignalRecord* metric = find_perf(all, "items_loaded");
  MPI_CHECK(metric != nullptr);
  if (metric != nullptr) {
    MPI_CHECK_EQ(str(metric->attributes, "unit"), std::string("as reported"));
    MPI_CHECK_EQ(str(metric->attributes, "parent_trace_name"), std::string("catalog_load"));
    MPI_CHECK_EQ(integer(metric->attributes, "max"), static_cast<std::int64_t>(30));
  }
}

MPI_TEST(firebase_resync_keeps_ids_and_never_double_counts, {}) {
  Env e;
  e.add("crashlytics-batch.jsonl");
  e.add("crashlytics-query.json");
  e.add("performance.ndjson");
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  const auto first = e.all();

  const Value again = e.sync();
  MPI_CHECK_EQ(str(again, "status"), std::string("complete"));
  MPI_CHECK(notes_mention(again, "0 new, 0 changed, 3 unchanged"));
  const auto second = e.all();
  MPI_CHECK_EQ(second.size(), first.size());
  for (const auto& r : first) {
    const SignalRecord* s = find(second, r.external_id);
    MPI_CHECK(s != nullptr);
    if (s == nullptr) continue;
    MPI_CHECK_EQ(s->id, r.id);
    MPI_CHECK_EQ(integer(s->attributes, "events"), integer(r.attributes, "events"));
  }

  // The same export saved a second time adds files, not events.
  e.add("crashlytics-query.json", "crashlytics-query-copy.json");
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  const auto copied = e.all();
  const SignalRecord* a = find(copied, crash_id(kIssueA, "2.4.0+2401"));
  MPI_CHECK(a != nullptr);
  if (a != nullptr) MPI_CHECK_EQ(integer(a->attributes, "events"), static_cast<std::int64_t>(4));

  // A genuinely new event is counted, on the same signal.
  {
    std::ofstream late(e.exports + "/late.jsonl");
    late << R"({"issue_id":")" << kIssueA
         << R"(","event_id":"a1000000000000000000000000000006","error_type":"FATAL",)"
         << R"("bundle_identifier":"com.example.shop","installation_uuid":"i-0001",)"
         << R"("application":{"display_version":"2.4.0","build_version":"2401"},)"
         << R"("event_timestamp":"2026-10-07 10:00:00 UTC"})" << "\n";
  }
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  const auto after = e.all();
  MPI_CHECK_EQ(after.size(), first.size());
  const SignalRecord* a5 = find(after, crash_id(kIssueA, "2.4.0+2401"));
  MPI_CHECK(a5 != nullptr);
  if (a5 != nullptr) {
    MPI_CHECK_EQ(a5->id, a->id);
    MPI_CHECK_EQ(integer(a5->attributes, "events"), static_cast<std::int64_t>(5));
    MPI_CHECK_EQ(integer(a5->attributes, "affected_installations"), static_cast<std::int64_t>(3));
    MPI_CHECK_EQ(str(a5->attributes, "last_event_at"), std::string("2026-10-07T10:00:00Z"));
  }
  MPI_CHECK_EQ(e.store.cursor("shop", "firebase-main").watermark, std::string("2026-10-07T10:00:00Z"));
}

MPI_TEST(firebase_a_broken_file_is_partial_and_the_rest_is_imported, {}) {
  Env e;
  e.add("crashlytics-batch.jsonl");
  e.add("malformed/truncated-array.json");
  e.add("malformed/some-bad-lines.jsonl");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("partial"));
  MPI_CHECK(notes_mention(r, "truncated-array.json was not read"));
  MPI_CHECK(notes_mention(r, "some-bad-lines.jsonl: 2 row(s)"));
  const auto all = e.all();
  const SignalRecord* a = find(all, crash_id(kIssueA, "2.4.0+2401"));
  MPI_CHECK(a != nullptr);
  // The truncated array's one complete event is not counted: half a file
  // would make a count look smaller than it is.
  if (a != nullptr) MPI_CHECK_EQ(integer(a->attributes, "events"), static_cast<std::int64_t>(3));
  // The readable line of a file with bad lines is.
  const SignalRecord* b = find(all, crash_id(kIssueB, "2.4.0+2401"));
  MPI_CHECK(b != nullptr);
  if (b != nullptr) MPI_CHECK_EQ(integer(b->attributes, "events"), static_cast<std::int64_t>(3));
  MPI_CHECK_MSG(e.store.cursor("shop", "firebase-main").watermark.empty(),
                "a partial sync does not advance the watermark");
}

MPI_TEST(firebase_app_identifier_and_dataset_kind_filter_rows, {}) {
  Value settings = Value::object();
  settings.set("app_identifier", Value::string("com.example.shop"));
  Env e(settings);
  e.add("crashlytics-batch.jsonl");
  e.add("performance.ndjson");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("complete"));
  MPI_CHECK(notes_mention(r, "not app com.example.shop"));
  MPI_CHECK(notes_mention(r, "name no app"));
  const auto all = e.all();
  MPI_CHECK(find(all, crash_id(kIssueD, "1.0.0+77")) == nullptr);
  MPI_CHECK(find(all, crash_id(kIssueA, "2.4.0+2401")) != nullptr);
  MPI_CHECK(find_perf(all, "_app_start") != nullptr);

  Value only_crashes = Value::object();
  only_crashes.set("dataset_kind", Value::string("crashlytics"));
  Env c(only_crashes);
  c.add("crashlytics-batch.jsonl");
  c.add("performance.ndjson");
  const Value rc = c.sync();
  MPI_CHECK_EQ(str(rc, "status"), std::string("complete"));
  MPI_CHECK(find_perf(c.all(), "_app_start") == nullptr);
  MPI_CHECK_EQ(c.all().size(), static_cast<std::size_t>(5));
}

MPI_TEST(firebase_validate_discover_and_an_empty_directory, {}) {
  Env e;
  const auto opts = mpi::signals::RunOptions{};
  const Value empty = e.sync();
  MPI_CHECK_EQ(str(empty, "status"), std::string("complete"));
  MPI_CHECK(notes_mention(empty, "no *.json"));
  MPI_CHECK_EQ(integer(empty, "records_written"), static_cast<std::int64_t>(0));

  e.add("crashlytics-batch.jsonl");
  e.add("performance.ndjson");
  const Value v = mpi::signals::validate_connector(e.store, "shop", "firebase-main", opts);
  MPI_CHECK(v.find("ok")->as_bool());
  MPI_CHECK(notes_mention(v, "2 export file(s)"));
  const Value d = mpi::signals::discover_connector(e.store, "shop", "firebase-main", opts);
  MPI_CHECK(d.find("ok")->as_bool());
  const Value* res = d.find("resources");
  MPI_CHECK(res != nullptr && res->size() == 2);
  if (res != nullptr && res->size() == 2) {
    MPI_CHECK_EQ(str(res->items()[0], "kind"), std::string("crashlytics"));
    MPI_CHECK_EQ(str(res->items()[1], "kind"), std::string("performance"));
  }

  Value missing = Value::object();
  missing.set("export_dir", Value::string("/nonexistent/devx-firebase-exports"));
  Env m(missing);
  const Value mv = mpi::signals::validate_connector(m.store, "shop", "firebase-main", opts);
  MPI_CHECK(!mv.find("ok")->as_bool());
  const Value ms = m.sync();
  MPI_CHECK_EQ(str(ms, "status"), std::string("failed"));
}
