import AppKit
import GeometryViewEngine
import ImageIO

/// A connected folder's gallery: its scan, the library service drawing what is in view first, and
/// the view painting the layout as facts and pictures arrive.
@MainActor
final class GalleryController {
    let gallery = GalleryHandle()
    let view: GalleryView
    private let library = LibraryHandle()
    private(set) var root: URL?
    /// A scan of the root is under way.
    private(set) var scanning = false

    /// What is known by path relative to the root, kept across a new scan of the same folder.
    private var facts: [String: LibraryFacts] = [:]
    private var pictures: [String: (png: String, picture: GeoGalleryPicture, style: UInt32)] = [:]
    private var failures: [String: String] = [:]
    private var entries: [String: ScanEntry] = [:]
    private var reloadScheduled = false
    private var wantScheduled = false
    private var picturesPending = 0
    private var styleRaw: UInt32 = 0

    /// Asked to open a file in the viewer.
    var onOpen: ((URL) -> Void)?
    /// Asked to close the gallery: Esc at its top folder.
    var onLeave: (() -> Void)?
    /// The folder in view changed, for the masthead.
    var onFolderChanged: (() -> Void)?

    init() {
        view = GalleryView(gallery: gallery)
        view.onAction = { [weak self] action in self?.act(action) }
        view.onViewChanged = { [weak self] in self?.scheduleWant() }
        library?.onEvent = { [weak self] event in self?.receive(event) }
    }

    var title: String { gallery.title }
    var subtitle: String { gallery.subtitle }

    /// Shows `folder` inside the connected folder `root`: at once from the last listing kept, then
    /// as a fresh scan finds it.
    func show(root: URL, folder: URL) {
        let relative = Self.relative(folder, to: root) ?? ""
        if root != self.root {
            self.root = root
            facts.removeAll()
            pictures.removeAll()
            failures.removeAll()
            view.clearImages()
            if let kept = library?.keptListing(root) {
                load(kept)
            } else {
                load([])
            }
            scanning = true
            Task { [weak self] in
                let entries = await self?.library?.scan(root)
                self?.scanning = false
                if let entries {
                    self?.rescanned(entries)
                }
            }
        }
        gallery.setFolder(relative)
        view.scrollToTop()
        view.reload()
        onFolderChanged?()
        scheduleWant()
    }

    func setStyle(_ style: GeoStyle) {
        styleRaw = style.rawValue
        library?.setPicture(
            style: style,
            colorMode: GeoColorParts,
            pixels: gallery.picturePixels,
            lineScale: min(1, max(0.5, Float(gallery.picturePixels) / 2 / 320))
        )
        view.setStyle(style)
        scheduleWant()
    }

    func stats() -> GeoLibraryStats? {
        library?.stats()
    }

    /// The folders right inside the folder in view, by path relative to the root.
    func subfolders() -> [String] {
        let folder = gallery.folder
        return entries.values.filter { entry in
            guard entry.folder else {
                return false
            }
            let parent = (entry.path as NSString).deletingLastPathComponent
            return parent == folder
                && (entries.values.contains { !$0.folder && $0.path.hasPrefix(entry.path + "/") })
        }.map(\.path).sorted()
    }

    /// Pictures still to arrive for files in view, for the checks.
    var waitingInView: Int {
        gallery.wanted().filter { want in
            let path = gallery.path(of: want.file)
            return want.band <= 2 && pictures[path]?.style != styleRaw && failures[path] == nil
        }.count
    }

    // MARK: Scans

    private func load(_ list: [ScanEntry]) {
        let folder = gallery.folder
        entries = Dictionary(list.map { ($0.path, $0) }, uniquingKeysWith: { a, _ in a })
        gallery.setEntries(title: root?.lastPathComponent ?? "", entries: list)
        gallery.setFolder(folder)
        // What was known of each file is known still.
        for (path, f) in facts {
            if let file = gallery.file(at: path) {
                gallery.setFacts(file, f)
            }
        }
        for (path, p) in pictures {
            if let file = gallery.file(at: path) {
                gallery.setPicture(file, p.picture)
                loadImage(p.png, for: file)
            }
        }
        for (path, message) in failures {
            if let file = gallery.file(at: path) {
                gallery.setFailure(file, message)
            }
        }
    }

    private func rescanned(_ list: [ScanEntry]) {
        // Unchanged, the layout stands.
        if list.count == entries.count,
            list.allSatisfy({ entries[$0.path]?.modified == $0.modified })
        {
            return
        }
        view.clearImages()
        load(list)
        view.reload()
        onFolderChanged?()
        scheduleWant()
    }

    // MARK: The library

    private func scheduleWant() {
        guard !wantScheduled else {
            return
        }
        wantScheduled = true
        DispatchQueue.main.async { [weak self] in
            self?.wantScheduled = false
            self?.sendWants()
        }
    }

    private func sendWants() {
        guard let root, let library else {
            return
        }
        let wants = gallery.wanted().map { want -> LibraryHandle.Want in
            let path = gallery.path(of: want.file)
            let entry = entries[path]
            return LibraryHandle.Want(
                path: root.appendingPathComponent(path).path,
                band: GeoLibraryBand(rawValue: want.band),
                order: want.order,
                bytes: entry?.bytes ?? 0,
                modified: entry?.modified ?? 0,
                folderColor: want.folder_color
            )
        }
        library.want(wants)
    }

    private func receive(_ event: LibraryEvent) {
        guard let root else {
            return
        }
        let relative: (String) -> String? = { path in
            Self.relative(URL(fileURLWithPath: path), to: root)
        }
        switch event {
        case .facts(let path, let f):
            guard let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            facts[rel] = f
            gallery.setFacts(file, f)
        case .picture(let path, let png, let picture):
            guard let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            pictures[rel] = (png, picture, styleRaw)
            gallery.setPicture(file, picture)
            loadImage(png, for: file)
        case .failed(let path, let message):
            guard let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            failures[rel] = message
            gallery.setFailure(file, message)
        case .progress:
            return
        }
        scheduleReload()
    }

    /// Decodes a picture off the main thread, then paints it.
    private func loadImage(_ png: String, for file: UInt32) {
        picturesPending += 1
        Task.detached(priority: .userInitiated) {
            let url = URL(fileURLWithPath: png) as CFURL
            let image = CGImageSourceCreateWithURL(url, nil).flatMap {
                CGImageSourceCreateImageAtIndex(
                    $0,
                    0,
                    [kCGImageSourceShouldCacheImmediately: true] as CFDictionary
                )
            }
            let sendable = UncheckedImage(image: image)
            await MainActor.run { [weak self] in
                guard let self else {
                    return
                }
                picturesPending -= 1
                view.setImage(sendable.image, for: file)
                scheduleReload(repaint: true)
            }
        }
    }

    private var repaintScheduled = false

    /// Facts and pictures arrive in bursts: one layout for each burst.
    private func scheduleReload(repaint: Bool = false) {
        repaintScheduled = repaintScheduled || repaint
        guard !reloadScheduled else {
            return
        }
        reloadScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            guard let self else {
                return
            }
            reloadScheduled = false
            if repaintScheduled {
                repaintScheduled = false
                view.repaint()
            }
            view.reload()
        }
    }

    // MARK: Actions

    private func act(_ action: GeoGalleryAction) {
        switch action.kind {
        case GeoActionEnterFolder.rawValue:
            onFolderChanged?()
            scheduleWant()
        case GeoActionOpenFile.rawValue, GeoActionPeek.rawValue:
            // Peek comes in G3; until then Space opens, as ⏎ does.
            if let root {
                onOpen?(root.appendingPathComponent(gallery.path(of: action.target)))
            }
        case GeoActionLeave.rawValue:
            onLeave?()
        default:
            break
        }
    }

    static func relative(_ url: URL, to root: URL) -> String? {
        let base = root.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        if path == base {
            return ""
        }
        guard path.hasPrefix(base + "/") else {
            return nil
        }
        return String(path.dropFirst(base.count + 1))
    }
}

private struct UncheckedImage: @unchecked Sendable {
    let image: CGImage?
}
