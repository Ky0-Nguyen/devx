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
    @State private var localStatus: JSON = .null
    @State private var localState = ""
    @State private var message = ""
    @State private var working = false
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
                    deviceList
                    automate
                }
                if !message.isEmpty {
                    Text(message).font(Term.small).foregroundStyle(Term.dim).textSelection(.enabled)
                }
            }
            .padding(16)
        }
        .onAppear { refreshStatus() }
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
                }
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
        act { Core.bsUpload(product: "app-live", file: path) } then: { d in
            appURL = d["body"]["app_url"].text
            if appURL.isEmpty { message = d["error"].text.isEmpty ? d["body"].serialized() : d["error"].text }
        }
    }

    private func openLive(_ d: JSON) {
        let u = Core.bsLiveURL(os: d["os"].text, version: d["os_version"].text,
                               device: d["device"].text, appURL: appURL)
        if let url = URL(string: u["url"].text) { NSWorkspace.shared.open(url) }
    }
}
