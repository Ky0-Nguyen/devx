import Foundation
import SwiftUI

/// Everything the UI knows, and nothing it infers.
///
/// The engine's JSON is held as-is. Where a view needs a derived value it
/// computes it here rather than at render time, but it never fills in a value
/// the engine left unknown: `unknown` reaches the screen as `unknown`.
/// A stop flag readable from any thread.
///
/// Deliberately not a member of AppState: the poll loop runs on the core queue
/// and must read it there, which an actor-isolated property forbids.
final class PollFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    var stopped: Bool {
        lock.lock(); defer { lock.unlock() }
        return value
    }
    func stop() {
        lock.lock(); defer { lock.unlock() }
        value = true
    }
    func reset() {
        lock.lock(); defer { lock.unlock() }
        value = false
    }
}

@MainActor
final class AppState: ObservableObject {
    typealias Tab = DevXTab

    @Published var tab: Tab = .devices
    @Published var lastError: String? = nil

    // In-flight blocking operations, by label.
    //
    // A single `busy: String?` was wrong: several operations run at once when
    // the window opens, the last writer won, and whichever finished first
    // cleared the indicator for the others. Worse, a stale label could sit over
    // a live capture that was already streaming. Keyed entries mean each
    // operation clears only its own.
    @Published private(set) var inFlight: [String] = []
    var busy: String? { inFlight.last }

    private func beginOperation(_ label: String) {
        inFlight.append(label)
    }
    private func endOperation(_ label: String) {
        if let at = inFlight.lastIndex(of: label) { inFlight.remove(at: at) }
    }

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

    // ---- live capture ----
    @Published var liveSnapshot: JSON = .null
    @Published var liveRunning = false
    @Published var liveStopResult: JSON? = nil
    // Live capture has its own progress state rather than sharing the global
    // indicator, so a capture that is already streaming can never be hidden
    // behind a "starting" overlay.
    @Published var liveStarting = false
    @Published var liveStopping = false
    @Published var liveTickMs = 500
    @Published var liveCpuWindowMs = 5000
    // Per-counter history for the sparklines, capped so a long session cannot
    // grow the UI's memory without bound.
    @Published var liveSeries: [String: [Double]] = [:]
    private let liveSeriesCap = 240

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

    /// Blocking core calls run here, not in a Task.
    ///
    /// Every Core call blocks for as long as the device tooling takes -- adb
    /// and devicectl invocations, and a whole capture window. Swift's
    /// cooperative pool is sized for non-blocking work, so occupying its
    /// threads with blocking calls starves it: with the version, device,
    /// session and live-start calls all in flight on appear, a `MainActor.run`
    /// inside one of them could not be scheduled and the UI sat on
    /// "Starting live capture..." indefinitely. A dedicated queue has no such
    /// limit.
    private static let coreQueue = DispatchQueue(
        label: "com.yum.superapp.devx.core", qos: .userInitiated,
        attributes: .concurrent)

    /// Runs a blocking core call off the main thread, then publishes the result.
    private func run(_ label: String, _ work: @escaping @Sendable () -> JSON,
                     then apply: @escaping (JSON) -> Void) {
        beginOperation(label)
        lastError = nil
        Self.coreQueue.async {
            let doc = work()
            DispatchQueue.main.async {
                self.endOperation(label)
                if let err = doc["error"].string, !err.isEmpty {
                    self.lastError = err
                }
                apply(doc)
            }
        }
    }

    func loadVersion() {
        Self.coreQueue.async {
            let v = Core.version()
            DispatchQueue.main.async {
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

    // ---- live capture --------------------------------------------------
    //
    // The poll loop runs on a detached task and publishes on the main actor.
    // Polling rather than a callback into Swift, because a C++ thread calling
    // back into Swift would need its own thread-safety contract for very
    // little gain: a UI only needs the current state, not every event.
    func startLive() {
        pollFlag.reset()
        guard !selectedDevice.isEmpty, !selectedApp.isEmpty else {
            lastError = "Pick a device and an app identifier first."
            return
        }
        let dir = sessionsDir, dev = selectedDevice, app = selectedApp
        let hz = recordHz, f = recordFrames, c = recordCpu, m = recordMemory
        let r = recordResetFrames, tick = liveTickMs, win = liveCpuWindowMs

        liveStarting = true
        lastError = nil
        liveStopResult = nil
        liveSeries = [:]
        liveSnapshot = .null

        Self.coreQueue.async {
            let started = Core.liveStart(dir: dir, device: dev, app: app,
                                         sampleHz: hz, frames: f, cpu: c,
                                         memory: m, resetFrames: r,
                                         tickMs: tick, cpuWindowMs: win)
            DispatchQueue.main.async {
                self.liveStarting = false
                if let err = started["error"].string, !err.isEmpty {
                    self.lastError = err
                    self.liveRunning = false
                    // An unsupported platform is reported in place, so the
                    // reason is visible rather than just an error toast.
                    if started["unsupported"].bool == true {
                        self.liveStopResult = started
                    }
                    return
                }
                self.liveRunning = true
                self.beginLivePolling()
            }
        }
    }

    private func beginLivePolling() {
        Self.coreQueue.async {
            while true {
                if self.pollFlag.stopped { break }
                let snap = Core.livePoll()
                let running = Core.liveRunning
                DispatchQueue.main.async {
                    self.liveSnapshot = snap
                    self.appendLiveSeries(from: snap)
                    if !running && self.liveRunning {
                        // The collector stopped on its own; reflect that rather
                        // than showing a session that is still streaming.
                        self.liveRunning = false
                    }
                }
                if !running { break }
                Thread.sleep(forTimeInterval: 0.35)
            }
        }
    }

    // The poll loop runs on the core queue and has to read this flag from
    // there, so it lives outside the actor rather than on it.
    private let pollFlag = PollFlag()
    private func requestPollStop() { pollFlag.stop() }

    private func appendLiveSeries(from snap: JSON) {
        for c in snap["latest_counters"].array {
            guard let name = c["name"].string, let v = c["value"].double else { continue }
            var series = liveSeries[name] ?? []
            series.append(v)
            if series.count > liveSeriesCap {
                series.removeFirst(series.count - liveSeriesCap)
            }
            liveSeries[name] = series
        }
    }

    func stopLive() {
        liveStopping = true
        requestPollStop()
        Self.coreQueue.async {
            let result = Core.liveStop()
            DispatchQueue.main.async {
                self.liveStopping = false
                self.liveRunning = false
                self.liveStopResult = result
                // The final snapshot is no longer preliminary, so it is read
                // once more to pick up the closed-window analysis.
                self.liveSnapshot = Core.livePoll()
                if let err = result["error"].string, !err.isEmpty {
                    self.lastError = err
                }
                if result["session_id"].string != nil { self.loadSessions() }
            }
        }
    }

    func cancel() { Core.cancel() }
}
