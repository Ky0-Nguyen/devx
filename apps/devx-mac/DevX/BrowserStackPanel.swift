import AppKit
import Security
import SwiftUI

/// BrowserStack credentials in the macOS Keychain (service
/// "com.devx.browserstack"), where `mpi` and `mpi mcp` read them too. Written
/// through the Security framework, never to a file or a command line.
enum BrowserStackKeychain {
    static let service = "com.devx.browserstack"

    static func save(username: String, key: String) -> Bool {
        remove()
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: username,
            kSecValueData as String: Data(key.utf8),
        ]
        return SecItemAdd(item as CFDictionary, nil) == errSecSuccess
    }

    static func remove() {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service]
        SecItemDelete(q as CFDictionary)
    }
}

/// Real devices in BrowserStack's cloud, beside the emulators: check the
/// account, list devices, upload an app, open App Live in the browser, run the
/// BrowserStack Local tunnel, and keep App Automate sessions for AI tools.
struct BrowserStackPanel: View {
    @EnvironmentObject var state: AppState
    @State private var username = ""
    @State private var accessKey = ""
    @State private var status: JSON = .null
    @State private var devices: [JSON] = []
    @State private var filter = ""
    @State private var appURL = ""
    @State private var builds: [JSON] = []
    @State private var buildSessions: [String: [JSON]] = [:]
    @State private var localStatus: JSON = .null
    @State private var localState = ""
    @State private var message = ""
    @State private var working = false
    // A real-device session DevX drives (App Automate).
    @State private var automateAppURL = ""
    @State private var network = ""
    @State private var gps = ""
    @State private var timezone = ""
    @State private var language = ""
    @State private var locale = ""
    @State private var orientation = ""
    @State private var biometric = false
    @State private var cameraInjection = false
    @State private var profiling = true
    @State private var throughLocal = false
    @State private var live: JSON = .null
    @State private var liveImage: NSImage?
    @State private var livePixels = CGSize.zero
    @State private var liveText = ""
    @State private var shotInFlight = false
    private let shotTimer = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()
    private static let networkProfiles = [
        "", "2g-gprs-good", "edge-good", "3g-umts-good", "3g-umts-lossy", "4g-lte-good",
        "4g-lte-high-latency", "4g-lte-lossy", "no-network",
    ]
    private let localId = "devx-" + String(ProcessInfo.processInfo.processIdentifier)

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(tr("Real devices in BrowserStack's cloud, for what an emulator cannot show. "
                        + "Needs a BrowserStack account; minutes are billed by BrowserStack."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                account
                if status["ok"].bool == true {
                    tunnel
                    if !live.isNull { liveSession }
                    deviceList
                    if !automateAppURL.isEmpty { sessionSettings }
                    automate
                }
                if !message.isEmpty {
                    Text(message).font(Term.small).foregroundStyle(Term.dim).textSelection(.enabled)
                }
            }
            .padding(16)
        }
        .onAppear { refreshStatus() }
        .onReceive(shotTimer) { _ in refreshScreen() }
    }

    // ---- account ----

    private var account: some View {
        Panel(title: tr("Account")) {
            VStack(alignment: .leading, spacing: 8) {
                if status["credentials"].bool == true {
                    HStack {
                        Chip(text: status["ok"].bool == true ? "connected" : "refused",
                             tone: status["ok"].bool == true ? .good : .bad)
                        Text(status["username"].text + " · " + status["source"].text)
                            .font(Term.body).foregroundStyle(Term.ink)
                        Spacer()
                        Button(tr("remove")) { BrowserStackKeychain.remove(); refreshStatus() }
                            .buttonStyle(TermButtonStyle(tone: Term.red))
                    }
                    if status["ok"].bool != true {
                        Text(status["error"].text).font(Term.small).foregroundStyle(Term.red)
                    }
                } else {
                    HStack {
                        TextField(tr("username"), text: $username).textFieldStyle(TermFieldStyle())
                        SecureField(tr("access key"), text: $accessKey).textFieldStyle(TermFieldStyle())
                        Button(tr("save to Keychain")) {
                            _ = BrowserStackKeychain.save(username: username, key: accessKey)
                            accessKey = ""
                            refreshStatus()
                        }
                        .buttonStyle(TermButtonStyle(filled: true))
                        .disabled(username.isEmpty || accessKey.isEmpty)
                    }
                    Text(tr("From BrowserStack's Account > Settings. Stored in the macOS Keychain; "
                            + "BROWSERSTACK_USERNAME / BROWSERSTACK_ACCESS_KEY take precedence when set."))
                        .font(Term.small).foregroundStyle(Term.dim)
                }
            }
        }
    }

    // ---- BrowserStack Local ----

    private var tunnel: some View {
        Panel(title: "BrowserStack Local") {
            VStack(alignment: .leading, spacing: 6) {
                Text(tr("A tunnel so BrowserStack's devices reach this Mac: localhost, a staging "
                        + "server, or Metro. On the device this Mac is bs-local.com, so point a "
                        + "React Native debug build at bs-local.com:8081 -- and the Inspect tab "
                        + "then reads it through Metro like any other device."))
                    .font(Term.small).foregroundStyle(Term.dim).fixedSize(horizontal: false, vertical: true)
                HStack {
                    if localStatus["installed"].bool != true {
                        Button(tr("download (10 MB)")) { act { Core.bsLocalInstall() } then: { _ in refreshLocal() } }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(localStatus["can_run"].bool == false)
                    } else if localState == "connected" {
                        Chip(text: "connected · " + localId, tone: .good)
                        Button(tr("stop")) { act { Core.bsLocalStop(localId) } then: { _ in localState = "" } }
                            .buttonStyle(TermButtonStyle(tone: Term.amber))
                    } else {
                        Button(tr("start tunnel")) {
                            act { Core.bsLocalStart(localId) } then: { d in localState = d["state"].text }
                        }
                        .buttonStyle(TermButtonStyle(filled: true))
                    }
                }
                if localStatus["can_run"].bool == false {
                    Text(tr("BrowserStack ships this tool for Intel only; this Mac needs Rosetta 2 to "
                            + "run it (softwareupdate --install-rosetta). DevX does not install it."))
                        .font(Term.small).foregroundStyle(Term.amber)
                }
                Text(tr("BrowserStack's binary takes the access key on its command line, so while the "
                        + "tunnel runs other processes on this Mac can see it."))
                    .font(Term.small).foregroundStyle(Term.dim)
            }
        }
    }

    // ---- devices, upload, App Live ----

    private var deviceList: some View {
        Panel(title: tr("Devices") + " (\(devices.count))") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Button(tr("load devices")) {
                        act { Core.bsGet("app-automate/devices.json") } then: { d in devices = d["body"].array }
                    }
                    .buttonStyle(TermButtonStyle(filled: true))
                    Button(tr("upload app…")) { upload() }.buttonStyle(TermButtonStyle())
                    if !appURL.isEmpty { Chip(text: appURL, tone: .good) }
                    TextField(tr("filter"), text: $filter).textFieldStyle(TermFieldStyle()).frame(maxWidth: 200)
                }
                ForEach(Array(shownDevices.prefix(80).enumerated()), id: \.offset) { _, d in
                    HStack {
                        Text(d["device"].text).font(Term.body).foregroundStyle(Term.ink)
                        Text(d["os"].text + " " + d["os_version"].text).font(Term.small).foregroundStyle(Term.dim)
                        Spacer()
                        Button(tr("start session")) { startSession(d) }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(automateAppURL.isEmpty || !live.isNull)
                        Button(tr("open in App Live")) { openLive(d) }
                            .buttonStyle(TermButtonStyle())
                            .disabled(appURL.isEmpty)
                    }
                }
            }
        }
    }

    private var shownDevices: [JSON] {
        guard !filter.isEmpty else { return devices }
        return devices.filter { ($0["device"].text + " " + $0["os"].text + " " + $0["os_version"].text)
            .localizedCaseInsensitiveContains(filter) }
    }

    private var automate: some View {
        Panel(title: tr("App Automate sessions")) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Button(tr("load recent builds")) {
                        act { Core.bsGet("app-automate/builds.json?limit=10") } then: { d in builds = d["body"].array }
                    }
                    .buttonStyle(TermButtonStyle())
                }
                ForEach(Array(builds.enumerated()), id: \.offset) { _, b in
                    let build = b["automation_build"].isNull ? b : b["automation_build"]
                    HStack {
                        Text(build["name"].text).font(Term.body).foregroundStyle(Term.ink).lineLimit(1)
                        Text(build["status"].text).font(Term.small).foregroundStyle(Term.dim)
                        Spacer()
                        Button(tr("sessions")) {
                            let id = build["hashed_id"].text
                            act { Core.bsGet("app-automate/builds/\(id)/sessions.json") } then: { d in
                                buildSessions[id] = d["body"].array.map {
                                    $0["automation_session"].isNull ? $0 : $0["automation_session"]
                                }
                            }
                        }
                        .buttonStyle(TermButtonStyle())
                        Button(tr("keep for AI tools")) {
                            let id = build["hashed_id"].text
                            act { Core.bsGet("app-automate/builds/\(id)/sessions.json") } then: { d in
                                let saved = Core.bsSave(sessionsDir: state.sessionsDir,
                                                        what: "app-automate build " + id,
                                                        json: d["body"].serialized())
                                message = saved["ok"].bool == true
                                    ? tr("Saved as observation") + " " + saved["id"].text : saved["error"].text
                            }
                        }
                        .buttonStyle(TermButtonStyle())
                    }
                    ForEach(Array((buildSessions[build["hashed_id"].text] ?? []).enumerated()),
                            id: \.offset) { _, s in
                        sessionRow(build: build["hashed_id"].text, session: s)
                    }
                }
            }
        }
    }

    /// One App Automate session, and bringing BrowserStack's App Profiling of
    /// it into DevX as a session (a paid BrowserStack plan profiles).
    private func sessionRow(build: String, session s: JSON) -> some View {
        HStack {
            Text("  " + s["device"].text + " · " + s["os"].text + " " + s["os_version"].text)
                .font(Term.small).foregroundStyle(Term.ink).lineLimit(1)
            Text(s["status"].text).font(Term.small).foregroundStyle(Term.dim)
            Spacer()
            Button(tr("import profiling")) {
                let dir = state.sessionsDir
                let id = s["hashed_id"].text
                act { Core.bsImportProfiling(sessionsDir: dir, buildID: build, sessionID: id) } then: { d in
                    guard d["ok"].bool == true else { return }
                    message = tr("Imported as session") + " " + d["session_id"].text
                    state.loadSessions()
                    state.openSession(d["session_id"].text, revealIn: .timeline)
                }
            }
            .buttonStyle(TermButtonStyle())
        }
    }

    // ---- a real-device session DevX drives ----

    private var sessionSettings: some View {
        Panel(title: tr("Session settings")) {
            VStack(alignment: .leading, spacing: 6) {
                Text(tr("BrowserStack's settings for the next session started from a device above. "
                        + "Empty means BrowserStack's default."))
                    .font(Term.small).foregroundStyle(Term.dim)
                HStack {
                    Picker(tr("network"), selection: $network) {
                        ForEach(Self.networkProfiles, id: \.self) { Text($0.isEmpty ? tr("default") : $0).tag($0) }
                    }
                    .frame(maxWidth: 260)
                    Picker(tr("orientation"), selection: $orientation) {
                        Text(tr("default")).tag("")
                        Text("portrait").tag("portrait")
                        Text("landscape").tag("landscape")
                    }
                    .frame(maxWidth: 220)
                }
                HStack {
                    TextField(tr("GPS lat,lng"), text: $gps).textFieldStyle(TermFieldStyle())
                    TextField(tr("timezone"), text: $timezone).textFieldStyle(TermFieldStyle())
                    TextField(tr("language"), text: $language).textFieldStyle(TermFieldStyle())
                    TextField(tr("locale"), text: $locale).textFieldStyle(TermFieldStyle())
                }
                HStack(spacing: 14) {
                    Toggle(tr("App Profiling"), isOn: $profiling)
                    Toggle(tr("biometrics"), isOn: $biometric)
                    Toggle(tr("camera injection"), isOn: $cameraInjection)
                    Toggle(tr("through BrowserStack Local"), isOn: $throughLocal)
                        .disabled(localState != "connected")
                }
                .font(Term.small)
            }
        }
    }

    private var liveSession: some View {
        Panel(title: tr("Real-device session") + " · " + live["device"].text + " "
                     + live["os_version"].text) {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .top, spacing: 12) {
                    screen
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Button(tr("Back")) { send(["key": .string("back")]) }
                            Button(tr("Home")) { send(["key": .string("home")]) }
                            Button(tr("Enter")) { send(["key": .string("enter")]) }
                        }
                        .buttonStyle(TermButtonStyle())
                        HStack {
                            TextField(tr("text to type"), text: $liveText).textFieldStyle(TermFieldStyle())
                            Button(tr("type")) {
                                send(["text": .string(liveText)])
                                liveText = ""
                            }
                            .buttonStyle(TermButtonStyle())
                            .disabled(liveText.isEmpty)
                        }
                        Text(tr("Click to tap, drag to swipe. The screen is a screenshot every 1.5 s, "
                                + "not a video stream; BrowserStack bills the minutes until you stop."))
                            .font(Term.small).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                        Text(live["session_id"].text).font(Term.small).foregroundStyle(Term.dim)
                            .textSelection(.enabled)
                        Button(live["profiling"].bool == true ? tr("stop and import profiling") : tr("stop")) {
                            stopSession()
                        }
                        .buttonStyle(TermButtonStyle(tone: Term.amber))
                    }
                }
            }
        }
    }

    /// The device's screen at a fixed width; clicks and drags are mapped back
    /// to the screenshot's pixels, which is what the session's input takes.
    private var screen: some View {
        let width: CGFloat = 300
        let height = livePixels.width > 0 ? width * livePixels.height / livePixels.width : width * 2
        return Group {
            if let img = liveImage {
                Image(nsImage: img).resizable().frame(width: width, height: height)
            } else {
                Rectangle().fill(Term.dim.opacity(0.15)).frame(width: width, height: height)
                    .overlay(Text(tr("waiting for the first screenshot…")).font(Term.small))
            }
        }
        .gesture(DragGesture(minimumDistance: 0).onEnded { g in
            guard livePixels.width > 0 else { return }
            let k = livePixels.width / width
            let a = g.startLocation, b = g.location
            if hypot(b.x - a.x, b.y - a.y) < 6 {
                send(["tap": .array([.number(Double(a.x * k)), .number(Double(a.y * k))])])
            } else {
                send(["swipe": .array([.number(Double(a.x * k)), .number(Double(a.y * k)),
                                       .number(Double(b.x * k)), .number(Double(b.y * k))])])
            }
        })
    }

    private func startSession(_ d: JSON) {
        var spec: [String: JSON] = [
            "app_url": .string(automateAppURL),
            "platform": .string(d["os"].text == "ios" ? "ios" : "android"),
            "device": .string(d["device"].text),
            "os_version": .string(d["os_version"].text),
            "name": .string("DevX " + d["device"].text),
            "app_profiling": .bool(profiling),
            "biometric": .bool(biometric),
            "camera_injection": .bool(cameraInjection),
        ]
        for (k, v) in [("network_profile", network), ("gps_location", gps), ("timezone", timezone),
                       ("language", language), ("locale", locale), ("orientation", orientation)]
            where !v.isEmpty {
            spec[k] = .string(v)
        }
        if throughLocal {
            spec["local"] = .bool(true)
            spec["local_identifier"] = .string(localId)
        }
        message = tr("Starting a session: BrowserStack picks the device, installs the app and launches it…")
        let s = JSON.object(spec)
        act { Core.bsAutomateStart(s) } then: { r in
            guard r["ok"].bool == true else { return }
            message = ""
            live = .object(["session_id": r["session_id"], "device": d["device"],
                            "os_version": d["os_version"], "profiling": .bool(profiling)])
            liveImage = nil
            refreshScreen()
        }
    }

    private func refreshScreen() {
        let sid = live["session_id"].text
        guard !sid.isEmpty, !shotInFlight else { return }
        shotInFlight = true
        let path = (NSTemporaryDirectory() as NSString).appendingPathComponent("devx-bs-\(sid).png")
        DispatchQueue.global(qos: .userInitiated).async {
            let r = Core.bsAutomateScreenshot(sid, to: path)
            let img = r["ok"].bool == true ? (try? Data(contentsOf: URL(fileURLWithPath: path))).flatMap(NSImage.init(data:)) : nil
            DispatchQueue.main.async {
                shotInFlight = false
                guard live["session_id"].text == sid else { return }
                if let img {
                    liveImage = img
                    livePixels = CGSize(width: r["width"].int ?? 0, height: r["height"].int ?? 0)
                } else if !r["error"].text.isEmpty {
                    message = r["error"].text
                }
            }
        }
    }

    private func send(_ action: [String: JSON]) {
        let sid = live["session_id"].text
        let a = JSON.object(action)
        DispatchQueue.global(qos: .userInitiated).async {
            let r = Core.bsAutomateInput(sid, a)
            DispatchQueue.main.async {
                if r["ok"].bool != true { message = r["error"].text }
                refreshScreen()
            }
        }
    }

    private func stopSession() {
        let sid = live["session_id"].text
        let importTo = live["profiling"].bool == true ? state.sessionsDir : ""
        live = .null
        liveImage = nil
        message = importTo.isEmpty ? tr("Stopping…")
            : tr("Stopping, then waiting for BrowserStack to publish the profiling (up to 90 s)…")
        act { Core.bsAutomateStop(sid, importTo: importTo) } then: { r in
            let p = r["profiling"]
            if p["ok"].bool == true {
                message = tr("Imported as session") + " " + p["session_id"].text
                state.loadSessions()
                state.openSession(p["session_id"].text, revealIn: .timeline)
            } else if !p["error"].text.isEmpty {
                message = tr("Stopped. The profiling was not imported:") + " " + p["error"].text
            } else if r["ok"].bool == true {
                message = tr("Stopped.")
            }
        }
    }

    // ---- actions ----

    private func act(_ work: @escaping @Sendable () -> JSON, then apply: @escaping (JSON) -> Void) {
        working = true
        DispatchQueue.global(qos: .userInitiated).async {
            let d = work()
            DispatchQueue.main.async {
                working = false
                if d["ok"].bool == false { message = d["error"].text }
                apply(d)
            }
        }
    }

    private func refreshStatus() {
        act { Core.bsStatus() } then: { d in status = d }
        refreshLocal()
    }

    private func refreshLocal() {
        act { Core.bsLocalStatus() } then: { d in localStatus = d }
    }

    private func upload() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = []
        panel.allowsOtherFileTypes = true
        panel.message = tr("Choose an .apk, .aab or .ipa to upload to BrowserStack")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let path = url.path
        // App Live and App Automate keep separate uploads; one file serves both.
        act { Core.bsUpload(product: "app-live", file: path) } then: { d in
            appURL = d["body"]["app_url"].text
            if appURL.isEmpty { message = d["error"].text.isEmpty ? d["body"].serialized() : d["error"].text }
        }
        act { Core.bsUpload(product: "app-automate", file: path) } then: { d in
            automateAppURL = d["body"]["app_url"].text
        }
    }

    private func openLive(_ d: JSON) {
        let u = Core.bsLiveURL(os: d["os"].text, version: d["os_version"].text,
                               device: d["device"].text, appURL: appURL)
        if let url = URL(string: u["url"].text) { NSWorkspace.shared.open(url) }
    }
}
