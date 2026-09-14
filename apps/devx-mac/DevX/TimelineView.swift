import SwiftUI

/// The Timeline view (spec section 13).
///
/// The rule this view is built around: **a blank lane is not a quiet lane.**
/// Every bin arrives with a state, and the four states get four different
/// treatments so none of them can be mistaken for another at a glance:
///
///   measured    a bar in ink. A measured zero is a baseline tick, still a
///               mark -- the app really did render nothing there.
///   no_reading  a hairline at the floor in dim. The collector was running
///               and this quantity was not sampled here, which is the normal
///               state between two memory readings.
///   partial     the bar, hatched in amber. The number is real and wrong-low.
///   unmeasured  cross-hatched in cyan, the colour this app reserves for "no
///               claim". Deliberately not empty: an empty cell in a chart
///               reads as zero, and that is the one reading this view exists
///               to prevent.
///
/// A renderer is where honesty usually dies, because drawing requires a
/// number for every pixel and absence has none. So this view never reaches
/// for `value` without first looking at `state`.
struct TimelineView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Group {
            if state.sessionDoc.isNull {
                TermEmpty(title: "no session open",
                          detail: "Open one from Sessions to see its timeline.",
                          hint: "mpi timeline <session>")
            } else if state.timelineDoc.isNull {
                TermEmpty(title: "timeline not built",
                          detail: "Binning re-reads and re-analyzes the trace, "
                                + "so it is done on request.",
                          hint: "press Build")
                    .onAppear { state.loadTimeline() }
            } else if !doc["error"].text.isEmpty {
                Banner(kind: .bad, title: "Timeline unavailable",
                       message: doc["error"].display())
            } else if !doc["empty_reason"].text.isEmpty {
                Banner(kind: .caution, title: "No timeline",
                       message: doc["empty_reason"].display())
            } else {
                content
            }
        }
        .navigationTitle("~/timeline")
        .toolbar { toolbarItems }
    }

    private var doc: JSON { state.timelineDoc }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItemGroup {
            Text("bins")
                .font(Term.font(11)).foregroundStyle(Term.dim)
            // A bin count, not a zoom: the window is the capture's, and
            // changing this changes resolution, not extent. Fewer bins means
            // coarser truth, not less of it.
            Picker("", selection: $state.timelineBins) {
                ForEach([40, 80, 160, 320, 640], id: \.self) { n in
                    Text("\(n)").font(Term.font(11))
                }
            }
            .frame(width: 84)
            .onChange(of: state.timelineBins) { _, _ in
                state.loadTimeline(force: true)
            }
            Button("Rebuild") { state.loadTimeline(force: true) }
                .buttonStyle(TermButtonStyle())
        }
    }

    private var content: some View {
        // ScrollView as the root, not nested in a VStack: an inner ScrollView
        // reports its content's height to the layout and collapsed the whole
        // pane into a band floating mid-window.
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                provenance
                header
                legend
                tracks
                bands
                notSaid
            }
            .padding(14)
        }
    }

    @ViewBuilder private var provenance: some View {
        if doc["synthetic"].bool == true {
            Banner(kind: .caution, title: "Synthetic data",
                   message: "These tracks came from a labelled fixture. "
                          + "Nothing here describes a real application.")
        }
        if doc["partial"].bool == true {
            Banner(kind: .caution, title: "Partial capture",
                   message: "The recording ended abnormally, so an empty "
                          + "stretch may be the recording stopping rather "
                          + "than the app going quiet.")
        }
    }

    private var header: some View {
        Panel(title: "Window") {
            VStack(alignment: .leading, spacing: 6) {
                Field(label: "duration") {
                    Text(Fmt.duration(doc["window_end_ns"].stamp
                                      - doc["window_start_ns"].stamp))
                }
                Field(label: "resolution") {
                    Text("\(doc["bin_count"].stamp) bins of "
                         + Fmt.duration(doc["bin_width_ns"].stamp))
                }
                Field(label: "clock") {
                    Text(doc["primary_clock_domain"].display())
                }
            }
        }
    }

    /// Printed above the tracks, never below them: the reader meets the
    /// meaning of a blank cell before meeting a blank cell.
    private var legend: some View {
        Panel(title: "Reading these tracks") {
            VStack(alignment: .leading, spacing: 7) {
                ForEach(BinState.allCases, id: \.rawValue) { s in
                    HStack(spacing: 9) {
                        BinSwatch(state: s)
                            .frame(width: 22, height: 14)
                        Text(s.label)
                            .font(Term.font(11))
                            .foregroundStyle(s == .unmeasured ? Term.cyan : Term.ink)
                        Text(s.detail)
                            .font(Term.font(11))
                            .foregroundStyle(Term.dim)
                    }
                }
            }
        }
    }

    private var tracks: some View {
        Panel(title: "Tracks", subtitle: "one row per source; families are "
                                       + "never stacked, because a total "
                                       + "would double-count") {
            VStack(alignment: .leading, spacing: 14) {
                ForEach(Array(doc["tracks"].array.enumerated()),
                        id: \.offset) { _, track in
                    TrackRow(track: track,
                             windowStart: doc["window_start_ns"].stamp,
                             windowEnd: doc["window_end_ns"].stamp,
                             focusedBandId: state.focusedBandId,
                             issues: doc["issues"].array,
                             gaps: doc["gaps"].array)
                }
            }
        }
    }

    @ViewBuilder private var bands: some View {
        let gaps = doc["gaps"].array
        let issues = doc["issues"].array
        if !gaps.isEmpty {
            Panel(title: "Coverage gaps",
                  subtitle: "from the collectors' own records, not inferred "
                          + "from quiet bins") {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(gaps.enumerated()), id: \.offset) { _, g in
                        HStack(spacing: 8) {
                            Chip(text: g["label"].display(), tone: .caution)
                            Text(Fmt.offset(g["start_ns"].stamp,
                                            doc["window_start_ns"].stamp)
                                 + " for " + Fmt.duration(g["end_ns"].stamp
                                                          - g["start_ns"].stamp))
                                .font(Term.font(11)).foregroundStyle(Term.ink)
                            Text(g["detail"].display(""))
                                .font(Term.font(11)).foregroundStyle(Term.dim)
                            Spacer(minLength: 0)
                        }
                    }
                }
            }
        }
        if !issues.isEmpty {
            Panel(title: "Issue intervals",
                  subtitle: "the evidence's own interval; clicking focuses it") {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(issues.enumerated()), id: \.offset) { _, i in
                        let id = i["id"].text
                        Button {
                            state.focusedBandId =
                                state.focusedBandId == id ? "" : id
                        } label: {
                            HStack(spacing: 8) {
                                Chip(text: i["label"].display(),
                                     tone: state.focusedBandId == id
                                           ? .good : .neutral)
                                Text(Fmt.offset(i["start_ns"].stamp,
                                                doc["window_start_ns"].stamp))
                                    .font(Term.font(11))
                                    .foregroundStyle(Term.ink)
                                if i["widened"].bool == true {
                                    // Without this the band's drawn width
                                    // would read as a duration that was never
                                    // measured.
                                    Chip(text: "widened to be visible",
                                         tone: .caution)
                                }
                                Text(i["detail"].display(""))
                                    .font(Term.font(11))
                                    .foregroundStyle(Term.dim)
                                    .lineLimit(1)
                                Spacer(minLength: 0)
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
    }

    /// Last, and unconditional: what the picture above does not say.
    private var notSaid: some View {
        Panel(title: "What these tracks do not say") {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(Array(doc["tracks"].array.enumerated()),
                        id: \.offset) { _, t in
                    let lims = t["limitations"].array.compactMap { $0.string }
                    if !lims.isEmpty {
                        VStack(alignment: .leading, spacing: 3) {
                            Text(t["label"].display())
                                .font(Term.font(11, .medium))
                                .foregroundStyle(Term.ink)
                            ForEach(lims, id: \.self) { l in
                                Text("· " + l)
                                    .font(Term.font(11))
                                    .foregroundStyle(Term.dim)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                }
            }
        }
    }
}

// MARK: - one track

struct TrackRow: View {
    let track: JSON
    let windowStart: Int
    let windowEnd: Int
    let focusedBandId: String
    let issues: [JSON]
    let gaps: [JSON]

    private var bins: [JSON] { track["bins"].array }
    private var peak: Double? { track["max_value"].double }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            header
            if track["placed"].bool == false {
                // A track that could not be positioned is shown as a track,
                // not omitted: a missing row and a row that could not be
                // placed are different facts.
                Banner(kind: .caution, title: "Not placed",
                       message: track["placement_note"].display())
            } else {
                lanes
                footer
            }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text(track["label"].display())
                .font(Term.font(12, .medium))
                .foregroundStyle(Term.ink)
            Chip(text: track["kind"].display(), tone: .neutral)
            Spacer(minLength: 0)
            if let p = peak {
                Text("peak " + Fmt.value(p, unit: track["unit"].text))
                    .font(Term.font(11)).foregroundStyle(Term.dim)
            } else if track["placed"].bool != false {
                // No fully measured bin: there is no maximum to scale to, and
                // saying "peak 0" would invent one.
                Text("no measured bin")
                    .font(Term.font(11)).foregroundStyle(Term.cyan)
            }
        }
    }

    private var lanes: some View {
        GeometryReader { geo in
            let scale = peak ?? 0
            // Two points of floor, so no state's mark ever touches the
            // lane's border and gets read as part of it.
            HStack(alignment: .bottom, spacing: 0) {
                ForEach(Array(bins.enumerated()), id: \.offset) { _, bin in
                    let s = BinState(bin["state"].text)
                    BinSwatch(state: s,
                              fraction: fraction(bin, state: s, scale: scale))
                }
            }
            .frame(height: geo.size.height - 2)
            .padding(.bottom, 2)
            .overlay(alignment: .topLeading) { bandOverlay(width: geo.size.width) }
        }
        .frame(height: 44)
        .background(Term.raised)
        .overlay(Rectangle().strokeBorder(Term.line))
    }

    private func fraction(_ bin: JSON, state: BinState, scale: Double) -> Double {
        barFraction(state: state, value: bin["value"].double, scale: scale)
    }

    /// The focused issue interval, drawn at full resolution over the bins
    /// rather than snapped to one: "issue click focuses the actual evidence
    /// interval" means the interval.
    @ViewBuilder private func bandOverlay(width: CGFloat) -> some View {
        let span = Double(windowEnd - windowStart)
        if span > 0, !focusedBandId.isEmpty,
           let band = issues.first(where: { $0["id"].text == focusedBandId }) {
            let x0 = Double(band["start_ns"].stamp - windowStart) / span
            let x1 = Double(band["end_ns"].stamp - windowStart) / span
            let left = CGFloat(max(0, min(1, x0))) * width
            let right = CGFloat(max(0, min(1, x1))) * width
            Rectangle()
                .fill(Term.amber.opacity(0.18))
                .overlay(Rectangle().strokeBorder(Term.amber, lineWidth: 1))
                .frame(width: max(2, right - left))
                .offset(x: left)
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 2) {
            // The basis for every blank cell above. Printed whether or not
            // there are any, because "nobody looked" and "looked and saw
            // nothing" are the two readings this line separates.
            let note = track["coverage_note"].text
            if !note.isEmpty {
                Text(note)
                    .font(Term.font(10))
                    .foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            let unplaced = track["unplaced_events"].stamp
            if unplaced > 0 {
                Text("\(unplaced) event(s) fell outside "
                     + "the window and are not drawn")
                    .font(Term.font(10))
                    .foregroundStyle(Term.amber)
            }
        }
    }
}

