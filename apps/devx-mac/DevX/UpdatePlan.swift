import Foundation

/// How this copy of DevX can be updated, decided from where it runs.
///
/// Pure so the decision can be tested: a copy Homebrew installed must be
/// updated by Homebrew (replacing it behind brew's back leaves brew's record
/// and the `mpi` link pointing at a version that is gone); a copy someone
/// dragged out of the disk image is replaced with the next disk image; a
/// development build is updated with git, never by a download.
enum UpdatePlan {
    enum Method: Equatable {
        case homebrew(brew: String)
        case diskImage(target: String)
        /// The folder it is in cannot be written: the disk image is opened in
        /// Finder for the reader to drag.
        case manual(target: String)
        case development
    }

    /// The Developer ID team every release is signed by
    /// (docs/packaging-and-signing.md). A disk image whose app is signed by
    /// anyone else is not installed, whatever its checksum says.
    static let teamID = "9QCJJFC22X"
    static let bundleID = "com.devx"
    /// For SecRequirementCreateWithString: Apple-issued Developer ID, this
    /// team, this app.
    static var signingRequirement: String {
        "anchor apple generic and certificate leaf[subject.OU] = \"\(teamID)\" and identifier \"\(bundleID)\""
    }

    static let brewCandidates = ["/opt/homebrew/bin/brew", "/usr/local/bin/brew"]
    static let caskrooms = ["/opt/homebrew/Caskroom/devx", "/usr/local/Caskroom/devx"]

    static func method(bundlePath: String,
                       exists: (String) -> Bool,
                       writable: (String) -> Bool) -> Method {
        if bundlePath.contains("/build/bin/") || !bundlePath.hasSuffix(".app") {
            return .development
        }
        let parent = (bundlePath as NSString).deletingLastPathComponent
        if bundlePath == "/Applications/DevX.app",
           caskrooms.contains(where: exists),
           let brew = brewCandidates.first(where: exists) {
            return .homebrew(brew: brew)
        }
        return writable(parent) ? .diskImage(target: bundlePath) : .manual(target: bundlePath)
    }

    /// The hash in a `shasum -a 256` line ("<hex>  DevX-0.6.1.dmg"),
    /// lowercased; nil unless it is 64 hex digits.
    static func publishedSHA256(_ text: String) -> String? {
        guard let first = text.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).first
        else { return nil }
        let hex = first.lowercased()
        guard hex.count == 64, hex.allSatisfy({ $0.isHexDigit }) else { return nil }
        return hex
    }

    /// The first lines of the release notes, for the panel: a long changelog
    /// belongs on the release page, which is linked.
    static func summary(_ notes: String, lines: Int = 12) -> String {
        let kept = notes.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("```") && !$0.hasPrefix("brew ") }
        var out: [String] = []
        for line in kept where out.count < lines {
            if line.isEmpty, out.last?.isEmpty ?? true { continue }
            out.append(line)
        }
        while out.last?.isEmpty == true { out.removeLast() }
        return out.joined(separator: "\n")
    }

    /// Checking at launch, when switched on, at most once a day.
    static func shouldAutoCheck(enabled: Bool, last: Date?, now: Date) -> Bool {
        guard enabled else { return false }
        guard let last else { return true }
        return now.timeIntervalSince(last) >= 24 * 3600
    }
}
