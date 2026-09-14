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
        .navigationTitle("Live")
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
                        .font(.system(size: 12, design: .monospaced))
                }
                Field(label: "app") {
                    TextField("package name or bundle id", text: $state.selectedApp)
                        .textFieldStyle(.roundedBorder).frame(maxWidth: 320)
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
                            Label("Stop and save", systemImage: "stop.circle.fill")
                        }
                        .buttonStyle(.borderedProminent).tint(.red)
                        ProgressView().controlSize(.small)
                        Text("streaming…").font(.callout).foregroundStyle(.secondary)
                    } else if state.liveStarting {
                        ProgressView().controlSize(.small)
                        Text("resolving the target…").font(.callout)
                            .foregroundStyle(.secondary)
                    } else if state.liveStopping {
                        ProgressView().controlSize(.small)
                        Text("stopping and saving…").font(.callout)
                            .foregroundStyle(.secondary)
                    } else {
                        Button {
                            state.startLive()
                        } label: {
                            Label("Start live capture", systemImage: "dot.radiowaves.left.and.right")
                        }
                        .buttonStyle(.borderedProminent)
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

    @ViewBuilder private var sparklines: some View {
        let counters = snap["latest_counters"].array
        if !counters.isEmpty {
            Panel(title: "Memory",
                  subtitle: "Each family is a separate measurement and is never summed "
                          + "with another.") {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(Array(counters.enumerated()), id: \.offset) { _, c in
                        let name = c["name"].text
                        let series = state.liveSeries[name] ?? []
                        HStack(spacing: 10) {
                            Text(name.replacingOccurrences(of: "memory.", with: "")
                                     .replacingOccurrences(of: "_bytes", with: ""))
                                .font(.system(size: 11, design: .monospaced))
                                .frame(width: 132, alignment: .leading)
                            Sparkline(values: series)
                                .frame(height: 26)
                            Text(formatBytes(c["value"].double ?? 0))
                                .font(.callout.weight(.medium))
                                .frame(width: 86, alignment: .trailing)
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
                                .font(.system(size: 11, design: .monospaced))
                            Spacer()
                            Chip(text: s["status"].text,
                                 tone: StatusTone.capability(s["status"].text))
                        }
                        if let e = s["evidence"].string, !e.isEmpty {
                            Text(e).font(.caption).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        BulletList(title: "",
                                   items: s["limitations"].array.compactMap { $0.string })
                        if let fix = s["recovery_action"].string, !fix.isEmpty {
                            Text("fix: \(fix)").font(.caption)
                                .foregroundStyle(StatusTone.caution.color)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(8)
                    .background(.quaternary.opacity(0.22),
                                in: RoundedRectangle(cornerRadius: 7))
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
                         ? "No detector that can run has found anything yet. That is not "
                           + "the same as nothing being wrong: several detectors cannot "
                           + "run until more evidence arrives."
                         : "No detector that ran produced a finding.")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(Array(issues.enumerated()), id: \.offset) { _, i in
                    HStack(alignment: .top, spacing: 8) {
                        Chip(text: i["severity"].text,
                             tone: StatusTone.severity(i["severity"].text))
                        VStack(alignment: .leading, spacing: 3) {
                            Text(i["title"].text).font(.callout)
                                .fixedSize(horizontal: false, vertical: true)
                            HStack(spacing: 5) {
                                Text(i["rule_id"].text)
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundStyle(.secondary)
                                Chip(text: i["detection_status"].text,
                                     tone: StatusTone.detection(i["detection_status"].text))
                                Chip(text: "cause: \(i["cause_status"].text)",
                                     tone: i["cause_status"].text == "unknown"
                                           ? .neutral : .caution)
                            }
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(7)
                    .background(.quaternary.opacity(0.2),
                                in: RoundedRectangle(cornerRadius: 7))
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
                        Label("Open in Issues", systemImage: "arrow.right.circle")
                    }
                    .buttonStyle(.borderedProminent)
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
            Text(value).font(.title2.weight(.semibold).monospacedDigit())
                .foregroundStyle(tone == .neutral ? Color.primary : tone.color)
            Text(label).font(.caption).foregroundStyle(.secondary)
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
                    .font(.caption2).foregroundStyle(.tertiary)
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
                .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 1.5,
                                                              lineJoin: .round))
                .background(alignment: .bottomLeading) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(.quaternary.opacity(0.25))
                }
            }
        }
    }
}
