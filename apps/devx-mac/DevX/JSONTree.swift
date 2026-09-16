// A response body as a foldable tree.
//
// "thêm phần thu gọn object trong body response trả về" -- a body is a few
// top-level keys and a lot of nesting, and reading it as text means scrolling
// past whichever subtree happens to be enormous to reach the one you want.
//
// This is a **rendering**, and that word is load-bearing. The tree is built
// by parsing with Foundation, and parsing is exactly what the re-indenter in
// BodyFormat refuses to do, for reasons measured on this machine:
//
//     {"v":1.0}      ->  a Double, printed back as 1
//     {"a":1,"a":2}  ->  one key; the duplicate is gone
//
// So the tree can be folded and searched and the text cannot, and the text is
// the bytes that arrived and the tree is not. Both are offered, the text is
// what the pane says is authoritative, and neither is called the other.
import Foundation

/// One node of a parsed body.
struct JSONNode: Identifiable {
    enum Kind: Equatable {
        case object(count: Int)
        case array(count: Int)
        case scalar
    }

    /// Stable within one parse: the path from the root, so folding a node
    /// survives a re-render. Not stable across captures, and not used as one.
    let id: String
    /// The key this node sits under, or an index like `[3]`. Empty at the
    /// root.
    let label: String
    let kind: Kind
    /// For a scalar, the value as text. Empty for containers.
    let value: String
    let children: [JSONNode]

    var isContainer: Bool { kind != .scalar }

    /// What a folded node says about itself: `{7 keys}`, `[12 items]`.
    var summary: String {
        switch kind {
        case .object(let n): return "{\(n)}"
        case .array(let n):  return "[\(n)]"
        case .scalar:        return ""
        }
    }
}

enum JSONTree {
    /// How many nodes are worth building.
    ///
    /// A 260 KB response is tens of thousands of nodes, and every one is a
    /// SwiftUI view. The cap is reported rather than silently shortening the
    /// tree -- a subtree that is missing without saying so is worse than no
    /// tree at all.
    static let nodeLimit = 4000

    struct Result {
        let root: JSONNode?
        /// Set when the tree is not the whole document.
        let truncated: String
    }

    /// Parses `body` into a tree, or a nil root when it is not JSON.
    static func parse(_ body: String, limit: Int = nodeLimit) -> Result {
        guard let data = body.data(using: .utf8),
              let any = try? JSONSerialization.jsonObject(
                  with: data, options: [.fragmentsAllowed]) else {
            return Result(root: nil, truncated: "")
        }
        var budget = limit
        let root = node(from: any, label: "", path: "$", budget: &budget)
        let note = budget <= 0
            ? tr("the tree stops at {n} nodes; the text below is complete")
                .replacingOccurrences(of: "{n}", with: "\(limit)")
            : ""
        return Result(root: root, truncated: note)
    }

    private static func node(from any: Any, label: String, path: String,
                             budget: inout Int) -> JSONNode? {
        guard budget > 0 else { return nil }
        budget -= 1

        if let dict = any as? [String: Any] {
            // Sorted so the tree is stable between renders. The *text* view
            // keeps the document's own key order; this one cannot, because a
            // dictionary has none to keep.
            var kids: [JSONNode] = []
            for key in dict.keys.sorted() {
                guard let child = node(from: dict[key]!, label: key,
                                       path: path + "." + key,
                                       budget: &budget) else { break }
                kids.append(child)
            }
            return JSONNode(id: path, label: label,
                            kind: .object(count: dict.count), value: "",
                            children: kids)
        }
        if let arr = any as? [Any] {
            var kids: [JSONNode] = []
            for (i, item) in arr.enumerated() {
                guard let child = node(from: item, label: "[\(i)]",
                                       path: path + "[\(i)]",
                                       budget: &budget) else { break }
                kids.append(child)
            }
            return JSONNode(id: path, label: label,
                            kind: .array(count: arr.count), value: "",
                            children: kids)
        }
        return JSONNode(id: path, label: label, kind: .scalar,
                        value: scalar(any), children: [])
    }

    /// A leaf as text.
    ///
    /// Strings are quoted so an empty string is visible as `""` rather than
    /// as nothing, and so a string holding `123` cannot be mistaken for a
    /// number. A whole-valued Double prints without its `.0` -- a
    /// consequence of having parsed at all, which is why the text view is
    /// the one the pane calls authoritative.
    static func scalar(_ any: Any) -> String {
        if any is NSNull { return "null" }
        if let b = any as? Bool { return b ? "true" : "false" }
        if let s = any as? String { return "\"\(s)\"" }
        if let n = any as? NSNumber {
            let d = n.doubleValue
            if d == d.rounded() && abs(d) < 9007199254740992 {
                return String(Int64(d))
            }
            return "\(d)"
        }
        return "\(any)"
    }
}

extension JSONTree {
    /// One row of the folded tree: a node and how deep it sits.
    struct Row: Identifiable {
        let node: JSONNode
        let depth: Int
        var id: String { node.id }
    }

    /// The rows currently visible, flattened.
    ///
    /// Flattened rather than rendered recursively, for two reasons. A
    /// recursive SwiftUI view cannot infer its own return type -- the
    /// compiler rejects `some View` defined in terms of itself -- and the
    /// usual escape, wrapping each level in AnyView, erases the type at every
    /// node of a tree that can be thousands deep. A flat list of rows has
    /// neither problem, renders as one ForEach, and puts the fold logic here
    /// where it can be tested instead of in a view.
    ///
    /// `expandAll` moves the baseline and `folded` holds the exceptions, so a
    /// node shut by hand stays shut when everything else is opened, and a
    /// node opened by hand survives the next render.
    static func visibleRows(_ root: JSONNode, folded: Set<String>,
                            expandAll: Bool, openDepth: Int) -> [Row] {
        var out: [Row] = []
        func walk(_ node: JSONNode, _ depth: Int) {
            out.append(Row(node: node, depth: depth))
            let base = expandAll || depth < openDepth
            let open = folded.contains(node.id) ? !base : base
            guard open else { return }
            for child in node.children { walk(child, depth + 1) }
        }
        walk(root, 0)
        return out
    }
}
