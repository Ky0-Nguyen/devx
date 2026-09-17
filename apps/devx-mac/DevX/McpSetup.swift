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

    /// The same path with the home directory written as `~`.
    ///
    /// For display and for anything a shell will see. A guide is read over
    /// someone's shoulder and pasted into a team chat, and the account name
    /// of the machine it was written on is not part of the instruction.
    ///
    /// The prefix has to end at a path separator. `/Users/tan` is a prefix of
    /// `/Users/tanya/...` as a string and is not a parent of it as a path,
    /// and rewriting that one would produce a path that does not exist.
    static func homeRelative(_ path: String, home: String = NSHomeDirectory())
        -> String {
        let root = home.hasSuffix("/") ? String(home.dropLast()) : home
        guard !root.isEmpty, path.hasPrefix(root + "/") else { return path }
        return "~" + path.dropFirst(root.count)
    }

    /// The same path with the home directory written as `${HOME}`.
    ///
    /// For a config file, where the host reads the text and starts the
    /// command itself. Measured with Claude Code: the string is stored in
    /// `~/.claude.json` exactly as written and expanded when the server is
    /// launched, so a pasted `${HOME}` connects. A `~` in the same place
    /// does not -- it is stored literally too, and nothing expands it,
    /// because expanding a tilde is something a shell does.
    static func envRelative(_ path: String, home: String = NSHomeDirectory())
        -> String {
        let root = home.hasSuffix("/") ? String(home.dropLast()) : home
        guard !root.isEmpty, path.hasPrefix(root + "/") else { return path }
        return "${HOME}" + path.dropFirst(root.count)
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
        /// Whether a shell sees this snippet before the host does.
        ///
        /// It decides whether the path can be written as `~`. A command typed
        /// into a terminal is expanded by the shell, so the host receives an
        /// absolute path either way. A JSON or TOML config is read by the host
        /// and the command is spawned directly: `~` is a shell feature, so it
        /// would arrive as a literal tilde and the server would not start.
        /// That is the same class of mistake as naming `mpi` instead of its
        /// path -- a guide that looks right and does not work.
        let shellExpands: Bool
    }

    /// Every recipe, for a given binary path.
    ///
    /// Takes the path rather than resolving it, so the whole catalogue is
    /// testable without a bundle.
    static func recipes(binary: String, home: String = NSHomeDirectory())
        -> [Recipe] {
        // Neither form names the account this machine happens to use. A
        // guide is read over someone's shoulder and pasted into a team chat;
        // whose laptop it was written on is not part of the instruction.
        //
        // Two forms because two different things expand them: a shell
        // expands `~` before the host is even started, and a host expands
        // `${HOME}` when it launches the server. Putting either in the
        // other's place produces a path that is never resolved -- measured,
        // and it fails as "Failed to connect".
        let q = envRelative(binary, home: home)
        let shell = homeRelative(binary, home: home)
        return [
            Recipe(
                id: "claude-code",
                host: "Claude Code",
                location: tr("one command in a terminal"),
                snippet: "claude mcp add -s user devx \(shell) mcp",
                afterwards: tr("`-s user` makes it available in every folder. "
                             + "Without it the server is registered only for "
                             + "the folder you run it in."),
                shellExpands: true),
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
                             + "configuration only at startup."),
                shellExpands: false),
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
                             + "appear."),
                shellExpands: false),
            Recipe(
                id: "codex",
                host: "Codex CLI",
                location: "~/.codex/config.toml",
                snippet: """
                [mcp_servers.devx]
                command = "\(q)"
                args = ["mcp"]
                """,
                afterwards: "",
                shellExpands: false),
        ]
    }

    /// Why one snippet says `~` and the rest say `${HOME}`.
    static var absolutePathNote: String {
        tr("Neither form names your account. The terminal command says `~` "
         + "because your own shell expands it before Claude is started; the "
         + "config files say `${HOME}` because there is no shell there and "
         + "the host expands it itself when it launches the server. They are "
         + "not interchangeable -- a `~` inside a config file is stored as a "
         + "tilde, nothing expands it, and the server fails to connect. "
         + "Measured with Claude Code. If a host of yours passes `${HOME}` "
         + "through unexpanded, replace it with the full path shown above.")
    }

    // MARK: - Registering with Claude Code without copying anything

    /// Where Claude Code's own CLI is, or nil when it is not installed.
    ///
    /// Searched by path rather than found on PATH, because an app launched
    /// from the Dock gets `/usr/bin:/bin:/usr/sbin:/sbin` and nothing else --
    /// measured. That is the same trap that made the Devices tab report zero
    /// Android devices while `adb` worked in a terminal, so the same fix:
    /// name the places it actually installs to.
    static func claudeBinary(home: String = NSHomeDirectory(),
                             exists: (String) -> Bool = {
                                 FileManager.default.isExecutableFile(atPath: $0)
                             }) -> String? {
        let candidates = [
            home + "/.local/bin/claude",
            home + "/.claude/local/claude",
            "/opt/homebrew/bin/claude",
            "/usr/local/bin/claude",
        ]
        return candidates.first(where: exists)
    }

    /// The command a `claude mcp get <name>` reply says is registered.
    ///
    /// Parsed rather than assumed so a registration left over from a build
    /// tree can be told apart from the bundled one -- which is exactly what
    /// happened here: the server was registered as `build/bin/mpi mcp` and
    /// kept working until the build directory moved.
    ///
    /// nil when the reply names no command, which includes the reply for a
    /// server that is not registered at all.
    static func registeredCommand(fromGet output: String) -> String? {
        for line in output.split(separator: "\n", omittingEmptySubsequences: false) {
            let t = line.trimmingCharacters(in: .whitespaces)
            guard t.hasPrefix("Command:") else { continue }
            let value = t.dropFirst("Command:".count)
                .trimmingCharacters(in: .whitespaces)
            return value.isEmpty ? nil : value
        }
        return nil
    }

    /// What the button should offer, given what is already registered.
    enum Registration: Equatable {
        /// Claude Code's CLI is not on this machine.
        case noClaude
        /// Nothing is registered under this name yet.
        case absent
        /// Registered, and pointing at this bundle's CLI.
        case current
        /// Registered, but pointing somewhere else -- the path it names.
        ///
        /// Kept as its own case rather than folded into `current`, because
        /// "already configured" would be a lie for a registration that names
        /// a binary that may no longer exist.
        case stale(String)
    }

    /// Reads a `claude mcp get` result into that decision.
    ///
    /// `found` is the CLI's exit status: it answers 1 and explains itself on
    /// stderr for a name it does not know, which is an answer and not a
    /// failure.
    static func registration(found: Bool, output: String,
                             bundled: String) -> Registration {
        guard found, let command = registeredCommand(fromGet: output) else {
            return .absent
        }
        return command == bundled ? .current : .stale(command)
    }

    /// The `claude` arguments that register this bundle's CLI.
    ///
    /// The absolute path, because these are arguments to `execve` and no
    /// shell will expand a tilde in them. The snippet a reader copies can say
    /// `~` precisely because their shell gets there first.
    static func addArguments(binary: String) -> [String] {
        ["mcp", "add", "-s", "user", "devx", binary, "mcp"]
    }

    /// The arguments that drop an existing registration, for replacing a
    /// stale one. `add` refuses to overwrite, by design, so a replacement is
    /// two steps and the second one is this first.
    static var removeArguments: [String] {
        ["mcp", "remove", "-s", "user", "devx"]
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
