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

    /// The interface language.
    ///
    /// A plain UI preference, so it lives in UserDefaults rather than in a
    /// file of its own: unlike Recents -- which is a note about what was
    /// profiled and therefore has to survive as data -- this says nothing
    /// about any device or measurement.
    @Published var language: DevXLanguage = .system {
        didSet {
            Strings.active = Strings.resolve(
                language, preferredLanguages: Locale.preferredLanguages)
            UserDefaults.standard.set(language.rawValue, forKey: Self.languageKey)
        }
    }
    static let languageKey = "devx.language"
    static let deviceKey = "devx.lastDevice"
    static let appKey = "devx.lastApp"

    /// Remembers the target so it does not have to be retyped every launch.
    ///
    /// A *preference*, not evidence. The device id is written down because it
    /// was selected here, and on the next launch it is only restored if
    /// discovery finds that device again -- a remembered id is not a claim
    /// that anything is connected, which is the same rule Recents follows.
    func rememberTarget() {
        UserDefaults.standard.set(selectedDevice, forKey: Self.deviceKey)
        UserDefaults.standard.set(selectedApp, forKey: Self.appKey)
    }

    private var pendingDevice: String? = nil

    /// Reads the remembered target. The device is *not* selected yet: it is
    /// held until discovery says whether it is there.
    func loadRememberedTarget() {
        // An explicit `--device` / `--app` on the command line has already
        // been applied at this point and must win: someone who named a target
        // is not asking for last week's.
        if selectedDevice.isEmpty {
            pendingDevice = UserDefaults.standard.string(forKey: Self.deviceKey)
        }
        if selectedApp.isEmpty,
           let app = UserDefaults.standard.string(forKey: Self.appKey),
           !app.isEmpty {
            selectedApp = app
        }
    }

    /// Applies the remembered device once a device list exists.
    ///
    /// Returns true when it was restored. A remembered device that is absent,
    /// or present but unusable, is left unselected rather than selected and
    /// then failing on the first operation.
    @discardableResult
    func applyRememberedDevice() -> Bool {
        let ids = usableDevices.map { $0["device_id"].text }
        guard let chosen = RecentTargets.restore(remembered: pendingDevice,
                                                 usable: ids) else {
            return false
        }
        pendingDevice = nil
        selectedDevice = chosen
        return true
    }

    /// Applies the stored preference before the first frame is drawn, so the
    /// window does not appear in English and then change under the reader.
    func loadLanguage() {
        let stored = UserDefaults.standard.string(forKey: Self.languageKey)
        language = stored.flatMap(DevXLanguage.init(rawValue:)) ?? .system
    }

    @Published var includeSimulators = true
    @Published var sessionsDir = Core.defaultSessionsDir

    @Published var devicesDoc: JSON = .null
    // When discovery last actually ran.
    //
    // This exists because a device list with no age is a lie waiting to
    // happen: an emulator started outside this app reads as *absent*, and
    // absent is exactly the claim the list has no evidence for. The whole
    // project turns on `unknown != false`, and a stale snapshot presented as
    // current is that mistake in the one view every session starts from. So
    // the view states when it looked, and the answer is allowed to look old.
    @Published private(set) var devicesLoadedAt: Date? = nil
    @Published private(set) var bootTargetsLoadedAt: Date? = nil
    /// Re-scans while the Devices tab is open. On by default: every tool that
    /// attaches to devices does this, and the cost does not touch the device
    /// -- `adb devices` talks to the local adb server and `simctl list` is
    /// entirely host-side, so polling cannot perturb a measurement. It is
    /// still suspended during a live capture, where the host's own CPU is
    /// part of what the operator is watching.
    @Published var watchDevices = true
    /// What stopped the last watch tick from scanning, or empty if it scanned.
    ///
    /// Recorded because the alternative is a label that states intent: it said
    /// "re-scanning every 5s" while a boot held the discovery queue for three
    /// minutes, next to an age of six minutes. Both cannot be true, and the
    /// reader cannot tell which is.
    @Published private(set) var deviceWatchSuppressedBy: String = ""
    /// Free-text filter over the device list, and whether to show the
    /// unusable ones.
    ///
    /// Twenty-three simulators is the normal case on a developer's machine,
    /// and a list that long buries the one device being worked with.
    @Published var deviceFilter: String = ""
    @Published var showUnusableDevices: Bool = true
    @Published var appsDoc: JSON = .null
    // Remembered targets. A note this app made on this machine -- never
    // evidence about the device, which is the whole of spec A23.
    @Published var recents = RecentTargets()
    // What could be started, and the result of the last attempt.
    @Published var bootTargetsDoc: JSON = .null
    @Published var lastBoot: JSON = .null
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
    // Inspect: reading a running app through the inspector it already runs.
    @Published var inspectTargetsDoc: JSON = .null
    @Published var inspectDoc: JSON = .null
    @Published var inspectSeconds: Int = 15
    @Published var inspectRedux: Bool = true
    /// Off by default, and deliberately not remembered: a store holds tokens
    /// and personal data, so including its values is a decision made per
    /// capture rather than a setting that quietly stays on.
    @Published var inspectReduxValues: Bool = false
    /// Watch the store while attached, rather than reading it once.
    ///
    /// On by default when observing live: a list of slice names answers "is
    /// Redux here", and the question people actually bring is "what just
    /// happened, and what did it change".
    @Published var inspectReduxWatch: Bool = true
    /// Also wrap `dispatch`, so action types and payloads are seen.
    ///
    /// Off by default and deliberately not remembered: this is the one
    /// setting in the whole feature that modifies the running app, so it is
    /// chosen per observation rather than left on.
    @Published var inspectReduxActions: Bool = false
    /// Whether the activity list is expanded, and which rows differ from it.
    ///
    /// Two pieces rather than one set of open rows: "expand all" has to keep
    /// working as new records stream in, and a set of ids collected before
    /// they arrived would leave every new row closed.
    @Published var reduxExpandAll: Bool = false
    @Published var reduxOpenRecords: Set<Int> = []
    /// Watermarks set by the per-list "clear" controls.
    ///
    /// Marks rather than deletions: the rows stay in the observation and in
    /// an export, and the view says how many it is holding back. A delete
    /// would also be undone on the next poll -- the assembler is cumulative
    /// and returns the whole observation every time.
    @Published var clearedNetwork: Int = 0
    @Published var clearedConsole: Int = 0
    /// A seq, not a count: the in-app buffer drops its oldest records under
    /// load, so the list shortens from the front and a count would hide the
    /// wrong rows.
    @Published var clearedReduxSeq: Int = 0
    /// Which request the detail column is showing, by request_id.
    ///
    /// An id and not an index: the rendered list is afterClear(captured) then
    /// filtered, and both shift every offset, so an index-keyed selection
    /// silently lands on a different request.
    @Published var selectedRequestId: String = ""
    /// Which preliminary finding the Live tab is showing, by issue_id.
    ///
    /// A fingerprint and not an index: live findings are recomputed every
    /// tick, so an index would slide onto a different finding as the list
    /// changes underneath the reader.
    @Published var selectedLiveIssueId: String = ""
    /// Whether detail was asked for when the observation on screen was
    /// started. nil means nobody recorded it.
    ///
    /// Not the live toggle. `inspectDetail` is a control for the *next*
    /// observation, and reading it to describe the document already on screen
    /// meant that switching it off after a capture made the pane claim the
    /// bodies it was displaying had never been captured. The report itself
    /// carries no such flag, so this is the only record there is -- and when
    /// there is none, the honest answer is that it is unknown rather than a
    /// negative claim about someone else's data.
    @Published var inspectDocCapturedDetail: Bool? = nil
    @Published var inspectScreenshots: Bool = false
    /// Capture request/response headers and bodies. Off by default and
    /// deliberately not remembered: this is the data in flight, including
    /// bearer tokens, so it is a decision made per observation.
    @Published var inspectDetail: Bool = false
    /// Live observation state.
    @Published var inspectStreaming: Bool = false
    @Published var inspectDisconnect: String = ""
    /// Which kinds of activity to show, and a free-text needle.
    @Published var inspectKinds: Set<InspectKind> = Set(InspectKind.allCases)
    @Published var inspectNeedle: String = ""
    /// Which attached device to observe, matched against Metro's device name.
    /// Empty is only valid when one device offers the app -- otherwise the
    /// core refuses and lists the choices rather than picking one.
    @Published var inspectTargetDevice: String = ""

    @Published var rulesDoc: JSON = .null
    @Published var recordDoc: JSON = .null

    @Published var selectedDevice: String = "" {
        didSet {
            guard selectedDevice != oldValue else { return }
            UserDefaults.standard.set(selectedDevice, forKey: Self.deviceKey)
        }
    }
    @Published var selectedApp: String = "" {
        didSet {
            guard selectedApp != oldValue else { return }
            UserDefaults.standard.set(selectedApp, forKey: Self.appKey)
        }
    }
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
    @Published var lastExport: String = ""
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
    /// Seconds of stack profiling at the *end* of a live capture, 0 for none.
    ///
    /// Opt-in for two reasons, and the second matters more: `/usr/bin/sample`
    /// blocks for the seconds it samples, so this delays the stop; and what
    /// it returns is an aggregate with no timestamps, which answers where the
    /// time went and can never answer when. Folding that into a live view by
    /// default would invite reading it as part of the timeline.
    @Published var liveStackProfileSeconds = 0
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

    var recentsPath: String { sessionsDir + "/recent-targets.json" }

    func loadRecents() {
        recents = RecentTargets.load(from: recentsPath)
    }

    /// Records the target being profiled. Called when a capture starts, not
    /// when a row is clicked: a remembered target should mean "this was
    /// profiled", not "this was looked at".
    func rememberCurrentTarget() {
        guard !selectedDevice.isEmpty, !selectedApp.isEmpty else { return }
        var name = selectedApp
        for a in apps {
            if a["application_key"]["app_identifier"].text == selectedApp {
                let display = a["display_name"].text
                if !display.isEmpty { name = display }
                break
            }
        }
        recents.record(deviceId: selectedDevice, appIdentifier: selectedApp,
                       name: name)
        recents.save(to: recentsPath)
    }

    func toggleFavourite(_ target: RecentTarget) {
        recents.setFavourite(target.id, !target.favourite)
        recents.save(to: recentsPath)
    }

    func forgetRecent(_ target: RecentTarget) {
        recents.forget(target.id)
        recents.save(to: recentsPath)
    }

    /// Where a remembered target stands against the current enumeration.
    ///
    /// `didEnumerate` is false when no listing has been fetched, which is a
    /// third state: an app missing from a listing and an app nobody listed
    /// are different facts.
    func presenceOf(_ target: RecentTarget) -> RecentPresence {
        let ids = Set(apps.map { $0["application_key"]["app_identifier"].text })
        let enumerated = !appsDoc.isNull && appsDoc["enumeration_failed"].bool != true
        return presence(of: target, identifiers: ids, didEnumerate: enumerated)
    }
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
            self.devicesLoadedAt = Date()
            // Selection is preserved across a refresh, and never silently
            // switched to a different device.
            if self.selectedDevice.isEmpty ||
                !self.usableDevices.contains(where: {
                    $0["device_id"].text == self.selectedDevice }) {
                // The device remembered from last time wins over whichever
                // one discovery happened to return first.
                if !self.applyRememberedDevice() {
                    self.selectedDevice = self.usableDevices.first?["device_id"].text ?? ""
                }
            }
        }
    }

    var bootTargets: [JSON] { bootTargetsDoc["boot_targets"].array }

    /// Loads the list once. Listing spawns `emulator -list-avds` and asks
    /// simctl, so it is not something to redo on every tab visit -- but a
    /// panel that shows nothing until clicked is a feature nobody finds.
    func loadBootTargetsIfNeeded() {
        guard bootTargetsDoc.isNull else { return }
        loadBootTargets()
    }

    func loadBootTargets() {
        run("Listing bootable devices…", { Core.bootTargets() }) {
            self.bootTargetsDoc = $0
            self.bootTargetsLoadedAt = Date()
        }
    }

    /// Starts a simulator or emulator, then refreshes discovery so the new
    /// device appears -- or does not, which the result says either way.
    func bootTarget(_ identifier: String) {
        guard !identifier.isEmpty else { return }
        lastBoot = .null
        Core.resetCancel()
        run("Starting \(identifier)…", {
            // Three minutes: a cold emulator boot took about 30 s on the
            // machine this was built on, and a second one did not finish in
            // 150 s. A budget that is too short reports a working boot as a
            // failure.
            Core.boot(identifier: identifier, readyTimeoutSeconds: 180)
        }) { doc in
            self.lastBoot = doc
            // Refresh regardless: a boot that did not confirm ready may still
            // have produced a device, and discovery is what settles that.
            self.loadDevices()
            self.loadBootTargets()
        }
    }

    /// Refreshes both lists.
    ///
    /// The two used to be separate buttons on the same screen -- one labelled
    /// "Refresh" next to the bootable devices, another in the toolbar -- and
    /// they refreshed different things, with nothing saying which. Pressing
    /// the nearer one after starting an emulator left the device list exactly
    /// as stale as before.
    func refreshDeviceViews() {
        loadDevices()
        loadBootTargets()
    }

    private var deviceWatch: Timer? = nil

    /// Starts re-scanning while the Devices tab is on screen.
    ///
    /// Five seconds: long enough that two child processes per tick are
    /// nothing, short enough that a device you just started appears while you
    /// are still looking at the screen.
    func startDeviceWatch() {
        guard deviceWatch == nil else { return }
        let timer = Timer(timeInterval: 5.0, repeats: true) { [weak self] _ in
            // The timer is added to RunLoop.main below, so this closure does
            // run on the main actor -- the compiler just cannot see it from
            // here. Asserting it is more honest than an async hop that would
            // let a tick land after `stopDeviceWatch`.
            MainActor.assumeIsolated { self?.deviceWatchTick() }
        }
        // `.common` rather than the default mode: a timer in the default mode
        // stops firing while a menu or a scroll is tracking, which is exactly
        // when someone is hunting for a device that has not appeared.
        RunLoop.main.add(timer, forMode: .common)
        deviceWatch = timer
    }

    func stopDeviceWatch() {
        deviceWatch?.invalidate()
        deviceWatch = nil
    }

    private func deviceWatchTick() {
        guard watchDevices else { return }
        // Every reason a tick does not scan is named, because a silent skip
        // leaves the label claiming a scan that is not happening.
        if liveRunning || liveStarting {
            // The host's CPU is part of what is being measured then, and
            // nobody hunts for devices mid-capture.
            deviceWatchSuppressedBy = "a live capture is running"
            return
        }
        if let busy = inFlight.last {
            // Discovery is serialised on one queue, so a tick would only
            // queue behind this and arrive in a burst. A boot holds the queue
            // for up to three minutes, which is exactly when someone is
            // watching for a device to appear -- so it is said, not hidden.
            deviceWatchSuppressedBy = busy.lowercased()
            return
        }
        deviceWatchSuppressedBy = ""
        rescanDevicesQuietly()
    }

    /// Re-scans without touching the status line, and publishes only when the
    /// answer actually changed.
    ///
    /// Quiet on purpose. A visible "Discovering devices…" every five seconds
    /// would make the status line useless for the operations that matter, and
    /// republishing an identical list would churn the view for no reason. What
    /// the operator wants from a poll is the moment it *differs*.
    private func rescanDevicesQuietly() {
        let sims = includeSimulators
        Self.coreQueue.async {
            let doc = Core.devices(includeSimulators: sims)
            DispatchQueue.main.async {
                // A failed scan is not an empty device list. Keep what was
                // last known, record that the attempt happened, and let the
                // visible age carry the fact that it is no fresher.
                if let err = doc["error"].string, !err.isEmpty {
                    self.lastError = err
                    return
                }
                let before = DeviceFreshness.fingerprint(self.devicesDoc)
                let after = DeviceFreshness.fingerprint(doc)
                self.devicesLoadedAt = Date()
                guard before != after else { return }
                self.devicesDoc = doc
                if self.selectedDevice.isEmpty ||
                    !self.usableDevices.contains(where: {
                        $0["device_id"].text == self.selectedDevice }) {
                    self.selectedDevice = self.usableDevices.first?["device_id"].text ?? ""
                }
                // A device appearing or leaving changes what can be started,
                // so the boot list is no longer right either.
                self.loadBootTargets()
            }
        }
    }

    /// Advice for the device the operator asked about, keyed by device id so
    /// two rows cannot show each other's answer.
    @Published var deviceAdvice: [String: JSON] = [:]

    /// Asks why a device cannot be used. Runs a live probe, so it is on
    /// demand rather than for every row in the list.
    func explainDevice(_ deviceId: String) {
        guard !deviceId.isEmpty else { return }
        run("Checking \(deviceId)…", {
            Core.deviceAdvice(deviceId: deviceId, probe: true)
        }) { doc in
            self.deviceAdvice[deviceId] = doc
        }
    }

    func loadInspectTargetsIfNeeded() {
        guard inspectTargetsDoc.isNull else { return }
        loadInspectTargets()
    }

    func loadInspectTargets() {
        run("Listing attachable apps…", { Core.inspectTargets(metroPort: 8081) }) {
            self.inspectTargetsDoc = $0
        }
    }

    /// Observes for the configured window.
    ///
    /// The screenshot directory sits beside the sessions, because the images
    /// are part of what a capture produced and belong with it rather than in
    /// a temporary folder that the next reboot clears.
    func runInspect() {
        guard !selectedApp.isEmpty else { return }
        let app = selectedApp
        let seconds = inspectSeconds
        let redux = inspectRedux
        let values = inspectReduxValues
        let watch = inspectReduxWatch
        let actions = inspectReduxActions
        let shots = inspectScreenshots
        let device = selectedDevice
        let hint = inspectTargetDevice
        let detail = inspectDetail
        let dir = sessionsDir + "/inspect-shots"
        if shots {
            try? FileManager.default.createDirectory(
                atPath: dir, withIntermediateDirectories: true)
        }
        Core.resetCancel()
        inspectDocCapturedDetail = detail
        selectedRequestId = ""
        run("Observing \(app) for \(seconds)s…", {
            Core.inspect(appId: app, seconds: seconds, metroPort: 8081,
                         redux: redux, reduxValues: values,
                         reduxWatch: watch, reduxActions: actions,
                         screenshots: shots, deviceId: device,
                         screenshotDir: dir, targetDevice: hint,
                         detail: detail)
        }) { doc in
            self.inspectDoc = doc
            // A capture changes what is attachable -- the app may have
            // reloaded and taken a new target id with it.
            self.loadInspectTargets()
        }
    }

    /// Hides everything currently in one of the three lists.
    ///
    /// Nothing is discarded; see InspectFilter.afterClear. Each list is
    /// marked separately because they answer different questions and the
    /// reason to clear one is rarely a reason to clear the others.
    func clearInspect(_ kind: InspectKind) {
        switch kind {
        case .network:
            clearedNetwork = inspectDoc["network"].array.count
        case .log:
            clearedConsole = inspectDoc["console"].array.count
        case .redux:
            // The highest seq present, so a record that arrives mid-clear is
            // kept rather than silently swallowed.
            clearedReduxSeq = inspectDoc["redux"]["records"].array
                .compactMap { $0["seq"].int }.max() ?? clearedReduxSeq
        }
    }

    /// Puts back what a clear is holding.
    func unclearInspect(_ kind: InspectKind) {
        switch kind {
        case .network: clearedNetwork = 0
        case .log: clearedConsole = 0
        case .redux: clearedReduxSeq = 0
        }
    }

    private var inspectTimer: Timer? = nil

    /// Starts watching the app live.
    ///
    /// The window-based `runInspect` is kept for a fixed-length observation;
    /// this is the one a person uses, because the interesting API calls
    /// happen when they tap something and a report fifteen seconds later
    /// cannot be connected to what they just did.
    func startInspectStream() {
        guard !selectedApp.isEmpty, !inspectStreaming else { return }
        let app = selectedApp
        let redux = inspectRedux
        let values = inspectReduxValues
        let watch = inspectReduxWatch
        let actions = inspectReduxActions
        let shots = inspectScreenshots
        let device = selectedDevice
        let hint = inspectTargetDevice
        let detail = inspectDetail
        let dir = sessionsDir + "/inspect-shots"
        if shots {
            try? FileManager.default.createDirectory(
                atPath: dir, withIntermediateDirectories: true)
        }
        inspectDisconnect = ""
        inspectDocCapturedDetail = detail
        selectedRequestId = ""
        run("Attaching to \(app)…", {
            Core.inspectStreamStart(appId: app, metroPort: 8081,
                                    redux: redux, reduxValues: values,
                                    reduxWatch: watch, reduxActions: actions,
                                    screenshots: shots, deviceId: device,
                                    screenshotDir: dir,
                                    targetDevice: hint, detail: detail)
        }) { doc in
            if doc["attached"].bool != true {
                self.inspectDoc = .null
                return
            }
            self.inspectDoc = doc["report"]
            self.inspectStreaming = true
            self.beginInspectPolling()
        }
    }

    private func beginInspectPolling() {
        guard inspectTimer == nil else { return }
        // Twice a second: fast enough that a tap and its request feel
        // connected, slow enough that the socket read is not the app's main
        // activity.
        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.pollInspectStream() }
        }
        RunLoop.main.add(timer, forMode: .common)
        inspectTimer = timer
    }

    private func pollInspectStream() {
        guard inspectStreaming else { return }
        // Off the main thread: the poll reads a socket, and a UI that blocks
        // on it would stutter exactly while the app is being used.
        Self.coreQueue.async {
            let doc = Core.inspectStreamPoll(budgetMs: 250)
            DispatchQueue.main.async {
                // Keep the last good report if a poll came back empty:
                // replacing it with null would blank the screen mid-session.
                if !doc["report"].isNull { self.inspectDoc = doc["report"] }
                if doc["running"].bool == false {
                    // The app closed the connection. Stop polling and keep
                    // what arrived; the report says it ended early.
                    self.inspectDisconnect = doc["disconnect_reason"].text
                    self.stopInspectStream(keepReport: true)
                }
            }
        }
    }

    func stopInspectStream(keepReport: Bool = true) {
        inspectTimer?.invalidate()
        inspectTimer = nil
        guard inspectStreaming else { return }
        inspectStreaming = false
        Self.coreQueue.async {
            let doc = Core.inspectStreamStop()
            DispatchQueue.main.async {
                if keepReport, !doc["report"].isNull {
                    self.inspectDoc = doc["report"]
                }
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

    /// Copies the open session's stored report to a path the user picked.
    ///
    /// The package's own report is copied rather than regenerated, so what
    /// leaves is what was recorded. Nothing is uploaded: this writes a local
    /// file, which is the only thing this app can do with a report.
    func exportSession(format: String, to path: String) {
        guard !selectedSession.isEmpty else {
            lastError = "Open a session first."
            return
        }
        let dir = sessionsDir, id = selectedSession
        run("Exporting \(id)…", {
            Core.exportSession(dir: dir, id: id, format: format, outPath: path)
        }) { doc in
            if let written = doc["written"].string, !written.isEmpty {
                self.lastExport = "Wrote \(doc["bytes"].stamp) bytes to \(written)"
            }
        }
    }

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
            // The Timeline view renders this error itself, with room for the
            // whole explanation. Leaving it in `lastError` too printed the
            // same paragraph twice, once squeezed into the banner strip.
            if !$0["error"].text.isEmpty { self.lastError = nil }
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
        rememberCurrentTarget()
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
        rememberCurrentTarget()
        let dir = sessionsDir, dev = selectedDevice, app = selectedApp
        let hz = recordHz, f = recordFrames, c = recordCpu, m = recordMemory
        let r = recordResetFrames, tick = liveTickMs, win = liveCpuWindowMs
        let stackSecs = liveStackProfileSeconds

        liveStarting = true
        lastError = nil
        liveStopResult = nil
        liveSeries = [:]
        liveSnapshot = .null

        Self.coreQueue.async {
            let started = Core.liveStart(dir: dir, device: dev, app: app,
                                         sampleHz: hz, frames: f, cpu: c,
                                         memory: m, resetFrames: r,
                                         tickMs: tick, cpuWindowMs: win,
                                         stackProfileSeconds: stackSecs)
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
