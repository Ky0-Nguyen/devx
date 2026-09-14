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

if listingRequirements { exit(0) }
print("\(passed) passed, \(failures.count) failed")
exit(failures.isEmpty ? 0 : 1)
