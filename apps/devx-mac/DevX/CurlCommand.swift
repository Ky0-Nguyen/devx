// One captured request, as a `curl` command another team can run.
//
// The request behind this: "có thể copy nguyên curl để gửi cho team khác
// invest nếu có issue". Someone else reproducing the call is the fastest way
// to settle whether a problem is the app's or the endpoint's, and retyping a
// URL with four query parameters and an auth header from a screenshot is how
// that goes wrong.
//
// Two rules shape everything here.
//
// **It reproduces what was sent, not a tidied version of it.** Every request
// header captured goes in, including the ones curl would otherwise set
// itself, because a header the app sent and curl guesses differently is
// exactly the kind of difference this is meant to expose.
//
// **It never implies it has more than it captured.** Headers and bodies only
// exist when detail was switched on for that observation. A command built
// without them is still useful -- the method and URL are real -- but it would
// read as "this request had no headers", so it carries a comment line saying
// otherwise. A shell ignores those; a reader does not.
import Foundation

enum CurlCommand {
    /// The command, ready to paste, or nil when there is not even a URL.
    ///
    /// Multi-line with trailing backslashes: a real request has enough
    /// headers that one line is unreadable, and unreadable is what makes
    /// someone retype it instead.
    static func build(_ row: JSON, capture: DetailCapture) -> String? {
        let url = row["url"].text
        guard !url.isEmpty else { return nil }

        var lines: [String] = []

        // Said first, because it changes how the rest should be read.
        switch capture {
        case .off:
            lines.append("# headers and bodies were not captured for this "
                       + "observation, so this is the method and URL only")
        case .unknown:
            lines.append("# whether headers and bodies were captured is not "
                       + "recorded, so their absence below says nothing")
        case .on:
            break
        }

        let method = row["method"].text.uppercased()
        var first = "curl"
        // curl sends GET by default; naming it adds noise. Anything else is
        // stated, including HEAD, which needs -I rather than -X HEAD to
        // behave the way the app's request did.
        if method == "HEAD" {
            first += " -I"
        } else if !method.isEmpty && method != "GET" {
            first += " -X \(method)"
        }
        first += " " + quote(url)
        lines.append(first)

        // Request headers only. A response header in a request would be a
        // fabrication, and `request_headers` is the only map that was sent.
        let headers = row["request_headers"]
        for name in headers.keys.sorted() {
            lines.append("-H " + quote("\(name): \(headers[name].text)"))
        }

        let body = BodyFormat.classify(row, .request, limit: Int.max)
        switch body.state {
        case .base64:
            // Emitting base64 as if it were the payload would send something
            // the app never sent.
            lines.append("# the request body was not text: the runtime "
                       + "returned it base64, so it is not reproduced here")
        case .json, .text:
            // The captured bytes, not the re-indented view: whitespace
            // between tokens is not what was on the wire.
            if let raw = body.raw, !raw.isEmpty {
                lines.append("--data-raw " + quote(raw))
            }
        case .empty:
            lines.append("# the request body was captured and was zero bytes")
        case .notCaptured, .unavailable:
            break
        }

        return lines.joined(separator: " \\\n  ")
            .replacingOccurrences(of: "# ", with: "# ")
    }

    /// Whether this command carries a credential, so the copy can say so.
    ///
    /// Names only, and hedged for the same reason as the header list: this
    /// has seen which headers are present, never a value, and "key" is also
    /// an ordinary parameter name.
    static func credentialWarning(_ row: JSON) -> String? {
        InspectSecrets.credentialNote(row["request_headers"].keys).map { _ in
            tr("this command carries the headers as they were sent, "
             + "credentials included -- treat it like a password")
        }
    }

    /// Wraps a value for `sh` in single quotes.
    ///
    /// Single quotes and not double: inside single quotes a shell expands
    /// nothing, so a body containing `$`, backticks or `!` survives intact.
    /// The one character that cannot appear is a single quote itself, which
    /// is closed, escaped and reopened -- the standard `'\''` dance. A JSON
    /// body with an apostrophe in a string is common enough that getting
    /// this wrong would corrupt real requests.
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
