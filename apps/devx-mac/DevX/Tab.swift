import Foundation

/// The top-level sections of the app.
///
/// Its own type rather than a member of AppState so LaunchOptions can
/// reference it without pulling in the observable object, which lets the
/// pure layers be unit-tested on their own.
enum DevXTab: String, CaseIterable, Identifiable {
    case devices, apps, preflight, live, record, inspect, layout, sessions, issues,
         threads, timeline, compare, detectors, settings, help
    var id: String { rawValue }
    /// Translated at the point of display. The `rawValue` is untouched --
    /// it is what `--tab=` accepts and what LaunchOptions parses, so it is an
    /// identifier, not a label.
    /// Wrapped case by case rather than once around a helper, so the
    /// catalog checker can see that every one of these strings reaches `tr()`
    /// -- an indirection it would have to guess at.
    var title: String {
        switch self {
        case .devices: return tr("Devices")
        case .apps: return tr("Apps")
        case .preflight: return tr("Preflight")
        case .live: return tr("Live")
        case .record: return tr("Record")
        case .inspect: return tr("Inspect")
        case .layout: return tr("Layout")
        case .sessions: return tr("Sessions")
        case .issues: return tr("Issues")
        case .threads: return tr("Threads")
        case .timeline: return tr("Timeline")
        case .compare: return tr("Compare")
        case .detectors: return tr("Detectors")
        case .settings: return tr("Export")
        case .help: return tr("Help")
        }
    }
    var icon: String {
        switch self {
        case .devices: return "iphone.gen3"
        case .apps: return "square.grid.2x2"
        case .preflight: return "checklist"
        case .live: return "dot.radiowaves.left.and.right"
        case .record: return "record.circle"
        case .inspect: return "scope"
        case .layout: return "rectangle.3.group"
        case .sessions: return "folder"
        case .issues: return "exclamationmark.magnifyingglass"
        case .threads: return "square.stack.3d.up"
        case .timeline: return "chart.bar.xaxis"
        case .compare: return "arrow.left.arrow.right"
        case .detectors: return "function"
        case .settings: return "square.and.arrow.up"
        case .help: return "questionmark.circle"
        }
    }
}
