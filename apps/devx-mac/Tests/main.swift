import Foundation

// Swift-side tests for the JSON bridge layer.
//
// Run as a plain executable rather than through XCTest so `ctest` can run it
// with no test-bundle host, matching how the C++ tests run.

var failures: [String] = []
var passed = 0

func check(_ condition: Bool, _ label: String, _ detail: String = "") {
    if condition {
        passed += 1
        print("  ok   \(label)")
    } else {
        failures.append(label)
        print("  FAIL \(label)\(detail.isEmpty ? "" : "\n       \(detail)")")
    }
}

func parse(_ text: String) -> JSON { JSON.parse(text) }

// --- the bug this file exists for -------------------------------------------
// JSONSerialization returns NSNumber for numbers *and* booleans, and
// `NSNumber(0) as? Bool` succeeds as false. Casting to Bool first turned every
// 0 into `false`, so a UI showing a count rendered "false counters".
do {
    let d = parse(#"{"zero":0,"one":1,"two":2,"yes":true,"no":false,"neg":-3,"real":1.5}"#)
    check(d["zero"].double == 0, "zero decodes as a number",
          "got \(d["zero"])")
    check(d["zero"].bool == nil, "zero is not a boolean")
    check(d["zero"].display("?") == "0", "zero displays as 0",
          "got \(d["zero"].display("?"))")
    check(d["one"].double == 1, "one decodes as a number")
    check(d["one"].bool == nil, "one is not a boolean")
    check(d["one"].display("?") == "1", "one displays as 1")
    check(d["yes"].bool == true, "true decodes as a boolean")
    check(d["no"].bool == false, "false decodes as a boolean")
    check(d["yes"].double == nil, "a boolean is not a number")
    check(d["neg"].int == -3, "negative integers decode")
    check(d["real"].double == 1.5, "fractional numbers decode")
}

// --- absent versus present-null ---------------------------------------------
// The whole unknown-versus-false model depends on telling these apart.
do {
    let d = parse(#"{"present":null,"value":7}"#)
    check(d["present"].isNull, "a present null is null")
    check(!d.isAbsent("present"), "a present null is not absent")
    check(d.isAbsent("missing"), "an absent key reports absent")
    check(d["missing"].isNull, "an absent key reads as null")
    check(d["value"].int == 7, "a sibling value still decodes")
}

// --- display never invents a value ------------------------------------------
do {
    let d = parse(#"{"n":null,"empty":"","text":"hello"}"#)
    check(d["n"].display("unknown") == "unknown",
          "null displays as the fallback, not as empty")
    check(d["empty"].display("unknown") == "unknown",
          "an empty string displays as the fallback rather than as blank")
    check(d["text"].display() == "hello", "a real string displays itself")
    check(d["missing"].display("not observed") == "not observed",
          "an absent key displays as the fallback")
}

// --- nesting and arrays ------------------------------------------------------
do {
    let d = parse(#"{"a":{"b":{"c":[10,20,{"d":"deep"}]}}}"#)
    check(d["a"]["b"]["c"][0].int == 10, "array indexing works")
    check(d["a"]["b"]["c"][2]["d"].text == "deep", "mixed nesting works")
    check(d["a"]["b"]["c"].array.count == 3, "array count is right")
    check(d["a"]["nope"]["deeper"].isNull,
          "walking through a missing key yields null rather than trapping")
    check(d["a"]["b"]["c"][99].isNull, "an out-of-range index yields null")
    check(d["a"]["b"]["c"][-1].isNull, "a negative index yields null")
}

// --- malformed input ---------------------------------------------------------
do {
    check(parse("").isNull, "empty input yields null")
    check(parse("{not json").isNull, "malformed input yields null")
    check(parse("[1,2").isNull, "truncated input yields null")
    // A bare scalar is valid JSON with fragmentsAllowed, which the bridge uses
    // because the core can legitimately return one.
    check(parse("42").double == 42, "a bare number parses")
    check(parse("\"str\"").text == "str", "a bare string parses")
}

// --- formatting helpers ------------------------------------------------------
do {
    check(formatNs(500) == "500 ns", "sub-microsecond formats as ns",
          "got \(formatNs(500))")
    check(formatNs(1_500).hasSuffix("µs"), "microseconds format as µs")
    check(formatNs(16_666_666).hasSuffix("ms"), "milliseconds format as ms")
    check(formatNs(2_000_000_000).hasSuffix("s"), "seconds format as s")
    check(formatBytes(512) == "512 B", "bytes format as B")
    check(formatBytes(1024 * 1024).hasSuffix("MB"), "megabytes format as MB")
}

// --- status tones must keep the three states distinct ------------------------
// `unknown` must never look like success and never like failure: the engine
// treats it as a third answer, so the UI has to as well.
do {
    check(StatusTone.capability("available").color == StatusTone.good.color,
          "available reads as good")
    check(StatusTone.capability("unknown").color == StatusTone.neutral.color,
          "unknown reads as neither good nor bad")
    check(StatusTone.capability("unsupported").color == StatusTone.bad.color,
          "unsupported reads as bad")
    check(StatusTone.runtime("unknown").color == StatusTone.caution.color,
          "an unknown runtime state is flagged, not treated as not-running")
    check(StatusTone.detection("observed").color == StatusTone.good.color,
          "observed reads as measured")
    check(StatusTone.detection("suspected").color == StatusTone.caution.color,
          "suspected is visually distinct from observed")
    check(StatusTone.outcome("skipped").color == StatusTone.neutral.color,
          "a skipped detector is not shown as a pass")
    check(StatusTone.outcome("ran_found_nothing").color == StatusTone.good.color,
          "ran-and-found-nothing is distinct from skipped")
}

// --- launch options ----------------------------------------------------------
do {
    let o = LaunchOptions.parse(["--session", "s-123", "--tab", "issues"])
    check(o.session == "s-123", "--session parses")
    check(o.tab == DevXTab.issues, "--tab parses to a known tab")
    let bad = LaunchOptions.parse(["--tab", "nonsense"])
    check(bad.tab == nil, "an unknown tab name is ignored rather than guessed")
    let none = LaunchOptions.parse([])
    check(none.session == nil && none.tab == nil && none.sessionsDir == nil,
          "no arguments yields no options")
}

print("")
print("\(passed) passed, \(failures.count) failed")
exit(failures.isEmpty ? 0 : 1)
