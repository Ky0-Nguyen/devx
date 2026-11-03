import AppKit
import SwiftUI

/// Settings › Updates: this copy's version, how it is updated, and a button.
struct UpdatesPanel: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Panel(title: "Updates",
              subtitle: "from DevX's GitHub releases, only when you ask") {
            VStack(alignment: .leading, spacing: 9) {
                Field(label: "this copy") {
                    Text(cat("DevX ", state.engineVersion)).font(Term.body)
                }
                Field(label: "updated by") {
                    Text(methodText).font(Term.small).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(spacing: 12) {
                    Button { state.checkForUpdates() } label: {
                        BusyLabel(title: tr("Check for updates"),
                                  busy: state.updatePhase == tr("Checking for updates…"),
                                  busyTitle: tr("checking…"))
                    }
                    .buttonStyle(TermButtonStyle(filled: !newer))
                    .disabled(state.updatePhase != nil)
                    Toggle(tr("Check when DevX opens (at most once a day)"), isOn: $state.autoCheckUpdates)
                        .toggleStyle(.checkbox).font(Term.small)
                    Spacer(minLength: 0)
                }
                result
                if let phase = state.updatePhase, phase != tr("Checking for updates…") {
                    HStack(spacing: 7) {
                        AsciiSpinner(color: Term.amber)
                        Text(phase).font(Term.body).foregroundStyle(Term.amber)
                    }
                }
                if !state.updateMessage.isEmpty {
                    Text(state.updateMessage).font(Term.small).foregroundStyle(Term.red)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
            }
        }
    }

    private var info: JSON { state.updateInfo }
    private var newer: Bool { info["newer"].bool == true }

    @ViewBuilder private var result: some View {
        if info["ok"].bool == true {
            if newer {
                VStack(alignment: .leading, spacing: 8) {
                    Text(cat("DevX ", info["latest"].text, " ", tr("is available."), " ",
                             tr("You have"), " ", info["current"].text, "."))
                        .font(Term.font(13, .bold)).foregroundStyle(Term.green)
                    let notes = UpdatePlan.summary(info["notes"].text)
                    if !notes.isEmpty {
                        Text(notes).font(Term.small).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(8)
                            .background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
                    }
                    HStack(spacing: 10) {
                        if state.updateMethod != .development {
                            Button { state.installUpdate() } label: {
                                BusyLabel(title: tr("Update now and restart"),
                                          busy: state.updatePhase != nil, busyTitle: tr("updating…"))
                            }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(state.updatePhase != nil)
                        }
                        if let url = URL(string: info["html_url"].text), url.scheme == "https" {
                            Button(tr("release notes ↗")) { NSWorkspace.shared.open(url) }
                                .buttonStyle(TermButtonStyle())
                        }
                    }
                }
            } else {
                Text(cat("✓ ", tr("You have the latest version."), " (", info["current"].text, ")"))
                    .font(Term.small).foregroundStyle(Term.green)
            }
        }
    }

    private var methodText: String {
        switch state.updateMethod {
        case .homebrew:
            return tr("Homebrew, which installed it: brew update, then brew upgrade --cask devx.")
        case .diskImage:
            return tr("The release's disk image: downloaded from GitHub, its checksum and Developer ID signature checked, then it replaces this copy.")
        case .manual:
            return tr("By hand: this folder cannot be written, so the checked disk image is opened for you to drag to Applications.")
        case .development:
            return tr("git pull and a rebuild: this is a development build.")
        }
    }
}
