import Foundation

/// The issue filter (spec section 13: filter by process, thread, screen,
/// category, severity).
///
/// Pure, and separated from the view for one reason: a filter is the easiest
/// way for a UI to say "nothing is wrong" when the truth is "you asked to see
/// a subset". The rules that stop it doing that are worth testing.
///
///   * An empty field means **no filter on that dimension**, never "match
///     nothing". A blank filter that matched nothing would empty the list.
///   * An issue with no value for a dimension does **not** match a filter
///     asking for one. An issue with no screen is not on every screen.
///   * Suppressed issues are hidden by default and counted as hidden, so a
///     suppression cannot quietly shrink the list.
struct IssueFilter: Equatable {
    var category = ""
    var severity = ""
    var screen = ""
    var thread = ""
    var process = ""
    var includeSuppressed = false

    var isActive: Bool {
        !category.isEmpty || !severity.isEmpty || !screen.isEmpty
            || !thread.isEmpty || !process.isEmpty || includeSuppressed
    }

    func matches(_ issue: JSON) -> Bool {
        if !includeSuppressed, issue["suppression"]["suppressed"].bool == true {
            return false
        }
        let checks: [(String, String)] = [
            (category, issue["category"].text),
            (severity, issue["severity"].text),
            (screen, issue["screen"].text),
            (thread, issue["thread_instance_id"].text),
            (process, issue["process_instance_id"].text),
        ]
        for (wanted, actual) in checks {
            if wanted.isEmpty { continue }
            if actual != wanted { return false }
        }
        return true
    }

    /// The issues that pass, and how many did not.
    ///
    /// Both numbers, always: the view is required to show what it is not
    /// showing, and it cannot do that from the filtered list alone.
    func apply(_ issues: [JSON]) -> (shown: [JSON], hidden: Int) {
        let shown = issues.filter { matches($0) }
        return (shown, issues.count - shown.count)
    }

    /// The distinct values of one field across `issues`.
    ///
    /// Only values actually present. Offering a filter that can match nothing
    /// in this session is another way to manufacture an empty list.
    static func options(_ field: String, in issues: [JSON]) -> [String] {
        var seen: [String] = []
        for i in issues {
            let v = i[field].text
            if v.isEmpty || seen.contains(v) { continue }
            seen.append(v)
        }
        return seen.sorted()
    }
}
