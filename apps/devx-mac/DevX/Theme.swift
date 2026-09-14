import SwiftUI

/// The terminal palette.
///
/// A phosphor console, not decoration for its own sake: dark ground, one
/// bright ink, and hairlines instead of shadows. The rule the palette has to
/// respect is the same one the engine respects -- `unknown` is a third thing,
/// so it gets a third colour and is never allowed to borrow green's or red's.
enum Term {
    /// The ground. Almost black, with just enough green in it to read as a
    /// phosphor screen rather than a grey window.
    static let bg = Color(red: 0.035, green: 0.047, blue: 0.041)
    static let panel = Color(red: 0.067, green: 0.086, blue: 0.075)
    static let raised = Color(red: 0.094, green: 0.118, blue: 0.102)
    /// Hairline rules and panel borders, in place of shadows and materials.
    static let line = Color(red: 0.16, green: 0.27, blue: 0.21)
    /// Body text: bright enough to read for a long session, not pure white.
    static let ink = Color(red: 0.82, green: 0.94, blue: 0.84)
    static let dim = Color(red: 0.49, green: 0.62, blue: 0.53)

    static let green = Color(red: 0.31, green: 0.93, blue: 0.47)
    static let amber = Color(red: 1.00, green: 0.75, blue: 0.24)
    static let red = Color(red: 1.00, green: 0.37, blue: 0.37)
    /// Reserved for "no claim": unknown, inconclusive, skipped. It has to be
    /// unmistakable against green and red, which is the whole point of it.
    static let cyan = Color(red: 0.38, green: 0.84, blue: 0.95)
}

/// Applies the console chrome: ground, ink, accent and a monospaced face for
/// every label in the window. Dark is forced rather than followed, because
/// half this palette stops meaning anything on a white ground.
struct TerminalChrome: ViewModifier {
    func body(content: Content) -> some View {
        content
            .fontDesign(.monospaced)
            .foregroundStyle(Term.ink)
            .tint(Term.green)
            .background(Term.bg)
            .preferredColorScheme(.dark)
    }
}

extension View {
    func terminalChrome() -> some View { modifier(TerminalChrome()) }

    /// A faint glow, the way a bright glyph blooms on a CRT. Used only on
    /// accents, never on body text, where it would just smear.
    func phosphor(_ color: Color, radius: CGFloat = 4) -> some View {
        shadow(color: color.opacity(0.45), radius: radius)
    }

    /// A console pane: flat fill, hairline border, corners barely rounded.
    /// Replaces the stock translucent materials, which read as a grey window
    /// sitting on the palette rather than part of it.
    func termCard(selected: Bool = false, radius: CGFloat = 3) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius)
        return background(selected ? Term.green.opacity(0.09) : Term.panel,
                          in: shape)
            .overlay(shape.strokeBorder(
                selected ? Term.green.opacity(0.65) : Term.line))
    }
}

/// Status colours, derived from what the engine actually means by each value.
///
/// The mapping matters: `unknown` must never look like a failure and must
/// never look like success. It is a third thing, and it gets a third colour.
enum StatusTone {
    case good, caution, bad, neutral

    var color: Color {
        switch self {
        case .good: return Term.green
        case .caution: return Term.amber
        case .bad: return Term.red
        case .neutral: return Term.cyan
        }
    }

    static func capability(_ status: String) -> StatusTone {
        switch status {
        case "available": return .good
        case "limited": return .caution
        case "permission_denied", "unsupported": return .bad
        default: return .neutral          // unknown
        }
    }
    static func trust(_ state: String) -> StatusTone {
        switch state {
        case "authorized": return .good
        case "offline", "unknown": return .caution
        default: return .bad              // unauthorized, untrusted, locked
        }
    }
    static func runtime(_ state: String) -> StatusTone {
        switch state {
        case "running": return .good
        case "unknown": return .caution   // not "not running"
        default: return .neutral
        }
    }
    static func profiling(_ state: String) -> StatusTone {
        switch state {
        case "available": return .good
        case "limited", "permission_required": return .caution
        case "unavailable": return .bad
        default: return .neutral
        }
    }
    static func severity(_ s: String) -> StatusTone {
        switch s {
        case "high": return .bad
        case "medium": return .caution
        case "low": return .neutral
        default: return .neutral
        }
    }
    static func detection(_ s: String) -> StatusTone {
        switch s {
        case "observed": return .good     // measured
        case "suspected": return .caution // indirect or proxy evidence
        default: return .neutral          // inconclusive
        }
    }
    static func outcome(_ s: String) -> StatusTone {
        switch s {
        case "ran_found_issues": return .caution
        case "ran_found_nothing": return .good
        default: return .neutral          // skipped: it did not run
        }
    }
}

/// The four-frame spinner every console tool has used forever. It replaces the
/// stock indeterminate spinner, which is the one control that gave the window
/// away as an ordinary app.
struct AsciiSpinner: View {
    var color: Color = Term.green
    private static let frames = ["|", "/", "-", "\\"]
    @State private var index = 0
    var body: some View {
        Text(AsciiSpinner.frames[index])
            .font(.system(size: 12, weight: .bold, design: .monospaced))
            .foregroundStyle(color)
            .frame(width: 10)
            .task {
                // Driven by a plain sleep loop rather than an animation, so it
                // keeps its exact four frames instead of tweening between them.
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 110_000_000)
                    index = (index + 1) % AsciiSpinner.frames.count
                }
            }
    }
}

/// A block cursor that blinks: the console's way of saying it is still
/// attached and waiting. Used where something is genuinely still arriving.
struct BlinkingCursor: View {
    var color: Color = Term.green
    @State private var lit = true
    var body: some View {
        Text("▊")
            .font(.system(size: 12, design: .monospaced))
            .foregroundStyle(color.opacity(lit ? 1 : 0.1))
            .phosphor(color, radius: lit ? 3 : 0)
            .task {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 530_000_000)
                    lit.toggle()
                }
            }
    }
}

/// A console button: bracketed label, square corners, hairline border. The
/// stock bordered buttons were the last obviously native control left in the
/// window.
struct TermButtonStyle: ButtonStyle {
    /// Primary actions take a filled bar; everything else stays outlined, so
    /// there is still one obvious thing to press per panel.
    var tone: Color = Term.green
    var filled = false
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: 2)
        let live = enabled ? tone : Term.dim
        // On a filled bar the brackets have to switch to the label's ink;
        // drawn in the tone they vanish into the fill behind them.
        let bracket = filled ? Term.bg.opacity(0.55) : live.opacity(0.55)
        return HStack(spacing: 4) {
            Text("[").foregroundStyle(bracket)
            configuration.label
            Text("]").foregroundStyle(bracket)
        }
        .font(.system(size: 12, weight: .semibold, design: .monospaced))
        .foregroundStyle(filled ? Term.bg : live)
        .padding(.horizontal, 9).padding(.vertical, 4)
        .background(filled ? live.opacity(enabled ? 0.9 : 0.4)
                           : live.opacity(0.10), in: shape)
        .overlay(shape.strokeBorder(live.opacity(filled ? 0.9 : 0.5)))
        .opacity(configuration.isPressed ? 0.6 : 1)
    }
}

/// A console input: square, hairline, and dark enough that the caret is the
/// brightest thing in it.
struct TermFieldStyle: TextFieldStyle {
    func _body(configuration: TextField<Self._Label>) -> some View {
        configuration
            .textFieldStyle(.plain)
            .font(.system(size: 12, design: .monospaced))
            .padding(.horizontal, 7).padding(.vertical, 4)
            .background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
            .overlay(RoundedRectangle(cornerRadius: 2)
                .strokeBorder(Term.line))
    }
}

/// An empty state, printed rather than illustrated. The stock
/// `ContentUnavailableView` brings its own large system type and a glyph, both
/// of which read as a different application than the one around it.
struct TermEmpty: View {
    let title: String
    let detail: String
    /// A short, lowercase hint of what to do, shown as a shell would offer it.
    var hint: String? = nil

    var body: some View {
        VStack(spacing: 9) {
            Text("░▒▓").font(.system(size: 20, design: .monospaced))
                .foregroundStyle(Term.line)
            Text(title.uppercased())
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .kerning(1.6)
            Text(detail)
                .font(.system(size: 12, design: .monospaced))
                .foregroundStyle(Term.dim)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            if let hint {
                Text("$ \(hint)")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(Term.green.opacity(0.75))
            }
        }
        .padding(28)
        // Capped so the prose stays readable, then centred across whatever
        // width it is given. Height is deliberately finite: inside a
        // ScrollView an infinite one has nothing to resolve against.
        .frame(maxWidth: 420)
        .frame(maxWidth: .infinity, minHeight: 200)
    }
}

/// A small status chip.
struct Chip: View {
    let text: String
    var tone: StatusTone = .neutral
    /// Square, not a capsule: a console has no rounded corners.
    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: 2) }
    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .medium, design: .monospaced))
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(tone.color.opacity(0.13), in: shape)
            .foregroundStyle(tone.color)
            .overlay(shape.strokeBorder(tone.color.opacity(0.45)))
            .fixedSize()
    }
}

/// A labelled row in a details panel.
struct Field<Content: View>: View {
    let label: String
    @ViewBuilder var content: Content
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(label)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Term.dim)
                .frame(width: 128, alignment: .trailing)
            content
            Spacer(minLength: 0)
        }
    }
}

/// A banner that carries a caveat the reader must not miss.
struct Banner: View {
    enum Kind { case info, caution, bad }
    let kind: Kind
    let title: String?
    let message: String

    private var tone: StatusTone {
        switch kind { case .info: return .neutral
                      case .caution: return .caution
                      case .bad: return .bad }
    }
    /// A typed marker instead of a symbol, so the severity survives being
    /// copied out of the window as text.
    private var marker: String {
        switch kind { case .info: return "[i]"
                      case .caution: return "[!]"
                      case .bad: return "[x]" }
    }
    var body: some View {
        HStack(alignment: .top, spacing: 9) {
            Text(marker)
                .font(.system(size: 12, weight: .bold, design: .monospaced))
                .foregroundStyle(tone.color)
            VStack(alignment: .leading, spacing: 2) {
                if let title {
                    Text(title.uppercased())
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .kerning(0.8)
                        .foregroundStyle(tone.color)
                }
                Text(message).font(.callout).foregroundStyle(Term.ink.opacity(0.85))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(11)
        .background(tone.color.opacity(0.07), in: RoundedRectangle(cornerRadius: 2))
        .overlay(RoundedRectangle(cornerRadius: 2)
            .strokeBorder(tone.color.opacity(0.45)))
        // A bar down the leading edge, the way a console marks a flagged line.
        .overlay(alignment: .leading) {
            Rectangle().fill(tone.color).frame(width: 2)
        }
    }
}

/// A bulleted list used for the evidence and caveat sections, which are the
/// substance of an issue rather than decoration.
struct BulletList: View {
    let title: String
    let items: [String]
    var body: some View {
        if !items.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text(title.uppercased())
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .kerning(0.7)
                    .foregroundStyle(Term.dim)
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .top, spacing: 6) {
                        Text("-").foregroundStyle(Term.green.opacity(0.75))
                        Text(item).font(.callout)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
    }
}

/// Section container.
struct Panel<Content: View>: View {
    let title: String
    var subtitle: String? = nil
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 7) {
                    Text("▌").foregroundStyle(Term.green).phosphor(Term.green, radius: 3)
                    Text(title.uppercased())
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .kerning(1.1)
                    Spacer(minLength: 0)
                }
                if let subtitle {
                    Text(subtitle).font(.caption).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Rectangle().fill(Term.line).frame(height: 1)
            }
            content
        }
        .padding(14)
        .background(Term.panel, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Term.line))
    }
}

func formatNs(_ ns: Double) -> String {
    if ns < 1_000 { return String(format: "%.0f ns", ns) }
    if ns < 1_000_000 { return String(format: "%.2f µs", ns / 1_000) }
    if ns < 1_000_000_000 { return String(format: "%.2f ms", ns / 1_000_000) }
    return String(format: "%.3f s", ns / 1_000_000_000)
}

func formatBytes(_ b: Double) -> String {
    if b < 1024 { return String(format: "%.0f B", b) }
    if b < 1024 * 1024 { return String(format: "%.1f KB", b / 1024) }
    if b < 1024 * 1024 * 1024 { return String(format: "%.1f MB", b / 1_048_576) }
    return String(format: "%.2f GB", b / 1_073_741_824)
}
