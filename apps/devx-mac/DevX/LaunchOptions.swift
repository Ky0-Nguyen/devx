import Foundation

/// Launch arguments, so a session can be opened straight from the CLI:
///
///     open -a DevX --args --session s-2026... --sessions-dir ~/.mpi/sessions
///     open -a DevX --args --device emulator-5554 --app com.example.app --start-live
///
/// Useful on its own, and it means the UI can be driven without Accessibility
/// permission.
struct LaunchOptions {
    var sessionsDir: String?
    var session: String?
    var tab: DevXTab?
    var device: String?
    var app: String?
    /// Begin a live capture as soon as the window appears. Lets a script or a
    /// shell alias start profiling without a click.
    var startLive = false
    /// Stop the live capture automatically after this many seconds. Without it
    /// a live session runs until stopped, which is the point of a live view.
    var liveSeconds: Int?
    /// A monospaced family to render the window in, e.g. `--font=Monaco`. An
    /// uninstalled family is reported on stderr and the default is kept.
    var font: String?

    static func parse(_ argv: [String]) -> LaunchOptions {
        var o = LaunchOptions()
        var i = 0
        while i < argv.count {
            // `--flag=value` is accepted alongside `--flag value`. It is the
            // safer form to script: AppKit pairs a `-`-prefixed argument with
            // whatever follows it, so a space-separated value can be left over
            // as a bare argument, which AppKit then treats as a file to open
            // (see AppDelegate). The joined form never produces one.
            let raw = argv[i]
            let split = raw.firstIndex(of: "=")
            let a = split.map { String(raw[raw.startIndex..<$0]) } ?? raw
            let joined = split.map { String(raw[raw.index(after: $0)...]) }
            let next: String? = joined
                ?? (i + 1 < argv.count ? argv[i + 1] : nil)
            // A joined value is part of this argument, so it must not also
            // advance past the next one.
            let step = joined == nil ? 1 : 0
            switch a {
            case "--sessions-dir":
                if let v = next { o.sessionsDir = (v as NSString).expandingTildeInPath; i += step }
            case "--session":
                if let v = next { o.session = v; i += step }
            case "--tab":
                if let v = next { o.tab = DevXTab(rawValue: v); i += step }
            case "--device":
                if let v = next { o.device = v; i += step }
            case "--app":
                if let v = next { o.app = v; i += step }
            case "--start-live":
                o.startLive = true
            case "--font":
                if let v = next { o.font = v; i += step }
            case "--live-seconds":
                if let v = next, let n = Int(v), n > 0 { o.liveSeconds = n; i += step }
            default: break
            }
            i += 1
        }
        return o
    }
}
