import SwiftUI

// MARK: - Record

struct RecordView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Live capture is implemented for Android, using the platform's own "
                     + "text interfaces: dumpsys gfxinfo framestats for frames, simpleperf "
                     + "for CPU stacks, dumpsys meminfo for memory. For iOS the collector "
                     + "is not wired up yet, and DevX will say so rather than writing a "
                     + "capture-shaped session with nothing measured in it.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Panel(title: "Target") {
                    VStack(alignment: .leading, spacing: 8) {
                        Field(label: "device") {
                            Text(state.selectedDevice.isEmpty
                                 ? "none selected" : state.selectedDevice)
                                .font(.system(size: 12, design: .monospaced))
                        }
                        Field(label: "app") {
                            TextField("package name or bundle id",
                                      text: $state.selectedApp)
                                .textFieldStyle(.roundedBorder).frame(maxWidth: 320)
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

                if state.recordResetFrames {
                    Banner(kind: .info, title: nil,
                           message: "framestats drains when read, so the history is reset "
                                  + "at capture start and the window holds only this "
                                  + "capture's frames. Turn the reset off to analyse "
                                  + "frames the app produced before you pressed Record.")
                }

                HStack {
                    Button {
                        state.startRecord()
                    } label: {
                        Label("Record", systemImage: "record.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(state.busy != nil || state.selectedDevice.isEmpty
                              || state.selectedApp.isEmpty)

                    if state.busy != nil {
                        Button("Cancel") { state.cancel() }
                        ProgressView().controlSize(.small)
                    }
                }

                if !state.recordDoc.isNull { RecordResult(doc: state.recordDoc) }
            }
            .padding(16)
        }
        .navigationTitle("Record")
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
                                        .font(.system(size: 12, design: .monospaced))
                                    Spacer()
                                    Chip(text: s["status"].text,
                                         tone: StatusTone.capability(s["status"].text))
                                }
                                if let e = s["evidence"].string, !e.isEmpty {
                                    Text(e).font(.caption).foregroundStyle(.secondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                                BulletList(title: "",
                                           items: s["limitations"].array
                                            .compactMap { $0.string })
                                if let fix = s["recovery_action"].string, !fix.isEmpty {
                                    Text("fix: \(fix)").font(.caption)
                                        .foregroundStyle(StatusTone.caution.color)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }
                            .padding(9)
                            .background(.quaternary.opacity(0.25),
                                        in: RoundedRectangle(cornerRadius: 8))
                        }
                    }
                }
            }

            if wrote, let id = doc["session_id"].string {
                Button { state.openSession(id) } label: {
                    Label("Open issues", systemImage: "arrow.right.circle")
                }
                .buttonStyle(.borderedProminent)
            }
        }
    }
}

// MARK: - Sessions

struct SessionsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Captures and imports on this machine. A session built from an "
                     + "import is labelled as one, and so is a session built from "
                     + "synthetic fixture data.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(state.sessionsDir)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.tertiary)
            }
            .padding(16)
            Divider()

            if state.sessions.isEmpty {
                ContentUnavailableView("No sessions yet", systemImage: "folder",
                    description: Text("Record one, or import a trace with the CLI."))
            } else {
                List {
                    ForEach(Array(state.sessions.enumerated()), id: \.offset) { _, s in
                        let id = s["session_id"].text
                        let failures = s["checksum_failures"].array.count
                        HStack(spacing: 10) {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(id).font(.system(size: 12, design: .monospaced))
                                HStack(spacing: 6) {
                                    Chip(text: s["state"].text,
                                         tone: s["state"].text == "completed"
                                               ? .good : .caution)
                                    // A synthetic session must be visible as such
                                    // in the list, not only once it is opened.
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
                                    Text(created).font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                if let err = s["error"].string, !err.isEmpty {
                                    Text(err).font(.caption)
                                        .foregroundStyle(StatusTone.bad.color)
                                }
                            }
                            Spacer()
                            Button("Open") { state.openSession(id) }
                                .buttonStyle(.bordered)
                        }
                        .padding(.vertical, 3)
                    }
                }
                .listStyle(.inset)
            }
        }
        .navigationTitle("Sessions")
        .toolbar {
            Button { state.loadSessions() } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
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
                Text("All twelve detectors from the specification catalog are registered, "
                     + "including the ones not implemented yet — a detector the engine has "
                     + "never heard of could not be reported as skipped, and then "
                     + "\"no findings\" would be indistinguishable from \"no analysis\".")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                ForEach(Array(state.rulesDoc["rules"].array.enumerated()),
                        id: \.offset) { _, r in
                    DetectorCard(rule: r)
                }
            }
            .padding(16)
        }
        .navigationTitle("Detectors")
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
                        Text("Thresholds").font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                        ForEach(Array(thresholds.enumerated()), id: \.offset) { _, t in
                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 6) {
                                    Text("\(t["name"].text) = "
                                         + "\(t["value"].display()) \(t["unit"].text)")
                                        .font(.system(size: 12, design: .monospaced))
                                    // A heuristic must never read as a platform
                                    // standard, so the origin is shown as a chip.
                                    Chip(text: t["origin"].text,
                                         tone: t["is_platform_standard"].bool == true
                                               ? .good : .caution)
                                }
                                Text(t["rationale"].text).font(.caption)
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
