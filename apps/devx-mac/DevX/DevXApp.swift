import SwiftUI

/// Belt and braces for file-open launches. `main.swift` keeps bare command-line
/// arguments away from AppKit, which is what actually stopped SwiftUI from
/// creating a window; these hooks cover the other way in — a file dropped on
/// the app icon or opened through LaunchServices — so DevX acknowledges it and
/// still shows its window instead of starting up invisible.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        sender.reply(toOpenOrPrint: .success)
    }

    func application(_ application: NSApplication, open urls: [URL]) {}

    func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool { true }



}

struct DevXApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var state = AppState()
    private let launch = devxLaunch

    /// Applies the launch options once the first frame is on screen.
    ///
    /// The tab is set before anything is fetched: selecting a tab already
    /// triggers that tab's own load, so loading first and selecting after ran
    /// the same request twice.
    @MainActor private func applyLaunchOptions() {
        if let dir = launch.sessionsDir { state.sessionsDir = dir }
        if let dev = launch.device { state.selectedDevice = dev }
        if let app = launch.app { state.selectedApp = app }
        if launch.startLive { state.tab = .live }
        if let tab = launch.tab { state.tab = tab }

        if let b = launch.baseline { state.baselinePath = b }
        if let c = launch.candidate { state.candidatePath = c }

        state.loadLanguage()
        state.loadRememberedTarget()
        state.loadRecents()
        state.loadVersion()
        state.loadDevices()
        if state.tab != .sessions { state.loadSessions() }
        if let id = launch.session {
            state.openSession(id, revealIn: launch.tab ?? .issues,
                              focusIssueId: launch.issue)
        }

        if launch.baseline != nil, launch.candidate != nil {
            state.tab = .compare
            state.runCompare()
        }

        guard launch.startLive else { return }
        Task { @MainActor in
            // Device resolution takes a couple of seconds, so the start is
            // deferred a moment rather than racing the initial discovery.
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            state.startLive()
            guard let seconds = launch.liveSeconds else { return }
            // Wait for the session to actually be running before timing it. A
            // fixed sleep raced the start and stopped a session that had not
            // begun, which silently produced no capture at all.
            var waited = 0
            while !state.liveRunning && waited < 40 {
                try? await Task.sleep(nanoseconds: 500_000_000)
                waited += 1
            }
            guard state.liveRunning else { return }
            try? await Task.sleep(nanoseconds: UInt64(seconds) * 1_000_000_000)
            if state.liveRunning { state.stopLive() }
        }
    }

    var body: some Scene {
        WindowGroup("DevX") {
            RootView().environmentObject(state)
                .frame(minWidth: 1040, minHeight: 680)
                .onAppear {
                    // Deferred by one turn of the run loop rather than run
                    // inside `onAppear` itself. Every call below publishes
                    // state, and doing that while the first frame is still
                    // being laid out is undefined behaviour in SwiftUI: with
                    // `--tab=sessions` it left the whole window blank, sidebar
                    // included, because setting the tab triggers that tab's
                    // own load and the same request was registered twice in
                    // one update cycle.
                    DispatchQueue.main.async { applyLaunchOptions() }
                }
        }
        .windowToolbarStyle(.unified)
        .commands {
            CommandGroup(after: .newItem) {
                Button(tr("Refresh Devices")) { state.loadDevices() }
                    .keyboardShortcut("r")
                Button(tr("Cancel Running Operation")) { state.cancel() }
                    .keyboardShortcut(".", modifiers: .command)
            }
        }
    }
}

struct RootView: View {
    @EnvironmentObject var state: AppState

    private func targetLine(_ key: String, _ value: String) -> some View {
        HStack(spacing: 5) {
            Text(key).foregroundStyle(Term.green.opacity(0.8))
            Text("▸").foregroundStyle(Term.line)
            Text(value).foregroundStyle(Term.dim).lineLimit(1).truncationMode(.middle)
        }
        .font(Term.font(10))
    }

    var body: some View {
        NavigationSplitView {
            List(DevXTab.allCases, selection: Binding(
                get: { state.tab },
                set: { if let v = $0 { state.tab = v } })) { tab in
                NavigationLink(value: tab) {
                    Label(tab.title.uppercased(), systemImage: tab.icon)
                        .font(Term.font(11, .semibold))
                        .kerning(0.9)
                }
            }
            .scrollContentBackground(.hidden)
            .background(Term.bg)
            .navigationSplitViewColumnWidth(min: 168, ideal: 186)
            .safeAreaInset(edge: .top) {
                // A banner line, the way a tool announces itself on stdout
                // before it gets to work.
                HStack(spacing: 6) {
                    Text("▚").foregroundStyle(Term.green).phosphor(Term.green, radius: 3)
                    Text("DEVX")
                        .font(Term.font(13, .heavy))
                        .kerning(2.4)
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 13).padding(.top, 4).padding(.bottom, 7)
                .background(Term.bg)
            }
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 3) {
                    Rectangle().fill(Term.line).frame(height: 1)
                    Text("engine \(state.engineVersion) · ruleset \(state.rulesetVersion)")
                        .font(Term.font(10))
                        .foregroundStyle(Term.dim)
                    // The pinned target, in the shape a shell would show it.
                    if !state.selectedDevice.isEmpty {
                        targetLine("dev", state.selectedDevice)
                    }
                    if !state.selectedApp.isEmpty {
                        targetLine("app", state.selectedApp)
                    }
                }
                .padding(.horizontal, 11).padding(.bottom, 7)
                .background(Term.bg)
            }
        } detail: {
            ZStack {
                switch state.tab {
                case .devices: DevicesView()
                case .apps: AppsView()
                case .preflight: PreflightView()
                case .live: LiveView()
                case .record: RecordView()
                case .inspect: InspectView()
                case .sessions: SessionsView()
                case .issues: IssuesView()
                case .threads: ThreadsView()
                case .timeline: TimelineView()
                case .compare: CompareView()
                case .settings: SettingsView()
                case .detectors: DetectorsView()
                }

                // The live view shows its own progress inline, so the modal
                // overlay is suppressed there: it previously sat on top of a
                // capture that was already streaming.
                if let busy = state.busy, state.tab != .live {
                    VStack(spacing: 9) {
                        HStack(spacing: 8) {
                            AsciiSpinner()
                            Text(busy).font(Term.body)
                        }
                        Button("cancel") { state.cancel() }
                            .buttonStyle(TermButtonStyle(tone: Term.amber))
                    }
                    .padding(22)
                    .background(Term.raised, in: RoundedRectangle(cornerRadius: 3))
                    .overlay(RoundedRectangle(cornerRadius: 3)
                        .strokeBorder(Term.green.opacity(0.45)))
                    .shadow(color: .black.opacity(0.7), radius: 18)
                }
            }
            .safeAreaInset(edge: .top) {
                if let err = state.lastError {
                    HStack(alignment: .top, spacing: 8) {
                        Text("stderr")
                            .font(Term.font(11, .bold))
                            .foregroundStyle(Term.red)
                        Text(err).font(Term.body).lineLimit(3)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer()
                        Button { state.lastError = nil } label: {
                            Text("[dismiss]")
                                .font(Term.font(11))
                        }
                        .buttonStyle(.borderless)
                    }
                    .padding(10)
                    .background(Term.red.opacity(0.10))
                    .overlay(alignment: .bottom) {
                        Rectangle().fill(Term.red.opacity(0.5)).frame(height: 1)
                    }
                }
            }
        }
        .onChange(of: state.tab) { _, tab in
            // Each view loads on entry rather than polling, so nothing runs
            // device tooling in the background without the operator asking.
            switch tab {
            case .apps: if state.apps.isEmpty { state.loadApps() }
            case .sessions: state.loadSessions()
            case .detectors: state.loadRules()
            default: break
            }
        }
        .onChange(of: state.selectedDevice) { _, _ in
            if state.tab == .apps { state.loadApps() }
        }
        .terminalChrome()
    }
}
