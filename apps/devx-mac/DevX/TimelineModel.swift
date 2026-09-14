import SwiftUI

// The timeline's pure layer: the bin states, the one rule about reading a
// bin's value, the shape that draws a missing one, and the formatters.
//
// Separated from the view so it can be tested without a running app. These
// are the parts where a mistake becomes a lie -- a bin drawn as zero when
// nothing measured it -- so they are the parts worth pinning with tests.

/// The one rule for turning a bin into a height.
///
/// `value` is read only after `state` has been checked, and only the two
/// states that have a number are allowed to produce one. A renderer that
/// reaches for `value` first gets 0 for every unmeasured bin and draws a
/// capture full of confident zeroes, which is the failure this function
/// exists to make impossible to write by accident.
func barFraction(state: BinState, value: Double?, scale: Double) -> Double {
    switch state {
    case .unmeasured, .noReading:
        return 0
    case .measured, .partial:
        guard let v = value, scale > 0 else { return 0 }
        return max(0, min(1, v / scale))
    }
}

// MARK: - bin states

/// The four states a bin can be in, mirrored from the core's own enum.
///
/// Mirrored rather than inferred: deriving the state from whether `value` is
/// null would work until a fifth state arrived, and then it would silently
/// pick the wrong one.
enum BinState: String, CaseIterable {
    case measured, noReading = "no_reading", partial, unmeasured

    init(_ raw: String) {
        self = BinState(rawValue: raw) ?? .unmeasured
    }

    var label: String {
        switch self {
        case .measured: return "measured"
        case .noReading: return "covered, not sampled"
        case .partial: return "partly covered"
        case .unmeasured: return "NOT MEASURED"
        }
    }

    var detail: String {
        switch self {
        case .measured:
            return "the collector covered this bin. A flat bar here is a real zero."
        case .noReading:
            return "the collector was running; this quantity was not read here."
        case .partial:
            return "a gap falls inside this bin, so the value is wrong-low."
        case .unmeasured:
            return "nothing covered this bin. There is no number, and this is not zero."
        }
    }

    var color: Color {
        switch self {
        case .measured: return Term.green
        case .noReading: return Term.dim
        case .partial: return Term.amber
        case .unmeasured: return Term.cyan
        }
    }
}

/// One bin, drawn. The swatch and the track use the same code so the legend
/// cannot drift from what the tracks actually show.
struct BinSwatch: View {
    let state: BinState
    var fraction: Double = 0.62

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .bottom) {
                switch state {
                case .unmeasured:
                    // Ruled, not empty: an empty cell reads as zero. Full
                    // height, so it cannot be mistaken for a bar, and light
                    // enough that a lane of it does not shout down the tracks
                    // that have real data -- it has to be unmistakable, not
                    // loudest.
                    Rectangle().fill(Term.cyan.opacity(0.07))
                    Picket(pitch: 5, inset: 1)
                        .stroke(Term.cyan.opacity(0.5), lineWidth: 1)
                case .noReading:
                    // A low band of rules, not a flat line. A 3pt line at the
                    // bottom rendered at the same brightness as the lane's own
                    // border and fused with it, so a lane of unsampled bins
                    // looked empty -- which is the one thing this state must
                    // not look like. The rules echo the unmeasured pattern in
                    // dim, at a third of the height, so the two read as
                    // related without being confusable.
                    Picket(pitch: 5, inset: 1)
                        .stroke(Term.dim.opacity(0.85), lineWidth: 1)
                        .frame(height: max(4, geo.size.height * 0.3))
                    Rectangle()
                        .fill(Term.dim.opacity(0.85))
                        .frame(height: 1)
                case .partial:
                    // The bar in amber, ruled: the number is real and the
                    // rules say it is incomplete. Only as tall as it measured
                    // -- a full-height texture would claim otherwise.
                    Rectangle()
                        .fill(Term.amber.opacity(0.45))
                        .frame(height: max(2, geo.size.height * fraction))
                    Picket(pitch: 4, inset: 1)
                        .stroke(Term.amber.opacity(0.9), lineWidth: 1)
                        .frame(height: max(2, geo.size.height * fraction))
                case .measured:
                    Rectangle()
                        .fill(Term.green.opacity(0.85))
                        // A measured zero still gets a mark: 1pt of baseline,
                        // so "the app rendered nothing" is visible as an
                        // answer rather than as an absence.
                        .frame(height: max(1, geo.size.height * fraction))
                }
            }
            // The ZStack must be told to fill, and to align to the bottom.
            // A GeometryReader places its content at its top-leading corner
            // and a ZStack sizes to its tallest child, so without this the
            // bars hung downward from the top of the lane -- an inverted
            // chart -- and the 3pt "not sampled" floor sat at the top edge
            // where the border hid it entirely.
            .frame(width: geo.size.width, height: geo.size.height,
                   alignment: .bottom)
        }
    }
}

/// Vertical rules at a fixed pitch in points.
///
/// Diagonal hatching was the first attempt and it failed for a reason worth
/// keeping: a bin is about six points wide and forty tall, so diagonals drawn
/// per bin overlapped into a solid block, and a lane of unmeasured bins became
/// a slab of colour. Vertical rules at a pitch wider than a bin give one mark
/// per bin and read as a ruled field across a run of them, at any bin width.
struct Picket: Shape {
    let pitch: CGFloat
    let inset: CGFloat

    func path(in rect: CGRect) -> Path {
        var p = Path()
        // Anchored to the rect's own origin so neighbouring bins line up into
        // an even field rather than beating against each other.
        var x = rect.minX + inset
        if rect.width <= pitch { x = rect.midX }
        while x < rect.maxX - inset * 0.5 {
            p.move(to: CGPoint(x: x, y: rect.minY))
            p.addLine(to: CGPoint(x: x, y: rect.maxY))
            x += pitch
        }
        return p
    }
}

// MARK: - formatting

/// A timestamp field the encoder always writes.
///
/// Kept separate from `bin["value"]`, which stays a real optional: a missing
/// window bound is a defect in the encoder, while a missing bin value is the
/// data saying "not measured" and must never acquire a default.
extension JSON {
    var stamp: Int { int ?? 0 }
}

enum Fmt {
    static func duration(_ ns: Int) -> String {
        let sign = ns < 0 ? "-" : ""
        let a = Double(abs(ns))
        if a < 1_000 { return "\(sign)\(Int(a)) ns" }
        if a < 1_000_000 { return String(format: "%@%.1f us", sign, a / 1e3) }
        if a < 1_000_000_000 { return String(format: "%@%.1f ms", sign, a / 1e6) }
        return String(format: "%@%.2f s", sign, a / 1e9)
    }

    /// Signed, and explicit when a band lies outside the window. A gap that
    /// starts before the window began is a real thing a capture can contain.
    static func offset(_ at: Int, _ windowStart: Int) -> String {
        let d = at - windowStart
        if d < 0 { return duration(d) + " (before the window)" }
        return "+" + duration(d)
    }

    static func value(_ v: Double, unit: String) -> String {
        if unit == "ns" { return duration(Int(v)) }
        if unit == "bytes" {
            let mib = v / (1024 * 1024)
            return String(format: "%.1f MiB", mib)
        }
        if v == v.rounded() && abs(v) < 1e15 {
            return "\(Int64(v)) \(unit)"
        }
        return String(format: "%.2f %@", v, unit)
    }
}
