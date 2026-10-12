import Foundation

/// The sidebar's chosen features, the tab the window reopens on, and the tour.
extension AppState {
    /// The sidebar, in order. See `FeatureSet.sidebar`.
    var sidebarTabs: [Tab] { FeatureSet.sidebar(shown: shownTabs, current: tab) }

    /// Reopens the tab that was open last time, if it is still shown.
    func restoreLastTab() {
        tab = FeatureSet.restore(last: UserDefaults.standard.string(forKey: Self.tabKey),
                                 shown: shownTabs)
    }

    func isShown(_ t: Tab) -> Bool {
        shownTabs.contains(t) || FeatureSet.alwaysShown.contains(t)
    }

    func setShown(_ t: Tab, _ on: Bool) {
        guard !FeatureSet.alwaysShown.contains(t) else { return }
        if on { shownTabs.insert(t) } else { shownTabs.remove(t) }
    }

    // MARK: - The tour

    /// True until this version of the tour has been finished or skipped once.
    var tourPending: Bool {
        UserDefaults.standard.integer(forKey: Self.tourKey) < Onboarding.version
    }

    func startTour() {
        // Starting again while it runs keeps the original place to return to.
        if tourStep == nil {
            tabBeforeTour = tab
            tourDetached = false
        }
        tourGo(to: 0)
    }

    func tourGo(to index: Int) {
        let steps = Onboarding.steps
        guard steps.indices.contains(index) else { return }
        tourStep = index
        // The card describes a screen, so that screen is what sits behind it.
        if let t = steps[index].tab, t != tab {
            tourNavigating = true
            tab = t
            tourNavigating = false
        }
    }

    /// The tab changed under the tour: show that tab's card.
    func tourFollow(_ t: Tab) {
        tourDetached = true
        if let i = Onboarding.steps.firstIndex(where: { $0.kind == .feature(t) }) {
            tourStep = i
        }
    }

    func tourNext() {
        guard let i = tourStep else { return }
        if i + 1 < Onboarding.steps.count { tourGo(to: i + 1) } else { finishTour() }
    }

    func tourBack() {
        guard let i = tourStep, i > 0 else { return }
        tourGo(to: i - 1)
    }

    /// Finishing leaves the reader on Settings, where the last card pointed.
    func finishTour() {
        tourStep = nil
        tabBeforeTour = nil
        UserDefaults.standard.set(Onboarding.version, forKey: Self.tourKey)
        UserDefaults.standard.set(tab.rawValue, forKey: Self.tabKey)
    }

    /// Skipping goes back to where the reader was, unless they or an AI tool
    /// went somewhere else meanwhile.
    func skipTour() {
        let back = tourDetached ? nil : tabBeforeTour
        finishTour()
        if let back { tab = back }
    }
}
