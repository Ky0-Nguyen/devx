import Foundation

/// The top-level sections of the app.
///
/// Its own type rather than a member of AppState so LaunchOptions can
/// reference it without pulling in the observable object, which lets the
/// pure layers be unit-tested on their own.
enum DevXTab: String, CaseIterable, Identifiable {
    case devices, apps, preflight, live, record, sessions, issues, threads,
         timeline, compare, detectors, settings
    var id: String { rawValue }
    var title: String {
        switch self {
        case .devices: return "Devices"
        case .apps: return "Apps"
        case .preflight: return "Preflight"
        case .live: return "Live"
        case .record: return "Record"
        case .sessions: return "Sessions"
        case .issues: return "Issues"
        case .threads: return "Threads"
        case .timeline: return "Timeline"
        case .compare: return "Compare"
        case .detectors: return "Detectors"
        case .settings: return "Export"
        }
    }
    var icon: String {
        switch self {
        case .devices: return "iphone.gen3"
        case .apps: return "square.grid.2x2"
        case .preflight: return "checklist"
        case .live: return "dot.radiowaves.left.and.right"
        case .record: return "record.circle"
        case .sessions: return "folder"
        case .issues: return "exclamationmark.magnifyingglass"
        case .threads: return "square.stack.3d.up"
        case .timeline: return "chart.bar.xaxis"
        case .compare: return "arrow.left.arrow.right"
        case .detectors: return "function"
        case .settings: return "square.and.arrow.up"
        }
    }
}
