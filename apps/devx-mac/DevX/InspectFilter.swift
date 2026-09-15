// Filtering an observation, and saying what a filter hid.
//
// The Inspect tab can carry hundreds of console lines and dozens of requests.
// Filtering is the obvious answer, and it has one non-obvious requirement in
// a tool like this: **a filtered empty list must not read like a quiet app.**
//
// "No API calls" is a claim about the app. "No API calls matching your
// filter" is a claim about the filter. The view has to be able to tell them
// apart, so filtering returns the counts it removed rather than just the rows
// it kept.
import Foundation

/// Which kinds of activity to show.
enum InspectKind: String, CaseIterable, Identifiable {
    case network
    case redux
    case log

    var id: String { rawValue }

    var label: String {
        switch self {
        // "API" rather than "network": it is what the request was, in the
        // words the person asking for this filter used.
        case .network: return tr("API")
        case .redux: return tr("Redux")
        case .log: return tr("Log")
        }
    }
}

/// A console line's kind.
///
/// Redux is recognised, not reported: an app printing redux-logger output is
/// the only way an action reaches the console, so this is a guess about a
/// line's shape and is labelled as one wherever it is shown.
func inspectKind(ofConsoleLine line: JSON) -> InspectKind {
    line["inferred_redux_action_type"].text.isEmpty ? .log : .redux
}

struct InspectFilterResult<T> {
    let shown: [T]
    /// How many rows the filter removed. Zero shown with a non-zero hidden
    /// count is a statement about the filter; zero and zero is a statement
    /// about the app.
    let hidden: Int

    var hidEverything: Bool { shown.isEmpty && hidden > 0 }
    var nothingToShow: Bool { shown.isEmpty && hidden == 0 }
}

enum InspectFilter {
    /// Filters console lines by kind and by a free-text needle.
    ///
    /// The needle is matched case-insensitively against the text and the
    /// inferred action type, because someone filtering for `user/login` is
    /// looking for the action and someone filtering for `error` is looking at
    /// the message.
    static func console(_ lines: [JSON], kinds: Set<InspectKind>,
                        needle: String) -> InspectFilterResult<JSON> {
        let want = needle.trimmingCharacters(in: .whitespaces).lowercased()
        var shown: [JSON] = []
        for line in lines {
            guard kinds.contains(inspectKind(ofConsoleLine: line)) else { continue }
            if !want.isEmpty {
                let haystack = (line["text"].text + " "
                    + line["inferred_redux_action_type"].text
                    + " " + line["level"].text).lowercased()
                guard haystack.contains(want) else { continue }
            }
            shown.append(line)
        }
        return InspectFilterResult(shown: shown, hidden: lines.count - shown.count)
    }

    /// Filters network exchanges. Matched against method, URL and status, so
    /// `500` and `POST` both work as needles.
    static func network(_ rows: [JSON], kinds: Set<InspectKind>,
                        needle: String) -> InspectFilterResult<JSON> {
        guard kinds.contains(.network) else {
            return InspectFilterResult(shown: [], hidden: rows.count)
        }
        let want = needle.trimmingCharacters(in: .whitespaces).lowercased()
        if want.isEmpty {
            return InspectFilterResult(shown: rows, hidden: 0)
        }
        var shown: [JSON] = []
        for r in rows {
            let status = r["status"].int.map(String.init) ?? ""
            let haystack = (r["method"].text + " " + r["url"].text + " "
                            + status).lowercased()
            if haystack.contains(want) { shown.append(r) }
        }
        return InspectFilterResult(shown: shown, hidden: rows.count - shown.count)
    }
}

// MARK: - Clearing

/// Clearing a list, without throwing anything away.
///
/// A live observation piles up: hundreds of console lines, dozens of
/// requests. "Clear" is the obvious control and it has the same requirement
/// as every other filter here -- **what is on screen must not become a claim
/// about the app.** So a clear is a *watermark*, not a delete: the rows are
/// still in the observation, still exported, still counted, and the view says
/// how many it is holding back and offers to show them again.
///
/// That also makes it correct while streaming. The assembler is cumulative
/// and hands back the whole observation on every poll, so a clear that
/// removed rows would see them return on the next tick.
extension InspectFilter {
    /// Rows kept after a clear that hid the first `clearedCount` of them.
    ///
    /// Count-based because the console and network lists are append-only: a
    /// network exchange is updated in place as its response arrives, so its
    /// index is stable while its contents are not. A mark that pointed at a
    /// row's contents would move.
    static func afterClear(_ rows: [JSON], clearedCount: Int) -> [JSON] {
        guard clearedCount > 0 else { return rows }
        // A clear from a previous run can outlive the list it was made
        // against -- the app reloaded and the observation started over. More
        // rows hidden than exist means the mark is stale, and hiding
        // everything forever is the one outcome that would look like a bug.
        guard clearedCount < rows.count else { return [] }
        return Array(rows.dropFirst(clearedCount))
    }

    /// Redux records kept after a clear at `clearedSeq`.
    ///
    /// Sequence-based rather than count-based: records carry a monotonic seq
    /// from inside the app, and the in-app buffer can drop the oldest ones
    /// under load. A count would then hide the wrong rows -- the list
    /// shortens from the front while the mark stays put.
    static func afterClear(reduxRecords: [JSON], clearedSeq: Int) -> [JSON] {
        guard clearedSeq > 0 else { return reduxRecords }
        return reduxRecords.filter { ($0["seq"].int ?? 0) > clearedSeq }
    }
}
