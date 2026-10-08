// The JSONL import connector: the proof that a new provider needs no change in
// core or in any consumer. Fixtures under fixtures/intelligence/jsonl are
// synthetic (see the README there).
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "adapters/intelligence/builtin.hpp"
#include "adapters/jsonl/jsonl.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
using mpi::json::Value;
using mpi::signals::EvidenceBasis;
using mpi::signals::SignalRecord;

std::string fixture(const std::string& name) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir != nullptr ? dir : "fixtures") + "/intelligence/jsonl/" + name;
}

struct Env {
  std::string root;
  std::string inputs;
  mpi::signals::SignalStore store;

  // `path` empty: the inputs directory this Env copies fixtures into.
  Env(const std::string& path, const std::string& label = "")
      : root(make_root()), store(root + "/sessions") {
    inputs = root + "/inputs";
    fs::create_directories(inputs);
    mpi::intelligence::register_builtin_connectors();
    mpi::signals::ProjectWorkspace w;
    w.id = "shop";
    std::string err;
    MPI_CHECK_MSG(store.save_workspace(w, &err), err);
    mpi::signals::ConnectorConfig c;
    c.id = "jsonl-import";
    c.provider = "jsonl";
    c.settings = Value::object();
    c.settings.set("path", Value::string(path.empty() ? inputs : path));
    if (!label.empty()) c.settings.set("provider_label", Value::string(label));
    MPI_CHECK_MSG(store.save_connector("shop", c, &err), err);
  }
  ~Env() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  static std::string make_root() {
    std::string tmpl = (fs::temp_directory_path() / "devx-jsonl-XXXXXX").string();
    const char* d = ::mkdtemp(tmpl.data());
    return d != nullptr ? std::string(d) : std::string();
  }
  void add(const std::string& name) {
    fs::copy_file(fixture(name), inputs + "/" + name, fs::copy_options::overwrite_existing);
  }
  Value sync() {
    return mpi::signals::sync_connector(store, "shop", "jsonl-import", mpi::signals::RunOptions{});
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

bool notes_mention(const Value& result, const std::string& needle) {
  const Value* notes = result.find("notes");
  if (notes == nullptr) return false;
  for (const auto& n : notes->items()) {
    if (n.as_string().find(needle) != std::string::npos) return true;
  }
  return false;
}

const SignalRecord* find(const std::vector<SignalRecord>& all, const std::string& external_id) {
  for (const auto& r : all) {
    if (r.external_id == external_id) return &r;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(jsonl_contract_is_local_only, {}) {
  auto c = mpi::intelligence::make_jsonl_connector();
  MPI_CHECK_EQ(c->info().provider, std::string("jsonl"));
  MPI_CHECK_EQ(c->info().display_name, std::string("JSONL import"));
  MPI_CHECK_EQ(c->info().category, std::string("import"));
  MPI_CHECK(c->info().credential_env.empty() && c->info().credential_service.empty());
  MPI_CHECK(!c->capabilities().network);
  MPI_CHECK(c->capabilities().issues && c->capabilities().metrics);
}

MPI_TEST(jsonl_imports_full_and_loose_records, {}) {
  Env e(fixture("signals.jsonl"));
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("complete"));
  MPI_CHECK_EQ(integer(r, "records_written"), static_cast<std::int64_t>(7));
  MPI_CHECK_EQ(integer(r, "raw_written"), static_cast<std::int64_t>(1));
  const auto all = e.all();
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(7));

  // A full record from elsewhere: owned by this connector now, its origin
  // kept as an attribute, its own raw_ref replaced by the imported file.
  const SignalRecord* full = find(all, "bugsnag:5f1");
  MPI_CHECK(full != nullptr);
  if (full != nullptr) {
    MPI_CHECK_EQ(full->provider, std::string("jsonl"));
    MPI_CHECK_EQ(full->connector_id, std::string("jsonl-import"));
    MPI_CHECK_EQ(full->workspace_id, std::string("shop"));
    MPI_CHECK_EQ(full->id, mpi::signals::make_signal_id("jsonl-import", "bugsnag:5f1"));
    MPI_CHECK_EQ(str(full->attributes, "source_provider"), std::string("bugsnag-export"));
    MPI_CHECK_EQ(integer(full->attributes, "events"), static_cast<std::int64_t>(12));
    MPI_CHECK_EQ(full->raw_ref, std::string("raw/jsonl/imports/signals.jsonl"));
    MPI_CHECK_MSG(full->basis == EvidenceBasis::kExact, "a full SHA supports exact");
    MPI_CHECK_EQ(*full->release.commit_sha, std::string("0123456789abcdef0123456789abcdef01234567"));
    MPI_CHECK(full->release.source == mpi::signals::FactSource::kBuildManifest);
    MPI_CHECK_EQ(*full->severity, std::string("fatal"));
  }

  const SignalRecord* ticket = find(all, "ticket-101");
  MPI_CHECK(ticket != nullptr);
  if (ticket != nullptr) {
    MPI_CHECK_EQ(ticket->kind, std::string("issue"));
    MPI_CHECK_EQ(*ticket->environment, std::string("production"));
    MPI_CHECK_MSG(ticket->basis == EvidenceBasis::kExact, "version + build + bundle supports exact");
    MPI_CHECK(ticket->release.source == mpi::signals::FactSource::kUserAsserted);
    MPI_CHECK_EQ(ticket->release.key(), std::string("com.example.shop@3.1.0+310"));
    MPI_CHECK_EQ(integer(ticket->attributes, "source_line"), static_cast<std::int64_t>(2));
  }

  // An integer id, no release: nothing to link, so the basis is unknown.
  const SignalRecord* metric = find(all, "42");
  MPI_CHECK(metric != nullptr);
  if (metric != nullptr) {
    MPI_CHECK_EQ(metric->kind, std::string("metric"));
    MPI_CHECK(metric->basis == EvidenceBasis::kUnknown);
    MPI_CHECK(metric->release.empty());
  }

  // Ids that make_signal_id would cut to the same 64 characters stay apart.
  const SignalRecord* l1 = find(all, "release-train/2026-10/an-identifier-long-enough-to-be-cut-by-make-signal-id-number-1");
  const SignalRecord* l2 = find(all, "release-train/2026-10/an-identifier-long-enough-to-be-cut-by-make-signal-id-number-2");
  MPI_CHECK(l1 != nullptr && l2 != nullptr);
  if (l1 != nullptr && l2 != nullptr) {
    MPI_CHECK(l1->id != l2->id);
    MPI_CHECK(l1->id.size() <= 64 && mpi::signals::id_is_safe(l1->id));
  }
  MPI_CHECK_EQ(e.store.cursor("shop", "jsonl-import").watermark, std::string("2026-10-06T00:00:01Z"));
}

MPI_TEST(jsonl_certainty_cannot_be_imported, {}) {
  Env e(fixture("signals.jsonl"));
  const Value r = e.sync();
  MPI_CHECK(notes_mention(r, "2 line(s) claimed basis exact"));
  const auto all = e.all();
  for (const char* id : {"deploy-77", "rel-3.1.0"}) {
    const SignalRecord* s = find(all, id);
    MPI_CHECK(s != nullptr);
    if (s == nullptr) continue;
    MPI_CHECK_MSG(s->basis == EvidenceBasis::kProviderAttributed, std::string(id) + " was downgraded");
    MPI_CHECK_EQ(str(s->attributes, "basis_downgraded_from"), std::string("exact"));
  }
}

MPI_TEST(jsonl_provider_label_names_the_source, {}) {
  Env e(fixture("signals.jsonl"), "release-tracker");
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  for (const auto& s : e.all()) {
    MPI_CHECK_EQ(str(s.attributes, "source_provider"), std::string("release-tracker"));
    MPI_CHECK_EQ(s.provider, std::string("jsonl"));
  }
}

MPI_TEST(jsonl_bad_lines_make_a_partial_import_and_good_lines_still_land, {}) {
  Env e("");
  e.add("signals.jsonl");
  e.add("bad-lines.jsonl");
  const Value r = e.sync();
  MPI_CHECK_EQ(str(r, "status"), std::string("partial"));
  MPI_CHECK(notes_mention(r, "bad-lines.jsonl: 4 line(s) not imported (first at line 2"));
  MPI_CHECK_EQ(integer(r, "records_written"), static_cast<std::int64_t>(8));
  const auto all = e.all();
  MPI_CHECK(find(all, "ticket-202") != nullptr);
  MPI_CHECK(find(all, "ticket-203") == nullptr);
  MPI_CHECK(find(all, "x-1") == nullptr);
  MPI_CHECK_MSG(e.store.cursor("shop", "jsonl-import").watermark.empty(),
                "a partial import does not advance the watermark");
}

MPI_TEST(jsonl_reimport_keeps_ids_stable, {}) {
  Env e(fixture("signals.jsonl"));
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  const auto first = e.all();
  MPI_CHECK_EQ(str(e.sync(), "status"), std::string("complete"));
  const auto second = e.all();
  MPI_CHECK_EQ(second.size(), first.size());
  for (const auto& r : first) {
    const SignalRecord* s = find(second, r.external_id);
    MPI_CHECK(s != nullptr && s->id == r.id && s->raw_ref == r.raw_ref);
  }
}

MPI_TEST(jsonl_validate_and_discover, {}) {
  Env e("");
  e.add("signals.jsonl");
  e.add("bad-lines.jsonl");
  const auto opts = mpi::signals::RunOptions{};
  const Value v = mpi::signals::validate_connector(e.store, "shop", "jsonl-import", opts);
  MPI_CHECK(v.find("ok")->as_bool());
  MPI_CHECK(notes_mention(v, "2 file(s)"));
  const Value d = mpi::signals::discover_connector(e.store, "shop", "jsonl-import", opts);
  const Value* res = d.find("resources");
  MPI_CHECK(res != nullptr && res->size() == 2);

  Env missing("/nonexistent/devx-import.jsonl");
  const Value mv = mpi::signals::validate_connector(missing.store, "shop", "jsonl-import", opts);
  MPI_CHECK(!mv.find("ok")->as_bool());
  MPI_CHECK_EQ(str(missing.sync(), "status"), std::string("failed"));
}
