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
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                if state.devicesDoc["enumeration_failed"].bool == true {
                    Banner(kind: .bad, title: "Device enumeration failed",
                           message: "That is a different answer from \"no devices are "
                                  + "connected\".")
                }

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
        .navigationTitle("Devices")
        .toolbar {
            Toggle("Simulators", isOn: $state.includeSimulators)
                .onChange(of: state.includeSimulators) { _, _ in state.loadDevices() }
            Button { state.loadDevices() } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
        }
    }
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
                .font(.title2)
                .foregroundStyle(StatusTone.trust(trust).color)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(device["display_name"].display("unnamed device"))
                        .font(.body.weight(.medium))
                    Chip(text: device["platform"].text)
                    Chip(text: form,
                         tone: form == "physical" ? .neutral : .caution)
                    Chip(text: trust, tone: StatusTone.trust(trust))
                }
                Text(device["device_id"].text)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
                Text("OS \(device["os_version"].display("unknown"))  ·  "
                     + device["model"].display("unknown model"))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if selected { Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.tint) }
        }
        .padding(11)
        .background(selected ? Color.accentColor.opacity(0.10) : Color.clear,
                    in: RoundedRectangle(cornerRadius: 9))
        .overlay(RoundedRectangle(cornerRadius: 9)
            .strokeBorder(selected ? Color.accentColor.opacity(0.4)
                                   : Color.secondary.opacity(0.18)))
        .contentShape(Rectangle())
    }
}

// MARK: - Apps

struct AppsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 9) {
                Text("**running** does not mean foreground. **unknown** means the provider "
                     + "could not observe the state — it does not mean not running. "
                     + "Profiling availability is independent of runtime state, and entries "
                     + "that cannot be profiled are kept and marked rather than hidden.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    TextField("filter by name or identifier", text: $state.appFilter)
                        .textFieldStyle(.roundedBorder).frame(maxWidth: 280)
                    Toggle("Running only", isOn: $state.runningOnly)
                    Spacer()
                    Text("\(state.filteredApps.count) of \(state.apps.count)")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if state.appsDoc["enumeration_failed"].bool == true {
                    Banner(kind: .bad, title: "App enumeration failed for this device",
                           message: "That is not the same as the device having no apps.")
                }
            }
            .padding(16)

            Divider()

            if state.selectedDevice.isEmpty {
                ContentUnavailableView("No device selected",
                                       systemImage: "iphone.slash",
                                       description: Text("Pick a usable device first."))
            } else {
                List {
                    ForEach(Array(state.filteredApps.enumerated()), id: \.offset) { _, a in
                        AppRow(app: a) {
                            state.selectedApp = a["application_key"]["app_identifier"].text
                        }
                    }
                }
                .listStyle(.inset)
            }
        }
        .navigationTitle("Apps")
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
                Text(id).font(.system(size: 12, design: .monospaced))
                if !name.isEmpty && name != id {
                    Text(name).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Use", action: use).buttonStyle(.borderless).font(.caption)
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
                Text(reason).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let fix = app["profiling_recovery_action"].string, !fix.isEmpty {
                    Text("fix: \(fix)").font(.caption)
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
                Text("Every row is a probe result, not a plan. **unknown** and "
                     + "**not_tested** are distinct answers from **unsupported**, and none "
                     + "of them means \"false\".")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HStack {
                    TextField("package name or bundle id (optional)",
                              text: $state.selectedApp)
                        .textFieldStyle(.roundedBorder).frame(maxWidth: 320)
                    Button("Probe") { state.loadPreflight() }
                        .buttonStyle(.borderedProminent)
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
        .navigationTitle("Preflight")
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
                    Text(r).font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(Array(target["process_instances"].array.enumerated()),
                        id: \.offset) { _, p in
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text("pid \(p["pid"].display())")
                                .font(.system(size: 12, design: .monospaced))
                            Chip(text: p["ownership_evidence"].text,
                                 tone: p["counts_toward_app_totals"].bool == true
                                       ? .good : .caution)
                            Chip(text: p["counts_toward_app_totals"].bool == true
                                 ? "counted" : "excluded from app totals",
                                 tone: p["counts_toward_app_totals"].bool == true
                                       ? .good : .bad)
                        }
                        if let note = p["ownership_note"].string, !note.isEmpty {
                            Text(note).font(.caption).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(8)
                    .background(.quaternary.opacity(0.3),
                                in: RoundedRectangle(cornerRadius: 7))
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
                    .font(.caption2).foregroundStyle(.secondary)
                Text(cap["id"].text).font(.system(size: 12, design: .monospaced))
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
                            Text(e).font(.caption)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    if let s = cap["scope"].string, !s.isEmpty {
                        Field(label: "scope") {
                            Text(s).font(.caption)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    BulletList(title: "Limitations",
                               items: cap["limitations"].array.compactMap { $0.string })
                    BulletList(title: "Prerequisites",
                               items: cap["prerequisites"].array.compactMap { $0.string })
                    if let fix = cap["recovery_action"].string, !fix.isEmpty {
                        Field(label: "fix") {
                            Text(fix).font(.caption)
                                .foregroundStyle(StatusTone.caution.color)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                .padding(.leading, 18)
            }
        }
        .padding(9)
        .background(.quaternary.opacity(0.25), in: RoundedRectangle(cornerRadius: 8))
    }
}
