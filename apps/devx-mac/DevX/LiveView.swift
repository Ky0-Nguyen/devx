import SwiftUI

/// The live capture view.
///
/// This is the one screen where the specification's caution about live results
/// has to be visible at all times, not just once: findings over a window that
/// is still open are preliminary by definition (spec sections 2.2 and 13). So
/// the banner stays up for the whole session, the issue list is titled
/// "preliminary", and the counts carry the tick cost that produced them.
struct LiveView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                header
                if state.liveRunning || !state.liveSnapshot.isNull {
                    if state.liveRunning {
                        Banner(kind: .caution, title: "Preliminary",
                               message: "The capture window is still open. A detector that "
                                      + "has found nothing yet may still fire, and a "
                                      + "finding may change as more evidence arrives. "
                                      + "Nothing below is a final result.")
                    }
                    liveCounters
                    sparklines
                    sourceStatus
                    preliminaryIssues
                }
                if let stop = state.liveStopResult, !stop.isNull {
                    stopSummary(stop)
                }
            }
            .padding(16)
        }
        .navigationTitle("~/live")
    }

    private var snap: JSON { state.liveSnapshot }

    private var header: some View {
        Panel(title: "Live capture",
              subtitle: "Frames and memory stream at the tick cadence. CPU sampling runs "
                      + "on its own thread in longer windows, because simpleperf costs "
                      + "about 5.6 s per record-and-symbolise cycle — so CPU numbers lag, "
                      + "and the intervals between windows are recorded as coverage gaps "
                      + "rather than as measured idle time.") {
            VStack(alignment: .leading, spacing: 9) {
                Field(label: "device") {
                    Text(state.selectedDevice.isEmpty ? "none selected" : state.selectedDevice)
                        .font(Term.font(12))
                }
                Field(label: "app") {
                    TextField("package name or bundle id", text: $state.selectedApp)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                        .disabled(state.liveRunning)
                }
                Field(label: "tick") {
                    Stepper("\(state.liveTickMs) ms", value: $state.liveTickMs,
                            in: 200...5000, step: 100)
                        .frame(maxWidth: 170).disabled(state.liveRunning)
                }
                Field(label: "cpu window") {
                    Stepper("\(state.liveCpuWindowMs / 1000) s",
                            value: $state.liveCpuWindowMs, in: 1000...30000, step: 1000)
                        .frame(maxWidth: 170).disabled(state.liveRunning)
                }
                Field(label: "stacks") {
                    HStack(spacing: 8) {
                        Toggle(tr("profile stacks at the end"), isOn: Binding(
                            get: { state.liveStackProfileSeconds > 0 },
                            set: { on in
                                state.liveStackProfileSeconds = on ? 3 : 0
                            }))
                            .toggleStyle(.checkbox)
                        if state.liveStackProfileSeconds > 0 {
                            Stepper(value: $state.liveStackProfileSeconds,
                                    in: 1...30) {
                                Text("\(state.liveStackProfileSeconds) s")
                                    .font(Term.body)
                            }
                            .frame(width: 110)
                        }
                    }
                    .disabled(state.liveRunning)
                }
                if state.liveStackProfileSeconds > 0 {
                    // Both facts matter, and the second more: this is an
                    // aggregate with no timestamps, so it says where the
                    // samples were and never when.
                    Text(tr("iOS simulator only. It runs `sample` for that "
                          + "long when you stop, so the stop waits — and what "
                          + "it returns is an aggregate with no timestamps: "
                          + "where the samples were, never when."))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .padding(.leading, 138)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Field(label: "sources") {
                    HStack(spacing: 14) {
                        Toggle("frames", isOn: $state.recordFrames)
                        Toggle("cpu", isOn: $state.recordCpu)
                        Toggle("memory", isOn: $state.recordMemory)
                    }
                    .disabled(state.liveRunning)
                }
                HStack(spacing: 10) {
                    if state.liveRunning {
                        Button {
                            state.stopLive()
                        } label: {
                            Text(tr("stop and save"))
                        }
                        .buttonStyle(TermButtonStyle(tone: Term.red, filled: true))
                        AsciiSpinner()
                        Text("streaming").font(Term.body)
                            .foregroundStyle(Term.green)
                        BlinkingCursor()
                    } else if state.liveStarting {
                        AsciiSpinner(color: Term.amber)
                        Text(tr("resolving the target…")).font(Term.body)
                            .foregroundStyle(Term.dim)
                    } else if state.liveStopping {
                        AsciiSpinner(color: Term.amber)
                        Text(tr("stopping and saving…")).font(Term.body)
                            .foregroundStyle(Term.dim)
                    } else {
                        Button {
                            state.startLive()
                        } label: {
                            Text(tr("start live capture"))
                        }
                        .buttonStyle(TermButtonStyle(filled: true))
                        .disabled(state.selectedDevice.isEmpty || state.selectedApp.isEmpty)
                    }
                }
            }
        }
    }

    private var liveCounters: some View {
        Panel(title: "Arriving now") {
            HStack(alignment: .top, spacing: 22) {
                Counter(label: "frames", value: snap["counts"]["frames"].display("0"))
                Counter(label: "cpu samples",
                        value: snap["counts"]["cpu_samples"].display("0"))
                Counter(label: "counter points",
                        value: snap["counts"]["counter_points"].display("0"))
                Counter(label: "threads", value: snap["counts"]["threads"].display("0"))
                Divider().frame(height: 40)
                Counter(label: "elapsed",
                        value: String(format: "%.1fs",
                                      (snap["elapsed_ms"].double ?? 0) / 1000))
                Counter(label: "ticks", value: snap["ticks"].display("0"))
                // The collector's own cost, shown next to the numbers it
                // produced rather than buried in a report.
                Counter(label: "last tick",
                        value: "\(snap["last_tick_cost_ms"].display("0")) ms",
                        tone: (snap["last_tick_cost_ms"].double ?? 0) >
                              Double(state.liveTickMs) ? .caution : .neutral)
                Spacer()
            }
        }
    }

    /// One panel per measured family, never one list under a single heading.
    ///
    /// This was a single panel titled Memory holding every counter, so CPU
    /// process time appeared as a memory family under a subtitle about not
    /// summing memory families. The collector keeps them apart deliberately
    /// -- "CPU time is not memory and must never be totalled with one" -- and
    /// the view now shows that separation instead of collapsing it.
    /// The counters, read off the wire as rows.
    private var counterRows: [CounterFormat.Row] {
        snap["latest_counters"].array.map { c in
            CounterFormat.Row(
                name: c["name"].text,
                value: c["value"].double ?? 0,
                unit: c["unit"].text,
                cumulative: c["cumulative"].bool ?? false,
                normalization: c["cpu_normalization"].text,
                delta: c["delta"].double,
                deltaSpanNs: c["delta_span_ns"].double.map { Int64($0) })
        }
    }

    /// One panel per measured family, never one list under a single heading.
    ///
    /// This was a single panel titled Memory holding every counter, so CPU
    /// process time appeared as a memory family under a subtitle about not
    /// summing memory families. The collector keeps them apart deliberately
    /// -- "CPU time is not memory and must never be totalled with one" -- and
    /// the view now shows that separation instead of collapsing it.
    @ViewBuilder private var sparklines: some View {
        let panels = CounterFormat.panels(counterRows)
        ForEach(Array(panels.enumerated()), id: \.offset) { _, panel in
            Panel(title: panel.family.title, subtitle: panel.family.subtitle) {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(panel.rows, id: \.name) { row in
                        HStack(spacing: 10) {
                            // Truncated rather than wrapped: a label that
                            // wrapped mid-word read as "cpu.process_user_ti"
                            // over "me_ns", which looks like two counters.
                            Text(CounterFormat.label(row.name))
                                .font(Term.font(11))
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .frame(width: 148, alignment: .leading)
                            Sparkline(values: state.liveSeries[row.name] ?? [])
                                .frame(height: 26)
                            // Leading with what this capture cost, because
                            // the absolute of a cumulative counter is the
                            // process's whole life: a 67 s window opened at
                            // 3.18 h of CPU time, true and not about the
                            // window. The total stays underneath.
                            VStack(alignment: .trailing, spacing: 1) {
                                Text(row.headline)
                                    .font(Term.font(12, .medium))
                                    .lineLimit(1)
                                if let footnote = row.footnote {
                                    Text(footnote)
                                        .font(Term.small)
                                        .foregroundStyle(Term.dim)
                                        .lineLimit(1)
                                }
                            }
                            .frame(width: 116, alignment: .trailing)
                        }
                    }
                }
            }
        }
    }

    private var sourceStatus: some View {
        Panel(title: "Sources") {
            VStack(alignment: .leading, spacing: 7) {
                ForEach(Array(snap["source_status"].array.enumerated()),
                        id: \.offset) { _, s in
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 7) {
                            Text(s["id"].text)
                                .font(Term.font(11))
                            Spacer()
                            Chip(text: s["status"].text,
                                 tone: StatusTone.capability(s["status"].text))
                        }
                        if let e = s["evidence"].string, !e.isEmpty {
                            Text(e).font(Term.small).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        BulletList(title: "",
                                   items: s["limitations"].array.compactMap { $0.string })
                        if let fix = s["recovery_action"].string, !fix.isEmpty {
                            Text("fix: \(fix)").font(Term.small)
                                .foregroundStyle(StatusTone.caution.color)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(8)
                    .termCard()
                }
                let notes = snap["notes"].array.compactMap { $0.string }
                if !notes.isEmpty { BulletList(title: "Notes", items: notes) }
            }
        }
    }

    private var preliminaryIssues: some View {
        let analysis = snap["preliminary_analysis"]
        let issues = analysis["issues"].array
        let runs = analysis["rule_runs"].array
        let ran = runs.filter { $0["outcome"].text != "skipped" }.count
        return Panel(title: state.liveRunning
                     ? "Preliminary findings (\(issues.count))"
                     : "Findings (\(issues.count))",
                     subtitle: state.liveRunning
                     ? "Recomputed as evidence arrives. \(ran) detector(s) can run so far."
                     : "Final: computed over the closed window.") {
            VStack(alignment: .leading, spacing: 6) {
                if issues.isEmpty {
                    Text(state.liveRunning
                         ? tr("No detector that can run has found anything yet. That is not "
                           + "the same as nothing being wrong: several detectors cannot "
                           + "run until more evidence arrives.")
                         : "No detector that ran produced a finding.")
                        .font(Term.body).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                // List on the left, the selected finding on the right.
                // Both columns are capped and scroll inside themselves, so a
                // capture that finds twenty things does not push the rest of
                // the tab off the screen -- the same treatment the Inspect
                // tab's request list has.
                //
                // Not an HSplitView: this sits inside the page's own
                // ScrollView, and a draggable split there has no height of
                // its own to divide. A fixed pair of columns does.
                let pane = FindingSelection.resolve(
                    selectedId: state.selectedLiveIssueId, findings: issues)
                HStack(alignment: .top, spacing: 10) {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(Array(issues.enumerated()),
                                    id: \.offset) { _, i in
                                IssueListRow(
                                    issue: i,
                                    selected: FindingSelection.id(of: i)
                                        == state.selectedLiveIssueId)
                                    .contentShape(Rectangle())
                                    .onTapGesture {
                                        // A finding with no fingerprint is
                                        // not selectable: an index fallback
                                        // would point at a different one
                                        // after the next recomputation.
                                        guard FindingSelection.isSelectable(i)
                                        else { return }
                                        state.selectedLiveIssueId =
                                            FindingSelection.id(of: i)
                                    }
                            }
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: 320)

                    Group {
                        switch pane {
                        case .noFindings:
                            EmptyView()
                        case .nothingSelected:
                            TermEmpty(
                                title: "no finding selected",
                                detail: tr("Pick one on the left to read its "
                                         + "evidence, thresholds and what is "
                                         + "still missing."),
                                hint: "")
                        case .gone:
                            TermEmpty(
                                title: "that finding is no longer in the list",
                                detail: tr("Findings are recomputed as "
                                         + "evidence arrives, and this one "
                                         + "stopped firing. That is a result, "
                                         + "not a lost selection."),
                                hint: "")
                        case .finding(let f):
                            // The same detail the Issues tab shows, minus the
                            // timeline jump: there is no saved session to
                            // open while the capture is still running.
                            IssueDetail(issue: f, canFocusTimeline: false)
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: 320)
                }
            }
        }
    }

    private func stopSummary(_ stop: JSON) -> some View {
        let wrote = stop["session_written"].bool == true
        return VStack(alignment: .leading, spacing: 10) {
            if wrote {
                Banner(kind: .info, title: "Session written",
                       message: "\(stop["frames"].display("0")) frame(s), "
                              + "\(stop["cpu_samples"].display("0")) sample(s), "
                              + "\(stop["counter_points"].display("0")) counter point(s), "
                              + "\(stop["issues"].display("0")) issue(s). The analysis in "
                              + "the saved session is final, not preliminary.")
                if let id = stop["session_id"].string {
                    Button { state.openSession(id) } label: {
                        Label(tr("Open in Issues"), systemImage: "arrow.right.circle")
                    }
                    .buttonStyle(TermButtonStyle(filled: true))
                }
            } else if let err = stop["error"].string, !err.isEmpty {
                Banner(kind: .bad, title: "No session was written", message: err)
            }
        }
    }
}

private struct Counter: View {
    let label: String
    let value: String
    var tone: StatusTone = .neutral
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            // No `monospacedDigit()`: every digit in this face is already
            // the same width, so a counter cannot jitter as it climbs.
            Text(value).font(Term.display)
                .foregroundStyle(tone == .neutral ? Color.primary : tone.color)
            Text(label).font(Term.small).foregroundStyle(.secondary)
        }
    }
}

/// A minimal sparkline. Its own shape rather than a chart dependency: this is
/// one polyline, and ADR-0002 keeps the licence inventory empty.
private struct Sparkline: View {
    let values: [Double]

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            if values.count < 2 {
                // One point is not a trend, so nothing is drawn that might
                // suggest one.
                Text(values.isEmpty ? "" : "collecting…")
                    .font(Term.micro).foregroundStyle(Term.dim.opacity(0.8))
                    .frame(width: w, height: h, alignment: .leading)
            } else {
                let lo = values.min() ?? 0
                let hi = values.max() ?? 1
                let span = hi - lo
                Path { p in
                    for (i, v) in values.enumerated() {
                        let x = w * Double(i) / Double(values.count - 1)
                        // A flat series draws through the middle rather than
                        // being scaled up into a fake wiggle.
                        let norm = span > 0 ? (v - lo) / span : 0.5
                        let y = h - (h * norm)
                        if i == 0 { p.move(to: CGPoint(x: x, y: y)) }
                        else { p.addLine(to: CGPoint(x: x, y: y)) }
                    }
                }
                .stroke(Term.green, style: StrokeStyle(lineWidth: 1.5,
                                                              lineJoin: .round))
                .background(alignment: .bottomLeading) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Term.raised)
                }
            }
        }
    }
}
