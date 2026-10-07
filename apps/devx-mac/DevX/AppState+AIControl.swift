import Foundation

/// AI tools showing things in this window, through `mpi mcp`
/// (devx_window_state, devx_window_show; core/capi/control.hpp).
///
/// The window publishes what it shows and takes one command at a time on the
/// main thread, through the same calls a person's clicks make. A command only
/// changes what is shown -- nothing is recorded, started or changed on a
/// device -- and every one puts a notice across the top of the window naming
/// what was opened and why, so the person always knows the window moved
/// because an AI tool moved it.
extension AppState {
    func startAIControl() {
        let dir = (NSHomeDirectory() as NSString)
            .appendingPathComponent("Library/Application Support/DevX")
        let started = Core.controlStart(dir: dir)
        if started["ok"].bool != true {
            // Not fatal: the window works without it; AI tools just cannot reach it.
            NSLog("DevX: AI control endpoint not started: \(started["error"].text)")
            return
        }
        controlTimer?.invalidate()
        controlTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollAIControl() }
        }
    }

    func stopAIControl() {
        controlTimer?.invalidate()
        controlTimer = nil
        Core.controlStop()
    }

    private func pollAIControl() {
        Core.controlSetState(windowState().serialized())
        let next = Core.controlNext()
        guard let id = next["id"].int else { return }
        let reply = applyAICommand(next["command"])
        Core.controlReply(id: id, reply.serialized())
    }

    /// What the window is showing, as `devx_window_state` returns it.
    func windowState() -> JSON {
        var o: [String: JSON] = [
            "app": .string("DevX"),
            "version": .string(engineVersion),
            "tab": .string(tab.rawValue),
            "selected_device": .string(selectedDevice),
            "selected_app": .string(selectedApp),
            "sessions_dir": .string(sessionsDir),
        ]
        if !selectedSession.isEmpty {
            o["open_session"] = .string(selectedSession)
            if selectedIssueIndex < issues.count {
                o["selected_issue"] = .string(issues[selectedIssueIndex]["issue_id"].text)
            }
        }
        if !layoutDoc.isNull { o["layout_shown"] = layoutDoc["saved"]["id"] }
        if !emulatorDisplays.isEmpty {
            o["emulators_shown"] = .array(emulatorDisplays.map { $0["avd_id"] })
        }
        if let busy { o["busy"] = .string(busy) }
        return .object(o)
    }

    private func applyAICommand(_ cmd: JSON) -> JSON {
        var shown: [String] = []
        var problems: [String] = []
        let reason = cmd["reason"].text

        if !cmd["device"].text.isEmpty {
            selectedDevice = cmd["device"].text
            shown.append("device " + selectedDevice)
        }
        if !cmd["app"].text.isEmpty {
            selectedApp = cmd["app"].text
            shown.append("app " + selectedApp)
        }
        var targetTab: DevXTab? = nil
        if !cmd["tab"].text.isEmpty {
            if let t = DevXTab(rawValue: cmd["tab"].text) {
                targetTab = t
            } else {
                problems.append("no tab '\(cmd["tab"].text)'")
            }
        }
        if !cmd["session"].text.isEmpty {
            let issue = cmd["issue"].text
            openSession(cmd["session"].text, revealIn: targetTab ?? .issues,
                        focusIssueId: issue.isEmpty ? nil : issue)
            shown.append("session " + cmd["session"].text + (issue.isEmpty ? "" : ", issue " + issue))
            targetTab = nil  // openSession reveals the tab once the session has loaded
        }
        if !cmd["observation"].text.isEmpty {
            let id = cmd["observation"].text
            let doc = Core.observation(dir: sessionsDir, id: id)
            switch doc["kind"].text {
            case "layout":
                layoutDoc = .object(["ok": .bool(true), "report": doc["document"],
                                     "saved": .object(["id": .string(id)]),
                                     "notes": .array([])])
                targetTab = .layout
                shown.append("layout snapshot " + id)
            case "inspect":
                inspectDoc = doc["document"]
                targetTab = .inspect
                shown.append("inspect observation " + id)
            case "":
                problems.append(doc["error"].text.isEmpty ? "no observation '\(id)'" : doc["error"].text)
            default:
                problems.append("an observation of kind '\(doc["kind"].text)' has no view to show it in")
            }
        }
        if !cmd["avd"].text.isEmpty {
            let id = cmd["avd"].text
            emulatorSelected = id
            targetTab = .emulator
            // Added beside whatever is already shown, not instead of it.
            if avds.contains(where: { $0["id"].text == id && !$0["running"].isNull }),
               display(for: id) == nil {
                openDisplay(id)
            }
            shown.append("emulator " + id)
            loadAndroidSdk()
        }
        if let t = targetTab {
            tab = t
            if shown.isEmpty { shown.append("the \(t.title) tab") }
        }

        if !shown.isEmpty {
            notifyAI(tr("An AI tool showed you") + " " + shown.joined(separator: ", ")
                     + (reason.isEmpty ? "" : " — " + reason))
        }
        var reply: [String: JSON] = ["ok": .bool(problems.isEmpty), "state": windowState()]
        if !shown.isEmpty { reply["shown"] = .array(shown.map { .string($0) }) }
        if !problems.isEmpty { reply["error"] = .string(problems.joined(separator: "; ")) }
        return .object(reply)
    }

    private func notifyAI(_ text: String) {
        aiNotice = text
        aiNoticeClear?.cancel()
        let work = DispatchWorkItem { [weak self] in
            Task { @MainActor in self?.aiNotice = nil }
        }
        aiNoticeClear = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: work)
    }
}
