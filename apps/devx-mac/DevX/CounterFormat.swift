// Rendering a counter's value in the unit it was actually measured in.
//
// The Live tab used to call a byte formatter on every counter, because the
// unit did not travel with the value -- `latest_counters` was a name and a
// number. So `cpu.process_time_ns`, which is nanoseconds, rendered as
// "8782.51 GB" beside the memory families. The number was right and the unit
// was invented, which is the one kind of mistake this project treats as
// worse than showing nothing.
//
// The unit now travels, and this picks the formatter from it. A counter that
// arrives with no unit is shown as a bare number rather than guessed at:
// defaulting to bytes is what produced the gigabytes of CPU time.
import Foundation

enum CounterFormat {
    /// The value, in its own unit.
    static func value(_ raw: Double, unit: String) -> String {
        switch unit {
        case "bytes":    return bytes(raw)
        case "ns":       return duration(nanoseconds: raw)
        case "fraction": return String(format: "%.1f%%", raw * 100)
        // Already a percentage when it arrives: the iOS host collector
        // publishes `cpu.utilisation_percent` that way, and multiplying it
        // again would report 4400% for a busy app.
        case "percent":  return String(format: "%.1f%%", raw)
        case "count":    return count(raw)
        case "":
            // No unit stated. A bare number is the honest rendering; adding a
            // suffix here would be this view deciding what was measured.
            return count(raw)
        default:
            // A unit this app has not been taught. Shown with the unit the
            // provider named, so it is at least not mislabelled.
            return count(raw) + " " + unit
        }
    }

    /// Bytes, in the largest unit that keeps the number readable.
    static func bytes(_ raw: Double) -> String {
        let n = abs(raw)
        if n < 1024 { return String(format: "%.0f B", raw) }
        if n < 1024 * 1024 { return String(format: "%.1f KB", raw / 1024) }
        if n < 1024 * 1024 * 1024 {
            return String(format: "%.1f MB", raw / (1024 * 1024))
        }
        return String(format: "%.2f GB", raw / (1024 * 1024 * 1024))
    }

    /// A duration given in nanoseconds.
    ///
    /// Scaled to whatever keeps it readable, and CPU time over a long capture
    /// reaches hours -- 8782 seconds of process time on a two-and-a-half hour
    /// window is what exposed the original bug.
    static func duration(nanoseconds raw: Double) -> String {
        let ns = abs(raw)
        if ns < 1_000 { return String(format: "%.0f ns", raw) }
        if ns < 1_000_000 { return String(format: "%.1f µs", raw / 1_000) }
        if ns < 1_000_000_000 {
            return String(format: "%.1f ms", raw / 1_000_000)
        }
        let seconds = raw / 1_000_000_000
        if abs(seconds) < 60 { return String(format: "%.2f s", seconds) }
        if abs(seconds) < 3600 {
            return String(format: "%.1f min", seconds / 60)
        }
        return String(format: "%.2f h", seconds / 3600)
    }

    /// A plain count, with thousands separated so six digits are readable.
    static func count(_ raw: Double) -> String {
        if raw != raw.rounded() { return String(format: "%.2f", raw) }
        let n = Int(raw)
        var digits = String(abs(n))
        var grouped: [String] = []
        while digits.count > 3 {
            grouped.insert(String(digits.suffix(3)), at: 0)
            digits = String(digits.dropLast(3))
        }
        grouped.insert(digits, at: 0)
        return (n < 0 ? "-" : "") + grouped.joined(separator: " ")
    }

    /// The label to show for a counter name, with the family prefix dropped.
    ///
    /// `memory.rss_total_bytes` reads as `rss_total` and
    /// `cpu.process_user_time_ns` as `process_user_time`. Both halves are
    /// stated elsewhere on the row: the family by the panel the row is in,
    /// the unit by the value beside it.
    ///
    /// The `cpu.` prefix used to be kept deliberately, because every counter
    /// sat in one list titled Memory and `process_time_ns` would have read as
    /// a memory family there. That list is now split per family, so the
    /// prefix is redundant -- and at 11pt in a 132pt column it wrapped
    /// mid-word into `cpu.process_user_ti` / `me_ns`.
    static func label(_ name: String) -> String {
        var out = name
        if let dot = out.firstIndex(of: ".") {
            out = String(out[out.index(after: dot)...])
        }
        for suffix in ["_bytes", "_ns", "_percent"] where out.hasSuffix(suffix) {
            out.removeLast(suffix.count)
        }
        return out
    }

    /// The family a counter name belongs to: the part before its first dot.
    ///
    /// Empty for a name that states no family, which is not the same as
    /// belonging to a default one.
    static func familyKey(_ name: String) -> String {
        guard let dot = name.firstIndex(of: ".") else { return "" }
        return String(name[name.startIndex..<dot])
    }

    /// One counter as a row: what it reads now, and what this capture cost.
    struct Row {
        let name: String
        let value: Double
        let unit: String
        /// True when `value` is a running total rather than a reading.
        let cumulative: Bool
        /// What a percentage is a percentage of, as the series declared it.
        let normalization: String
        /// The change across the points this capture collected. Absent when
        /// there was no pair to difference -- never zero, which would claim
        /// the app used none of it.
        let delta: Double?
        let deltaSpanNs: Int64?

        /// The number this row leads with.
        ///
        /// For a cumulative counter that is the delta: `cpu.process_time_ns`
        /// reads 3.18 h on a 67-second capture because it counts from when
        /// the process started, and that figure is true and not about this
        /// capture. Where no delta exists yet the absolute is shown rather
        /// than a blank, since it is still a measurement.
        var headline: String {
            guard cumulative, let delta else {
                return CounterFormat.value(value, unit: unit)
            }
            return "+" + CounterFormat.value(delta, unit: unit)
        }

        /// The line under it: the running total the delta was taken from.
        /// Nil when the headline already is the absolute.
        ///
        /// The word follows the number rather than being interpolated into a
        /// sentence, because a sentence template puts the value where one
        /// language's word order wants it and not the other's.
        var footnote: String? {
            guard cumulative, delta != nil else { return nil }
            return CounterFormat.value(value, unit: unit) + " "
                 + tr("cumulative")
        }
    }

    /// One panel's worth of counters.
    struct Family {
        /// The wire prefix: `memory`, `cpu`, or empty when unstated.
        let key: String
        let title: String
        let subtitle: String?
        /// Positions in the array that was passed in, in their original order.
        let indices: [Int]
    }

    /// The rows of a family, a reading before a running total.
    ///
    /// Within a family the rate is what someone reads first -- it is the
    /// number that says what is happening now -- while a cumulative counter
    /// is context for it. On the wire the order is creation order, which puts
    /// `cpu.utilisation_percent` last because it cannot exist until the
    /// second tick. Relative order inside each group is kept, so nothing
    /// jumps around as ticks arrive.
    static func ordered(_ rows: [Row]) -> [Row] {
        return rows.filter { !$0.cumulative } + rows.filter { $0.cumulative }
    }

    /// Counters split into the families they were measured in.
    ///
    /// They used to render as one list under a panel titled Memory, which put
    /// `cpu.process_time_ns` under a heading it is not -- beside a subtitle
    /// about never summing memory families, which the collector is explicit
    /// about: "CPU time is not memory and must never be totalled with one".
    ///
    /// Known families come first in a fixed order so the panels do not
    /// reshuffle as counters arrive tick by tick; anything else keeps the
    /// order it was seen in, and unstated-family counters go last.
    static func families(_ names: [String]) -> [Family] {
        var order: [String] = []
        var members: [String: [Int]] = [:]
        for (i, name) in names.enumerated() {
            let key = familyKey(name)
            if members[key] == nil { order.append(key) }
            members[key, default: []].append(i)
        }
        let known = ["memory", "cpu"]
        let sorted = known.filter { members[$0] != nil }
                   + order.filter { !known.contains($0) && !$0.isEmpty }
                   + order.filter { $0.isEmpty }
        return sorted.map { describe($0, indices: members[$0] ?? []) }
    }

    /// The panels the Live tab draws: each family, with its rows in order.
    ///
    /// The whole arrangement rule lives here rather than in the view, so it
    /// can be tested without SwiftUI.
    static func panels(_ rows: [Row]) -> [(family: Family, rows: [Row])] {
        return families(rows.map { $0.name }).map { family in
            (family, ordered(family.indices.map { rows[$0] }))
        }
    }

    /// A family's heading and the caveat a reader needs to read its numbers.
    ///
    /// An unrecognised family is titled with the prefix the provider used
    /// rather than folded under a heading this app made up, and carries no
    /// caveat, because none is known for it.
    static func describe(_ key: String, indices: [Int]) -> Family {
        switch key {
        case "memory":
            return Family(key: key,
                          title: "Memory",
                          subtitle: "Each family is a separate measurement "
                                  + "and is never summed with another.",
                          indices: indices)
        case "cpu":
            // The 67-second capture that read 10 808 s of process time. The
            // number was right; what was missing was that it is not the
            // window's CPU time.
            return Family(key: key,
                          title: "CPU time",
                          subtitle: "Process CPU time is cumulative since "
                                  + "the process started, so the figure in "
                                  + "front is what this capture cost and the "
                                  + "running total is under it. Utilisation "
                                  + "is a percentage of one core with all "
                                  + "the app's threads counted together, so "
                                  + "it passes 100% when more than one core "
                                  + "is working.",
                          indices: indices)
        case "":
            return Family(key: key,
                          title: "Other counters",
                          subtitle: "These counters arrived without a "
                                  + "family, so they are listed rather than "
                                  + "grouped under one.",
                          indices: indices)
        default:
            return Family(key: key, title: key, subtitle: nil,
                          indices: indices)
        }
    }
}
