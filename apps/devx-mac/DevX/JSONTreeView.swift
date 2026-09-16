// The foldable view of a parsed body.
//
// Collapsed past the first level: a response is a few top-level keys each
// holding a lot, so one line per subtree until it is asked for. The rows are
// flattened by JSONTree.visibleRows rather than rendered recursively -- see
// the reasoning there -- so this view is one ForEach over a list.
import SwiftUI

struct JSONTreeView: View {
    let root: JSONNode
    var openDepth: Int = 1

    @State private var folded: Set<String> = []
    @State private var expandAll = false

    var body: some View {
        let rows = JSONTree.visibleRows(root, folded: folded,
                                        expandAll: expandAll,
                                        openDepth: openDepth)
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                Button(expandAll ? tr("collapse all") : tr("expand all")) {
                    expandAll.toggle()
                    folded.removeAll()
                }
                .buttonStyle(TermButtonStyle())
                Text("\(rows.count) " + tr("row(s)"))
                    .font(Term.micro).foregroundStyle(Term.dim)
                Spacer(minLength: 0)
            }
            ForEach(rows) { row in
                HStack(alignment: .top, spacing: 6) {
                    if row.depth > 0 {
                        Spacer().frame(width: CGFloat(row.depth) * 12)
                    }
                    if row.node.isContainer && !row.node.children.isEmpty {
                        Button {
                            if folded.contains(row.node.id) {
                                folded.remove(row.node.id)
                            } else {
                                folded.insert(row.node.id)
                            }
                        } label: {
                            Image(systemName: isOpen(row) ? "chevron.down"
                                                          : "chevron.right")
                                .font(Term.micro).foregroundStyle(Term.dim)
                                .frame(width: 10)
                        }
                        .buttonStyle(.plain)
                    } else {
                        Spacer().frame(width: 10)
                    }
                    if !row.node.label.isEmpty {
                        Text(row.node.label)
                            .font(Term.micro).foregroundStyle(Term.cyan)
                    }
                    if row.node.isContainer {
                        Text(row.node.summary)
                            .font(Term.micro).foregroundStyle(Term.dim)
                        // A container whose children were not built says so,
                        // rather than looking like an empty one.
                        if row.node.children.isEmpty, childCount(row.node) > 0 {
                            Text(tr("not expanded"))
                                .font(Term.micro).foregroundStyle(Term.amber)
                        }
                    } else {
                        Text(row.node.value)
                            .font(Term.micro).foregroundStyle(Term.ink)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer(minLength: 0)
                }
            }
        }
    }

    private func isOpen(_ row: JSONTree.Row) -> Bool {
        let base = expandAll || row.depth < openDepth
        return folded.contains(row.node.id) ? !base : base
    }

    private func childCount(_ node: JSONNode) -> Int {
        switch node.kind {
        case .object(let n): return n
        case .array(let n):  return n
        case .scalar:        return 0
        }
    }
}
