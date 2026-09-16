// A panel that starts closed and says what is inside it anyway.
//
// "default collab đi nào cần thì expand ra sau" -- start collapsed, expand
// what is wanted. The case for it was a screenshot: one `authorization`
// header holding a two-thousand-character bearer token, which pushed the
// request summary, the other five headers and both bodies off the screen.
// Six headers were technically on display and none of them was readable.
//
// Closed is only better if the header still answers "is it worth opening",
// so every section carries a summary -- a count, a size, a state -- beside
// its title. A disclosure that hides whether there is anything behind it
// just moves the problem.
import SwiftUI

struct DisclosureSection<Content: View>: View {
    let title: String
    /// Shown beside the title in both states: `(6)`, `JSON · 4.2 KB`,
    /// `not captured`.
    var summary: String? = nil
    /// Whether it starts open. Closed by default, which is the whole point;
    /// the request summary passes true because it is the row's identity and
    /// is three lines long.
    var startsOpen: Bool = false
    @ViewBuilder var content: Content

    @State private var open: Bool? = nil

    private var isOpen: Bool { open ?? startsOpen }

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            Button {
                open = !isOpen
            } label: {
                HStack(spacing: 7) {
                    Image(systemName: isOpen ? "chevron.down" : "chevron.right")
                        .font(Term.micro).foregroundStyle(Term.green)
                    Text(tr(title).uppercased())
                        .font(Term.font(12, .bold)).kerning(1.1)
                        .foregroundStyle(Term.ink)
                    if let summary, !summary.isEmpty {
                        Text(summary)
                            .font(Term.small).foregroundStyle(Term.dim)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if isOpen {
                Rectangle().fill(Term.line).frame(height: 1)
                content
            }
        }
        .padding(14)
        .background(Term.panel, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Term.line))
    }
}
