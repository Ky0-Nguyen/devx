// Which request the detail pane is showing, and why it might not be showing
// one.
//
// Selection is keyed by `request_id`, never by a row index. The list the view
// renders is `afterClear(captured)` then filtered, so both of those shift
// every offset: clearing drops rows from the front and filtering removes them
// from the middle. An index-keyed selection survives neither -- it silently
// lands on a different request, which in a pane whose whole job is showing
// one request's headers and body is the worst available failure.
//
// The other half is that "nothing is on the right" has four different causes
// and only one of them is "you have not clicked anything yet". A request can
// also be hidden by the filter, held back by clear, or genuinely gone because
// the app reloaded and the observation started over. Each needs its own
// sentence: the list on the left is the current one, and a pane that said
// "pick a request" about a request the reader had already picked would be
// blaming them for the filter.
import Foundation

/// Whether the selected request is in the list the reader can see, and if not,
/// what is holding it.
enum ExchangeVisibility: Equatable {
    case visible
    /// Present in the observation, removed by the current filter.
    case hiddenByFilter
    /// Present in the observation, held back by a clear watermark.
    case heldByClear
}

/// What the detail column should render.
enum InspectPane: Equatable {
    /// No observation yet, so there is nothing to pick from.
    case noDocument
    /// An observation, and nothing picked.
    case nothingSelected
    /// Something was picked and is no longer in the observation at all --
    /// the app reloaded, or a new capture replaced it.
    case gone
    /// Show this request. `index` is its position in the captured list, for
    /// display only; never for identity.
    case exchange(row: JSON, visibility: ExchangeVisibility, index: Int)

    static func == (a: InspectPane, b: InspectPane) -> Bool {
        switch (a, b) {
        case (.noDocument, .noDocument), (.nothingSelected, .nothingSelected),
             (.gone, .gone):
            return true
        case (.exchange(_, let av, let ai), .exchange(_, let bv, let bi)):
            return av == bv && ai == bi
        default:
            return false
        }
    }
}

enum InspectSelection {
    /// Resolves a selected request id against the three lists the network
    /// panel derives, in the same order the panel derives them.
    ///
    /// - `captured`: everything in the observation.
    /// - `afterClear`: what a clear watermark left.
    /// - `shown`: what the filter then left — the rows the reader can click.
    ///
    /// The order matters for attribution. `networkPanel` applies the clear
    /// *before* the filter, so a row missing from `afterClear` was held by
    /// clear and blaming the filter for it would name the wrong cause -- in a
    /// pane that deliberately keeps clear and filter as separate, separately
    /// worded ideas.
    static func resolve(selectedId: String, captured: [JSON],
                        afterClear: [JSON], shown: [JSON]) -> InspectPane {
        if captured.isEmpty { return .noDocument }
        if selectedId.isEmpty { return .nothingSelected }

        guard let idx = captured.firstIndex(where: { id(of: $0) == selectedId })
        else {
            // Picked once, and not in the observation any more.
            return .gone
        }
        let row = captured[idx]
        if shown.contains(where: { id(of: $0) == selectedId }) {
            return .exchange(row: row, visibility: .visible, index: idx)
        }
        if afterClear.contains(where: { id(of: $0) == selectedId }) {
            // Survived the clear, so the filter is what removed it.
            return .exchange(row: row, visibility: .hiddenByFilter, index: idx)
        }
        return .exchange(row: row, visibility: .heldByClear, index: idx)
    }

    /// A row's identity. Empty when the producer did not supply one -- in
    /// which case the row is not selectable, rather than falling back to an
    /// index that the next clear would invalidate.
    static func id(of row: JSON) -> String { row["request_id"].text }

    /// Whether a row can be selected at all.
    static func isSelectable(_ row: JSON) -> Bool { !id(of: row).isEmpty }
}
