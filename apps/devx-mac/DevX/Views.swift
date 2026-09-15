import SwiftUI

// MARK: - Devices

struct DevicesView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Android and iOS are discovered together. A paired-but-unreachable "
                     + "device reads as offline — it is not absent, and it is not usable. "
                     + "Simulators and emulators are listed separately on purpose: their "
                     + "timings are never comparable to a physical device.")
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                if state.devicesDoc["enumeration_failed"].bool == true {
                    Banner(kind: .bad, title: "Device enumeration failed",
                           message: "That is a different answer from \"no devices are "
                                  + "connected\".")
                }

                DiscoveryAge()

                // Loaded on arrival even though the list starts collapsed:
                // the summary line is the discoverable part, and it needs the
                // counts.
                BootPanel()
                    .onAppear { state.loadBootTargetsIfNeeded() }

                if state.devices.isEmpty && !state.devicesDoc.isNull {
                    Banner(kind: .info, title: "No device discovered",
                           message: "Android: enable USB debugging and accept the "
                                  + "authorization prompt. iOS: unlock the device, trust "
                                  + "this computer, and enable Developer Mode.")
                }

                ForEach(Array(state.devices.enumerated()), id: \.offset) { _, d in
                    DeviceRow(device: d,
                              selected: d["device_id"].text == state.selectedDevice)
                        .onTapGesture {
                            if d["trust"].text == "authorized" {
                                state.selectedDevice = d["device_id"].text
                            }
                        }
                }

                let errors = state.devicesDoc["provider_errors"].array
                    .compactMap { $0.string }
                if !errors.isEmpty {
                    Panel(title: "Provider notes") {
                        BulletList(title: "", items: errors)
                    }
                }
            }
            .padding(16)
        }
        .navigationTitle("~/devices")
        // The watch runs only while this tab is on screen. Leaving it running
        // behind the other eleven tabs would spawn child processes nobody is
        // looking at.
        .onAppear { state.startDeviceWatch() }
        .onDisappear { state.stopDeviceWatch() }
        .toolbar {
            Toggle("watch", isOn: $state.watchDevices)
                .toggleStyle(.checkbox)
                .help("Re-scan every 5s while this tab is open")
            Toggle("simulators", isOn: $state.includeSimulators)
                .toggleStyle(.checkbox)
                .onChange(of: state.includeSimulators) { _, _ in state.loadDevices() }
            Button { state.refreshDeviceViews() } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
        }
    }
}

/// The age of the device list, stated where the list is read.
///
/// Deliberately above the device rows rather than in a corner: the question it
/// answers -- "is this current?" -- is asked at the moment of reading a row,
/// and an answer in the toolbar is an answer nobody sees.
private struct DiscoveryAge: View {
    @EnvironmentObject var state: AppState
    // Drives the countdown. Without it the text would be written once and
    // then quietly age on screen, which is the same bug one level up.
    @State private var now = Date()

    var body: some View {
        let s = DeviceFreshness.status(loadedAt: state.devicesLoadedAt,
                                       now: now,
                                       watching: state.watchDevices)
        HStack(spacing: 8) {
            // Cyan is this app's colour for "no claim", and a list that has
            // never been fetched is exactly that.
            Circle()
                .fill(colour(s.confidence))
                .frame(width: 5, height: 5)
            Text(s.text)
                .font(Term.small)
                .foregroundStyle(colour(s.confidence))
            Spacer(minLength: 0)
        }
        .onReceive(every: 1) { now = Date() }
    }

    private func colour(_ c: DiscoveryConfidence) -> Color {
        switch c {
        case .noClaim: return Term.cyan
        case .current: return Term.dim
        case .aging:   return Term.amber
        }
    }
}

private extension View {
    /// A ticking clock for views whose text is about elapsed time.
    func onReceive(every seconds: TimeInterval, _ action: @escaping () -> Void) -> some View {
        modifier(TickModifier(seconds: seconds, action: action))
    }
}

private struct TickModifier: ViewModifier {
    let seconds: TimeInterval
    let action: () -> Void
    func body(content: Content) -> some View {
        content.onAppear {
            // `.common` mode, so the label keeps counting while a scroll or a
            // menu is tracking.
            let t = Timer(timeInterval: seconds, repeats: true) { _ in action() }
            RunLoop.main.add(t, forMode: .common)
            timer = t
        }
        .onDisappear { timer?.invalidate(); timer = nil }
    }
    @State private var timer: Timer? = nil
}

private struct DeviceRow: View {
    let device: JSON
    let selected: Bool

    var body: some View {
        let trust = device["trust"].text
        let form = device["form"].text
        HStack(spacing: 12) {
            Image(systemName: form == "physical"
                  ? (device["platform"].text == "ios" ? "iphone" : "candybarphone")
                  : "macwindow.on.rectangle")
                .font(Term.display)
                .foregroundStyle(StatusTone.trust(trust).color)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(device["display_name"].display("unnamed device"))
                        .font(Term.font(12, .medium))
                    Chip(text: device["platform"].text)
                    Chip(text: form,
                         tone: form == "physical" ? .neutral : .caution)
                    Chip(text: trust, tone: StatusTone.trust(trust))
                }
                Text(device["device_id"].text)
                    .font(Term.font(11))
                    .foregroundStyle(.secondary)
                Text("OS \(device["os_version"].display("unknown"))  ·  "
                     + device["model"].display("unknown model"))
                    .font(Term.small).foregroundStyle(.secondary)
            }
            Spacer()
            if selected { Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.tint) }
        }
        .padding(11)
        .termCard(selected: selected)
        .contentShape(Rectangle())
    }
}

// MARK: - Apps

struct AppsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                VStack(alignment: .leading, spacing: 9) {
                // `Text` parses markdown only from a string literal. These
                // are joined with `+`, so the emphasis markers were printed
                // verbatim -- "**running** does not mean foreground" -- until
                // the result was wrapped back into a LocalizedStringKey.
                Text(.init("**running** does not mean foreground. **unknown** means the "
                     + "provider could not observe the state — it does not mean not "
                     + "running. Profiling availability is independent of runtime state, "
                     + "and entries that cannot be profiled are kept and marked rather "
                     + "than hidden."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    TextField("filter by name or identifier", text: $state.appFilter)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 280)
                    Toggle("running only", isOn: $state.runningOnly)
                        .toggleStyle(.checkbox)
                    Spacer()
                    Text("\(state.filteredApps.count) of \(state.apps.count)")
                        .font(Term.small).foregroundStyle(.secondary)
                }
                if state.appsDoc["enumeration_failed"].bool == true {
                    Banner(kind: .bad, title: "App enumeration failed for this device",
                           message: "That is not the same as the device having no apps.")
                }
                RecentTargetsPanel()
                }
                Rectangle().fill(Term.line).frame(height: 1)

                if state.selectedDevice.isEmpty {
                    TermEmpty(title: "no device selected",
                              detail: "Pick a usable device first.",
                              hint: "mpi devices")
                } else {
                    VStack(spacing: 8) {
                        ForEach(Array(state.filteredApps.enumerated()), id: \.offset) { _, a in
                            AppRow(app: a) {
                                state.selectedApp = a["application_key"]["app_identifier"].text
                            }
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
        .navigationTitle("~/apps")
        .toolbar {
            Button { state.loadApps() } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
        }
    }
}

private struct AppRow: View {
    let app: JSON
    let use: () -> Void

    var body: some View {
        let id = app["application_key"]["app_identifier"].text
        let name = app["display_name"].text
        let runtime = app["runtime_state"].text
        let profiling = app["profiling_availability"].text
        let scope = app["visibility_scope"].text

        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 7) {
                Text(id).font(Term.font(12))
                if !name.isEmpty && name != id {
                    Text(name).font(Term.small).foregroundStyle(.secondary)
                }
                Spacer()
                Button("use", action: use).buttonStyle(TermButtonStyle())
            }
            HStack(spacing: 6) {
                Chip(text: runtime, tone: StatusTone.runtime(runtime))
                Chip(text: profiling, tone: StatusTone.profiling(profiling))
                Chip(text: scope, tone: scope == "partial" ? .caution : .neutral)
                let procs = app["process_instances"].array.count
                Chip(text: "\(procs) proc\(procs == 1 ? "" : "s")")
            }
            // The reason a target cannot be profiled is the actionable part,
            // so it is shown inline rather than hidden behind a click.
            if profiling != "available",
               let reason = app["profiling_reason"].string, !reason.isEmpty {
                Text(reason).font(Term.small).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let fix = app["profiling_recovery_action"].string, !fix.isEmpty {
                    Text("fix: \(fix)").font(Term.small)
                        .foregroundStyle(StatusTone.caution.color)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .padding(.vertical, 3)
    }
}

// MARK: - Preflight

struct PreflightView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(.init("Every row is a probe result, not a plan. **unknown** and "
                     + "**not_tested** are distinct answers from **unsupported**, and none "
                     + "of them means \"false\"."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HStack {
                    TextField("package name or bundle id (optional)",
                              text: $state.selectedApp)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                    Button("probe") { state.loadPreflight() }
                        .buttonStyle(TermButtonStyle(filled: true))
                        .disabled(state.selectedDevice.isEmpty)
                }

                let be = state.preflightDoc["benchmark_eligibility"]
                if !be.isNull {
                    let certified = be["certified_benchmark"].bool == true
                    Banner(kind: certified ? .info : .caution,
                           title: "Benchmark eligibility: \(be["status"].text)",
                           message: certified
                             ? "Eligible for production-like comparison. Eligibility does "
                               + "not mean zero profiler overhead."
                             : "This session cannot certify release performance.")
                    let reasons = be["reasons"].array.compactMap { $0.string }
                    if !reasons.isEmpty {
                        Panel(title: "Every reason, not just the first") {
                            BulletList(title: "", items: reasons)
                        }
                    }
                }

                let matches = state.preflightDoc["target_match_count"].int ?? 0
                if !state.selectedApp.isEmpty && !state.preflightDoc.isNull {
                    if matches == 0 {
                        Banner(kind: .bad, title: "Target not found",
                               message: "\(state.selectedApp) is not in the current "
                                      + "listing. The identifier may be wrong, the app "
                                      + "may not be installed, or the listing may be "
                                      + "partial.")
                    } else if matches > 1 {
                        Banner(kind: .caution, title: "Ambiguous target",
                               message: "\(state.selectedApp) matches \(matches) entries "
                                      + "on this device. Narrow the target.")
                    } else {
                        TargetPanel(target: state.preflightDoc["target"])
                    }
                }

                let caps = state.preflightDoc["capabilities"]["capabilities"].array
                if !caps.isEmpty {
                    Panel(title: "Capabilities (\(caps.count))") {
                        VStack(alignment: .leading, spacing: 9) {
                            ForEach(Array(caps.enumerated()), id: \.offset) { _, c in
                                CapabilityRow(cap: c)
                            }
                        }
                    }
                }
            }
            .padding(16)
        }
        .navigationTitle("~/preflight")
    }
}

private struct TargetPanel: View {
    let target: JSON
    var body: some View {
        Panel(title: "Target",
              subtitle: target["application_key"]["app_identifier"].text) {
            VStack(alignment: .leading, spacing: 7) {
                HStack(spacing: 6) {
                    Chip(text: target["runtime_state"].text,
                         tone: StatusTone.runtime(target["runtime_state"].text))
                    Chip(text: target["profiling_availability"].text,
                         tone: StatusTone.profiling(
                            target["profiling_availability"].text))
                    Chip(text: target["visibility_scope"].text)
                }
                if let r = target["profiling_reason"].string, !r.isEmpty {
                    Text(r).font(Term.body).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(Array(target["process_instances"].array.enumerated()),
                        id: \.offset) { _, p in
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text("pid \(p["pid"].display())")
                                .font(Term.font(12))
                            Chip(text: p["ownership_evidence"].text,
                                 tone: p["counts_toward_app_totals"].bool == true
                                       ? .good : .caution)
                            Chip(text: p["counts_toward_app_totals"].bool == true
                                 ? "counted" : "excluded from app totals",
                                 tone: p["counts_toward_app_totals"].bool == true
                                       ? .good : .bad)
                        }
                        if let note = p["ownership_note"].string, !note.isEmpty {
                            Text(note).font(Term.small).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(8)
                    .termCard()
                }
            }
        }
    }
}

private struct CapabilityRow: View {
    let cap: JSON
    @State private var expanded = false

    var body: some View {
        let status = cap["status"].text
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 7) {
                Image(systemName: expanded ? "chevron.down" : "chevron.right")
                    .font(Term.micro).foregroundStyle(.secondary)
                Text(cap["id"].text).font(Term.font(12))
                Spacer()
                Chip(text: status, tone: StatusTone.capability(status))
                Chip(text: cap["tested"].text,
                     tone: cap["tested"].text.hasPrefix("verified") ? .good : .caution)
            }
            .contentShape(Rectangle())
            .onTapGesture { expanded.toggle() }

            if expanded {
                VStack(alignment: .leading, spacing: 6) {
                    if let e = cap["evidence"].string, !e.isEmpty {
                        Field(label: "evidence") {
                            Text(e).font(Term.small)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    if let s = cap["scope"].string, !s.isEmpty {
                        Field(label: "scope") {
                            Text(s).font(Term.small)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    BulletList(title: "Limitations",
                               items: cap["limitations"].array.compactMap { $0.string })
                    BulletList(title: "Prerequisites",
                               items: cap["prerequisites"].array.compactMap { $0.string })
                    if let fix = cap["recovery_action"].string, !fix.isEmpty {
                        Field(label: "fix") {
                            Text(fix).font(Term.small)
                                .foregroundStyle(StatusTone.caution.color)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                .padding(.leading, 18)
            }
        }
        .padding(9)
        .termCard()
    }
}

/// Recently profiled and favourited targets.
///
/// Spec A23 is an honesty rule rather than a feature: **a recent or favourite
/// entry is not proof the app is running.** These rows are a note this app
/// made on this machine; nothing about the device was consulted to write one,
/// and time has passed. So no row shows a runtime state. What it shows instead
/// is where the entry stands against the *current* enumeration -- and when it
/// is absent from that listing, it says absent from the listing, not "not
/// running", because a listing that could not see an app and an app that is
/// gone are different facts.
struct RecentTargetsPanel: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        let rows = state.selectedDevice.isEmpty
            ? state.recents.ordered
            : state.recents.forDevice(state.selectedDevice)
        if !rows.isEmpty {
            Panel(title: "Recently profiled",
                  subtitle: "what was profiled from this machine — never "
                          + "evidence that an app is running now") {
                VStack(alignment: .leading, spacing: 7) {
                    ForEach(rows) { target in
                        row(target)
                    }
                }
            }
        }
    }

    private func row(_ target: RecentTarget) -> some View {
        let presence = state.presenceOf(target)
        return VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                Button(target.favourite ? "★" : "☆") {
                    state.toggleFavourite(target)
                }
                .buttonStyle(.plain)
                .font(Term.font(13))
                .foregroundStyle(target.favourite ? Term.amber : Term.dim)

                Button {
                    // Selecting a remembered target does not assert anything
                    // about it: preflight and discovery still decide whether
                    // it can be captured.
                    state.selectedApp = target.appIdentifier
                    if state.selectedDevice.isEmpty {
                        state.selectedDevice = target.deviceId
                    }
                } label: {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(target.appIdentifier)
                            .font(Term.font(12)).foregroundStyle(Term.ink)
                        if target.lastKnownName != target.appIdentifier {
                            Text(target.lastKnownName + "  (the name when it "
                                 + "was last profiled)")
                                .font(Term.font(10)).foregroundStyle(Term.dim)
                        }
                    }
                }
                .buttonStyle(.plain)

                Spacer(minLength: 0)
                // Never a runtime state. Where it stands against the current
                // listing, which is a different claim.
                Chip(text: presence.label,
                     tone: presence == .inListing ? .neutral : .caution)
                Button("forget") { state.forgetRecent(target) }
                    .buttonStyle(TermButtonStyle())
            }
            Text(presence.detail)
                .font(Term.font(10)).foregroundStyle(Term.dim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// Starting a simulator or emulator.
///
/// Kept below the device list rather than mixed into it, because these are
/// not devices: an AVD name is not a device id, and the id only exists once
/// the thing is running. A boot therefore reports the device id it
/// *observed*, and says it could not confirm one rather than guessing --
/// which matters because a second emulator lands on `emulator-5556`, not on
/// the 5554 everyone assumes.
///
/// "Started" and "ready" are shown separately. Measured on this machine: a
/// cold AVD came up in about 30 s, and a second one started and never
/// reported `sys.boot_completed` inside 150 s -- adb saw it as `offline` the
/// whole time. A capture taken against that device would have measured the
/// boot.
struct BootPanel: View {
    @EnvironmentObject var state: AppState
    // Collapsed by default. This machine offers 27 bootable targets across two
    // iOS runtimes, and expanded they pushed the device list -- which is what
    // the tab is for -- off the bottom of the window.
    @State private var expanded = false

    private var summary: String {
        let all = state.bootTargets
        let android = all.filter { $0["platform"].text == "android" }.count
        let ios = all.count - android
        let running = all.filter { $0["already_running"].bool == true }.count
        if all.isEmpty { return "nothing bootable was found" }
        var s = "\(android) AVD(s), \(ios) simulator(s)"
        if running > 0 { s += " — \(running) already running" }
        return s
    }

    var body: some View {
        Panel(title: "Start a simulator or emulator",
              subtitle: "not devices yet: a device id exists only once one is "
                      + "running") {
            VStack(alignment: .leading, spacing: 9) {
                HStack(spacing: 10) {
                    Button(expanded ? "Hide" : "Show \(summary)") {
                        expanded.toggle()
                        if expanded { state.loadBootTargetsIfNeeded() }
                    }
                    .buttonStyle(TermButtonStyle())
                    Button("Refresh") { state.refreshDeviceViews() }
                        .buttonStyle(TermButtonStyle())
                    Spacer(minLength: 0)
                }

                // The result of the last attempt stays visible whether or not
                // the list is expanded: a boot that did not come up is the
                // thing most worth not hiding.
                if !state.lastBoot.isNull { bootResult(state.lastBoot) }

                ForEach(Array((expanded ? state.bootTargets : []).enumerated()),
                        id: \.offset) { _, t in
                    let running = t["already_running"].bool == true
                    HStack(spacing: 8) {
                        Chip(text: t["platform"].display(), tone: .neutral)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(t["display_name"].text.isEmpty
                                 ? t["identifier"].display()
                                 : t["display_name"].text)
                                .font(Term.font(12)).foregroundStyle(Term.ink)
                            Text(t["identifier"].display())
                                .font(Term.font(10)).foregroundStyle(Term.dim)
                        }
                        .frame(width: 260, alignment: .leading)
                        if !t["os_version"].text.isEmpty {
                            Text(t["os_version"].text)
                                .font(Term.font(11)).foregroundStyle(Term.dim)
                        }
                        Spacer(minLength: 0)
                        if running {
                            Chip(text: "already running", tone: .good)
                        } else {
                            Button("Start") { state.bootTarget(t["identifier"].text) }
                                .buttonStyle(TermButtonStyle())
                        }
                    }
                }

                let errs = expanded
                    ? state.bootTargetsDoc["errors"].array.compactMap { $0.string }
                    : []
                ForEach(errs, id: \.self) { e in
                    // A provider that could not be asked is reported: an empty
                    // list with a failed provider is not "nothing exists".
                    Text("· " + e)
                        .font(Term.font(10)).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    @ViewBuilder private func bootResult(_ r: JSON) -> some View {
        if !r["error"].text.isEmpty {
            Banner(kind: .bad, title: "Could not start it",
                   message: r["error"].display())
        } else if r["was_already_running"].bool == true {
            Banner(kind: .info, title: "Already running",
                   message: "Nothing was started. Device id "
                          + r["device_id"].display("(not reported)") + ".")
        } else if r["ready"].bool == true {
            Banner(kind: .info, title: "Ready",
                   message: "Device id " + r["device_id"].display("(not confirmed)")
                          + ", after " + String(r["waited_ms"].stamp) + " ms. "
                          + "It is still a simulator or emulator: its timings "
                          + "are never comparable to a physical device.")
        } else if r["started"].bool == true {
            // The outcome that matters most, and the one a convenience
            // wrapper would have called success.
            Banner(kind: .caution, title: "Started, NOT confirmed ready",
                   message: (r["notes"].array.compactMap { $0.string }.first
                             ?? "it did not report itself ready inside the "
                              + "budget")
                          + " Recording against it now would measure the boot.")
        }
    }
}
