#include <sstream>

#include "core/util/json.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

MPI_TEST(json_roundtrip_preserves_key_order, {"H12"}) {
  json::Value o = json::Value::object();
  o.set("z", json::Value::integer(1));
  o.set("a", json::Value::integer(2));
  o.set("m", json::Value::string("x"));
  // Insertion order is preserved so a report diff is stable across runs.
  MPI_CHECK_EQ(o.dump(), std::string("{\"z\":1,\"a\":2,\"m\":\"x\"}"));
}

MPI_TEST(json_null_is_distinct_from_absent, {"C18", "H11"}) {
  json::ParseError err;
  auto v = json::parse("{\"present_null\":null}", &err);
  MPI_CHECK(v.has_value());
  // A present-but-null field and an absent field must be distinguishable:
  // the whole unknown-vs-false model depends on it.
  MPI_CHECK(v->find("present_null") != nullptr);
  MPI_CHECK(v->find("present_null")->is_null());
  MPI_CHECK(v->find("absent") == nullptr);
}

MPI_TEST(json_rejects_depth_bomb, {"J03"}) {
  std::string deep;
  for (int i = 0; i < 500; ++i) deep += "[";
  for (int i = 0; i < 500; ++i) deep += "]";
  json::Limits limits;
  limits.max_depth = 64;
  json::ParseError err;
  auto v = json::parse(deep, limits, &err);
  MPI_CHECK_MSG(!v.has_value(), "a 500-deep array must be refused");
  MPI_CHECK(err.message.find("depth") != std::string::npos);
}

MPI_TEST(json_rejects_oversized_input, {"J03"}) {
  json::Limits limits;
  limits.max_bytes = 8;
  json::ParseError err;
  auto v = json::parse("[1,2,3,4,5,6,7,8,9,10]", limits, &err);
  MPI_CHECK(!v.has_value());
  MPI_CHECK(err.message.find("max_bytes") != std::string::npos);
}

MPI_TEST(json_rejects_container_element_bomb, {"J03"}) {
  std::string big = "[";
  for (int i = 0; i < 200; ++i) big += (i ? ",1" : "1");
  big += "]";
  json::Limits limits;
  limits.max_container_elements = 50;
  json::ParseError err;
  MPI_CHECK(!json::parse(big, limits, &err).has_value());
}

MPI_TEST(json_rejects_malformed, {"J02", "D08"}) {
  json::ParseError err;
  const char* bad[] = {"{",          "{\"a\":}",   "[1,2",     "tru",
                       "{\"a\" 1}",  "\"unterminated", "[1,2]junk",
                       "{\"a\":1,}", "01.2.3"};
  for (const char* b : bad) {
    MPI_CHECK_MSG(!json::parse(b, &err).has_value(),
                  std::string("should have been rejected: ") + b);
  }
}

MPI_TEST(json_control_characters_must_be_escaped, {"J02"}) {
  json::ParseError err;
  // A raw newline inside a string is invalid JSON and is refused.
  MPI_CHECK(!json::parse("\"a\nb\"", &err).has_value());
}

MPI_TEST(json_escapes_on_output, {"J01"}) {
  auto v = json::Value::string("line\nbreak\"quote\\slash\ttab");
  const std::string dumped = v.dump();
  MPI_CHECK(dumped.find("\\n") != std::string::npos);
  MPI_CHECK(dumped.find("\\\"") != std::string::npos);
  MPI_CHECK(dumped.find("\\\\") != std::string::npos);
  MPI_CHECK(dumped.find("\\t") != std::string::npos);
  // Round-trips back to the original.
  json::ParseError err;
  auto back = json::parse(dumped, &err);
  MPI_CHECK(back.has_value());
  MPI_CHECK_EQ(back->as_string(), v.as_string());
}

MPI_TEST(json_unicode_surrogate_pairs, {"J02"}) {
  json::ParseError err;
  auto v = json::parse("\"\\ud83d\\ude00\"", &err);  // emoji
  MPI_CHECK(v.has_value());
  MPI_CHECK_EQ(v->as_string().size(), static_cast<std::size_t>(4));
  // A lone high surrogate does not crash and does not silently vanish.
  auto lone = json::parse("\"\\ud83d\"", &err);
  MPI_CHECK(lone.has_value());
  MPI_CHECK(!lone->as_string().empty());
}

MPI_TEST(json_non_finite_double_stays_valid_json, {"H12"}) {
  auto v = json::Value::number(std::nan(""));
  // NaN has no JSON representation; emitting null keeps the document parseable
  // instead of writing a value no consumer can read.
  MPI_CHECK_EQ(v.dump(), std::string("null"));
  MPI_CHECK_EQ(json::Value::number(HUGE_VAL).dump(), std::string("null"));
}

MPI_TEST(json_large_integer_does_not_wrap, {"H12"}) {
  json::ParseError err;
  // Beyond int64: must not silently wrap to a negative number.
  auto v = json::parse("99999999999999999999999", &err);
  MPI_CHECK(v.has_value());
  MPI_CHECK(v->is_double());
  MPI_CHECK(v->as_double() > 1e22);
}

MPI_TEST(json_bom_is_tolerated, {"D18"}) {
  json::ParseError err;
  auto v = json::parse("\xEF\xBB\xBF{\"a\":1}", &err);
  MPI_CHECK(v.has_value());
  MPI_CHECK_EQ(v->find("a")->as_int(), static_cast<std::int64_t>(1));
}

MPI_TEST(json_accessors_do_not_throw_on_type_mismatch, {"J11"}) {
  auto v = json::Value::string("not a number");
  // No exception may cross an ABI boundary, so accessors fall back instead.
  MPI_CHECK_EQ(v.as_int(-1), static_cast<std::int64_t>(-1));
  MPI_CHECK_EQ(v.as_bool(true), true);
  MPI_CHECK_EQ(json::Value::integer(5).as_string(), std::string(""));
}

// --- StreamParser -----------------------------------------------------------

MPI_TEST(stream_parser_walks_members_in_order, {"section-15"}) {
  json::StreamParser sp("{\"a\":1,\"b\":\"two\",\"c\":[1,2,3]}", json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  json::Value v;

  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("a"));
  MPI_CHECK(sp.read_value(v));
  MPI_CHECK_EQ(v.as_int(), static_cast<std::int64_t>(1));

  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("b"));
  MPI_CHECK(sp.read_value(v));
  MPI_CHECK_EQ(v.as_string(), std::string("two"));

  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("c"));
  MPI_CHECK(sp.array_begin());
  int sum = 0;
  while (sp.next_array_element(v)) sum += static_cast<int>(v.as_int());
  MPI_CHECK_EQ(sum, 6);

  MPI_CHECK_MSG(!sp.next_member(key), "the object is exhausted");
  MPI_CHECK(!sp.failed());
}

MPI_TEST(stream_parser_skips_without_materialising, {"section-15", "D18"}) {
  // A nested structure the caller does not want must be skipped correctly,
  // leaving the cursor on the following member.
  json::StreamParser sp(
      "{\"skip\":{\"deep\":[1,{\"x\":[[[]]]},\"str\"]},\"want\":42}",
      json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("skip"));
  MPI_CHECK(sp.skip_value());
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("want"));
  json::Value v;
  MPI_CHECK(sp.read_value(v));
  MPI_CHECK_EQ(v.as_int(), static_cast<std::int64_t>(42));
}

MPI_TEST(stream_parser_handles_empty_arrays_and_objects, {"D16"}) {
  json::StreamParser sp("{\"e\":[],\"o\":{},\"n\":null}", json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  json::Value v;
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK(sp.array_begin());
  MPI_CHECK_MSG(!sp.next_array_element(v), "an empty array yields no elements");
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK(sp.read_value(v));
  MPI_CHECK(v.is_object());
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK(sp.read_value(v));
  MPI_CHECK(v.is_null());
  MPI_CHECK(!sp.failed());
}

MPI_TEST(stream_parser_can_abandon_an_array_midway, {"D04"}) {
  json::StreamParser sp("{\"a\":[1,2,3,4,5],\"b\":7}", json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  json::Value v;
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK(sp.array_begin());
  MPI_CHECK(sp.next_array_element(v));
  MPI_CHECK(sp.next_array_element(v));
  // A cancelled ingest stops mid-array; the parser must still be able to
  // finish the document rather than being left wedged.
  MPI_CHECK(sp.array_skip_rest());
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("b"));
}

MPI_TEST(stream_parser_reports_malformed_input, {"D08", "J02"}) {
  const char* bad[] = {
      "[1,2]",                  // not an object at top level
      "{\"a\" 1}",              // missing colon
      "{\"a\":1 \"b\":2}",      // missing comma
      "{\"a\":[1 2]}",          // missing comma in array
      "{\"a\":",                // truncated
      "{\"a\":[1,",             // truncated array
  };
  for (const char* b : bad) {
    json::StreamParser sp(b, json::Limits{});
    std::string key;
    json::Value v;
    bool ok = sp.object_begin();
    while (ok && sp.next_member(key)) {
      if (!sp.read_value(v)) break;
    }
    MPI_CHECK_MSG(sp.failed(), std::string("should have failed: ") + b);
    MPI_CHECK_MSG(!sp.error().message.empty(),
                  std::string("a failure must carry a reason: ") + b);
  }
}

MPI_TEST(stream_parser_enforces_the_byte_limit, {"J03"}) {
  json::Limits limits;
  limits.max_bytes = 4;
  json::StreamParser sp("{\"a\":1,\"b\":2}", limits);
  MPI_CHECK_MSG(!sp.object_begin(), "an oversized input must be refused");
  MPI_CHECK(sp.failed());
  MPI_CHECK(sp.error().message.find("max_bytes") != std::string::npos);
}

MPI_TEST(stream_parser_enforces_depth_inside_a_skip, {"J03"}) {
  std::string deep = "{\"a\":";
  for (int i = 0; i < 300; ++i) deep += "[";
  for (int i = 0; i < 300; ++i) deep += "]";
  deep += "}";
  json::Limits limits;
  limits.max_depth = 32;
  json::StreamParser sp(deep, limits);
  MPI_CHECK(sp.object_begin());
  std::string key;
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_MSG(!sp.skip_value(),
                "skipping must respect the depth limit, not recurse freely");
  MPI_CHECK(sp.failed());
}

MPI_TEST(stream_parser_tolerates_a_bom, {"D18"}) {
  json::StreamParser sp("\xEF\xBB\xBF{\"a\":1}", json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  MPI_CHECK(sp.next_member(key));
  MPI_CHECK_EQ(key, std::string("a"));
}

MPI_TEST(stream_parser_agrees_with_the_dom_parser, {"H12"}) {
  // The two parsers must read the same document identically; a divergence
  // would mean a streamed trace and a materialised one disagree.
  const std::string doc =
      "{\"s\":\"x\\ny\",\"i\":-42,\"d\":1.5e2,\"t\":true,\"n\":null,"
      "\"arr\":[1,\"two\",{\"k\":3}],\"obj\":{\"nested\":[true,false]}}";
  json::ParseError err;
  auto dom = json::parse(doc, &err);
  MPI_CHECK(dom.has_value());

  json::StreamParser sp(doc, json::Limits{});
  MPI_CHECK(sp.object_begin());
  std::string key;
  json::Value v;
  std::size_t members = 0;
  while (sp.next_member(key)) {
    MPI_CHECK(sp.read_value(v));
    ++members;
    const json::Value* expected = dom->find(key);
    MPI_CHECK_MSG(expected != nullptr, "streamed key absent from the DOM: " + key);
    MPI_CHECK_MSG(v.dump() == expected->dump(),
                  "value mismatch for '" + key + "': streamed " + v.dump() +
                      " vs DOM " + expected->dump());
  }
  MPI_CHECK(!sp.failed());
  MPI_CHECK_EQ(members, dom->members().size());
}
