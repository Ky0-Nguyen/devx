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

    static func sessions(dir: String) -> JSON { call { mpi_sessions_json(dir) } }

    static func session(dir: String, id: String) -> JSON {
        call { mpi_session_json(dir, id) }
    }

    static func timeline(dir: String, id: String, bins: Int) -> JSON {
        call { mpi_session_timeline_json(dir, id, Int32(bins)) }
    }

    static func record(dir: String, device: String, app: String,
                       durationSeconds: Int, sampleHz: Int, frames: Bool,
                       cpu: Bool, memory: Bool, resetFrames: Bool) -> JSON {
        call {
            mpi_record_json(dir, device, app, Int32(durationSeconds),
                            Int32(sampleHz), frames ? 1 : 0, cpu ? 1 : 0,
                            memory ? 1 : 0, resetFrames ? 1 : 0, 120000)
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
