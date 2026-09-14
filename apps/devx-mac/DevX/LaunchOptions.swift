import Foundation

/// Launch arguments, so a session can be opened straight from the CLI:
///
///     open -a DevX --args --session s-2026... --sessions-dir ~/.mpi/sessions
///
/// Useful on its own, and it means the UI can be driven without Accessibility
/// permission.
struct LaunchOptions {
    var sessionsDir: String?
    var session: String?
    var tab: DevXTab?

    static func parse(_ argv: [String]) -> LaunchOptions {
        var o = LaunchOptions()
        var i = 0
        while i < argv.count {
            let a = argv[i]
            let next: String? = i + 1 < argv.count ? argv[i + 1] : nil
            switch a {
            case "--sessions-dir":
                if let v = next { o.sessionsDir = (v as NSString).expandingTildeInPath; i += 1 }
            case "--session":
                if let v = next { o.session = v; i += 1 }
            case "--tab":
                if let v = next { o.tab = DevXTab(rawValue: v); i += 1 }
            default: break
            }
            i += 1
        }
        return o
    }
}
