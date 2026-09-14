import SwiftUI

@main
struct DevXApp: App {
    @StateObject private var state = AppState()
    private let launch = LaunchOptions.parse(Array(CommandLine.arguments.dropFirst()))

    var body: some Scene {
        WindowGroup("DevX") {
            RootView().environmentObject(state)
                .frame(minWidth: 1040, minHeight: 680)
                .onAppear {
                    if let dir = launch.sessionsDir { state.sessionsDir = dir }
                    state.loadVersion()
                    state.loadDevices()
                    state.loadSessions()
                    if let tab = launch.tab { state.tab = tab }
                    if let id = launch.session { state.openSession(id) }
                }
        }
        .windowToolbarStyle(.unified)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Refresh Devices") { state.loadDevices() }
                    .keyboardShortcut("r")
                Button("Cancel Running Operation") { state.cancel() }
                    .keyboardShortcut(".", modifiers: .command)
            }
        }
    }
}

struct RootView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        NavigationSplitView {
            List(DevXTab.allCases, selection: Binding(
                get: { state.tab },
                set: { if let v = $0 { state.tab = v } })) { tab in
                NavigationLink(value: tab) {
                    Label(tab.title, systemImage: tab.icon)
                }
            }
            .navigationSplitViewColumnWidth(min: 168, ideal: 186)
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 3) {
                    Divider()
                    Text("engine \(state.engineVersion) · ruleset \(state.rulesetVersion)")
                        .font(.caption2).foregroundStyle(.secondary)
                    if !state.selectedDevice.isEmpty {
                        Text(state.selectedDevice)
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundStyle(.tertiary).lineLimit(1)
                    }
                    if !state.selectedApp.isEmpty {
                        Text(state.selectedApp)
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundStyle(.tertiary).lineLimit(1)
                    }
                }
                .padding(.horizontal, 11).padding(.bottom, 7)
            }
        } detail: {
            ZStack {
                switch state.tab {
                case .devices: DevicesView()
                case .apps: AppsView()
                case .preflight: PreflightView()
                case .record: RecordView()
                case .sessions: SessionsView()
                case .issues: IssuesView()
                case .detectors: DetectorsView()
                }

                if let busy = state.busy {
                    VStack(spacing: 9) {
                        ProgressView()
                        Text(busy).font(.callout)
                        Button("Cancel") { state.cancel() }
                            .buttonStyle(.bordered)
                    }
                    .padding(22)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
                    .shadow(radius: 14)
                }
            }
            .safeAreaInset(edge: .top) {
                if let err = state.lastError {
                    HStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                        Text(err).font(.callout).lineLimit(3)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer()
                        Button { state.lastError = nil } label: {
                            Image(systemName: "xmark.circle.fill")
                        }
                        .buttonStyle(.borderless)
                    }
                    .padding(10)
                    .background(.orange.opacity(0.12))
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
    }
}
