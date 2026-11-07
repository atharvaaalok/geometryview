import Foundation
import GeometryEngine

/// One visible row: a document's own row, or one of its tree nodes.
struct TreeRow: Equatable {
    static let documentRow = UInt32.max

    let document: UInt32
    let node: UInt32
    let depth: Int

    var isDocument: Bool { node == Self.documentRow }
}

/// The tree's visible rows across all open documents, rebuilt when something expands or collapses.
///
/// Rows come from walking nodes in depth-first order and jumping over the subtree of each collapsed
/// node, so the work grows with what is shown rather than with the size of the model.
@MainActor
final class TreeRows {
    struct Document {
        let name: String
        var model: TreeModel?
        var depths: [UInt16] = []
        /// How many parts lie under each node, the node itself included.
        var partCounts: [UInt32] = []
        var expanded = Set<UInt32>()
        var documentExpanded = true

        /// A file's own row heads its tree only when several files are open, since the masthead
        /// names a lone one, or while its tree is still on its way.
        func isHeaded(among count: Int) -> Bool { count > 1 || model == nil }
    }

    private(set) var documents: [Document] = []
    private(set) var rows: [TreeRow] = []

    func reset() {
        documents = []
        rows = []
    }

    func addDocument(named name: String) {
        documents.append(Document(name: name))
        rebuild()
    }

    func setModel(_ model: TreeModel, for document: Int) {
        guard document < documents.count else {
            return
        }
        // A parent always comes before its children, so one pass down gives every depth and one
        // pass back up every count.
        var depths = [UInt16](repeating: 0, count: model.count)
        for index in 0..<model.count {
            let parent = model.nodes[index].parent
            if parent != UInt32.max {
                depths[index] = depths[Int(parent)] &+ 1
            }
        }
        var parts = [UInt32](repeating: 0, count: model.count)
        for index in stride(from: model.count - 1, through: 0, by: -1) {
            if model.nodes[index].kind == GeoNodePart.rawValue {
                parts[index] += 1
            }
            let parent = model.nodes[index].parent
            if parent != UInt32.max {
                parts[Int(parent)] += parts[index]
            }
        }
        documents[document].model = model
        documents[document].depths = depths
        documents[document].partCounts = parts
        // Roots start open, so a single top assembly shows its parts at once. A root with few
        // children opens its sub-assemblies too, when that is still a tree to read at a glance.
        for index in 0..<model.count where model.nodes[index].parent == UInt32.max {
            documents[document].expanded.insert(UInt32(index))
            var children: [Int] = []
            var child = index + 1
            while child < Int(model.nodes[index].subtree_end), children.count <= 12 {
                children.append(child)
                child = Int(model.nodes[child].subtree_end)
            }
            guard children.count <= 12 else {
                continue
            }
            for child in children where model.nodes[child].kind == GeoNodeAssembly.rawValue {
                documents[document].expanded.insert(UInt32(child))
            }
        }
        rebuild()
    }

    func hasChildren(_ row: TreeRow) -> Bool {
        if row.isDocument {
            return (documents[Int(row.document)].model?.count ?? 0) > 0
        }
        guard let model = documents[Int(row.document)].model else {
            return false
        }
        return model.nodes[Int(row.node)].subtree_end > row.node + 1
    }

    func isExpanded(_ row: TreeRow) -> Bool {
        let document = documents[Int(row.document)]
        return row.isDocument ? document.documentExpanded : document.expanded.contains(row.node)
    }

    func toggle(_ row: TreeRow) {
        setExpanded(row, !isExpanded(row))
    }

    func setExpanded(_ row: TreeRow, _ expanded: Bool) {
        if row.isDocument {
            documents[Int(row.document)].documentExpanded = expanded
        } else if expanded {
            documents[Int(row.document)].expanded.insert(row.node)
        } else {
            documents[Int(row.document)].expanded.remove(row.node)
        }
        rebuild()
    }

    /// Opens every ancestor of a node, so its row is visible, and returns that row's index.
    @discardableResult
    func reveal(document: UInt32, node: UInt32) -> Int? {
        guard Int(document) < documents.count, let model = documents[Int(document)].model,
            Int(node) < model.count
        else {
            return nil
        }
        documents[Int(document)].documentExpanded = true
        var parent = model.nodes[Int(node)].parent
        while parent != UInt32.max {
            documents[Int(document)].expanded.insert(parent)
            parent = model.nodes[Int(parent)].parent
        }
        rebuild()
        return rows.firstIndex { $0.document == document && $0.node == node }
    }

    func name(of row: TreeRow) -> String {
        let document = documents[Int(row.document)]
        if row.isDocument {
            return document.name
        }
        guard let model = document.model else {
            return ""
        }
        let name = model.name(Int(row.node))
        if !name.isEmpty {
            return name
        }
        switch model.nodes[Int(row.node)].kind {
        case GeoNodeAssembly.rawValue: return "Unnamed assembly"
        case GeoNodeBody.rawValue: return "Body \(bodyOrdinal(Int(row.node), in: model))"
        default: return "Unnamed part"
        }
    }

    // A body's place among its parent's bodies, from 1. Bodies have no children, so the scan steps
    // over whole subtrees and costs as many steps as there are siblings before it.
    private func bodyOrdinal(_ node: Int, in model: TreeModel) -> Int {
        let parent = model.nodes[node].parent
        guard parent != UInt32.max else {
            return 1
        }
        var ordinal = 1
        var sibling = Int(parent) + 1
        while sibling < node {
            if model.nodes[sibling].kind == GeoNodeBody.rawValue {
                ordinal += 1
            }
            sibling = Int(model.nodes[sibling].subtree_end)
        }
        return ordinal
    }

    /// What a row's count says: the parts in an assembly or a file, or ×N on a part made of N
    /// bodies. Empty when there is nothing worth counting.
    func count(of row: TreeRow) -> String {
        let document = documents[Int(row.document)]
        guard let model = document.model else {
            return ""
        }
        if row.isDocument {
            var parts: UInt32 = 0
            for index in 0..<model.count where model.nodes[index].parent == UInt32.max {
                parts += document.partCounts[index]
            }
            return parts > 0 ? "\(parts)" : ""
        }
        let node = model.nodes[Int(row.node)]
        switch node.kind {
        case GeoNodeAssembly.rawValue:
            let parts = document.partCounts[Int(row.node)]
            return parts > 0 ? "\(parts)" : ""
        case GeoNodePart.rawValue:
            // A part's children are its bodies.
            let bodies = node.subtree_end - row.node - 1
            return bodies > 1 ? "×\(bodies)" : ""
        default:
            return ""
        }
    }

    /// The first occurrence under a row, whose colour stands for it, or nil for a file's row.
    func firstOccurrence(of row: TreeRow) -> UInt32? {
        guard !row.isDocument, let model = documents[Int(row.document)].model else {
            return nil
        }
        let node = model.nodes[Int(row.node)]
        return node.occurrence_end > node.first_occurrence ? node.first_occurrence : nil
    }

    /// The names of a row's ancestors, from the root down, as a path.
    func path(of row: TreeRow) -> String {
        guard !row.isDocument, let model = documents[Int(row.document)].model else {
            return ""
        }
        var names: [String] = []
        var parent = model.nodes[Int(row.node)].parent
        while parent != UInt32.max {
            names.append(name(of: TreeRow(document: row.document, node: parent, depth: 0)))
            parent = model.nodes[Int(parent)].parent
        }
        names.append(documents[Int(row.document)].name)
        return names.reversed().joined(separator: " / ")
    }

    func kind(of row: TreeRow) -> GeoNodeKind? {
        guard !row.isDocument, let model = documents[Int(row.document)].model else {
            return nil
        }
        return GeoNodeKind(rawValue: model.nodes[Int(row.node)].kind)
    }

    private func rebuild() {
        var result: [TreeRow] = []
        for (index, document) in documents.enumerated() {
            let documentIndex = UInt32(index)
            let headed = document.isHeaded(among: documents.count)
            if headed {
                result.append(TreeRow(document: documentIndex, node: TreeRow.documentRow, depth: 0))
            }
            guard !headed || document.documentExpanded, let model = document.model else {
                continue
            }
            let indent = headed ? 1 : 0
            var node = 0
            while node < model.count {
                result.append(
                    TreeRow(
                        document: documentIndex,
                        node: UInt32(node),
                        depth: Int(document.depths[node]) + indent
                    )
                )
                node =
                    document.expanded.contains(UInt32(node))
                    ? node + 1 : Int(model.nodes[node].subtree_end)
            }
        }
        rows = result
    }
}
