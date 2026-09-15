// How old the device list is, in words.
//
// This is a separate, pure file because it is the fix for a real confusion and
// deserves tests. An emulator was started outside the app; the Devices tab
// went on showing the list it had fetched at launch; and the operator read
// "not there" off a screen that had no idea whether it was there or not.
//
// The list was never wrong about what it saw. It was wrong about *when*, and
// a snapshot with no timestamp invites exactly the inference this project
// exists to refuse: absent from a stale list is not absent from the machine.
import Foundation

/// How much the wording is entitled to claim.
enum DiscoveryConfidence {
    /// Discovery has not run. The list is not evidence of anything yet.
    case noClaim
    /// Re-scanned recently enough to be read as current.
    case current
    /// Old enough that something could have changed unnoticed.
    case aging
}

struct DiscoveryStatus: Equatable {
    let text: String
    let confidence: DiscoveryConfidence
}

extension DiscoveryConfidence: Equatable {}

enum DeviceFreshness {
    /// Past which an unwatched list is called out as aging. Thirty seconds is
    /// about how long it takes to plug in a phone and look back at the screen.
    static let agingAfter: TimeInterval = 30

    /// Whole units only, and never rounded up: "0s ago" for a scan that just
    /// happened is worse than "a moment ago", and "1m ago" for 55 seconds
    /// overstates how recent it is in the wrong direction.
    static func ago(_ seconds: TimeInterval) -> String {
        // A clock that moved backwards -- an NTP correction, a laptop waking
        // -- must not print "-4s ago" or a huge number. It is simply unknown.
        //
        // But "slightly negative" is the normal case, not a broken clock. The
        // label's own clock ticks once a second while the watch re-scans every
        // five, so for up to one second after each scan the timestamp sits
        // ahead of the `now` this was called with. Reading that as a backwards
        // clock printed "re-scanning every 5s · last looked at an unknown
        // time" -- a list that had just been refreshed claiming it did not
        // know when -- for roughly one second in five.
        //
        // So the threshold is the UI's own granularity, not zero. Beyond it,
        // the skew is larger than anything this view can cause and is
        // genuinely unknown.
        if seconds < -uiClockGranularity { return tr("at an unknown time") }
        if seconds < 2 { return tr("a moment ago") }
        if seconds < 60 { return fill(tr("{n}s ago"), "{n}", Int(seconds)) }
        if seconds < 3600 { return fill(tr("{n}m ago"), "{n}", Int(seconds / 60)) }
        return fill(tr("{n}h ago"), "{n}", Int(seconds / 3600))
    }

    /// How stale the `now` passed to `ago` can legitimately be: the labels
    /// using it are redrawn on a one-second timer, and a scan can land at any
    /// point between two ticks. Doubled, so a redraw that is merely late does
    /// not cross the line either.
    static let uiClockGranularity: TimeInterval = 2

    /// Substitutes one placeholder.
    ///
    /// The templates carry named braces rather than `%@`, so a translator can
    /// see what goes where and a mistyped token cannot silently swap two
    /// values. A placeholder a translation dropped leaves the sentence short
    /// rather than inserting a stray number.
    static func fill(_ template: String, _ placeholder: String,
                     _ value: Any) -> String {
        template.replacingOccurrences(of: placeholder, with: "\(value)")
    }

    /// Why the watch is not scanning right now, when it is not.
    ///
    /// The label used to say "re-scanning every 5s" whenever the watch was
    /// enabled, which is a statement of intent. A screenshot showed it
    /// reading "re-scanning every 5s · last looked 6m ago" -- two claims that
    /// cannot both be true, and the reader has no way to tell which one is.
    ///
    /// Scans are suppressed while other work is in flight, because discovery
    /// is serialised on one queue, and a boot holds that queue for up to
    /// three minutes. That is precisely when someone is watching for a device
    /// to appear, so the reason has to be on screen instead of a claim that
    /// contradicts the age beside it.
    static func status(loadedAt: Date?, now: Date, watching: Bool,
                       suppressedBy: String) -> DiscoveryStatus {
        if watching && !suppressedBy.isEmpty {
            let when = loadedAt.map { ago(now.timeIntervalSince($0)) }
            return DiscoveryStatus(
                text: "re-scanning is paused while \(suppressedBy)"
                    + (when.map { " · last looked \($0)" } ?? ""),
                // Aging, not current: the list is not being kept up to date,
                // whatever the watch setting says.
                confidence: .aging)
        }
        return status(loadedAt: loadedAt, now: now, watching: watching)
    }

    static func status(loadedAt: Date?, now: Date, watching: Bool) -> DiscoveryStatus {
        guard let loadedAt else {
            return DiscoveryStatus(
                text: tr("discovery has not run — this list is not yet a claim "
                      + "about what is connected"),
                confidence: .noClaim)
        }
        let age = now.timeIntervalSince(loadedAt)
        let when = ago(age)
        if watching {
            return DiscoveryStatus(
                text: fill(tr("re-scanning every 5s · last looked {when}"),
                           "{when}", when),
                confidence: .current)
        }
        if age <= agingAfter {
            return DiscoveryStatus(
                text: fill(tr("last looked {when}"), "{when}", when),
                confidence: .current)
        }
        return DiscoveryStatus(
            text: fill(tr("last looked {when} — a device connected or started "
                       + "since then is not in this list"), "{when}", when),
            confidence: .aging)
    }

    /// A stable summary of what discovery saw, used to decide whether a
    /// re-scan is worth publishing.
    ///
    /// Identity *and* state, because the interesting transitions are not only
    /// arrivals and departures: a phone going from offline to authorized is
    /// the moment it becomes usable, and a fingerprint of ids alone would
    /// leave it reading as untrusted until something else changed. Sorted, so
    /// that a provider returning the same devices in a different order is not
    /// mistaken for the machine changing.
    static func fingerprint(_ doc: JSON) -> String {
        doc["devices"].array.map {
            [$0["device_id"].text, $0["trust"].text, $0["os_version"].text]
                .joined(separator: "\u{1}")
        }.sorted().joined(separator: "\u{2}")
    }
}
