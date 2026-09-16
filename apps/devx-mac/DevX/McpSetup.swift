// How to connect this tool to an AI host, with a path that is actually true.
//
// The one thing people get wrong is the path. A host launches the server
// without a shell, so `mpi` on your PATH is not `mpi` on its PATH -- the same
// trap that made the Devices tab report zero Android devices while `adb`
// worked fine in a terminal. Printing `/path/to/mpi` in a guide and letting
// the reader fill it in reproduces the bug.
//
// So the CLI ships inside the app bundle, beside the app's own binary, and
// this resolves it at runtime. The snippets a reader copies therefore name a
// path that exists on their machine. When it is missing -- a build tree where
// the copy did not happen -- that is said plainly rather than papered over
// with a placeholder that looks like a path.
import Foundation

enum McpSetup {
    /// Where the bundled CLI is, or nil when it is not there.
    ///
    /// Beside the running executable, which is `DevX.app/Contents/MacOS/DevX`,
    /// so its sibling is `.../MacOS/mpi`. Checked for executability rather
    /// than existence: a file that cannot be run is not a usable command, and
    /// a host would report that as the server failing to start.
    static func bundledBinary() -> String? {
        guard let exe = Bundle.main.executableURL else { return nil }
        let candidate = exe.deletingLastPathComponent()
            .appendingPathComponent("mpi")
        guard FileManager.default.isExecutableFile(atPath: candidate.path) else {
            return nil
        }
        return candidate.path
    }

    /// One host's configuration, ready to copy.
    struct Recipe: Identifiable {
        let id: String
        /// The host's name as it calls itself.
        let host: String
        /// Where the text goes, in the host's own terms.
        let location: String
        /// What to copy. Empty when there is no binary to name.
        let snippet: String
        /// Anything the reader has to do afterwards.
        let afterwards: String
    }

    /// Every recipe, for a given binary path.
    ///
    /// Takes the path rather than resolving it, so the whole catalogue is
    /// testable without a bundle.
    static func recipes(binary: String) -> [Recipe] {
        let q = binary
        return [
            Recipe(
                id: "claude-code",
                host: "Claude Code",
                location: tr("one command in a terminal"),
                snippet: "claude mcp add -s user devx \(q) mcp",
                afterwards: tr("`-s user` makes it available in every folder. "
                             + "Without it the server is registered only for "
                             + "the folder you run it in.")),
            Recipe(
                id: "claude-desktop",
                host: "Claude Desktop",
                location:
                    "~/Library/Application Support/Claude/claude_desktop_config.json",
                snippet: """
                {
                  "mcpServers": {
                    "devx": {
                      "command": "\(q)",
                      "args": ["mcp"]
                    }
                  }
                }
                """,
                afterwards: tr("Merge this into the file rather than "
                             + "replacing it, then quit Claude Desktop "
                             + "completely and open it again -- it reads the "
                             + "configuration only at startup.")),
            Recipe(
                id: "cursor",
                host: "Cursor",
                location: ".cursor/mcp.json in the project, or ~/.cursor/mcp.json",
                snippet: """
                {
                  "mcpServers": {
                    "devx": {
                      "command": "\(q)",
                      "args": ["mcp"]
                    }
                  }
                }
                """,
                afterwards: tr("Cursor picks it up without a restart in most "
                             + "versions; reload the window if it does not "
                             + "appear.")),
            Recipe(
                id: "codex",
                host: "Codex CLI",
                location: "~/.codex/config.toml",
                snippet: """
                [mcp_servers.devx]
                command = "\(q)"
                args = ["mcp"]
                """,
                afterwards: ""),
        ]
    }

    /// The sentence that has to accompany every recipe.
    ///
    /// Read-only is the default and that is a choice, not an oversight, so it
    /// is stated where someone is about to connect rather than discovered
    /// when a call is refused.
    static var readOnlyNote: String {
        tr("Connected like this the server reads and changes nothing. "
         + "Recording a capture, booting a device and attaching a debugger "
         + "are refused, and say so. Add `--allow-actions` after `mcp` to "
         + "enable them -- a model deciding on its own to try recording is a "
         + "different thing from you asking for it, and a recording cannot be "
         + "un-started.")
    }

    /// What a reader can ask once it is connected, so the first thing they try
    /// is one that works.
    static var firstQuestions: [String] {
        ["list my mpi sessions",
         "read the most recent session and tell me what it found",
         "what are DET-01's thresholds and where do they come from?",
         "which capabilities are unavailable on my device, and why?"]
    }
}
