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
        var expanded = Set<UInt32>()
        var documentExpanded = true
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
        // A parent always comes before its children, so one pass gives every depth.
        var depths = [UInt16](repeating: 0, count: model.count)
        for index in 0..<model.count {
            let parent = model.nodes[index].parent
            if parent != UInt32.max {
                depths[index] = depths[Int(parent)] &+ 1
            }
        }
        documents[document].model = model
        documents[document].depths = depths
        // Roots start open, so a single top assembly shows its parts at once.
        for index in 0..<model.count where model.nodes[index].parent == UInt32.max {
            documents[document].expanded.insert(UInt32(index))
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
        let name = document.model?.name(Int(row.node)) ?? ""
        if !name.isEmpty {
            return name
        }
        switch document.model?.nodes[Int(row.node)].kind {
        case GeoNodeAssembly.rawValue: return "Unnamed assembly"
        case GeoNodeBody.rawValue: return "Unnamed body"
        default: return "Unnamed part"
        }
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
            result.append(TreeRow(document: documentIndex, node: TreeRow.documentRow, depth: 0))
            guard document.documentExpanded, let model = document.model else {
                continue
            }
            var node = 0
            while node < model.count {
                result.append(
                    TreeRow(
                        document: documentIndex,
                        node: UInt32(node),
                        depth: Int(document.depths[node]) + 1
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
