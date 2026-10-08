import AppKit
import Security
import SwiftUI

/// Provider credentials for Intelligence connectors in the macOS Keychain,
/// where `mpi` and `mpi mcp` read them too. Written through the Security
/// framework, never to a file or a command line, and never read back into
/// the window: after saving, the window only knows that one exists.
enum ConnectorKeychain {
    static func save(service: String, value: String) -> Bool {
        remove(service: service)
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "devx",
            kSecValueData as String: Data(value.utf8),
        ]
        return SecItemAdd(item as CFDictionary, nil) == errSecSuccess
    }

    static func remove(service: String) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service]
        SecItemDelete(q as CFDictionary)
    }
}

/// Joins pieces of text. A long `+` chain of Strings inside a SwiftUI
/// builder is what Swift's type checker gives up on ("unable to type-check
/// this expression in reasonable time" on CI's toolchain); a variadic call
/// checks each piece on its own.
func cat(_ parts: String...) -> String { parts.joined() }

/// Opens a provider's page (where its token is made, where its export is set
/// up) in the browser. https only: these links come from the connector
/// contract and a person's signed-in browser is what opens them.
func openProviderPage(_ link: String) {
    guard link.hasPrefix("https://"), let url = URL(string: link) else { return }
    NSWorkspace.shared.open(url)
}

/// A connector's token page for the base_url being typed, or its default.
func tokenURL(_ info: JSON, baseURL: String) -> String {
    let template = info["credential_url"].text
    guard !template.isEmpty else { return "" }
    var base = baseURL.trimmingCharacters(in: .whitespaces)
    if base.isEmpty { base = info["default_base_url"].text }
    while base.hasSuffix("/") { base.removeLast() }
    return template.replacingOccurrences(of: "{base_url}", with: base)
}

/// The Intelligence module's state. Everything is read through the C ABI;
/// the window never talks to a provider (FR-17).
@MainActor
final class IntelligenceModel: ObservableObject {
    @Published var workspaces: [JSON] = []
    @Published var workspace = ""
    @Published var overview: JSON = .null
    @Published var integrations: JSON = .null
    @Published var providers: [JSON] = []
    @Published var releases: [JSON] = []
    @Published var selectedRelease = ""
    @Published var release: JSON = .null
    @Published var compareBase = ""
    @Published var compare: JSON = .null
    @Published var signals: JSON = .null
    @Published var selectedSignal = ""
    @Published var signal: JSON = .null
    @Published var related: JSON = .null
    @Published var code: JSON = .null
    @Published var pack: JSON = .null
    @Published var egress: JSON = .null
    @Published var retention: JSON = .null
    @Published var message = ""
    @Published var loading = false

    private static let queue = DispatchQueue(label: "devx.intelligence", qos: .userInitiated)

    /// A local read: quick, off the main thread, no busy overlay.
    func read(_ work: @escaping @Sendable () -> JSON, then apply: @escaping (JSON) -> Void) {
        loading = true
        Self.queue.async {
            let doc = work()
            DispatchQueue.main.async {
                self.loading = false
                if doc["ok"].bool == false { self.message = doc["error"].text }
                apply(doc)
            }
        }
    }

    func loadWorkspaces(_ dir: String) {
        read({ Core.intelWorkspaces(dir) }) { doc in
            self.workspaces = doc["workspaces"].array
            if self.workspace.isEmpty || !self.workspaces.contains(where: { $0["id"].text == self.workspace }) {
                self.workspace = self.workspaces.first?["id"].text ?? ""
            }
            self.reloadAll(dir)
        }
        if providers.isEmpty {
            read({ Core.intelProviders() }) { self.providers = $0["providers"].array }
        }
    }

    func reloadAll(_ dir: String) {
        let ws = workspace
        guard !ws.isEmpty else { return }
        read({ Core.intelOverview(dir, ws) }) { self.overview = $0 }
        read({ Core.intelIntegrations(dir, ws) }) { self.integrations = $0 }
        read({ Core.intelReleases(dir, ws) }) { doc in
            self.releases = doc["releases"].array
            if self.selectedRelease.isEmpty, let first = self.releases.first {
                self.selectRelease(dir, first["key"].text)
            }
        }
    }

    func selectRelease(_ dir: String, _ key: String) {
        selectedRelease = key
        compare = .null
        let ws = workspace
        read({ Core.intelRelease(dir, ws, key) }) { self.release = $0 }
    }

    func runCompare(_ dir: String) {
        let ws = workspace, base = compareBase, cand = selectedRelease
        guard !base.isEmpty, !cand.isEmpty else { return }
        read({ Core.intelCompare(dir, ws, base, cand) }) { self.compare = $0 }
    }

    func query(_ dir: String, _ q: [String: JSON]) {
        let ws = workspace
        let doc = JSON.object(q)
        read({ Core.intelSignals(dir, ws, doc) }) { self.signals = $0 }
    }

    func selectSignal(_ dir: String, _ id: String) {
        selectedSignal = id
        code = .null
        let ws = workspace
        read({ Core.intelSignal(dir, ws, id, raw: false) }) { self.signal = $0 }
        read({ Core.intelRelated(dir, ws, id) }) { self.related = $0 }
    }

    func loadRaw(_ dir: String) {
        let ws = workspace, id = selectedSignal
        read({ Core.intelSignal(dir, ws, id, raw: true) }) { self.signal = $0 }
    }

    func loadCode(_ dir: String) {
        let ws = workspace, id = selectedSignal
        read({ Core.intelCodeContext(dir, ws, id) }) { self.code = $0 }
    }

    func buildPack(_ dir: String, release: String, signal: String, question: String) {
        let ws = workspace
        var scope: [String: JSON] = ["question": .string(question)]
        if !release.isEmpty { scope["release"] = .string(release) }
        if !signal.isEmpty { scope["signal_id"] = .string(signal) }
        let s = JSON.object(scope)
        read({ Core.intelPack(dir, ws, s) }) { self.pack = $0 }
    }
}

/// DevX > Intelligence: production, CI/CD and other external evidence, kept
/// locally and correlated with releases, code and sessions.
struct IntelligenceView: View {
    @EnvironmentObject var state: AppState
    @StateObject private var model = IntelligenceModel()
    @State private var section = 0

    private let sections = ["Overview", "Releases", "Production", "CI/CD", "Signals",
                            "AI Analysis", "Integrations"]

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                if !model.workspaces.isEmpty {
                    Picker("", selection: $model.workspace) {
                        ForEach(model.workspaces.map { $0["id"].text }, id: \.self) { Text($0).tag($0) }
                    }
                    .labelsHidden().frame(maxWidth: 170)
                    .onChange(of: model.workspace) { _, _ in
                        model.selectedRelease = ""
                        model.reloadAll(state.sessionsDir)
                    }
                }
                Picker("", selection: $section) {
                    ForEach(Array(sections.enumerated()), id: \.offset) { i, name in
                        Text(tr(name)).tag(i)
                    }
                }
                .pickerStyle(.segmented).frame(maxWidth: 720)
                Spacer()
                if model.loading { AsciiSpinner() }
                Button(tr("refresh")) { model.loadWorkspaces(state.sessionsDir) }
                    .buttonStyle(TermButtonStyle())
            }
            .padding(.horizontal, 16).padding(.vertical, 10)
            if !model.message.isEmpty {
                HStack {
                    Text(model.message).font(Term.small).foregroundStyle(Term.amber)
                        .textSelection(.enabled).lineLimit(3)
                    Spacer()
                    Button(tr("dismiss")) { model.message = "" }.buttonStyle(.plain).font(Term.small)
                }
                .padding(.horizontal, 16).padding(.bottom, 6)
            }
            Group {
                if model.workspaces.isEmpty && section != 6 {
                    VStack {
                        TermEmpty(title: "No workspace yet",
                                  detail: "A workspace binds a repository and your app ids to connectors "
                                      + "such as Sentry, GitLab CI or Firebase exports.",
                                  hint: "open Integrations to create one")
                        Button(tr("create a workspace")) { section = 6 }
                            .buttonStyle(TermButtonStyle(filled: true))
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    switch section {
                    case 1: IntelReleasesView(model: model)
                    case 2: IntelSignalsView(model: model, preset: ["crash", "issue", "metric"],
                                             title: "Production")
                    case 3: IntelSignalsView(model: model, preset: ["pipeline", "ci_job", "deploy", "test"],
                                             title: "CI/CD")
                    case 4: IntelSignalsView(model: model, preset: [], title: "Signals")
                    case 5: IntelAIView(model: model)
                    case 6: IntelIntegrationsView(model: model)
                    default: IntelOverviewView(model: model, openRelease: { key in
                        model.selectRelease(state.sessionsDir, key)
                        section = 1
                    })
                    }
                }
            }
            .environmentObject(state)
        }
        .navigationTitle("~/intelligence")
        .onAppear { model.loadWorkspaces(state.sessionsDir) }
    }
}

// ---- shared pieces ----

private func basisTone(_ basis: String) -> StatusTone {
    switch basis {
    case "exact": return .good
    case "provider_attributed": return .good
    case "candidate": return .neutral
    default: return .caution
    }
}

private func severityTone(_ s: String) -> StatusTone {
    switch s {
    case "fatal", "error": return .bad
    case "warning": return .caution
    case "": return .neutral
    default: return .good
    }
}

private func stateTone(_ s: String) -> StatusTone {
    switch s {
    case "up_to_date", "complete": return .good
    case "partial", "syncing", "never_synced", "paused": return .caution
    case "needs_auth", "error", "failed": return .bad
    default: return .neutral
    }
}

/// One link, exact or candidate, in the words it rests on.
private struct LinkRow: View {
    let edge: JSON
    var body: some View {
        HStack(alignment: .top, spacing: 6) {
            Chip(text: edge["basis"].text, tone: basisTone(edge["basis"].text))
            VStack(alignment: .leading, spacing: 2) {
                Text(cat(edge["relation"].text, ": ", edge["from"].text, " → ", edge["to"].text))
                    .font(Term.small).foregroundStyle(Term.dim).lineLimit(1).truncationMode(.middle)
                Text(edge["evidence"].text).font(Term.body).foregroundStyle(Term.ink)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

private struct SignalRow: View {
    let s: JSON
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(String(s["occurred_at"].text.prefix(16))).font(Term.small).foregroundStyle(Term.dim)
                .frame(width: 118, alignment: .leading)
            Text(cat(s["provider"].text, "/", s["kind"].text)).font(Term.small).foregroundStyle(Term.cyan)
                .frame(width: 130, alignment: .leading).lineLimit(1)
            if !s["severity"].text.isEmpty {
                Chip(text: s["severity"].text, tone: severityTone(s["severity"].text))
            }
            Text(s["title"].display(s["external_id"].text)).font(Term.body).foregroundStyle(Term.ink)
                .lineLimit(1).truncationMode(.tail)
            Spacer(minLength: 4)
            let a = s["attributes"]
            if let n = a["count"].int ?? a["events"].int {
                Text("\(n) ev").font(Term.small).foregroundStyle(Term.dim)
            }
            if !a["root_error"].text.isEmpty {
                Text(a["root_error"].text).font(Term.small).foregroundStyle(Term.red).lineLimit(1)
                    .frame(maxWidth: 260, alignment: .trailing)
            }
            if a["p95"].double != nil {
                Text(cat("p95 ", a["p95"].display(), " ", a["unit"].text)).font(Term.small).foregroundStyle(Term.dim)
            }
        }
    }
}

// ---- Overview ----

private struct IntelOverviewView: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel
    let openRelease: (String) -> Void

    var body: some View {
        let o = model.overview
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                let attention = o["needs_attention"].array
                if !attention.isEmpty {
                    Banner(kind: .caution, title: "Needs attention",
                           message: attention.map { $0.text }.joined(separator: "\n"))
                }
                freshness(o["freshness"])
                HStack(alignment: .top, spacing: 10) {
                    card("Production", o["cards"]["production"])
                    card("CI/CD", o["cards"]["ci_cd"])
                    card("Tests", o["cards"]["tests"])
                    card("Local sessions", o["cards"]["local_sessions"])
                }
                Panel(title: "Latest releases",
                      subtitle: "Newest activity first. Open one for its timeline, evidence and links.") {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(Array(o["latest_releases"].array.enumerated()), id: \.offset) { _, r in
                            HStack(spacing: 8) {
                                Button(r["key"].text) { openRelease(r["key"].text) }
                                    .buttonStyle(.plain).font(Term.font(12, .semibold)).foregroundStyle(Term.green)
                                Text(cat("crashes \(r["crashes"].int ?? 0) · issues \(r["issues"].int ?? 0) · ",
                                         "CI failures \(r["ci_failures"].int ?? 0) · sessions \(r["sessions"].int ?? 0)"))
                                    .font(Term.small).foregroundStyle(Term.dim)
                                if (r["conflicts"].int ?? 0) > 0 {
                                    Chip(text: "\(r["conflicts"].int ?? 0) conflict", tone: .caution)
                                }
                                Spacer()
                                Text(r["deployed_at"].display("not deployed")).font(Term.small).foregroundStyle(Term.dim)
                            }
                        }
                        if o["latest_releases"].array.isEmpty {
                            Text(tr("No evidence names a release yet. Sync a connector in Integrations."))
                                .font(Term.body).foregroundStyle(Term.dim)
                        }
                    }
                }
                Panel(title: "AI insight",
                      subtitle: "Ask your AI tool through mpi mcp; DevX gives it the evidence, not the conclusion.") {
                    Text(tr("Open AI Analysis to build an evidence pack for a release or a signal. The signals "
                            + "above stay the source of truth: an AI answer is never the only place a problem "
                            + "is shown."))
                        .font(Term.body).foregroundStyle(Term.ink).fixedSize(horizontal: false, vertical: true)
                }
                Text(cat(tr("Links: \(o["exact_links"].int ?? 0) exact, \(o["candidate_links"].int ?? 0) candidate. Storage: "),
                         formatBytes(o["storage"]["total_bytes"].double ?? 0)))
                    .font(Term.small).foregroundStyle(Term.dim)
            }
            .padding(16)
        }
    }

    private func card(_ title: String, _ c: JSON) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(tr(title).uppercased()).font(Term.font(11, .bold)).foregroundStyle(Term.dim)
            Text(c["count"].display("0")).font(Term.font(22, .bold)).foregroundStyle(Term.ink)
            Text(c["note"].text).font(Term.small).foregroundStyle(Term.dim)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(12).frame(maxWidth: .infinity, alignment: .leading)
        .background(Term.panel, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Term.line))
    }

    private func freshness(_ f: JSON) -> some View {
        HStack(spacing: 8) {
            Text(tr("freshness")).font(Term.small).foregroundStyle(Term.dim)
            ForEach(f.keys, id: \.self) { id in
                let s = f[id]
                Chip(text: cat(id, ": ", s["state"].text), tone: stateTone(s["state"].text))
                    .help(cat(tr("last success"), " ", s["last_success_at"].display("never")))
            }
            if f.keys.isEmpty {
                Text(tr("no connectors")).font(Term.small).foregroundStyle(Term.dim)
            }
        }
    }
}

// ---- Releases ----

private struct IntelReleasesView: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel

    var body: some View {
        HSplitView {
            ScrollView {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(model.releases.enumerated()), id: \.offset) { _, r in
                        let key = r["key"].text
                        Button {
                            model.selectRelease(state.sessionsDir, key)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(key).font(Term.font(12, .semibold))
                                    .foregroundStyle(key == model.selectedRelease ? Term.green : Term.ink)
                                    .lineLimit(1).truncationMode(.middle)
                                Text(cat("crashes \(r["crashes"].int ?? 0) · CI fail \(r["ci_failures"].int ?? 0) · ",
                                         "sessions \(r["sessions"].int ?? 0)"))
                                    .font(Term.small).foregroundStyle(Term.dim)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(6)
                            .background(key == model.selectedRelease ? Term.raised : .clear)
                        }
                        .buttonStyle(.plain)
                    }
                    if model.releases.isEmpty {
                        Text(tr("No releases yet.")).font(Term.body).foregroundStyle(Term.dim).padding(8)
                    }
                }
                .padding(8)
            }
            .frame(minWidth: 220, idealWidth: 280, maxWidth: 360)
            detail.frame(minWidth: 420, maxWidth: .infinity)
        }
    }

    private var detail: some View {
        let r = model.release
        return ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if r["ok"].bool == true {
                    header(r)
                    if !r["conflicts"].array.isEmpty {
                        Banner(kind: .caution, title: "Sources disagree",
                               message: r["conflicts"].array.map { $0.text }.joined(separator: "\n"))
                    }
                    Panel(title: "Timeline", subtitle: "build → test → deploy → first production signal → sessions") {
                        VStack(alignment: .leading, spacing: 3) {
                            ForEach(Array(r["timeline"].array.enumerated()), id: \.offset) { _, t in
                                HStack {
                                    Text(t["at"].text).font(Term.small).foregroundStyle(Term.dim)
                                        .frame(width: 170, alignment: .leading)
                                    Text(t["what"].text).font(Term.body).foregroundStyle(Term.ink)
                                }
                            }
                        }
                    }
                    ForEach(r["evidence"].keys, id: \.self) { provider in
                        Panel(title: provider) {
                            VStack(alignment: .leading, spacing: 4) {
                                ForEach(Array(r["evidence"][provider].array.prefix(40).enumerated()), id: \.offset) { _, s in
                                    SignalRow(s: s)
                                }
                            }
                        }
                    }
                    if !r["sessions"].array.isEmpty {
                        Panel(title: "Sessions", subtitle: "DevX and BrowserStack sessions that measured this build") {
                            ForEach(Array(r["sessions"].array.enumerated()), id: \.offset) { _, s in
                                HStack {
                                    Button(s["id"].text) { state.openSession(s["id"].text, revealIn: .timeline) }
                                        .buttonStyle(.plain).foregroundStyle(Term.green).font(Term.body)
                                    Text(cat(s["device_id"].text, " · ", s["created_at"].text))
                                        .font(Term.small).foregroundStyle(Term.dim)
                                }
                            }
                        }
                    }
                    Panel(title: "Exact links") {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(Array(r["exact_links"].array.prefix(60).enumerated()), id: \.offset) { _, e in
                                LinkRow(edge: e)
                            }
                        }
                    }
                    if !r["candidate_links"].array.isEmpty {
                        Panel(title: "Candidate links",
                              subtitle: "Timing or partial metadata: something to investigate, never a cause.") {
                            VStack(alignment: .leading, spacing: 6) {
                                ForEach(Array(r["candidate_links"].array.prefix(60).enumerated()), id: \.offset) { _, e in
                                    LinkRow(edge: e)
                                }
                            }
                        }
                    }
                    if !r["missing_evidence"].array.isEmpty {
                        Panel(title: "Missing evidence") {
                            VStack(alignment: .leading, spacing: 3) {
                                ForEach(Array(r["missing_evidence"].array.enumerated()), id: \.offset) { _, m in
                                    Text("- " + m.text).font(Term.body).foregroundStyle(Term.amber)
                                }
                            }
                        }
                    }
                    comparePanel
                } else {
                    TermEmpty(title: "Pick a release", detail: "Releases are what your signals name.")
                }
            }
            .padding(16)
        }
    }

    private func header(_ r: JSON) -> some View {
        let id = r["identity"]
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(r["key"].text).font(Term.font(16, .bold)).foregroundStyle(Term.ink).textSelection(.enabled)
                Spacer()
                Button(r["pinned"].bool == true ? tr("unpin") : tr("pin")) {
                    let dir = state.sessionsDir, ws = model.workspace, key = r["key"].text
                    let pin = r["pinned"].bool != true
                    model.read({ Core.intelPin(dir, ws, kind: "release", id: key, pinned: pin) }) { _ in
                        model.selectRelease(dir, key)
                    }
                }
                .buttonStyle(TermButtonStyle())
            }
            HStack(spacing: 14) {
                Field(label: "version") { Text(id["version"].display()).font(Term.body) }
                Field(label: "build") { Text(id["build_number"].display()).font(Term.body) }
            }
            Field(label: "commit") { Text(id["commit_sha"].display()).font(Term.body).textSelection(.enabled) }
            Field(label: "branch") { Text(id["branch"].display()).font(Term.body) }
            Field(label: "deployed") { Text(r["deployed_at"].display("not recorded")).font(Term.body) }
            Field(label: "environments") {
                Text(r["environments"].array.map { $0.text }.joined(separator: ", ")).font(Term.body)
            }
        }
    }

    private var comparePanel: some View {
        Panel(title: "Compare", subtitle: "Against a baseline release: counts, failures and metric deltas") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Picker(tr("baseline"), selection: $model.compareBase) {
                        Text(tr("choose")).tag("")
                        ForEach(model.releases.map { $0["key"].text }.filter { $0 != model.selectedRelease },
                                id: \.self) { Text($0).tag($0) }
                    }
                    .frame(maxWidth: 360)
                    Button(tr("compare")) { model.runCompare(state.sessionsDir) }
                        .buttonStyle(TermButtonStyle(filled: true)).disabled(model.compareBase.isEmpty)
                }
                let c = model.compare
                if c["ok"].bool == true {
                    HStack(alignment: .top, spacing: 20) {
                        side("baseline", c["base_summary"])
                        side("this release", c["candidate_summary"])
                    }
                    ForEach(Array(c["metric_deltas"].array.enumerated()), id: \.offset) { _, m in
                        HStack {
                            Text(m["metric"].text).font(Term.body).foregroundStyle(Term.ink)
                            ForEach(["p50", "p95"], id: \.self) { p in
                                if !m[p].isNull {
                                    Text(cat("\(p) ", m[p]["base"].display(), " → ", m[p]["candidate"].display(),
                                             " ", m["unit"].text))
                                        .font(Term.small)
                                        .foregroundStyle((m[p]["delta"].double ?? 0) > 0 ? Term.amber : Term.green)
                                }
                            }
                        }
                    }
                    if !c["issues_only_in_candidate"].array.isEmpty {
                        Text(cat(tr("Only in this release: "),
                                 c["issues_only_in_candidate"].array.prefix(8).map { $0.text }.joined(separator: "; ")))
                            .font(Term.small).foregroundStyle(Term.amber).fixedSize(horizontal: false, vertical: true)
                    }
                    Text(c["note"].text).font(Term.small).foregroundStyle(Term.dim)
                }
            }
        }
    }

    private func side(_ title: String, _ s: JSON) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(tr(title).uppercased()).font(Term.font(11, .bold)).foregroundStyle(Term.dim)
            Text("crashes \(s["crashes"].int ?? 0) · issues \(s["issues"].int ?? 0)").font(Term.body)
            Text(cat(tr("events "), s["events"].display("not reported"), " · ",
                     tr("users "), s["affected_users"].display("not reported"))).font(Term.small)
            Text(cat("CI failures \(s["ci_failures"].int ?? 0) · tests failed ", s["tests_failed"].display("no report")))
                .font(Term.small)
        }
    }
}

// ---- Signals (and the Production / CI/CD presets) ----

private struct IntelSignalsView: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel
    let preset: [String]
    let title: String
    @State private var kind = ""
    @State private var provider = ""
    @State private var severity = ""
    @State private var environment = ""
    @State private var releaseKey = ""
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Picker(tr("kind"), selection: $kind) {
                    Text(tr("any")).tag("")
                    ForEach(kinds, id: \.self) { Text($0).tag($0) }
                }
                .frame(maxWidth: 170)
                TextField(tr("provider"), text: $provider).textFieldStyle(TermFieldStyle()).frame(maxWidth: 110)
                Picker(tr("severity"), selection: $severity) {
                    Text(tr("any")).tag("")
                    ForEach(["fatal", "error", "warning", "info"], id: \.self) { Text($0).tag($0) }
                }
                .frame(maxWidth: 170)
                TextField(tr("environment"), text: $environment).textFieldStyle(TermFieldStyle()).frame(maxWidth: 120)
                Picker(tr("release"), selection: $releaseKey) {
                    Text(tr("any")).tag("")
                    ForEach(model.releases.map { $0["key"].text }, id: \.self) { Text($0).tag($0) }
                }
                .frame(maxWidth: 260)
                TextField(tr("search"), text: $text).textFieldStyle(TermFieldStyle())
                    .onSubmit { search() }
                Button(tr("search")) { search() }.buttonStyle(TermButtonStyle(filled: true))
            }
            .padding(.horizontal, 16).padding(.bottom, 8)
            HSplitView {
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(model.signals["total"].int ?? 0) " + tr("signal(s)"))
                            .font(Term.small).foregroundStyle(Term.dim).padding(.bottom, 4)
                        ForEach(Array(model.signals["signals"].array.enumerated()), id: \.offset) { _, s in
                            Button { model.selectSignal(state.sessionsDir, s["id"].text) } label: {
                                SignalRow(s: s).padding(.vertical, 3).padding(.horizontal, 4)
                                    .background(s["id"].text == model.selectedSignal ? Term.raised : .clear)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(10)
                }
                .frame(minWidth: 420, maxWidth: .infinity)
                IntelSignalDetail(model: model).frame(minWidth: 320, idealWidth: 420, maxWidth: 560)
            }
        }
        .onAppear { search() }
        .onChange(of: model.workspace) { _, _ in search() }
    }

    private var kinds: [String] {
        preset.isEmpty ? ["crash", "issue", "metric", "pipeline", "ci_job", "deploy", "test", "release", "event"]
                       : preset
    }

    private func search() {
        var q: [String: JSON] = ["limit": .number(300)]
        for (k, v) in [("kind", kind), ("provider", provider), ("severity", severity),
                       ("environment", environment), ("release_key", releaseKey), ("text", text)] where !v.isEmpty {
            q[k] = .string(v)
        }
        // The Production and CI/CD presets ask for each of their kinds when
        // none is chosen; the core filters on one kind at a time.
        if kind.isEmpty && !preset.isEmpty {
            let dir = state.sessionsDir, ws = model.workspace, kinds = preset
            let base = q
            model.read({
                var all: [JSON] = []
                var total = 0
                for k in kinds {
                    var one = base
                    one["kind"] = .string(k)
                    let doc = Core.intelSignals(dir, ws, .object(one))
                    all += doc["signals"].array
                    total += doc["total"].int ?? 0
                }
                all.sort { $0["occurred_at"].text > $1["occurred_at"].text }
                return .object(["ok": .bool(true), "signals": .array(all), "total": .number(Double(total))])
            }) { model.signals = $0 }
        } else {
            model.query(state.sessionsDir, q)
        }
    }
}

private struct IntelSignalDetail: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel

    var body: some View {
        let doc = model.signal
        let s = doc["signal"]
        return ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                if doc["ok"].bool == true {
                    Text(s["title"].display(s["external_id"].text)).font(Term.font(14, .bold))
                        .foregroundStyle(Term.ink).textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Chip(text: cat(s["provider"].text, "/", s["kind"].text))
                        if !s["severity"].text.isEmpty { Chip(text: s["severity"].text, tone: severityTone(s["severity"].text)) }
                        Chip(text: s["basis"].text, tone: basisTone(s["basis"].text))
                        Spacer()
                        Button(tr("pin")) {
                            let dir = state.sessionsDir, ws = model.workspace, id = s["id"].text
                            model.read({ Core.intelPin(dir, ws, kind: "signal", id: id, pinned: true) }) { _ in
                                model.message = tr("Pinned: kept through retention.")
                            }
                        }
                        .buttonStyle(TermButtonStyle())
                    }
                    Panel(title: "Normalized") {
                        VStack(alignment: .leading, spacing: 3) {
                            Field(label: "occurred") { Text(s["occurred_at"].display()).font(Term.body) }
                            Field(label: "environment") { Text(s["environment"].display()).font(Term.body) }
                            Field(label: "release") {
                                Text([s["release"]["version"].text, s["release"]["build_number"].text,
                                      s["release"]["commit_sha"].text].filter { !$0.isEmpty }.joined(separator: " · "))
                                    .font(Term.body).textSelection(.enabled)
                            }
                            ForEach(s["attributes"].keys.filter { $0 != "frames" }, id: \.self) { k in
                                Field(label: k) {
                                    Text(s["attributes"][k].display(s["attributes"][k].serialized()))
                                        .font(Term.small).lineLimit(3).textSelection(.enabled)
                                }
                            }
                        }
                    }
                    let rel = model.related
                    if rel["ok"].bool == true {
                        Panel(title: "Links") {
                            VStack(alignment: .leading, spacing: 6) {
                                ForEach(Array((rel["exact_links"].array + rel["candidate_links"].array).enumerated()),
                                        id: \.offset) { _, e in LinkRow(edge: e) }
                                if rel["exact_links"].array.isEmpty && rel["candidate_links"].array.isEmpty {
                                    Text(tr("unlinked: insufficient evidence")).font(Term.body).foregroundStyle(Term.dim)
                                }
                            }
                        }
                    }
                    HStack {
                        Button(tr("raw evidence")) { model.loadRaw(state.sessionsDir) }
                            .buttonStyle(TermButtonStyle()).disabled(s["raw_ref"].text.isEmpty)
                        Button(tr("code context")) { model.loadCode(state.sessionsDir) }
                            .buttonStyle(TermButtonStyle())
                    }
                    if !doc["raw"].isNull {
                        Banner(kind: .caution, title: "Raw evidence", message: doc["raw_warning"].text)
                        Text(doc["raw"]["what"].text).font(Term.small).foregroundStyle(Term.dim)
                        Text(doc["raw"]["text"].display(doc["raw"]["why"].text))
                            .font(Term.font(11)).textSelection(.enabled)
                            .padding(8).background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
                    }
                    if !model.code.isNull { codePanel(model.code) }
                } else {
                    TermEmpty(title: "Pick a signal", detail: "Normalized fields first, then links, then raw evidence.")
                }
            }
            .padding(12)
        }
    }

    private func codePanel(_ c: JSON) -> some View {
        Panel(title: "Code context", subtitle: c["ref_basis"].text) {
            VStack(alignment: .leading, spacing: 6) {
                if c["ok"].bool != true {
                    Text(c["error"].text).font(Term.body).foregroundStyle(Term.amber)
                }
                ForEach(Array(c["frames"].array.enumerated()), id: \.offset) { _, f in
                    if f["resolved"].bool == true {
                        Text(f["path"].text).font(Term.font(12, .semibold)).foregroundStyle(Term.green)
                        Text(f["source"]["text"].text).font(Term.font(11)).textSelection(.enabled)
                        if f["blame"]["ok"].bool == true {
                            Text(cat(tr("last changed in "), String(f["blame"]["commit"].text.prefix(10)), " · ",
                                     f["blame"]["summary"].text, " · ", f["blame"]["author"].text))
                                .font(Term.small).foregroundStyle(Term.dim)
                        }
                    } else {
                        Text(f["why"].text).font(Term.small).foregroundStyle(Term.dim)
                    }
                }
                if c["diff"]["ok"].bool == true {
                    Text(tr("diff against ") + c["diff_base"].text).font(Term.small).foregroundStyle(Term.dim)
                    Text(c["diff"]["patch"].text).font(Term.font(11)).textSelection(.enabled)
                        .padding(6).background(Term.bg, in: RoundedRectangle(cornerRadius: 2))
                }
            }
        }
    }
}

// ---- AI Analysis ----

private struct IntelAIView: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel
    @State private var question = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Panel(title: "Evidence pack",
                      subtitle: "What your AI tool gets through mpi mcp: small cited facts, exact and candidate "
                          + "links apart, redacted excerpts, code context, and what is missing.") {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Picker(tr("release"), selection: $model.selectedRelease) {
                                Text(tr("choose")).tag("")
                                ForEach(model.releases.map { $0["key"].text }, id: \.self) { Text($0).tag($0) }
                            }
                            .frame(maxWidth: 380)
                            if !model.selectedSignal.isEmpty {
                                Chip(text: tr("signal ") + model.selectedSignal)
                            }
                        }
                        TextField(tr("question, e.g. why did this release regress?"), text: $question)
                            .textFieldStyle(TermFieldStyle())
                        HStack {
                            Button(tr("build evidence pack")) {
                                model.buildPack(state.sessionsDir, release: model.selectedRelease,
                                                signal: model.selectedSignal, question: question)
                            }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(model.selectedRelease.isEmpty && model.selectedSignal.isEmpty)
                            Button(tr("copy pack")) { copy(model.pack.serialized()) }
                                .buttonStyle(TermButtonStyle()).disabled(model.pack.isNull)
                            Button(tr("copy prompt for my AI tool")) { copy(prompt) }
                                .buttonStyle(TermButtonStyle())
                        }
                    }
                }
                Banner(kind: .info, title: "What leaves this Mac",
                       message: tr("Nothing, until you paste the pack or your AI tool asks mpi mcp for it. "
                                   + "Provider tokens are never in a pack: they stay in the Keychain."))
                let p = model.pack
                if p["ok"].bool == true {
                    Panel(title: "Facts") {
                        VStack(alignment: .leading, spacing: 3) {
                            ForEach(Array(p["facts"].array.enumerated()), id: \.offset) { _, f in
                                Text("- " + f.text).font(Term.body).foregroundStyle(Term.ink)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                    if !p["missing_evidence"].array.isEmpty {
                        Panel(title: "Missing evidence") {
                            ForEach(Array(p["missing_evidence"].array.enumerated()), id: \.offset) { _, m in
                                Text("- " + m.text).font(Term.body).foregroundStyle(Term.amber)
                            }
                        }
                    }
                    Text(cat(tr("Redacted: "), p["redacted"].serialized(), " · ", tr("sources: "),
                             "\(p["source_refs"].array.count)"))
                        .font(Term.small).foregroundStyle(Term.dim)
                }
            }
            .padding(16)
        }
    }

    private var prompt: String {
        let scope: String = model.selectedRelease.isEmpty ? cat("signal ", model.selectedSignal)
                                                          : cat("release ", model.selectedRelease)
        let ask: String = question.isEmpty ? "what changed and what needs attention?" : question
        return cat("Using the DevX MCP server (workspace \(model.workspace)), call intelligence_evidence_pack for ",
                   "\(scope) and answer: \(ask) ",
                   "Cite the source_refs, keep exact and candidate links apart, and ask for signal_read, ",
                   "signal_related or code_context_for_signal before guessing.")
    }

    private func copy(_ s: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
        model.message = tr("Copied.")
    }
}

// ---- Integrations ----

private struct IntelIntegrationsView: View {
    @EnvironmentObject var state: AppState
    @ObservedObject var model: IntelligenceModel
    // New / edited workspace.
    @State private var wsId = ""
    @State private var wsName = ""
    @State private var wsRepo = ""
    @State private var wsApps = ""
    @State private var wsEnvs = "production"
    @State private var wsDays = 30
    // New connector.
    @State private var provider = "sentry"
    @State private var connectorId = ""
    @State private var settings: [String: String] = [:]
    @State private var credentialRef = ""
    @State private var secret = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                workspacePanel
                if !model.workspace.isEmpty {
                    connectorsPanel
                    addConnectorPanel
                    retentionPanel
                }
                egressPanel
            }
            .padding(16)
        }
        .onAppear {
            fillFromWorkspace()
            let dir = state.sessionsDir
            model.read({ Core.intelEgress(dir) }) { model.egress = $0 }
        }
        .onChange(of: model.workspace) { _, _ in fillFromWorkspace() }
    }

    private func fillFromWorkspace() {
        guard let w = model.workspaces.first(where: { $0["id"].text == model.workspace }) else { return }
        wsId = w["id"].text
        wsName = w["name"].text
        wsRepo = w["repository_root"].text
        wsApps = w["app_identifiers"].array.map { $0.text }.joined(separator: ",")
        wsEnvs = w["environments"].array.map { $0.text }.joined(separator: ",")
        wsDays = w["retention"]["days"].int ?? 30
    }

    private var workspacePanel: some View {
        Panel(title: "Workspace", subtitle: "A repository and the apps built from it. Ids are lowercase letters, digits, - and _.") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    TextField(tr("id, e.g. superapp"), text: $wsId).textFieldStyle(TermFieldStyle()).frame(maxWidth: 180)
                    TextField(tr("name"), text: $wsName).textFieldStyle(TermFieldStyle())
                }
                HStack {
                    TextField(tr("repository root"), text: $wsRepo).textFieldStyle(TermFieldStyle())
                    Button(tr("choose…")) {
                        let panel = NSOpenPanel()
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        if panel.runModal() == .OK, let url = panel.url { wsRepo = url.path }
                    }
                    .buttonStyle(TermButtonStyle())
                }
                HStack {
                    TextField(tr("app ids, comma-separated"), text: $wsApps).textFieldStyle(TermFieldStyle())
                    TextField(tr("environments"), text: $wsEnvs).textFieldStyle(TermFieldStyle()).frame(maxWidth: 200)
                    Picker(tr("keep"), selection: $wsDays) {
                        Text("7 days").tag(7)
                        Text("30 days").tag(30)
                        Text("90 days").tag(90)
                        Text(tr("forever")).tag(0)
                    }
                    .frame(maxWidth: 170)
                }
                HStack {
                    Button(tr("save workspace")) { saveWorkspace() }
                        .buttonStyle(TermButtonStyle(filled: true)).disabled(wsId.isEmpty)
                    Button(tr("new")) {
                        wsId = ""; wsName = ""; wsRepo = ""; wsApps = ""; wsEnvs = "production"; wsDays = 30
                    }
                    .buttonStyle(TermButtonStyle())
                }
            }
        }
    }

    private func saveWorkspace() {
        let w = JSON.object([
            "id": .string(wsId.trimmingCharacters(in: .whitespaces)),
            "name": .string(wsName),
            "repository_root": .string(wsRepo),
            "app_identifiers": .array(wsApps.split(separator: ",").map {
                .string($0.trimmingCharacters(in: .whitespaces)) }),
            "environments": .array(wsEnvs.split(separator: ",").map {
                .string($0.trimmingCharacters(in: .whitespaces)) }),
            "retention": .object(["days": .number(Double(wsDays))]),
        ])
        let dir = state.sessionsDir, id = wsId
        model.read({ Core.intelSaveWorkspace(dir, w) }) { doc in
            guard doc["ok"].bool == true else { return }
            model.workspace = id
            model.loadWorkspaces(dir)
        }
    }

    private var connectorsPanel: some View {
        let ints = model.integrations
        return Panel(title: "Connectors") {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(ints["connectors"].array.enumerated()), id: \.offset) { _, c in
                    connectorCard(c)
                }
                if ints["connectors"].array.isEmpty {
                    Text(tr("No connectors yet: add one below.")).font(Term.body).foregroundStyle(Term.dim)
                }
            }
        }
    }

    private func connectorCard(_ c: JSON) -> some View {
        let cfg = c["config"], h = c["health"], sync = c["sync"]
        let id = cfg["id"].text
        return VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(id).font(Term.font(13, .bold)).foregroundStyle(Term.ink)
                Text(c["info"]["display_name"].text).font(Term.small).foregroundStyle(Term.dim)
                Chip(text: h["state"].text, tone: stateTone(h["state"].text))
                // Only for a connector that uses one: a file import has
                // nothing stored, and saying "stored" would be untrue.
                if !c["credential_services"].array.isEmpty || !c["credential_env"].text.isEmpty {
                    if c["credential_present"].bool == true {
                        Chip(text: "credential stored", tone: .good)
                    } else if c["info"]["credential_optional"].bool == true {
                        Chip(text: "no credential: public access", tone: .neutral)
                    }
                }
                Spacer()
            }
            if !h["detail"].text.isEmpty {
                Text(h["detail"].text).font(Term.small).foregroundStyle(Term.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if h["state"].text == "needs_auth" && !c["credential_url"].text.isEmpty {
                Button(tr("get a token") + " ↗") { openProviderPage(c["credential_url"].text) }
                    .buttonStyle(TermButtonStyle(tone: Term.amber))
                    .help(c["credential_url"].text)
            }
            Text(cat(tr("last success "), sync["last_success_at"].display("never"), " · ", tr("last attempt "),
                     sync["last_attempt_at"].display("never"), " · ", tr("records "),
                     sync["records_written"].display("0")))
                .font(Term.small).foregroundStyle(Term.dim)
            Text(tr("settings ") + cfg["settings"].serialized()).font(Term.small).foregroundStyle(Term.dim).lineLimit(2)
            HStack {
                Button(tr("validate")) { network(tr("Validating ") + id) { Core.intelValidate($0, $1, id) } }
                Button(tr("discover")) { network(tr("Discovering ") + id) { Core.intelDiscover($0, $1, id) } }
                Button(tr("sync now")) { network(tr("Syncing ") + id) { Core.intelSync($0, $1, id) } }
                Button(cfg["paused"].bool == true ? tr("resume") : tr("pause")) { togglePause(cfg) }
                Button(tr("disconnect")) { remove(id, deleteLocal: false) }
                Button(tr("delete local data")) { remove(id, deleteLocal: true) }
                    .buttonStyle(TermButtonStyle(tone: Term.red))
            }
            .buttonStyle(TermButtonStyle())
        }
        .padding(10).background(Term.raised, in: RoundedRectangle(cornerRadius: 3))
    }

    /// A call that reaches a provider: it blocks, so it runs as an operation
    /// the window's cancel button can stop.
    private func network(_ label: String, _ work: @escaping @Sendable (String, String) -> JSON) {
        let dir = state.sessionsDir, ws = model.workspace
        Core.resetCancel()
        state.beginOperation(label)
        DispatchQueue.global(qos: .userInitiated).async {
            let doc = work(dir, ws)
            DispatchQueue.main.async {
                state.endOperation(label)
                if doc["ok"].bool == false {
                    model.message = doc["error"].text.isEmpty ? doc.serialized() : doc["error"].text
                } else {
                    var parts: [String] = []
                    if !doc["status"].text.isEmpty {
                        parts.append(doc["status"].text + ": \(doc["records_written"].int ?? 0) record(s)")
                    }
                    if !doc["account"].text.isEmpty { parts.append(tr("account ") + doc["account"].text) }
                    if !doc["resources"].array.isEmpty {
                        parts.append(doc["resources"].array.prefix(12).map { $0["id"].text }.joined(separator: ", "))
                    }
                    parts += doc["notes"].array.prefix(4).map { $0.text }
                    model.message = parts.isEmpty ? tr("Done.") : parts.joined(separator: " · ")
                }
                model.reloadAll(dir)
            }
        }
    }

    private func togglePause(_ cfg: JSON) {
        guard case .object(var o) = cfg else { return }
        o["paused"] = .bool(cfg["paused"].bool != true)
        let dir = state.sessionsDir, ws = model.workspace, c = JSON.object(o)
        model.read({ Core.intelSaveConnector(dir, ws, c) }) { _ in model.reloadAll(dir) }
    }

    private func remove(_ id: String, deleteLocal: Bool) {
        let alert = NSAlert()
        alert.messageText = deleteLocal ? tr("Delete \(id) and its local evidence?") : tr("Disconnect \(id)?")
        alert.informativeText = deleteLocal
            ? tr("Its signals and raw evidence are removed from this Mac. This cannot be undone.")
            : tr("Its configuration is removed. Evidence already synced stays on this Mac.")
        alert.addButton(withTitle: deleteLocal ? tr("Delete") : tr("Disconnect"))
        alert.addButton(withTitle: tr("Cancel"))
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let dir = state.sessionsDir, ws = model.workspace
        model.read({ Core.intelRemoveConnector(dir, ws, id, deleteLocal: deleteLocal) }) { _ in model.reloadAll(dir) }
    }

    private var currentProvider: JSON {
        model.providers.first { $0["provider"].text == provider } ?? .null
    }

    private var addConnectorPanel: some View {
        Panel(title: "Add a connector", subtitle: currentProvider["egress"].text) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Picker(tr("provider"), selection: $provider) {
                        ForEach(model.providers.map { $0["provider"].text }, id: \.self) { p in
                            Text(model.providers.first { $0["provider"].text == p }?["display_name"].text ?? p).tag(p)
                        }
                    }
                    .frame(maxWidth: 300)
                    TextField(tr("connector id, e.g. sentry-prod"), text: $connectorId)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 220)
                }
                ForEach(currentProvider["settings"].array.map { $0["name"].text }, id: \.self) { name in
                    HStack {
                        Text(name).font(Term.small).foregroundStyle(Term.dim).frame(width: 150, alignment: .trailing)
                        TextField(currentProvider["settings"].array.first { $0["name"].text == name }?["help"].text ?? "",
                                  text: Binding(get: { settings[name] ?? "" }, set: { settings[name] = $0 }))
                            .textFieldStyle(TermFieldStyle())
                    }
                }
                if !currentProvider["setup_url"].text.isEmpty {
                    HStack {
                        Text("").frame(width: 150)
                        Button(tr("set it up / read how") + " ↗") { openProviderPage(currentProvider["setup_url"].text) }
                            .buttonStyle(TermButtonStyle())
                            .help(currentProvider["setup_url"].text)
                    }
                }
                if !currentProvider["credential_service"].text.isEmpty {
                    let link = tokenURL(currentProvider, baseURL: settings["base_url"] ?? "")
                    HStack {
                        Text(tr("credential")).font(Term.small).foregroundStyle(Term.dim)
                            .frame(width: 150, alignment: .trailing)
                        if !link.isEmpty {
                            // The provider's own page for making a token,
                            // with name and scopes filled in where it allows.
                            Button(tr("get a token") + " ↗") { openProviderPage(link) }
                                .buttonStyle(TermButtonStyle(filled: true))
                                .help(link)
                        }
                        SecureField(currentProvider["credential_help"].text, text: $secret)
                            .textFieldStyle(TermFieldStyle())
                        TextField(tr("Keychain service (optional)"), text: $credentialRef)
                            .textFieldStyle(TermFieldStyle()).frame(maxWidth: 220)
                    }
                    Text(cat(tr("Saved to this connector's own Keychain item and never shown again. "),
                             currentProvider["credential_env"].text,
                             tr(" in the environment is used only while this is the workspace's one connector of its kind.")))
                        .font(Term.small).foregroundStyle(Term.dim)
                }
                Button(tr("add connector")) { addConnector() }
                    .buttonStyle(TermButtonStyle(filled: true)).disabled(connectorId.isEmpty)
            }
        }
    }

    private func addConnector() {
        var s: [String: JSON] = [:]
        for (k, v) in settings where !v.isEmpty { s[k] = .string(v) }
        let id = connectorId
        let c = JSON.object([
            "id": .string(id), "provider": .string(provider), "name": .string(id),
            "settings": .object(s), "credential_ref": .string(credentialRef),
        ])
        // The connector's own Keychain item, so a second Sentry connector
        // never replaces the first one's token (and never sends it to the
        // first one's host). Saved only once the connector itself is.
        let service = credentialRef.isEmpty
            ? currentProvider["credential_service"].text + "." + id : credentialRef
        let token = secret
        let dir = state.sessionsDir, ws = model.workspace
        model.read({ Core.intelSaveConnector(dir, ws, c) }) { doc in
            guard doc["ok"].bool == true else { return }
            if !token.isEmpty && !ConnectorKeychain.save(service: service, value: token) {
                model.message = tr("The connector was saved, but its credential could not be saved to the Keychain.")
            }
            secret = ""
            connectorId = ""
            settings = [:]
            model.reloadAll(dir)
        }
    }

    private var retentionPanel: some View {
        Panel(title: "Retention and storage",
              subtitle: "Pinned signals and releases are kept. Raw evidence nothing retained points at is removed.") {
            VStack(alignment: .leading, spacing: 6) {
                let st = model.integrations["storage"]
                Text(cat(tr("signals "), st["signals"].display("0"), " · ", tr("raw "),
                         formatBytes(st["raw_bytes"].double ?? 0), " · ", tr("total "),
                         formatBytes(st["total_bytes"].double ?? 0), " · ", tr("oldest "),
                         st["oldest_evidence_at"].display("-"), " · ", tr("quarantined "),
                         st["quarantined"].display("0")))
                    .font(Term.small).foregroundStyle(Term.dim)
                HStack {
                    Button(tr("what would be deleted")) { retention(apply: false) }.buttonStyle(TermButtonStyle())
                    Button(tr("apply retention")) { retention(apply: true) }
                        .buttonStyle(TermButtonStyle(tone: Term.amber))
                }
                let r = model.retention
                if r["ok"].bool == true {
                    let verb: String = r["applied"].bool == true ? tr("Deleted: ") : tr("Would delete: ")
                    Text(cat(verb, "\(r["signals_expired"].int ?? 0) ", tr("signal(s)"), ", ",
                             "\(r["raw_deleted"].int ?? 0) ", tr("raw file(s)"), "; ",
                             "\(r["kept_by_pin"].int ?? 0) ", tr("kept by a pin")))
                        .font(Term.body).foregroundStyle(Term.ink)
                }
            }
        }
    }

    private func retention(apply: Bool) {
        let dir = state.sessionsDir, ws = model.workspace
        model.read({ Core.intelRetention(dir, ws, apply: apply) }) { doc in
            model.retention = doc
            if apply { model.reloadAll(dir) }
        }
    }

    private var egressPanel: some View {
        Panel(title: "What leaves this Mac", subtitle: "Connector by connector, and when.") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(model.egress["ledger"].array.enumerated()), id: \.offset) { _, r in
                    VStack(alignment: .leading, spacing: 1) {
                        Text(r["what"].text).font(Term.body).foregroundStyle(Term.ink)
                            .fixedSize(horizontal: false, vertical: true)
                        Text(cat(tr("to "), r["to"].text, " · ", r["when"].text)).font(Term.small).foregroundStyle(Term.dim)
                    }
                }
            }
        }
    }
}
