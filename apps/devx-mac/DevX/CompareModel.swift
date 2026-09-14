import SwiftUI

// The Compare view's pure layer: which words a verdict gets, and which
// conditions are shown side by side.
//
// Separated from the view so it can be tested. The wording is the part that
// matters: "inconclusive" is the verdict people read as "no change", and the
// difference between those two is the whole reason the engine refuses to
// collapse them.

enum CompareWording {
    static let conditionKeys = [
        "platform", "scenario_id", "scenario_version", "device_model",
        "device_form", "os_version", "refresh_policy", "collector_preset",
        "collector_sample_rate_hz", "launch_class", "thermal_state",
        "power_state", "input_data_version", "account_state",
        "network_condition", "cache_state",
    ]

    static func tone(_ verdict: String) -> StatusTone {
        switch verdict {
        case "regression": return .bad
        case "improvement": return .good
        case "no_significant_change": return .neutral
        default: return .caution   // inconclusive: a caution, never a pass
        }
    }

    /// The verdict in words, because the word alone is the part people
    /// over-read. "inconclusive" especially: it is not "no change".
    static func plainly(_ verdict: String) -> String {
        switch verdict {
        case "regression":
            return "the candidate measured worse on at least one metric, by "
                 + "more than the thresholds below"
        case "improvement":
            return "the candidate measured better, by more than the thresholds "
                 + "below"
        case "no_significant_change":
            return "the difference did not clear the thresholds. That is not "
                 + "proof the two are the same"
        default:
            return "no verdict could be reached. This is not 'no change': it "
                 + "means the evidence does not support any conclusion, and "
                 + "each metric says why"
        }
    }
}
