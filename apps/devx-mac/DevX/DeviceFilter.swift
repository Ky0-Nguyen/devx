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

    /// Splits a list into two columns, filling left-to-right by row.
    ///
    /// Row-major rather than column-major: the list is ordered -- usable
    /// devices first, physical before simulated -- and splitting it in half
    /// down the middle would put the second-most-relevant device at the top
    /// of the right column, far from the first. Reading order has to survive
    /// the layout.
    static func columns(_ devices: [JSON], count: Int = 2) -> [[JSON]] {
        guard count > 1, devices.count > count else { return [devices] }
        var cols = Array(repeating: [JSON](), count: count)
        for (i, d) in devices.enumerated() { cols[i % count].append(d) }
        return cols
    }
}
