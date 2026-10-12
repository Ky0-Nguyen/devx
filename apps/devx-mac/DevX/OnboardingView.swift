import SwiftUI

/// The tour card. The screen it describes is open behind it and its tab is
/// highlighted in the sidebar (RootView), so the card only has to say what the
/// reader is looking at and what to try first.
///
/// No Return or Esc shortcut: the screen behind stays usable on purpose, and
/// AppKit would hand those keys to the card before the emulator screen or a
/// search field got them -- one Esc meant for the emulator would end the tour.
struct TourCard: View {
    @EnvironmentObject var state: AppState
    let index: Int

    private var step: TourStep { Onboarding.steps[index] }
    private var total: Int { Onboarding.steps.count }
    private var isLast: Bool { index == total - 1 }

    var body: some View {
        VStack(alignment: .leading, spacing: 11) {
            HStack(spacing: 8) {
                Text(cat(tr("tour"), " ", String(index + 1), "/", String(total)))
                    .font(Term.font(10)).foregroundStyle(Term.dim)
                Spacer(minLength: 0)
                Button(tr("Skip tour")) { state.skipTour() }
                    .buttonStyle(.plain)
                    .font(Term.font(11)).foregroundStyle(Term.dim)
            }
            HStack(spacing: 2) {
                ForEach(0..<total, id: \.self) { i in
                    Rectangle().fill(i <= index ? Term.amber : Term.line).frame(height: 2)
                }
            }
            content
            HStack(spacing: 8) {
                Button(tr("Back")) { state.tourBack() }
                    .buttonStyle(TermButtonStyle(tone: Term.dim))
                    .disabled(index == 0)
                Spacer(minLength: 0)
                Button(isLast ? tr("Finish") : tr("Next")) { state.tourNext() }
                    .buttonStyle(TermButtonStyle(tone: Term.amber, filled: true))
            }
        }
        .padding(16)
        .frame(width: 460)
        .background(Term.raised, in: RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Term.amber.opacity(0.7)))
        .shadow(color: .black.opacity(0.7), radius: 20)
    }

    @ViewBuilder private var content: some View {
        switch step.kind {
        case .welcome:
            heading("▚", Onboarding.welcomeTitle)
            prose(Onboarding.welcomeText)
            HStack(spacing: 10) {
                Text(tr("Interface language")).font(Term.small).foregroundStyle(Term.dim)
                Picker("", selection: $state.language) {
                    ForEach(DevXLanguage.allCases) { lang in Text(lang.label).tag(lang) }
                }
                .labelsHidden().pickerStyle(.segmented).frame(width: 260)
            }
        case .feature(let tab):
            let guide = Onboarding.guide(tab)
            HStack(spacing: 8) {
                Image(systemName: tab.icon).foregroundStyle(Term.amber)
                Text(tab.title.uppercased()).font(Term.font(14, .heavy)).kerning(1.4)
                Spacer(minLength: 0)
                Chip(text: FeatureSet.group(of: tab).title)
                if FeatureSet.defaults.contains(tab) {
                    Chip(text: tr("main feature"), tone: .good)
                }
            }
            prose(guide.text)
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(cat("▸ ", tr("try"))).font(Term.font(11, .bold)).foregroundStyle(Term.green)
                Text(guide.tryIt).font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if FeatureSet.alwaysShown.contains(tab) {
                Chip(text: tr("always shown"))
            } else {
                Toggle(tr("Show in sidebar"), isOn: Binding(
                    get: { state.isShown(tab) },
                    set: { state.setShown(tab, $0) }))
                    .toggleStyle(.checkbox).font(Term.small)
            }
        case .finish:
            heading("✓", Onboarding.finishTitle)
            prose(Onboarding.finishText)
            Text(cat(String(state.sidebarTabs.count), "/", String(DevXTab.allCases.count),
                     " ", tr("shown")))
                .font(Term.font(11)).foregroundStyle(Term.green)
        }
    }

    private func heading(_ mark: String, _ text: String) -> some View {
        HStack(spacing: 8) {
            Text(mark).foregroundStyle(Term.amber)
            Text(text.uppercased()).font(Term.font(14, .heavy)).kerning(1.4)
        }
    }

    private func prose(_ text: String) -> some View {
        Text(text).font(Term.body)
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// Settings › Features: every tab, grouped, with a switch each.
struct FeaturesPanel: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        Panel(title: "Features",
              subtitle: "what the sidebar shows; saved on this Mac, and DevX reopens on the tab you left") {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(FeatureSet.Group.allCases) { group in
                    VStack(alignment: .leading, spacing: 6) {
                        Text(group.title.uppercased())
                            .font(Term.font(10, .bold)).kerning(1.2)
                            .foregroundStyle(Term.dim)
                        ForEach(group.tabs) { tab in row(tab) }
                    }
                }
                HStack(spacing: 8) {
                    Button(tr("Main features only")) { state.shownTabs = FeatureSet.defaults }
                        .buttonStyle(TermButtonStyle())
                    Button(tr("Show all")) { state.shownTabs = Set(DevXTab.allCases) }
                        .buttonStyle(TermButtonStyle())
                    Spacer(minLength: 0)
                    Button(tr("Replay the tour")) { state.startTour() }
                        .buttonStyle(TermButtonStyle(tone: Term.amber))
                }
            }
        }
    }

    private func row(_ tab: DevXTab) -> some View {
        let always = FeatureSet.alwaysShown.contains(tab)
        return HStack(alignment: .top, spacing: 9) {
            Toggle("", isOn: Binding(get: { state.isShown(tab) },
                                     set: { state.setShown(tab, $0) }))
                .labelsHidden().toggleStyle(.checkbox)
                .disabled(always)
            Image(systemName: tab.icon).foregroundStyle(Term.green).frame(width: 18)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(tab.title).font(Term.font(12, .semibold))
                    if FeatureSet.defaults.contains(tab) {
                        Chip(text: tr("main feature"), tone: .good)
                    }
                    if always { Chip(text: tr("always shown")) }
                }
                Text(Onboarding.guide(tab).text)
                    .font(Term.small).foregroundStyle(Term.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
    }
}
