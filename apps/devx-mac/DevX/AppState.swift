import Foundation
import SwiftUI

/// Everything the UI knows, and nothing it infers.
///
/// The engine's JSON is held as-is. Where a view needs a derived value it
/// computes it here rather than at render time, but it never fills in a value
/// the engine left unknown: `unknown` reaches the screen as `unknown`.
@MainActor
final class AppState: ObservableObject {
    typealias Tab = DevXTab

    @Published var tab: Tab = .devices
    @Published var busy: String? = nil
    @Published var lastError: String? = nil

    @Published var includeSimulators = true
    @Published var sessionsDir = Core.defaultSessionsDir

    @Published var devicesDoc: JSON = .null
    @Published var appsDoc: JSON = .null
    @Published var preflightDoc: JSON = .null
    @Published var sessionsDoc: JSON = .null
    @Published var sessionDoc: JSON = .null
    @Published var rulesDoc: JSON = .null
    @Published var recordDoc: JSON = .null

    @Published var selectedDevice: String = ""
    @Published var selectedApp: String = ""
    @Published var selectedSession: String = ""
    @Published var selectedIssueIndex: Int = 0
    @Published var appFilter: String = ""
    @Published var runningOnly = false

    @Published var recordDuration = 6
    @Published var recordHz = 200
    @Published var recordFrames = true
    @Published var recordCpu = true
    @Published var recordMemory = true
    @Published var recordResetFrames = true

    var engineVersion = "?"
    var rulesetVersion = "?"

    var devices: [JSON] { devicesDoc["devices"].array }
    var usableDevices: [JSON] {
        devices.filter { $0["trust"].text == "authorized" }
    }
    var apps: [JSON] { appsDoc["apps"].array }
    var sessions: [JSON] { sessionsDoc["sessions"].array }
    var issues: [JSON] { sessionDoc["analysis"]["issues"].array }
    var ruleRuns: [JSON] { sessionDoc["analysis"]["rule_runs"].array }

    var filteredApps: [JSON] {
        let needle = appFilter.lowercased()
        return apps.filter { a in
            // "running only" keeps unknown-state entries: excluding them would
            // be the same mistake as rendering unknown as not running.
            if runningOnly && a["runtime_state"].text == "not_running" { return false }
            guard !needle.isEmpty else { return true }
            let id = a["application_key"]["app_identifier"].text.lowercased()
            let name = a["display_name"].text.lowercased()
            return id.contains(needle) || name.contains(needle)
        }
        .sorted { lhs, rhs in
            func rank(_ a: JSON) -> Int {
                var r = 0
                if a["runtime_state"].text == "running" { r -= 4 }
                else if a["runtime_state"].text == "unknown" { r -= 1 }
                if a["profiling_availability"].text == "available" { r -= 2 }
                else if a["profiling_availability"].text == "unavailable" { r += 2 }
                return r
            }
            let (l, r) = (rank(lhs), rank(rhs))
            if l != r { return l < r }
            return lhs["application_key"]["app_identifier"].text
                 < rhs["application_key"]["app_identifier"].text
        }
    }

    func device(_ id: String) -> JSON? {
        devices.first { $0["device_id"].text == id }
    }

    /// Runs a blocking core call off the main actor, then publishes the result.
    private func run(_ label: String, _ work: @escaping @Sendable () -> JSON,
                     then apply: @escaping (JSON) -> Void) {
        busy = label
        lastError = nil
        Task.detached(priority: .userInitiated) {
            let doc = work()
            await MainActor.run {
                self.busy = nil
                if let err = doc["error"].string, !err.isEmpty {
                    self.lastError = err
                }
                apply(doc)
            }
        }
    }

    func loadVersion() {
        Task.detached(priority: .utility) {
            let v = Core.version()
            await MainActor.run {
                self.engineVersion = v["engine_version"].display("?")
                self.rulesetVersion = v["ruleset_version"].display("?")
            }
        }
    }

    func loadDevices() {
        let sims = includeSimulators
        run("Discovering devices…", { Core.devices(includeSimulators: sims) }) { doc in
            self.devicesDoc = doc
            // Selection is preserved across a refresh, and never silently
            // switched to a different device.
            if self.selectedDevice.isEmpty ||
                !self.usableDevices.contains(where: {
                    $0["device_id"].text == self.selectedDevice }) {
                self.selectedDevice = self.usableDevices.first?["device_id"].text ?? ""
            }
        }
    }

    func loadApps() {
        guard !selectedDevice.isEmpty else { appsDoc = .null; return }
        let dev = selectedDevice, sims = includeSimulators
        run("Enumerating apps…",
            { Core.apps(device: dev, includeSimulators: sims) }) { self.appsDoc = $0 }
    }

    func loadPreflight() {
        guard !selectedDevice.isEmpty else { return }
        let dev = selectedDevice, app = selectedApp, sims = includeSimulators
        run("Probing capabilities…",
            { Core.preflight(device: dev, app: app, includeSimulators: sims) }) {
            self.preflightDoc = $0
        }
    }

    func loadRules() {
        guard rulesDoc.isNull else { return }
        run("Loading detectors…", { Core.rules() }) { self.rulesDoc = $0 }
    }

    func loadSessions() {
        let dir = sessionsDir
        run("Reading sessions…", { Core.sessions(dir: dir) }) { self.sessionsDoc = $0 }
    }

    func openSession(_ id: String) {
        let dir = sessionsDir
        selectedSession = id
        selectedIssueIndex = 0
        run("Opening \(id)…", { Core.session(dir: dir, id: id) }) {
            self.sessionDoc = $0
            self.tab = .issues
        }
    }

    func startRecord() {
        guard !selectedDevice.isEmpty, !selectedApp.isEmpty else {
            lastError = "Pick a device and an app identifier first."
            return
        }
        let dir = sessionsDir, dev = selectedDevice, app = selectedApp
        let d = recordDuration, hz = recordHz
        let f = recordFrames, c = recordCpu, m = recordMemory, r = recordResetFrames
        Core.resetCancel()
        recordDoc = .null
        run("Recording \(app) for \(d)s…", {
            Core.record(dir: dir, device: dev, app: app, durationSeconds: d,
                        sampleHz: hz, frames: f, cpu: c, memory: m,
                        resetFrames: r)
        }) { doc in
            self.recordDoc = doc
            if let id = doc["session_id"].string, !id.isEmpty {
                self.loadSessions()
            }
        }
    }

    func cancel() { Core.cancel() }
}
