// What a captured HTTP body actually is, and how to show it.
//
// The request behind this was "có cách nào format response của API ko?" -- a
// response body was being rendered as one unwrapped line, and a real one from
// the app under test is 260 KB of JSON containing AWS presigned URLs two
// thousand characters long. Unreadable.
//
// Formatting is the easy half. The half that matters is that a body has more
// states than "present" and "absent", and the old view collapsed them:
//
//     !row["response_body"].text.isEmpty
//
// `response_body` is `std::optional<std::string>` in the core, and the C++
// side only emits the key when the optional holds a value (see
// InspectAssembler's to_json). So that test read three different facts as one:
//
//   * the key is absent because detail was never captured,
//   * the key is absent because the runtime had nothing to give -- which the
//     core reports separately in `response_body_unavailable`, since a HEAD or
//     a 204 genuinely has no body and that is not a failure,
//   * the key is present and holds an empty string -- a real, captured,
//     zero-byte body.
//
// All three rendered as nothing at all. This file keeps them apart, because
// "the app sent no body" and "we did not look" are different claims and only
// one of them is about the app.
import Foundation

/// Which of an exchange's two bodies is being read.
enum BodyField {
    case request
    case response

    var key: String {
        switch self {
        case .request: return "request_body"
        case .response: return "response_body"
        }
    }
}

/// What a body turned out to be. Never a default: every case is something
/// the data positively said.
enum BodyState: Equatable {
    /// No body was captured and nothing was said about why. Detail capture is
    /// off, or this exchange never got that far.
    case notCaptured
    /// A body was asked for and the runtime had none to give, with its reason.
    /// Distinct from `notCaptured`: someone did look.
    case unavailable(String)
    /// Captured, and genuinely zero bytes.
    case empty
    /// Captured, and not text. The runtime decides -- text comes back as text
    /// and anything else base64 -- so this is reported rather than decoded:
    /// showing base64 as text is nonsense, and decoding it here would destroy
    /// the distinction the runtime drew.
    case base64(String)
    /// Captured text that parsed as JSON, re-printed with indentation.
    case json(String)
    /// Captured text that did not parse as JSON, shown as it arrived.
    case text(String)
}

struct FormattedBody: Equatable {
    let state: BodyState
    /// The bytes as they were captured, before any formatting, whenever
    /// anything was captured at all. A reader can always get back to what was
    /// actually on the wire -- a pretty-printer that loses the original is a
    /// pretty-printer you cannot check.
    let raw: String?
    /// Set when what is shown is not the whole of what was captured, or when
    /// formatting did not run. Never empty just because everything worked.
    let note: String

    /// The text to display for this state.
    var display: String {
        switch state {
        case .notCaptured, .empty: return ""
        case .unavailable(let why): return why
        case .base64(let s), .json(let s), .text(let s): return s
        }
    }

    /// Whether the display string is the body itself rather than a statement
    /// about it. Drives whether the view offers "copy" and "show raw".
    var isBodyText: Bool {
        switch state {
        case .base64, .json, .text: return true
        case .notCaptured, .unavailable, .empty: return false
        }
    }
}

enum BodyFormat {
    /// How much of a body is worth rendering at once.
    ///
    /// A 260 KB pretty-printed body in a single Text view is measured in
    /// seconds of layout, and nobody reads 260 KB. The cut is explicit and
    /// carries its own note, because a body silently shortened reads as a
    /// body that was that short.
    static let displayLimit = 20_000

    /// Reads one of an exchange's bodies and says what it is.
    static func classify(_ row: JSON, _ field: BodyField,
                         limit: Int = displayLimit) -> FormattedBody {
        // Absent means the key was never emitted, which is not the same as a
        // key holding "". `isAbsent` is the only test that can tell them
        // apart; `.text.isEmpty` cannot, which is the bug this replaces.
        if row.isAbsent(field.key) {
            if field == .response {
                let why = row["response_body_unavailable"].text
                if !why.isEmpty { return FormattedBody(state: .unavailable(why),
                                                       raw: nil, note: "") }
            }
            return FormattedBody(state: .notCaptured, raw: nil, note: "")
        }

        let body = row[field.key].text
        if body.isEmpty {
            return FormattedBody(state: .empty, raw: "", note: "")
        }

        // Base64 travels as a flag beside the value and applies to the
        // response only; the request body is whatever the app posted.
        if field == .response, row["response_body_base64"].bool == true {
            let (shown, note) = cut(body, to: limit)
            return FormattedBody(state: .base64(shown), raw: body, note: note)
        }

        if let pretty = prettyJSON(body) {
            let (shown, note) = cut(pretty, to: limit)
            return FormattedBody(state: .json(shown), raw: body, note: note)
        }
        let (shown, note) = cut(body, to: limit)
        return FormattedBody(state: .text(shown), raw: body, note: note)
    }

    /// The largest body worth re-indenting.
    ///
    /// The scan is linear and a 260 KB body costs microseconds, so this is a
    /// guard against the pathological case rather than a performance tuning
    /// knob. Beyond it the body is shown as it arrived, with a note -- which
    /// is the honest outcome anyway.
    static let formatLimit = 2 << 20

    /// Re-indents a JSON document, or nil if it is not JSON.
    ///
    /// **Whitespace only.** Not a re-serialisation. Every token comes out
    /// byte-for-byte as it went in; the only edits are to whitespace between
    /// tokens. That matters more than it sounds, because the obvious
    /// implementation -- parse to objects and print them back -- quietly
    /// changes the data. Measured with Foundation on this machine:
    ///
    ///     {"v":1.0}      ->  {"v": 1}      a server that sent 1.0 is shown
    ///                                      as having sent 1
    ///     {"a":1,"a":2}  ->  {"a": 1}      a duplicate key disappears
    ///
    /// Neither is a disaster on its own, and both make the result something
    /// other than the response body -- which is the one thing this pane is
    /// for. Key order is preserved for the same reason: sorting keys would
    /// make two captures of one endpoint easier to compare, and it would also
    /// mean the document on screen is not the document that arrived.
    ///
    /// Validity is still decided by Foundation's parser -- that is what it is
    /// good at -- and the re-indent runs only on a string it has accepted.
    /// So a body that is not JSON is never half-reformatted into something
    /// the server never sent.
    static func prettyJSON(_ s: String) -> String? {
        guard s.count <= formatLimit else { return nil }
        guard let data = s.data(using: .utf8) else { return nil }
        // `.fragmentsAllowed` so a bare string or number body -- which real
        // APIs do return -- is recognised as JSON rather than falling through
        // to the raw branch. Parsed and thrown away: this call decides only
        // whether the text is JSON.
        guard (try? JSONSerialization.jsonObject(
                with: data, options: [.fragmentsAllowed])) != nil else {
            return nil
        }
        return reindent(s)
    }

    /// Re-emits JSON with one token per line and two-space nesting, touching
    /// nothing inside a string.
    ///
    /// Exposed for its own tests. The invariant worth holding onto: removing
    /// all whitespace outside strings from the output gives exactly the same
    /// text as doing that to the input.
    static func reindent(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.count + s.count / 2)
        var depth = 0
        var inString = false
        var escaped = false
        // Whether the last thing written was a structural opener, so an empty
        // object prints as {} rather than as two lines with nothing between.
        var pendingOpen: Character? = nil

        func newline() {
            out.append("\n")
            out.append(String(repeating: "  ", count: depth))
        }
        func flushOpen() {
            guard let open = pendingOpen else { return }
            pendingOpen = nil
            out.append(open)
            depth += 1
            newline()
        }

        for c in s {
            if inString {
                // Inside a string nothing is touched -- not whitespace, not
                // escapes. A pretty-printer that normalises \u0041 to A is
                // showing a body the server did not send.
                out.append(c)
                if escaped { escaped = false }
                else if c == "\\" { escaped = true }
                else if c == "\"" { inString = false }
                continue
            }
            switch c {
            case " ", "\t", "\n", "\r":
                continue          // existing layout is discarded, not kept
            case "{", "[":
                flushOpen()
                pendingOpen = c
            case "}", "]":
                if let open = pendingOpen {
                    // Empty container: `{}` on one line.
                    pendingOpen = nil
                    out.append(open)
                    out.append(c)
                } else {
                    depth = max(0, depth - 1)
                    newline()
                    out.append(c)
                }
            case ",":
                flushOpen()
                out.append(c)
                newline()
            case ":":
                flushOpen()
                out.append(": ")
            case "\"":
                flushOpen()
                inString = true
                out.append(c)
            default:
                flushOpen()
                out.append(c)
            }
        }
        flushOpen()
        return out
    }

    /// Cuts a string to `limit`, and says so when it cuts.
    ///
    /// Returns the text and the note that must accompany it. The note is the
    /// point: a body shortened without one is indistinguishable from a body
    /// that was always that size.
    static func cut(_ s: String, to limit: Int) -> (String, String) {
        guard limit > 0, s.count > limit else { return (s, "") }
        let shown = String(s.prefix(limit))
        // "captured" and not "exported": whether an inspect report reaches a
        // session package was never traced, and claiming it would be a
        // statement about this tool that nobody checked.
        return (shown,
                fillCount(tr("{shown} of {total} chars"), shown.count, s.count))
    }

    /// Two-placeholder fill. DeviceFreshness.fill does one.
    private static func fillCount(_ template: String, _ n: Int,
                                  _ total: Int) -> String {
        template
            .replacingOccurrences(of: "{shown}", with: "\(n)")
            .replacingOccurrences(of: "{total}", with: "\(total)")
    }
}
