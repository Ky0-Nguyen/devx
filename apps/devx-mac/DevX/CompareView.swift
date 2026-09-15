import SwiftUI
import AppKit

/// The Compare view (spec section 13).
///
/// A comparison UI is where a performance tool is most tempted to lie, because
/// the thing everyone wants from it is a single word. The engine already
/// refuses to produce that word when it cannot be earned -- incompatible
/// conditions, too few runs, variance too high, a cross-platform pair -- and
/// this view's job is to not undo that refusal by presenting an inconclusive
/// result as if it were a verdict.
///
/// So three things are load-bearing here:
///
///   * `usable_as_regression_gate` is shown before the verdict, not after it.
///     A regression that cannot gate is a different fact from one that can.
///   * A verdict never appears without its reasons. The engine's own
///     sentences are shown verbatim rather than being re-summarised.
///   * An excluded run is listed with why it was excluded. Runs that quietly
///     disappear are how a median becomes whatever you need it to be.
struct CompareView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                inputs
                if !state.compareDoc.isNull {
                    if !cmp["error"].text.isEmpty {
                        Banner(kind: .bad, title: "Cannot compare",
                               message: cmp["error"].display())
                    } else {
                        gate
                        verdict
                        conditions
                        metrics
                        thresholds
                    }
                } else {
                    TermEmpty(title: "no comparison yet",
                              detail: "Pick a baseline and a candidate run-set "
                                    + "file. A run set is several runs of the "
                                    + "same scenario under stated conditions -- "
                                    + "one capture is not a benchmark.",
                              hint: "mpi compare <baseline> <candidate>")
                }
            }
            .padding(14)
        }
        .navigationTitle("~/compare")
    }

    private var cmp: JSON { state.compareDoc["comparison"].isNull
                            ? state.compareDoc
                            : state.compareDoc["comparison"] }

    private var inputs: some View {
        Panel(title: "Run sets",
              subtitle: "each side is a set of runs, not a single capture") {
            VStack(alignment: .leading, spacing: 8) {
                filePicker(label: "baseline", path: $state.baselinePath)
                filePicker(label: "candidate", path: $state.candidatePath)
                HStack(spacing: 10) {
                    Text("min valid runs")
                        .font(Term.font(11)).foregroundStyle(Term.dim)
                    // 0 is "the engine's default", spelled out: a UI that read
                    // an empty field as a threshold of zero would pass every
                    // difference as significant.
                    Picker("", selection: $state.compareMinRuns) {
                        Text("default (5)").font(Term.font(11)).tag(0)
                        ForEach([3, 5, 10, 20], id: \.self) { n in
                            Text("\(n)").font(Term.font(11)).tag(n)
                        }
                    }
                    .frame(width: 130)
                    Button(tr("Compare")) { state.runCompare() }
                        .buttonStyle(TermButtonStyle())
                    Spacer(minLength: 0)
                }
            }
        }
    }

    private func filePicker(label: String, path: Binding<String>) -> some View {
        HStack(spacing: 8) {
            Text(label)
                .font(Term.font(11)).foregroundStyle(Term.dim)
                .frame(width: 70, alignment: .trailing)
            TextField("", text: path)
                .textFieldStyle(TermFieldStyle())
            Button(tr("Choose…")) {
                let p = NSOpenPanel()
                p.allowsMultipleSelection = false
                p.canChooseDirectories = false
                p.allowedContentTypes = [.json]
                if p.runModal() == .OK, let url = p.url {
                    path.wrappedValue = url.path
                }
            }
            .buttonStyle(TermButtonStyle())
        }
    }

    /// Before the verdict, deliberately. Whether a result may drive a gate is
    /// the question a reader actually has, and a regression that cannot gate
    /// must not be read as one that can.
    @ViewBuilder private var gate: some View {
        let usable = cmp["usable_as_regression_gate"].bool ?? false
        if cmp["cross_platform"].bool == true {
            Banner(kind: .bad, title: "Cross-platform pair",
                   message: "Android and iOS measure different things with "
                          + "different providers on different hardware. These "
                          + "two can be displayed side by side, but the pair "
                          + "cannot be used as a regression gate.")
        }
        let incompat = cmp["incompatibilities"].array.compactMap { $0.string }
        if !incompat.isEmpty {
            Banner(kind: .bad, title: "Conditions do not match",
                   message: incompat.joined(separator: "; "))
        }
        Banner(kind: usable ? .info : .caution,
               title: usable ? "Usable as a regression gate"
                             : "NOT usable as a regression gate",
               message: usable
                 ? "Both sides are certified-eligible benchmark runs under "
                 + "matching conditions."
                 : "Whatever the verdict below says, this pair does not meet "
                 + "the bar for gating a release. The reasons are on each "
                 + "metric.")
    }

    private var verdict: some View {
        Panel(title: "Overall") {
            HStack(spacing: 10) {
                Chip(text: cmp["overall_verdict"].display(),
                     tone: CompareWording.tone(cmp["overall_verdict"].text))
                Text(CompareWording.plainly(cmp["overall_verdict"].text))
                    .font(Term.font(11)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
        }
    }

    private var conditions: some View {
        Panel(title: "Conditions", subtitle: "the pair is only comparable "
                                           + "where these agree") {
            VStack(alignment: .leading, spacing: 4) {
                let b = cmp["baseline"]["conditions"]
                let c = cmp["candidate"]["conditions"]
                ForEach(CompareWording.conditionKeys, id: \.self) { key in
                    let bv = b[key].display("")
                    let cv = c[key].display("")
                    if !bv.isEmpty || !cv.isEmpty {
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Text(key)
                                .font(Term.font(10)).foregroundStyle(Term.dim)
                                .frame(width: 170, alignment: .trailing)
                            Text(bv.isEmpty ? tr("not stated") : bv)
                                .font(Term.font(11))
                                .foregroundStyle(bv.isEmpty ? Term.cyan : Term.ink)
                                .frame(width: 210, alignment: .leading)
                            Text(cv.isEmpty ? tr("not stated") : cv)
                                .font(Term.font(11))
                                // A difference is marked, not merely shown:
                                // scanning two columns for a changed string is
                                // how a mismatched condition gets missed.
                                .foregroundStyle(cv.isEmpty ? Term.cyan
                                                 : (bv == cv ? Term.ink : Term.amber))
                            Spacer(minLength: 0)
                        }
                    }
                }
            }
        }
    }

    private var metrics: some View {
        Panel(title: "Metrics") {
            VStack(alignment: .leading, spacing: 12) {
                ForEach(Array(cmp["metrics"].array.enumerated()),
                        id: \.offset) { _, m in
                    MetricComparisonRow(metric: m)
                }
                if cmp["metrics"].array.isEmpty {
                    Text(tr("No metric appears in both run sets, so there is "
                         + "nothing to compare. That is not a result of zero "
                         + "change."))
                        .font(Term.font(11)).foregroundStyle(Term.cyan)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var thresholds: some View {
        Panel(title: "Thresholds applied",
              subtitle: "a verdict is only as meaningful as the bar it cleared") {
            let t = cmp["thresholds"]
            VStack(alignment: .leading, spacing: 6) {
                Field(label: "min valid runs") {
                    Text(t["min_valid_runs"].display())
                }
                Field(label: "min relative delta") {
                    Text(t["min_relative_delta"].display())
                }
                Field(label: "min absolute delta") {
                    Text(t["min_absolute_delta"].display())
                }
                Field(label: "max relative spread") {
                    Text(t["max_relative_spread"].display())
                }
            }
        }
    }

}

/// One metric, compared. Everything that qualifies the number travels with it.
struct MetricComparisonRow: View {
    let metric: JSON

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 8) {
                Text(metric["metric_name"].display())
                    .font(Term.font(12, .medium)).foregroundStyle(Term.ink)
                Chip(text: metric["verdict"].display(),
                     tone: CompareWording.tone(metric["verdict"].text))
                if metric["certified"].bool != true {
                    // The single most over-read number in any performance
                    // tool: a delta from uncertified runs.
                    Chip(text: "certifies nothing about release",
                         tone: .caution)
                }
                Spacer(minLength: 0)
            }

            HStack(alignment: .top, spacing: 18) {
                side(label: "baseline",
                     median: metric["baseline_median"],
                     spread: metric["baseline_spread_iqr"],
                     runs: metric["baseline_valid_runs"])
                side(label: "candidate",
                     median: metric["candidate_median"],
                     spread: metric["candidate_spread_iqr"],
                     runs: metric["candidate_valid_runs"])
                delta
                Spacer(minLength: 0)
            }

            // The engine's own sentences, verbatim. Re-summarising them is how
            // a qualified verdict loses its qualification.
            ForEach(metric["reasons"].array.compactMap { $0.string },
                    id: \.self) { r in
                Text("· " + r)
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            let excluded = metric["excluded_runs"].array.compactMap { $0.string }
            if !excluded.isEmpty {
                // Excluded runs are shown, never just subtracted. A median
                // over a silently filtered set is whatever you want it to be.
                VStack(alignment: .leading, spacing: 2) {
                    Text("excluded runs")
                        .font(Term.font(10)).foregroundStyle(Term.amber)
                    ForEach(excluded, id: \.self) { e in
                        Text("· " + e)
                            .font(Term.font(10)).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
        .padding(.bottom, 2)
    }

    private func side(label: String, median: JSON, spread: JSON,
                      runs: JSON) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(Term.font(10)).foregroundStyle(Term.dim)
            // A median with no value is shown as such: a missing median is
            // not a zero, and the unit alone would make "0" look measured.
            Text(median.double.map {
                    Fmt.value($0, unit: metric["unit"].text) } ?? "no value")
                .font(Term.font(12))
                .foregroundStyle(median.double == nil ? Term.cyan : Term.ink)
            Text(tr("spread ") + (spread.double.map {
                    Fmt.value($0, unit: metric["unit"].text) } ?? "unknown"))
                .font(Term.font(10)).foregroundStyle(Term.dim)
            Text("\(runs.int ?? 0) valid run(s)")
                .font(Term.font(10)).foregroundStyle(Term.dim)
        }
        .frame(width: 180, alignment: .leading)
    }

    private var delta: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("delta").font(Term.font(10)).foregroundStyle(Term.dim)
            Text(metric["absolute_delta"].double.map {
                    Fmt.value($0, unit: metric["unit"].text) } ?? "not computed")
                .font(Term.font(12))
                .foregroundStyle(metric["absolute_delta"].double == nil
                                 ? Term.cyan : Term.ink)
            Text(metric["relative_delta"].double.map {
                    String(format: "%+.1f%%", $0 * 100) } ?? "")
                .font(Term.font(10)).foregroundStyle(Term.dim)
        }
        .frame(width: 150, alignment: .leading)
    }
}

extension CompareView {
    static var conditionKeys: [String] { CompareWording.conditionKeys }
    static func tone(_ v: String) -> StatusTone { CompareWording.tone(v) }
    static func plainly(_ v: String) -> String { CompareWording.plainly(v) }
}
