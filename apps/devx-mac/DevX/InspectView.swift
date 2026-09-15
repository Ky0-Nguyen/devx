// The Inspect tab: what the app is asking for and saying, read without adding
// anything to it.
//
// The view's job here is unusually specific. The data is genuinely useful --
// a list of the app's HTTP calls and console output, live, with no SDK -- and
// it is also easy to over-read. Three things are therefore always on screen
// and not behind a disclosure: what was attached to, what each empty list
// means, and the fact that a debugger was attached while this was recorded.
import SwiftUI

/// Headers and bodies for one exchange.
///
/// Collapsed by default even when captured: a request with twenty headers
/// would bury the next request, and the list is the thing being scanned.
private struct ExchangeDetail: View {
    let row: JSON
    @State private var open = false

    var body: some View {
        let reqH = row["request_headers"]
        let resH = row["response_headers"]
        let hasBody = !row["response_body"].text.isEmpty
            || !row["request_body"].text.isEmpty
        let count = reqH.keys.count + resH.keys.count
        if count > 0 || hasBody {
            VStack(alignment: .leading, spacing: 2) {
                Button(open ? tr("hide detail")
                            : tr("detail") + " (\(count) " + tr("headers") + ")") {
                    open.toggle()
                }
                .buttonStyle(TermButtonStyle())
                if open {
                    ForEach(reqH.keys, id: \.self) { k in
                        Text("> \(k): " + reqH[k].text)
                            .font(Term.micro).foregroundStyle(Term.dim)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if !row["request_body"].text.isEmpty {
                        Text("> " + row["request_body"].text)
                            .font(Term.micro).foregroundStyle(Term.ink)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(resH.keys, id: \.self) { k in
                        Text("< \(k): " + resH[k].text)
                            .font(Term.micro).foregroundStyle(Term.dim)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if !row["response_body"].text.isEmpty {
                        // The encoding matters: text shown as base64 is
                        // unreadable, and base64 shown as text is nonsense.
                        let b64 = row["response_body_base64"].bool == true
                        Text("< " + (b64 ? tr("body (base64)") + " " : "")
                             + row["response_body"].text)
                            .font(Term.micro).foregroundStyle(Term.ink)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    } else if !row["response_body_unavailable"].text.isEmpty {
                        Text("< " + row["response_body_unavailable"].text)
                            .font(Term.micro).foregroundStyle(Term.cyan)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .padding(.leading, 60)
        }
    }
}

/// A pulsing dot, so "watching" is visible without reading a label.
private struct LiveDot: View {
    @State private var bright = false
    var body: some View {
        Circle()
            .fill(Term.green)
            .frame(width: 6, height: 6)
            .opacity(bright ? 1.0 : 0.25)
            .animation(.easeInOut(duration: 0.7).repeatForever(autoreverses: true),
                       value: bright)
            .onAppear { bright = true }
    }
}

struct InspectView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(tr("A React Native debug build already runs an inspector "
                      + "and already connects itself to Metro. This reads that "
                      + "connection, so nothing is added to the app: no "
                      + "dependency, no import, no rebuild. A release build "
                      + "runs no inspector, and an empty capture there means "
                      + "there was nothing to attach to."))
                    .font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)

                targetsPanel
                configPanel

                if !state.inspectDoc.isNull {
                    if state.inspectDoc["debugger_attached"].bool == false {
                        Banner(kind: .caution,
                               title: "Nothing was attached",
                               message: tr("Nothing below is a statement "
                                         + "about the app."))
                    }
                    sourcesPanel
                    filterBar
                    networkPanel
                    consolePanel
                    if state.inspectRedux { reduxPanel }
                    if state.inspectReduxWatch { reduxActivityPanel }
                    if !state.inspectDoc["screenshots"].array.isEmpty {
                        screenshotsPanel
                    }
                    caveatsPanel
                }
            }
            .padding(14)
        }
        .navigationTitle("~/inspect")
        .onAppear { state.loadInspectTargetsIfNeeded() }
    }

    @ViewBuilder private var targetsPanel: some View {
        Panel(title: "Attachable now",
              subtitle: "listed by Metro; a debug build appears here by itself") {
            VStack(alignment: .leading, spacing: 6) {
                let doc = state.inspectTargetsDoc
                if doc.isNull {
                    Text(tr("not looked yet")).font(Term.small)
                        .foregroundStyle(Term.cyan)
                } else if doc["metro_reachable"].bool != true {
                    Banner(kind: .bad, title: "Metro is not answering",
                           message: doc["error"].text.isEmpty
                               ? tr("This says nothing about the app.")
                               : doc["error"].text)
                } else if doc["targets"].array.isEmpty {
                    Banner(kind: .info, title: "No app is attached",
                           message: tr("Metro is running and nothing is "
                                     + "attached to its inspector. A debug "
                                     + "build connects itself; a release "
                                     + "build has no inspector to connect."))
                } else {
                    ForEach(Array(doc["targets"].array.enumerated()),
                            id: \.offset) { _, t in
                        HStack(spacing: 10) {
                            Text(t["app_id"].text).font(Term.body)
                            Chip(text: t["device_name"].text, tone: .neutral)
                            Text(t["description"].text).font(Term.micro)
                                .foregroundStyle(Term.dim)
                            Spacer(minLength: 0)
                            Button(tr("use")) {
                                state.selectedApp = t["app_id"].text
                                // Pin the device as well. Choosing a row and
                                // then being observed on a different device
                                // is the bug this button exists to avoid.
                                state.inspectTargetDevice = t["device_name"].text
                            }
                            .buttonStyle(TermButtonStyle())
                        }
                    }
                }
                HStack {
                    Button(tr("Refresh")) { state.loadInspectTargets() }
                        .buttonStyle(TermButtonStyle())
                    Spacer(minLength: 0)
                }
            }
        }
    }

    @ViewBuilder private var configPanel: some View {
        Panel(title: "Observe",
              subtitle: "a debugger session, not a measurement") {
            VStack(alignment: .leading, spacing: 8) {
                Field(label: "app") {
                    TextField("package name or bundle id", text: $state.selectedApp)
                        .textFieldStyle(TermFieldStyle()).frame(maxWidth: 320)
                }
                Field(label: "device") {
                    HStack(spacing: 8) {
                        TextField(tr("any attached device"),
                                  text: $state.inspectTargetDevice)
                            .textFieldStyle(TermFieldStyle()).frame(maxWidth: 240)
                        if !state.inspectTargetDevice.isEmpty {
                            Button(tr("any")) { state.inspectTargetDevice = "" }
                                .buttonStyle(TermButtonStyle())
                        }
                    }
                }
                Field(label: "seconds") {
                    Stepper(value: $state.inspectSeconds, in: 1...300, step: 5) {
                        Text("\(state.inspectSeconds) s").font(Term.body)
                    }.frame(width: 150)
                }
                Toggle(tr("read the Redux store"), isOn: $state.inspectRedux)
                    .toggleStyle(.checkbox).font(Term.body)
                if state.inspectRedux {
                    Toggle(tr("include its values"), isOn: $state.inspectReduxValues)
                        .toggleStyle(.checkbox).font(Term.small)
                        .padding(.leading, 18)
                    Text(tr("a store holds tokens and personal data; the "
                          + "values are left out unless asked for"))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .padding(.leading, 18)
                    Toggle(tr("watch it change, not just read it once"),
                           isOn: $state.inspectReduxWatch)
                        .toggleStyle(.checkbox).font(Term.small)
                        .padding(.leading, 18)
                    if state.inspectReduxWatch {
                        Toggle(tr("also name the actions"),
                               isOn: $state.inspectReduxActions)
                            .toggleStyle(.checkbox).font(Term.small)
                            .padding(.leading, 36)
                        // The one setting here that changes the running app,
                        // so it says so where it is switched on rather than
                        // only in the report afterwards.
                        Text(state.inspectReduxActions
                             ? tr("this wraps dispatch inside the running app "
                                + "for the duration and puts it back "
                                + "afterwards -- the only setting here that "
                                + "modifies the app")
                             : tr("without it, changes are seen through "
                                + "store.subscribe, which names no action"))
                            .font(Term.micro)
                            .foregroundStyle(state.inspectReduxActions
                                             ? Term.amber : Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(.leading, 36)
                    }
                }
                Toggle(tr("capture headers and response bodies"),
                       isOn: $state.inspectDetail)
                    .toggleStyle(.checkbox).font(Term.body)
                if state.inspectDetail {
                    Text(tr("this is the data in flight, including "
                          + "Authorization headers and whatever a login "
                          + "returns; it is captured verbatim"))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .padding(.leading, 18)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Toggle(tr("screenshot the screen before and after"),
                       isOn: $state.inspectScreenshots)
                    .toggleStyle(.checkbox).font(Term.body)
                if state.inspectScreenshots && state.selectedDevice.isEmpty {
                    Text(tr("needs a device: Metro knows the app but not "
                          + "which device it is on"))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .padding(.leading, 18)
                }
                HStack(spacing: 10) {
                    // The live one first: it is what someone wants when they
                    // are about to tap around in the app.
                    if state.inspectStreaming {
                        Button(tr("stop watching")) { state.stopInspectStream() }
                            .buttonStyle(TermButtonStyle(tone: Term.amber,
                                                         filled: true))
                    } else {
                        Button(tr("watch live")) { state.startInspectStream() }
                            .buttonStyle(TermButtonStyle(filled: true))
                            .disabled(state.selectedApp.isEmpty)
                    }
                    Button(state.busy == nil
                            ? tr("observe for \(state.inspectSeconds)s")
                            : tr("observing…")) {
                        state.runInspect()
                    }
                    .buttonStyle(TermButtonStyle())
                    .disabled(state.busy != nil || state.selectedApp.isEmpty
                              || state.inspectStreaming)
                    if state.inspectStreaming {
                        LiveDot()
                        Text(tr("watching — act in the app and calls appear "
                              + "here"))
                            .font(Term.micro).foregroundStyle(Term.green)
                    }
                    Spacer(minLength: 0)
                }
                if !state.inspectDisconnect.isEmpty {
                    Banner(kind: .caution,
                           title: "The observation ended early",
                           message: state.inspectDisconnect)
                }
            }
        }
    }

    /// Kind and text filters.
    ///
    /// The kinds are the three things asked for -- API, Redux, Log -- and
    /// they are toggles rather than a segmented control because watching API
    /// calls *and* Redux actions together is the common case.
    @ViewBuilder private var filterBar: some View {
        Panel(title: "Filter", subtitle: "what to show, not what was captured") {
            HStack(spacing: 14) {
                ForEach(InspectKind.allCases) { kind in
                    Toggle(kind.label, isOn: Binding(
                        get: { state.inspectKinds.contains(kind) },
                        set: { on in
                            if on { state.inspectKinds.insert(kind) }
                            else { state.inspectKinds.remove(kind) }
                        }))
                        .toggleStyle(.checkbox).font(Term.body)
                }
                TextField(tr("url, action, status…"), text: $state.inspectNeedle)
                    .textFieldStyle(TermFieldStyle()).frame(maxWidth: 260)
                if !state.inspectNeedle.isEmpty
                    || state.inspectKinds.count != InspectKind.allCases.count {
                    Button(tr("clear")) {
                        state.inspectNeedle = ""
                        state.inspectKinds = Set(InspectKind.allCases)
                    }
                    .buttonStyle(TermButtonStyle())
                }
                Spacer(minLength: 0)
            }
        }
    }

    @ViewBuilder private var sourcesPanel: some View {
        Panel(title: "Sources") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(state.inspectDoc["sources"].array.enumerated()),
                        id: \.offset) { _, s in
                    let st = s["state"].text
                    HStack(alignment: .top, spacing: 8) {
                        // "ran and saw nothing" is a statement about the app;
                        // "unavailable" is a statement about us. They are
                        // never the same colour.
                        Chip(text: st,
                             tone: st == "attached" ? .good
                                 : st == "ran_saw_nothing" ? .neutral : .bad)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(s["name"].text).font(Term.body)
                            Text(s["detail"].text).font(Term.micro)
                                .foregroundStyle(Term.dim)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer(minLength: 0)
                    }
                }
            }
        }
    }

    @ViewBuilder private var networkPanel: some View {
        let captured = state.inspectDoc["network"].array
        let all = InspectFilter.afterClear(captured,
                                           clearedCount: state.clearedNetwork)
        let result = InspectFilter.network(all, kinds: state.inspectKinds,
                                           needle: state.inspectNeedle)
        let rows = result.shown
        Panel(title: tr("Network") + " (\(rows.count)"
                     + (result.hidden > 0 ? " / \(all.count)" : "") + ")",
              subtitle: "the JavaScript side only") {
            ClearBar(kind: .network, held: captured.count - all.count)
            // An empty list because of a filter is a statement about the
            // filter. An empty list with nothing hidden is a statement about
            // the app. They must not look the same.
            if result.hidEverything {
                Text(tr("\(result.hidden) request(s) are hidden by the "
                      + "filter. This says nothing about the app."))
                    .font(Term.small).foregroundStyle(Term.cyan)
                    .fixedSize(horizontal: false, vertical: true)
            } else if rows.isEmpty {
                Text(tr("Nothing was reported. The domain was enabled, so "
                      + "this is the app making no JavaScript HTTP calls in "
                      + "the window. A WebView's requests never appear here "
                      + "— which covers most SSO and payment screens — and "
                      + "neither do a native module's."))
                    .font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(rows.enumerated()), id: \.offset) { _, r in
                        HStack(spacing: 8) {
                            Text(r["method"].text).font(Term.small)
                                .frame(width: 52, alignment: .leading)
                            // A status nobody sent renders as a dash. The
                            // accessors are optional for exactly this
                            // reason -- an absent status coerced to 0 would
                            // show as a failed request.
                            let status = r["status"].int
                            Chip(text: status.map(String.init)
                                     ?? (r["failed"].bool == true ? "fail" : "—"),
                                 tone: status == nil ? .neutral
                                     : (status! >= 400 ? .bad : .good))
                            Text(r["duration_ms"].double
                                    .map { String(format: "%.0f ms", $0) } ?? "—")
                                .font(Term.small).foregroundStyle(Term.dim)
                                .frame(width: 70, alignment: .trailing)
                            Text(r["url"].text).font(Term.small)
                                .lineLimit(1).truncationMode(.head)
                            Spacer(minLength: 0)
                        }
                        if r["incomplete"].bool == true {
                            Text(tr("still in flight when the window closed: "
                                  + "evidence of the request, none of its "
                                  + "outcome"))
                                .font(Term.micro).foregroundStyle(Term.cyan)
                                .padding(.leading, 60)
                        }
                        if state.inspectDetail { ExchangeDetail(row: r) }
                    }
                }
            }
        }
    }

    @ViewBuilder private var consolePanel: some View {
        let captured = state.inspectDoc["console"].array
        let all = InspectFilter.afterClear(captured,
                                           clearedCount: state.clearedConsole)
        let result = InspectFilter.console(all, kinds: state.inspectKinds,
                                           needle: state.inspectNeedle)
        let rows = result.shown
        Panel(title: tr("Console") + " (\(rows.count)"
                     + (result.hidden > 0 ? " / \(all.count)" : "") + ")",
              subtitle: "whatever the app chose to log") {
            ClearBar(kind: .log, held: captured.count - all.count)
            if result.hidEverything {
                Text(tr("\(result.hidden) line(s) are hidden by the filter. "
                      + "This says nothing about the app."))
                    .font(Term.small).foregroundStyle(Term.cyan)
                    .fixedSize(horizontal: false, vertical: true)
            } else if rows.isEmpty {
                Text(tr("The app logged nothing in this window."))
                    .font(Term.small).foregroundStyle(Term.dim)
            } else {
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(Array(rows.enumerated()), id: \.offset) { _, r in
                        let level = r["level"].text
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(alignment: .top, spacing: 8) {
                                Chip(text: level.isEmpty ? "log" : level,
                                     tone: level == "error" ? .bad
                                         : level == "warning" ? .caution
                                         : .neutral)
                                Text(r["text"].text).font(Term.small)
                                    .fixedSize(horizontal: false, vertical: true)
                                Spacer(minLength: 0)
                            }
                            if !r["inferred_redux_action_type"].text.isEmpty {
                                Text(tr("inferred Redux action:") + " "
                                     + r["inferred_redux_action_type"].text
                                     + " — " + r["inference_basis"].text)
                                    .font(Term.micro).foregroundStyle(Term.cyan)
                                    .padding(.leading, 60)
                            }
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder private var reduxPanel: some View {
        let st = state.inspectDoc["redux_state"]
        Panel(title: "Redux state",
              subtitle: "state, not actions") {
            VStack(alignment: .leading, spacing: 6) {
                Text(st["basis"].text).font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
                if st["found"].bool == true {
                    let slices = st["slice_names"].array.map { $0.text }
                    Text("\(slices.count) " + tr("slice(s)")).font(Term.body)
                    Text(slices.joined(separator: ", "))
                        .font(Term.micro).foregroundStyle(Term.ink.opacity(0.8))
                        .fixedSize(horizontal: false, vertical: true)
                }
                if !st["note"].text.isEmpty {
                    Text(st["note"].text).font(Term.micro)
                        .foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    /// What happened to the store, one row per change.
    ///
    /// Collapsed by default, and that is the point: a single dispatch on a
    /// real app cleared a profile slice and produced thirty deltas, which
    /// printed inline would push everything else off the screen. The row says
    /// what happened; the deltas are there when the answer is "what exactly
    /// did it change".
    @ViewBuilder private var reduxActivityPanel: some View {
        let rx = state.inspectDoc["redux"]
        let captured = rx["records"].array
        let records = InspectFilter.afterClear(
            reduxRecords: captured, clearedSeq: state.clearedReduxSeq)
        Panel(title: "Redux activity",
              subtitle: rx["dispatch_wrapped"].bool == true
                        ? "dispatch is wrapped; it is put back when this stops"
                        : "state changes, read-only") {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 10) {
                    Text("\(records.count) " + tr("record(s)"))
                        .font(Term.body)
                    Spacer(minLength: 0)
                    if !records.isEmpty {
                        // One control for the whole list. Opening thirty rows
                        // one at a time to find a field is not reading, it is
                        // clicking.
                        Button(state.reduxExpandAll
                               ? tr("collapse all") : tr("expand all")) {
                            state.reduxExpandAll.toggle()
                            state.reduxOpenRecords.removeAll()
                        }
                        .buttonStyle(TermButtonStyle())
                    }
                }
                ClearBar(kind: .redux, held: captured.count - records.count)
                if let dropped = rx["dropped"].int, dropped > 0 {
                    Text(DeviceFreshness.fill(
                            tr("{n} record(s) were dropped by the in-app "
                             + "buffer: this list is the tail, not the whole "
                             + "capture"), "{n}", dropped))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if !rx["restore_error"].text.isEmpty {
                    Text(rx["restore_error"].text)
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if records.isEmpty && rx["store_found"].bool == true {
                    // Not the same answer as a missing store.
                    Text(tr("the store was found and nothing has dispatched "
                          + "yet"))
                        .font(Term.small).foregroundStyle(Term.dim)
                }
                ForEach(Array(records.enumerated()), id: \.offset) { _, rec in
                    ReduxRecordRow(record: rec)
                }
                if !rx["note"].text.isEmpty {
                    Text(rx["note"].text).font(Term.micro)
                        .foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    @ViewBuilder private var screenshotsPanel: some View {
        Panel(title: tr("Screenshots"),
              subtitle: "each shows the moment it was taken, and nothing else") {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(state.inspectDoc["screenshots"].array.enumerated()),
                        id: \.offset) { _, shot in
                    if shot["captured"].bool == true {
                        HStack(alignment: .top, spacing: 10) {
                            // Loaded from disk by path: the core wrote the
                            // PNG, and the view does not re-encode it.
                            if let img = NSImage(contentsOfFile: shot["path"].text) {
                                Image(nsImage: img)
                                    .resizable().scaledToFit()
                                    .frame(maxWidth: 150, maxHeight: 300)
                                    .border(Term.line)
                            }
                            VStack(alignment: .leading, spacing: 3) {
                                Chip(text: shot["moment"].text, tone: .neutral)
                                Text(shot["shows"].text).font(Term.micro)
                                    .foregroundStyle(Term.dim)
                                    .fixedSize(horizontal: false, vertical: true)
                                let w = shot["width"].int
                                let h = shot["height"].int
                                Text((w != nil && h != nil
                                        ? "\(w!)x\(h!)" : tr("size unknown"))
                                     + "  ·  " + shot["basis"].text)
                                    .font(Term.micro).foregroundStyle(Term.dim)
                            }
                            Spacer(minLength: 0)
                        }
                    } else {
                        Banner(kind: .caution, title: "A screenshot was not taken",
                               message: shot["error"].text)
                    }
                }
            }
        }
    }

    @ViewBuilder private var caveatsPanel: some View {
        Panel(title: "What this does not show") {
            BulletList(title: "",
                       items: state.inspectDoc["caveats"].array.map { $0.text })
        }
    }
}

/// One change to the store: a heading that fits on a line, and the detail
/// underneath when asked for.
private struct ReduxRecordRow: View {
    @EnvironmentObject var state: AppState
    let record: JSON

    private var isOpen: Bool {
        let seq = record["seq"].int ?? 0
        // Expand-all sets the baseline and the set holds the exceptions, so
        // one row can still be closed while everything else is open.
        return state.reduxExpandAll != state.reduxOpenRecords.contains(seq)
    }

    var body: some View {
        let deltas = record["deltas"].array
        let slices = record["changed_slices"].array.map { $0.text }
        VStack(alignment: .leading, spacing: 3) {
            Button {
                let seq = record["seq"].int ?? 0
                if state.reduxOpenRecords.contains(seq) {
                    state.reduxOpenRecords.remove(seq)
                } else {
                    state.reduxOpenRecords.insert(seq)
                }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: isOpen ? "chevron.down" : "chevron.right")
                        .font(Term.micro).foregroundStyle(Term.dim)
                    if let type = record["action_type"].string {
                        // Printed whole. An action type is identified by its
                        // head, so cutting the end of it loses the name.
                        Text(type).font(Term.font(11, .medium))
                            .foregroundStyle(Term.cyan)
                    } else if record["dispatch_bypassed"].bool == true {
                        Text(tr("no action named"))
                            .font(Term.font(11, .medium))
                            .foregroundStyle(Term.dim)
                    } else {
                        Text(tr("state change")).font(Term.font(11, .medium))
                            .foregroundStyle(Term.dim)
                    }
                    Text(slices.prefix(3).joined(separator: ", ")
                         + (slices.count > 3
                            ? " +\(slices.count - 3)" : ""))
                        .font(Term.micro).foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                    if !deltas.isEmpty {
                        Text("\(deltas.count) Δ").font(Term.micro)
                            .foregroundStyle(Term.dim)
                    } else if record["equal_replacement"].bool == true {
                        // Visible while collapsed: this is the row worth
                        // finding, and requiring a click to see it would hide
                        // the one finding the list can offer on its own.
                        Text(tr("no-op")).font(Term.micro)
                            .foregroundStyle(Term.amber)
                    }
                }
            }
            .buttonStyle(.plain)

            if isOpen {
                VStack(alignment: .leading, spacing: 2) {
                    if record["dispatch_bypassed"].bool == true {
                        Text(tr("dispatched through a reference the wrapper "
                              + "does not sit on -- a thunk is handed one -- "
                              + "so the change is real and the action is not "
                              + "named"))
                            .font(Term.micro).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if slices.count > 3 {
                        Text(tr("slices:") + " " + slices.joined(separator: ", "))
                            .font(Term.micro).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(Array(deltas.enumerated()), id: \.offset) { _, d in
                        HStack(alignment: .top, spacing: 6) {
                            Text(mark(d["kind"].text))
                                .font(Term.micro)
                                .foregroundStyle(tone(d["kind"].text))
                                .frame(width: 8)
                            Text(d["path"].text).font(Term.micro)
                            if d["before"].string != nil || d["after"].string != nil {
                                Text((d["before"].string ?? tr("(absent)"))
                                     + "  ->  "
                                     + (d["after"].string ?? tr("(absent)")))
                                    .font(Term.micro).foregroundStyle(.secondary)
                                    .textSelection(.enabled)
                            }
                            Spacer(minLength: 0)
                        }
                    }
                    if let payload = record["action_payload"].string {
                        Text(tr("payload:") + " " + payload)
                            .font(Term.micro).foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if !record["truncated"].text.isEmpty {
                        Text(record["truncated"].text)
                            .font(Term.micro).foregroundStyle(Term.amber)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if record["equal_replacement"].bool == true {
                        // Not an empty row: the slice came back as a new
                        // object holding the same values, so everything
                        // watching it re-rendered for nothing.
                        Text(tr("the slice was replaced with an equal value: "
                              + "subscribers re-rendered and nothing changed"))
                            .font(Term.micro).foregroundStyle(Term.amber)
                            .fixedSize(horizontal: false, vertical: true)
                    } else if deltas.isEmpty && !state.inspectReduxValues {
                        Text(tr("values were not captured, so the slice names "
                              + "above are the whole finding"))
                            .font(Term.micro).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.leading, 16)
            }
        }
        .padding(.vertical, 2)
    }

    private func mark(_ kind: String) -> String {
        switch kind {
        case "added": return "+"
        case "removed": return "-"
        default: return "~"
        }
    }

    private func tone(_ kind: String) -> Color {
        switch kind {
        case "added": return Term.green
        case "removed": return Term.amber
        default: return Term.cyan
        }
    }
}

/// The `clear` control for one list, plus what the clear is holding.
///
/// Shown inside the list it acts on rather than as one global control: the
/// three lists answer different questions, and a reason to clear the console
/// is rarely a reason to throw away the API calls next to it.
///
/// The count and the way back are not optional extras. A clear that silently
/// hid rows would turn "what is on screen" into a claim about the app, which
/// is the one thing every other filter here is careful not to do.
private struct ClearBar: View {
    @EnvironmentObject var state: AppState
    let kind: InspectKind
    /// How many rows the clear is currently holding back.
    let held: Int

    var body: some View {
        HStack(spacing: 8) {
            Button(tr("clear")) { state.clearInspect(kind) }
                .buttonStyle(TermButtonStyle())
                .disabled(held == 0 && nothingToClear)
            if held > 0 {
                Text(DeviceFreshness.fill(
                        tr("{n} held back by clear — still captured, still "
                         + "exported"), "{n}", held))
                    .font(Term.micro).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
                Button(tr("show them")) { state.unclearInspect(kind) }
                    .buttonStyle(TermButtonStyle())
            }
            Spacer(minLength: 0)
        }
    }

    /// Nothing captured yet, so there is nothing a clear could do.
    private var nothingToClear: Bool {
        switch kind {
        case .network: return state.inspectDoc["network"].array.isEmpty
        case .log: return state.inspectDoc["console"].array.isEmpty
        case .redux: return state.inspectDoc["redux"]["records"].array.isEmpty
        }
    }
}
