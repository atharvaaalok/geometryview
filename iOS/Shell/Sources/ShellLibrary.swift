import UIKit
import UniformTypeIdentifiers
import WebKit

/// The Library's way to the person's files on the phone: the system's pickers for files and for
/// folders, and the folders connected, kept as bookmarks between launches and served to the page by
/// id (`WebFiles`). The page asks through the `library` message handler, and is answered:
///
///   { op: "folders" }               → [{ id, name, state: "ready" | "missing" }]
///   { op: "connectFolder" }         → the new folder's id, or null if the picker was dismissed
///   { op: "pickFiles" }             → [{ path, name, size, lastModified }], or null
///   { op: "disconnect", id }        → true
///
/// A folder bookmark is resolved when the page first asks for the folders, not at launch.
@MainActor
final class ShellLibrary: NSObject, WKScriptMessageHandlerWithReply, UIDocumentPickerDelegate {
    private struct Saved: Codable {
        var id: String
        var bookmark: Data
    }

    private struct Folder {
        var id: String
        var name: String
        var url: URL?
        var bookmark: Data
    }

    private enum Picking {
        case files, folder
    }

    private let opened: OpenedFiles
    weak var webView: WKWebView?
    private var folders: [Folder]?
    private var picking: Picking?
    private var reply: ((Any?, String?) -> Void)?
    private static let key = "library.folders"

    init(opened: OpenedFiles) {
        self.opened = opened
    }

    func userContentController(
        _ userContentController: WKUserContentController,
        didReceive message: WKScriptMessage,
        replyHandler: @escaping @MainActor @Sendable (Any?, String?) -> Void
    ) {
        guard let body = message.body as? [String: Any], let op = body["op"] as? String else {
            replyHandler(nil, "The shell did not understand that request.")
            return
        }
        switch op {
        case "folders":
            replyHandler(listFolders(), nil)
        case "connectFolder":
            present(.folder, replyHandler)
        case "pickFiles":
            present(.files, replyHandler)
        case "disconnect":
            disconnect(body["id"] as? String ?? "")
            replyHandler(true, nil)
        default:
            replyHandler(nil, "The shell has no \(op).")
        }
    }

    // MARK: Folders

    private func loadFolders() -> [Folder] {
        if let folders {
            return folders
        }
        var loaded: [Folder] = []
        let data = UserDefaults.standard.data(forKey: Self.key)
        for saved in (data.flatMap { try? JSONDecoder().decode([Saved].self, from: $0) }) ?? [] {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: saved.bookmark, bookmarkDataIsStale: &stale),
                url.startAccessingSecurityScopedResource()
            {
                let bookmark = stale ? (try? url.bookmarkData()) ?? saved.bookmark : saved.bookmark
                opened.addFolder(url, id: saved.id)
                loaded.append(Folder(id: saved.id, name: url.lastPathComponent, url: url, bookmark: bookmark))
            } else {
                let name = URL.resourceValues(forKeys: [.nameKey], fromBookmarkData: saved.bookmark)?.name ?? "Folder"
                loaded.append(Folder(id: saved.id, name: name, url: nil, bookmark: saved.bookmark))
            }
        }
        folders = loaded
        save()
        return loaded
    }

    private func listFolders() -> [[String: String]] {
        loadFolders().map { ["id": $0.id, "name": $0.name, "state": $0.url == nil ? "missing" : "ready"] }
    }

    private func connect(_ url: URL) -> String? {
        var current = loadFolders()
        if let existing = current.first(where: { $0.url?.standardizedFileURL == url.standardizedFileURL }) {
            return existing.id
        }
        // A folder from the picker is security-scoped; one in the app's own storage needs no scope.
        _ = url.startAccessingSecurityScopedResource()
        guard let bookmark = try? url.bookmarkData() else {
            return nil
        }
        let id = UUID().uuidString.lowercased()
        opened.addFolder(url, id: id)
        current.append(Folder(id: id, name: url.lastPathComponent, url: url, bookmark: bookmark))
        folders = current
        save()
        return id
    }

    private func disconnect(_ id: String) {
        var current = loadFolders()
        if let folder = current.first(where: { $0.id == id }) {
            folder.url?.stopAccessingSecurityScopedResource()
        }
        current.removeAll { $0.id == id }
        folders = current
        opened.remove(id)
        save()
    }

    private func save() {
        let saved = (folders ?? []).map { Saved(id: $0.id, bookmark: $0.bookmark) }
        UserDefaults.standard.set(try? JSONEncoder().encode(saved), forKey: Self.key)
    }

    /// For the page's scripted Library check: a folder in the app's own storage, as a person's would
    /// be, holding the pack in a sub-folder and a file the Library leaves out, connected.
    func connectTestFolder(bundle: URL) {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let folder = documents.appendingPathComponent("library-test", isDirectory: true)
        let inner = folder.appendingPathComponent("sub", isDirectory: true)
        let pack = inner.appendingPathComponent("pack.step")
        if !FileManager.default.fileExists(atPath: pack.path) {
            try? FileManager.default.createDirectory(at: inner, withIntermediateDirectories: true)
            try? FileManager.default.copyItem(
                at: bundle.appendingPathComponent("spike/fixtures/step/pack.step"), to: pack)
            FileManager.default.createFile(atPath: folder.appendingPathComponent("notes.txt").path, contents: Data())
        }
        _ = connect(folder)
    }

    /// For the page's scripted gallery check: a folder put in the app's own storage (as
    /// `devicectl device copy to` puts one in Documents), connected as a person's would be.
    func connectDocumentsFolder(named name: String) {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let folder = documents.appendingPathComponent(name, isDirectory: true)
        guard FileManager.default.fileExists(atPath: folder.path) else {
            return
        }
        _ = connect(folder)
    }

    // MARK: Pickers

    private func present(_ kind: Picking, _ replyHandler: @escaping (Any?, String?) -> Void) {
        guard reply == nil, let presenter = webView?.window?.rootViewController else {
            replyHandler(nil, "A picker is open already.")
            return
        }
        let types: [UTType] =
            switch kind {
            case .folder: [.folder]
            case .files:
                [UTType("org.iso.step"), UTType("public.standard-tesselated-geometry-format"),
                 UTType("com.aalok.geometryview.cache")].compactMap { $0 }
            }
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: false)
        picker.allowsMultipleSelection = kind == .files
        picker.delegate = self
        picking = kind
        reply = replyHandler
        var top = presenter
        while let shown = top.presentedViewController {
            top = shown
        }
        top.present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        let answer: Any?
        switch picking {
        case .folder:
            answer = urls.first.flatMap { connect($0) }
        case .files:
            answer = urls.map { url -> [String: Any] in
                let id = opened.add(url)
                let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
                let name = url.lastPathComponent
                return [
                    "path": "/open/\(id)/\(name.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? name)",
                    "name": name,
                    "size": values?.fileSize ?? 0,
                    "lastModified": (values?.contentModificationDate?.timeIntervalSince1970 ?? 0) * 1000,
                ]
            }
        case nil:
            answer = nil
        }
        finish(answer)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        finish(nil)
    }

    private func finish(_ answer: Any?) {
        let reply = reply
        self.reply = nil
        picking = nil
        reply?(answer, nil)
    }
}
