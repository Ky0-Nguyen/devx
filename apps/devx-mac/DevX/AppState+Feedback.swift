import Foundation

/// Telling the reader that a click did something.
extension AppState {
    /// Shows the confirmation bar, replacing any earlier one, and clears it
    /// after a while unless something newer has replaced it.
    func confirm(_ message: String, next tabs: [Tab], hint: String? = nil) {
        nextStepSerial += 1
        let step = NextStep(message: message, tabs: tabs, hint: hint, serial: nextStepSerial)
        nextStep = step
        DispatchQueue.main.asyncAfter(deadline: .now() + NextSteps.lifetime) { [weak self] in
            if self?.nextStep?.serial == step.serial { self?.nextStep = nil }
        }
    }

    /// Choosing a device, from the Devices list or anywhere else that offers it.
    func useDevice(_ id: String, name: String) {
        guard !id.isEmpty else { return }
        let changed = selectedDevice != id
        selectedDevice = id
        if changed || nextStep == nil {
            confirm(cat(tr("Device selected:"), " ", name.isEmpty ? id : name), next: NextSteps.afterDevice)
        }
    }

    /// Choosing an app, from Apps, Inspect, or anywhere else that offers one.
    func useApp(_ id: String) {
        guard !id.isEmpty else { return }
        selectedApp = id
        confirm(cat(tr("App selected:"), " ", id), next: NextSteps.afterApp)
    }

    /// Whether `label` is one of the operations in flight, for buttons that
    /// show their own pending state.
    func isRunning(_ label: String) -> Bool { inFlight.contains(label) }

    /// A fixed-window Inspect observation is running. Keyed on its own
    /// operation: the button said "observing…" while the attachable apps were
    /// merely being listed.
    var inspectObserving: Bool { inFlight.contains { $0.hasPrefix("Observing ") } }

    /// A session is being opened, for buttons that open one where the busy
    /// overlay is not shown (Live).
    var openingSession: Bool { inFlight.contains { $0.hasPrefix("Opening ") } }
}
