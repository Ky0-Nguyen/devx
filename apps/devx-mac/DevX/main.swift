import Foundation

/// AppKit reads the process argument vector itself and treats every argument
/// it does not pair with a preceding `-flag` as a document to open. DevX has no
/// document scene, so such a launch made SwiftUI skip creating its window
/// altogether: the process ran with a healthy run loop, zero `NSWindow`s, and
/// `onAppear` never fired — an app that is alive with no UI at all.
///
/// It is easy to hit by accident, because AppKit's pairing is not DevX's. In
///
///     DevX --start-live --live-seconds 30
///
/// AppKit takes `--live-seconds` to be the value of `--start-live`, which
/// leaves `30` bare. The same flags in another order, or `--live-seconds=30`,
/// leave nothing bare and always worked, which is what made this look like a
/// fault in the live-capture code rather than in argument handling.
///
/// The vector cannot be rewritten in place — the Swift runtime and Foundation
/// have both snapshotted it before `main` runs — so DevX moves its options
/// into the environment and re-executes itself with a bare vector that gives
/// AppKit nothing to interpret. That costs one `execv` at startup, only on the
/// launches that would otherwise have failed, and keeps every documented
/// spelling working.
private let optionsVariable = "DEVX_OPTIONS"

/// Separates packed options. A control character, because it is the one thing
/// that cannot appear in an argument DevX was handed.
private let optionSeparator = "\u{1}"

/// Resolves DevX's options, re-executing first if AppKit would misread the
/// argument vector. Does not return in that case.
private func resolveLaunchOptions() -> LaunchOptions {
    let environment = ProcessInfo.processInfo.environment
    if let packed = environment[optionsVariable] {
        // Already re-executed: argv is empty and the options live here.
        return LaunchOptions.parse(
            packed.components(separatedBy: optionSeparator).filter { !$0.isEmpty })
    }

    let arguments = Array(CommandLine.arguments.dropFirst())
    let bare = arguments.contains { !$0.hasPrefix("-") }
    if bare, let executable = Bundle.main.executablePath {
        setenv(optionsVariable, arguments.joined(separator: optionSeparator), 1)
        var vector: [UnsafeMutablePointer<CChar>?] = [strdup(executable), nil]
        execv(executable, &vector)
        // Only reached if the re-exec failed. The options are still valid, so
        // carry on: a window that ignores an argument beats no window, and the
        // UI can start a capture by hand.
    }
    return LaunchOptions.parse(arguments)
}

let devxLaunch = resolveLaunchOptions()

// Set before the first view is built, which is when the face resolves.
Term.requestedFace = devxLaunch.font

DevXApp.main()
