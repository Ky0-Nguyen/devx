import Foundation
import MPICore

/// A dynamic JSON value.
///
/// The core already serialises everything, and its documents are wide and
/// evolving. Decoding into this rather than into exhaustive `Codable` structs
/// means a new field in the engine shows up in the UI without a parallel Swift
/// type to maintain -- and, more importantly, a field the engine deliberately
/// leaves `null` stays distinguishable from one that is absent.
enum JSON: Sendable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSON])
    case object([String: JSON])

    init(_ any: Any?) {
        switch any {
        case nil, is NSNull:
            self = .null
        case let n as NSNumber:
            // JSONSerialization returns NSNumber for both numbers and
            // booleans, and `NSNumber(0) as? Bool` *succeeds* as false. Casting
            // to Bool first therefore turned every 0 into `false` and every 1
            // into `true` -- "0 counters" rendered as "false counters". Only
            // CFBoolean is genuinely a boolean, so ask the type directly.
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                self = .bool(n.boolValue)
            } else {
                self = .number(n.doubleValue)
            }
        case let b as Bool:
            self = .bool(b)
        case let s as String:
            self = .string(s)
        case let a as [Any]:
            self = .array(a.map(JSON.init))
        case let d as [String: Any]:
            self = .object(d.mapValues(JSON.init))
        default:
            self = .null
        }
    }

    /// Back to Foundation types, and to text, so a document the core handed
    /// over can be handed back -- to be kept as an observation, for one.
    var foundationValue: Any {
        switch self {
        case .null: return NSNull()
        case .bool(let b): return b
        case .number(let n): return n
        case .string(let s): return s
        case .array(let a): return a.map { $0.foundationValue }
        case .object(let o): return o.mapValues { $0.foundationValue }
        }
    }

    func serialized() -> String {
        guard let data = try? JSONSerialization.data(
                withJSONObject: foundationValue, options: [.fragmentsAllowed]),
              let text = String(data: data, encoding: .utf8) else { return "null" }
        return text
    }

    static func parse(_ text: String) -> JSON {
        guard let data = text.data(using: .utf8),
              let any = try? JSONSerialization.jsonObject(
                with: data, options: [.fragmentsAllowed]) else { return .null }
        return JSON(any)
    }

    subscript(_ key: String) -> JSON {
        if case .object(let d) = self, let v = d[key] { return v }
        return .null
    }
    subscript(_ index: Int) -> JSON {
        if case .array(let a) = self, index >= 0, index < a.count { return a[index] }
        return .null
    }

    /// True only when the key is genuinely absent. A present `null` is not
    /// missing -- the whole unknown-versus-false model depends on the
    /// difference, so the UI must be able to ask.
    func isAbsent(_ key: String) -> Bool {
        if case .object(let d) = self { return d[key] == nil }
        return true
    }
    var isNull: Bool { if case .null = self { return true }; return false }

    var string: String? { if case .string(let s) = self { return s }; return nil }
    var double: Double? { if case .number(let n) = self { return n }; return nil }
    var int: Int? { double.map { Int($0) } }
    var bool: Bool? {
        if case .bool(let b) = self { return b }
        return nil
    }
    var array: [JSON] { if case .array(let a) = self { return a }; return [] }

    /// An object's keys, sorted. Sorted because these are rendered as a list
    /// and dictionary order is not stable between runs -- an unsorted header
    /// list would reshuffle itself on every poll.
    var keys: [String] {
        if case .object(let d) = self { return d.keys.sorted() }
        return []
    }
    var text: String { string ?? "" }
    /// For display: a value the engine left unknown reads as such rather than
    /// as an empty cell that could be mistaken for zero.
    func display(_ fallback: String = "unknown") -> String {
        switch self {
        case .null: return fallback
        case .string(let s): return s.isEmpty ? fallback : s
        case .bool(let b): return b ? "true" : "false"
        case .number(let n):
            return n == n.rounded() && abs(n) < 1e15
                ? String(Int(n)) : String(format: "%.3f", n)
        default: return fallback
        }
    }
}

/// The bridge to the C++ core.
///
/// Every call is blocking, so callers run them off the main actor. The core's
/// operations are the slow part (device tooling, captures), not the bridge.
enum Core {
    private static func call(_ fn: () -> UnsafeMutablePointer<CChar>?) -> JSON {
        guard let raw = fn() else {
            return .object(["error": .string("the core returned no document")])
        }
        defer { mpi_string_free(raw) }
        return JSON.parse(String(cString: raw))
    }

    static func version() -> JSON { call { mpi_version_json() } }

    static func devices(includeSimulators: Bool, timeoutMs: Int32 = 20000) -> JSON {
        call { mpi_devices_json(includeSimulators ? 1 : 0, timeoutMs) }
    }

    static func apps(device: String, includeSimulators: Bool,
                     timeoutMs: Int32 = 25000) -> JSON {
        call { mpi_apps_json(device, includeSimulators ? 1 : 0, timeoutMs) }
    }

    static func preflight(device: String, app: String, includeSimulators: Bool,
                          timeoutMs: Int32 = 45000) -> JSON {
        call { mpi_preflight_json(device, app, includeSimulators ? 1 : 0, timeoutMs) }
    }

    static func rules() -> JSON { call { mpi_rules_json() } }

    static func bootTargets() -> JSON { call { mpi_boot_targets_json() } }

    /// Starts a simulator or emulator. Blocks until the device answers or the
    /// budget runs out, which is why the caller runs it off the main thread.
    static func boot(identifier: String, readyTimeoutSeconds: Int) -> JSON {
        call { mpi_boot_json(identifier, Int32(readyTimeoutSeconds)) }
    }

    /// Why an unusable device is unusable, and what to do about it.
    /// `probe` runs a live reachability check (~0.1s).
    static func deviceAdvice(deviceId: String, probe: Bool) -> JSON {
        call { mpi_device_advice_json(deviceId, probe ? 1 : 0) }
    }

    /// One layout snapshot of an app's screen. With `relaunch` the app is
    /// restarted with the iOS layout probe injected, and its state is lost,
    /// so the caller asks first. Blocks; run it off the main thread.
    static func layout(device: String, app: String, relaunch: Bool,
                       settleMs: Int, includeTree: Bool,
                       includeSimulators: Bool, saveTo: String,
                       timeoutMs: Int32 = 20000) -> JSON {
        call {
            mpi_layout_json(device, app, relaunch ? 1 : 0, Int32(settleMs),
                            includeTree ? 1 : 0, includeSimulators ? 1 : 0, timeoutMs,
                            saveTo)
        }
    }

    // ---- the window's control endpoint (AI tools through `mpi mcp`) ----

    static func controlStart(dir: String) -> JSON { call { mpi_control_start_json(dir) } }
    static func controlStop() { mpi_control_stop() }
    static func controlSetState(_ json: String) { mpi_control_set_state_json(json) }
    static func controlNext() -> JSON { call { mpi_control_next_json() } }
    static func controlReply(id: Int, _ json: String) { mpi_control_reply(Int32(id), json) }
    static func observation(dir: String, id: String) -> JSON {
        call { mpi_observation_json(dir, id) }
    }

    // ---- BrowserStack ----

    static func bsStatus() -> JSON { call { mpi_bs_status_json() } }
    static func bsGet(_ path: String) -> JSON { call { mpi_bs_get_json(path) } }
    static func bsUpload(product: String, file: String) -> JSON {
        call { mpi_bs_upload_json(product, file) }
    }
    static func bsLiveURL(os: String, version: String, device: String, appURL: String) -> JSON {
        call { mpi_bs_live_url_json(os, version, device, appURL) }
    }
    static func bsSave(sessionsDir: String, what: String, json: String) -> JSON {
        call { mpi_bs_save_json(sessionsDir, what, json) }
    }
    static func bsImportProfiling(sessionsDir: String, buildID: String, sessionID: String) -> JSON {
        call { mpi_bs_import_profiling_json(sessionsDir, buildID, sessionID) }
    }
    static func bsAutomateStart(_ spec: JSON) -> JSON {
        call { mpi_bs_automate_start_json(spec.serialized()) }
    }
    static func bsAutomateScreenshot(_ session: String, to path: String) -> JSON {
        call { mpi_bs_automate_screenshot_json(session, path) }
    }
    static func bsAutomateInput(_ session: String, _ action: JSON) -> JSON {
        call { mpi_bs_automate_input_json(session, action.serialized()) }
    }
    static func bsAutomateStop(_ session: String, importTo: String) -> JSON {
        call { mpi_bs_automate_stop_json(session, importTo) }
    }
    static func bsLocalStatus() -> JSON { call { mpi_bs_local_status_json() } }

    // ---- Intelligence (core/capi/mpi_capi_intelligence.cpp) ----
    static func intelProviders() -> JSON { call { mpi_intelligence_providers_json() } }
    static func intelEgress(_ dir: String) -> JSON { call { mpi_intelligence_egress_json(dir) } }
    static func intelWorkspaces(_ dir: String) -> JSON { call { mpi_intelligence_workspaces_json(dir) } }
    static func intelSaveWorkspace(_ dir: String, _ w: JSON) -> JSON {
        call { mpi_intelligence_workspace_save_json(dir, w.serialized()) }
    }
    static func intelDeleteWorkspace(_ dir: String, _ ws: String) -> JSON {
        call { mpi_intelligence_workspace_delete_json(dir, ws) }
    }
    static func intelIntegrations(_ dir: String, _ ws: String) -> JSON {
        call { mpi_intelligence_integrations_json(dir, ws) }
    }
    static func intelSaveConnector(_ dir: String, _ ws: String, _ c: JSON) -> JSON {
        call { mpi_intelligence_connector_save_json(dir, ws, c.serialized()) }
    }
    static func intelRemoveConnector(_ dir: String, _ ws: String, _ id: String, deleteLocal: Bool) -> JSON {
        call { mpi_intelligence_connector_remove_json(dir, ws, id, deleteLocal ? 1 : 0) }
    }
    static func intelValidate(_ dir: String, _ ws: String, _ id: String) -> JSON {
        call { mpi_intelligence_validate_json(dir, ws, id) }
    }
    static func intelDiscover(_ dir: String, _ ws: String, _ id: String) -> JSON {
        call { mpi_intelligence_discover_json(dir, ws, id) }
    }
    static func intelSync(_ dir: String, _ ws: String, _ id: String) -> JSON {
        call { mpi_intelligence_sync_json(dir, ws, id) }
    }
    static func intelOverview(_ dir: String, _ ws: String) -> JSON {
        call { mpi_intelligence_overview_json(dir, ws) }
    }
    static func intelSignals(_ dir: String, _ ws: String, _ query: JSON) -> JSON {
        call { mpi_intelligence_signals_json(dir, ws, query.serialized()) }
    }
    static func intelSignal(_ dir: String, _ ws: String, _ id: String, raw: Bool) -> JSON {
        call { mpi_intelligence_signal_json(dir, ws, id, raw ? 1 : 0) }
    }
    static func intelReleases(_ dir: String, _ ws: String) -> JSON {
        call { mpi_intelligence_releases_json(dir, ws) }
    }
    static func intelRelease(_ dir: String, _ ws: String, _ key: String) -> JSON {
        call { mpi_intelligence_release_json(dir, ws, key) }
    }
    static func intelCompare(_ dir: String, _ ws: String, _ base: String, _ cand: String) -> JSON {
        call { mpi_intelligence_compare_json(dir, ws, base, cand) }
    }
    static func intelRelated(_ dir: String, _ ws: String, _ id: String) -> JSON {
        call { mpi_intelligence_related_json(dir, ws, id) }
    }
    static func intelCodeContext(_ dir: String, _ ws: String, _ id: String) -> JSON {
        call { mpi_intelligence_code_context_json(dir, ws, id) }
    }
    static func intelPack(_ dir: String, _ ws: String, _ scope: JSON) -> JSON {
        call { mpi_intelligence_evidence_pack_json(dir, ws, scope.serialized()) }
    }
    static func intelRetention(_ dir: String, _ ws: String, apply: Bool) -> JSON {
        call { mpi_intelligence_retention_json(dir, ws, apply ? 1 : 0) }
    }
    static func intelPin(_ dir: String, _ ws: String, kind: String, id: String, pinned: Bool) -> JSON {
        call { mpi_intelligence_pin_json(dir, ws, kind, id, pinned ? 1 : 0) }
    }
    static func intelLinkSession(_ dir: String, _ ws: String, session: String, release: String,
                                 linked: Bool) -> JSON {
        call { mpi_intelligence_link_session_json(dir, ws, session, release, linked ? 1 : 0) }
    }
    static func bsLocalInstall() -> JSON { call { mpi_bs_local_install_json() } }
    static func bsLocalStart(_ id: String) -> JSON { call { mpi_bs_local_start_json(id) } }
    static func bsLocalStop(_ id: String) -> JSON { call { mpi_bs_local_stop_json(id) } }

    // ---- Android emulator ----

    static func androidSdk() -> JSON { call { mpi_android_sdk_json() } }
    static func androidCatalog() -> JSON { call { mpi_android_catalog_json(90000) } }
    static func androidAcceptLicense(_ id: String) -> JSON {
        call { mpi_android_accept_license_json(id) }
    }
    static func androidInstallStart(_ path: String) -> JSON {
        call { mpi_android_install_start_json(path) }
    }
    static func androidInstallPoll() -> JSON { call { mpi_android_install_poll_json() } }
    static func androidInstallCancel() { mpi_android_install_cancel() }
    static func androidPresets() -> JSON { call { mpi_android_presets_json() } }
    static func avdCreate(name: String, image: String, preset: String, width: Int,
                          height: Int, density: Int, ramMb: Int) -> JSON {
        call {
            mpi_avd_create_json(name, image, preset, Int32(width), Int32(height),
                                Int32(density), Int32(ramMb))
        }
    }
    static func avdDelete(_ id: String) -> JSON { call { mpi_avd_delete_json(id) } }
    static func avdResize(_ id: String, width: Int, height: Int, density: Int,
                          override: Bool) -> JSON {
        call { mpi_avd_resize_json(id, Int32(width), Int32(height), Int32(density), override ? 1 : 0) }
    }
    static func emulatorStart(_ id: String, cold: Bool) -> JSON {
        call { mpi_emulator_start_json(id, cold ? 1 : 0, 0) }
    }
    static func emulatorStop(_ id: String) -> JSON { call { mpi_emulator_stop_json(id) } }
    static func displayOpen(_ id: String, box: Int) -> JSON {
        call { mpi_emulator_display_open_json(id, Int32(box)) }
    }
    /// Closes one session, or every session with 0.
    static func displayClose(_ handle: Int) { mpi_emulator_display_close(Int32(handle)) }
    /// The latest frame of a session, or nil once it has ended.
    static func displayFrame(_ handle: Int) -> (seq: UInt32, width: UInt32, height: UInt32)? {
        var seq: UInt32 = 0, w: UInt32 = 0, h: UInt32 = 0
        guard mpi_emulator_display_frame(Int32(handle), &seq, &w, &h) != 0 else { return nil }
        return (seq, w, h)
    }
    static func touch(_ handle: Int, x: Int, y: Int, pressure: Int) {
        mpi_emulator_touch(Int32(handle), Int32(x), Int32(y), Int32(pressure))
    }
    static func key(_ handle: Int, _ key: String, phase: Int = 2) {
        mpi_emulator_key(Int32(handle), key, Int32(phase))
    }
    static func text(_ handle: Int, _ text: String) { mpi_emulator_text(Int32(handle), text) }
    static func rotate(_ handle: Int, _ degrees: Int) -> JSON {
        call { mpi_emulator_rotate_json(Int32(handle), Int32(degrees)) }
    }
    static func extendedControls(_ handle: Int, pane: Int) -> JSON {
        call { mpi_emulator_extended_controls_json(Int32(handle), Int32(pane)) }
    }
    static func emulatorStatus(_ handle: Int) -> JSON { call { mpi_emulator_status_json(Int32(handle)) } }
    static func emulatorScreenshot(_ handle: Int, sessionsDir: String) -> JSON {
        call { mpi_emulator_screenshot_json(Int32(handle), sessionsDir) }
    }

    /// Keeps a result on disk as an observation, where `mpi mcp` reads it.
    static func saveObservation(sessionsDir: String, kind: String, app: String,
                                device: String, summary: String,
                                document: String) -> JSON {
        call {
            mpi_save_observation_json(sessionsDir, kind, app, device, summary, document)
        }
    }

    /// What is attachable right now. Cheap, and it does not hold the single
    /// debugger slot the way a capture does.
    static func inspectTargets(metroPort: Int) -> JSON {
        call { mpi_inspect_targets_json(Int32(metroPort)) }
    }

    /// Opens a live observation. One at a time.
    static func inspectStreamStart(appId: String, metroPort: Int,
                                   redux: Bool, reduxValues: Bool,
                                   reduxWatch: Bool, reduxActions: Bool,
                                   screenshots: Bool, deviceId: String,
                                   screenshotDir: String,
                                   targetDevice: String,
                                   detail: Bool) -> JSON {
        var flags: Int32 = 0
        if redux { flags |= 1 }
        if reduxValues { flags |= 3 }
        if reduxWatch { flags |= 16 }
        if reduxActions { flags |= 48 }   // wrapping implies watching
        if screenshots { flags |= 4 }
        if detail { flags |= 8 }
        return call {
            mpi_inspect_stream_start(appId, Int32(metroPort), flags, deviceId,
                                     screenshotDir, targetDevice)
        }
    }

    /// Reads for up to `budgetMs`, then returns the observation so far.
    /// Cumulative, not a delta.
    static func inspectStreamPoll(budgetMs: Int) -> JSON {
        call { mpi_inspect_stream_poll(Int32(budgetMs)) }
    }

    static func inspectStreamStop() -> JSON {
        call { mpi_inspect_stream_stop() }
    }

    /// Reads a running app's network calls, console output and Redux state
    /// through the inspector the app already runs. Blocks for the whole
    /// window, so the caller runs it off the main thread.
    static func inspect(appId: String, seconds: Int, metroPort: Int,
                        redux: Bool, reduxValues: Bool,
                        reduxWatch: Bool, reduxActions: Bool,
                        screenshots: Bool, deviceId: String,
                        screenshotDir: String, targetDevice: String,
                        detail: Bool) -> JSON {
        var flags: Int32 = 0
        if redux { flags |= 1 }
        if reduxValues { flags |= 3 }     // values imply reading the store
        if reduxWatch { flags |= 16 }
        if reduxActions { flags |= 48 }   // wrapping implies watching
        if screenshots { flags |= 4 }
        if detail { flags |= 8 }
        return call {
            mpi_inspect_json(appId, Int32(seconds), Int32(metroPort), flags,
                             deviceId, screenshotDir, targetDevice)
        }
    }

    static func sessions(dir: String) -> JSON { call { mpi_sessions_json(dir) } }

    static func session(dir: String, id: String) -> JSON {
        call { mpi_session_json(dir, id) }
    }

    /// The project's suppression list. Kept beside the sessions directory so
    /// the CLI's `--suppressions` and this app point at the same file: a
    /// suppression only one of them can see is not a project decision.
    static func suppressionsPath(sessionsDir: String) -> String {
        sessionsDir + "/suppressions.json"
    }

    static func exportSession(dir: String, id: String, format: String,
                              outPath: String) -> JSON {
        call { mpi_export_session_json(dir, id, format, outPath) }
    }

    static func suppressions(path: String) -> JSON {
        call { mpi_suppressions_json(path) }
    }

    static func addSuppression(path: String, ruleId: String,
                               fingerprint: String, reason: String,
                               expiry: String, author: String,
                               reference: String) -> JSON {
        call {
            mpi_add_suppression_json(path, ruleId, fingerprint, reason, expiry,
                                     author, reference)
        }
    }

    static func removeSuppression(path: String, ruleId: String,
                                  fingerprint: String) -> JSON {
        call { mpi_remove_suppression_json(path, ruleId, fingerprint) }
    }

    static func reanalyze(dir: String, id: String,
                          suppressionsPath: String) -> JSON {
        call { mpi_reanalyze_session_json(dir, id, suppressionsPath) }
    }

    static func timeline(dir: String, id: String, bins: Int) -> JSON {
        call { mpi_session_timeline_json(dir, id, Int32(bins)) }
    }

    /// Zero means "unspecified" for every threshold: the engine's own
    /// default applies, rather than a gate of zero that would pass anything.
    /// Whether this app can actually read `path`, decided with a deadline.
    ///
    /// macOS gates the protected folders -- Documents, Desktop, Downloads,
    /// iCloud Drive -- behind a consent prompt, and an ad-hoc signed app that
    /// cannot present one blocks inside `open()` indefinitely. A spinner that
    /// never ends is a worse answer than naming the problem, so the read is
    /// probed on its own thread with a deadline.
    ///
    /// `nil` means the probe did not finish: not readable, not unreadable,
    /// unknown. The caller says so rather than picking one.
    static func probeReadable(_ path: String,
                              timeout: TimeInterval = 2.0) -> Bool? {
        guard !path.isEmpty else { return false }
        let sem = DispatchSemaphore(value: 0)
        // The result is written on the probe thread and read here only after
        // the semaphore reports completion, so no lock is needed -- and on a
        // timeout it is never read at all.
        final class Box { var value = false }
        let box = Box()
        Thread.detachNewThread {
            if let h = FileHandle(forReadingAtPath: path) {
                // Reading a byte, not just opening: a handle can be granted
                // and the first read still be the thing that blocks.
                box.value = ((try? h.read(upToCount: 1)) != nil)
                try? h.close()
            }
            sem.signal()
        }
        return sem.wait(timeout: .now() + timeout) == .success
               ? box.value : nil
    }

    static func compare(baseline: String, candidate: String,
                        minRuns: Int = 0, minRelativeDelta: Double = 0,
                        minAbsoluteDelta: Double = 0,
                        maxRelativeSpread: Double = 0) -> JSON {
        call {
            mpi_compare_json(baseline, candidate, Int32(minRuns),
                             minRelativeDelta, minAbsoluteDelta,
                             maxRelativeSpread)
        }
    }

    /// `scheduling` and `heap` are the heavier collectors. The caller is
    /// expected to have told the user what they cost first -- the Record tab
    /// does, and will not enable either until it has been read.
    static func record(dir: String, device: String, app: String,
                       durationSeconds: Int, sampleHz: Int, frames: Bool,
                       cpu: Bool, memory: Bool, resetFrames: Bool,
                       scheduling: Bool = false, heap: Bool = false) -> JSON {
        call {
            mpi_record_json(dir, device, app, Int32(durationSeconds),
                            Int32(sampleHz), frames ? 1 : 0, cpu ? 1 : 0,
                            memory ? 1 : 0, resetFrames ? 1 : 0,
                            scheduling ? 1 : 0, heap ? 1 : 0,
                            // A heap dump adds a whole-heap walk and a pull of
                            // tens of megabytes to the capture, so the budget
                            // is raised when one was asked for.
                            heap ? 600_000 : 120_000)
        }
    }

    static func analyze(tracePath: String) -> JSON {
        call { mpi_analyze_trace_json(tracePath) }
    }

    // ---- live capture ----
    /// `stackProfileSeconds` > 0 takes a stack profile at the end of the
    /// capture (iOS simulator: `/usr/bin/sample`). It is an aggregate with no
    /// timestamps, so it answers where and never when -- which is why it is
    /// opt-in rather than part of the tick loop.
    static func liveStart(dir: String, device: String, app: String,
                          sampleHz: Int, frames: Bool, cpu: Bool, memory: Bool,
                          resetFrames: Bool, tickMs: Int, cpuWindowMs: Int,
                          stackProfileSeconds: Int = 0) -> JSON {
        call {
            mpi_live_start(dir, device, app, Int32(sampleHz),
                           frames ? 1 : 0, cpu ? 1 : 0, memory ? 1 : 0,
                           resetFrames ? 1 : 0, Int32(tickMs),
                           Int32(cpuWindowMs), 45000,
                           Int32(stackProfileSeconds))
        }
    }
    static func livePoll() -> JSON { call { mpi_live_poll_json() } }
    static func liveStop() -> JSON { call { mpi_live_stop_json() } }
    static var liveRunning: Bool { mpi_live_is_running() != 0 }

    static func cancel() { mpi_cancel_all() }
    static func resetCancel() { mpi_cancel_reset() }

    static var defaultSessionsDir: String {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".mpi/sessions").path
    }
}
