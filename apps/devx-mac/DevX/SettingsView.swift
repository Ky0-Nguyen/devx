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
    @State private var egress: JSON = .null

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                languagePanel
                exportPanel
                pathsPanel
                privacyPanel
            }
            .padding(14)
        }
        .navigationTitle("~/export")
    }

    /// The interface language, and the boundary of what it changes.
    ///
    /// The note is not boilerplate. A Vietnamese reader who sees the tabs and
    /// panels in Vietnamese will reasonably read the English left in the
    /// Issues tab as an unfinished translation, when in fact it is deliberate:
    /// that text is written into the session package and compared across runs,
    /// so it has to be the same text on every machine.
    @ViewBuilder private var languagePanel: some View {
        Panel(title: "Language",
              subtitle: "the app's own interface only") {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 10) {
                    Text(tr("Interface language"))
                        .font(Term.body).foregroundStyle(Term.dim)
                    Picker("", selection: $state.language) {
                        ForEach(DevXLanguage.allCases) { lang in
                            // Each language named in itself, so it is legible
                            // to the person looking for it.
                            Text(lang.label).tag(lang)
                        }
                    }
                    .labelsHidden()
                    .frame(width: 180)
                    Spacer(minLength: 0)
                }
                Text(tr("Findings, coverage notes and refusal messages come "
                     + "from the analysis core and stay in English. They are "
                     + "written into the session package and compared across "
                     + "runs, so they are evidence rather than interface: "
                     + "translating them would make two captures of the same "
                     + "app incomparable because the machines were configured "
                     + "differently."))
                    .font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    @ViewBuilder private var exportPanel: some View {
        Panel(title: "Export the open session",
              subtitle: "the package's own report, copied unchanged") {
            VStack(alignment: .leading, spacing: 8) {
                if state.selectedSession.isEmpty {
                    Text(tr("No session is open. Open one from Sessions first."))
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
                        Button(tr("Export Markdown…")) { save(format: "markdown") }
                            .buttonStyle(TermButtonStyle())
                        Button(tr("Export JSON…")) { save(format: "json") }
                            .buttonStyle(TermButtonStyle())
                        Spacer(minLength: 0)
                    }
                    if !state.lastExport.isEmpty {
                        Text(state.lastExport)
                            .font(Term.font(11)).foregroundStyle(Term.green)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text(tr("A suppressed finding stays in the exported document, "
                         + "with its reason: that is what makes a suppression "
                         + "auditable rather than a deletion."))
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
                            Button(tr("Choose…")) { chooseSessionsDir() }
                                .buttonStyle(TermButtonStyle())
                            Button(tr("Reload")) {
                                // The remembered targets and the suppression
                                // list both live beside the sessions, so a
                                // changed directory changes all three.
                                state.loadSessions()
                                state.loadSuppressions()
                                state.loadRecents()
                            }
                            .buttonStyle(TermButtonStyle())
                        }
                        Text(tr("Use Choose… rather than typing a path into "
                             + "Documents, Desktop or Downloads: macOS gates "
                             + "those and an ad-hoc signed build cannot raise "
                             + "the prompt, so a typed path there cannot be "
                             + "read at all."))
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

    /// An egress ledger rather than a blanket claim. "Nothing leaves this
    /// machine" stopped being true when BrowserStack and the Intelligence
    /// connectors arrived: each of them sends something, deliberately and
    /// only when asked, and this lists exactly what, to where and when.
    private var privacyPanel: some View {
        Panel(title: "What leaves this machine",
              subtitle: "Captures, sessions, reports and stored evidence stay here. What can leave, and when:") {
            VStack(alignment: .leading, spacing: 7) {
                ForEach(Array(egress["ledger"].array.enumerated()), id: \.offset) { _, row in
                    VStack(alignment: .leading, spacing: 1) {
                        Text(row["what"].text).font(Term.font(11)).foregroundStyle(Term.ink)
                            .fixedSize(horizontal: false, vertical: true)
                        Text(tr("to ") + row["to"].text + " · " + row["when"].text)
                            .font(Term.font(10)).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Text(tr("Exporting writes a local file. The SDK transport is the only network listener: "
                     + "it binds 127.0.0.1 only, requires a token, and receives markers rather than "
                     + "sending anything. Provider credentials stay in the Keychain or the environment "
                     + "and are never written to a file, a command line or an AI tool's context."))
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .onAppear {
            let dir = state.sessionsDir
            DispatchQueue.global(qos: .userInitiated).async {
                let doc = Core.intelEgress(dir)
                DispatchQueue.main.async { egress = doc }
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
