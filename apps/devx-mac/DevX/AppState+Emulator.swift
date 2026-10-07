import Foundation

/// The Android emulator module: SDK, catalog, installs, AVDs, starting and
/// stopping, and the embedded display. Every call goes through the C ABI and
/// runs off the main thread; the results land back on it.
extension AppState {
    private static let emulatorQueue = DispatchQueue(label: "devx.emulator", qos: .userInitiated)

    private func emulatorRun(_ label: String, _ work: @escaping @Sendable () -> JSON,
                             then apply: @escaping (JSON) -> Void) {
        beginOperation(label)
        Self.emulatorQueue.async {
            let doc = work()
            DispatchQueue.main.async {
                self.endOperation(label)
                if doc["ok"].bool == false || !doc["error"].text.isEmpty {
                    self.lastError = doc["error"].text.isEmpty ? nil : doc["error"].text
                }
                apply(doc)
            }
        }
    }

    var avds: [JSON] { androidSdkDoc["avds"].array }
    var selectedAvd: JSON {
        avds.first(where: { $0["id"].text == emulatorSelected }) ?? .null
    }
    var installedSystemImages: [String] {
        androidSdkDoc["installed"].array.map { $0["path"].text }
            .filter { $0.hasPrefix("system-images;") }
    }

    func loadAndroidSdk() {
        emulatorRun(tr("Reading the Android SDK…"), { Core.androidSdk() }) { doc in
            self.androidSdkDoc = doc
            if self.emulatorSelected.isEmpty, let first = doc["avds"].array.first {
                self.emulatorSelected = first["id"].text
            }
        }
        if androidPresets.isEmpty {
            androidPresets = Core.androidPresets().array
        }
    }

    func loadAndroidCatalog() {
        emulatorRun(tr("Fetching Google's SDK catalog…"), { Core.androidCatalog() }) {
            self.androidCatalogDoc = $0
        }
    }

    func acceptLicense(_ id: String) {
        emulatorRun(tr("Recording the license…"), { Core.androidAcceptLicense(id) }) { _ in
            self.loadAndroidCatalog()
        }
    }

    func installPackage(_ path: String) {
        let doc = Core.androidInstallStart(path)
        if doc["ok"].bool != true {
            lastError = doc["error"].text
            return
        }
        installDoc = Core.androidInstallPoll()
        installTimer?.invalidate()
        installTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] t in
            Task { @MainActor in
                guard let self else { t.invalidate(); return }
                self.installDoc = Core.androidInstallPoll()
                if self.installDoc["finished"].bool == true {
                    t.invalidate()
                    if self.installDoc["ok"].bool != true {
                        self.lastError = self.installDoc["error"].text
                    }
                    self.loadAndroidSdk()
                    self.loadAndroidCatalog()
                }
            }
        }
    }

    func cancelInstall() { Core.androidInstallCancel() }

    func createAvd(name: String, image: String, preset: String, width: Int, height: Int,
                   density: Int, ramMb: Int) {
        emulatorRun(tr("Creating the virtual device…"), {
            Core.avdCreate(name: name, image: image, preset: preset, width: width,
                           height: height, density: density, ramMb: ramMb)
        }) { doc in
            if doc["ok"].bool == true {
                self.emulatorSelected = doc["avd"]["id"].text
                self.loadAndroidSdk()
            }
        }
    }

    func deleteAvd(_ id: String) {
        emulatorRun(tr("Deleting…"), { Core.avdDelete(id) }) { _ in
            if self.emulatorSelected == id { self.emulatorSelected = "" }
            self.loadAndroidSdk()
        }
    }

    func startEmulator(_ id: String, cold: Bool = false) {
        Core.resetCancel()
        emulatorRun(tr("Starting the emulator and waiting for Android to boot…"), {
            Core.emulatorStart(id, cold: cold)
        }) { doc in
            self.emulatorNote = doc["notes"].array.compactMap { $0.string }.first
            self.loadAndroidSdk()
            if doc["ok"].bool == true {
                self.openDisplay(id)
                self.loadDevices()  // it is an adb device now: Live, Inspect, Layout reach it
            }
        }
    }

    func stopEmulator(_ id: String) {
        hideDisplay(id)
        emulatorRun(tr("Stopping the emulator…"), { Core.emulatorStop(id) }) { _ in
            self.loadAndroidSdk()
            self.loadDevices()
        }
    }

    func display(for id: String) -> JSON? {
        emulatorDisplays.first { $0["avd_id"].text == id }
    }

    /// Shows a running device's screen beside any already shown. A device
    /// already on screen is reconnected in place. The frame file is a square
    /// so either orientation fits; 1600 keeps a tablet sharp at 10 MB.
    func openDisplay(_ id: String) {
        emulatorRun(tr("Connecting to the screen…"), { Core.displayOpen(id, box: 1600) }) { doc in
            guard doc["ok"].bool == true else { return }
            if let i = self.emulatorDisplays.firstIndex(where: { $0["avd_id"].text == id }) {
                self.emulatorDisplays[i] = doc
            } else {
                self.emulatorDisplays.append(doc)
            }
            self.emulatorRotations[id] = doc["status"]["rotation"].int ?? 0
        }
    }

    /// Takes a device off screen; it keeps running.
    func hideDisplay(_ id: String) {
        guard let d = display(for: id) else { return }
        Core.displayClose(d["handle"].int ?? 0)
        emulatorDisplays.removeAll { $0["avd_id"].text == id }
    }

    /// A device's stream ended under its view. Reconnect when the emulator is
    /// still (or again) running; otherwise say what happened.
    func displayLost(_ id: String) {
        guard display(for: id) != nil else { return }
        emulatorDisplays.removeAll { $0["avd_id"].text == id }
        emulatorNote = id + ": " + tr("disconnected from the emulator: it stopped or restarted.")
        Self.emulatorQueue.asyncAfter(deadline: .now() + 1.5) {
            let sdk = Core.androidSdk()
            DispatchQueue.main.async {
                self.androidSdkDoc = sdk
                let running = sdk["avds"].array.contains {
                    $0["id"].text == id && $0["running"]["attachable"].bool == true
                }
                if running && self.display(for: id) == nil {
                    self.emulatorNote = id + ": " + tr("reconnected after the emulator restarted.")
                    self.openDisplay(id)
                }
            }
        }
    }

    func rotateEmulator(_ id: String, by delta: Int) {
        guard let d = display(for: id) else { return }
        let handle = d["handle"].int ?? 0
        let next = (((emulatorRotations[id] ?? 0) + delta) % 360 + 360) % 360
        emulatorRun(tr("Rotating…"), { Core.rotate(handle, next) }) { doc in
            if doc["ok"].bool == true { self.emulatorRotations[id] = next }
        }
    }

    func showExtendedControls(_ id: String, pane: Int = 0) {
        guard let handle = display(for: id)?["handle"].int else { return }
        emulatorRun(tr("Opening Extended Controls…"), { Core.extendedControls(handle, pane: pane) }) { _ in }
    }

    func emulatorScreenshot(_ id: String) {
        guard let handle = display(for: id)?["handle"].int else { return }
        let dir = sessionsDir
        emulatorRun(tr("Taking a screenshot…"), { Core.emulatorScreenshot(handle, sessionsDir: dir) }) { doc in
            if doc["ok"].bool == true {
                self.emulatorNote = tr("Saved") + " " + doc["path"].text
                    + (doc["observation"].text.isEmpty ? "" : " · read_observation "
                       + doc["observation"].text)
            }
        }
    }

    /// `override` changes a running device without a restart (wm size); the
    /// hardware size takes effect at the next, cold, boot.
    func resizeEmulator(_ id: String, width: Int, height: Int, density: Int, override: Bool) {
        emulatorRun(tr("Changing the screen size…"), {
            Core.avdResize(id, width: width, height: height, density: density, override: override)
        }) { doc in
            if doc["ok"].bool == true {
                self.emulatorNote = doc["note"].text
                self.loadAndroidSdk()
            }
        }
    }
}

extension JSON {
    /// The members of an object, or none.
    var objectValue: [String: JSON] {
        if case .object(let o) = self { return o }
        return [:]
    }
}
