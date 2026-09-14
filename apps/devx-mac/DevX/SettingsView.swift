import SwiftUI
import AppKit

/// The Export / settings view (spec section 13's eleventh view).
///
/// Two jobs, and a statement that belongs with them.
///
/// **Export** copies the open session's stored report to a path the user
/// picks. Copies, not regenerates: what leaves is what was recorded, and if
/// the file no longer matches its checksum the export says so rather than
/// quietly handing over something that changed on disk.
///
/// **Settings** is the sessions directory and the project's suppression file
/// -- the two paths that decide what this app reads and what it applies.
///
/// And the statement: spec section 13 requires "no source or trace upload
/// without configured user consent". There is no consent control here because
/// there is nothing to consent to -- this build has no upload path at all. The
/// only thing it can do with a report is write a local file, and saying that
/// plainly is more useful than a switch that implies an upload exists.
struct SettingsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                exportPanel
                pathsPanel
                privacyPanel
            }
            .padding(14)
        }
        .navigationTitle("~/export")
    }

    @ViewBuilder private var exportPanel: some View {
        Panel(title: "Export the open session",
              subtitle: "the package's own report, copied unchanged") {
            VStack(alignment: .leading, spacing: 8) {
                if state.selectedSession.isEmpty {
                    Text("No session is open. Open one from Sessions first.")
                        .font(Term.font(11)).foregroundStyle(Term.cyan)
                } else {
                    Field(label: "session") {
                        Text(state.selectedSession).font(Term.font(12))
                    }
                    if state.reanalyzed {
                        // The re-analysis lives in memory; the package on disk
                        // still holds what was recorded. Exporting would hand
                        // over the recorded report, not what is on screen, and
                        // that difference has to be visible.
                        Banner(kind: .caution, title: "Showing a re-analysis",
                               message: "The findings on screen were re-run "
                                      + "with the current suppression list. "
                                      + "This export copies the report as it "
                                      + "was recorded, which is not the same "
                                      + "document. Use `mpi analyze "
                                      + "--suppressions` to write a report "
                                      + "with them applied.")
                    }
                    HStack(spacing: 10) {
                        Button("Export Markdown…") { save(format: "markdown") }
                            .buttonStyle(TermButtonStyle())
                        Button("Export JSON…") { save(format: "json") }
                            .buttonStyle(TermButtonStyle())
                        Spacer(minLength: 0)
                    }
                    if !state.lastExport.isEmpty {
                        Text(state.lastExport)
                            .font(Term.font(11)).foregroundStyle(Term.green)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text("A suppressed finding stays in the exported document, "
                         + "with its reason: that is what makes a suppression "
                         + "auditable rather than a deletion.")
                        .font(Term.font(10)).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var pathsPanel: some View {
        Panel(title: "Paths",
              subtitle: "what this app reads, and what the CLI reads with it") {
            VStack(alignment: .leading, spacing: 8) {
                Field(label: "sessions") {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            TextField("", text: $state.sessionsDir)
                                .textFieldStyle(TermFieldStyle())
                            Button("Choose…") { chooseSessionsDir() }
                                .buttonStyle(TermButtonStyle())
                            Button("Reload") {
                                // The remembered targets and the suppression
                                // list both live beside the sessions, so a
                                // changed directory changes all three.
                                state.loadSessions()
                                state.loadSuppressions()
                                state.loadRecents()
                            }
                            .buttonStyle(TermButtonStyle())
                        }
                        Text("Use Choose… rather than typing a path into "
                             + "Documents, Desktop or Downloads: macOS gates "
                             + "those and an ad-hoc signed build cannot raise "
                             + "the prompt, so a typed path there cannot be "
                             + "read at all.")
                            .font(Term.font(10)).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Field(label: "suppressions") {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(Core.suppressionsPath(sessionsDir: state.sessionsDir))
                            .font(Term.font(11)).foregroundStyle(Term.ink)
                            .fixedSize(horizontal: false, vertical: true)
                        Text("`mpi analyze --suppressions` reads this same "
                             + "file, which is what makes a suppression a "
                             + "project decision rather than one machine's "
                             + "preference.")
                            .font(Term.font(10)).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Field(label: "recents") {
                    Text(state.recentsPath)
                        .font(Term.font(11)).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var privacyPanel: some View {
        Panel(title: "What leaves this machine") {
            VStack(alignment: .leading, spacing: 6) {
                Text("Nothing.")
                    .font(Term.font(13, .bold)).foregroundStyle(Term.green)
                Text("Spec section 13 asks for no source or trace upload "
                     + "without configured consent. There is no consent "
                     + "control here because there is nothing to consent to: "
                     + "this build has no upload path. Exporting writes a "
                     + "local file and that is the whole of it.")
                    .font(Term.font(11)).foregroundStyle(Term.ink)
                    .fixedSize(horizontal: false, vertical: true)
                Text("The SDK transport is the only network listener, it binds "
                     + "127.0.0.1 only, it requires a token, and it receives "
                     + "markers rather than sending anything. There is no "
                     + "wireless path, on purpose.")
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func save(format: String) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue =
            state.selectedSession + (format == "json" ? ".json" : ".md")
        panel.canCreateDirectories = true
        // A save panel, rather than a path typed into a field: picking the
        // destination is what grants this app permission to write there.
        if panel.runModal() == .OK, let url = panel.url {
            state.exportSession(format: format, to: url.path)
        }
    }

    private func chooseSessionsDir() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            state.sessionsDir = url.path
            state.loadSessions()
            state.loadSuppressions()
            state.loadRecents()
        }
    }
}
