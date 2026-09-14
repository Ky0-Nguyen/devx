import SwiftUI

/// The issue browser.
///
/// This is where the specification's honesty rules have to survive contact
/// with a UI, so the layout is deliberate: detection status and cause status
/// are separate columns, severity carries its rationale rather than standing
/// alone, and the detector-execution table is always present so "no findings"
/// can never be read as "nothing is wrong".
struct IssuesView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Group {
            if state.sessionDoc.isNull {
                TermEmpty(title: "no session open",
                          detail: "Open one from Sessions, or record a new capture.",
                          hint: "mpi analyze <session>")
            } else {
                // Both columns must be told to fill: an HSplitView sizes to
                // its children's ideal height, which collapsed the whole pane
                // into a short band floating in the middle of the window and
                // clipped the context panel mid-row.
                HSplitView {
                    leftColumn
                        .frame(minWidth: 340, idealWidth: 430,
                               maxHeight: .infinity)
                    detailColumn.frame(minWidth: 380, maxHeight: .infinity)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .navigationTitle("~/issues")
    }

    private var trace: JSON { state.sessionDoc["trace"] }
    private var analysis: JSON { state.sessionDoc["analysis"] }

    private var leftColumn: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                provenanceBanners
                contextPanel
                issueList
                detectorTable
            }
            .padding(14)
        }
    }

    @ViewBuilder private var provenanceBanners: some View {
        // Provenance first. A reader must not get to a number before learning
        // whether it describes a real application.
        if trace["synthetic"].bool == true {
            Banner(kind: .caution, title: "Synthetic data",
                   message: "This session came from a labelled fixture, not a real device "
                          + "capture. Nothing here describes a real application. "
                          + trace["synthetic_note"].display(""))
        }
        if trace["partial"].bool == true {
            Banner(kind: .caution, title: "Partial capture",
                   message: "The recording ended abnormally, so absence of a finding is "
                          + "not evidence of absence. "
                          + trace["partial_reasons"].array
                              .compactMap { $0.string }.joined(separator: "; "))
        }
        let failures = state.sessionDoc["checksum_failures"].array.compactMap { $0.string }
        if !failures.isEmpty {
            Banner(kind: .bad, title: "Checksum mismatch",
                   message: failures.joined(separator: "; "))
        }
    }

    private var contextPanel: some View {
        let be = analysis["benchmark_eligibility"]
        let counts = trace["counts"]
        return Panel(title: "Measurement context") {
            VStack(alignment: .leading, spacing: 6) {
                Field(label: "platform") {
                    HStack(spacing: 5) {
                        Text(trace["device"]["platform"].display())
                        Chip(text: trace["device"]["form"].display(),
                             tone: trace["device"]["form"].text == "physical"
                                   ? .neutral : .caution)
                    }
                }
                Field(label: "device") {
                    Text(trace["device"]["display_name"]
                          .display(trace["device"]["device_id"].display()))
                }
                Field(label: "target") {
                    Text(trace["target"]["application_key"]["app_identifier"].display())
                        .font(Term.font(12))
                }
                Field(label: "mode") { Text(analysis["measurement_mode"].display()) }
                Field(label: "window") {
                    Text(formatNs(trace["duration_ns"].double ?? 0))
                }
                Field(label: "clock") {
                    Text(trace["primary_clock_domain"].display())
                        .font(Term.font(11))
                }
                Field(label: "collected") {
                    Text("\(counts["frames"].display("0")) frames · "
                         + "\(counts["cpu_samples"].display("0")) samples · "
                         + "\(counts["js_tasks"].display("0")) js tasks · "
                         + "\(counts["counters"].display("0")) counters")
                        .font(Term.small)
                }
                Field(label: "eligibility") {
                    HStack(spacing: 5) {
                        Text(be["status"].display())
                        if be["certified_benchmark"].bool != true {
                            Chip(text: "cannot certify release", tone: .caution)
                        }
                    }
                }
                let notes = analysis["data_quality_notes"].array.compactMap { $0.string }
                if !notes.isEmpty { BulletList(title: "Data quality", items: notes) }
            }
        }
    }

    @ViewBuilder private var issueList: some View {
        let issues = state.issues
        if issues.isEmpty {
            Banner(kind: .info, title: "No detector that ran produced a finding",
                   message: "See the detector table below for which detectors could not "
                          + "run, and why. A skipped detector found nothing because it "
                          + "did not run — that is not the same statement as \"no issue "
                          + "exists\".")
        } else {
            Panel(title: "Issues (\(issues.count))") {
                VStack(spacing: 5) {
                    ForEach(Array(issues.enumerated()), id: \.offset) { idx, i in
                        IssueListRow(issue: i, selected: idx == state.selectedIssueIndex)
                            .onTapGesture { state.selectedIssueIndex = idx }
                    }
                }
            }
        }
    }

    private var detectorTable: some View {
        let runs = state.ruleRuns
        let ran = runs.filter { $0["outcome"].text != "skipped" }.count
        return Panel(title: "Detector execution",
                     subtitle: "\(ran) ran, \(runs.count - ran) could not.") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(runs.enumerated()), id: \.offset) { _, r in
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 6) {
                            Text("\(r["rule_id"].text) v\(r["rule_version"].text)")
                                .font(Term.font(11))
                            Chip(text: r["outcome"].text,
                                 tone: StatusTone.outcome(r["outcome"].text))
                            if let n = r["issues_emitted"].int, n > 0 {
                                Chip(text: "\(n) issue\(n == 1 ? "" : "s")",
                                     tone: .caution)
                            }
                            Spacer()
                        }
                        let why = r["skipped_reasons"].array.compactMap { $0.string }
                        if !why.isEmpty {
                            ForEach(Array(why.enumerated()), id: \.offset) { _, w in
                                Text(w).font(Term.small).foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                    .padding(7)
                    .termCard()
                }
            }
        }
    }

    @ViewBuilder private var detailColumn: some View {
        if state.issues.indices.contains(state.selectedIssueIndex) {
            IssueDetail(issue: state.issues[state.selectedIssueIndex])
        } else {
            TermEmpty(title: "nothing selected",
                      detail: "This session produced no issue to inspect. "
                            + "That is not the same as nothing being wrong: "
                            + "the detector table says which detectors ran.")
                .frame(maxHeight: .infinity)
        }
    }
}

private struct IssueListRow: View {
    let issue: JSON
    let selected: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Chip(text: issue["severity"].text,
                 tone: StatusTone.severity(issue["severity"].text))
            VStack(alignment: .leading, spacing: 3) {
                Text(issue["title"].text).font(Term.body)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 5) {
                    Text(issue["rule_id"].text)
                        .font(Term.font(10))
                        .foregroundStyle(.secondary)
                    // The two axes are always shown together: an observed
                    // symptom with an unknown cause is the normal result and
                    // must not look like an incomplete one.
                    Chip(text: issue["detection_status"].text,
                         tone: StatusTone.detection(issue["detection_status"].text))
                    Chip(text: "cause: \(issue["cause_status"].text)",
                         tone: issue["cause_status"].text == "unknown"
                               ? .neutral : .caution)
                    if issue["suppression"]["suppressed"].bool == true {
                        Chip(text: "suppressed", tone: .caution)
                    }
                }
            }
            Spacer(minLength: 0)
        }
        .padding(8)
        .termCard(selected: selected)
        .contentShape(Rectangle())
    }
}

private struct IssueDetail: View {
    let issue: JSON

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(issue["title"].text).font(Term.font(15, .bold))
                    .fixedSize(horizontal: false, vertical: true)

                if issue["suppression"]["suppressed"].bool == true {
                    Banner(kind: .caution, title: "Suppressed",
                           message: issue["suppression"]["reason"].display("no reason given")
                             + (issue["suppression"]["expiry"].string.map {
                                 " (expires \($0))" } ?? ""))
                }

                Panel(title: "Classification") {
                    VStack(alignment: .leading, spacing: 6) {
                        Field(label: "detector") {
                            Text("\(issue["rule_id"].text) v\(issue["rule_version"].text)")
                                .font(Term.font(12))
                        }
                        Field(label: "severity") {
                            VStack(alignment: .leading, spacing: 2) {
                                Chip(text: issue["severity"].text,
                                     tone: StatusTone.severity(issue["severity"].text))
                                Text(issue["severity_rationale"].text)
                                    .font(Term.small).foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                        Field(label: "detection") {
                            Chip(text: issue["detection_status"].text,
                                 tone: StatusTone.detection(
                                    issue["detection_status"].text))
                        }
                        Field(label: "cause") {
                            Chip(text: issue["cause_status"].text,
                                 tone: issue["cause_status"].text == "unknown"
                                       ? .neutral : .caution)
                        }
                        Field(label: "interval") {
                            Text("\(formatNs(issue["start_ns"].double ?? 0)) → "
                                 + "\(formatNs(issue["end_ns"].double ?? 0))  ("
                                 + formatNs((issue["end_ns"].double ?? 0)
                                            - (issue["start_ns"].double ?? 0)) + ")")
                                .font(Term.small)
                        }
                        Field(label: "screen") {
                            // Null means no marker covered the interval. It is
                            // never guessed from a function name.
                            Text(issue["screen"].display("not observed"))
                                .foregroundStyle(issue["screen"].isNull
                                                 ? .secondary : .primary)
                        }
                        Field(label: "occurrences") {
                            Text(issue["occurrence_count"].display())
                        }
                        Field(label: "symbols") { Text(issue["symbol_status"].display()) }
                        Field(label: "fingerprint") {
                            Text(issue["fingerprint"].text)
                                .font(Term.font(11))
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                if let basis = issue["confidence_basis"].string, !basis.isEmpty {
                    Panel(title: "What the evidence supports") {
                        Text(basis).font(Term.body)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }

                let metrics = issue["metrics"].array
                if !metrics.isEmpty {
                    Panel(title: "Metrics") {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(Array(metrics.enumerated()), id: \.offset) { _, m in
                                MetricRow(metric: m)
                            }
                        }
                    }
                }

                if let th = issue["threshold_expression"].string, !th.isEmpty {
                    Panel(title: "Threshold") {
                        VStack(alignment: .leading, spacing: 3) {
                            Text(th).font(Term.font(12))
                            Text("origin: \(issue["threshold_origin"].text)")
                                .font(Term.small).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }

                Panel(title: "What is still unknown") {
                    VStack(alignment: .leading, spacing: 10) {
                        BulletList(title: "Missing evidence",
                                   items: issue["missing_evidence"].array
                                    .compactMap { $0.string })
                        BulletList(title: "Alternative explanations",
                                   items: issue["alternative_explanations"].array
                                    .compactMap { $0.string })
                        BulletList(title: "Suggested verification",
                                   items: issue["suggested_verification"].array
                                    .compactMap { $0.string })
                        BulletList(title: "Proposed remediation",
                                   items: issue["proposed_remediation"].array
                                    .compactMap { $0.string })
                    }
                }

                let stacks = issue["candidate_stacks"].array
                if !stacks.isEmpty {
                    Panel(title: "Candidate stacks") {
                        VStack(alignment: .leading, spacing: 10) {
                            ForEach(Array(stacks.enumerated()), id: \.offset) { _, s in
                                StackView(stack: s)
                            }
                        }
                    }
                }

                let evidence = issue["evidence_refs"].array
                if !evidence.isEmpty {
                    Panel(title: "Evidence (\(evidence.count))") {
                        VStack(alignment: .leading, spacing: 5) {
                            ForEach(Array(evidence.prefix(20).enumerated()),
                                    id: \.offset) { _, e in
                                HStack(alignment: .top, spacing: 7) {
                                    Chip(text: e["kind"].text)
                                    VStack(alignment: .leading, spacing: 1) {
                                        Text(e["id"].text)
                                            .font(Term.font(11))
                                        if let n = e["note"].string, !n.isEmpty {
                                            Text(n).font(Term.small)
                                                .foregroundStyle(.secondary)
                                        }
                                    }
                                    Spacer()
                                    if e["synthetic"].bool == true {
                                        Chip(text: "synthetic", tone: .caution)
                                    }
                                }
                            }
                        }
                    }
                }
            }
            .padding(14)
        }
    }
}

private struct MetricRow: View {
    let metric: JSON

    private var valueText: String {
        guard let v = metric["value"].double else { return "not measured" }
        switch metric["unit"].text {
        case "ns": return formatNs(v)
        case "bytes": return formatBytes(v)
        case "fraction": return String(format: "%.1f%%", v * 100)
        case "count": return String(Int(v))
        default: return "\(metric["value"].display()) \(metric["unit"].text)"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 7) {
                Text(metric["name"].text)
                    .font(Term.font(12))
                Spacer()
                // A metric the collector never produced reads as "not
                // measured", never as zero.
                Text(valueText)
                    .font(Term.font(12, .medium))
                    .foregroundStyle(metric["value"].isNull ? .secondary : .primary)
                Chip(text: metric["method"].text)
            }
            let limits = metric["limitations"].array.compactMap { $0.string }
            if !limits.isEmpty {
                ForEach(Array(limits.enumerated()), id: \.offset) { _, l in
                    Text("· \(l)").font(Term.small).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
}

private struct StackView: View {
    let stack: JSON

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 6) {
                if let share = stack["sample_share"].double {
                    Text(String(format: "%.1f%%", share * 100))
                        .font(Term.font(12, .semibold))
                }
                // Whether a share may be summed with others is a property of
                // the measurement, so it is stated rather than implied.
                Chip(text: stack["inclusive"].bool == true
                     ? "inclusive — not summable as a disjoint cost"
                     : "self time",
                     tone: stack["inclusive"].bool == true ? .caution : .neutral)
            }
            let frames = stack["frames"].array
            let locations = stack["locations"].array
            ForEach(Array(frames.enumerated()), id: \.offset) { idx, f in
                let loc = idx < locations.count ? locations[idx] : JSON.null
                VStack(alignment: .leading, spacing: 1) {
                    HStack(spacing: 6) {
                        Text(f.text).font(Term.font(11))
                        if !loc.isNull {
                            Chip(text: loc["symbol_status"].text,
                                 tone: loc["safe_to_open"].bool == true
                                       ? .good : .caution)
                        }
                    }
                    if let file = loc["file"].string, !file.isEmpty {
                        Text("\(file)\(loc["line"].int.map { ":\($0)" } ?? "")")
                            .font(Term.small).foregroundStyle(.secondary)
                    }
                    // A non-exact match explains itself, because the editor
                    // must not navigate on it.
                    if loc["safe_to_open"].bool == false,
                       let note = loc["note"].string, !note.isEmpty {
                        Text(note).font(Term.small).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.leading, CGFloat(idx) * 9)
            }
        }
        .padding(9)
        .termCard()
    }
}
