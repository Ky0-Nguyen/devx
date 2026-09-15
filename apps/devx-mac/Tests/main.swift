import Foundation

// Swift-side tests for the JSON bridge layer.
//
// Run as a plain executable rather than through XCTest so `ctest` can run it
// with no test-bundle host, matching how the C++ tests run.

var failures: [String] = []
var passed = 0

/// Whether we were asked to list what these checks cover rather than run them.
///
/// The C++ harness has the same mode, and `tools/gen-requirement-map.py` reads
/// both: a UI behaviour that is tested here belongs in the requirement map, and
/// leaving the Swift side out of the map made those items read as untested.
let listingRequirements = CommandLine.arguments.contains("--list-requirements")

/// One check, optionally declaring the specification checklist ids it covers.
///
/// `req` is empty for the many checks that pin an internal invariant rather
/// than a checklist item -- tagging everything would make the map claim
/// coverage it does not have.
func check(_ condition: Bool, _ label: String, _ detail: String = "",
           req: [String] = []) {
    if listingRequirements {
        for r in req { print("\(r)\t\(label)") }
        return
    }
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

    // Both spellings must give the same result. The joined form is what the
    // docs recommend, because a space-separated value can survive as a bare
    // argument that AppKit takes for a file to open.
    let spaced = LaunchOptions.parse(
        ["--device", "emulator-5554", "--app", "com.example.app",
         "--start-live", "--live-seconds", "30"])
    let joined = LaunchOptions.parse(
        ["--device=emulator-5554", "--app=com.example.app",
         "--start-live", "--live-seconds=30"])
    for (label, o) in [("spaced", spaced), ("joined", joined)] {
        check(o.device == "emulator-5554", "\(label) --device parses")
        check(o.app == "com.example.app", "\(label) --app parses")
        check(o.startLive, "\(label) --start-live parses")
        check(o.liveSeconds == 30, "\(label) --live-seconds parses")
    }

    // A joined value must not also swallow the argument after it.
    let noSwallow = LaunchOptions.parse(["--tab=live", "--session", "s-9"])
    check(noSwallow.tab == DevXTab.live && noSwallow.session == "s-9",
          "a joined value does not consume the following argument")

    let face = LaunchOptions.parse(["--font", "Monaco"])
    check(face.font == "Monaco", "--font parses spaced")
    check(LaunchOptions.parse(["--font=PT Mono"]).font == "PT Mono",
          "--font parses joined, including a family name with a space")

    // Junk stays ignored rather than being guessed at.
    let junk = LaunchOptions.parse(["--live-seconds=0", "--live-seconds=abc"])
    check(junk.liveSeconds == nil,
          "a non-positive or non-numeric --live-seconds is ignored")
}

print("")

// --- the timeline's pure layer ----------------------------------------------
// A chart has to produce a number for every pixel, and absence has none. These
// pin the two places that pressure shows up: turning a bin into a height, and
// naming a state.
do {
    // The rule: a bin with no number produces no height, whatever is in
    // `value`. Reading `value` first and defaulting to 0 would draw a capture
    // full of confident zeroes.
    check(barFraction(state: .unmeasured, value: nil, scale: 10) == 0,
          "an unmeasured bin has no height")
    check(barFraction(state: .noReading, value: nil, scale: 10) == 0,
          "an unsampled bin has no height")
    // Even if a value were present, these two states must not draw it: the
    // state is the authority, not the presence of a number.
    check(barFraction(state: .unmeasured, value: 9, scale: 10) == 0,
          "an unmeasured bin stays flat even with a value attached",
          req: ["H01"])
    check(barFraction(state: .noReading, value: 9, scale: 10) == 0,
          "an unsampled bin stays flat even with a value attached")

    check(barFraction(state: .measured, value: 0, scale: 10) == 0,
          "a measured zero is zero height -- the view gives it a 1pt floor")
    check(barFraction(state: .measured, value: 5, scale: 10) == 0.5,
          "a measured value scales against the peak")
    check(barFraction(state: .partial, value: 10, scale: 10) == 1,
          "a partial bin still draws the number it has")
    // A value above the scale cannot overflow the lane.
    check(barFraction(state: .measured, value: 99, scale: 10) == 1,
          "a bar is clamped to the lane")
    // No peak means nothing to scale against, and no invented height.
    check(barFraction(state: .measured, value: 5, scale: 0) == 0,
          "with no measured peak there is no scale, so no bar")
    check(barFraction(state: .measured, value: nil, scale: 10) == 0,
          "a measured bin with no value still draws nothing")
}

do {
    // The state names come off the wire, so an unknown one must fail safe:
    // "unmeasured" claims nothing, any other default would claim something.
    check(BinState("measured") == .measured, "measured maps")
    check(BinState("no_reading") == .noReading, "no_reading maps")
    check(BinState("partial") == .partial, "partial maps")
    check(BinState("unmeasured") == .unmeasured, "unmeasured maps")
    check(BinState("") == .unmeasured, "an empty state claims nothing")
    check(BinState("whatever_comes_next") == .unmeasured,
          "an unrecognised state claims nothing rather than guessing",
          req: ["H01", "H11"])
    check(BinState.allCases.count == 4, "four states, all in the legend")
    // Every state has to be nameable in the legend, or the reader meets a
    // pattern with no explanation.
    for s in BinState.allCases {
        check(!s.label.isEmpty && !s.detail.isEmpty,
              "state \(s.rawValue) is described")
    }
    check(BinState.unmeasured.label == "NOT MEASURED",
          "the state that claims nothing says so loudest")
}

do {
    // Pickets: one mark per bin at any bin width. Diagonal hatching failed
    // here -- drawn per bin it overlapped into a solid slab.
    let narrow = Picket(pitch: 5, inset: 1)
        .path(in: CGRect(x: 0, y: 0, width: 6, height: 44))
    check(!narrow.isEmpty, "a bin narrower than the pitch still gets a mark")
    let wide = Picket(pitch: 5, inset: 1)
        .path(in: CGRect(x: 0, y: 0, width: 40, height: 44))
    check(wide.boundingRect.height <= 44,
          "pickets stay inside the lane's height")
    check(wide.boundingRect.width <= 40,
          "pickets stay inside the bin's width: a diagonal hatch did not, "
          + "which is how a lane became a slab")
}

do {
    // Durations pick a unit; a negative one keeps its sign, because a gap
    // that starts before the window is a real thing a capture can contain.
    check(Fmt.duration(500) == "500 ns", "sub-microsecond stays in ns")
    check(Fmt.duration(1_500) == "1.5 us", "got \(Fmt.duration(1_500))")
    check(Fmt.duration(2_500_000) == "2.5 ms", "got \(Fmt.duration(2_500_000))")
    check(Fmt.duration(12_770_000_000) == "12.77 s",
          "got \(Fmt.duration(12_770_000_000))")
    check(Fmt.duration(-5_000_000) == "-5.0 ms",
          "a negative duration keeps its sign: got \(Fmt.duration(-5_000_000))")
    check(Fmt.offset(900, 1_000).contains("before the window"),
          "a band before the window says so rather than printing '+-'")
    check(Fmt.offset(1_000_000_900, 900).hasPrefix("+"),
          "an offset inside the window is signed positive")
    check(Fmt.value(922_353_664, unit: "bytes") == "879.6 MiB",
          "got \(Fmt.value(922_353_664, unit: "bytes"))")
    check(Fmt.value(6, unit: "count") == "6 count",
          "got \(Fmt.value(6, unit: "count"))")
    check(Fmt.value(40_000_000, unit: "ns") == "40.0 ms",
          "got \(Fmt.value(40_000_000, unit: "ns"))")
}

do {
    // `stamp` is for fields the encoder always writes. It must not be what a
    // bin's value goes through: a missing window bound is an encoder defect,
    // a missing bin value is the data saying "not measured".
    let d = parse(#"{"window_start_ns":12,"bins":[{"value":null}]}"#)
    check(d["window_start_ns"].stamp == 12, "a written stamp reads back")
    check(d["nope"].stamp == 0, "a missing stamp falls back to 0")
    check(d["bins"][0]["value"].double == nil,
          "a null bin value stays nil and never becomes 0")
}

do {
    // The launch option that deep-links to one finding.
    let o = LaunchOptions.parse(["--session", "s-1", "--tab", "timeline",
                                 "--issue", "DET-04-abc"])
    check(o.session == "s-1", "session parses")
    check(o.tab == .timeline, "an explicit tab parses")
    check(o.issue == "DET-04-abc", "an issue id parses")
    let joined = LaunchOptions.parse(["--session=s-2", "--issue=DET-01-xyz",
                                      "--tab=timeline"])
    check(joined.session == "s-2" && joined.issue == "DET-01-xyz"
          && joined.tab == .timeline, "the joined form parses too")
    check(DevXTab(rawValue: "timeline") == .timeline, "the tab is addressable")
}


// --- the compare view's wording ---------------------------------------------
// A comparison UI is asked for a single word, and two of the four words mean
// "we do not know". Which word gets which treatment is the honesty of the
// view, so it is tested rather than left to a switch nobody reads.
do {
    check(CompareWording.tone("regression") == .bad, "a regression is bad")
    check(CompareWording.tone("improvement") == .good, "an improvement is good")
    check(CompareWording.tone("no_significant_change") == .neutral,
          "no significant change is neutral")
    // The one that matters: inconclusive must never be toned as a pass.
    check(CompareWording.tone("inconclusive") == .caution,
          "inconclusive is a caution, never a pass", req: ["I02", "H11"])
    check(CompareWording.tone("") == .caution,
          "an unknown verdict is a caution, never a pass")
    check(CompareWording.tone("something_new") == .caution,
          "an unrecognised verdict is a caution, never a pass")

    // And it must say, in words, that it is not "no change".
    let inc = CompareWording.plainly("inconclusive")
    check(inc.contains("not 'no change'"),
          "inconclusive says it is not 'no change': got \(inc)",
          req: ["I02", "H11"])
    let same = CompareWording.plainly("no_significant_change")
    check(same.contains("not proof"),
          "no-significant-change does not claim equivalence: got \(same)")
    for v in ["regression", "improvement", "no_significant_change",
              "inconclusive", "", "anything"] {
        check(!CompareWording.plainly(v).isEmpty,
              "every verdict has an explanation, including '\(v)'")
    }

    // Every condition the comparability check can reject on has to be on
    // screen, or a reader cannot see why a pair was refused.
    for key in ["platform", "scenario_id", "scenario_version", "device_form",
                "os_version", "refresh_policy", "collector_preset",
                "launch_class", "thermal_state", "power_state",
                "input_data_version", "account_state", "network_condition",
                "cache_state"] {
        check(CompareWording.conditionKeys.contains(key),
              "condition '\(key)' is shown")
    }
}

do {
    // The readability probe. A path this app was handed -- typed or passed on
    // the command line -- can sit in a folder macOS gates, and the block
    // happens inside the read, so the probe has a deadline and a third answer.
    let tmp = NSTemporaryDirectory() + "devx-probe-\(getpid()).json"
    try? "{}".write(toFile: tmp, atomically: true, encoding: .utf8)
    check(Core.probeReadable(tmp) == true, "a readable file probes true")
    check(Core.probeReadable(tmp + ".nope") == false,
          "a missing file probes false, not nil: it is not a permission wall")
    check(Core.probeReadable("") == false, "an empty path probes false")
    try? FileManager.default.removeItem(atPath: tmp)
    // A directory is not a readable file, and must not pass as one.
    check(Core.probeReadable(NSTemporaryDirectory()) != true,
          "a directory does not probe as a readable file")
}

do {
    let o = LaunchOptions.parse(["--baseline=/tmp/b.json",
                                 "--candidate=/tmp/c.json"])
    check(o.baseline == "/tmp/b.json" && o.candidate == "/tmp/c.json",
          "a comparison can be opened from the command line")
    check(DevXTab(rawValue: "compare") == .compare, "the compare tab is addressable")
    // Every tab needs a title and an icon or the sidebar shows a blank row.
    for t in DevXTab.allCases {
        check(!t.title.isEmpty && !t.icon.isEmpty, "tab \(t.rawValue) is labelled")
    }
}


// --- the issue filter -------------------------------------------------------
// A filter is the easiest way for a UI to report "nothing is wrong" when the
// truth is "you asked to see a subset".
do {
    let issues = [
        parse(#"{"issue_id":"a","category":"frames","severity":"high","screen":"Checkout","thread_instance_id":"t1","process_instance_id":"p1"}"#),
        parse(#"{"issue_id":"b","category":"memory","severity":"medium","thread_instance_id":"t2","process_instance_id":"p1"}"#),
        parse(#"{"issue_id":"c","category":"frames","severity":"low","screen":"Feed","process_instance_id":"p2"}"#),
        parse(#"{"issue_id":"d","category":"memory","severity":"high","suppression":{"suppressed":true,"reason":"known"}}"#),
    ]

    // No filter set: everything except the suppressed one, and the suppressed
    // one is counted as hidden rather than vanishing.
    var f = IssueFilter()
    check(!f.isActive, "a blank filter is not active")
    var r = f.apply(issues)
    check(r.shown.count == 3, "got \(r.shown.count)")
    check(r.hidden == 1, "the suppressed issue is counted as hidden",
          req: ["H14", "A25"])

    f.includeSuppressed = true
    r = f.apply(issues)
    check(r.shown.count == 4, "suppressed issues appear when asked for")
    check(r.hidden == 0, "and nothing is hidden then")

    f = IssueFilter(); f.category = "frames"
    check(f.isActive, "a set field makes the filter active")
    r = f.apply(issues)
    check(r.shown.count == 2, "category filters: got \(r.shown.count)")
    check(r.hidden == 2, "the hidden count includes the suppressed one")

    f = IssueFilter(); f.severity = "high"
    r = f.apply(issues)
    check(r.shown.count == 1, "severity filters, and the suppressed high one "
          + "stays hidden: got \(r.shown.count)")

    // The rule that matters: an issue with no screen is not on every screen.
    f = IssueFilter(); f.screen = "Checkout"
    r = f.apply(issues)
    check(r.shown.count == 1, "got \(r.shown.count)")
    check(r.shown.first?["issue_id"].text == "a",
          "an issue with no screen does not match a screen filter",
          req: ["A25", "H01"])

    // Dimensions compose, and a combination matching nothing yields an empty
    // list with every issue counted as hidden -- which is what lets the view
    // say why it is empty.
    f = IssueFilter(); f.category = "frames"; f.severity = "medium"
    r = f.apply(issues)
    check(r.shown.isEmpty, "an unmatched combination shows nothing")
    check(r.hidden == issues.count, "and reports every issue as hidden")

    f = IssueFilter(); f.process = "p1"; f.thread = "t2"
    r = f.apply(issues)
    check(r.shown.count == 1 && r.shown.first?["issue_id"].text == "b",
          "process and thread compose")

    // An empty field never means "match nothing".
    f = IssueFilter(); f.category = ""; f.thread = ""
    check(f.apply(issues).shown.count == 3,
          "empty fields are not filters")
}

do {
    let issues = [
        parse(#"{"category":"frames","screen":"Feed"}"#),
        parse(#"{"category":"memory","screen":"Feed"}"#),
        parse(#"{"category":"frames"}"#),
    ]
    let cats = IssueFilter.options("category", in: issues)
    check(cats == ["frames", "memory"], "options are distinct and sorted: got \(cats)")
    let screens = IssueFilter.options("screen", in: issues)
    check(screens == ["Feed"], "a blank value is not an option: got \(screens)")
    check(IssueFilter.options("thread_instance_id", in: issues).isEmpty,
          "a field nothing records offers no options, so a filter cannot be "
          + "set to something that matches nothing", req: ["A25"])
}


// --- recents and favourites -------------------------------------------------
// Spec A23 is an honesty rule, not a feature: a recent or favourite entry is
// not proof the app is running. The tests are mostly about what an entry must
// NOT be able to say.
do {
    var r = RecentTargets()
    let t0 = Date(timeIntervalSince1970: 1_000)
    r.record(deviceId: "d1", appIdentifier: "io.a", name: "A", at: t0)
    r.record(deviceId: "d1", appIdentifier: "io.b", name: "B",
             at: t0.addingTimeInterval(10))
    check(r.entries.count == 2, "two targets remembered")
    check(r.ordered.first?.appIdentifier == "io.b", "newest first")

    // Re-profiling moves an entry rather than duplicating it.
    r.record(deviceId: "d1", appIdentifier: "io.a", name: "A",
             at: t0.addingTimeInterval(20))
    check(r.entries.count == 2, "re-profiling does not duplicate")
    check(r.ordered.first?.appIdentifier == "io.a", "and moves it to the front")

    // The same identifier on another device is a different target: an app id
    // is not unique across devices.
    r.record(deviceId: "d2", appIdentifier: "io.a", name: "A",
             at: t0.addingTimeInterval(30))
    check(r.entries.count == 3, "the same app on another device is separate")
    check(r.forDevice("d1").count == 2, "and filtering by device separates them")

    // Favourites survive re-recording and are never evicted by the limit.
    r.setFavourite("d1\u{1f}io.a", true)
    check(r.entries.first(where: { $0.id == "d1\u{1f}io.a" })?.favourite == true,
          "a favourite is marked")
    r.record(deviceId: "d1", appIdentifier: "io.a", name: "A renamed",
             at: t0.addingTimeInterval(40))
    check(r.entries.first(where: { $0.id == "d1\u{1f}io.a" })?.favourite == true,
          "re-profiling keeps the favourite mark")
    check(r.entries.first(where: { $0.id == "d1\u{1f}io.a" })?.lastKnownName
            == "A renamed",
          "and updates the remembered name")

    for i in 0..<40 {
        r.record(deviceId: "d1", appIdentifier: "io.filler\(i)",
                 name: "F", at: t0.addingTimeInterval(Double(100 + i)))
    }
    let favourites = r.entries.filter { $0.favourite }
    check(favourites.count == 1,
          "the favourite survived 40 later targets: someone marked it on "
          + "purpose and dropping it would look like it was never marked",
          req: ["A23"])
    check(r.entries.count <= RecentTargets.recentLimit + favourites.count,
          "non-favourites are capped: got \(r.entries.count)")
    check(r.ordered.first?.favourite == true, "favourites sort first")

    r.forget("d1\u{1f}io.a")
    check(!r.entries.contains { $0.id == "d1\u{1f}io.a" }, "forgetting removes it")
}

do {
    // The rule itself: an entry says nothing about whether the app is running.
    let target = RecentTarget(deviceId: "d1", appIdentifier: "io.a",
                              lastKnownName: "A",
                              lastUsedAt: Date(), favourite: true)
    // In the listing: the app's own row carries the state, not this one.
    let present = presence(of: target, identifiers: ["io.a"], didEnumerate: true)
    check(present == .inListing, "an enumerated app is in the listing")
    check(present.detail.contains("runtime state discovery reported"),
          "and the state comes from discovery, not from being remembered")

    // Absent from the listing is NOT "not running".
    let absent = presence(of: target, identifiers: ["io.other"],
                          didEnumerate: true)
    check(absent == .notInListing, "an app not enumerated is not in the listing",
          req: ["A23"])
    check(absent.label.contains("not in the current listing"),
          "labelled as absent from the listing: got \(absent.label)")
    check(!absent.label.contains("not running"),
          "never labelled 'not running': a listing that could not see an app "
          + "and an app that is gone are different facts", req: ["A23", "H01"])
    check(absent.detail.contains("not the same as not running"),
          "and the detail says so outright")

    // No enumeration at all is a third state.
    let unknown = presence(of: target, identifiers: [], didEnumerate: false)
    check(unknown == .noListing, "no enumeration means nothing can be said",
          req: ["A23"])
    check(unknown.detail.contains("says nothing about the device now"),
          "and it says that")

    // An empty listing that DID enumerate is a real answer, distinct from
    // never having asked: a device can genuinely show no apps.
    let emptyButAsked = presence(of: target, identifiers: [], didEnumerate: true)
    check(emptyButAsked == .notInListing,
          "an empty enumeration that ran is an answer, not a missing one")

    for p in [RecentPresence.inListing, .notInListing, .noListing] {
        check(!p.label.isEmpty && !p.detail.isEmpty,
              "every presence state is explained")
    }
}

do {
    // Persistence round-trips, and a missing or corrupt file is an empty list
    // rather than a failure: a machine that has profiled nothing is the
    // normal first case.
    let path = NSTemporaryDirectory() + "devx-recents-\(getpid()).json"
    var r = RecentTargets()
    r.record(deviceId: "d1", appIdentifier: "io.a", name: "A")
    r.setFavourite("d1\u{1f}io.a", true)
    r.save(to: path)
    let back = RecentTargets.load(from: path)
    check(back == r, "the list round-trips through a file")
    check(back.entries.first?.favourite == true, "including the favourite mark")

    try? "not json".write(toFile: path, atomically: true, encoding: .utf8)
    check(RecentTargets.load(from: path).entries.isEmpty,
          "a corrupt file reads as an empty list, not a crash")
    try? FileManager.default.removeItem(atPath: path)
    check(RecentTargets.load(from: path).entries.isEmpty,
          "a missing file reads as an empty list")

    // An entry with no device or no identifier is not recorded: a remembered
    // target that cannot be selected again is noise.
    var empty = RecentTargets()
    empty.record(deviceId: "", appIdentifier: "io.a", name: "A")
    empty.record(deviceId: "d1", appIdentifier: "", name: "A")
    check(empty.entries.isEmpty, "an incomplete target is not remembered")
}

do {
    // Every tab the spec section 13 view list names has a home. The list is
    // 11 views; DevX has 11 tabs because Recording splits into Record and
    // Live, and Issue detail plus Stack/source live inside Issues rather
    // than being their own tabs -- which the map below states rather than
    // leaving to be inferred.
    let tabs = Set(DevXTab.allCases.map { $0.rawValue })
    for expected in ["devices", "apps", "preflight", "record", "live",
                     "sessions", "issues", "timeline", "compare",
                     "detectors", "settings"] {
        check(tabs.contains(expected), "the \(expected) view exists",
              req: expected == "settings" ? ["H15"] : [])
    }
    check(DevXTab.settings.title == "Export",
          "the export/settings view is labelled for what it does")
    let o = LaunchOptions.parse(["--tab=settings"])
    check(o.tab == .settings, "and is addressable from the command line")
}


// --- the JS / native thread split -------------------------------------------
// "The main thread" means a different thread depending on which one you mean,
// and in a React Native app a finding can land on at least three. The role is
// the claim this view makes, so how it is established is what gets tested.
do {
    // The UI thread is the only role with a platform signal behind it.
    var r = classifyThread(name: "io.pizzahut.hutbot.debug", isMainUi: true, isJs: false)
    check(r.role == .uiMain && r.basis == .platformSignal,
          "the UI main thread comes from a platform signal", req: ["E20"])

    // `is_js_thread` is itself derived from the name in the Android
    // collector, so a thread carrying it is reported as name-derived. Calling
    // this a platform signal would overstate what was measured.
    r = classifyThread(name: "mqt_v_js", isMainUi: false, isJs: true)
    check(r.role == .js, "mqt_v_js is the JS thread")
    check(r.basis == .threadName,
          "and its role is reported as name-derived, not observed", req: ["E20"])

    // Both React Native JS thread names, and the Hermes case.
    for name in ["mqt_js", "mqt_v_js", "com.facebook.hermes.worker", "JavaScriptCore"] {
        let c = classifyThread(name: name, isMainUi: false, isJs: false)
        check(c.role == .js, "\(name) classifies as JS")
        check(c.basis == .threadName, "\(name) is name-derived")
    }
    for name in ["mqt_native_modules", "mqt_v_native_modules"] {
        check(classifyThread(name: name, isMainUi: false, isJs: false).role
                == .nativeModules,
              "\(name) is a native-module thread")
    }
    check(classifyThread(name: "RenderThread", isMainUi: false, isJs: false).role
            == .renderer,
          "the platform's render thread is not the app's code")

    // A thread nothing identifies lands in `other` with no basis -- never
    // guessed into a role it might not have.
    r = classifyThread(name: "binder:21854_6", isMainUi: false, isJs: false)
    check(r.role == .other && r.basis == RoleBasis.none,
          "an unrecognised thread is 'other', not guessed", req: ["E20", "H01"])
    r = classifyThread(name: "", isMainUi: false, isJs: false)
    check(r.role == .other && r.basis == RoleBasis.none,
          "an unnamed thread is 'other' too")
    check(RoleBasis.threadName.caution && RoleBasis.none.caution,
          "both weak bases are flagged as cautions in the view")
    check(!RoleBasis.platformSignal.caution, "a platform signal is not")
}

do {
    // Shares, from the per-thread counts the report carries. The numbers are
    // the real ones measured on emulator-5554.
    let session = parse(#"""
    {"trace":{"threads":[
      {"thread_instance_id":"t1","name":"io.pizzahut.hutbot.debug","tid":21854,
       "is_main_ui_thread":true,"is_js_thread":false,"sample_count":96},
      {"thread_instance_id":"t2","name":"mqt_v_js","tid":22105,
       "is_main_ui_thread":false,"is_js_thread":true,"sample_count":48},
      {"thread_instance_id":"t3","name":"binder:21854_6","tid":23971,
       "is_main_ui_thread":false,"is_js_thread":false,"sample_count":136},
      {"thread_instance_id":"t4","name":"RenderThread","tid":21893,
       "is_main_ui_thread":false,"is_js_thread":false,"sample_count":1}]}}
    """#)
    let split = threadSplit(from: session)
    check(split.totalSamples == 281, "the total is the sum: got \(split.totalSamples ?? -1)")
    check(split.threads.first?.name == "binder:21854_6", "busiest first")
    check(split.anyRoleFromName, "the JS role rests on a name here")

    let js = split.threads.first { $0.role == .js }
    check(js?.samples == 48, "the JS thread's samples")
    if let share = js?.share {
        check(abs(share - 48.0/281.0) < 0.0001, "and its share of the app's samples")
    } else {
        check(false, "the JS thread has a share")
    }
    let ui = split.threads.first { $0.role == .uiMain }
    check(ui?.basis == .platformSignal, "the UI thread keeps its stronger basis")

    // Roles aggregate, and the shares sum to one -- a share of this app's
    // samples, which is the only thing they are a share of.
    let total = split.byRole.compactMap { $0.share }.reduce(0, +)
    check(abs(total - 1.0) < 0.0001, "role shares sum to 1: got \(total)")
}

do {
    // A capture that named threads but collected no samples: every share must
    // be ABSENT, not zero. A thread with no samples attributed is not a
    // thread that used no CPU, and 0% would read as the second.
    let session = parse(#"""
    {"trace":{"threads":[
      {"thread_instance_id":"t1","name":"main","tid":1,
       "is_main_ui_thread":true,"is_js_thread":false,"sample_count":null},
      {"thread_instance_id":"t2","name":"mqt_v_js","tid":2,
       "is_main_ui_thread":false,"is_js_thread":true,"sample_count":null}]}}
    """#)
    let split = threadSplit(from: session)
    check(split.totalSamples == nil, "no total when nothing was sampled",
          req: ["H01"])
    check(split.threadsWithoutSamples,
          "and the view is told to say so rather than showing zeroes")
    for t in split.threads {
        check(t.samples == nil, "\(t.name): samples absent, not zero")
        check(t.share == nil, "\(t.name): share absent, not zero")
    }
    // The roles are still known: a split with no shares is still a split.
    check(split.threads.contains { $0.role == .js }, "roles survive with no samples")
}

do {
    // A capture with no threads at all is a missing provider, not a
    // single-threaded app.
    let split = threadSplit(from: parse(#"{"trace":{"threads":[]}}"#))
    check(split.threads.isEmpty, "no threads recorded")
    check(!split.threadsWithoutSamples,
          "and that is not reported as 'threads with no samples', which is a "
          + "different thing")
    for role in ThreadRole.allCases {
        check(!role.label.isEmpty && !role.detail.isEmpty,
              "role \(role.rawValue) is described")
    }
}

do {
    // The bug this file exists for: an emulator started outside the app, and
    // a device list that went on presenting a launch-time snapshot as the
    // current state of the machine.
    let t0 = Date(timeIntervalSince1970: 1_000_000)

    // Never scanned is not "nothing is connected".
    let none = DeviceFreshness.status(loadedAt: nil, now: t0, watching: false)
    check(none.confidence == .noClaim,
          "a list that was never fetched makes no claim")
    check(none.text.contains("not yet a claim"),
          "and says so rather than reading as an empty machine")

    // A fresh scan reads as current.
    let fresh = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-3),
                                       now: t0, watching: false)
    check(fresh.confidence == .current, "a 3s-old scan is current")
    check(fresh.text.contains("3s ago"), "and states its age")

    // The case that actually misled someone: old, unwatched, and silent.
    let old = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-600),
                                     now: t0, watching: false)
    check(old.confidence == .aging, "a 10-minute-old scan is called out")
    check(old.text.contains("10m ago"), "with its real age, not 'a while'")
    check(old.text.contains("started since then"),
          "and names the inference it must not let the reader make")

    // Watching changes what the age means: it is a countdown, not a warning.
    let watched = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-600),
                                         now: t0, watching: true)
    check(watched.confidence == .current,
          "the same age while watching is not a warning")
    check(watched.text.contains("re-scanning"),
          "because the list is being kept up to date")

    // Wording at the boundaries.
    check(DeviceFreshness.ago(0) == "a moment ago", "zero is not '0s ago'")
    check(DeviceFreshness.ago(1.4) == "a moment ago", "nor is 1.4s")
    check(DeviceFreshness.ago(2) == "2s ago", "2s is stated")
    check(DeviceFreshness.ago(59) == "59s ago", "59s stays in seconds")
    check(DeviceFreshness.ago(60) == "1m ago", "60s becomes a minute")
    check(DeviceFreshness.ago(3599) == "59m ago", "and 59m stays minutes")
    check(DeviceFreshness.ago(3600) == "1h ago", "an hour is an hour")

    // A clock that jumps backwards -- NTP, or a laptop waking -- must not
    // produce "-4s ago" or a number from 1970.
    check(DeviceFreshness.ago(-4) == "at an unknown time",
          "a backwards clock is unknown, not negative")
    let future = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(120),
                                        now: t0, watching: false)
    check(!future.text.contains("-"),
          "a scan stamped in the future prints no negative age")
}

do {
    // The poll publishes only when the answer changed, so what counts as a
    // change is load-bearing: too strict and the view churns every five
    // seconds, too loose and the moment a device becomes usable is missed.
    func doc(_ body: String) -> JSON { parse(body) }
    let two = doc(#"""
      {"devices":[
        {"device_id":"emulator-5554","trust":"authorized","os_version":"17"},
        {"device_id":"ABC","trust":"offline","os_version":"26.5"}]}
    """#)

    check(DeviceFreshness.fingerprint(two) == DeviceFreshness.fingerprint(two),
          "an unchanged list fingerprints the same, so the view is not churned")

    let reordered = doc(#"""
      {"devices":[
        {"device_id":"ABC","trust":"offline","os_version":"26.5"},
        {"device_id":"emulator-5554","trust":"authorized","os_version":"17"}]}
    """#)
    check(DeviceFreshness.fingerprint(reordered) == DeviceFreshness.fingerprint(two),
          "provider ordering is not mistaken for the machine changing")

    // The transition that matters most: a device becoming usable.
    let trusted = doc(#"""
      {"devices":[
        {"device_id":"emulator-5554","trust":"authorized","os_version":"17"},
        {"device_id":"ABC","trust":"authorized","os_version":"26.5"}]}
    """#)
    check(DeviceFreshness.fingerprint(trusted) != DeviceFreshness.fingerprint(two),
          "offline becoming authorized counts as a change")

    // Arrival and departure, which is the case that started all this.
    let one = doc(#"""
      {"devices":[
        {"device_id":"emulator-5554","trust":"authorized","os_version":"17"}]}
    """#)
    check(DeviceFreshness.fingerprint(one) != DeviceFreshness.fingerprint(two),
          "a device appearing or leaving counts as a change")

    // An OS upgrade on the same device id is a different device to profile
    // against, so it must not be absorbed.
    let upgraded = doc(#"""
      {"devices":[
        {"device_id":"emulator-5554","trust":"authorized","os_version":"18"},
        {"device_id":"ABC","trust":"offline","os_version":"26.5"}]}
    """#)
    check(DeviceFreshness.fingerprint(upgraded) != DeviceFreshness.fingerprint(two),
          "an OS version change counts as a change")

    // An empty document and an empty device list fingerprint alike -- both
    // say "no devices in this answer" -- but neither is confused with the
    // two-device list.
    check(DeviceFreshness.fingerprint(doc("{}")) == DeviceFreshness.fingerprint(doc(#"{"devices":[]}"#)),
          "a missing devices array and an empty one fingerprint alike")
    check(DeviceFreshness.fingerprint(doc("{}")) != DeviceFreshness.fingerprint(two),
          "and neither is confused with a populated list")
}

do {
    // Language resolution. The interesting cases are all in the tags.
    check(Strings.resolve(.vi, preferredLanguages: ["en-US"]) == .vi,
          "an explicit choice ignores the system's preference")
    check(Strings.resolve(.en, preferredLanguages: ["vi-VN"]) == .en,
          "and in the other direction too")
    check(Strings.resolve(.system, preferredLanguages: ["vi-VN", "en-US"]) == .vi,
          "a regional tag resolves on its language subtag")
    check(Strings.resolve(.system, preferredLanguages: ["vi-Hani-VN"]) == .vi,
          "so does a tag carrying a script")
    check(Strings.resolve(.system, preferredLanguages: ["VI"]) == .vi,
          "case in a tag does not decide the language")
    check(Strings.resolve(.system, preferredLanguages: ["fr-FR", "vi-VN"]) == .vi,
          "a language this app lacks falls through to the next preference")
    check(Strings.resolve(.system, preferredLanguages: ["fr-FR"]) == .en,
          "and English is the last resort")
    check(Strings.resolve(.system, preferredLanguages: []) == .en,
          "an empty preference list does not crash or pick at random")

    // The point of keying on English: a miss returns usable text, never a key.
    let notInCatalog = "DET-04 did not run: no frame provider on this capture"
    check(Strings.translate(notInCatalog, into: .vi) == notInCatalog,
          "an untranslated string comes back unchanged, not as a key")
    check(Strings.translate(notInCatalog, into: .en) == notInCatalog,
          "and English is a pass-through by construction")
    check(Strings.translate("", into: .vi) == "",
          "an empty string survives translation")

    // Core-emitted evidence must pass through. This is the boundary the
    // Settings panel describes, and it holds because those strings are simply
    // not in the catalog.
    for evidence in ["unknown", "not_tested", "unsupported", "offline",
                     "kRanFoundNothing", "authorized"] {
        check(Strings.translate(evidence, into: .vi) == evidence,
              "the core's value '\(evidence)' is never translated")
    }

    // Tokens quoted inside translated prose stay verbatim, or the sentence
    // would explain a word that appears nowhere on screen.
    let apps = Strings.translate(
        "**running** does not mean foreground. **unknown** means the provider "
        + "could not observe the state — it does not mean not running. "
        + "Profiling availability is independent of runtime state, and entries "
        + "that cannot be profiled are kept and marked rather than hidden.",
        into: .vi)
    check(apps.contains("**running**") && apps.contains("**unknown**"),
          "the values a sentence explains are left untranslated inside it")
    check(!apps.contains("foreground.  "), "and the prose is not mangled")

    // A tab's rawValue is an identifier -- `--tab=` parses it -- so it must
    // not move when the interface language does.
    Strings.active = .vi
    for tab in DevXTab.allCases {
        check(DevXTab(rawValue: tab.rawValue) == tab,
              "tab '\(tab.rawValue)' still round-trips in Vietnamese")
    }
    check(DevXTab.devices.title == "Thiết bị", "and its title is translated")
    Strings.active = .en
    check(DevXTab.devices.title == "Devices", "and back again")

    // Every language is offered and names itself.
    check(DevXLanguage.allCases.count == 3, "three choices are offered")
    for lang in DevXLanguage.allCases {
        check(!lang.label.isEmpty, "language '\(lang.rawValue)' has a label")
        check(DevXLanguage(rawValue: lang.rawValue) == lang,
              "and a rawValue that round-trips, which is what is persisted")
    }
    check(DevXLanguage.vi.label == "Tiếng Việt",
          "Vietnamese is named in Vietnamese, for the reader looking for it")
}

do {
    // Placeholders, which are the part a translation can silently break.
    check(DeviceFreshness.fill("{n}s ago", "{n}", 7) == "7s ago",
          "a placeholder is substituted")
    check(DeviceFreshness.fill("{n}s trước", "{n}", 7) == "7s trước",
          "in either language")
    check(DeviceFreshness.fill("no placeholder", "{n}", 7) == "no placeholder",
          "a template that dropped its placeholder loses the value rather "
          + "than gaining a stray number")

    // The freshness line in Vietnamese, end to end.
    Strings.active = .vi
    let t0 = Date(timeIntervalSince1970: 2_000_000)
    let watched = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-90),
                                         now: t0, watching: true)
    check(watched.text.contains("đang quét lại"),
          "the watched line is Vietnamese")
    check(watched.text.contains("1 phút trước"),
          "and its age is Vietnamese too, with the number in place")
    check(!watched.text.contains("{when}"),
          "no placeholder survives into the rendered text")
    let aging = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-600),
                                       now: t0, watching: false)
    check(!aging.text.contains("{"), "nor in the aging warning")
    check(aging.confidence == .aging,
          "and translating does not change what is being claimed")
    let never = DeviceFreshness.status(loadedAt: nil, now: t0, watching: false)
    check(never.confidence == .noClaim && never.text.contains("chưa quét"),
          "a list that makes no claim says so in Vietnamese")
    Strings.active = .en
    check(DeviceFreshness.status(loadedAt: t0.addingTimeInterval(-90),
                                 now: t0, watching: true).text
            .contains("re-scanning"),
          "and English still works afterwards")
}

do {
    // Remembering the target. The rule is small and the failure is silent:
    // restoring a device discovery did not return leaves the app pointed at
    // something absent, and every later error blames the device rather than
    // the stale preference.
    check(RecentTargets.restore(remembered: "emulator-5554",
                                usable: ["emulator-5554", "ABC"])
            == "emulator-5554",
          "a remembered device that is present is restored")
    check(RecentTargets.restore(remembered: "emulator-5554",
                                usable: ["ABC"]) == nil,
          "a remembered device that is absent is not restored")
    check(RecentTargets.restore(remembered: "emulator-5554", usable: []) == nil,
          "and an empty device list restores nothing")
    check(RecentTargets.restore(remembered: nil,
                                usable: ["emulator-5554"]) == nil,
          "nothing remembered restores nothing")
    check(RecentTargets.restore(remembered: "", usable: ["emulator-5554"]) == nil,
          "an empty remembered id is not a device id")
    // The case that matters most: absent must not silently become "the first
    // one", or a capture gets attributed to hardware nobody chose.
    check(RecentTargets.restore(remembered: "gone", usable: ["a", "b"]) != "a",
          "an absent device never falls through to the first in the list")
}

do {
    // Filtering, and the distinction that matters: an empty list because of a
    // filter is a statement about the filter; an empty list with nothing
    // hidden is a statement about the app. Conflating them is how a filtered
    // view reads as a quiet app.
    let lines = [
        parse(#"{"level":"log","text":"fetching /v2/profile"}"#),
        parse(#"{"level":"error","text":"BIOMETRIC_ERROR_NONE_ENROLLED"}"#),
        parse(#"{"level":"log","text":"action user/login @ 1","inferred_redux_action_type":"user/login"}"#),
    ]
    let all = Set(InspectKind.allCases)

    var r = InspectFilter.console(lines, kinds: all, needle: "")
    check(r.shown.count == 3 && r.hidden == 0, "no filter shows everything")
    check(!r.hidEverything && !r.nothingToShow, "and hides nothing")

    r = InspectFilter.console(lines, kinds: [.redux], needle: "")
    check(r.shown.count == 1, "the Redux filter keeps the action line")
    check(r.shown.first?["inferred_redux_action_type"].text == "user/login",
          "which is the one carrying an inferred action type")
    check(r.hidden == 2, "and reports how many it removed")

    r = InspectFilter.console(lines, kinds: [.log], needle: "")
    check(r.shown.count == 2, "the Log filter keeps the non-action lines")

    r = InspectFilter.console(lines, kinds: [.network], needle: "")
    check(r.hidEverything,
          "a filter that matches no console line says the filter hid them")
    check(!r.nothingToShow, "not that there was nothing to show")

    r = InspectFilter.console([], kinds: all, needle: "")
    check(r.nothingToShow, "an app that logged nothing is a different answer")
    check(!r.hidEverything, "and is not blamed on the filter")

    // Text search covers the fields someone would actually type into.
    check(InspectFilter.console(lines, kinds: all, needle: "profile")
            .shown.count == 1, "a URL fragment matches the message")
    check(InspectFilter.console(lines, kinds: all, needle: "user/login")
            .shown.count == 1, "an action type matches")
    check(InspectFilter.console(lines, kinds: all, needle: "ERROR")
            .shown.count == 1, "and matching ignores case")
    check(InspectFilter.console(lines, kinds: all, needle: "  ")
            .shown.count == 3, "a whitespace-only needle is not a filter")

    // Network rows, where a status is a plausible search term.
    let reqs = [
        parse(#"{"method":"POST","url":"http://10.0.2.2:8081/symbolicate","status":500}"#),
        parse(#"{"method":"HEAD","url":"https://clients3.google.com/generate_204","status":204}"#),
        parse(#"{"method":"GET","url":"https://api.example.com/v2/profile","status":null}"#),
    ]
    check(InspectFilter.network(reqs, kinds: all, needle: "").shown.count == 3,
          "no needle shows every request")
    check(InspectFilter.network(reqs, kinds: all, needle: "500")
            .shown.count == 1, "a status is searchable")
    check(InspectFilter.network(reqs, kinds: all, needle: "post")
            .shown.count == 1, "so is a method, case-insensitively")
    check(InspectFilter.network(reqs, kinds: all, needle: "example.com")
            .shown.count == 1, "and a host")
    // A request with no status must still be searchable by its URL: the
    // absent status is not allowed to make the row invisible.
    check(InspectFilter.network(reqs, kinds: all, needle: "profile")
            .shown.count == 1,
          "a request with no status is still matched on its URL")
    let hiddenAll = InspectFilter.network(reqs, kinds: [.log], needle: "")
    check(hiddenAll.hidEverything && hiddenAll.hidden == 3,
          "turning the API filter off hides requests and says so")

    for kind in InspectKind.allCases {
        check(!kind.label.isEmpty, "kind \(kind.rawValue) has a label")
        check(InspectKind(rawValue: kind.rawValue) == kind,
              "and a rawValue that round-trips")
    }
}

if listingRequirements { exit(0) }
print("\(passed) passed, \(failures.count) failed")
exit(failures.isEmpty ? 0 : 1)
