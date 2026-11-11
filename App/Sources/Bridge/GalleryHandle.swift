import Foundation
import GeometryViewEngine

/// A file or folder found by a scan, its path relative to the gallery's root.
struct ScanEntry: Sendable {
    let path: String
    let folder: Bool
    let bytes: UInt64
    let modified: Double
}

/// The gallery's layout core (gallery.h) on the Swift side: the folder tree, the folder in view and
/// its display list, which GalleryView paints.
@MainActor
final class GalleryHandle {
    private let gallery: OpaquePointer

    init() {
        gallery = geo_gallery_create()
    }

    isolated deinit {
        geo_gallery_destroy(gallery)
    }

    func setEntries(title: String, entries: [ScanEntry]) {
        // The paths' C strings live for the call.
        let paths = entries.map { strdup($0.path) }
        defer { paths.forEach { free($0) } }
        var list = entries.enumerated().map { index, entry in
            GeoGalleryEntry(
                path: paths[index],
                folder: entry.folder,
                bytes: entry.bytes,
                modified: entry.modified
            )
        }
        geo_gallery_set_entries(gallery, title, &list, UInt32(list.count))
    }

    var fileCount: UInt32 { geo_gallery_file_count(gallery) }

    func path(of file: UInt32) -> String {
        String(cString: geo_gallery_file_path(gallery, file))
    }

    func file(at path: String) -> UInt32? {
        let index = geo_gallery_file_at(gallery, path)
        return index == UInt32.max ? nil : index
    }

    func setFacts(_ file: UInt32, _ facts: LibraryFacts) {
        facts.product.withCString { product in
            var f = GeoGalleryFacts(
                size: (facts.size.0, facts.size.1, facts.size.2),
                has_box: facts.hasBox,
                estimated: facts.estimated,
                parts: facts.parts,
                product: product,
                triangles: facts.triangles
            )
            geo_gallery_set_facts(gallery, file, &f)
        }
    }

    func setPicture(_ file: UInt32, _ picture: GeoGalleryPicture) {
        var p = picture
        geo_gallery_set_picture(gallery, file, &p)
    }

    func setFailure(_ file: UInt32, _ message: String) {
        geo_gallery_set_failure(gallery, file, message)
    }

    /// The view as a poster, printed with `date` in its foot.
    func setPoster(_ poster: Bool, date: String) {
        geo_gallery_set_poster(gallery, poster, date)
    }

    var poster: Bool { geo_gallery_poster(gallery) }

    /// A file being read, 0 to 1, or negative once it is not.
    func setReading(_ file: UInt32, _ fraction: Float) {
        geo_gallery_set_reading(gallery, file, fraction)
    }

    /// A file changed on disk: a Δ until seen, its picture faded until a new one comes.
    func setChanged(_ file: UInt32, _ changed: Bool) {
        geo_gallery_set_changed(gallery, file, changed)
    }

    func setStyle(_ style: UInt32) {
        geo_gallery_set_style(gallery, style)
    }

    func setViewport(width: CGFloat, height: CGFloat) {
        geo_gallery_set_viewport(gallery, Float(width), Float(height))
    }

    func setScroll(_ top: CGFloat) {
        geo_gallery_set_scroll(gallery, Float(top))
    }

    @discardableResult
    func setFolder(_ path: String) -> Bool {
        geo_gallery_set_folder(gallery, path)
    }

    var folder: String { String(cString: geo_gallery_folder(gallery)) }
    var title: String { String(cString: geo_gallery_folder_title(gallery)) }
    var subtitle: String { String(cString: geo_gallery_folder_subtitle(gallery)) }

    @discardableResult
    func layout() -> Bool {
        geo_gallery_layout(gallery)
    }

    var items: UnsafeBufferPointer<GeoGalleryItem> {
        UnsafeBufferPointer(
            start: geo_gallery_items(gallery),
            count: Int(geo_gallery_item_count(gallery))
        )
    }

    /// An item's text, from the gallery's text buffer.
    func text(of item: GeoGalleryItem) -> String {
        let base = UnsafeRawPointer(geo_gallery_text(gallery)!).advanced(by: Int(item.text))
        let bytes = UnsafeRawBufferPointer(start: base, count: Int(item.text_length))
        return String(decoding: bytes, as: UTF8.self)
    }

    var contentHeight: CGFloat { CGFloat(geo_gallery_content_height(gallery)) }
    var picturePixels: UInt32 { geo_gallery_picture_pixels(gallery) }

    func hit(_ point: CGPoint) -> GeoGalleryHit {
        geo_gallery_hit(gallery, Float(point.x), Float(point.y))
    }

    @discardableResult
    func hover(kind: UInt32, target: UInt32) -> Bool {
        geo_gallery_hover(gallery, kind, target)
    }

    func click(kind: UInt32, target: UInt32, twice: Bool) -> GeoGalleryAction {
        geo_gallery_click(gallery, kind, target, twice)
    }

    func key(_ key: GeoGalleryKey) -> GeoGalleryAction {
        geo_gallery_key(gallery, key.rawValue)
    }

    var selected: UInt32? {
        let file = geo_gallery_selected(gallery)
        return file == UInt32.max ? nil : file
    }

    // MARK: Views, search, Peek and motion

    /// The views that may be shown, a bit each (1 << GeoViewPlates ...).
    var views: UInt32 {
        get { geo_gallery_views(gallery) }
        set { geo_gallery_set_views(gallery, newValue) }
    }

    /// Shows a view if it is enabled; the folder in view remembers it.
    @discardableResult
    func setView(_ view: UInt32) -> Bool {
        geo_gallery_set_view(gallery, view)
    }

    var view: UInt32 { geo_gallery_view(gallery) }

    func setSheet(scale: UInt32, cell: CGFloat, sort: UInt32) {
        geo_gallery_set_sheet(gallery, scale, Float(cell), sort)
    }

    var search: String {
        get { String(cString: geo_gallery_search(gallery)) }
        set { geo_gallery_set_search(gallery, newValue) }
    }

    /// How many files the search found, or nil with no search.
    var matches: UInt32? {
        let n = geo_gallery_matches(gallery)
        return n == UInt32.max ? nil : n
    }

    var peeking: UInt32? {
        let file = geo_gallery_peeking(gallery)
        return file == UInt32.max ? nil : file
    }

    func peek(_ file: UInt32?) {
        geo_gallery_peek(gallery, file ?? UInt32.max)
    }

    /// Where to scroll so the selection shows, or nil if it shows already.
    var reveal: CGFloat? {
        let top = geo_gallery_reveal(gallery)
        return top < 0 ? nil : CGFloat(top)
    }

    /// What the last layout plays against the one before.
    var transition: GeoGalleryTransition { geo_gallery_transition(gallery) }

    var tracks: [GeoGalleryTrack] {
        let count = geo_gallery_tracks(gallery, nil, 0)
        var out = [GeoGalleryTrack](repeating: GeoGalleryTrack(), count: Int(count))
        _ = geo_gallery_tracks(gallery, &out, count)
        return out
    }

    func wanted() -> [GeoGalleryWant] {
        let count = geo_gallery_wanted(gallery, nil, 0)
        var out = [GeoGalleryWant](repeating: GeoGalleryWant(), count: Int(count))
        _ = geo_gallery_wanted(gallery, &out, count)
        return out
    }

    /// The alignment groups painted more than `tolerance` apart, and how many were measured.
    func check(lines: [Float], columns: [Float], tolerance: Float) -> (
        over: [GeoGalleryGroupSpread], checked: UInt32
    ) {
        var checked: UInt32 = 0
        var out = [GeoGalleryGroupSpread](repeating: GeoGalleryGroupSpread(), count: 64)
        let over = geo_gallery_check(gallery, lines, columns, tolerance, &out, 64, &checked)
        return (Array(out.prefix(Int(min(over, 64)))), checked)
    }
}
