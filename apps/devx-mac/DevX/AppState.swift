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
    @Published var timelineDoc: JSON = .null
    @Published var compareDoc: JSON = .null
    @Published var baselinePath: String = ""
    @Published var candidatePath: String = ""
    // Thresholds the operator can raise. Zero means "the engine's default",
    // never "no gate": a comparison that cleared a threshold of zero would
    // report every difference as a regression.
    @Published var compareMinRuns: Int = 0
    @Published var compareMinRelativeDelta: Double = 0
    // How many bins the timeline is asked for. The capture's size does not
    // enter into it: a ten-minute recording and a ten-second one both come
    // back with this many, so the view's cost is fixed.
    @Published var timelineBins: Int = 160
    // The band the user asked to focus, as an index into the timeline's
    // issue list. Clicking an issue focuses its evidence interval (spec
    // section 13), so this is what carries that selection.
    @Published var focusedBandId: String = ""
    @Published var rulesDoc: JSON = .null
    @Published var recordDoc: JSON = .null

    @Published var selectedDevice: String = ""
    @Published var selectedApp: String = ""
    @Published var selectedSession: String = ""
    // Issue filters (spec section 13). Empty means "no filter on this
    // dimension" -- never "match nothing", which would make an empty list
    // look like a clean app.
    @Published var filterCategory: String = ""
    @Published var filterSeverity: String = ""
    @Published var filterScreen: String = ""
    @Published var filterThread: String = ""
    @Published var filterProcess: String = ""
    @Published var showSuppressed: Bool = false

    // The project's suppression list, and the fields for adding one. The
    // reason is mandatory and the UI enforces it before the core has to.
    @Published var suppressionsDoc: JSON = .null
    @Published var suppressReason: String = ""
    @Published var suppressExpiry: String = ""
    @Published var suppressReference: String = ""
    /// Whether the open report came from re-analysis rather than from the
    /// session as recorded. Shown, because a reader needs to know which they
    /// are looking at.
    @Published var reanalyzed: Bool = false
    @Published var selectedIssueIndex: Int = 0
    @Published var appFilter: String = ""
    @Published var runningOnly = false

    @Published var recordDuration = 6
    @Published var recordHz = 200
    @Published var recordFrames = true
    // The heavier collectors, off by default. Spec section 13 requires the
    // overhead to be explained before one is enabled, so the UI gates them
    // behind a panel that states the cost rather than a bare switch.
    @Published var recordScheduling = false
    @Published var recordHeap = false
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

    /// The issues after filtering, and the count that was hidden.
    var filteredIssues: (shown: [JSON], hidden: Int) {
        issueFilter.apply(issues)
    }

    var issueFilter: IssueFilter {
        IssueFilter(category: filterCategory, severity: filterSeverity,
                    screen: filterScreen, thread: filterThread,
                    process: filterProcess, includeSuppressed: showSuppressed)
    }

    func matchesFilters(_ issue: JSON) -> Bool { issueFilter.matches(issue) }

    func filterOptions(_ field: String) -> [String] {
        IssueFilter.options(field, in: issues)
    }

    var anyFilterActive: Bool { issueFilter.isActive }

    func clearFilters() {
        filterCategory = ""
        filterSeverity = ""
        filterScreen = ""
        filterThread = ""
        filterProcess = ""
        showSuppressed = false
    }
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

    /// Checks a path this app was handed rather than chose, and explains a
    /// refusal instead of stalling on it.
    ///
    /// macOS gates Documents, Desktop and Downloads behind a consent prompt
    /// that an ad-hoc signed build cannot raise, and the block happens inside
    /// the read -- so a path typed or passed on the command line can hang the
    /// operation forever behind a spinner. A named failure is a better answer.
    /// Paths chosen through an open panel are exempt in practice: the panel
    /// grants access to what the user picked.
    @discardableResult
    func ensureReadable(_ path: String, label: String) -> Bool {
        switch Core.probeReadable(path) {
        case .some(true):
            return true
        case .some(false):
            lastError = "The \(label) file cannot be read: \(path). If it "
                + "exists, macOS may be withholding access to the folder it "
                + "is in -- use Choose… instead of typing the path, which "
                + "grants this app access to that one file."
            return false
        case .none:
            // Neither readable nor unreadable: the probe itself blocked,
            // which is what a withheld folder looks like from here.
            lastError = "macOS has not granted this app access to \(path) -- "
                + "the read did not return. Documents, Desktop and Downloads "
                + "are gated, and an ad-hoc signed build cannot raise the "
                + "prompt. Use Choose… to grant access to the file, or move "
                + "it somewhere else."
            return false
        }
    }

    func runCompare() {
        guard !baselinePath.isEmpty, !candidatePath.isEmpty else {
            lastError = "Pick a baseline and a candidate run-set file."
            return
        }
        let b = baselinePath, c = candidatePath
        guard ensureReadable(b, label: "baseline"),
              ensureReadable(c, label: "candidate") else { return }
        let runs = compareMinRuns, rel = compareMinRelativeDelta
        compareDoc = .null
        run("Comparing…", {
            Core.compare(baseline: b, candidate: c, minRuns: runs,
                         minRelativeDelta: rel)
        }) { self.compareDoc = $0 }
    }

    // ---- suppressions ---------------------------------------------------
    //
    // A suppression is a project decision, so it goes in a file the project
    // keeps and the CLI reads too. The session on disk is never rewritten:
    // the raw trace is immutable, and applying a suppression re-runs the
    // analysis over it rather than editing what was recorded.

    func loadSuppressions() {
        let path = Core.suppressionsPath(sessionsDir: sessionsDir)
        run("Reading suppressions…", { Core.suppressions(path: path) }) {
            self.suppressionsDoc = $0
        }
    }

    var suppressionEntries: [JSON] {
        suppressionsDoc["suppressions"].array
    }

    /// Adds a suppression for the selected issue, then re-analyses so the
    /// effect is visible rather than promised.
    func suppressSelectedIssue(allFindings: Bool) {
        guard issues.indices.contains(selectedIssueIndex) else { return }
        let issue = issues[selectedIssueIndex]
        let reason = suppressReason.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !reason.isEmpty else {
            lastError = "A suppression needs a reason. One nobody can review "
                + "is permanent by accident."
            return
        }
        let path = Core.suppressionsPath(sessionsDir: sessionsDir)
        let ruleId = issue["rule_id"].text
        // Empty fingerprint means every finding from the rule, which is a
        // much larger claim -- so it is a separate, deliberate action.
        let fingerprint = allFindings ? "" : issue["fingerprint"].text
        let expiry = suppressExpiry.trimmingCharacters(in: .whitespacesAndNewlines)
        let author = NSFullUserName()
        let reference = suppressReference
        run("Suppressing \(ruleId)…", {
            Core.addSuppression(path: path, ruleId: ruleId,
                                fingerprint: fingerprint, reason: reason,
                                expiry: expiry, author: author,
                                reference: reference)
        }) { doc in
            if doc["error"].text.isEmpty {
                self.suppressionsDoc = doc
                self.suppressReason = ""
                self.suppressExpiry = ""
                self.suppressReference = ""
                self.reanalyzeSession()
            }
        }
    }

    func removeSuppression(_ entry: JSON) {
        let path = Core.suppressionsPath(sessionsDir: sessionsDir)
        let ruleId = entry["rule_id"].text
        let fingerprint = entry["fingerprint"].text
        run("Removing the suppression…", {
            Core.removeSuppression(path: path, ruleId: ruleId,
                                   fingerprint: fingerprint)
        }) { doc in
            if doc["error"].text.isEmpty {
                self.suppressionsDoc = doc
                self.reanalyzeSession()
            }
        }
    }

    /// Re-runs the analysis over the open session's stored trace with the
    /// current suppression list. The session package is not modified.
    func reanalyzeSession() {
        guard !selectedSession.isEmpty else { return }
        let dir = sessionsDir, id = selectedSession
        let path = Core.suppressionsPath(sessionsDir: sessionsDir)
        run("Re-analysing \(id)…", {
            Core.reanalyze(dir: dir, id: id, suppressionsPath: path)
        }) { doc in
            if doc["error"].text.isEmpty {
                self.sessionDoc = doc
                self.reanalyzed = true
                self.selectedIssueIndex = 0
                // The timeline's bands come from the analysis, so a
                // suppression that hides a finding must hide its band too.
                self.timelineDoc = .null
                self.focusedBandId = ""
            }
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

    /// Opens a session and reveals it in `revealIn`.
    ///
    /// The destination is a parameter because a launch that asked for a tab
    /// has already said where it wants to land: hard-coding Issues here meant
    /// `--session X --tab timeline` opened X and then jumped away from the
    /// tab that was requested.
    func openSession(_ id: String, revealIn: DevXTab = .issues,
                     focusIssueId: String? = nil) {
        let dir = sessionsDir
        guard ensureReadable(dir + "/" + id + "/manifest.json",
                             label: "session") else { return }
        selectedSession = id
        selectedIssueIndex = 0
        timelineDoc = .null
        focusedBandId = ""
        reanalyzed = false
        loadSuppressions()
        run("Opening \(id)…", { Core.session(dir: dir, id: id) }) {
            self.sessionDoc = $0
            self.tab = revealIn
            if let wanted = focusIssueId {
                // Selecting the issue as well as focusing it: a link that
                // lands on a finding should also have that finding selected
                // in the Issues tab, not just highlighted on the timeline.
                self.focusedBandId = wanted
                if let idx = self.issues.firstIndex(where: {
                    $0["issue_id"].text == wanted
                        || $0["fingerprint"].text == wanted }) {
                    self.selectedIssueIndex = idx
                } else {
                    self.lastError = "No issue in this session has the id "
                        + "'\(wanted)'. The session is open; nothing is focused."
                    self.focusedBandId = ""
                }
            }
            if revealIn == .timeline { self.loadTimeline() }
        }
    }

    /// Loads the open session's timeline. Separate from `openSession` because
    /// it re-reads and re-analyzes the trace, which is work the Issues tab
    /// does not need.
    func loadTimeline(force: Bool = false) {
        guard !selectedSession.isEmpty else { return }
        guard force || timelineDoc.isNull else { return }
        let dir = sessionsDir, id = selectedSession, bins = timelineBins
        run("Binning \(id)…", { Core.timeline(dir: dir, id: id, bins: bins) }) {
            self.timelineDoc = $0
        }
    }

    /// Focuses an issue's own interval on the timeline. The band is found by
    /// id rather than by position so a re-bin cannot move the focus onto a
    /// different finding.
    func focusIssue(_ issue: JSON) {
        let id = issue["issue_id"].text.isEmpty ? issue["fingerprint"].text
                                                : issue["issue_id"].text
        focusedBandId = id
        tab = .timeline
        loadTimeline()
    }

    func startRecord() {
        guard !selectedDevice.isEmpty, !selectedApp.isEmpty else {
            lastError = "Pick a device and an app identifier first."
            return
        }
        let dir = sessionsDir, dev = selectedDevice, app = selectedApp
        let d = recordDuration, hz = recordHz
        let f = recordFrames, c = recordCpu, m = recordMemory, r = recordResetFrames
        let sched = recordScheduling, heap = recordHeap
        Core.resetCancel()
        recordDoc = .null
        let label = heap ? "Recording \(app) for \(d)s, then dumping the heap…"
                         : "Recording \(app) for \(d)s…"
        run(label, {
            Core.record(dir: dir, device: dev, app: app, durationSeconds: d,
                        sampleHz: hz, frames: f, cpu: c, memory: m,
                        resetFrames: r, scheduling: sched, heap: heap)
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
