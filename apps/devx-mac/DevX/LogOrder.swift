// The order the three activity lists are read in, and the clock beside each
// row.
//
// Both came from the same complaint: a long capture puts what just happened
// at the bottom, off the end of a section that scrolls, and a row carries no
// answer to "when was this reported". Newest first puts the interesting end
// where the eye already is, and a clock time makes a row placeable against
// what the person was doing in the app.
//
// The ordering is a *view* concern and stays one. The lists are appended in
// arrival order and every other piece of logic depends on that: `clear` is a
// watermark counted from the front, and the detail pane's "#N" is a position
// in the captured list. Reversing for display only, at the last moment, keeps
// all of that true -- which is why this reverses a copy rather than sorting
// the model.
import Foundation

enum LogOrder {
    /// Newest first, without disturbing the model's own order.
    ///
    /// A plain reverse rather than a sort by timestamp: the lists arrive in
    /// order, and a sort would need a key every row has. Network rows only
    /// carry a wall clock when the runtime sent one, so a timestamp sort
    /// would have to invent a position for the rows that have none.
    static func newestFirst(_ rows: [JSON]) -> [JSON] {
        Array(rows.reversed())
    }

    /// The clock time to show beside a row, or nil when the row has none.
    ///
    /// Nil is a real answer and is rendered as nothing. The three lists get
    /// their time from three different fields because the runtime reports
    /// them differently, and only two of the three are a wall clock at all:
    ///
    ///   * console: `timestamp_unix_ms` -- `Runtime`/`Log` timestamps are
    ///     milliseconds since the epoch, so this is a clock. The sibling
    ///     `timestamp_ns` is the same instant and must not be read here: it
    ///     is past 2^53, where a Double no longer counts by ones.
    ///   * network: `wall_unix_ms` -- from CDP's `wallTime`. Its sibling
    ///     `started_ns` is monotonic from an arbitrary origin and is not a
    ///     clock; rendering that as one would date every request to 1970.
    ///   * redux: `at_unix_ms` -- `Date.now()` inside the app.
    static func clock(_ row: JSON, _ kind: LogKind, now: Date? = nil) -> String? {
        let ms: Int?
        switch kind {
        case .network: ms = row["wall_unix_ms"].int
        // The millisecond field, not the nanosecond one: a JSON number
        // reaches this app as a Double, and an epoch in nanoseconds is past
        // 2^53 where a Double still counts by ones -- it arrives a
        // millisecond short. The core exports both for this reason.
        case .console: ms = row["timestamp_unix_ms"].int
        case .redux:   ms = row["at_unix_ms"].int
        }
        guard let ms, ms > 0 else { return nil }
        return time(ofEpochMs: ms)
    }

    /// `HH:MM:SS.mmm` in the reader's own time zone.
    ///
    /// Time of day and not a date: every row in one capture is from the same
    /// few minutes, and a date on each would be noise. Milliseconds are kept
    /// because two requests a reader is comparing are often in the same
    /// second.
    static func time(ofEpochMs ms: Int) -> String {
        let whole = ms / 1000
        let millis = ms % 1000
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = .current
        let parts = cal.dateComponents([.hour, .minute, .second],
                                       from: Date(timeIntervalSince1970: Double(whole)))
        return String(format: "%02d:%02d:%02d.%03d",
                      parts.hour ?? 0, parts.minute ?? 0, parts.second ?? 0,
                      millis)
    }
}

/// Which of the three activity lists a row belongs to. They carry their time
/// in different fields, so the caller has to say.
enum LogKind {
    case network
    case console
    case redux
}
