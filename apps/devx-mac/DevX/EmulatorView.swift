import SwiftUI

/// Android emulators inside DevX, with no Android Studio: install system
/// images from Google's catalog, make virtual devices from standard screen
/// sizes, run them with their screen drawn here, and -- from the same tab --
/// reach real devices on BrowserStack.
struct EmulatorView: View {
    @EnvironmentObject var state: AppState
    @State private var section = 0
    @State private var showNew = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                Picker("", selection: $section) {
                    Text(tr("Virtual devices")).tag(0)
                    Text(tr("Install images")).tag(1)
                    Text("BrowserStack").tag(2)
                }
                .pickerStyle(.segmented).frame(maxWidth: 460)
                Spacer()
                Text(state.androidSdkDoc["root"].text)
                    .font(Term.small).foregroundStyle(Term.dim).lineLimit(1).truncationMode(.middle)
            }
            .padding(.horizontal, 16).padding(.vertical, 10)

            switch section {
            case 1: InstallImagesPanel()
            case 2: BrowserStackPanel()
            default: devices
            }
        }
        .navigationTitle("~/emulator")
        .onAppear { state.loadAndroidSdk() }
        .sheet(isPresented: $showNew) { NewDeviceSheet(isPresented: $showNew) }
    }

    private var devices: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(tr("Devices")).font(Term.font(12, .bold)).foregroundStyle(Term.ink)
                    Spacer()
                    Button(tr("new")) { showNew = true }
                        .buttonStyle(TermButtonStyle(filled: true))
                        .disabled(state.installedSystemImages.isEmpty)
                    Button(tr("refresh")) { state.loadAndroidSdk() }
                        .buttonStyle(TermButtonStyle())
                }
                if state.androidSdkDoc["emulator_installed"].bool == false {
                    Banner(kind: .caution, title: "Emulator not installed",
                           message: tr("Install the Android Emulator and a system image from "
                                       + "Install images. Nothing else is needed: no Android "
                                       + "Studio, no Java."))
                }
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(Array(state.avds.enumerated()), id: \.offset) { _, a in
                            avdRow(a)
                        }
                        if state.avds.isEmpty {
                            Text(tr("No virtual devices yet."))
                                .font(Term.body).foregroundStyle(Term.dim)
                        }
                    }
                }
            }
            .padding(12)
            .frame(minWidth: 260, idealWidth: 300, maxWidth: 380)

            display.frame(minWidth: 420)
        }
    }

    private func avdRow(_ a: JSON) -> some View {
        let id = a["id"].text
        let running = !a["running"].isNull
        let selected = state.emulatorSelected == id
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Circle().fill(running ? Term.green : Term.dim.opacity(0.4)).frame(width: 7, height: 7)
                Text(a["display_name"].text).font(Term.font(12, .bold)).foregroundStyle(Term.ink)
                    .lineLimit(1)
                Spacer()
            }
            Text("\(a["width"].int ?? 0)×\(a["height"].int ?? 0) · \(a["density"].int ?? 0) dpi · "
                 + "\(a["width_dp"].int ?? 0)×\(a["height_dp"].int ?? 0) dp · "
                 + a["window_class"].text)
                .font(Term.small).foregroundStyle(Term.dim)
            Text("API \(a["api_level"].int ?? 0) · \(a["tag"].text)"
                 + (running ? " · " + a["running"]["serial"].text : ""))
                .font(Term.small).foregroundStyle(Term.dim)
            HStack(spacing: 6) {
                if running {
                    if state.display(for: id) == nil {
                        Button(tr("show")) { state.emulatorSelected = id; state.openDisplay(id) }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(a["running"]["attachable"].bool != true)
                    } else {
                        Button(tr("hide")) { state.hideDisplay(id) }
                            .buttonStyle(TermButtonStyle())
                    }
                    Button(tr("stop")) { state.stopEmulator(id) }
                        .buttonStyle(TermButtonStyle(tone: Term.amber))
                } else {
                    Button(tr("start")) { state.emulatorSelected = id; state.startEmulator(id) }
                        .buttonStyle(TermButtonStyle(filled: true))
                    Button(tr("cold boot")) { state.emulatorSelected = id; state.startEmulator(id, cold: true) }
                        .buttonStyle(TermButtonStyle())
                    Button(tr("delete")) { state.deleteAvd(id) }
                        .buttonStyle(TermButtonStyle(tone: Term.red))
                }
            }
            if running && a["running"]["attachable"].bool != true {
                Text(a["running"]["note"].text).font(Term.small).foregroundStyle(Term.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(8)
        .background(selected ? Term.raised : Term.panel, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(selected ? Term.green.opacity(0.5) : Term.line))
        .contentShape(Rectangle())
        .onTapGesture { state.emulatorSelected = id }
    }

    /// Every device on screen, side by side; the window scrolls sideways when
    /// they do not fit.
    @ViewBuilder private var display: some View {
        if state.emulatorDisplays.isEmpty {
            VStack(spacing: 10) {
                Text(tr("Start a virtual device, or press show on a running one, to see its screen "
                        + "here. Several can be shown side by side."))
                    .font(Term.body).foregroundStyle(Term.dim).multilineTextAlignment(.center)
                if let note = state.emulatorNote {
                    Text(note).font(Term.small).foregroundStyle(Term.dim)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity).padding(20)
        } else {
            VStack(spacing: 0) {
                ScrollView(.horizontal) {
                    HStack(alignment: .top, spacing: 10) {
                        ForEach(Array(state.emulatorDisplays.enumerated()), id: \.offset) { _, d in
                            screenColumn(d)
                        }
                    }
                    .padding(10)
                }
                if let note = state.emulatorNote {
                    Text(note).font(Term.small).foregroundStyle(Term.dim).lineLimit(2)
                        .padding(.horizontal, 10).padding(.bottom, 6)
                        .textSelection(.enabled)
                }
            }
        }
    }

    /// One device: its toolbar over its screen. With one device on screen it
    /// takes the whole width; with several each gets a phone-sized column.
    private func screenColumn(_ d: JSON) -> some View {
        let id = d["avd_id"].text
        let hw = d["status"]["hardware"]
        let several = state.emulatorDisplays.count > 1
        return VStack(spacing: 0) {
            toolbar(d)
            EmulatorScreen(handle: d["handle"].int ?? 0, path: d["path"].text,
                           box: d["box"].int ?? 1600,
                           deviceWidth: Int(hw["hw.lcd.width"].text) ?? 0,
                           deviceHeight: Int(hw["hw.lcd.height"].text) ?? 0,
                           onLost: { state.displayLost(id) })
                .padding(6)
        }
        .frame(minWidth: several ? 300 : 420, idealWidth: several ? 380 : 700,
               maxWidth: several ? 460 : .infinity, minHeight: 500, maxHeight: .infinity)
        .background(Term.bg)
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Term.line))
    }

    private func toolbar(_ d: JSON) -> some View {
        let id = d["avd_id"].text
        let handle = d["handle"].int ?? 0
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text(id).font(Term.font(12, .bold)).foregroundStyle(Term.ink).lineLimit(1)
                Text(d["serial"].text).font(Term.small).foregroundStyle(Term.dim)
                Spacer()
                iconButton("xmark", tr("Take off screen (it keeps running)")) { state.hideDisplay(id) }
            }
            HStack(spacing: 4) {
                iconButton("chevron.backward", tr("Back")) { Core.key(handle, "GoBack") }
                iconButton("circle", tr("Home")) { Core.key(handle, "GoHome") }
                iconButton("square.on.square", tr("Recents")) { Core.key(handle, "AppSwitch") }
                Divider().frame(height: 14)
                iconButton("rotate.left", tr("Rotate left")) { state.rotateEmulator(id, by: -90) }
                iconButton("rotate.right", tr("Rotate right")) { state.rotateEmulator(id, by: 90) }
                iconButton("speaker.minus", tr("Volume down")) { Core.key(handle, "AudioVolumeDown") }
                iconButton("speaker.plus", tr("Volume up")) { Core.key(handle, "AudioVolumeUp") }
                iconButton("power", tr("Power")) { Core.key(handle, "Power") }
                Divider().frame(height: 14)
                sizeMenu(id)
                iconButton("camera", tr("Screenshot")) { state.emulatorScreenshot(id) }
                iconButton("ellipsis.circle", tr("Extended controls: location, battery, camera, phone, fingerprint, sensors, snapshots")) {
                    state.showExtendedControls(id)
                }
            }
        }
        .padding(.horizontal, 8).padding(.vertical, 6)
        .background(Term.panel)
    }

    private func sizeMenu(_ id: String) -> some View {
        Menu {
            Section(tr("Override now (no restart)")) {
                ForEach(Array(state.androidPresets.enumerated()), id: \.offset) { _, p in
                    Button("\(p["name"].text) — \(p["width"].int ?? 0)×\(p["height"].int ?? 0) @\(p["density"].int ?? 0)") {
                        state.resizeEmulator(id, width: p["width"].int ?? 0, height: p["height"].int ?? 0,
                                             density: p["density"].int ?? 0, override: true)
                    }
                }
                Button(tr("Reset to the device's own size")) {
                    state.resizeEmulator(id, width: 0, height: 0, density: 0, override: true)
                }
            }
            Section(tr("Change the hardware (next start, cold boot)")) {
                ForEach(Array(state.androidPresets.enumerated()), id: \.offset) { _, p in
                    Button(p["name"].text) {
                        state.resizeEmulator(id, width: p["width"].int ?? 0, height: p["height"].int ?? 0,
                                             density: p["density"].int ?? 0, override: false)
                    }
                }
            }
        } label: {
            Image(systemName: "aspectratio")
        }
        .menuStyle(.borderlessButton).frame(width: 30).help(tr("Screen size"))
    }

    private func iconButton(_ symbol: String, _ help: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).frame(width: 18, height: 18)
        }
        .buttonStyle(.borderless)
        .foregroundStyle(Term.ink)
        .help(help)
    }
}

/// A new virtual device: a name, an installed system image, and a screen from
/// the standard sizes (or a custom one).
struct NewDeviceSheet: View {
    @EnvironmentObject var state: AppState
    @Binding var isPresented: Bool
    @State private var name = "Pixel 9"
    @State private var image = ""
    @State private var preset = "pixel_9"
    @State private var width = ""
    @State private var height = ""
    @State private var density = ""
    @State private var ram = "2048"

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(tr("New virtual device")).font(Term.font(14, .bold))
            Field(label: tr("name")) {
                TextField("", text: $name).textFieldStyle(TermFieldStyle())
            }
            Field(label: tr("system image")) {
                Picker("", selection: $image) {
                    ForEach(state.installedSystemImages, id: \.self) { Text($0).tag($0) }
                }
            }
            Field(label: tr("screen")) {
                Picker("", selection: $preset) {
                    ForEach(Array(state.androidPresets.enumerated()), id: \.offset) { _, p in
                        Text("\(p["name"].text) — \(p["width"].int ?? 0)×\(p["height"].int ?? 0) @\(p["density"].int ?? 0) · \(p["width_dp"].int ?? 0)×\(p["height_dp"].int ?? 0) dp · \(p["window_class"].text)")
                            .tag(p["id"].text)
                    }
                    Text(tr("Custom")).tag("")
                }
            }
            if preset.isEmpty {
                HStack {
                    TextField(tr("width px"), text: $width).textFieldStyle(TermFieldStyle())
                    TextField(tr("height px"), text: $height).textFieldStyle(TermFieldStyle())
                    TextField(tr("dpi"), text: $density).textFieldStyle(TermFieldStyle())
                }
            }
            Field(label: "RAM MB") {
                TextField("", text: $ram).textFieldStyle(TermFieldStyle()).frame(width: 90)
            }
            HStack {
                Spacer()
                Button(tr("cancel")) { isPresented = false }.buttonStyle(TermButtonStyle())
                Button(tr("create")) {
                    state.createAvd(name: name, image: image, preset: preset,
                                    width: Int(width) ?? 0, height: Int(height) ?? 0,
                                    density: Int(density) ?? 0, ramMb: Int(ram) ?? 2048)
                    isPresented = false
                }
                .buttonStyle(TermButtonStyle(filled: true))
                .disabled(name.isEmpty || image.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 620)
        .onAppear { if image.isEmpty { image = state.installedSystemImages.first ?? "" } }
    }
}

/// Google's catalog for this Mac: the emulator, platform-tools, and system
/// images by Android version. A license is shown in full and accepted only
/// by a person pressing accept.
struct InstallImagesPanel: View {
    @EnvironmentObject var state: AppState
    @State private var filter = ""
    @State private var licenseFor: JSON = .null

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                TextField(tr("filter: 35, playstore, google_apis…"), text: $filter)
                    .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                Button(tr("load catalog")) { state.loadAndroidCatalog() }
                    .buttonStyle(TermButtonStyle(filled: true))
                Spacer()
                installProgress
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(packages.enumerated()), id: \.offset) { _, p in row(p) }
                }
            }
            if state.androidCatalogDoc.isNull {
                Text(tr("Loads Google's SDK manifests from dl.google.com: the same catalog "
                        + "Android Studio's SDK Manager shows, stable channel, for this Mac."))
                    .font(Term.body).foregroundStyle(Term.dim)
            }
        }
        .padding(16)
        .sheet(isPresented: Binding(get: { !licenseFor.isNull }, set: { if !$0 { licenseFor = .null } })) {
            LicenseSheet(license: licenseFor) { licenseFor = .null }
        }
    }

    private var packages: [JSON] {
        let all = state.androidCatalogDoc["packages"].array
        guard !filter.isEmpty else { return all }
        return all.filter { $0["path"].text.localizedCaseInsensitiveContains(filter)
            || $0["display_name"].text.localizedCaseInsensitiveContains(filter) }
    }

    private var accepted: Set<String> {
        Set(state.androidCatalogDoc["accepted_licenses"].array.compactMap { $0.string })
    }

    @ViewBuilder private var installProgress: some View {
        let d = state.installDoc
        if d["running"].bool == true {
            let total = Double(max(1, d["total"].int ?? 1))
            HStack(spacing: 6) {
                Text("\(d["phase"].text) \(d["path"].text.components(separatedBy: ";").dropFirst().joined(separator: " "))")
                    .font(Term.small).foregroundStyle(Term.dim).lineLimit(1)
                ProgressView(value: Double(d["done"].int ?? 0) / total).frame(width: 140)
                Button(tr("cancel")) { state.cancelInstall() }.buttonStyle(TermButtonStyle(tone: Term.amber))
            }
        }
    }

    private func row(_ p: JSON) -> some View {
        let lic = p["license_id"].text
        let ok = lic.isEmpty || accepted.contains(lic)
        let installed = !p["installed_revision"].isNull
        let gb = Double(p["size_bytes"].int ?? 0) / 1_073_741_824
        return HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 1) {
                Text(p["api_level"].int != nil ? "Android API \(p["api_level"].int ?? 0) · \(p["tag_display"].text)"
                                                : p["display_name"].text)
                    .font(Term.font(12, .bold)).foregroundStyle(Term.ink)
                Text(p["path"].text + " · r" + p["revision"].text)
                    .font(Term.small).foregroundStyle(Term.dim).lineLimit(1)
            }
            Spacer()
            Text(gb >= 1 ? String(format: "%.1f GB", gb) : String(format: "%.0f MB", gb * 1024))
                .font(Term.small).foregroundStyle(Term.dim)
            if installed {
                Chip(text: "installed " + p["installed_revision"].text, tone: .good)
            } else if !ok {
                Button(tr("license…")) {
                    licenseFor = state.androidCatalogDoc["licenses"].array.first { $0["id"].text == lic } ?? .null
                }
                .buttonStyle(TermButtonStyle(tone: Term.amber))
            } else {
                Button(tr("install")) { state.installPackage(p["path"].text) }
                    .buttonStyle(TermButtonStyle(filled: true))
                    .disabled(state.installDoc["running"].bool == true)
            }
        }
        .padding(.vertical, 4).padding(.horizontal, 8)
        .background(Term.panel, in: RoundedRectangle(cornerRadius: 3))
    }
}

struct LicenseSheet: View {
    @EnvironmentObject var state: AppState
    let license: JSON
    let dismiss: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(license["id"].text).font(Term.font(14, .bold))
            Text(tr("Google requires this license to be accepted before the package is "
                    + "installed. DevX records your acceptance the way sdkmanager does."))
                .font(Term.body).foregroundStyle(Term.dim)
            ScrollView {
                Text(license["text"].text).font(Term.small).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(height: 360)
            .background(Term.panel)
            HStack {
                Spacer()
                Button(tr("decline")) { dismiss() }.buttonStyle(TermButtonStyle())
                Button(tr("accept")) {
                    state.acceptLicense(license["id"].text)
                    dismiss()
                }
                .buttonStyle(TermButtonStyle(filled: true))
            }
        }
        .padding(20).frame(width: 680)
    }
}
