import Foundation

/// Recently used and favourited targets.
///
/// The requirement here is an honesty rule, not a feature: **a recent or
/// favourite entry is not proof the app is running** (spec A23). It is a note
/// about what someone profiled before, made by this app on this machine --
/// nothing about the device was consulted to write it, and time has passed
/// since.
///
/// So an entry never carries a runtime state. The only thing that can say
/// whether an app is running is discovery, and when a recent entry does not
/// appear in the current enumeration the reason is stated rather than being
/// rendered as "not running": an app that is absent from a list and an app
/// the list could not see are different facts.
struct RecentTarget: Codable, Equatable, Identifiable {
    var deviceId: String
    var appIdentifier: String
    /// What the app was called when it was last seen. A remembered label, not
    /// a current one -- the app may have been renamed or uninstalled since.
    var lastKnownName: String
    var lastUsedAt: Date
    var favourite: Bool

    var id: String { deviceId + "\u{1f}" + appIdentifier }
}

/// The remembered list, ordered most-recent-first with favourites kept.
///
/// Pure and `Codable`, so it round-trips through a file and can be tested
/// without a device or a window.
struct RecentTargets: Codable, Equatable {
    var entries: [RecentTarget] = []

    /// How many non-favourite entries to keep. Favourites are never evicted:
    /// someone marked them on purpose, and silently dropping one would look
    /// like it was never marked.
    static let recentLimit = 12

    mutating func record(deviceId: String, appIdentifier: String,
                         name: String, at when: Date = Date()) {
        guard !deviceId.isEmpty, !appIdentifier.isEmpty else { return }
        // Truncated to a whole second, because that is what the file can
        // hold: ISO-8601 encoding drops the fraction, so keeping it in memory
        // would mean the list in hand and the list on disk were never equal
        // and no comparison between them could be trusted. A "last profiled"
        // time needs no more precision than this anyway.
        let at = Date(timeIntervalSince1970:
                          (when.timeIntervalSince1970).rounded(.down))
        let key = deviceId + "\u{1f}" + appIdentifier
        var favourite = false
        if let existing = entries.first(where: { $0.id == key }) {
            favourite = existing.favourite
        }
        entries.removeAll { $0.id == key }
        entries.insert(RecentTarget(deviceId: deviceId,
                                    appIdentifier: appIdentifier,
                                    lastKnownName: name,
                                    lastUsedAt: at,
                                    favourite: favourite),
                       at: 0)
        prune()
    }

    mutating func setFavourite(_ id: String, _ value: Bool) {
        guard let i = entries.firstIndex(where: { $0.id == id }) else { return }
        entries[i].favourite = value
        prune()
    }

    mutating func forget(_ id: String) {
        entries.removeAll { $0.id == id }
    }

    /// Favourites first, then the rest newest-first. Within favourites the
    /// same ordering, so a list someone curated does not shuffle.
    var ordered: [RecentTarget] {
        entries.sorted { a, b in
            if a.favourite != b.favourite { return a.favourite }
            return a.lastUsedAt > b.lastUsedAt
        }
    }

    func forDevice(_ deviceId: String) -> [RecentTarget] {
        ordered.filter { $0.deviceId == deviceId }
    }

    private mutating func prune() {
        var kept: [RecentTarget] = []
        var recents = 0
        for e in ordered {
            if e.favourite {
                kept.append(e)
                continue
            }
            if recents < Self.recentLimit {
                kept.append(e)
                recents += 1
            }
        }
        entries = kept
    }

    // ---- persistence ------------------------------------------------------

    static func load(from path: String) -> RecentTargets {
        // A missing or unreadable file is an empty list, not an error: a
        // machine that has profiled nothing yet is the normal first case.
        guard let data = FileManager.default.contents(atPath: path) else {
            return RecentTargets()
        }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return (try? decoder.decode(RecentTargets.self, from: data))
            ?? RecentTargets()
    }

    func save(to path: String) {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(self) else { return }
        // Written beside the target and moved into place, so an interrupted
        // write cannot leave a file that decodes to nothing.
        let temp = path + ".partial"
        do {
            try data.write(to: URL(fileURLWithPath: temp))
            _ = try? FileManager.default.removeItem(atPath: path)
            try FileManager.default.moveItem(atPath: temp, toPath: path)
        } catch {
            try? FileManager.default.removeItem(atPath: temp)
        }
    }
}

/// What a remembered entry may be shown as, next to the current enumeration.
///
/// The three states exist so the view cannot collapse them. `notInListing` is
/// the one that matters: it is not "not running", because a listing that could
/// not see an app and an app that is absent are different facts, and the
/// enumeration's own scope decides which.
enum RecentPresence: Equatable {
    /// The current enumeration has this app; its row carries the real state.
    case inListing
    /// The enumeration ran and this app was not in it.
    case notInListing
    /// No enumeration to compare against, so nothing can be said.
    case noListing

    var label: String {
        switch self {
        case .inListing: return "in the current listing"
        case .notInListing: return "not in the current listing"
        case .noListing: return "no listing to compare against"
        }
    }

    var detail: String {
        switch self {
        case .inListing:
            return "its row above carries the runtime state discovery reported."
        case .notInListing:
            return "which is not the same as not running: it may be "
                 + "uninstalled, or outside what this listing can see."
        case .noListing:
            return "this is a note about what was profiled here before, and "
                 + "says nothing about the device now."
        }
    }
}

/// Where a remembered target stands against the current enumeration.
///
/// `identifiers` is what discovery listed; `didEnumerate` says whether it ran
/// at all. Passing an empty set with `didEnumerate` true is a real case -- a
/// device with no visible apps -- and is different from not having asked.
func presence(of target: RecentTarget, identifiers: Set<String>,
              didEnumerate: Bool) -> RecentPresence {
    if !didEnumerate { return .noListing }
    return identifiers.contains(target.appIdentifier) ? .inListing
                                                      : .notInListing
}
