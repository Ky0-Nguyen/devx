// Filtering the device list.
//
// A developer's machine has twenty-odd simulators on it, and the list was
// printing every one at full height with an advice button underneath --
// burying the single device being worked with. That is a presentation
// problem, and it has the same requirement as the Inspect filters: **a
// filtered empty list must not read like a machine with no devices.**
//
// "No device discovered" is a claim about the machine and has its own
// carefully worded banner. "Nothing matches your filter" is a claim about the
// filter. So filtering reports what it removed, and the view says which of
// the two it is showing.
import Foundation

struct DeviceFilterResult {
    let shown: [JSON]
    /// How many the filter removed. Zero shown with a non-zero hidden count
    /// says something about the filter; zero and zero says something about
    /// the machine.
    let hidden: Int

    var hidEverything: Bool { shown.isEmpty && hidden > 0 }
    var nothingToShow: Bool { shown.isEmpty && hidden == 0 }
}

enum DeviceFilter {
    /// Filters by a free-text needle and by usability.
    ///
    /// The needle is matched case-insensitively against the fields someone
    /// would actually type: the name, the id, the model, the OS version and
    /// the platform. Matching the *trust* state too would be a trap -- typing
    /// "offline" would hide every usable device, which is the opposite of
    /// what anyone means by it.
    static func apply(_ devices: [JSON], needle: String,
                      showUnusable: Bool) -> DeviceFilterResult {
        let want = needle.trimmingCharacters(in: .whitespaces).lowercased()
        var shown: [JSON] = []
        for d in devices {
            if !showUnusable && d["trust"].text != "authorized" { continue }
            if !want.isEmpty {
                let haystack = [d["display_name"].text, d["device_id"].text,
                                d["model"].text, d["os_version"].text,
                                d["platform"].text, d["form"].text]
                    .joined(separator: " ").lowercased()
                guard haystack.contains(want) else { continue }
            }
            shown.append(d)
        }
        return DeviceFilterResult(shown: shown, hidden: devices.count - shown.count)
    }
}

/// One platform's devices, as a labelled column.
struct DevicePlatformColumn: Identifiable {
    /// The platform as discovery reported it: "android", "ios", or whatever
    /// else a provider produced.
    let platform: String
    let devices: [JSON]
    /// How many of this platform the filter removed. Kept per column so an
    /// empty column can say which of the two things it means.
    let hiddenByFilter: Int

    var id: String { platform }

    /// An empty column with something hidden is a statement about the filter.
    var hidEverything: Bool { devices.isEmpty && hiddenByFilter > 0 }
    /// An empty column with nothing hidden is a statement about the machine
    /// -- and specifically about *this platform*, which is the answer the
    /// split makes possible: "no Android device is connected" used to be
    /// indistinguishable from "the Android devices are further down".
    var noneOnThisPlatform: Bool { devices.isEmpty && hiddenByFilter == 0 }
}

extension DeviceFilter {
    /// Groups devices into one column per platform.
    ///
    /// Replaces the row-major two-column split. That split kept reading order
    /// but put an iPad next to a Pixel, so telling at a glance what was
    /// connected on each side meant reading every row's platform chip.
    ///
    /// Android and iOS always get a column, in that order, even when empty:
    /// an absent column would be silently different from an empty one, and
    /// "no Android device is connected" is a real and useful answer that the
    /// mixed list could not express. Any other platform a provider reports
    /// gets its own column after those two rather than being dropped -- a
    /// device this view does not know how to label is still a device, and
    /// hiding it would be the one unrecoverable mistake here.
    ///
    /// `beforeFilter` is the unfiltered list, so each column can say how many
    /// of *its own* platform the filter removed. A single total could not:
    /// with one needle hiding every Android device, the Android column has to
    /// blame the filter while the iOS column does not.
    static func byPlatform(_ shown: [JSON],
                           beforeFilter: [JSON]) -> [DevicePlatformColumn] {
        // Ordered, and Android first because that is the order the rest of
        // the app names them in ("Android and iOS are discovered together").
        var order: [String] = ["android", "ios"]
        for d in beforeFilter {
            let p = d["platform"].text
            // An empty platform string is its own bucket rather than being
            // folded into one of the known two: guessing would attribute a
            // device to a platform nothing said it was on.
            let key = p.isEmpty ? "unknown" : p
            if !order.contains(key) { order.append(key) }
        }
        return order.map { key in
            let mine = shown.filter { matches($0, platform: key) }
            let allMine = beforeFilter.filter { matches($0, platform: key) }
            return DevicePlatformColumn(platform: key, devices: mine,
                                        hiddenByFilter: allMine.count - mine.count)
        }
    }

    private static func matches(_ device: JSON, platform key: String) -> Bool {
        let p = device["platform"].text
        return p.isEmpty ? key == "unknown" : p == key
    }
}

extension DeviceFilter {
    /// The share of the width each platform column should get.
    ///
    /// Equal halves are wrong here, and the machine this was built on shows
    /// why: one Android emulator beside twenty-five simulators left half the
    /// window empty while the iOS column scrolled. The split was supposed to
    /// make the list easier to read and that made it harder.
    ///
    /// So width follows the number of devices -- with a floor, because a
    /// column squeezed to one twenty-sixth of the window cannot show a device
    /// id, and a column too narrow to read is worse than one that is emptier
    /// than it needs to be. The floor also keeps an empty column wide enough
    /// to show *why* it is empty, which is a sentence that has to be legible.
    ///
    /// Returns one weight per column, in the order given, summing to 1.
    static func columnWeights(_ columns: [DevicePlatformColumn],
                              minimumShare: Double = 0.26) -> [Double] {
        guard !columns.isEmpty else { return [] }
        // A floor that cannot be satisfied is not a floor. With four columns
        // at 0.26 there is no room, so it gives way to equal shares rather
        // than producing weights that overflow the window.
        let floor = min(minimumShare, 1.0 / Double(columns.count))
        let equal = Array(repeating: 1.0 / Double(columns.count),
                          count: columns.count)
        let counts = columns.map { Double($0.devices.count) }
        let total = counts.reduce(0, +)
        guard total > 0 else { return equal }

        // Proportional, then lifted to the floor. Lifting takes width from
        // the columns above the floor in proportion to what they have, so the
        // biggest column pays the most for it.
        var weights = counts.map { $0 / total }
        let owed = weights.map { max(0, floor - $0) }.reduce(0, +)
        guard owed > 0 else { return weights }
        let spare = weights.enumerated()
            .map { $0.element > floor ? $0.element - floor : 0 }
        let totalSpare = spare.reduce(0, +)
        // Not enough headroom anywhere to fund the floor: equal shares are
        // the honest fallback.
        guard totalSpare >= owed else { return equal }
        for i in weights.indices {
            weights[i] = weights[i] < floor
                ? floor
                : weights[i] - owed * (spare[i] / totalSpare)
        }
        return weights
    }
}
