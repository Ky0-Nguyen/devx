import SwiftUI

/// Status colours, derived from what the engine actually means by each value.
///
/// The mapping matters: `unknown` must never look like a failure and must
/// never look like success. It is a third thing, and it gets a third colour.
enum StatusTone {
    case good, caution, bad, neutral

    var color: Color {
        switch self {
        case .good: return .green
        case .caution: return .orange
        case .bad: return .red
        case .neutral: return .secondary
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

/// A small status chip.
struct Chip: View {
    let text: String
    var tone: StatusTone = .neutral
    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .medium, design: .rounded))
            .padding(.horizontal, 7).padding(.vertical, 2)
            .background(tone.color.opacity(0.16), in: Capsule())
            .foregroundStyle(tone == .neutral ? Color.secondary : tone.color)
            .overlay(Capsule().strokeBorder(tone.color.opacity(0.28)))
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
                .font(.caption)
                .foregroundStyle(.secondary)
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
    private var icon: String {
        switch kind { case .info: return "info.circle.fill"
                      case .caution: return "exclamationmark.triangle.fill"
                      case .bad: return "xmark.octagon.fill" }
    }
    var body: some View {
        HStack(alignment: .top, spacing: 9) {
            Image(systemName: icon).foregroundStyle(tone.color)
            VStack(alignment: .leading, spacing: 2) {
                if let title { Text(title).font(.callout.weight(.semibold)) }
                Text(message).font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(11)
        .background(tone.color.opacity(0.10), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .strokeBorder(tone.color.opacity(0.25)))
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
                Text(title).font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .top, spacing: 6) {
                        Text("•").foregroundStyle(.tertiary)
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
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                if let subtitle {
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            content
        }
        .padding(14)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
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
