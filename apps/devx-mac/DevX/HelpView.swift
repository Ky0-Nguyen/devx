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
                        Text(binary).font(Term.micro)
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
                        recipeRow(r)
                    }

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

    @ViewBuilder private func recipeRow(_ r: McpSetup.Recipe) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 8) {
                Text(r.host).font(Term.font(12, .medium))
                Text(r.location).font(Term.micro).foregroundStyle(Term.dim)
                Spacer(minLength: 0)
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
        }
        .padding(.vertical, 4)
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
