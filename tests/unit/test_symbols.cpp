#include <cstdlib>
#include <sstream>

#include "core/symbols/symbol_service.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

std::string fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir ? dir : "fixtures") + "/" + rel;
}

}  // namespace

MPI_TEST(source_map_v3_loads_and_decodes_vlq, {"G14"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  MPI_CHECK_MSG(map.has_value(), "load failed: " + err);
  MPI_CHECK_EQ(map->bundle_id, std::string("bundle-hash-abc123"));
  MPI_CHECK_EQ(map->sources.size(), static_cast<std::size_t>(2));
  MPI_CHECK_MSG(!map->mappings.empty(), "no mappings were decoded");
  // Generated line 1199, column 14 must resolve into CartScreen.
  const auto* m = map->lookup(1199, 14);
  MPI_CHECK(m != nullptr);
  MPI_CHECK_EQ(m->source_index, 0);
  MPI_CHECK_EQ(m->original_line, 41);
}

MPI_TEST(source_map_rejects_wrong_version, {"D18"}) {
  std::string err;
  MPI_CHECK(!symbols::load_source_map(fixture("symbols/wrong-version.map.json"),
                                      json::Limits{}, &err).has_value());
  MPI_CHECK(err.find("version") != std::string::npos);
}

MPI_TEST(source_map_rejects_malformed_vlq, {"J02", "G18"}) {
  std::string err;
  MPI_CHECK_MSG(!symbols::load_source_map(fixture("symbols/malformed.map.json"),
                                          json::Limits{}, &err).has_value(),
                "a corrupt mappings string must be refused, not approximated");
  MPI_CHECK(!err.empty());
}

MPI_TEST(matching_bundle_id_yields_exact_build_match, {"C15", "G14"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  MPI_CHECK(map.has_value());
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_source_map(std::move(*map));
  MPI_CHECK_EQ(svc.bindings().size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(svc.bindings()[0].status, std::string("exact_build_match"));
  MPI_CHECK_EQ(svc.aggregate_status(), std::string("exact_build_match"));
}

MPI_TEST(mismatched_bundle_id_is_rejected_not_used_silently, {"C15", "G18"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/mismatched.map.json"),
                                      json::Limits{}, &err);
  MPI_CHECK(map.has_value());
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_source_map(std::move(*map));
  MPI_CHECK_EQ(svc.bindings()[0].status, std::string("mismatch"));
  MPI_CHECK_EQ(svc.aggregate_status(), std::string("mismatch"));
  // Resolution still happens, but the result is never safe to open.
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK_MSG(!loc.safe_to_open(),
                "a mismatched map must never navigate the editor");
  MPI_CHECK(loc.note.find("bundle-hash-DIFFERENT") != std::string::npos);
}

MPI_TEST(absent_expected_bundle_id_downgrades_to_partial, {"C15", "C18"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  symbols::SymbolService svc;  // no expected bundle id set
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_source_map(std::move(*map));
  MPI_CHECK_EQ(svc.bindings()[0].status, std::string("partial"));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK(!loc.safe_to_open());
}

MPI_TEST(exact_match_resolves_to_a_real_local_file, {"G14", "G16"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_source_map(std::move(*map));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK_EQ(loc.file, std::string("src/screens/CartScreen.tsx"));
  MPI_CHECK(loc.line.has_value());
  MPI_CHECK_EQ(*loc.line, 42);
  MPI_CHECK_MSG(loc.safe_to_open(),
                "an exact match with a present file should be navigable; note: " +
                    loc.note);
}

MPI_TEST(dirty_checkout_blocks_exact_source_claims, {"C17"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.set_source_revision("abc123", /*dirty=*/true);
  svc.add_source_map(std::move(*map));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK_MSG(!loc.safe_to_open(), "a dirty checkout must block navigation");
  MPI_CHECK(loc.note.find("dirty") != std::string::npos);
  // The symbol name still resolves: a mismatch blocks source claims, not all
  // inspection.
  MPI_CHECK_EQ(loc.symbol, std::string("renderList"));
}

MPI_TEST(missing_local_file_downgrades_to_partial, {"G16"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/nonexistent-checkout"));
  svc.add_source_map(std::move(*map));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK(!loc.safe_to_open());
  MPI_CHECK(loc.note.find("not present locally") != std::string::npos);
}

MPI_TEST(path_traversal_in_a_source_map_is_rejected, {"J04", "G19"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/traversal.map.json"),
                                      json::Limits{}, &err);
  MPI_CHECK(map.has_value());
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_source_map(std::move(*map));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK_MSG(loc.symbol_status == "mismatch",
                "a path escaping the source root must be refused, got status " +
                    loc.symbol_status);
  MPI_CHECK(loc.note.find("outside the configured source root") != std::string::npos);
  MPI_CHECK(!loc.safe_to_open());
}

MPI_TEST(path_root_containment_check, {"J04"}) {
  MPI_CHECK(symbols::path_is_within_root("/a/b", "c/d"));
  MPI_CHECK(symbols::path_is_within_root("/a/b", "/a/b/c"));
  MPI_CHECK(symbols::path_is_within_root("/a/b", "./c"));
  MPI_CHECK(symbols::path_is_within_root("/a/b", "c/../d"));
  // Escapes.
  MPI_CHECK(!symbols::path_is_within_root("/a/b", "../x"));
  MPI_CHECK(!symbols::path_is_within_root("/a/b", "../../../../etc/passwd"));
  MPI_CHECK(!symbols::path_is_within_root("/a/b", "/etc/passwd"));
  MPI_CHECK(!symbols::path_is_within_root("/a/b", "c/../../../etc/passwd"));
  // A sibling directory that merely shares a prefix is not inside.
  MPI_CHECK(!symbols::path_is_within_root("/a/b", "/a/bc/d"));
  // Degenerate inputs.
  MPI_CHECK(!symbols::path_is_within_root("", "c"));
  std::string with_nul("a");
  with_nul.push_back('\0');
  MPI_CHECK(!symbols::path_is_within_root("/a/b", with_nul));
}

MPI_TEST(monorepo_source_root_remap_applies, {"G17"}) {
  std::string err;
  auto map = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                      json::Limits{}, &err);
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_local_source_root(fixture("symbols/checkout"));
  svc.add_remap(symbols::SourceRootRemap{"src/", "src/"});
  svc.add_source_map(std::move(*map));
  const auto loc = svc.resolve_frame("renderList (bundle.js:1200:14)");
  MPI_CHECK_EQ(loc.file, std::string("src/screens/CartScreen.tsx"));
}

MPI_TEST(r8_map_deobfuscates_and_binds_to_a_build, {"C14", "G18"}) {
  std::string err;
  auto map = symbols::load_obfuscation_map(fixture("symbols/r8-mapping.txt"), &err);
  MPI_CHECK_MSG(map.has_value(), "load failed: " + err);
  MPI_CHECK_EQ(map->build_id, std::string("7f3a91c"));
  // Class-level mapping.
  const auto cls = map->deobfuscate("a.b.c");
  MPI_CHECK(cls.has_value());
  MPI_CHECK_EQ(*cls, std::string("com.example.perf.render.TextShaper"));
  // Member-level mapping.
  const auto member = map->deobfuscate("a.b.c.d");
  MPI_CHECK(member.has_value());
  MPI_CHECK(member->find("com.example.perf.render.TextShaper.shape") !=
            std::string::npos);
  // An unknown frame stays unknown.
  MPI_CHECK(!map->deobfuscate("z.z.z.unknown").has_value());
}

MPI_TEST(r8_map_from_another_build_is_reported_as_mismatch, {"C14"}) {
  std::string err;
  auto map = symbols::load_obfuscation_map(
      fixture("symbols/r8-mapping-other-build.txt"), &err);
  MPI_CHECK(map.has_value());
  symbols::SymbolService svc;
  svc.set_expected_native_build_id("7f3a91c");
  svc.add_obfuscation_map(std::move(*map));
  MPI_CHECK_EQ(svc.bindings()[0].status, std::string("mismatch"));
  MPI_CHECK_EQ(svc.aggregate_status(), std::string("mismatch"));
}

MPI_TEST(missing_symbol_artifacts_yield_unavailable_with_a_reason, {"B13"}) {
  symbols::SymbolService svc;
  MPI_CHECK_EQ(svc.aggregate_status(), std::string("unavailable"));
  const auto loc = svc.resolve_frame("com.example.perf.Whatever.method");
  MPI_CHECK_EQ(loc.symbol_status, std::string("unavailable"));
  MPI_CHECK(loc.note.find("no symbol artifacts") != std::string::npos);
  MPI_CHECK(!loc.safe_to_open());
  // The raw frame is preserved, so the user still sees what was sampled.
  MPI_CHECK_EQ(loc.symbol, std::string("com.example.perf.Whatever.method"));
}

MPI_TEST(missing_file_is_an_error_not_a_silent_empty_map, {"C14", "C15"}) {
  std::string err;
  MPI_CHECK(!symbols::load_source_map(fixture("symbols/nope.map.json"),
                                      json::Limits{}, &err).has_value());
  MPI_CHECK(!err.empty());
  std::string err2;
  MPI_CHECK(!symbols::load_obfuscation_map(fixture("symbols/nope.txt"), &err2)
                 .has_value());
  MPI_CHECK(!err2.empty());
}

MPI_TEST(a_mismatch_anywhere_dominates_the_aggregate_status, {"G18"}) {
  std::string err;
  auto good = symbols::load_source_map(fixture("symbols/matching.map.json"),
                                       json::Limits{}, &err);
  auto bad = symbols::load_obfuscation_map(
      fixture("symbols/r8-mapping-other-build.txt"), &err);
  symbols::SymbolService svc;
  svc.set_expected_bundle_id("bundle-hash-abc123");
  svc.set_expected_native_build_id("7f3a91c");
  svc.add_source_map(std::move(*good));
  svc.add_obfuscation_map(std::move(*bad));
  // One artifact matches exactly, the other does not. The mismatch is the
  // fact that would make a source claim wrong, so it wins.
  MPI_CHECK_EQ(svc.aggregate_status(), std::string("mismatch"));
}
