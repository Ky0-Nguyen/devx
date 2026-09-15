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

    /// What is attachable right now. Cheap, and it does not hold the single
    /// debugger slot the way a capture does.
    static func inspectTargets(metroPort: Int) -> JSON {
        call { mpi_inspect_targets_json(Int32(metroPort)) }
    }

    /// Opens a live observation. One at a time.
    static func inspectStreamStart(appId: String, metroPort: Int,
                                   redux: Bool, reduxValues: Bool,
                                   screenshots: Bool, deviceId: String,
                                   screenshotDir: String) -> JSON {
        var flags: Int32 = 0
        if redux { flags |= 1 }
        if reduxValues { flags |= 3 }
        if screenshots { flags |= 4 }
        return call {
            mpi_inspect_stream_start(appId, Int32(metroPort), flags, deviceId,
                                     screenshotDir)
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
                        screenshots: Bool, deviceId: String,
                        screenshotDir: String) -> JSON {
        var flags: Int32 = 0
        if redux { flags |= 1 }
        if reduxValues { flags |= 3 }     // values imply reading the store
        if screenshots { flags |= 4 }
        return call {
            mpi_inspect_json(appId, Int32(seconds), Int32(metroPort), flags,
                             deviceId, screenshotDir)
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
    static func liveStart(dir: String, device: String, app: String,
                          sampleHz: Int, frames: Bool, cpu: Bool, memory: Bool,
                          resetFrames: Bool, tickMs: Int, cpuWindowMs: Int) -> JSON {
        call {
            mpi_live_start(dir, device, app, Int32(sampleHz),
                           frames ? 1 : 0, cpu ? 1 : 0, memory ? 1 : 0,
                           resetFrames ? 1 : 0, Int32(tickMs),
                           Int32(cpuWindowMs), 45000)
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
