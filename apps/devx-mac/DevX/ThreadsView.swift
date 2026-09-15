import SwiftUI

/// The JS-versus-native thread split.
///
/// "The main thread" means a different thread depending on which one you
/// mean, and in a React Native app there are at least three a finding can
/// land on: the UI thread that draws, the JS thread where the app's own code
/// runs, and the native-module threads that bridge between them. A long
/// blocking read means something different on each, and conflating them sends
/// someone to look at the wrong code.
///
/// The view leads with how the split was decided, because that is the weakest
/// part of it: on Android the JS thread is identified by its *name*
/// (`mqt_v_js`, `mqt_js`, anything containing `hermes`), since no platform
/// signal says "this is the JS thread". A name is a convention -- React
/// Native has used more than one, and any thread could be given one -- so a
/// name-derived role is shown as name-derived rather than as an observed fact.
struct ThreadsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Group {
            if state.sessionDoc.isNull {
                TermEmpty(title: "no session open",
                          detail: "Open one from Sessions to see how its work "
                                + "divides between the UI, JS and native "
                                + "threads.",
                          hint: "mpi analyze <session>")
            } else {
                content
            }
        }
        .navigationTitle("~/threads")
    }

    private var split: ThreadSplit { threadSplit(from: state.sessionDoc) }

    private var content: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                let s = split
                if s.threads.isEmpty {
                    Banner(kind: .caution, title: "No threads recorded",
                           message: "This capture names no threads, so there "
                                  + "is no split to show. That is a missing "
                                  + "provider, not an app with one thread: "
                                  + "thread identity comes from the sampler, "
                                  + "and a capture without CPU sampling "
                                  + "records none.")
                } else {
                    basisBanner(s)
                    roleSummary(s)
                    threadTable(s)
                    caveats(s)
                }
            }
            .padding(14)
        }
    }

    /// First, not last: the split is only as good as what decided it.
    @ViewBuilder private func basisBanner(_ s: ThreadSplit) -> some View {
        if s.anyRoleFromName {
            Banner(kind: .caution, title: "Roles below come from thread names",
                   message: "No platform signal says which thread runs "
                          + "JavaScript, so the JS and native-module roles are "
                          + "matched on the thread's name -- `mqt_v_js`, "
                          + "`mqt_js`, anything containing `hermes`. That is a "
                          + "convention React Native has changed before, and "
                          + "any thread could carry such a name. The UI main "
                          + "thread is the exception: the collector "
                          + "establishes it from the process, not the name.")
        }
    }

    private func roleSummary(_ s: ThreadSplit) -> some View {
        Panel(title: "Where the sampled work was",
              subtitle: s.totalSamples == nil
                ? "no CPU samples in this capture, so no share can be computed"
                : "\(s.totalSamples ?? 0) sample(s) across this app's threads") {
            VStack(alignment: .leading, spacing: 9) {
                ForEach(Array(s.byRole.enumerated()), id: \.offset) { _, entry in
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            Chip(text: entry.role.label,
                                 tone: entry.role == .uiMain ? .caution : .neutral)
                            if let share = entry.share {
                                Text(String(format: "%.1f%%", share * 100))
                                    .font(Term.font(12, .medium))
                                    .foregroundStyle(Term.ink)
                                Text("\(entry.samples) sample(s)")
                                    .font(Term.font(10)).foregroundStyle(Term.dim)
                            } else {
                                // Absent, not zero: a thread with no samples
                                // was not necessarily idle.
                                Text(tr("no share available"))
                                    .font(Term.font(11)).foregroundStyle(Term.cyan)
                            }
                            Spacer(minLength: 0)
                        }
                        if let share = entry.share {
                            ShareBar(fraction: share,
                                     tone: entry.role == .uiMain ? Term.amber : Term.green)
                                .frame(height: 6)
                        }
                        Text(entry.role.detail)
                            .font(Term.font(10)).foregroundStyle(Term.dim)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
    }

    private func threadTable(_ s: ThreadSplit) -> some View {
        Panel(title: tr("Threads") + " (\(s.threads.count))",
              subtitle: "busiest first; a thread with no samples is not a "
                      + "thread that did nothing") {
            VStack(alignment: .leading, spacing: 7) {
                ForEach(s.threads) { t in
                    HStack(alignment: .top, spacing: 8) {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(t.name.isEmpty ? tr("(unnamed)") : t.name)
                                .font(Term.font(12)).foregroundStyle(Term.ink)
                            Text("tid \(t.tid)")
                                .font(Term.font(10)).foregroundStyle(Term.dim)
                        }
                        .frame(width: 190, alignment: .leading)

                        VStack(alignment: .leading, spacing: 1) {
                            Chip(text: t.role.label,
                                 tone: t.role == .uiMain ? .caution : .neutral)
                            // The basis travels with every row, not just the
                            // banner: a reader scanning the table should not
                            // have to remember it.
                            Text(t.basis.label)
                                .font(Term.font(10))
                                .foregroundStyle(t.basis.caution ? Term.amber
                                                                 : Term.dim)
                        }
                        .frame(width: 150, alignment: .leading)

                        if let n = t.samples, let share = t.share {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(String(format: "%.1f%%  (%d)", share * 100, n))
                                    .font(Term.font(11)).foregroundStyle(Term.ink)
                                ShareBar(fraction: share, tone: Term.green)
                                    .frame(height: 5)
                            }
                        } else {
                            Text(tr("no samples attributed"))
                                .font(Term.font(10)).foregroundStyle(Term.cyan)
                        }
                        Spacer(minLength: 0)
                    }
                }
            }
        }
    }

    private func caveats(_ s: ThreadSplit) -> some View {
        Panel(title: "What this split does not say") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(caveatList(s), id: \.self) { c in
                    Text("· " + c)
                        .font(Term.font(10)).foregroundStyle(Term.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private func caveatList(_ s: ThreadSplit) -> [String] {
        var out = [
            "a share here is a share of THIS APP'S samples, not of a core and "
            + "not of wall time: every thread could be at 100% of this list "
            + "while the device was mostly idle",
            "sampling says where time went, not how long anything took; work "
            + "shorter than the sampling interval can be missed entirely",
            "a thread with no samples attributed was not necessarily idle -- "
            + "the sampler covers windows, and an uncovered stretch is a gap",
            "the process's CPU time is whole-process: nothing here measures "
            + "per-thread CPU time, only per-thread sample counts",
        ]
        if s.anyRoleFromName {
            out.append("the JS and native-module roles rest on thread names, "
                       + "so a renamed or unusually named thread lands in "
                       + "'other' rather than being guessed at")
        }
        if s.threadsWithoutSamples {
            out.append("this capture named threads but collected no samples, "
                       + "so the split is a list of threads with no work "
                       + "attributed to any of them")
        }
        return out
    }
}

/// A share, drawn. Deliberately plain: it carries no scale of its own, so it
/// cannot imply a share of anything but the total it was given.
struct ShareBar: View {
    let fraction: Double
    let tone: Color

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Rectangle().fill(Term.raised)
                Rectangle()
                    .fill(tone.opacity(0.8))
                    .frame(width: max(1, geo.size.width *
                                         max(0, min(1, fraction))))
            }
            .overlay(Rectangle().strokeBorder(Term.line))
        }
    }
}
