// Warning that a header carries a credential, without touching its value.
//
// The screenshot that prompted this pane had an `authorization` header
// holding a bearer token about two thousand characters long, and a response
// body full of AWS presigned URLs -- each of which is itself a credential,
// valid until its signature expires, and each of which looks like a harmless
// link.
//
// The tempting answer is to mask them. This project's answer is the opposite,
// and the core states why where it captures them: a redacted header is a
// claim about what was sent that is not true. Someone reading this pane is
// debugging an auth problem half the time, and a masked token cannot be
// compared against the one the server expected.
//
// So nothing is hidden. What this does instead is say, once, which of the
// headers present are the kind that usually carry a credential -- so the
// reader knows before they screen-share, not after. Names only: this never
// looks at a value, and never asserts that a particular header does carry a
// secret, because "key" and "token" are also ordinary non-secret parameters.
import Foundation

enum InspectSecrets {
    /// Header names that usually carry a credential.
    ///
    /// Matched case-insensitively on the whole name, not as a substring: a
    /// substring match on "key" would flag `x-monkey-id`, and a warning that
    /// fires on the wrong thing gets ignored on the right thing.
    static let credentialHeaders: Set<String> = [
        "authorization", "proxy-authorization", "cookie", "set-cookie",
        "x-api-key", "x-auth-token", "x-amz-security-token",
        "x-csrf-token", "x-xsrf-token", "authentication",
    ]

    /// Which of these header names usually carry a credential, in the order
    /// they were given. Empty when none do.
    static func credentialHeaderNames(_ names: [String]) -> [String] {
        names.filter { credentialHeaders.contains($0.lowercased()) }
    }

    /// The sentence to show above a header list, or nil when none of its
    /// names are the credential-carrying kind.
    ///
    /// Hedged deliberately -- "usually carry" rather than "carries" -- because
    /// this has seen only the name. Asserting a secret from a name would be a
    /// claim about a value nobody looked at.
    static func credentialNote(_ names: [String]) -> String? {
        let hits = credentialHeaderNames(names)
        guard !hits.isEmpty else { return nil }
        return tr("headers that usually carry a credential are shown in full "
                + "below, as captured:") + " " + hits.joined(separator: ", ")
    }
}
