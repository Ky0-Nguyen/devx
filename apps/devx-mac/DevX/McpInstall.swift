// Running Claude Code's own CLI, so connecting is a button and not a recipe.
//
// The guide was correct and still asked the reader to copy a command into a
// terminal, which is a step that can be got wrong -- and the one thing people
// get wrong here is the path. Pressing a button cannot mistype it.
//
// `claude mcp add` is used rather than writing `~/.claude.json` directly. The
// CLI owns that file's shape, and a GUI editing another tool's configuration
// behind its back is how you lose someone's other servers. Everything the
// subprocess says is shown verbatim: a connect step that failed silently
// would be worse than the copy it replaced.
import Foundation

/// Runs a command and reports what it said, with no shell involved.
enum McpInstall {
    struct Outcome {
        let ok: Bool
        /// Everything the command printed, both streams, trimmed. Shown as
        /// it is: this is the one place the reader can see why it failed.
        let output: String
    }

    /// Runs `claude` with the given arguments.
    ///
    /// No shell: the arguments are passed as an array, so a path with a space
    /// in it needs no quoting and cannot be re-split. The timeout exists
    /// because this runs on a click and the window must not hang on a CLI
    /// that waits for something.
    static func run(_ binary: String, _ arguments: [String],
                    timeout: TimeInterval = 30) -> Outcome {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: binary)
        task.arguments = arguments
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        // A minimal environment, plus HOME, which the CLI needs to find its
        // own user config. Inheriting the app's environment would make the
        // result depend on how the app was launched -- from the Dock or from
        // a shell -- and those differ.
        task.environment = [
            "HOME": NSHomeDirectory(),
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        ]

        do {
            try task.run()
        } catch {
            return Outcome(ok: false,
                           output: "could not run \(binary): "
                                 + error.localizedDescription)
        }

        // Read before waiting. A pipe has a finite buffer, and a process that
        // fills it blocks forever on write while the reader waits for exit.
        var data = Data()
        let handle = pipe.fileHandleForReading
        let reader = DispatchQueue(label: "devx.mcp-install.read")
        let done = DispatchSemaphore(value: 0)
        reader.async {
            data = handle.readDataToEndOfFile()
            done.signal()
        }

        let deadline = Date().addingTimeInterval(timeout)
        while task.isRunning, Date() < deadline {
            usleep(50_000)
        }
        if task.isRunning {
            task.terminate()
            _ = done.wait(timeout: .now() + 2)
            return Outcome(ok: false,
                           output: tr("it did not finish within")
                                 + " \(Int(timeout))s")
        }
        _ = done.wait(timeout: .now() + 2)

        let text = String(data: data, encoding: .utf8) ?? ""
        return Outcome(ok: task.terminationStatus == 0,
                       output: text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// What is registered with Claude Code right now, for the given bundle.
    static func registration(bundled: String) -> McpSetup.Registration {
        guard let claude = McpSetup.claudeBinary() else { return .noClaude }
        let got = run(claude, ["mcp", "get", "devx"], timeout: 15)
        return McpSetup.registration(found: got.ok, output: got.output,
                                     bundled: bundled)
    }

    /// Registers this bundle's CLI, replacing a stale registration first.
    ///
    /// `claude mcp add` refuses to overwrite an existing name -- "MCP server
    /// devx already exists in user config" -- so replacing is remove then
    /// add. The remove is only ever reached for a registration this already
    /// determined points somewhere else.
    static func install(bundled: String, replacing: Bool) -> Outcome {
        guard let claude = McpSetup.claudeBinary() else {
            return Outcome(ok: false,
                           output: tr("Claude Code's `claude` command is not "
                                    + "installed in any of the places this "
                                    + "looks for it."))
        }
        if replacing {
            let removed = run(claude, McpSetup.removeArguments)
            if !removed.ok {
                return Outcome(ok: false,
                               output: tr("the existing registration could "
                                        + "not be removed:") + "\n"
                                     + removed.output)
            }
        }
        return run(claude, McpSetup.addArguments(binary: bundled))
    }
}
