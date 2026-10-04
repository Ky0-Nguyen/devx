import SwiftUI

/// How the screen the selected app is showing is built: views per screen,
/// nesting depth, hidden and off-screen views, navigation stacks, and React
/// Native screens mounted against showing.
///
/// Everything it shows comes from `mpi_layout_json`; the rules (a physical
/// iOS device is refused, an app is only relaunched when asked) live in the
/// core so the CLI and this view cannot disagree. Relaunching is the one
/// destructive step, so it is confirmed here before it is asked for.
struct LayoutView: View {
    @EnvironmentObject var state: AppState
    @State private var confirmRelaunch = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(tr("How the screen an app is showing is built. Nothing is "
                     + "added to the app: Android is read through dumpsys; an iOS "
                     + "simulator needs the layout probe, loaded by relaunching "
                     + "the app once. A snapshot of structure, not a performance "
                     + "measurement."))
                    .font(Term.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HStack {
                    TextField("package name or bundle id", text: $state.selectedApp)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                    Button(tr("snapshot")) { state.takeLayout(relaunch: false) }
                        .buttonStyle(TermButtonStyle(filled: true))
                        .disabled(!canRun)
                    Button(tr("relaunch with probe")) { confirmRelaunch = true }
                        .buttonStyle(TermButtonStyle(tone: Term.amber))
                        .disabled(!canRun)
                }
                .confirmationDialog(tr("Relaunch the app with the layout probe?"),
                                    isPresented: $confirmRelaunch) {
                    Button(tr("Relaunch"), role: .destructive) {
                        state.takeLayout(relaunch: true)
                    }
                } message: {
                    Text(tr("The app restarts and its current state is lost. iOS "
                         + "simulator only; Android needs no relaunch."))
                }

                if !state.layoutDoc.isNull { result(state.layoutDoc) }
            }
            .padding(16)
        }
        .navigationTitle("~/layout")
    }

    private var canRun: Bool {
        !state.selectedDevice.isEmpty && !state.selectedApp.isEmpty
    }

    @ViewBuilder
    private func result(_ doc: JSON) -> some View {
        let notes = doc["notes"].array.compactMap { $0.string }
        if !notes.isEmpty { BulletList(title: "", items: notes) }
        if !doc["saved"]["id"].text.isEmpty {
            Text(tr("Saved for AI tools:") + " " + doc["saved"]["id"].text
                 + " · read_observation")
                .font(Term.small).foregroundStyle(Term.dim).textSelection(.enabled)
        }

        if doc["ok"].bool != true {
            let failure = doc["failure"].text
            Banner(kind: failure == "probe_not_loaded" ? .caution : .bad,
                   title: failure, message: doc["error"].text)
            if failure == "probe_not_loaded" {
                Button(tr("relaunch with probe")) { confirmRelaunch = true }
                    .buttonStyle(TermButtonStyle(tone: Term.amber, filled: true))
            }
        } else {
            report(doc["report"])
        }
    }

    @ViewBuilder
    private func report(_ r: JSON) -> some View {
        let totals = r["totals"]
        Banner(kind: .info, title: r["source"].text,
               message: r["basis"].text + ". " + r["not_a_measurement"].text)

        Panel(title: "Layout", subtitle: r["app_identifier"].text) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 18) {
                    stat(totals["views"].int ?? 0, tr("views"))
                    stat(totals["visible_views"].int ?? 0, tr("visible"))
                    stat(totals["hidden_views"].int ?? 0, tr("hidden"))
                    stat(totals["offscreen_views"].int ?? 0, tr("off screen"))
                    stat(totals["max_depth"].int ?? 0, tr("deepest"))
                    stat(totals["windows"].int ?? 0, tr("windows"))
                }
                if !r["activity"].text.isEmpty {
                    Text("activity " + r["activity"].text)
                        .font(Term.small).foregroundStyle(Term.dim)
                }
                if totals["truncated"].bool == true {
                    Chip(text: "truncated", tone: .caution)
                }
            }
        }

        let screens = r["screens"].array
        Panel(title: tr("Screens") + " (\(screens.count))") {
            VStack(alignment: .leading, spacing: 10) {
                if screens.isEmpty {
                    Text(tr("No screen could be identified."))
                        .font(Term.body).foregroundStyle(Term.dim)
                }
                ForEach(Array(screens.enumerated()), id: \.offset) { i, sc in
                    screenRow(index: i, sc)
                }
            }
        }

        let navigation = r["navigation"].array
        if !navigation.isEmpty {
            Panel(title: tr("Navigation")) {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(Array(navigation.enumerated()), id: \.offset) { _, nav in
                        navigationRow(nav)
                    }
                }
            }
        }

        let rn = r["react_native"]
        Panel(title: "React Native") {
            if rn["detected"].bool == true {
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 18) {
                        stat(rn["screens_mounted"].int ?? 0, tr("screens mounted"))
                        stat(rn["screens_on_screen"].int ?? 0, tr("showing"))
                        stat(rn["host_views"].int ?? 0, tr("host views"))
                    }
                    Text("architecture " + rn["architecture"].text)
                        .font(Term.small).foregroundStyle(Term.dim)
                }
            } else {
                Text(tr("Not detected: no React Native host views in the tree."))
                    .font(Term.body).foregroundStyle(Term.dim)
            }
        }

        let observations = r["observations"].array.compactMap { $0["message"].string }
        Panel(title: tr("Observations")) {
            if observations.isEmpty {
                Text(tr("None past the thresholds."))
                    .font(Term.body).foregroundStyle(Term.dim)
            } else {
                BulletList(title: "", items: observations)
            }
        }

        let warnings = r["warnings"].array.compactMap { $0.string }
        if !warnings.isEmpty {
            BulletList(title: "warnings", items: warnings)
        }
    }

    private func stat(_ value: Int, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text("\(value)").font(Term.font(18, .bold)).foregroundStyle(Term.ink)
            Text(label).font(Term.small).foregroundStyle(Term.dim)
        }
    }

    private func screenRow(index: Int, _ sc: JSON) -> some View {
        let shown = sc["on_screen"].bool == true
        let top = sc["top_classes"].array.prefix(5).map {
            "\($0["class"].text) \($0["count"].int ?? 0)"
        }.joined(separator: " · ")
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text("#\(index)").font(Term.small).foregroundStyle(Term.dim)
                Text(sc["name"].text).font(Term.font(12, .bold)).foregroundStyle(Term.ink)
                    .lineLimit(1).truncationMode(.middle)
                Chip(text: shown ? "on screen" : "not shown", tone: shown ? .good : .neutral)
                Chip(text: sc["basis"].text)
                if let inside = sc["inside"].int {
                    Text("inside #\(inside)").font(Term.small).foregroundStyle(Term.dim)
                }
            }
            Text("\(sc["views"].int ?? 0) views · \(sc["visible_views"].int ?? 0) visible · "
                 + "\(sc["hidden_views"].int ?? 0) hidden · "
                 + "\(sc["offscreen_views"].int ?? 0) off screen · depth "
                 + "\(sc["depth"].int ?? 0) · \(sc["single_child_wrappers"].int ?? 0) wrappers")
                .font(Term.body).foregroundStyle(Term.ink)
            if !top.isEmpty {
                Text(top).font(Term.small).foregroundStyle(Term.dim)
                    .lineLimit(2).fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func navigationRow(_ nav: JSON) -> some View {
        let screens = nav["screens"].array.compactMap { $0.string }
        let isStack = nav["kind"].text == "navigation_controller"
        return VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Text(nav["container"].text).font(Term.font(12, .bold))
                    .foregroundStyle(Term.ink).lineLimit(1).truncationMode(.middle)
                Chip(text: nav["kind"].text)
                Text("\(screens.count)").font(Term.small).foregroundStyle(Term.dim)
            }
            ForEach(Array(screens.enumerated()), id: \.offset) { i, name in
                Text("\(i + 1). \(name)"
                     + (isStack && i == screens.count - 1 ? "  ← top" : ""))
                    .font(Term.small).foregroundStyle(Term.dim)
                    .lineLimit(1).truncationMode(.middle)
            }
        }
    }
}
