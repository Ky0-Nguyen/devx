import SwiftUI

// MARK: - Record

struct RecordView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Live capture is implemented for Android, using the platform's own "
                     + "text interfaces: dumpsys gfxinfo framestats for frames, simpleperf "
                     + "for CPU stacks, dumpsys meminfo for memory.\n\n"
                     + "For iOS the collector IS wired up and a batch record uses it, but "
                     + "it cannot stream: `xctrace record` produces a trace bundle when it "
                     + "finishes rather than events that can be read while it runs. On "
                     + "this host it attaches and then never finishes, which it reports as "
                     + "a provider failure rather than writing a capture-shaped session "
                     + "with nothing measured in it.")
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Panel(title: "Target") {
                    VStack(alignment: .leading, spacing: 8) {
                        Field(label: "device") {
                            Text(state.selectedDevice.isEmpty
                                 ? "none selected" : state.selectedDevice)
                                .font(Term.font(12))
                        }
                        Field(label: "app") {
                            TextField("package name or bundle id",
                                      text: $state.selectedApp)
                                .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                        }
                    }
                }

                Panel(title: "Capture configuration",
                      subtitle: "Recorded with the session, because two runs are only "
                              + "comparable when they used the same collector settings.") {
                    VStack(alignment: .leading, spacing: 8) {
                        Field(label: "duration") {
                            Stepper("\(state.recordDuration) s",
                                    value: $state.recordDuration, in: 1...120)
                                .frame(maxWidth: 160)
                        }
                        Field(label: "sampling") {
                            Stepper("\(state.recordHz) Hz",
                                    value: $state.recordHz, in: 50...2000, step: 50)
                                .frame(maxWidth: 160)
                        }
                        Field(label: "sources") {
                            HStack(spacing: 14) {
                                Toggle("frames", isOn: $state.recordFrames)
                                Toggle("cpu", isOn: $state.recordCpu)
                                Toggle("memory", isOn: $state.recordMemory)
                            }
                        }
                        Field(label: "frame history") {
                            Toggle("reset before capture", isOn: $state.recordResetFrames)
                        }
                    }
                }

                HeavierCollectors()

                if state.recordResetFrames {
                    Banner(kind: .info, title: nil,
                           message: "framestats is a ring buffer of about the last 120 "
                                  + "frames and does not drain when read, so the history "
                                  + "is cleared at capture start to keep the window to "
                                  + "this capture's frames. Turn the reset off to analyse "
                                  + "frames the app produced before you pressed Record.")
                }

                HStack {
                    Button {
                        state.startRecord()
                    } label: {
                        Text("record")
                    }
                    .buttonStyle(TermButtonStyle(filled: true))
                    .disabled(state.busy != nil || state.selectedDevice.isEmpty
                              || state.selectedApp.isEmpty)

                    if state.busy != nil {
                        Button("cancel") { state.cancel() }
                        ProgressView().controlSize(.small)
                    }
                }

                if !state.recordDoc.isNull { RecordResult(doc: state.recordDoc) }
            }
            .padding(16)
        }
        .navigationTitle("~/record")
    }
}

private struct RecordResult: View {
    let doc: JSON
    @EnvironmentObject var state: AppState

    var body: some View {
        let sources = doc["source_results"].array
        let wrote = doc["session_written"].bool == true
        let unsupported = doc["unsupported"].bool == true

        VStack(alignment: .leading, spacing: 12) {
            if unsupported {
                Banner(kind: .caution, title: "Live capture not implemented for this platform",
                       message: doc["error"].text)
            } else if wrote {
                Banner(kind: .info, title: "Session written",
                       message: "\(doc["frames"].display("0")) frame(s), "
                              + "\(doc["cpu_samples"].display("0")) sample(s), "
                              + "\(doc["counters"].display("0")) counter series, "
                              + "\(doc["issues"].display("0")) issue(s) — in "
                              + "\(doc["elapsed_ms"].display("?")) ms.")
            } else if let err = doc["error"].string, !err.isEmpty {
                Banner(kind: .bad, title: "No session was written", message: err)
            }

            if !sources.isEmpty {
                Panel(title: "Capture sources",
                      subtitle: "Each source is independent: one failing does not stop "
                              + "the others, and a source that did not run becomes a "
                              + "coverage gap rather than an absence of events.") {
                    VStack(alignment: .leading, spacing: 9) {
                        ForEach(Array(sources.enumerated()), id: \.offset) { _, s in
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 7) {
                                    Text(s["id"].text)
                                        .font(Term.font(12))
                                    Spacer()
                                    Chip(text: s["status"].text,
                                         tone: StatusTone.capability(s["status"].text))
                                }
                                if let e = s["evidence"].string, !e.isEmpty {
                                    Text(e).font(Term.small).foregroundStyle(.secondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                                BulletList(title: "",
                                           items: s["limitations"].array
                                            .compactMap { $0.string })
                                if let fix = s["recovery_action"].string, !fix.isEmpty {
                                    Text("fix: \(fix)").font(Term.small)
                                        .foregroundStyle(StatusTone.caution.color)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }
                            .padding(9)
                            .termCard()
                        }
                    }
                }
            }

            if wrote, let id = doc["session_id"].string {
                Button { state.openSession(id) } label: {
                    Label(tr("Open issues"), systemImage: "arrow.right.circle")
                }
                .buttonStyle(TermButtonStyle(filled: true))
            }
        }
    }
}

// MARK: - Sessions

struct SessionsView: View {
    @EnvironmentObject var state: AppState

    @ViewBuilder private func sessionRow(_ s: JSON) -> some View {
        let id = s["session_id"].text
        let failures = s["checksum_failures"].array.count
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 5) {
                Text(id).font(Term.font(12))
                HStack(spacing: 6) {
                    Chip(text: s["state"].text,
                         tone: s["state"].text == "completed" ? .good : .caution)
                    // A synthetic session must be visible as such in the list,
                    // not only once it is opened.
                    if s["synthetic"].bool == true {
                        Chip(text: "synthetic", tone: .caution)
                    } else if s["synthetic"].isNull {
                        Chip(text: "kind unknown", tone: .caution)
                    } else {
                        Chip(text: "real capture", tone: .good)
                    }
                    Chip(text: failures == 0
                         ? "checksums verified"
                         : "\(failures) checksum mismatch",
                         tone: failures == 0 ? .good : .bad)
                }
                if let created = s["created_at"].string, !created.isEmpty {
                    Text(created).font(Term.small).foregroundStyle(Term.dim)
                }
                if let err = s["error"].string, !err.isEmpty {
                    Text(err).font(Term.small).foregroundStyle(StatusTone.bad.color)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 0)
            Button("open") { state.openSession(id) }
                .buttonStyle(TermButtonStyle())
        }
        .padding(11)
        .termCard()
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                VStack(alignment: .leading, spacing: 6) {
                Text(tr("Captures and imports on this machine. A session built from an "
                     + "import is labelled as one, and so is a session built from "
                     + "synthetic fixture data."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(state.sessionsDir)
                    .font(Term.font(11))
                    .foregroundStyle(Term.dim.opacity(0.8))
                }
                Rectangle().fill(Term.line).frame(height: 1)

                if state.sessions.isEmpty {
                    TermEmpty(title: "no sessions yet",
                              detail: "Record one, or import a trace with the CLI.",
                              hint: "mpi record --device <id> --app <identifier>")
                } else {
                    // Cards rather than a `List`: a List here drew nothing at
                    // all and took the sidebar down with it.
                    VStack(spacing: 8) {
                        ForEach(Array(state.sessions.enumerated()), id: \.offset) { _, s in
                            sessionRow(s)
                        }
                    }
                }
            }
            .padding(14)
        }
        // The ScrollView is the root of the pane, as in every other tab. When
        // it was the last child of a VStack instead, it reported its content's
        // height as the pane's -- 1975 points inside an 880-point window --
        // which stretched the whole NavigationSplitView past the window and
        // left both columns looking blank.
        .navigationTitle("~/sessions")
        .toolbar {
            Button { state.loadSessions() } label: {
                Label(tr("Refresh"), systemImage: "arrow.clockwise")
            }
        }
    }
}

// MARK: - Detectors

struct DetectorsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(tr("All twelve detectors from the specification catalog are registered, "
                     + "including the ones not implemented yet — a detector the engine has "
                     + "never heard of could not be reported as skipped, and then "
                     + "\"no findings\" would be indistinguishable from \"no analysis\"."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                ForEach(Array(state.rulesDoc["rules"].array.enumerated()),
                        id: \.offset) { _, r in
                    DetectorCard(rule: r)
                }
            }
            .padding(16)
        }
        .navigationTitle("~/detectors")
        .onAppear { state.loadRules() }
    }
}

private struct DetectorCard: View {
    let rule: JSON

    var body: some View {
        let implemented = rule["rule_version"].text != "0"
        Panel(title: "\(rule["rule_id"].text) — \(rule["title"].text)") {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 6) {
                    Chip(text: implemented ? "implemented"
                                           : "registered, not implemented",
                         tone: implemented ? .good : .neutral)
                    Chip(text: rule["delivery_phase"].text)
                    Chip(text: rule["category"].text)
                }
                BulletList(title: "Prerequisites",
                           items: rule["prerequisites"].array.map {
                               "\($0["id"].text) — \($0["description"].text)" })

                let thresholds = rule["thresholds"].array
                if !thresholds.isEmpty {
                    VStack(alignment: .leading, spacing: 5) {
                        Text("Thresholds").font(Term.font(11, .semibold))
                            .foregroundStyle(.secondary)
                        ForEach(Array(thresholds.enumerated()), id: \.offset) { _, t in
                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 6) {
                                    Text("\(t["name"].text) = "
                                         + "\(t["value"].display()) \(t["unit"].text)")
                                        .font(Term.font(12))
                                    // A heuristic must never read as a platform
                                    // standard, so the origin is shown as a chip.
                                    Chip(text: t["origin"].text,
                                         tone: t["is_platform_standard"].bool == true
                                               ? .good : .caution)
                                }
                                Text(t["rationale"].text).font(Term.small)
                                    .foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                }

                BulletList(title: "Known false positives",
                           items: rule["known_false_positives"].array
                            .compactMap { $0.string })
            }
        }
    }
}

/// The collectors that cost something, and what they cost.
///
/// Spec section 13: "Collect-missing-evidence action explains overhead before
/// enabling heavier collectors." The explanation is not a tooltip and it is
/// not behind a disclosure triangle -- it sits between the switch and the
/// reader, above the switch, because an overhead note nobody reads is not an
/// explanation. Each one names the evidence it buys, so the trade is legible
/// rather than being a switch labelled with a flag name.
struct HeavierCollectors: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Panel(title: "Heavier collectors",
              subtitle: "off by default; each one costs something on the "
                      + "device and says what") {
            VStack(alignment: .leading, spacing: 12) {
                row(title: "Scheduling and I/O  (atrace)",
                    buys: "DET-03 (synchronous main-thread I/O) and DET-09 "
                        + "(wait contention). Without it both report that the "
                        + "provider did not run, which is not the same as "
                        + "finding nothing.",
                    costs: "traces the WHOLE DEVICE, not just this app, for "
                         + "the capture's duration. Costs CPU at every context "
                         + "switch across every process, and its kernel ring "
                         + "buffer can overflow -- dropped events are reported "
                         + "as a coverage gap rather than absorbed.",
                    isOn: $state.recordScheduling)
                row(title: "Heap dump  (am dumpheap)",
                    buys: "DET-06 (retained-object investigation): the chain "
                        + "of references that keeps an object alive, which no "
                        + "memory counter can give you -- a counter says how "
                        + "much is held, never by what.",
                    costs: "PAUSES THE APP while the runtime walks the whole "
                         + "heap, and writes tens of megabytes into the "
                         + "session (49 MB for a React Native app on an "
                         + "emulator). Taken after every other source, so the "
                         + "pause falls outside the measured window.",
                    isOn: $state.recordHeap)
            }
        }
    }

    private func row(title: String, buys: String, costs: String,
                     isOn: Binding<Bool>) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(Term.font(12, .medium)).foregroundStyle(Term.ink)
            HStack(alignment: .top, spacing: 6) {
                Text("buys")
                    .font(Term.font(10)).foregroundStyle(Term.green)
                    .frame(width: 34, alignment: .trailing)
                Text(buys)
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(alignment: .top, spacing: 6) {
                Text("costs")
                    .font(Term.font(10)).foregroundStyle(Term.amber)
                    .frame(width: 34, alignment: .trailing)
                Text(costs)
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            // The switch last: the cost is read on the way to it.
            Toggle("enable", isOn: isOn)
                .font(Term.font(11))
                .padding(.leading, 40)
        }
    }
}
