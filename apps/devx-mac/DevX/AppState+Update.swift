import AppKit
import CryptoKit
import Foundation
import Security

/// Checking for and installing a newer DevX. See UpdatePlan for which way a
/// given copy is updated, and core/update for the check itself.
extension AppState {
    static let updateAutoKey = "devx.update.auto"
    static let updateLastKey = "devx.update.lastCheck"

    private static let updateQueue = DispatchQueue(label: "devx.update", qos: .userInitiated)

    var updateMethod: UpdatePlan.Method {
        UpdatePlan.method(bundlePath: Bundle.main.bundlePath,
                          exists: { FileManager.default.fileExists(atPath: $0) },
                          writable: { FileManager.default.isWritableFile(atPath: $0) })
    }

    /// Asks GitHub for the latest release. `quiet` is the launch check: a
    /// failure stays in the Settings panel, and a newer release is announced
    /// in the confirmation bar instead of needing the panel open.
    func checkForUpdates(quiet: Bool = false) {
        guard updatePhase == nil else { return }
        let label = tr("Checking for updates…")
        setUpdatePhase(label)
        Self.updateQueue.async {
            let doc = Core.updateCheck()
            DispatchQueue.main.async {
                self.setUpdatePhase(nil)
                UserDefaults.standard.set(Date(), forKey: Self.updateLastKey)
                self.updateInfo = doc
                self.updateMessage = doc["ok"].bool == true ? "" : doc["error"].text
                if quiet, doc["newer"].bool == true {
                    self.confirm(cat("DevX ", doc["latest"].text, " ", tr("is available.")),
                                 next: [.settings])
                }
            }
        }
    }

    func autoCheckForUpdatesIfDue() {
        let last = UserDefaults.standard.object(forKey: Self.updateLastKey) as? Date
        if UpdatePlan.shouldAutoCheck(enabled: autoCheckUpdates, last: last, now: Date()) {
            checkForUpdates(quiet: true)
        }
    }

    /// Installs the release `checkForUpdates` found, then restarts DevX.
    func installUpdate() {
        guard updatePhase == nil, updateInfo["newer"].bool == true else { return }
        // Restarting ends whatever is running; a capture is not something to
        // lose to an update.
        if liveRunning || inspectStreaming {
            updateMessage = tr("Stop the running capture first: updating restarts DevX.")
            return
        }
        updateMessage = ""
        switch updateMethod {
        case .development:
            updateMessage = tr("A development build is updated with git pull and a rebuild.")
        case .homebrew(let brew):
            upgradeWithHomebrew(brew)
        case .diskImage(let target):
            installDiskImage(target: target, openInFinder: false)
        case .manual(let target):
            installDiskImage(target: target, openInFinder: true)
        }
    }

    // MARK: - Homebrew

    private func upgradeWithHomebrew(_ brew: String) {
        let wanted = updateInfo["latest"].text
        setUpdatePhase(tr("Updating Homebrew…"))
        Self.updateQueue.async {
            let env = [
                "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                "HOME": NSHomeDirectory(),
                "HOMEBREW_NO_ENV_HINTS": "1",
                // Homebrew 7 asks "[y/n]" before an upgrade, and nobody is at
                // a terminal to answer it: the press of "Update now" is the yes.
                "HOMEBREW_NO_ASK": "1",
                // And it quits a running cask app while upgrading it, which
                // here is DevX itself, before it could relaunch the new copy.
                "HOMEBREW_NO_UPGRADE_QUIT_CASKS": "1",
            ]
            // brew upgrade only auto-updates its taps once a day; the cask
            // for a release made an hour ago needs this.
            let fetched = Self.runTool(brew, ["update", "--quiet"], env: env)
            DispatchQueue.main.async { self.setUpdatePhase(cat(tr("Installing DevX"), " ", wanted, "…")) }
            let upgraded = Self.runTool(brew, ["upgrade", "--cask", "devx"], env: env)
            let installed = Self.bundleVersion("/Applications/DevX.app")
            DispatchQueue.main.async {
                self.setUpdatePhase(nil)
                if installed == wanted {
                    Self.relaunch("/Applications/DevX.app")
                } else if upgraded.status == 0 {
                    // The release workflow updates the cask in a pull request
                    // after the release itself, so brew can lag behind it.
                    self.updateMessage = cat(tr("Homebrew does not offer"), " ", wanted, " ",
                                             tr("yet: its cask is updated shortly after each release. Try again later."))
                } else {
                    self.updateMessage = cat(tr("Homebrew could not update DevX:"), "\n",
                                             Self.tail(fetched.output + upgraded.output))
                }
            }
        }
    }

    // MARK: - Disk image

    private func installDiskImage(target: String, openInFinder: Bool) {
        let info = updateInfo
        let version = info["latest"].text
        guard let dmgURL = URL(string: info["dmg_url"].text), dmgURL.scheme == "https",
              let shaURL = URL(string: info["sha256_url"].text), shaURL.scheme == "https" else {
            updateMessage = tr("This release has no disk image to install from. Open the release page instead.")
            return
        }
        setUpdatePhase(cat(tr("Downloading DevX"), " ", version, "…"))
        Self.updateQueue.async {
            do {
                let fm = FileManager.default
                let dir = fm.temporaryDirectory.appendingPathComponent("devx-update-" + version)
                try? fm.removeItem(at: dir)
                try fm.createDirectory(at: dir, withIntermediateDirectories: true)
                let dmg = dir.appendingPathComponent(info["dmg_name"].text)
                let shaText = String(decoding: try Self.download(shaURL), as: UTF8.self)
                try Self.download(dmgURL).write(to: dmg)
                guard let expected = UpdatePlan.publishedSHA256(shaText) else {
                    throw UpdateError(tr("The release's checksum file could not be read."))
                }
                guard try Self.sha256(of: dmg) == expected else {
                    throw UpdateError(tr("The downloaded disk image does not match the release's checksum. Nothing was changed."))
                }
                if openInFinder {
                    DispatchQueue.main.async {
                        self.setUpdatePhase(nil)
                        NSWorkspace.shared.open(dmg)
                        self.updateMessage = tr("This folder cannot be written, so the checked disk image is open in Finder: quit DevX and drag the new one to Applications.")
                    }
                    return
                }
                DispatchQueue.main.async { self.setUpdatePhase(tr("Checking the signature…")) }
                let mount = dir.appendingPathComponent("mnt")
                try fm.createDirectory(at: mount, withIntermediateDirectories: true)
                let attach = Self.runTool("/usr/bin/hdiutil",
                                          ["attach", "-nobrowse", "-readonly", "-noautoopen",
                                           "-mountpoint", mount.path, dmg.path])
                guard attach.status == 0 else {
                    throw UpdateError(cat(tr("The disk image could not be opened:"), " ", Self.tail(attach.output)))
                }
                defer { _ = Self.runTool("/usr/bin/hdiutil", ["detach", mount.path, "-force"]) }
                let app = mount.appendingPathComponent("DevX.app")
                try Self.verifySignature(app)
                guard Self.bundleVersion(app.path) == version else {
                    throw UpdateError(tr("The app in the disk image is not the version the release names. Nothing was changed."))
                }

                DispatchQueue.main.async { self.setUpdatePhase(tr("Installing…")) }
                let parent = (target as NSString).deletingLastPathComponent
                let staging = (parent as NSString).appendingPathComponent(".DevX-" + version + "-update.app")
                let previous = (parent as NSString).appendingPathComponent(".DevX-previous.app")
                try? fm.removeItem(atPath: staging)
                try? fm.removeItem(atPath: previous)
                let copy = Self.runTool("/usr/bin/ditto", [app.path, staging])
                guard copy.status == 0 else {
                    throw UpdateError(cat(tr("The new version could not be copied:"), " ", Self.tail(copy.output)))
                }
                // Swap, and put the old copy back if the new one cannot be
                // moved in: a failed update must leave a DevX that opens.
                try fm.moveItem(atPath: target, toPath: previous)
                do {
                    try fm.moveItem(atPath: staging, toPath: target)
                } catch {
                    try? fm.moveItem(atPath: previous, toPath: target)
                    throw error
                }
                try? fm.removeItem(atPath: previous)
                try? fm.removeItem(at: dir)
                DispatchQueue.main.async {
                    self.setUpdatePhase(nil)
                    Self.relaunch(target)
                }
            } catch {
                DispatchQueue.main.async {
                    self.setUpdatePhase(nil)
                    self.updateMessage = (error as? UpdateError)?.message ?? error.localizedDescription
                }
            }
        }
    }

    // MARK: - Helpers

    private func setUpdatePhase(_ phase: String?) {
        if let old = updatePhase { endBackground(old) }
        updatePhase = phase
        if let phase { beginBackground(phase) }
    }

    private struct UpdateError: Error {
        let message: String
        init(_ message: String) { self.message = message }
    }

    /// Runs a tool with an argv and no shell, both streams into one string.
    nonisolated private static func runTool(_ path: String, _ args: [String],
                                env: [String: String]? = nil) -> (status: Int32, output: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        if let env { p.environment = env }
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        // Nothing can answer a prompt: one reads end-of-file instead of waiting.
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return (-1, error.localizedDescription) }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    nonisolated private static func tail(_ text: String, lines: Int = 6) -> String {
        text.split(separator: "\n").suffix(lines).joined(separator: "\n")
    }

    nonisolated private static func bundleVersion(_ path: String) -> String? {
        Bundle(path: path)?.infoDictionary?["CFBundleShortVersionString"] as? String
    }

    nonisolated private static func download(_ url: URL) throws -> Data {
        let done = DispatchSemaphore(value: 0)
        var result: Result<Data, Error> = .failure(UpdateError(tr("The download did not finish.")))
        var request = URLRequest(url: url, timeoutInterval: 300)
        request.setValue("DevX", forHTTPHeaderField: "User-Agent")
        URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                result = .failure(error)
            } else if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                result = .failure(UpdateError(cat(tr("GitHub answered HTTP"), " ", String(http.statusCode))))
            } else if let data {
                result = .success(data)
            }
            done.signal()
        }.resume()
        done.wait()
        return try result.get()
    }

    nonisolated private static func sha256(of file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// The app must be signed by the release team's Developer ID, intact, with
    /// everything nested in it -- the `mpi` beside it included.
    nonisolated private static func verifySignature(_ app: URL) throws {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &code) == errSecSuccess, let code else {
            throw UpdateError(tr("The new app's signature could not be read. Nothing was changed."))
        }
        var requirement: SecRequirement?
        guard SecRequirementCreateWithString(UpdatePlan.signingRequirement as CFString, [], &requirement)
                == errSecSuccess else {
            throw UpdateError(tr("The new app's signature could not be read. Nothing was changed."))
        }
        let flags = SecCSFlags(rawValue: kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode)
        guard SecStaticCodeCheckValidity(code, flags, requirement) == errSecSuccess else {
            throw UpdateError(tr("The new app is not signed by DevX's Developer ID. Nothing was changed."))
        }
    }

    private static func relaunch(_ path: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        p.arguments = ["-n", path]
        try? p.run()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NSApp.terminate(nil) }
    }
}
