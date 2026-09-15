import Foundation

/// Which role a thread plays, for the JS-versus-native split.
///
/// The roles matter because "the main thread" means a different thread
/// depending on which one you mean. In a React Native app there are at least
/// three that a finding can land on, and conflating them sends someone to
/// look at the wrong code:
///
///   * the **UI main thread**, which draws and handles touches;
///   * the **JS thread**, where the app's own JavaScript runs;
///   * the **native-module threads**, where bridged native work happens.
///
/// A blocking read means something different on each. DET-03 already keeps
/// them apart when it reports; this is the same distinction made visible.
enum ThreadRole: String, CaseIterable {
    case uiMain
    case js
    case nativeModules
    case renderer
    case other

    /// React Native's own names for these threads.
    ///
    /// Deliberately the conventional ones -- "UI thread", "JS thread",
    /// "native modules thread" -- rather than shorter labels of this tool's
    /// invention. Someone reading a profile has read the React Native
    /// documentation, and a tool that renames the three threads everyone
    /// already argues about makes its reader translate before they can think.
    var label: String {
        switch self {
        case .uiMain: return tr("UI thread (main)")
        case .js: return tr("JS thread")
        case .nativeModules: return tr("native modules thread")
        case .renderer: return tr("render thread")
        case .other: return tr("other")
        }
    }

    var detail: String {
        switch self {
        case .uiMain:
            return tr("draws and handles input; work here is what a user "
                    + "feels first")
        case .js:
            return tr("the app's own JavaScript; a long task here does not "
                    + "block drawing unless the UI thread waits on it")
        case .nativeModules:
            return tr("bridged native work on behalf of JS")
        case .renderer:
            return tr("the platform's own render thread, not the app's code")
        case .other:
            return tr("no role could be established from what this capture "
                    + "records")
        }
    }
}

/// How a thread's role was established.
///
/// This is the honest part. The Android collector derives `is_js_thread` from
/// the thread's *name* -- `mqt_v_js`, `mqt_js`, anything containing `hermes` --
/// because no platform signal says "this is the JS thread". A name is a
/// convention: React Native has used more than one, a future version may use
/// another, and any thread could be given one of those names.
///
/// So a role is labelled by its basis, and a name-derived role never claims
/// to be an observed fact.
enum RoleBasis: String {
    /// A flag the collector set from a platform signal (the main thread's tid
    /// matching the process's, say).
    case platformSignal
    /// Derived from the thread's name.
    case threadName
    /// Nothing established it.
    case none

    var label: String {
        switch self {
        case .platformSignal: return tr("platform signal")
        case .threadName: return tr("from the thread's name")
        case .none: return tr("not established")
        }
    }

    var caution: Bool { self == .threadName || self == .none }
}

struct ThreadBreakdown: Identifiable {
    var threadInstanceId: String
    var name: String
    var tid: Int
    var role: ThreadRole
    var basis: RoleBasis
    /// Samples attributed to this thread. Absent when the capture has no
    /// sampled data at all -- which is not the same as a thread that used no
    /// CPU, and the view says so.
    var samples: Int?
    /// This thread's share of the app's samples, 0...1. Absent for the same
    /// reason.
    var share: Double?

    var id: String { threadInstanceId.isEmpty ? "\(tid)" : threadInstanceId }
}

/// The split, computed from a session document.
///
/// Pure so it can be tested without a device or a window: the classification
/// and the share arithmetic are where this could quietly mislead.
struct ThreadSplit {
    var threads: [ThreadBreakdown] = []
    /// Total samples across the app's threads, or nil when none were
    /// collected.
    var totalSamples: Int?
    /// True when the capture recorded threads but no samples, so the split is
    /// a list of threads with no work attributed -- not a list of idle ones.
    var threadsWithoutSamples = false

    /// Samples per role, for the headline split. Only roles present.
    var byRole: [(role: ThreadRole, samples: Int, share: Double?)] {
        var sums: [ThreadRole: Int] = [:]
        for t in threads {
            sums[t.role, default: 0] += t.samples ?? 0
        }
        let total = totalSamples ?? 0
        return ThreadRole.allCases.compactMap { role in
            guard let n = sums[role], n > 0 || threads.contains(where: { $0.role == role })
            else { return nil }
            return (role, n, total > 0 ? Double(n) / Double(total) : nil)
        }
    }

    /// Whether any role rests on a thread name rather than a platform signal.
    /// The view leads with this when true.
    var anyRoleFromName: Bool { threads.contains { $0.basis == .threadName } }
}

/// Classifies a thread by the flags the capture recorded, then by its name.
///
/// Order matters: a flag the collector set is preferred, and the name is only
/// consulted when no flag says anything -- so a capture that gains a real
/// platform signal later stops depending on the name without this changing.
func classifyThread(name: String, isMainUi: Bool, isJs: Bool)
    -> (role: ThreadRole, basis: RoleBasis) {
    if isMainUi { return (.uiMain, .platformSignal) }
    // `is_js_thread` is itself name-derived in the Android collector, so a
    // thread carrying it is reported as name-derived rather than observed.
    // Claiming a platform signal here would overstate what was measured.
    if isJs { return (.js, .threadName) }

    let lower = name.lowercased()
    // React Native's JS thread names, observed on real debug builds.
    if lower == "mqt_js" || lower == "mqt_v_js" || lower.hasPrefix("mqt_js")
        || lower.contains("hermes") || lower.contains("javascript") {
        return (.js, .threadName)
    }
    // React Native's native-module threads.
    if lower.hasPrefix("mqt_native") || lower.hasPrefix("mqt_v_native")
        || lower.contains("nativemodules") {
        return (.nativeModules, .threadName)
    }
    if lower == "renderthread" || lower == "gpu" || lower.contains("surfaceflinger") {
        return (.renderer, .threadName)
    }
    if name.isEmpty { return (.other, .none) }
    return (.other, .none)
}

/// Builds the split from a session report document.
func threadSplit(from session: JSON) -> ThreadSplit {
    var split = ThreadSplit()
    let trace = session["trace"]
    let threads = trace["threads"].array
    // `sample_count` is written per thread by the trace's own serializer: the
    // report carries the thread list but not the samples, which live in the
    // raw trace and can be gigabytes. Counting them here would mean re-reading
    // that, so the count comes down with the report -- one integer per thread,
    // bounded by the thread count rather than the capture's size.
    //
    // It is `null` when the capture collected no samples at all, which is not
    // the same as a thread that used no CPU: the first leaves every share
    // absent, the second is a measured zero.
    var perThread: [String: Int?] = [:]
    var total = 0
    var anyCount = false
    for t in threads {
        let key = t["thread_instance_id"].text
        let n = t["sample_count"].int
        perThread[key] = n
        if let n { total += n; anyCount = true }
    }
    if !anyCount { total = 0 }

    for t in threads {
        let name = t["name"].text
        let (role, basis) = classifyThread(
            name: name,
            isMainUi: t["is_main_ui_thread"].bool ?? false,
            isJs: t["is_js_thread"].bool ?? false)
        let key = t["thread_instance_id"].text
        let n = perThread[key] ?? nil
        split.threads.append(ThreadBreakdown(
            threadInstanceId: key,
            name: name,
            tid: t["tid"].int ?? 0,
            role: role,
            basis: basis,
            samples: n,
            // A share needs both this thread's count and a non-zero total.
            share: (n != nil && total > 0) ? Double(n!) / Double(total) : nil))
    }
    split.totalSamples = total > 0 ? total : nil
    split.threadsWithoutSamples = !threads.isEmpty && total == 0
    // Busiest first, then by name so the order is stable across reloads.
    split.threads.sort {
        if ($0.samples ?? 0) != ($1.samples ?? 0) {
            return ($0.samples ?? 0) > ($1.samples ?? 0)
        }
        return $0.name < $1.name
    }
    return split
}
