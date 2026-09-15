import AppKit
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

extension Term {
    /// The console face.
    ///
    /// Menlo is the target: it ships with every macOS, it is the face Terminal
    /// and Xcode are read in, and unlike Monaco it has a real bold rather than
    /// a synthesised one. The rest of the chain exists only so the window can
    /// never fall back to a proportional face, and nothing is bundled -- a font
    /// file would be the first third-party asset in the repository (ADR-0002).
    /// A family asked for with `--font=<family>`, set before the UI is built.
    /// A face is a matter of taste, and Monaco or an installed coding font is
    /// a reasonable thing to prefer; the default stays fixed so the window
    /// looks the same on every machine.
    static var requestedFace: String?

    static let face: String? = {
        var chain = ["Menlo", "Monaco", "PT Mono", "Courier New"]
        if let asked = requestedFace, !asked.isEmpty {
            if NSFont(name: asked, size: 12) != nil {
                chain.insert(asked, at: 0)
            } else {
                // Said rather than silently ignored -- the window would
                // otherwise come up in a face the operator did not ask for.
                FileHandle.standardError.write(Data(
                    ("DevX: no font family named '\(asked)' is installed; "
                     + "using \(chain[0])\n").utf8))
            }
        }
        for name in chain where NSFont(name: name, size: 12) != nil {
            return name
        }
        return nil
    }()

    /// A face at an exact size. Sizes are fixed rather than scaled: this is a
    /// dense instrument panel, and a grown body size reflows every column.
    static func font(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        guard let face else {
            // No named face resolved, which should not happen on macOS. The
            // system's own monospaced design is still a monospaced face.
            return .system(size: size, weight: weight, design: .monospaced)
        }
        return .custom(face, fixedSize: size).weight(weight)
    }

    /// The type scale. Monospaced glyphs are wider and read larger than the
    /// system face at the same point size, so each step sits a little below
    /// the semantic style it replaces.
    static var micro: Font { font(10) }
    static var small: Font { font(11) }
    static var body: Font { font(12) }
    static var heading: Font { font(13, .bold) }
    static var display: Font { font(18, .bold) }
}

/// Applies the console chrome: ground, ink, accent and a monospaced face for
/// every label in the window. Dark is forced rather than followed, because
/// half this palette stops meaning anything on a white ground.
struct TerminalChrome: ViewModifier {
    func body(content: Content) -> some View {
        content
            .font(Term.body)
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
            .font(Term.font(12, .bold))
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
            .font(Term.font(12))
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
        .font(Term.font(12, .semibold))
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
            .font(Term.font(12))
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
            Text("░▒▓").font(Term.font(20))
                .foregroundStyle(Term.line)
            Text(tr(title).uppercased())
                .font(Term.font(13, .bold))
                .kerning(1.6)
            Text(tr(detail))
                .font(Term.font(12))
                .foregroundStyle(Term.dim)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            if let hint {
                Text("$ \(tr(hint))")
                    .font(Term.font(11))
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
            .font(Term.font(11, .medium))
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
                .font(Term.font(11))
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
                .font(Term.font(12, .bold))
                .foregroundStyle(tone.color)
            VStack(alignment: .leading, spacing: 2) {
                if let title {
                    Text(tr(title).uppercased())
                        .font(Term.font(12, .bold))
                        .kerning(0.8)
                        .foregroundStyle(tone.color)
                }
                Text(tr(message)).font(Term.body).foregroundStyle(Term.ink.opacity(0.85))
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
                Text(tr(title).uppercased())
                    .font(Term.font(11, .semibold))
                    .kerning(0.7)
                    .foregroundStyle(Term.dim)
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .top, spacing: 6) {
                        Text("-").foregroundStyle(Term.green.opacity(0.75))
                        Text(tr(item)).font(Term.body)
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
                    Text(tr(title).uppercased())
                        .font(Term.font(12, .bold))
                        .kerning(1.1)
                    Spacer(minLength: 0)
                }
                if let subtitle {
                    Text(tr(subtitle)).font(Term.small).foregroundStyle(Term.dim)
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
