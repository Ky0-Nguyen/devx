// The Inspect tab: what the app is asking for and saying, read without adding
// anything to it.
//
// The view's job here is unusually specific. The data is genuinely useful --
// a list of the app's HTTP calls and console output, live, with no SDK -- and
// it is also easy to over-read. Three things are therefore always on screen
// and not behind a disclosure: what was attached to, what each empty list
// means, and the fact that a debugger was attached while this was recorded.
import SwiftUI

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
        // The split is the ROOT of the pane, as in IssuesView, and the two
        // scrollers are siblings -- neither is inside the other. A ScrollView
        // nested in a scrolling parent is handed no height to resolve
        // against, so it resolves to its content's height and reports THAT
        // outward, which is how a 2000-point list ended up stretching a
        // window (the same failure is recorded at Views.swift:420 and
        // TimelineView.swift:76).
        //
        // Both children must be told to fill, and so must the HSplitView: it
        // sizes to its children's ideal height otherwise, which collapsed a
        // pane into a band floating mid-window (IssuesView.swift:20).
        HSplitView {
            activityColumn
                .frame(minWidth: 420, idealWidth: 480, maxHeight: .infinity)
            exchangeColumn
                .frame(minWidth: 360, idealWidth: 370, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle("~/inspect")
        .onAppear { state.loadInspectTargetsIfNeeded() }
    }

    private var activityColumn: some View {
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
    }

    // MARK: - The detail column

    /// One request, read at length, in its own scroller.
    ///
    /// Stacked named sections rather than a DevTools tab strip. A tab is
    /// always visible whether or not it has anything behind it, so an empty
    /// "Timing" tab reads as "this request had no timing" when the truth is
    /// that nothing here measured it -- the distinction the core's SourceState
    /// exists to keep. A section that cannot be filled is not rendered, and
    /// what this pane cannot show is stated once, on every selection, so its
    /// absence can never be read as a claim either.
    private var exchangeColumn: some View {
        let captured = state.inspectDoc["network"].array
        let afterClear = InspectFilter.afterClear(
            captured, clearedCount: state.clearedNetwork)
        let shown = InspectFilter.network(afterClear, kinds: state.inspectKinds,
                                          needle: state.inspectNeedle).shown
        let pane = InspectSelection.resolve(selectedId: state.selectedRequestId,
                                            captured: captured,
                                            afterClear: afterClear,
                                            shown: shown)
        return ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                switch pane {
                case .noDocument:
                    TermEmpty(title: "no request selected",
                              detail: tr("Observe or watch an app on the left."),
                              hint: "mpi inspect --detail")
                case .nothingSelected:
                    TermEmpty(title: "no request selected",
                              detail: detailInvitation,
                              hint: "mpi inspect --detail")
                case .gone:
                    TermEmpty(title: "that request is no longer in the list",
                              detail: tr("The list on the left is the current "
                                       + "one."),
                              hint: "")
                case .exchange(let row, let visibility, let index):
                    ExchangeColumn(row: row, visibility: visibility,
                                   index: index, capture: captureState)
                }
            }
            .padding(14)
        }
    }

    /// What clicking a row will actually get you, which depends on whether
    /// detail was captured. Promising headers and a body when the toggle was
    /// off would be an invitation to a sentence saying there is nothing here.
    private var detailInvitation: String {
        captureState == .on
            ? tr("Pick a request on the left to read its headers and body.")
            : tr("Pick a request on the left.")
    }

    /// Whether headers and bodies were captured for the observation on
    /// screen.
    ///
    /// From what was recorded when that observation started, not from the
    /// live toggle -- the toggle governs the next capture, and reading it
    /// here made the pane claim that bodies it was displaying had never been
    /// captured as soon as someone switched it off.
    private var captureState: DetailCapture {
        switch state.inspectDocCapturedDetail {
        case .some(true): return .on
        case .some(false): return .off
        case nil: return .unknown
        }
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
                // Capped, with its own scroller. The ask was "maxheight cho
                // session network ... cần thì scroll trong mỗi session" -- fit
                // one screen, scroll inside a section rather than growing the
                // page. Nothing is hidden by the cap: everything above it is
                // reachable by scrolling, and the Panel title already carries
                // the count, so the height says nothing about how many there
                // are.
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(LogOrder.newestFirst(rows).enumerated()),
                                id: \.offset) { _, r in
                            NetworkRow(row: r,
                                       selected: InspectSelection.id(of: r)
                                           == state.selectedRequestId)
                                .onTapGesture {
                                    // A row with no request_id is not
                                    // selectable: an index fallback would
                                    // point at a different request after the
                                    // next clear.
                                    guard InspectSelection.isSelectable(r)
                                    else { return }
                                    state.selectedRequestId =
                                        InspectSelection.id(of: r)
                                }
                        }
                    }
                }
                .frame(maxHeight: 280)
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
                // Capped with its own scroller, like the request list: the
                // page should fit one screen and a long section should scroll
                // inside itself. The count is in the Panel title, so the
                // height makes no claim about how many lines there are.
                ScrollView {
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(Array(LogOrder.newestFirst(rows).enumerated()),
                            id: \.offset) { _, r in
                        let level = r["level"].text
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(alignment: .top, spacing: 8) {
                                // Blank when the runtime sent no timestamp,
                                // never a zero rendered as a time.
                                Text(LogOrder.clock(r, .console) ?? "")
                                    .font(Term.micro)
                                    .foregroundStyle(Term.dim)
                                    .frame(width: 84, alignment: .leading)
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
                .frame(maxHeight: 280)
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
                // Capped with its own scroller. This is the list that grows
                // fastest -- a single tap can produce dozens of records.
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(LogOrder.newestFirst(records).enumerated()),
                                id: \.offset) { _, rec in
                            ReduxRecordRow(record: rec)
                        }
                    }
                }
                .frame(maxHeight: 280)
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
                    Text(LogOrder.clock(record, .redux) ?? "")
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .frame(width: 84, alignment: .leading)
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

/// One request's detail, in named sections.
private struct ExchangeColumn: View {
    let row: JSON
    let visibility: ExchangeVisibility
    let index: Int
    let capture: DetailCapture

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            summary
            visibilityNote
            captureNote
            HeaderSection(title: "Request headers", row: row,
                          key: "request_headers", capture: capture,
                          emptyText: tr("no request headers were recorded"))
            HeaderSection(title: "Response headers", row: row,
                          key: "response_headers", capture: capture,
                          emptyText: tr("no response headers were recorded"))
            BodySection(title: "Request body", row: row, field: .request,
                        capture: capture)
            BodySection(title: "Response body", row: row, field: .response,
                        capture: capture)
            limits
        }
    }

    @ViewBuilder private var summary: some View {
        // Open: three lines, and it is what identifies the row the reader
        // just clicked.
        DisclosureSection(title: "Request", summary: nil, startsOpen: true) {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 7) {
                    Text(row["method"].display("?"))
                        .font(Term.font(12, .medium))
                    // A status nobody sent is a dash, never a zero: a 0 reads
                    // as a failed request that never happened.
                    let status = row["status"].int
                    Chip(text: status.map(String.init)
                             ?? (row["failed"].bool == true ? "fail" : "—"),
                         tone: status == nil ? .neutral
                             : (status! >= 400 ? .bad : .good))
                    Spacer(minLength: 0)
                    Text("#\(index + 1)").font(Term.micro)
                        .foregroundStyle(Term.dim)
                }
                Text(row["url"].display("no url recorded"))
                    .font(Term.font(11)).textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                Field(label: "duration") {
                    Text(row["duration_ms"].double
                            .map { String(format: "%.0f ms", $0) } ?? "—")
                        .font(Term.small)
                }
                Field(label: "bytes on the wire") {
                    Text(row["encoded_bytes"].int.map(String.init) ?? "—")
                        .font(Term.small)
                }
                Field(label: "content type") {
                    Text(row["mime_type"].text.isEmpty
                            ? tr("not stated") : row["mime_type"].text)
                        .font(Term.small)
                }
                if row["failed"].bool == true {
                    Text(row["failure"].text.isEmpty
                            ? tr("the runtime reported this request as failed "
                               + "and gave no reason")
                            : row["failure"].text)
                        .font(Term.small).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if row["incomplete"].bool == true {
                    Text(tr("still in flight when the window closed: evidence "
                          + "of the request, none of its outcome"))
                        .font(Term.micro).foregroundStyle(Term.cyan)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let curl = CurlCommand.build(row, capture: capture) {
                    HStack(spacing: 8) {
                        Button(tr("copy as curl")) {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(curl, forType: .string)
                        }
                        .buttonStyle(TermButtonStyle())
                        .help(tr("reproduces the request as it was sent, for "
                               + "someone else to run"))
                        Spacer(minLength: 0)
                    }
                    if let warning = CurlCommand.credentialWarning(row) {
                        Text(warning)
                            .font(Term.micro).foregroundStyle(Term.amber)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
    }

    /// The selected request is not in the list on the left. Said here because
    /// the reader is looking at a request they cannot see beside it, and the
    /// two causes are separate ideas with separate controls.
    @ViewBuilder private var visibilityNote: some View {
        switch visibility {
        case .visible:
            EmptyView()
        case .hiddenByFilter:
            Text(tr("this request is hidden by the filter. It is still "
                  + "captured."))
                .font(Term.micro).foregroundStyle(Term.cyan)
                .fixedSize(horizontal: false, vertical: true)
        case .heldByClear:
            Text(tr("this request is held back by clear. It is still "
                  + "captured."))
                .font(Term.micro).foregroundStyle(Term.dim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder private var captureNote: some View {
        switch capture {
        case .on:
            EmptyView()
        case .off:
            VStack(alignment: .leading, spacing: 2) {
                Text(tr("headers and bodies were not captured for this "
                      + "observation"))
                Text(tr("switch on \"capture headers and response bodies\" "
                      + "before the next observation"))
                    .foregroundStyle(Term.dim)
            }
            .font(Term.micro).foregroundStyle(Term.cyan)
            .fixedSize(horizontal: false, vertical: true)
        case .unknown:
            Text(tr("whether headers and bodies were captured for this "
                  + "observation is not recorded, so their absence here says "
                  + "nothing either"))
                .font(Term.micro).foregroundStyle(Term.cyan)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Stated on every selection, never conditionally.
    ///
    /// A DevTools reader arrives expecting Timing, Cookies and an initiator
    /// stack. None of the three is measured here, and a tab strip with three
    /// empty tabs would answer "this request had none" instead of "nothing
    /// looked". Rendering this unconditionally is what stops its absence from
    /// becoming a claim of its own.
    @ViewBuilder private var limits: some View {
        // Closed, and deliberately still present: the title is the claim --
        // that there are things this pane cannot show -- so collapsing it
        // hides the list, never the fact.
        DisclosureSection(title: "What this pane cannot show",
                          summary: tr("not measured, rather than measured as "
                                    + "nothing")) {
            BulletList(title: "", items: [
                tr("no timing breakdown: one duration is recorded, and the "
                 + "DNS, connect, TLS and time-to-first-byte phases are not"),
                tr("no cookies: the inspector reports an empty cookie list "
                 + "for every request, so nothing here distinguishes that "
                 + "from a request that sent none"),
                tr("no initiator: what in the app made this call is not "
                 + "captured"),
            ])
        }
    }
}

/// One header map, with four distinguishable empty states.
private struct HeaderSection: View {
    let title: String
    let row: JSON
    let key: String
    let capture: DetailCapture
    let emptyText: String

    var body: some View {
        // Absent means the key was never emitted; present-and-empty means the
        // map was captured and had nothing in it. `isAbsent` is the only test
        // that separates them.
        let absent = row.isAbsent(key)
        let names = row[key].keys
        // Closed by default: one `authorization` header can be two thousand
        // characters, and open-by-default meant a single row pushed the rest
        // of the pane off the screen.
        DisclosureSection(
            title: title,
            summary: absent ? BodySummary.short(
                        FormattedBody(state: .notCaptured, raw: nil, note: ""))
                            : "(\(names.count))") {
            if absent {
                switch capture {
                case .off:
                    Text(tr("headers and bodies were not captured for this "
                          + "observation"))
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                case .unknown, .on:
                    // `.on` and still absent: the setting says they were
                    // asked for and this row has none, which is a gap in what
                    // arrived rather than a gap in what was requested.
                    Text(tr("not recorded for this request"))
                        .font(Term.micro).foregroundStyle(Term.cyan)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } else if names.isEmpty {
                Text(emptyText)
                    .font(Term.micro).foregroundStyle(Term.cyan)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                VStack(alignment: .leading, spacing: 3) {
                    if let note = InspectSecrets.credentialNote(names) {
                        Text(note)
                            .font(Term.micro).foregroundStyle(Term.amber)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(names, id: \.self) { name in
                        VStack(alignment: .leading, spacing: 0) {
                            Text(name).font(Term.micro)
                                .foregroundStyle(Term.dim)
                            Text(row[key][name].text)
                                .font(Term.micro).foregroundStyle(Term.ink)
                                .textSelection(.enabled)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }
        }
    }
}

/// One body, formatted, with every state it can be in kept apart.
/// Which rendering of a body is on screen.
enum BodyView: Hashable { case tree, text }

private struct BodySection: View {
    let title: String
    let row: JSON
    let field: BodyField
    let capture: DetailCapture
    /// Bodies are revealed a step at a time. A 260 KB response re-indents to
    /// thousands of lines, and rendering all of it is seconds of layout on
    /// exactly the captures people most want to read.
    @State private var steps = 1
    /// Per-section, not shared: a reader searching the response body has no
    /// use for the same needle in the request body.
    @State private var needle = ""
    /// Tree by default, because folding is what was asked for. The text view
    /// is one click away and is the authoritative one.
    @State private var view: BodyView = .tree

    private var limit: Int { BodyFormat.displayLimit * steps }

    var body: some View {
        let b = BodyFormat.classify(row, field, limit: limit)
        // The response body opens; everything else starts closed. It is the
        // thing a reader clicked the row for, and the request body is
        // usually absent or a few dozen bytes.
        DisclosureSection(title: title, summary: BodySummary.short(b),
                          startsOpen: field == .response) {
            VStack(alignment: .leading, spacing: 4) {
                switch b.state {
                case .notCaptured:
                    notCapturedText
                case .unavailable(let why):
                    // The runtime's own words, never translated: the core
                    // writes this only when a body was asked for and there
                    // was none, and rewording it would be putting our
                    // sentence in its mouth.
                    Text(why).font(Term.micro).foregroundStyle(Term.cyan)
                        .fixedSize(horizontal: false, vertical: true)
                case .empty:
                    Text(tr("the body was fetched and was zero bytes"))
                        .font(Term.micro).foregroundStyle(Term.cyan)
                        .fixedSize(horizontal: false, vertical: true)
                case .base64:
                    Text(tr("not text: the runtime returned this body base64, "
                          + "and it is shown as it arrived rather than decoded"))
                        .font(Term.micro).foregroundStyle(Term.amber)
                        .fixedSize(horizontal: false, vertical: true)
                    bodyText(b)
                case .json:
                    // Two views of one body, and they are not the same kind
                    // of thing. The tree folds and is parsed -- which turns
                    // 1.0 into 1 and drops a duplicate key -- so the text is
                    // the one that is the bytes that arrived.
                    Picker("", selection: $view) {
                        Text(tr("tree")).tag(BodyView.tree)
                        Text(tr("text")).tag(BodyView.text)
                    }
                    .pickerStyle(.segmented).labelsHidden()
                    .frame(maxWidth: 180)
                    if view == .tree {
                        let parsed = JSONTree.parse(b.raw ?? b.display)
                        if let root = parsed.root {
                            Text(tr("a parsed rendering, so it folds -- "
                                  + "switch to text for the bytes that "
                                  + "arrived"))
                                .font(Term.micro).foregroundStyle(Term.dim)
                                .fixedSize(horizontal: false, vertical: true)
                            if !parsed.truncated.isEmpty {
                                Text(parsed.truncated)
                                    .font(Term.micro)
                                    .foregroundStyle(Term.amber)
                                    .fixedSize(horizontal: false,
                                               vertical: true)
                            }
                            JSONTreeView(root: root)
                        } else {
                            Text(tr("this body could not be parsed into a "
                                  + "tree; the text below is what arrived"))
                                .font(Term.micro).foregroundStyle(Term.amber)
                                .fixedSize(horizontal: false, vertical: true)
                            bodyText(b)
                        }
                    } else {
                        Text(tr("re-indented: every value is the bytes that "
                              + "arrived; only the whitespace between them "
                              + "changed"))
                            .font(Term.micro).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                        bodyText(b)
                    }
                case .text:
                    Text(tr("shown as captured: this is not JSON"))
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                    bodyText(b)
                }
            }
        }
    }

    /// Why a body is absent. `incomplete` and `failed` each prove the body was
    /// never asked for -- the core only requests one for a finished request --
    /// so on those rows the cause is in the document and must not be blamed on
    /// a setting.
    @ViewBuilder private var notCapturedText: some View {
        let words: String = {
            if row["incomplete"].bool == true {
                return tr("still in flight when the window closed, so no body "
                        + "was ever asked for")
            }
            if row["failed"].bool == true {
                return tr("the request failed, so no body was ever asked for")
            }
            switch capture {
            case .off:
                return tr("headers and bodies were not captured for this "
                        + "observation")
            case .unknown:
                return tr("whether headers and bodies were captured for this "
                        + "observation is not recorded, so their absence here "
                        + "says nothing either")
            case .on:
                return tr("the body was asked for and no answer came back")
            }
        }()
        Text(words).font(Term.micro).foregroundStyle(Term.cyan)
            .fixedSize(horizontal: false, vertical: true)
    }

    /// The body itself: one Text, not one per line.
    ///
    /// One Text per line would instantiate thousands of views for a real
    /// response. A single Text of a bounded prefix renders in one pass, and
    /// the bound is stated rather than silent.
    @ViewBuilder private func bodyText(_ b: FormattedBody) -> some View {
        HStack(spacing: 8) {
            TextField(tr("search this body…"), text: $needle)
                .textFieldStyle(TermFieldStyle()).frame(maxWidth: 240)
            if !needle.isEmpty {
                Button(tr("clear")) { needle = "" }
                    .buttonStyle(TermButtonStyle())
            }
            Spacer(minLength: 0)
        }
        if let m = BodySearch.find(in: b.display, needle: needle) {
            // Says how many matched out of how much was searched: "3
            // matches" against a body the pane has only partly loaded would
            // read as three in the whole response.
            Text(BodySearch.summary(m))
                .font(Term.micro)
                .foregroundStyle(m.total == 0 ? Term.amber : Term.cyan)
                .fixedSize(horizontal: false, vertical: true)
            if !m.truncated.isEmpty {
                Text(m.truncated).font(Term.micro).foregroundStyle(Term.dim)
            }
            // The body's own line numbers, so a match stays locatable in the
            // unfiltered text.
            ForEach(m.lines, id: \.number) { line in
                HStack(alignment: .top, spacing: 8) {
                    Text("\(line.number)")
                        .font(Term.micro).foregroundStyle(Term.dim)
                        .frame(width: 46, alignment: .trailing)
                    Text(line.text)
                        .font(Term.micro).foregroundStyle(Term.ink)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                }
            }
        } else {
            Text(b.display)
                .font(Term.micro).foregroundStyle(Term.ink)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        if !b.note.isEmpty {
            HStack(spacing: 8) {
                Text(b.note).font(Term.micro).foregroundStyle(Term.cyan)
                Button(tr("show more")) { steps += 1 }
                    .buttonStyle(TermButtonStyle())
                Spacer(minLength: 0)
            }
            .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// One row in the request list.
///
/// Selection is a left bar and a tint, not `termCard(selected:)`: that
/// modifier's unselected fill is Term.panel -- the same colour as the Panel
/// around it -- so every row would gain a visible box and the list would stop
/// reading as a list.
private struct NetworkRow: View {
    let row: JSON
    let selected: Bool

    var body: some View {
        HStack(spacing: 8) {
            Rectangle()
                .fill(selected ? Term.green : Color.clear)
                .frame(width: 2)
            // From CDP's wallTime. Blank when the runtime sent none -- the
            // monotonic `started_ns` beside it is not a clock and would date
            // every request to 1970.
            Text(LogOrder.clock(row, .network) ?? "")
                .font(Term.micro).foregroundStyle(Term.dim)
                .frame(width: 84, alignment: .leading)
            Text(row["method"].text).font(Term.small)
                .frame(width: 48, alignment: .leading)
            // A status nobody sent renders as a dash. The accessors are
            // optional for exactly this reason -- an absent status coerced to
            // 0 would show as a failed request.
            let status = row["status"].int
            Chip(text: status.map(String.init)
                     ?? (row["failed"].bool == true ? "fail" : "—"),
                 tone: status == nil ? .neutral
                     : (status! >= 400 ? .bad : .good))
            Text(row["duration_ms"].double
                    .map { String(format: "%.0f ms", $0) } ?? "—")
                .font(Term.small).foregroundStyle(Term.dim)
                .frame(width: 64, alignment: .trailing)
            // Head-truncated: a URL's path is what identifies it, and cutting
            // the end leaves every request from one host looking identical.
            Text(row["url"].text).font(Term.small)
                .lineLimit(1).truncationMode(.head)
            Spacer(minLength: 0)
            if row["incomplete"].bool == true {
                // A marker rather than a sentence under the row: the sentence
                // is in the detail pane, and repeating it per row was what
                // made this list hard to scan.
                Text("⋯").font(Term.micro).foregroundStyle(Term.cyan)
                    .help(tr("still in flight when the window closed: "
                           + "evidence of the request, none of its outcome"))
            }
        }
        .padding(.vertical, 2)
        .padding(.trailing, 4)
        .background(selected ? Term.green.opacity(0.10) : Color.clear)
        .contentShape(Rectangle())
    }
}
