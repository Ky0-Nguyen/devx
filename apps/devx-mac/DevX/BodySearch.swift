// Finding something inside a response body.
//
// "thêm ô search trong body response trả về vì đôi lúc cần search" -- a
// 260 KB response is not read, it is searched. The pane already shows the
// first twenty thousand characters and offers more in steps, which is fine
// for reading the shape of a document and useless for answering "is this
// user's id in here, and what is next to it".
//
// Searching filters to the lines that match and keeps their line numbers.
// Two reasons it is lines rather than a scroll-to-next-match: the body is
// rendered as one Text, so there is nothing to scroll to; and a filtered
// view answers "how many, and where" in one look, which is the actual
// question. The line numbers are the body's own, so a match stays locatable
// in the full text.
import Foundation

struct BodyMatches: Equatable {
    /// Matching lines, in document order, each with its 1-based line number
    /// in the whole body.
    let lines: [(number: Int, text: String)]
    /// How many lines matched, which is `lines.count` unless the list was
    /// capped -- in which case this is the real total and the list is not.
    let total: Int
    /// The whole body's line count, so "3 of 812 lines" can be said.
    let scanned: Int
    /// Set when the shown list is shorter than `total`.
    let truncated: String

    static func == (a: BodyMatches, b: BodyMatches) -> Bool {
        a.total == b.total && a.scanned == b.scanned
            && a.truncated == b.truncated
            && a.lines.map(\.number) == b.lines.map(\.number)
    }
}

enum BodySearch {
    /// How many matching lines are worth rendering at once. A needle like `"`
    /// matches every line of a JSON document, and rendering eight hundred
    /// lines is the stall this tool exists to find.
    static let lineLimit = 200

    /// Lines of `body` containing `needle`, case-insensitively.
    ///
    /// Case-insensitive because a reader searching for a header name or an id
    /// does not know how the server cased it, and an exact-case miss reads as
    /// "not in the body" -- the one wrong answer this must not give.
    ///
    /// Searches the text it is given. The caller passes the formatted body,
    /// so a needle spanning a line break in the re-indented view will not
    /// match; the raw bytes are one line and would match instead. That is a
    /// real limit and is why the field says which text it searched.
    static func find(in body: String, needle: String,
                     limit: Int = lineLimit) -> BodyMatches? {
        let want = needle.trimmingCharacters(in: .whitespaces)
        guard !want.isEmpty else { return nil }
        let all = body.split(separator: "\n", omittingEmptySubsequences: false)
        var hits: [(number: Int, text: String)] = []
        var total = 0
        for (i, line) in all.enumerated() {
            guard line.range(of: want, options: .caseInsensitive) != nil else {
                continue
            }
            total += 1
            if hits.count < limit { hits.append((i + 1, String(line))) }
        }
        let note = total > hits.count
            ? fill(tr("showing the first {n} matching lines"), hits.count)
            : ""
        return BodyMatches(lines: hits, total: total, scanned: all.count,
                           truncated: note)
    }

    /// The sentence above a filtered body: how many matched, out of how much.
    ///
    /// States the denominator because "3 matches" in a body the pane has only
    /// partly loaded would be read as three in the whole response.
    static func summary(_ m: BodyMatches) -> String {
        if m.total == 0 {
            return fill(tr("no match in the {n} lines shown"), m.scanned)
        }
        return fill(fill(tr("{hits} of {n} lines match"), m.scanned),
                    m.total, placeholder: "{hits}")
    }

    private static func fill(_ template: String, _ n: Int,
                             placeholder: String = "{n}") -> String {
        template.replacingOccurrences(of: placeholder, with: "\(n)")
    }
}
