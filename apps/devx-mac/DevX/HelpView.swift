// The Help tab: what every screen is, and how to connect this to an AI tool.
//
// Two things a new reader needs and neither was anywhere in the app. The
// explanations are bilingual on purpose -- see HelpTopics for why -- and the
// connect recipes name the real path of the CLI inside this bundle, because a
// guide that prints `/path/to/mpi` and asks the reader to fill it in
// reproduces the exact mistake it is meant to prevent.
import SwiftUI

struct HelpView: View {
    @State private var copied: String? = nil
    /// What Claude Code already has registered, once looked up. Nil until
    /// then: "not checked" and "not registered" are different states and the
    /// button must not offer to add something that is already there.
    @State private var registration: McpSetup.Registration? = nil
    @State private var installing = false
    /// Everything the CLI said, verbatim. A connect step that failed
    /// silently would be worse than the copy it replaced.
    @State private var installOutput: String = ""
    @State private var installOk = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(tr("What each screen shows, and how to let an AI tool "
                      + "read these reports. Screen names stay in English, "
                      + "because that is what the sidebar says."))
                    .font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)

                connectPanel
                conceptsPanel
                screensPanel
            }
            .padding(14)
        }
        .navigationTitle("~/help")
    }

    // MARK: - Connecting an AI tool

    @ViewBuilder private var connectPanel: some View {
        Panel(title: "Connect an AI tool",
              subtitle: "Model Context Protocol, over stdio") {
            VStack(alignment: .leading, spacing: 10) {
                Text(tr("This tool can serve its captures to Claude, Cursor, "
                      + "Codex or anything else that speaks MCP, so a model "
                      + "can read a whole report instead of a screenshot of "
                      + "one."))
                    .font(Term.small)
                    .fixedSize(horizontal: false, vertical: true)

                if let binary = McpSetup.bundledBinary() {
                    Text(McpSetup.readOnlyNote)
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)

                    Field(label: "the command") {
                        // Written with `~` rather than the account name of
                        // this machine: this panel is read over someone's
                        // shoulder and pasted into a team chat.
                        Text(McpSetup.homeRelative(binary)).font(Term.micro)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text(tr("Shipped inside this app, so the path above is "
                          + "correct on this machine. A host launches it "
                          + "without a shell, which is why a path and not a "
                          + "command name."))
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)

                    ForEach(McpSetup.recipes(binary: binary)) { r in
                        recipeRow(r, binary: binary)
                    }

                    Text(McpSetup.absolutePathNote)
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)

                    Rectangle().fill(Term.line).frame(height: 1)
                    Text(tr("Once it is connected, try asking:"))
                        .font(Term.small).foregroundStyle(Term.dim)
                    ForEach(McpSetup.firstQuestions, id: \.self) { q in
                        Text("· " + q).font(Term.micro)
                            .foregroundStyle(Term.cyan)
                            .textSelection(.enabled)
                    }
                } else {
                    // Honest about the one case where it cannot say a path.
                    Text(tr("The `mpi` command is not inside this app "
                          + "bundle, so there is no path here that is "
                          + "certainly right. A build run before this was "
                          + "added, or a bundle assembled by hand, will look "
                          + "like this. Rebuild, or run `mpi mcp` from "
                          + "wherever you built it."))
                        .font(Term.small).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    /// Registering with Claude Code in one click, inside its own recipe row.
    ///
    /// The copied command stays beside it: this only removes the step for the
    /// host whose own CLI can be asked to do it. Every other host is a config
    /// file, and a GUI rewriting another tool's JSON behind its back is how
    /// someone loses the rest of their servers.
    @ViewBuilder private func claudeCodeButton(binary: String) -> some View {
        Group {
            switch registration {
                case .none:
                    Button(tr("check Claude Code")) { checkRegistration(binary) }
                        .buttonStyle(TermButtonStyle())
                case .noClaude:
                    Text(tr("`claude` not found on this machine"))
                        .font(Term.micro).foregroundStyle(Term.amber)
                case .current:
                    Text(tr("already connected to this app"))
                        .font(Term.micro).foregroundStyle(Term.green)
                case .absent:
                    Button(installing ? tr("connecting…") : tr("connect it now")) {
                        performInstall(binary: binary, replacing: false)
                    }
                    .buttonStyle(TermButtonStyle())
                    .disabled(installing)
                case .stale:
                    Button(installing ? tr("updating…") : tr("point it here")) {
                        performInstall(binary: binary, replacing: true)
                    }
                    .buttonStyle(TermButtonStyle())
                    .disabled(installing)
            }
        }
    }

    /// What the button's state needs saying underneath it.
    @ViewBuilder private func claudeCodeStatus() -> some View {
        VStack(alignment: .leading, spacing: 4) {
            // A registration that names another binary is the failure this
            // app has already had: it pointed at a build tree and kept
            // working until that directory moved.
            if case .stale(let other) = registration {
                Text(tr("registered, but pointing at another binary:") + " "
                     + McpSetup.homeRelative(other))
                    .font(Term.micro).foregroundStyle(Term.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if case .current = registration {
                Text(tr("Nothing to do. Start a new session to use it: a host "
                      + "reads its server list when it starts, so one that "
                      + "was already open will not see this."))
                    .font(Term.micro).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !installOutput.isEmpty {
                Text(installOutput)
                    .font(Term.micro)
                    .foregroundStyle(installOk ? Term.green : Term.amber)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(8)
                    .background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
            }
        }
    }

    private func checkRegistration(_ binary: String) {
        installing = true
        // Off the main thread: it starts a process and waits for it, and a
        // window that blocks on that is a window that beachballs.
        DispatchQueue.global(qos: .userInitiated).async {
            let state = McpInstall.registration(bundled: binary)
            DispatchQueue.main.async {
                registration = state
                installing = false
            }
        }
    }

    private func performInstall(binary: String, replacing: Bool) {
        installing = true
        installOutput = ""
        DispatchQueue.global(qos: .userInitiated).async {
            let outcome = McpInstall.install(bundled: binary,
                                             replacing: replacing)
            let state = McpInstall.registration(bundled: binary)
            DispatchQueue.main.async {
                installOk = outcome.ok
                installOutput = outcome.output.isEmpty
                    ? (outcome.ok ? tr("done") : tr("it failed and said nothing"))
                    : outcome.output
                registration = state
                installing = false
            }
        }
    }

    @ViewBuilder private func recipeRow(_ r: McpSetup.Recipe,
                                       binary: String) -> some View {
        // Claude Code is the one host that can be configured for you, because
        // it ships a CLI whose job is exactly this.
        let isClaudeCode = r.id == "claude-code"
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 8) {
                Text(r.host).font(Term.font(12, .medium))
                Text(r.location).font(Term.micro).foregroundStyle(Term.dim)
                Spacer(minLength: 0)
                if isClaudeCode { claudeCodeButton(binary: binary) }
                Button(copied == r.id ? tr("copied") : tr("copy")) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(r.snippet, forType: .string)
                    copied = r.id
                }
                .buttonStyle(TermButtonStyle())
            }
            Text(r.snippet)
                .font(Term.micro).foregroundStyle(Term.ink)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
                .padding(8)
                .background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
            if !r.afterwards.isEmpty {
                Text(r.afterwards).font(Term.micro)
                    .foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if isClaudeCode { claudeCodeStatus() }
        }
        .padding(.vertical, 4)
        .onAppear {
            if isClaudeCode, registration == nil { checkRegistration(binary) }
        }
    }

    // MARK: - The ideas behind several screens

    @ViewBuilder private var conceptsPanel: some View {
        Panel(title: "Why the numbers look the way they do",
              subtitle: "three rules that shape every screen") {
            VStack(alignment: .leading, spacing: 12) {
                ForEach(HelpTopics.concepts) { t in topicRow(t) }
            }
        }
    }

    @ViewBuilder private var screensPanel: some View {
        Panel(title: "Every screen", subtitle: "in sidebar order") {
            VStack(alignment: .leading, spacing: 12) {
                ForEach(HelpTopics.screens) { t in topicRow(t) }
            }
        }
    }

    /// One topic, in the language the reader chose.
    ///
    /// The title stays as the sidebar spells it -- an English screen name is
    /// what is on screen to look for -- while the explanation follows the
    /// language picker like everything else in the app.
    @ViewBuilder private func topicRow(_ t: HelpTopic) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(t.title).font(Term.font(12, .medium))
                .foregroundStyle(Term.green)
            Text(t.text).font(Term.small)
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
            if !t.fields.isEmpty {
                // The field list is what a screen-level paragraph leaves
                // out: `pss_total`, `cause: unknown`, `limited` are the
                // words someone is actually stuck on. Names are spelled as
                // the screen spells them, so they match by eye.
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(t.fields) { f in
                        VStack(alignment: .leading, spacing: 1) {
                            Text(f.name).font(Term.font(11, .medium))
                                .foregroundStyle(Term.cyan)
                                .textSelection(.enabled)
                            Text(f.text).font(Term.micro)
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                                .textSelection(.enabled)
                        }
                    }
                }
                .padding(.leading, 10)
                .padding(.top, 2)
            }
        }
    }
}
