// Which preliminary finding the Live tab is showing, and why it might not be
// showing one.
//
// Selection is keyed by `issue_id` -- the content fingerprint the engine
// derives, `DET-01-2c5ba6255ef9062f` -- and never by a row index. The Live
// tab's findings are *recomputed on every tick* as evidence arrives, so the
// list reorders and grows underneath the reader: an index-keyed selection
// would quietly slide onto a different finding while they read it.
//
// The same recomputation means a selected finding can stop existing. A
// detector that fired on twelve missed frames may stop firing when a longer
// window changes the rate, and that is a real event the reader should see
// stated rather than have the pane silently fall back to "nothing selected"
// -- which would read as though they had never clicked.
import Foundation

enum FindingPane: Equatable {
    /// No findings in this capture yet. Not the same as nothing selected:
    /// there is nothing to select.
    case noFindings
    /// Findings exist and none is selected.
    case nothingSelected
    /// Selected, and no longer among the findings -- recomputed away.
    case gone
    /// Show this finding.
    case finding(JSON)

    static func == (a: FindingPane, b: FindingPane) -> Bool {
        switch (a, b) {
        case (.noFindings, .noFindings), (.nothingSelected, .nothingSelected),
             (.gone, .gone):
            return true
        case (.finding(let x), .finding(let y)):
            return FindingSelection.id(of: x) == FindingSelection.id(of: y)
        default:
            return false
        }
    }
}

enum FindingSelection {
    /// A finding's identity. `issue_id` is the engine's own fingerprint;
    /// `fingerprint` is the same value under the name older reports used, so
    /// both are accepted -- matching `AppState.focusIssue`, which has always
    /// read them in this order.
    static func id(of finding: JSON) -> String {
        let primary = finding["issue_id"].text
        return primary.isEmpty ? finding["fingerprint"].text : primary
    }

    /// Whether a finding can be selected at all. One with no fingerprint is
    /// not selectable, rather than falling back to an index the next
    /// recomputation would invalidate.
    static func isSelectable(_ finding: JSON) -> Bool { !id(of: finding).isEmpty }

    static func resolve(selectedId: String, findings: [JSON]) -> FindingPane {
        if findings.isEmpty { return .noFindings }
        if selectedId.isEmpty { return .nothingSelected }
        if let hit = findings.first(where: { id(of: $0) == selectedId }) {
            return .finding(hit)
        }
        return .gone
    }
}
