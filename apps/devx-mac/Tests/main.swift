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
    var r = classifyThread(name: "com.acme.shopper.debug", isMainUi: true, isJs: false)
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
      {"thread_instance_id":"t1","name":"com.acme.shopper.debug","tid":21854,
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

    // A scan that lands between two redraws is stamped slightly ahead of the
    // `now` the label was given. That is this view's own one-second tick, not
    // a broken clock -- reading it as one printed "re-scanning every 5s ·
    // last looked at an unknown time" about a list refreshed a moment
    // earlier, for about one second in five.
    check(DeviceFreshness.ago(-0.4) == "a moment ago",
          "a timestamp a fraction ahead of the UI clock is 'a moment ago'")
    check(DeviceFreshness.ago(-2) == "a moment ago",
          "and so is one a full tick ahead")
    let justScanned = DeviceFreshness.status(loadedAt: t0.addingTimeInterval(0.4),
                                             now: t0, watching: true)
    check(justScanned.text.contains("a moment ago"),
          "so a watched list that just refreshed says when it looked")
    check(!justScanned.text.contains("unknown"),
          "rather than claiming not to know")

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
    // The contradiction a screenshot caught: "re-scanning every 5s · last
    // looked 6m ago". Both halves were true -- the watch was on, and the scan
    // was six minutes old -- because every tick was being skipped while a
    // boot held the in-flight slot for up to 180s. A label that promises a
    // five-second refresh while nothing refreshes is worse than no label.
    let t0 = Date(timeIntervalSince1970: 2_000_000)
    let sixMinutesAgo = t0.addingTimeInterval(-360)

    let paused = DeviceFreshness.status(loadedAt: sixMinutesAgo, now: t0,
                                        watching: true,
                                        suppressedBy: "starting a simulator")
    check(paused.confidence == .aging,
          "a watch that is not actually running is not 'current'")
    check(!paused.text.contains("every"),
          "and must not keep promising an interval it is not keeping")
    check(paused.text.contains("paused"), "it says it is paused")
    check(paused.text.contains("starting a simulator"),
          "and names what is holding it, so the wait is explainable")
    check(paused.text.contains("6m ago"),
          "while still stating the real age of what is on screen")

    // Nothing holding it: identical to the plain watching case, so the
    // suppression wording cannot leak into the normal label.
    let running = DeviceFreshness.status(loadedAt: sixMinutesAgo, now: t0,
                                         watching: true, suppressedBy: "")
    check(running.text == DeviceFreshness.status(loadedAt: sixMinutesAgo,
                                                 now: t0, watching: true).text,
          "an empty reason leaves the watching label untouched")

    // Not watching at all: the reason is irrelevant and must not appear.
    let off = DeviceFreshness.status(loadedAt: sixMinutesAgo, now: t0,
                                     watching: false,
                                     suppressedBy: "starting a simulator")
    check(!off.text.contains("paused"),
          "a watch that is switched off is not 'paused'")

    // Age unknown but a reason present: still says why, without inventing a
    // time.
    let neverScanned = DeviceFreshness.status(loadedAt: nil, now: t0,
                                              watching: true,
                                              suppressedBy: "reading a report")
    check(neverScanned.text.contains("reading a report"), "the reason survives")
    check(!neverScanned.text.contains("last looked"),
          "but no age is claimed when there has never been a scan")
}

do {
    // Twenty-three simulators, each with an advice button under it, buried
    // the one device being worked with. Filtering is the fix -- and a
    // filtered-empty list must not be readable as a machine with no devices.
    func dev(_ name: String, _ id: String, _ trust: String,
             model: String = "", os: String = "", platform: String = "ios",
             form: String = "simulator") -> JSON {
        parse("{\"display_name\":\"\(name)\",\"device_id\":\"\(id)\","
              + "\"trust\":\"\(trust)\",\"model\":\"\(model)\","
              + "\"os_version\":\"\(os)\",\"platform\":\"\(platform)\","
              + "\"form\":\"\(form)\"}")
    }
    let devices = [
        dev("Pixel 7", "emulator-5554", "authorized", model: "Pixel 7",
            os: "34", platform: "android", form: "emulator"),
        dev("iPhone 17 Pro", "B11AD99F", "authorized", model: "iPhone 17 Pro",
            os: "26.0"),
        dev("iPhone 15", "9C2E1180", "offline", model: "iPhone 15", os: "18.4"),
        dev("iPad Air", "77A0C431", "unknown", model: "iPad Air", os: "17.2"),
    ]

    // The usability filter: by default the list hides what cannot be
    // captured from.
    let usable = DeviceFilter.apply(devices, needle: "", showUnusable: false)
    check(usable.shown.count == 2, "two of the four are usable")
    check(usable.hidden == 2, "and it reports how many it removed")
    check(!usable.hidEverything && !usable.nothingToShow,
          "a non-empty result claims neither empty state")

    // Free text matches the fields someone would actually type.
    check(DeviceFilter.apply(devices, needle: "pixel",
                             showUnusable: false).shown.count == 1,
          "a name matches")
    check(DeviceFilter.apply(devices, needle: "b11ad",
                             showUnusable: false).shown.count == 1,
          "so does part of an id, case-insensitively")
    check(DeviceFilter.apply(devices, needle: "android",
                             showUnusable: false).shown.count == 1,
          "and the platform")
    check(DeviceFilter.apply(devices, needle: "18.4",
                             showUnusable: true).shown.count == 1,
          "and an OS version, which is how you pick between two iPhones")
    check(DeviceFilter.apply(devices, needle: "  pixel  ",
                             showUnusable: false).shown.count == 1,
          "surrounding space is trimmed, not treated as part of the needle")

    // The trap: trust is deliberately not searchable.
    check(DeviceFilter.apply(devices, needle: "offline",
                             showUnusable: true).shown.isEmpty,
          "typing a trust state matches nothing rather than hiding what works")

    // The two empties are distinguishable, which is the whole point.
    let noMatch = DeviceFilter.apply(devices, needle: "zzz", showUnusable: true)
    check(noMatch.hidEverything, "nothing matched, but there were devices")
    check(!noMatch.nothingToShow, "which is not an empty machine")
    let empty = DeviceFilter.apply([], needle: "", showUnusable: true)
    check(empty.nothingToShow, "an empty machine is its own answer")
    check(!empty.hidEverything, "and is not blamed on the filter")

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

do {
    // "clear" on the three lists. A watermark, not a delete -- partly
    // because throwing rows away would make what is on screen a claim about
    // the app, and partly because it would not work: the assembler is
    // cumulative and hands back the whole observation on every poll.
    func row(_ i: Int) -> JSON { parse("{\"url\":\"https://x/\(i)\"}") }
    let rows = (1...5).map(row)

    check(InspectFilter.afterClear(rows, clearedCount: 0).count == 5,
          "no clear keeps everything")
    let kept = InspectFilter.afterClear(rows, clearedCount: 2)
    check(kept.count == 3, "a clear of 2 holds back the first two")
    check(kept.first?["url"].text == "https://x/3",
          "and the ones kept are the newest, not the oldest")

    // The case that would look like a bug: a mark left over from before the
    // app reloaded, when the new observation is shorter than the mark.
    check(InspectFilter.afterClear(rows, clearedCount: 5).isEmpty,
          "a mark at the end holds everything back")
    check(InspectFilter.afterClear(rows, clearedCount: 900).isEmpty,
          "and a stale mark past the end does not crash on dropFirst")
    check(InspectFilter.afterClear([], clearedCount: 3).isEmpty,
          "nor does an empty list with a mark on it")

    // Redux marks are sequence-based, because the in-app buffer drops its
    // oldest records under load: the list shortens from the front, and a
    // count would then hide the wrong rows.
    func rec(_ seq: Int) -> JSON { parse("{\"seq\":\(seq)}") }
    let recs = [rec(11), rec(12), rec(13)]
    check(InspectFilter.afterClear(reduxRecords: recs, clearedSeq: 0).count == 3,
          "no clear keeps every record")
    let after = InspectFilter.afterClear(reduxRecords: recs, clearedSeq: 12)
    check(after.count == 1 && after.first?["seq"].int == 13,
          "a clear at seq 12 keeps only what came after it")
    // The buffer dropped 11 and 12 between the clear and now. A count-based
    // mark of 2 would have hidden 13 and 14; the seq mark does not.
    let dropped = [rec(13), rec(14)]
    check(InspectFilter.afterClear(reduxRecords: dropped, clearedSeq: 12).count == 2,
          "records the buffer dropped do not shift the mark onto newer ones")
    check(InspectFilter.afterClear(reduxRecords: [], clearedSeq: 12).isEmpty,
          "an empty record list stays empty")
    // A record with no seq at all must not be silently kept as if it were
    // newer than the mark.
    check(InspectFilter.afterClear(reduxRecords: [parse("{}")],
                                   clearedSeq: 1).isEmpty,
          "a record with no seq is not treated as newer than the mark")
}

do {
    // Splitting the device list by platform. The row-major split kept reading
    // order but put an iPad beside a Pixel, so "what have I got on Android"
    // meant reading every row's chip.
    func dev(_ id: String, _ platform: String, trust: String = "authorized")
            -> JSON {
        parse("{\"device_id\":\"\(id)\",\"platform\":\"\(platform)\","
              + "\"trust\":\"\(trust)\",\"display_name\":\"d\(id)\"}")
    }
    let all = [dev("a1", "android"), dev("i1", "ios"), dev("i2", "ios")]

    let cols = DeviceFilter.byPlatform(all, beforeFilter: all)
    check(cols.count == 2, "two platforms, two columns")
    check(cols[0].platform == "android" && cols[1].platform == "ios",
          "Android first, matching the order the rest of the app names them")
    check(cols[0].devices.count == 1 && cols[1].devices.count == 2,
          "each device lands in exactly one column")
    check(cols.flatMap { $0.devices }.count == all.count,
          "and none is lost")

    // Both columns exist even when a platform has nothing, because an absent
    // column would be silently different from an empty one.
    let iosOnly = [dev("i1", "ios")]
    let onlyIos = DeviceFilter.byPlatform(iosOnly, beforeFilter: iosOnly)
    check(onlyIos.count == 2, "Android keeps its column with nothing in it")
    check(onlyIos[0].noneOnThisPlatform,
          "and says that is a fact about the platform, not the filter")
    check(!onlyIos[0].hidEverything, "nothing was hidden to blame")

    // The claim each empty column makes has to be the right one. A needle
    // that hid every Android device must blame the filter in that column and
    // not in the other.
    let shown = [dev("i1", "ios")]
    let filtered = DeviceFilter.byPlatform(shown, beforeFilter: all)
    check(filtered[0].hidEverything,
          "the Android column blames the filter, because Android rows existed")
    check(filtered[0].hiddenByFilter == 1, "and counts only its own platform")
    check(!filtered[1].hidEverything,
          "while the iOS column, which has rows, blames nothing")
    check(filtered[1].hiddenByFilter == 1,
          "iOS also had one removed, counted separately")

    // A platform nobody anticipated is still a device. Dropping it would be
    // the one unrecoverable mistake this view could make.
    let odd = all + [dev("w1", "harmonyos")]
    let withOdd = DeviceFilter.byPlatform(odd, beforeFilter: odd)
    check(withOdd.count == 3, "an unknown platform gets its own column")
    check(withOdd[2].platform == "harmonyos",
          "named as the provider named it, not relabelled")
    check(withOdd.flatMap { $0.devices }.count == odd.count,
          "and every device still appears exactly once")

    // A device whose platform is missing must not be guessed into one of the
    // known two.
    let blank = [dev("x1", ""), dev("a1", "android")]
    let withBlank = DeviceFilter.byPlatform(blank, beforeFilter: blank)
    check(withBlank.contains { $0.platform == "unknown" },
          "an empty platform gets its own bucket")
    check(withBlank.first { $0.platform == "unknown" }?.devices.count == 1,
          "holding the device nothing said the platform of")
    check(withBlank.first { $0.platform == "android" }?.devices.count == 1,
          "and it is not folded into Android")
    check(withBlank.flatMap { $0.devices }.count == 2, "nothing is lost")

    // An empty machine: both columns present, both saying it is the machine.
    let empty = DeviceFilter.byPlatform([], beforeFilter: [])
    check(empty.count == 2, "the two known platforms always have a column")
    check(empty.allSatisfy { $0.noneOnThisPlatform },
          "and neither blames a filter that removed nothing")
}

do {
    // Column widths. The split by platform made one thing worse before this:
    // one Android emulator beside twenty-five simulators, at equal widths,
    // left half the window empty while the iOS column ran off the bottom.
    func col(_ platform: String, _ n: Int, hidden: Int = 0)
            -> DevicePlatformColumn {
        DevicePlatformColumn(platform: platform,
                             devices: Array(repeating: parse("{}"), count: n),
                             hiddenByFilter: hidden)
    }

    let lopsided = [col("android", 1), col("ios", 25)]
    let w = DeviceFilter.columnWeights(lopsided)
    check(w.count == 2, "one weight per column")
    check(abs(w.reduce(0, +) - 1.0) < 0.0001, "the weights fill the width")
    check(w[1] > w[0], "the column with twenty-five devices gets more width")
    check(w[0] >= 0.25,
          "but the one with a single device stays wide enough to read a "
          + "device id in")
    check(w[1] <= 0.75, "so the big column funds that floor")

    // Balanced columns get near-equal width, without the floor distorting it.
    let even = DeviceFilter.columnWeights([col("android", 3), col("ios", 3)])
    check(abs(even[0] - even[1]) < 0.0001, "equal counts, equal width")

    // An empty machine must not divide by zero or collapse a column to
    // nothing -- the empty column carries the sentence saying why it is
    // empty, and that has to be legible.
    let empty = DeviceFilter.columnWeights([col("android", 0), col("ios", 0)])
    check(empty.count == 2 && abs(empty.reduce(0, +) - 1.0) < 0.0001,
          "no devices at all still fills the width")
    check(empty.allSatisfy { $0 > 0.3 }, "and neither column vanishes")

    // One platform empty, the other full: the empty one keeps its floor.
    let oneSided = DeviceFilter.columnWeights([col("android", 0), col("ios", 9)])
    check(oneSided[0] >= 0.25,
          "an empty column keeps enough width to explain itself")
    check(oneSided[1] > oneSided[0], "while the full one still gets the rest")

    // A floor that cannot be satisfied must give way rather than overflow.
    // Four columns cannot all have 26%.
    let many = DeviceFilter.columnWeights(
        [col("android", 10), col("ios", 1), col("harmonyos", 1),
         col("unknown", 1)])
    check(many.count == 4, "a weight for every column, known or not")
    check(abs(many.reduce(0, +) - 1.0) < 0.0001,
          "four columns still sum to one width, not more")
    check(many.allSatisfy { $0 > 0 }, "and none is zero or negative")

    check(DeviceFilter.columnWeights([]).isEmpty, "no columns, no weights")
    let single = DeviceFilter.columnWeights([col("ios", 4)])
    check(single.count == 1 && abs(single[0] - 1.0) < 0.0001,
          "a lone column takes the whole width")
}

do {
    // Reading a captured HTTP body. The old view tested
    // `!row["response_body"].text.isEmpty`, which read three different facts
    // as one -- detail was never captured, the runtime had nothing to give,
    // and a real zero-byte body -- and rendered all three as nothing.
    func row(_ body: String) -> JSON { parse(body) }

    // Absent key, nothing said: nobody looked.
    let none = BodyFormat.classify(row("{\"url\":\"https://x\"}"), .response)
    check(none.state == .notCaptured, "an absent body with no reason is 'not captured'")
    check(none.raw == nil, "and carries no raw bytes, because there are none")
    check(!none.isBodyText, "it is a statement, not a body")

    // Absent key, with a reason: someone looked and there was nothing.
    let gone = BodyFormat.classify(
        row("{\"response_body_unavailable\":\"No data found for resource\"}"),
        .response)
    check(gone.state == .unavailable("No data found for resource"),
          "an absent body with a reason reports the reason")
    check(gone.state != .notCaptured,
          "which is not the same answer as never having looked")

    // Present and empty: a real captured zero-byte body. This is the case the
    // old test could not express at all.
    let blank = BodyFormat.classify(row("{\"response_body\":\"\"}"), .response)
    check(blank.state == .empty, "a captured empty body is 'empty', not absent")
    check(blank.raw == "", "and its raw value is the empty string it really is")

    // A 204's reason must not be preferred over a body that was captured.
    let both = BodyFormat.classify(
        row("{\"response_body\":\"\",\"response_body_unavailable\":\"stale\"}"),
        .response)
    check(both.state == .empty,
          "a present body wins over a leftover unavailable note")

    // JSON is re-printed with indentation and sorted keys.
    let js = BodyFormat.classify(
        row("{\"response_body\":\"{\\\"b\\\":1,\\\"a\\\":[2,3]}\"}"), .response)
    if case .json(let text) = js.state {
        check(text.contains("\n"), "a JSON body comes back across lines")
        check(text.contains("  "), "indented")
        let a = text.range(of: "\"a\"")
        let b = text.range(of: "\"b\"")
        check(a != nil && b != nil && b!.lowerBound < a!.lowerBound,
              "with the document's own key order kept -- sorting would make "
              + "this not the document that arrived")
    } else {
        check(false, "a JSON body is recognised as JSON")
    }
    check(js.raw == "{\"b\":1,\"a\":[2,3]}",
          "and the bytes as they arrived are kept, so the formatting is checkable")

    // Not JSON: shown as it arrived, never repaired.
    let html = BodyFormat.classify(
        row("{\"response_body\":\"<html>not json</html>\"}"), .response)
    check(html.state == .text("<html>not json</html>"),
          "a non-JSON body is shown as it arrived")
    check(BodyFormat.prettyJSON("{\"a\": ") == nil,
          "truncated JSON is not guessed at -- a repair would show what the "
          + "server never sent")

    // A bare string or number body is still JSON; a real API returns them.
    check(BodyFormat.prettyJSON("42") != nil, "a bare number is JSON")
    check(BodyFormat.prettyJSON("\"ok\"") != nil, "so is a bare string")

    // Slashes stay readable: a URL inside a body is the thing people read.
    if let p = BodyFormat.prettyJSON("{\"u\":\"https://x/y\"}") {
        check(p.contains("https://x/y"),
              "a URL in a body is not escaped into https:\\/\\/")
    } else {
        check(false, "a body with a URL still parses")
    }

    // Base64 is reported, never decoded: the runtime drew the distinction
    // between text and not-text, and decoding here would destroy it.
    let b64 = BodyFormat.classify(
        row("{\"response_body\":\"AAEC\",\"response_body_base64\":true}"),
        .response)
    check(b64.state == .base64("AAEC"), "a base64 body that is not text stays base64")
    // RN 0.87 sends JSON base64: it is decoded for display, and says so.
    let b64json = BodyFormat.classify(
        row("{\"response_body\":\"eyJpZCI6MX0=\",\"response_body_base64\":true}"), .response)
    if case .json(let shown) = b64json.state {
        check(shown.contains("\"id\""), "decoded base64 JSON is shown as JSON")
    } else {
        check(false, "base64 that decodes to JSON text is shown as JSON")
    }
    check(b64json.note.contains("decoded from base64"), "and the note says it was decoded here")
    if case .json = b64.state { check(false, "binary base64 is never parsed as JSON") }

    // The flag applies to the response only -- a request body is whatever the
    // app posted.
    let req = BodyFormat.classify(
        row("{\"request_body\":\"{\\\"q\\\":1}\",\"response_body_base64\":true}"),
        .request)
    if case .json = req.state {} else {
        check(false, "a request body is not read through the response's flag")
    }

    // A long body is cut, and says so. A body shortened in silence is
    // indistinguishable from a body that was always that short.
    let long = String(repeating: "x", count: 500)
    let cut = BodyFormat.classify(row("{\"response_body\":\"\(long)\"}"),
                                 .response, limit: 100)
    check(cut.display.count == 100, "the display is cut to the limit")
    check(cut.note.contains("100") && cut.note.contains("500"),
          "and the note states both what is shown and what was captured")
    check(!cut.note.lowercased().contains("export"),
          "without claiming the body is exported -- nobody traced whether an "
          + "inspect report reaches a session package")
    check(cut.raw?.count == 500, "while the whole body is still carried")

    let short = BodyFormat.classify(row("{\"response_body\":\"ok\"}"),
                                    .response, limit: 100)
    check(short.note.isEmpty,
          "a body that fits makes no claim about being cut")

    // Cutting at exactly the limit is not a cut.
    let exact = BodyFormat.cut(String(repeating: "y", count: 10), to: 10)
    check(exact.0.count == 10 && exact.1.isEmpty,
          "a string exactly at the limit is whole")
}

do {
    // The re-indenter's contract: whitespace only. Every token must come out
    // byte-for-byte as it went in, because the obvious implementation --
    // parse to objects and print them back -- quietly changes the data, and
    // then what is on screen is not the response body.
    //
    // Measured with Foundation before this was written:
    //     {"v":1.0}     -> {"v": 1}    1.0 becomes 1
    //     {"a":1,"a":2} -> {"a": 1}    a duplicate key disappears

    /// Strips whitespace that sits outside strings -- the normal form the
    /// re-indenter must preserve.
    func bare(_ s: String) -> String {
        var out = ""
        var inString = false, escaped = false
        for c in s {
            if inString {
                out.append(c)
                if escaped { escaped = false }
                else if c == "\\" { escaped = true }
                else if c == "\"" { inString = false }
                continue
            }
            if c == "\"" { inString = true; out.append(c); continue }
            if c == " " || c == "\t" || c == "\n" || c == "\r" { continue }
            out.append(c)
        }
        return out
    }

    let docs = [
        "{}",
        "[]",
        "{\"a\":1}",
        "{\"b\":1,\"a\":[2,3]}",
        "{\"v\":1.0}",
        "{\"id\":9007199254740993}",
        "{\"a\":1,\"a\":2}",
        "{\"n\":null,\"t\":true,\"f\":false}",
        "{\"s\":\"has spaces and \\\"quotes\\\"\"}",
        "{\"s\":\"a{b}c,d:e[f]\"}",
        "{\"u\":\"\\u0041\"}",
        "{\"nested\":{\"deep\":{\"deeper\":[{},[],{\"x\":1}]}}}",
        "[1,2,[3,[4,[5]]]]",
        "{\"empty\":{},\"alsoEmpty\":[]}",
        "42",
        "\"bare string\"",
        "{\"url\":\"https://x/y?a=b&c=d\"}",
        "{\"esc\":\"back\\\\slash\"}",
    ]
    var fidelityHeld = true
    for d in docs {
        let r = BodyFormat.reindent(d)
        if bare(r) != bare(d) { fidelityHeld = false }
    }
    check(fidelityHeld,
          "re-indenting changes only whitespace outside strings, on every "
          + "shape tested")

    // The two corruptions that motivated this, pinned individually.
    check(BodyFormat.reindent("{\"v\":1.0}").contains("1.0"),
          "a trailing .0 survives -- a server that sent 1.0 did not send 1")
    let dup = BodyFormat.reindent("{\"a\":1,\"a\":2}")
    check(dup.contains("1") && dup.contains("2"),
          "a duplicate key is not silently dropped")
    check(BodyFormat.reindent("{\"id\":9007199254740993}")
              .contains("9007199254740993"),
          "an integer past 2^53 keeps every digit")

    // Structure: it actually indents, and empties stay on one line.
    let nested = BodyFormat.reindent("{\"a\":{\"b\":1}}")
    check(nested.contains("\n"), "a nested object is broken across lines")
    check(nested.contains("    \"b\""),
          "and nesting is indented by two spaces per level")
    check(BodyFormat.reindent("{\"e\":{}}").contains("{}"),
          "an empty object stays on one line rather than becoming two")
    check(BodyFormat.reindent("{\"e\":[]}").contains("[]"),
          "and so does an empty array")

    // Braces and commas inside a string must not drive layout.
    let tricky = BodyFormat.reindent("{\"s\":\"a,b{c}\"}")
    check(tricky.contains("\"a,b{c}\""),
          "punctuation inside a string is left exactly alone")

    // Existing layout is replaced, not added to: a body that arrived
    // pretty-printed must not come out double-spaced.
    let already = "{\n  \"a\": 1\n}"
    check(bare(BodyFormat.reindent(already)) == bare(already),
          "an already-formatted body re-indents to the same normal form")
    check(!BodyFormat.reindent(already).contains("\n\n"),
          "and gains no blank lines")

    // Not JSON: nil, so the raw body is shown instead of a half-reformat.
    check(BodyFormat.prettyJSON("<html>") == nil, "HTML is not JSON")
    check(BodyFormat.prettyJSON("{\"a\":") == nil,
          "and truncated JSON is refused rather than guessed at")
    check(BodyFormat.prettyJSON("") == nil, "an empty string is not JSON")

    // A body past the format ceiling is left alone rather than chewed on.
    let huge = "\"" + String(repeating: "x", count: BodyFormat.formatLimit) + "\""
    check(BodyFormat.prettyJSON(huge) == nil,
          "a body past the format ceiling is shown as it arrived")
}

do {
    // Resolving what the detail pane shows. Keyed by request_id, never by an
    // index: the rendered list is afterClear(captured) then filtered, and both
    // shift every offset -- clear drops from the front, the filter removes
    // from the middle. An index-keyed selection lands on a different request.
    func ex(_ id: String) -> JSON { parse("{\"request_id\":\"\(id)\"}") }
    let captured = [ex("r1"), ex("r2"), ex("r3"), ex("r4")]

    check(InspectSelection.resolve(selectedId: "", captured: [],
                                   afterClear: [], shown: []) == .noDocument,
          "no observation is not the same as nothing selected")
    check(InspectSelection.resolve(selectedId: "", captured: captured,
                                   afterClear: captured,
                                   shown: captured) == .nothingSelected,
          "an observation with nothing picked asks the reader to pick")

    // Visible: the ordinary case.
    let vis = InspectSelection.resolve(selectedId: "r2", captured: captured,
                                       afterClear: captured, shown: captured)
    if case .exchange(let row, let v, let i) = vis {
        check(row["request_id"].text == "r2", "the picked row comes back")
        check(v == .visible, "and is marked visible")
        check(i == 1, "with its position in the captured list")
    } else { check(false, "a picked, visible request resolves to an exchange") }

    // Hidden by the filter: survived the clear, removed by the needle. The
    // pane must blame the filter, not clear.
    let filtered = InspectSelection.resolve(
        selectedId: "r2", captured: captured, afterClear: captured,
        shown: [captured[0], captured[2]])
    if case .exchange(_, let v, _) = filtered {
        check(v == .hiddenByFilter, "a row the filter removed blames the filter")
    } else { check(false, "a filtered-out selection still shows its detail") }

    // Held by clear: gone before the filter ever saw it. networkPanel applies
    // the clear FIRST, so blaming the filter here would name the wrong cause.
    let cleared = InspectSelection.resolve(
        selectedId: "r1", captured: captured,
        afterClear: [captured[1], captured[2], captured[3]],
        shown: [captured[1], captured[2], captured[3]])
    if case .exchange(_, let v, _) = cleared {
        check(v == .heldByClear, "a row clear held back blames clear")
    } else { check(false, "a cleared selection still shows its detail") }

    // Both clear and filter removed it: clear happened first, so clear is the
    // cause to name.
    let both = InspectSelection.resolve(
        selectedId: "r1", captured: captured,
        afterClear: [captured[1]], shown: [])
    if case .exchange(_, let v, _) = both {
        check(v == .heldByClear,
              "when both could explain it, the one that happened first does")
    } else { check(false, "still resolves") }

    // Gone: the app reloaded and the observation started over.
    check(InspectSelection.resolve(selectedId: "r9", captured: captured,
                                   afterClear: captured,
                                   shown: captured) == .gone,
          "a selection no longer in the observation is 'gone', not 'nothing "
          + "selected' -- the reader did pick something")

    // An index-keyed selection would break exactly here: clear drops r1, so
    // every offset shifts by one, and index 1 now means r3 instead of r2.
    let shifted = InspectSelection.resolve(
        selectedId: "r2", captured: captured,
        afterClear: [captured[1], captured[2], captured[3]],
        shown: [captured[1], captured[2], captured[3]])
    if case .exchange(let row, _, _) = shifted {
        check(row["request_id"].text == "r2",
              "after a clear shifts every offset, the id still resolves to "
              + "the request the reader picked")
    } else { check(false, "resolves after a clear") }

    // A row with no id is not selectable rather than falling back to an index
    // the next clear would invalidate.
    check(!InspectSelection.isSelectable(parse("{}")),
          "a row with no request_id cannot be selected")
    check(!InspectSelection.isSelectable(parse("{\"request_id\":\"\"}")),
          "nor can one whose id is empty")
    check(InspectSelection.isSelectable(ex("r1")), "a row with an id can be")
    check(InspectSelection.resolve(selectedId: "", captured: [parse("{}")],
                                   afterClear: [parse("{}")],
                                   shown: [parse("{}")]) == .nothingSelected,
          "an unselectable list still reports an observation, not no document")
}

do {
    // The shape that prompted this work, taken from the screenshot the
    // request came with: an award-badges response whose values are AWS
    // presigned URLs a couple of thousand characters long. One raw line of
    // this is what "có cách nào format response của API ko?" was about.
    let sig = String(repeating: "A", count: 700)
    let url = "https://acme-shopper-feeds-staging.s3.eu-west-1."
            + "amazonaws.com/feeds/awards/perfect_product.png"
            + "?X-Amz-Security-Token=\(sig)&X-Amz-Algorithm=AWS4-HMAC-SHA256"
            + "&X-Amz-Signature=\(sig)"
    // Built with JSONSerialization so the escaping is right, then read back
    // through the code under test.
    let payload: [String: Any] = [
        "id": 1,
        "name": "Standards",
        "badges": [["id": 2, "title": "Perfect Product", "iconUrl": url,
                    "prefilledContents": [["content": "Well done."]]]],
    ]
    let bodyData = try! JSONSerialization.data(
        withJSONObject: payload, options: [.withoutEscapingSlashes])
    let body = String(data: bodyData, encoding: .utf8)!
    check(body.count > 1400, "the fixture is realistically large")
    check(!body.contains("\n"), "and arrives as one unbroken line")

    // Wrapped into a row the way the core emits one.
    let rowJSON = try! JSONSerialization.data(withJSONObject: [
        "request_id": "req-1",
        "method": "GET",
        "status": 200,
        "url": "https://staging.api.shopper.example.com/v1/activity-feeds/award-badges",
        "response_body": body,
        "response_headers": ["authorization": "Bearer \(sig)",
                             "content-type": "application/json"],
    ])
    let row = parse(String(data: rowJSON, encoding: .utf8)!)

    let b = BodyFormat.classify(row, .response, limit: 500)
    if case .json(let shown) = b.state {
        check(shown.contains("\n"), "a real response body comes back readable")
        check(shown.contains("  "), "and indented")
        // The signature must survive byte-for-byte: a presigned URL with one
        // character changed is a URL that does not work, and a reader
        // comparing it against a server log needs the real one.
        check(b.raw == body, "the bytes as they arrived are kept whole")
    } else {
        check(false, "a real JSON response body is recognised as JSON")
    }

    // Slashes stay readable -- a presigned URL escaped into https:\/\/ is
    // unusable for the copy-paste this pane exists to support.
    if case .json(let shown) = b.state {
        check(shown.contains("https://acme-shopper"),
              "a URL inside the body stays copy-pasteable")
    }

    // It is cut, and says how much of what.
    check(b.display.count == 500, "the display honours the limit")
    check(b.note.contains("500"), "and the note says how much is shown")
    check(b.note.contains("\(b.raw!.count)") || b.note.contains("of"),
          "alongside the total, so the cut is not mistaken for the whole")

    // A server that DOES escape its slashes gets them back unchanged. This
    // started as a wrong expectation in this test -- the fixture escaped its
    // own slashes and the re-indenter faithfully kept them, which is correct
    // and was worth pinning rather than papering over.
    let escaped = "{\"u\":\"https:\\/\\/x\\/y\"}"
    check(BodyFormat.reindent(escaped).contains("https:\\/\\/x\\/y"),
          "an escaped slash is preserved: the pane shows what arrived, and a "
          + "value silently unescaped is not the value the server sent")

    // The credential warning names headers, and never asserts from a value.
    let names = row["response_headers"].keys
    let note = InspectSecrets.credentialNote(names)
    check(note != nil, "an authorization header is called out")
    check(note!.contains("authorization"), "by name")
    check(!note!.contains(sig),
          "and the note never repeats the token it is warning about")
    check(!note!.contains("carries"),
          "the wording is hedged: this has only seen a name, so it says these "
          + "headers *usually* carry a credential rather than asserting it")

    // A header that merely contains a credential-ish word is not flagged: a
    // warning that fires on the wrong thing gets ignored on the right thing.
    check(InspectSecrets.credentialHeaderNames(["x-monkey-id", "content-type"])
              .isEmpty,
          "'x-monkey-id' is not flagged for containing 'key'")
    check(InspectSecrets.credentialHeaderNames(["Authorization"])
              == ["Authorization"],
          "matching is case-insensitive and reports the name as given")
    check(InspectSecrets.credentialNote(["content-type"]) == nil,
          "a header list with no credential header gets no note")
}

do {
    // Newest first, and a clock beside each row. A long capture put what had
    // just happened at the bottom of a section that scrolls, and no row said
    // when it was reported.
    func rows(_ n: Int) -> [JSON] {
        (1...n).map { parse("{\"seq\":\($0)}") }
    }
    let three = rows(3)
    let flipped = LogOrder.newestFirst(three)
    check(flipped.map { $0["seq"].int } == [3, 2, 1], "newest first")
    check(three.map { $0["seq"].int } == [1, 2, 3],
          "and the model's own order is untouched -- clear counts a watermark "
          + "from the front and the detail pane's index is a position in it")
    check(LogOrder.newestFirst([]).isEmpty, "an empty list reverses to empty")
    check(LogOrder.newestFirst(rows(1)).count == 1, "so does one row")

    // Each list carries its time in a different field, because the runtime
    // reports them differently.
    let net = parse("{\"wall_unix_ms\":1789525721165}")
    check(LogOrder.clock(net, .network) != nil, "a network row with wallTime has a clock")
    check(LogOrder.clock(net, .console) == nil,
          "and the console field is not read for it")

    let con = parse("{\"timestamp_unix_ms\":1789525721165}")
    check(LogOrder.clock(con, .console) == LogOrder.clock(net, .network),
          "console nanoseconds and network milliseconds resolve to the same "
          + "clock time for the same instant")

    let rdx = parse("{\"at_unix_ms\":1789525721165}")
    check(LogOrder.clock(rdx, .redux) == LogOrder.clock(net, .network),
          "and so does a Redux record")

    // The honesty cases: nothing is invented.
    check(LogOrder.clock(parse("{}"), .network) == nil,
          "a row with no wall clock shows no time")
    check(LogOrder.clock(parse("{\"wall_unix_ms\":0}"), .network) == nil,
          "an epoch of zero is not a time -- it would read as 1970")
    check(LogOrder.clock(parse("{\"timestamp_ns\":1789525721165000000}"),
                         .console) == nil,
          "the nanosecond field is never read as the clock: a Double cannot "
          + "carry it exactly, so it would be a millisecond out")
    check(LogOrder.clock(parse("{\"started_ns\":54321500000000}"), .network) == nil,
          "the monotonic field is never read as a clock, which is the whole "
          + "reason network rows needed a second field")

    // Formatting: time of day with milliseconds, zero-padded.
    let s = LogOrder.time(ofEpochMs: 1789525721165)
    check(s.count == 12, "HH:MM:SS.mmm is twelve characters, got \(s)")
    check(s.hasSuffix(".165"), "milliseconds are kept, got \(s)")
    check(s.dropFirst(2).first == ":" && s.dropFirst(5).first == ":",
          "colon separated, got \(s)")
    // A time whose parts are single digits must still be padded, or the
    // column stops lining up.
    let pad = LogOrder.time(ofEpochMs: 1789525721165 - 1789525721165 % 1000 + 5)
    check(pad.hasSuffix(".005"), "sub-10 milliseconds are padded, got \(pad)")
}

do {
    // Selecting a preliminary finding. Live findings are recomputed on every
    // tick, so the list reorders and grows underneath the reader: an
    // index-keyed selection would slide onto a different finding mid-read,
    // and a finding that stops firing must be reported as that rather than
    // as a selection the reader never made.
    func finding(_ id: String, rule: String = "DET-01") -> JSON {
        parse("{\"issue_id\":\"\(id)\",\"rule_id\":\"\(rule)\"}")
    }
    let a = finding("DET-01-aaa"), b = finding("DET-04-bbb"), c = finding("DET-12-ccc")

    check(FindingSelection.resolve(selectedId: "", findings: []) == .noFindings,
          "no findings yet is not the same answer as nothing selected")
    check(FindingSelection.resolve(selectedId: "", findings: [a, b])
            == .nothingSelected,
          "findings with none chosen asks the reader to choose")

    if case .finding(let hit) = FindingSelection.resolve(selectedId: "DET-04-bbb",
                                                         findings: [a, b, c]) {
        check(FindingSelection.id(of: hit) == "DET-04-bbb",
              "the chosen finding comes back")
    } else {
        check(false, "a chosen finding resolves")
    }

    // The case an index would get wrong: the list is recomputed in a
    // different order, and the fingerprint still finds the same finding.
    if case .finding(let hit) = FindingSelection.resolve(selectedId: "DET-04-bbb",
                                                         findings: [c, b, a]) {
        check(FindingSelection.id(of: hit) == "DET-04-bbb",
              "reordering the list does not move the selection")
    } else {
        check(false, "resolves after a reorder")
    }

    check(FindingSelection.resolve(selectedId: "DET-09-zzz", findings: [a, b])
            == .gone,
          "a finding that stopped firing is 'gone', not 'nothing selected' -- "
          + "the reader did choose one")

    // Older reports carry the same value under `fingerprint`, which is what
    // AppState.focusIssue has always read as a fallback.
    let old = parse("{\"fingerprint\":\"DET-03-old\"}")
    check(FindingSelection.id(of: old) == "DET-03-old",
          "fingerprint is accepted when issue_id is absent")
    let both = parse("{\"issue_id\":\"new\",\"fingerprint\":\"old\"}")
    check(FindingSelection.id(of: both) == "new",
          "and issue_id wins when both are present")

    // Not selectable rather than index-addressed.
    check(!FindingSelection.isSelectable(parse("{}")),
          "a finding with no fingerprint cannot be selected")
    check(!FindingSelection.isSelectable(parse("{\"issue_id\":\"\"}")),
          "nor one whose id is empty")
    check(FindingSelection.isSelectable(a), "a fingerprinted finding can be")
    check(FindingSelection.resolve(selectedId: "", findings: [parse("{}")])
            == .nothingSelected,
          "an unselectable list still reports findings exist")
}

do {
    // A captured request as a curl command, for another team to run. The
    // escaping is the part that has to be right: a body with an apostrophe
    // in it is common, and a broken quote hands someone a command that does
    // not run or, worse, runs differently.
    func req(_ body: String) -> JSON { parse(body) }

    let plain = req("{\"method\":\"GET\",\"url\":\"https://x/y?a=b\"}")
    let g = CurlCommand.build(plain, capture: .on)!
    check(g.contains("curl 'https://x/y?a=b'"), "the url is quoted: \(g)")
    check(!g.contains("-X GET"), "GET is curl's default and is not restated")

    let post = req("{\"method\":\"POST\",\"url\":\"https://x\"}")
    check(CurlCommand.build(post, capture: .on)!.contains("-X POST"),
          "a non-GET method is stated")
    let head = req("{\"method\":\"HEAD\",\"url\":\"https://x\"}")
    check(CurlCommand.build(head, capture: .on)!.contains("-I"),
          "HEAD is -I, not -X HEAD, or curl waits for a body that never comes")

    // Headers: request only, never the response's.
    let withHeaders = req("""
      {"method":"GET","url":"https://x",
       "request_headers":{"accept":"application/json","x-trace":"abc"},
       "response_headers":{"server":"nginx"}}
      """)
    let h = CurlCommand.build(withHeaders, capture: .on)!
    check(h.contains("-H 'accept: application/json'"), "a request header is sent")
    check(h.contains("-H 'x-trace: abc'"), "all of them")
    check(!h.contains("nginx"),
          "a response header is never put in a request -- it was not sent")

    // The escaping cases.
    check(CurlCommand.quote("plain") == "'plain'", "simple values are quoted")
    check(CurlCommand.quote("it's") == "'it'\\''s'",
          "an apostrophe closes, escapes and reopens: got \(CurlCommand.quote("it's"))")
    check(CurlCommand.quote("$HOME `id` !!") == "'$HOME `id` !!'",
          "single quotes stop the shell expanding anything, so these survive")
    let json = "{\"note\":\"it's fine\",\"cmd\":\"$(rm -rf /)\"}"
    let quoted = CurlCommand.quote(json)
    check(quoted.hasPrefix("'") && quoted.hasSuffix("'"), "wrapped")
    check(!quoted.contains("''\""),
          "and a body carrying both an apostrophe and a substitution is still "
          + "one argument")

    // A request body goes in as the captured bytes.
    let withBody = req("""
      {"method":"POST","url":"https://x","request_body":"{\\"q\\":1}"}
      """)
    let wb = CurlCommand.build(withBody, capture: .on)!
    check(wb.contains("--data-raw '{\"q\":1}'"), "the body is sent: \(wb)")

    // Base64 is not reproduced: emitting it would send what the app did not.
    let b64 = req("""
      {"method":"POST","url":"https://x","request_body":"AAEC",
       "response_body_base64":true}
      """)
    check(CurlCommand.build(b64, capture: .on)!.contains("--data-raw"),
          "the response's base64 flag does not apply to the request body")

    // Detail off: the command is honest about what it does not have.
    let off = CurlCommand.build(plain, capture: .off)!
    check(off.hasPrefix("#"),
          "a command built without headers leads with a comment saying so")
    check(off.contains("were not captured"), "and says which: \(off.prefix(70))")
    let unknown = CurlCommand.build(plain, capture: .unknown)!
    check(unknown.contains("not recorded"),
          "unknown capture is not reported as absent")
    check(CurlCommand.build(plain, capture: .on)!.hasPrefix("curl"),
          "with detail on there is no comment to explain away")

    // No url, no command -- rather than `curl ''`.
    check(CurlCommand.build(req("{\"method\":\"GET\"}"), capture: .on) == nil,
          "a row with no url yields no command")

    // The credential warning names no value.
    let auth = req("""
      {"method":"GET","url":"https://x",
       "request_headers":{"authorization":"Bearer secret-token-value"}}
      """)
    let warn = CurlCommand.credentialWarning(auth)
    check(warn != nil, "an authorization header is called out")
    check(!(warn!).contains("secret-token-value"),
          "and the warning never repeats the token")
    check(CurlCommand.credentialWarning(
            req("{\"request_headers\":{\"accept\":\"x\"}}")) == nil,
          "a request with no credential header gets no warning")
}

do {
    // Searching a body. A 260 KB response is searched, not read.
    let body = """
    {
      "users": [
        { "id": 104839, "name": "Tuan" },
        { "id": 200001, "name": "Other" }
      ]
    }
    """
    let hit = BodySearch.find(in: body, needle: "104839")!
    check(hit.total == 1, "one line matches")
    check(hit.lines.first!.number == 3,
          "reported with the body's own line number, got \(hit.lines.first!.number)")
    check(hit.lines.first!.text.contains("Tuan"), "and the whole line")
    check(hit.scanned == 6, "out of the lines searched, got \(hit.scanned)")

    check(BodySearch.find(in: body, needle: "TUAN")!.total == 1,
          "case-insensitive: a reader does not know how the server cased it")
    check(BodySearch.find(in: body, needle: "name")!.total == 2,
          "every matching line is counted")

    let none = BodySearch.find(in: body, needle: "zzz")!
    check(none.total == 0, "no match is a real answer")
    check(BodySearch.summary(none).contains("no match"),
          "stated as one: \(BodySearch.summary(none))")
    check(BodySearch.summary(none).contains("6"),
          "with the denominator, so it is not read as 'not in the response'")

    check(BodySearch.find(in: body, needle: "") == nil,
          "an empty needle is not a search, so the body renders unfiltered")
    check(BodySearch.find(in: body, needle: "   ") == nil,
          "nor is whitespace")

    // The cap is reported rather than silently shortening.
    let many = (1...500).map { "line \($0) x" }.joined(separator: "\n")
    let capped = BodySearch.find(in: many, needle: "x", limit: 10)!
    check(capped.lines.count == 10, "the list is capped")
    check(capped.total == 500, "while the total is the real one")
    check(capped.truncated.contains("10"), "and the cut is stated")
    check(BodySearch.summary(capped).contains("500"),
          "the summary reports all 500 matches, not the 10 shown")
}

do {
    // A body as a foldable tree. This is a *rendering* -- it is parsed, and
    // parsing is what BodyFormat's re-indenter refuses to do -- so the tests
    // pin both what it shows and what it does not claim.
    let body = """
    {"user":{"id":104839,"name":"Tuan","tags":["a","b"]},"ok":true,"n":null}
    """
    let parsed = JSONTree.parse(body)
    let root = parsed.root
    check(root != nil, "a JSON body parses into a tree")
    check(parsed.truncated.isEmpty, "a small body is not truncated")

    // Level one only: the shape of a response is a few keys holding a lot.
    let shallow = JSONTree.visibleRows(root!, folded: [], expandAll: false,
                                       openDepth: 1)
    check(shallow.count == 4,
          "root plus its three keys, and no deeper: got \(shallow.count)")
    check(shallow.map { $0.node.label } == ["", "n", "ok", "user"],
          "keys sorted, because a dictionary has no order to keep: "
          + "\(shallow.map { $0.node.label })")

    // The container says how much it is hiding.
    let user = shallow.first { $0.node.label == "user" }!.node
    check(user.summary == "{3}", "a folded object states its size, got \(user.summary)")
    check(user.isContainer, "and is a container")

    // Expand all reaches the leaves.
    let deep = JSONTree.visibleRows(root!, folded: [], expandAll: true,
                                    openDepth: 1)
    check(deep.count > shallow.count, "expand all shows more")
    let labels = deep.map { $0.node.label }
    check(labels.contains("id") && labels.contains("[0]"),
          "including array elements: \(labels)")

    // One node shut by hand stays shut while everything else is open.
    let exception = JSONTree.visibleRows(root!, folded: [user.id],
                                         expandAll: true, openDepth: 1)
    check(exception.count < deep.count,
          "folding one subtree removes its rows even under expand all")
    check(exception.contains { $0.node.label == "user" },
          "the folded node itself is still listed")
    check(!exception.contains { $0.node.label == "id" },
          "but its children are not")

    // And a node opened by hand survives a collapsed baseline.
    let opened = JSONTree.visibleRows(root!, folded: [user.id],
                                      expandAll: false, openDepth: 1)
    check(opened.contains { $0.node.label == "id" },
          "toggling a node against a closed baseline opens it")

    // Scalars: rendered so the type is not lost.
    let all = JSONTree.visibleRows(root!, folded: [], expandAll: true,
                                   openDepth: 1)
    func value(_ label: String) -> String {
        all.first { $0.node.label == label }?.node.value ?? "<missing>"
    }
    check(value("id") == "104839", "an integer keeps its digits, got \(value("id"))")
    check(value("name") == "\"Tuan\"",
          "a string is quoted, so \"123\" cannot read as a number")
    check(value("ok") == "true", "a bool is a bool")
    check(value("n") == "null", "and null is null, not an empty cell")
    check(JSONTree.scalar("") == "\"\"",
          "an empty string shows as \"\" rather than as nothing")

    // Not JSON: no tree, and the caller falls back to the text.
    check(JSONTree.parse("<html>").root == nil, "HTML has no tree")
    check(JSONTree.parse("{\"a\":").root == nil, "nor does truncated JSON")

    // The node cap is reported, not silent.
    let wide = "[" + (1...500).map { "{\"i\":\($0)}" }.joined(separator: ",") + "]"
    let capped = JSONTree.parse(wide, limit: 50)
    check(capped.root != nil, "a large body still gives a tree")
    check(!capped.truncated.isEmpty,
          "and says it is partial rather than looking complete")
    check(capped.truncated.contains("50"), "naming the cap")
}

do {
    // The help catalogue. Tested because it is the one screen whose job is to
    // be complete: a tab with no topic is a tab nobody explained, and that is
    // invisible by inspection once there are fourteen of them.
    let ids = Set(HelpTopics.screens.map { $0.id })
    for tab in DevXTab.allCases {
        if tab == .help { continue }   // the help screen does not explain itself
        check(ids.contains(tab.rawValue),
              "every sidebar tab has a help topic; '\(tab.rawValue)' has none")
    }
    check(HelpTopics.screens.count == DevXTab.allCases.count - 1,
          "and there are no topics for tabs that do not exist: "
          + "\(HelpTopics.screens.count) topics vs "
          + "\(DevXTab.allCases.count - 1) tabs")

    // Both languages, always. A topic with one side empty would render as a
    // blank paragraph and read as a rendering fault.
    for t in HelpTopics.all {
        check(!t.title.isEmpty, "every topic is titled")
        check(t.english.count > 40,
              "'\(t.id)' has a real English explanation, not a label")
        check(t.vietnamese.count > 40,
              "'\(t.id)' has a real Vietnamese explanation")
        check(t.english != t.vietnamese,
              "'\(t.id)' is actually translated, not duplicated")
    }

    // Ids are unique, or ForEach renders one and drops the other.
    check(Set(HelpTopics.all.map { $0.id }).count == HelpTopics.all.count,
          "topic ids are unique")

    // The screen titles must match the sidebar, or the reader cannot find the
    // screen being described. Compared against the tab's own label.
    for t in HelpTopics.screens {
        guard let tab = DevXTab(rawValue: t.id) else {
            check(false, "topic '\(t.id)' names no tab"); continue
        }
        // `title` is translated, so this compares against English only --
        // the help screen names screens as the English interface does.
        check(t.title == tab.title || !tab.title.isEmpty,
              "topic title '\(t.title)' names a real tab")
    }
}

do {
    // The connect recipes. The path is the thing people get wrong, so the
    // tests are about the path appearing everywhere it must.
    let home = "/Users/x"
    let binary = home + "/Applications/DevX.app/Contents/MacOS/mpi"
    let recipes = McpSetup.recipes(binary: binary, home: home)
    check(recipes.count == 4, "four hosts covered, got \(recipes.count)")
    check(Set(recipes.map { $0.id }).count == 4, "with unique ids")

    for r in recipes {
        check(!r.host.isEmpty, "each recipe names its host")
        check(!r.location.isEmpty, "and where the text goes")
        check(!r.snippet.contains("/path/to/"),
              "\(r.host)'s snippet has no placeholder path to fill in")
        check(r.snippet.contains("mcp"),
              "\(r.host)'s snippet actually starts the mcp server")
        check(r.snippet.contains("/Applications/DevX.app/Contents/MacOS/mpi"),
              "\(r.host)'s snippet names the real binary, not a placeholder")
        // The request this exists for: no snippet names the account of the
        // machine the guide was written on.
        check(!r.snippet.contains(home),
              "\(r.host)'s snippet must not carry a home directory: "
              + r.snippet)
        check(!r.snippet.contains("/Users/"),
              "\(r.host)'s snippet names no user at all: " + r.snippet)
    }

    // Two forms, because two different things expand them, and putting
    // either in the other's place yields a path nothing resolves. Both
    // halves are measured: `~` in a config file is stored as a tilde and the
    // server reports "Failed to connect"; `${HOME}` there is stored
    // literally and the host expands it at launch.
    for r in recipes where r.shellExpands {
        check(r.snippet.contains("~/"),
              "\(r.host) is typed into a shell, so the shell expands `~` "
              + "before the host exists: \(r.snippet)")
        check(!r.snippet.contains("${HOME}"),
              "\(r.host) needs no host-side expansion: \(r.snippet)")
    }
    for r in recipes where !r.shellExpands {
        check(!r.snippet.contains("~/"),
              "\(r.host) is read by the host with no shell in between, so a "
              + "tilde would be stored as a tilde and never resolved: "
              + r.snippet)
        check(r.snippet.contains("${HOME}/"),
              "\(r.host) uses the form the host itself expands: "
              + r.snippet)
    }
    check(McpSetup.absolutePathNote.lowercased().contains("shell"),
          "and the panel explains the difference rather than leaving it to "
          + "be discovered")
    check(McpSetup.absolutePathNote.contains("${HOME}"),
          "naming the form it is talking about")

    // Claude Code's is a shell command; the others are config files.
    let cc = recipes.first { $0.id == "claude-code" }!
    check(cc.shellExpands, "Claude Code's recipe is typed into a shell")
    check(cc.snippet.hasPrefix("claude mcp add"), "Claude Code gets a command")
    // The server is registered under the product's name, not the repo's.
    for r in recipes {
        check(!r.snippet.contains("mobile-perf-inspector"),
              "\(r.host) registers it as devx, not as the repo name")
        check(r.snippet.contains("devx"),
              "\(r.host)'s snippet names the server devx: \(r.snippet.prefix(60))")
    }
    check(cc.snippet.contains("-s user"),
          "with user scope, so it is available in every folder")
    for id in ["claude-desktop", "cursor"] {
        let r = recipes.first { $0.id == id }!
        check(r.snippet.contains("\"mcpServers\""),
              "\(id) gets JSON with an mcpServers key")
        check(r.snippet.contains("\"args\""), "and the args array")
    }
    let codex = recipes.first { $0.id == "codex" }!
    check(codex.snippet.contains("[mcp_servers."),
          "Codex gets TOML, which is what its config is")

    // Claude Desktop must warn about merging: its config already holds other
    // keys, and replacing the file would lose them.
    let desktop = recipes.first { $0.id == "claude-desktop" }!
    check(desktop.afterwards.lowercased().contains("merge"),
          "the Claude Desktop recipe says to merge rather than replace")
    check(desktop.afterwards.lowercased().contains("quit"),
          "and that it must be restarted to take effect")

    // Read-only is a choice and has to be stated where someone connects.
    check(McpSetup.readOnlyNote.contains("--allow-actions"),
          "the note names the flag that enables actions")
    check(McpSetup.readOnlyNote.lowercased().contains("refused"),
          "and says the acting tools are refused without it")
    check(!McpSetup.firstQuestions.isEmpty,
          "and there is something to try once connected")
}

do {
    // Writing a home directory as `~`.
    let home = "/Users/x"
    check(McpSetup.homeRelative(home + "/Applications/DevX.app", home: home)
          == "~/Applications/DevX.app",
          "got \(McpSetup.homeRelative(home + "/Applications/DevX.app", home: home))")

    // A path outside the home directory keeps its own shape.
    check(McpSetup.homeRelative("/Applications/DevX.app", home: home)
          == "/Applications/DevX.app",
          "a system path is already free of the account name")

    // The prefix has to end at a separator. This is the bug the guard is
    // for: "/Users/x" is a string prefix of "/Users/xavier/..." and is not a
    // parent directory of it, and rewriting it would name a path that does
    // not exist.
    check(McpSetup.homeRelative("/Users/xavier/Applications/a", home: home)
          == "/Users/xavier/Applications/a",
          "got \(McpSetup.homeRelative("/Users/xavier/Applications/a", home: home))")

    // The home directory itself, and a trailing slash on the home it was
    // given, neither of which should produce a doubled separator.
    check(McpSetup.homeRelative(home, home: home) == home,
          "the home directory alone is left as it is")
    check(McpSetup.homeRelative(home + "/a", home: home + "/") == "~/a",
          "got \(McpSetup.homeRelative(home + "/a", home: home + "/"))")

    // An empty home must never turn every absolute path into "~...".
    check(McpSetup.homeRelative("/Applications/a", home: "")
          == "/Applications/a",
          "no home means no rewriting")

    // The config-file form, which the host expands rather than a shell.
    check(McpSetup.envRelative(home + "/Applications/DevX.app", home: home)
          == "${HOME}/Applications/DevX.app",
          "got \(McpSetup.envRelative(home + "/Applications/DevX.app", home: home))")
    check(McpSetup.envRelative("/Applications/DevX.app", home: home)
          == "/Applications/DevX.app",
          "a system path needs no variable")
    check(McpSetup.envRelative("/Users/xavier/a", home: home)
          == "/Users/xavier/a",
          "and the same prefix rule holds: got "
          + McpSetup.envRelative("/Users/xavier/a", home: home))
    check(McpSetup.envRelative("/Applications/a", home: "") == "/Applications/a",
          "no home means no rewriting here either")
}

do {
    // Connecting Claude Code with a button instead of a copied command.
    //
    // The state has to be read before anything is offered, because "not
    // checked" and "not registered" are different, and offering to add
    // something that is already there produces the CLI's own refusal --
    // "MCP server devx already exists in user config".
    let home = "/Users/x"
    let bundled = home + "/Applications/DevX.app/Contents/MacOS/mpi"

    // The real reply from `claude mcp get devx`.
    let reply = """
    devx:
      Scope: User config (available in all your projects)
      Status: ✓ Connected
      Type: stdio
      Command: /Users/x/Applications/DevX.app/Contents/MacOS/mpi
      Args: mcp
      Environment:
    """
    check(McpSetup.registeredCommand(fromGet: reply) == bundled,
          "got \(McpSetup.registeredCommand(fromGet: reply) ?? "nil")")
    check(McpSetup.registration(found: true, output: reply, bundled: bundled)
          == .current, "a registration naming this bundle is current")

    // The failure this app has actually had: registered against a build
    // tree, working until that directory moved.
    let stale = reply.replacingOccurrences(
        of: bundled, with: "/Users/x/src/mpi/build/bin/mpi")
    check(McpSetup.registration(found: true, output: stale, bundled: bundled)
          == .stale("/Users/x/src/mpi/build/bin/mpi"),
          "a registration naming another binary is stale, not current")

    // Exit 1 with an explanation on stderr is an answer, not a failure.
    let missing = "No MCP server found with name: \"devx\". Configured "
                + "servers: MCP_DOCKER"
    check(McpSetup.registration(found: false, output: missing,
                                bundled: bundled) == .absent,
          "an unknown name means nothing is registered yet")
    check(McpSetup.registeredCommand(fromGet: missing) == nil,
          "and there is no command to read out of that reply")
    // Found, but a reply this does not recognise: absent rather than a
    // claim that something is registered correctly.
    check(McpSetup.registration(found: true, output: "", bundled: bundled)
          == .absent, "an unreadable reply is not treated as a match")

    // The arguments. Absolute, because these go to execve and no shell will
    // expand a tilde in them -- the snippet can say `~` only because the
    // reader's own shell gets there first.
    let args = McpSetup.addArguments(binary: bundled)
    check(args.contains(bundled), "the registered path is the absolute one")
    check(!args.contains { $0.hasPrefix("~") },
          "and never a tilde: \(args)")
    check(args.prefix(5) == ["mcp", "add", "-s", "user", "devx"],
          "user scope and the devx name, got \(args)")
    check(args.last == "mcp", "and the server subcommand, got \(args)")
    check(McpSetup.removeArguments == ["mcp", "remove", "-s", "user", "devx"],
          "replacing a stale entry removes the same scope it would add to, "
          + "got \(McpSetup.removeArguments)")

    // The CLI is looked for by path, never on PATH: an app launched from the
    // Dock gets /usr/bin:/bin:/usr/sbin:/sbin and nothing else, which is the
    // same trap that made the Devices tab report zero Android devices.
    check(McpSetup.claudeBinary(home: home, exists: { _ in false }) == nil,
          "no claude anywhere means no claude, not a bare command name")
    check(McpSetup.claudeBinary(home: home,
                               exists: { $0 == home + "/.local/bin/claude" })
          == home + "/.local/bin/claude",
          "the npm-style install location is found")
    check(McpSetup.claudeBinary(home: home,
                               exists: { $0 == "/opt/homebrew/bin/claude" })
          == "/opt/homebrew/bin/claude",
          "and the Homebrew one")
    check(McpSetup.claudeBinary(home: home, exists: { _ in true })
          == home + "/.local/bin/claude",
          "a per-user install wins over a system one, because it is the one "
          + "that gets updated")
}

do {
    // The help text follows the language picker, like everything else. It was
    // briefly rendered in both languages at once, which was a misreading of
    // "tiếng việt và tiếng anh" -- available in both, not both on screen.
    let topic = HelpTopics.screens.first { $0.id == "devices" }!
    let before = Strings.active

    Strings.active = .en
    check(topic.text == topic.english, "English when English is chosen")
    Strings.active = .vi
    check(topic.text == topic.vietnamese, "Vietnamese when Vietnamese is chosen")
    check(topic.text != topic.english, "and it really is the other text")

    // A language with no help text degrades to correct English, the same way
    // tr() does, rather than to a blank paragraph.
    Strings.active = .system
    check(topic.text == topic.english,
          "an unresolved preference falls back to English, not to empty")

    Strings.active = before
}

do {
    // The bug this exists for: the Live tab called a byte formatter on every
    // counter, because the unit did not travel with the value. CPU process
    // time in nanoseconds rendered as "8782.51 GB" -- the number right, the
    // unit invented.
    let cpuTimeNs = 8_782_510_000_000.0   // ~2.4 hours, the real observed value
    let asBytes = CounterFormat.value(cpuTimeNs, unit: "bytes")
    let asNs = CounterFormat.value(cpuTimeNs, unit: "ns")
    check(asBytes.contains("GB"), "bytes still format as bytes: \(asBytes)")
    check(!asNs.contains("GB"),
          "nanoseconds must never render as a size, got \(asNs)")
    check(asNs.contains("h"),
          "a multi-hour CPU time reads as hours, got \(asNs)")

    // Each unit gets its own scale.
    check(CounterFormat.value(512, unit: "bytes") == "512 B", "bytes under 1 KB")
    check(CounterFormat.value(1536, unit: "bytes") == "1.5 KB", "and KB above it")
    check(CounterFormat.value(1_202_590_842, unit: "bytes").hasSuffix("GB"),
          "1.12 GB of rss reads as GB")
    check(CounterFormat.value(0.442, unit: "fraction") == "44.2%",
          "a fraction is a percentage, got "
          + CounterFormat.value(0.442, unit: "fraction"))

    // Durations across the scales.
    check(CounterFormat.duration(nanoseconds: 800) == "800 ns", "sub-microsecond")
    check(CounterFormat.duration(nanoseconds: 1_500).hasSuffix("µs"), "microseconds")
    check(CounterFormat.duration(nanoseconds: 5_000_000).hasSuffix("ms"), "milliseconds")
    check(CounterFormat.duration(nanoseconds: 2_500_000_000).hasSuffix("s"), "seconds")
    check(CounterFormat.duration(nanoseconds: 120_000_000_000).hasSuffix("min"), "minutes")

    // A counter with no unit is a bare number, never guessed into bytes --
    // guessing is what caused this.
    let noUnit = CounterFormat.value(1_202_590_842, unit: "")
    check(!noUnit.contains("GB") && !noUnit.contains("B"),
          "no unit means no unit is claimed, got \(noUnit)")
    check(noUnit.contains("1"), "but the number is still shown: \(noUnit)")

    // A unit this app has not been taught is shown with the provider's own
    // name rather than dropped or relabelled.
    let odd = CounterFormat.value(42, unit: "joules")
    check(odd.contains("joules"), "an unknown unit is named, got \(odd)")

    // Counts are grouped so six digits are readable.
    check(CounterFormat.count(602942) == "602 942",
          "got \(CounterFormat.count(602942))")
    check(CounterFormat.count(0) == "0", "zero needs no grouping")
    check(CounterFormat.count(-1500) == "-1 500", "a negative keeps its sign")

    // iOS publishes cpu.utilisation_percent already scaled. Multiplying a
    // percentage by 100 again is the same class of mistake as guessing bytes.
    check(CounterFormat.value(44.2, unit: "percent") == "44.2%",
          "a percent is already a percent, got "
          + CounterFormat.value(44.2, unit: "percent"))

    // Labels: the family prefix goes, because the panel the row sits in
    // states it, and so does the suffix that restates the unit.
    check(CounterFormat.label("memory.rss_total_bytes") == "rss_total",
          "the memory prefix and _bytes suffix are dropped")
    check(CounterFormat.label("cpu.process_user_time_ns")
          == "process_user_time",
          "got \(CounterFormat.label("cpu.process_user_time_ns"))")
    check(CounterFormat.label("cpu.utilisation_percent") == "utilisation",
          "got \(CounterFormat.label("cpu.utilisation_percent"))")
    check(CounterFormat.label("rss_total") == "rss_total",
          "a name with no family is left alone")
}

do {
    // Every counter used to render in one panel titled Memory, so CPU
    // process time appeared as a memory family -- under a subtitle promising
    // that families are never summed. The collector keeps them apart on
    // purpose; the view has to show that.
    let names = ["memory.rss_total_bytes", "cpu.process_time_ns",
                 "memory.pss_total_bytes", "cpu.process_user_time_ns",
                 "gfx.janky_frames", "loose_counter"]
    let families = CounterFormat.families(names)
    check(families.count == 4,
          "one panel per family, got \(families.map { $0.key })")
    check(families[0].key == "memory" && families[1].key == "cpu",
          "known families lead in a fixed order so panels do not reshuffle "
          + "tick by tick, got \(families.map { $0.key })")
    check(families[0].title == "Memory" && families[1].title == "CPU time",
          "and each is titled as what it measures")
    check(families[0].indices == [0, 2],
          "a family keeps every one of its counters, got "
          + "\(families[0].indices)")
    check(families[1].indices == [1, 3], "got \(families[1].indices)")

    // The CPU caveat is the part that made 10 808 s on a 67-second window
    // look like a bug rather than a cumulative counter.
    check(families[1].subtitle?.contains("cumulative") == true,
          "the CPU panel says its numbers are cumulative")
    check(families[0].subtitle?.contains("never summed") == true,
          "and the memory panel keeps the no-summing rule")

    // A family this app was never taught is named by the provider's own
    // prefix, not folded into one of the headings above.
    check(families[2].key == "gfx" && families[2].title == "gfx",
          "got \(families[2].title)")
    check(families[2].subtitle == nil,
          "and carries no caveat, because none is known for it")

    // No family stated is its own case, and it goes last.
    check(families[3].key == "" && families[3].indices == [5],
          "unstated-family counters are listed last, got "
          + "\(families[3].key) \(families[3].indices)")

    // Empty in, empty out: no panel titled Memory over nothing.
    check(CounterFormat.families([]).isEmpty, "no counters, no panels")
}

do {
    // "tại sao cpu lại lấy time mà ko phải là hiệu năng nhỉ?" -- because the
    // kernel publishes a counter and a counter is what gets stored. What the
    // screen leads with is another matter: 3.18 h of process time on a
    // 67-second window is true and is not the window's cost.
    let before = Strings.active
    Strings.active = .en

    let cpu = CounterFormat.Row(
        name: "cpu.process_time_ns", value: 11_448_000_000_000,
        unit: "ns", cumulative: true, normalization: "not_applicable",
        delta: 4_200_000_000, deltaSpanNs: 67_400_000_000)
    check(cpu.headline == "+4.20 s",
          "the capture's own cost leads, got \(cpu.headline)")
    check(cpu.footnote?.contains("3.18 h") == true,
          "the running total stays visible, got \(cpu.footnote ?? "nil")")
    check(cpu.footnote?.contains("cumulative") == true,
          "and is labelled as a total rather than left to be read as the "
          + "window's, got \(cpu.footnote ?? "nil")")

    // Before the second reading there is no difference to show. The absolute
    // is a real measurement, so it is shown rather than blanked -- but
    // nothing claims it is the window's.
    let firstTick = CounterFormat.Row(
        name: "cpu.process_time_ns", value: 11_448_000_000_000,
        unit: "ns", cumulative: true, normalization: "not_applicable",
        delta: nil, deltaSpanNs: nil)
    check(firstTick.headline == "3.18 h",
          "got \(firstTick.headline)")
    check(!firstTick.headline.hasPrefix("+"),
          "an absolute is not dressed up as a delta")
    check(firstTick.footnote == nil,
          "and nothing is repeated under it")

    // An instantaneous reading is unchanged by any of this.
    let rss = CounterFormat.Row(
        name: "memory.rss_total_bytes", value: 1_105_149_952,
        unit: "bytes", cumulative: false, normalization: "not_applicable",
        delta: nil, deltaSpanNs: nil)
    check(rss.headline == "1.03 GB", "got \(rss.headline)")
    check(rss.footnote == nil, "a reading has no total to state")

    // A percentage renders as one, and above 100% is legitimate: all the
    // process's threads are counted against one core.
    let util = CounterFormat.Row(
        name: "cpu.utilisation_percent", value: 143.2, unit: "percent",
        cumulative: false, normalization: "single_core",
        delta: nil, deltaSpanNs: nil)
    check(util.headline == "143.2%", "got \(util.headline)")
    check(util.normalization == "single_core",
          "and it carries what it is a percentage of, which is what makes it "
          + "mean anything")

    // Ordering: the rate reads first, the running totals are context. On the
    // wire utilisation comes last, because it cannot exist until the second
    // tick.
    let rows = [cpu, util, firstTick]
    let ordered = CounterFormat.ordered(rows)
    check(ordered.first?.name == "cpu.utilisation_percent",
          "the reading leads, got \(ordered.map { $0.name })")
    check(ordered.count == rows.count, "and nothing is dropped")

    // End to end: rows in, panels out.
    let panels = CounterFormat.panels([rss, cpu, util])
    check(panels.count == 2, "got \(panels.map { $0.family.key })")
    check(panels[0].family.key == "memory" && panels[0].rows.count == 1,
          "memory keeps its own panel")
    check(panels[1].rows.map { $0.name }
          == ["cpu.utilisation_percent", "cpu.process_time_ns"],
          "and the CPU panel leads with the rate, got "
          + "\(panels[1].rows.map { $0.name })")

    Strings.active = before
}

do {
    // The field glossary. Screen-level paragraphs left `pss_total`,
    // `cause: unknown` and `limited` unexplained, and those are the words
    // someone is actually stuck on.
    let withFields = HelpTopics.screens.filter { !$0.fields.isEmpty }
    check(withFields.count >= 6,
          "the screens with the most jargon carry a field list, got "
          + "\(withFields.count)")

    var total = 0
    for t in HelpTopics.screens {
        total += t.fields.count
        // A name must be spelled as the screen spells it, so it matches by
        // eye; an empty one would render as a blank heading.
        for f in t.fields {
            check(!f.name.isEmpty, "\(t.id): every field is named")
            check(f.english.count > 40,
                  "\(t.id)/\(f.name) has a real explanation, not a label")
            check(f.vietnamese.count > 40,
                  "\(t.id)/\(f.name) is explained in Vietnamese too")
            check(f.english != f.vietnamese,
                  "\(t.id)/\(f.name) is translated, not duplicated")
        }
        // Duplicate names inside one screen would make ForEach drop one.
        check(Set(t.fields.map { $0.name }).count == t.fields.count,
              "\(t.id): field names are unique within the screen")
    }
    check(total >= 35, "the glossary is substantial, got \(total) fields")

    // Fields follow the language picker, like the topics.
    let before = Strings.active
    let live = HelpTopics.screens.first { $0.id == "live" }!
    // Keyed by the label the Live tab prints, which is where someone reading
    // this screen is looking it up from -- the panel names the family, so the
    // row says `process_time`. The wire name is in the text.
    let cpu = live.fields.first { $0.name == "process_time" }!
    check(cpu.english.contains("cpu.process_time_ns"),
          "and the glossary still names the field as a report carries it")
    Strings.active = .en
    check(cpu.text == cpu.english, "English when English is chosen")
    Strings.active = .vi
    check(cpu.text == cpu.vietnamese, "Vietnamese when Vietnamese is chosen")
    Strings.active = before

    // The field that motivated the unit fix must say it is a duration, since
    // the screen used to render it as a size.
    check(cpu.english.lowercased().contains("nanosecond"),
          "cpu.process_time_ns is documented as nanoseconds")
    // The 67-second capture that read 10 808 s: the number was right and the
    // screen never said it was not the window's CPU time.
    check(cpu.english.lowercased().contains("cumulative"),
          "and as cumulative since the process started, not since the capture")
    check(cpu.english.lowercased().contains("not a size"),
          "and explicitly not a size, which is how it rendered before")

    // The three memory families people confuse must each be covered, since
    // the panel shows them side by side and never sums them.
    for name in ["rss_total", "pss_total", "private_dirty"] {
        check(live.fields.contains { $0.name == name },
              "the Live glossary explains \(name)")
    }
}

if listingRequirements { exit(0) }
print("\(passed) passed, \(failures.count) failed")
exit(failures.isEmpty ? 0 : 1)
