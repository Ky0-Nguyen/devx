import Foundation

/// Which tabs the sidebar shows, and the tab the window reopens on.
///
/// Seventeen tabs is a lot to read down for someone who only records and reads
/// issues, so the sidebar shows a chosen subset. Pure and separate from
/// AppState so the rules -- defaults, a tab added by a later version, a stored
/// tab that is no longer shown -- can be tested without a window.
///
/// Hiding a tab removes it from the sidebar and nothing else: the screen still
/// opens when something asks for it (`--tab`, a session link, an AI tool, the
/// tour), and while it is open it stays in the sidebar so the reader can see
/// where they are.
enum FeatureSet {
    /// Always in the sidebar: the place to turn features back on, and the
    /// place that explains them. Hiding either would strand the reader.
    static let alwaysShown: Set<DevXTab> = [.settings, .help]

    /// The main features, shown on a first launch. The rest are one switch
    /// away in Settings -- they are narrower tools, not lesser ones.
    static let defaults: Set<DevXTab> = [
        .devices, .apps, .live, .record, .inspect, .emulator,
        .sessions, .issues, .timeline, .intelligence, .settings, .help,
    ]

    /// Restores the stored choice.
    ///
    /// `stored` is nil before the reader ever chose, which means the defaults.
    /// `known` is every tab the version that saved it knew about: a tab added
    /// since then was never offered, so it takes its default rather than
    /// reading as one the reader turned off. Unknown names (a tab a newer
    /// version added, then a downgrade) are ignored.
    static func decode(shown stored: [String]?, known: [String]?) -> Set<DevXTab> {
        guard let stored else { return defaults }
        var out = Set(stored.compactMap(DevXTab.init(rawValue:)))
        let seen = Set((known ?? stored).compactMap(DevXTab.init(rawValue:)))
        for tab in DevXTab.allCases where !seen.contains(tab) && defaults.contains(tab) {
            out.insert(tab)
        }
        return out.union(alwaysShown)
    }

    /// The choice as stored, in sidebar order so the plist reads sensibly.
    ///
    /// `keeping` is what was stored before: names this version does not know
    /// (a newer version's tab, before a downgrade) are carried over, so going
    /// back up does not forget the reader's choice for them.
    static func encode(_ shown: Set<DevXTab>, keeping previous: [String]? = nil) -> [String] {
        DevXTab.allCases.filter { shown.contains($0) || alwaysShown.contains($0) }.map(\.rawValue)
            + foreign(previous)
    }

    /// Every tab this version knows, plus any a newer one stored.
    static func known(keeping previous: [String]?) -> [String] {
        DevXTab.allCases.map(\.rawValue) + foreign(previous)
    }

    private static func foreign(_ names: [String]?) -> [String] {
        (names ?? []).filter { DevXTab(rawValue: $0) == nil }
    }

    /// The sidebar: the shown tabs in their usual order, plus the open tab
    /// when it is hidden, so a screen something else opened is never one the
    /// reader cannot locate.
    static func sidebar(shown: Set<DevXTab>, current: DevXTab) -> [DevXTab] {
        DevXTab.allCases.filter { shown.contains($0) || alwaysShown.contains($0) || $0 == current }
    }

    /// The tab to reopen on: the one last open, while it is still shown;
    /// otherwise the first shown tab.
    static func restore(last: String?, shown: Set<DevXTab>) -> DevXTab {
        if let last, let tab = DevXTab(rawValue: last),
           shown.contains(tab) || alwaysShown.contains(tab) {
            return tab
        }
        return sidebar(shown: shown, current: .settings).first ?? .devices
    }

    /// The groups the Settings page lists tabs under, in the order a session
    /// usually goes: pick a target, capture, read what came back, connect.
    enum Group: String, CaseIterable, Identifiable {
        case setup, capture, analyze, connect, app
        var id: String { rawValue }

        var title: String {
            switch self {
            case .setup: return tr("Set up")
            case .capture: return tr("Capture")
            case .analyze: return tr("Analyze")
            case .connect: return tr("Connect")
            case .app: return tr("The app")
            }
        }

        var tabs: [DevXTab] {
            DevXTab.allCases.filter { FeatureSet.group(of: $0) == self }
        }
    }

    static func group(of tab: DevXTab) -> Group {
        switch tab {
        case .devices, .apps, .preflight, .emulator: return .setup
        case .live, .record, .inspect, .layout: return .capture
        case .sessions, .issues, .threads, .timeline, .compare, .detectors: return .analyze
        case .intelligence: return .connect
        case .settings, .help: return .app
        }
    }
}
