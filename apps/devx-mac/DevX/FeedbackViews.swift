import SwiftUI

/// The confirmation bar: what a click changed, and buttons to the screens
/// that use it. Shown under the toolbar on every tab; it clears itself.
struct NextStepBar: View {
    @EnvironmentObject var state: AppState
    let step: NextStep

    var body: some View {
        let tabs = NextSteps.offered(step.tabs, from: state.tab)
        HStack(spacing: 8) {
            Text("✓").font(Term.font(12, .bold)).foregroundStyle(Term.green)
            Text(step.message).font(Term.body).lineLimit(1).truncationMode(.middle)
            if let hint = step.hint {
                Text(hint).font(Term.small).foregroundStyle(Term.dim).lineLimit(2)
            }
            if !tabs.isEmpty {
                Text(tr("next:")).font(Term.small).foregroundStyle(Term.dim)
                ForEach(tabs) { tab in
                    Button(tab.title) {
                        state.tab = tab
                        state.nextStep = nil
                    }
                    .buttonStyle(TermButtonStyle())
                    .controlSize(.small)
                }
            }
            Spacer(minLength: 0)
            Button { state.nextStep = nil } label: { Text("[dismiss]").font(Term.font(11)) }
                .buttonStyle(.plain)
                .foregroundStyle(Term.dim)
        }
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(Term.green.opacity(0.10))
        .overlay(alignment: .bottom) { Rectangle().fill(Term.green.opacity(0.4)).frame(height: 1) }
    }
}

/// Everything in flight, at the foot of the sidebar, on every tab -- the busy
/// overlay covers only the screen it was started from, and not Live at all.
struct ActivityLine: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        let all = state.inFlight + state.backgroundOps
        if let current = all.last {
            HStack(spacing: 6) {
                AsciiSpinner(color: Term.amber)
                Text(current).font(Term.font(10)).foregroundStyle(Term.amber)
                    .lineLimit(1).truncationMode(.tail)
                if all.count > 1 {
                    Text(cat("+", String(all.count - 1)))
                        .font(Term.font(10)).foregroundStyle(Term.dim)
                }
            }
            .help(all.joined(separator: "\n"))
        }
    }
}

/// A button label that turns into a spinner and a progressive verb while its
/// work runs. The button should also be `.disabled(busy)` so it cannot be
/// pressed twice.
struct BusyLabel: View {
    let title: String
    let busy: Bool
    var busyTitle: String? = nil

    var body: some View {
        if busy {
            HStack(spacing: 5) {
                AsciiSpinner(color: Term.dim)
                Text(busyTitle ?? cat(title, "…"))
            }
        } else {
            Text(title)
        }
    }
}
