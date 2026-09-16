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
    /// `memory.rss_total_bytes` reads as `rss_total`: the panel is already
    /// titled Memory and the `_bytes` suffix is what the value's own unit
    /// says. A `cpu.` prefix is kept, because those sit in the same list and
    /// dropping it would leave `process_time_ns` looking like a memory family.
    static func label(_ name: String) -> String {
        var out = name
        if out.hasPrefix("memory.") { out.removeFirst("memory.".count) }
        if out.hasSuffix("_bytes") { out.removeLast("_bytes".count) }
        return out
    }
}
