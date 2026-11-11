// Catalogue: the folder in view typeset as a printed parts catalogue, in two-page spreads
// (Docs/design/gallery-spec.md, "Views"): an inside cover, a title page, contents that show every
// part at once, an opener for each chapter (each subfolder), a plate and tables of measured sizes
// for each folder that holds files, and an index. Pages take the drawing style's own sheet. Only
// the spread in view is set, fitted to the view; ← and → turn it.
#include "gallery/catalogue.h"

#include "gallery/draw.h"
#include "gallery/names.h"
#include "gallery/text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace geo::gallery {
namespace {

constexpr float kPageW = 504.0f; // a page: its text 424 wide, 40 either side
constexpr float kPageH = 665.0f;
constexpr float kText = 424.0f;
constexpr float kMargin = 40.0f;
constexpr usize kRows = 23; // rows of a table a page

/// A count in thousands or millions, as the board writes triangles: 864, 1.2k, 34k, 1.5M.
std::string kilo(u64 n) {
    if (n >= 1'000'000) {
        return std::format("{:.1f}M", double(n) / 1e6);
    }
    if (n >= 1'000) {
        return n >= 10'000 ? std::format("{:.0f}k", double(n) / 1e3)
                           : std::format("{:.1f}k", double(n) / 1e3);
    }
    return std::to_string(n);
}

/// Bytes on disk, as the board writes them: 820 B, 34 KB, 1.4 MB, 52 MB.
std::string bytesText(u64 n) {
    if (n >= 1'000'000) {
        return n >= 10'000'000 ? std::format("{:.0f} MB", double(n) / 1e6)
                               : std::format("{:.1f} MB", double(n) / 1e6);
    }
    if (n >= 1'000) {
        return std::format("{:.0f} KB", double(n) / 1e3);
    }
    return std::format("{} B", n);
}

u32 partsOf(const File& file) {
    return file.facts ? std::max<u32>(1, file.facts->parts) : 1;
}

/// A name that is more than a number: "M12", "bearing_608", not "0613" or "12x40".
bool worded(const File& file) {
    if (!file.names.named) {
        return false;
    }
    return std::ranges::any_of(file.names.label, [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 && c != 'x' && c != 'X';
    });
}

/// The board's typical part of a set: of those drawn, the ones with the usual number of parts,
/// those with names when there are a few, then the middle one by size. kNone when none is drawn.
u32 typicalOf(const Tree& tree, const std::vector<u32>& files) {
    const std::vector<u32> ok = drawable(tree, files);
    if (ok.empty()) {
        return kNone;
    }
    std::map<u32, usize> tally;
    for (const u32 f : ok) {
        ++tally[partsOf(tree.file(f))];
    }
    u32 usual = 0;
    usize most = 0;
    for (const auto& [parts, n] : tally) { // fewer parts first, so a tie keeps the fewer
        if (n > most) {
            most = n;
            usual = parts;
        }
    }
    std::vector<u32> pool, named;
    for (const u32 f : ok) {
        if (partsOf(tree.file(f)) == usual) {
            pool.push_back(f);
            if (worded(tree.file(f))) {
                named.push_back(f);
            }
        }
    }
    if (named.size() >= std::min<usize>(3, pool.size())) {
        pool = std::move(named);
    }
    const std::vector<u32> sorted = bySize(tree, pool);
    return sorted[(sorted.size() - 1) / 2];
}

/// `n` of them evenly across their sizes, smallest to largest.
std::vector<u32> sampleOf(const Tree& tree, const std::vector<u32>& files, usize n) {
    const std::vector<u32> sorted = bySize(tree, drawable(tree, files));
    if (sorted.size() <= n) {
        return sorted;
    }
    std::vector<u32> out;
    for (usize i = 0; i < n; ++i) {
        out.push_back(sorted[usize(std::lround(double(i) * double(sorted.size() - 1) /
                                               double(std::max<usize>(1, n - 1))))]);
    }
    return out;
}

enum class Kind {
    Inside,
    Cover,
    Contents,
    Glance,
    Opener,
    Composition,
    Plate,
    Table,
    Notes,
    Index
};

struct Chapter {
    u32 no = 0;
    std::string title;
    u32 folder = kNone;
    std::vector<u32> families;
    u32 count = 0;
    bool plain = false; // a folder of files alone, or one folder that is the chapter itself
    bool only = false;  // the book's one chapter: a folder of files alone
    u32 page = 0;
};

struct Family {
    u32 folder = kNone;
    usize chapter = 0;
    std::string title;
    std::vector<u32> files;
    u32 page = 0;
};

struct Line {
    std::string letter; // a letter's heading, or empty
    std::string name;
    std::vector<u32> pages;
};

struct Page {
    Kind kind = Kind::Notes;
    usize chapter = 0, family = 0;
    usize from = 0;                         // a table's first row
    bool first = false;                     // the first contents or index page
    std::vector<usize> items;               // contents: indices into Book::contents
    std::vector<std::vector<Line>> columns; // index
};

Page pageOfKind(Kind kind, usize chapter = 0, usize family = 0, usize from = 0) {
    Page page;
    page.kind = kind;
    page.chapter = chapter;
    page.family = family;
    page.from = from;
    return page;
}

struct ContentsItem {
    enum { ChapterHead, FamilyLine, IndexLine } kind;
    usize index = 0;
};

struct Book {
    std::string title;
    std::vector<Chapter> chapters;
    std::vector<Family> families;
    std::vector<ContentsItem> contents;
    std::vector<Page> pages;
    std::map<u32, u32> pageOf; // file -> the page its row is on
    u32 indexAt = 0;
};

/// A file known only by a number or an id is indexed by its folder, not by its name.
bool numbery(const File& f) {
    usize digits = 0;
    for (const char c : f.names.label) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            ++digits;
        } else if (!std::strchr("#. -_x", c)) {
            return false;
        }
    }
    return digits >= 5;
}

Book bookOf(const Tree& tree, u32 folder) {
    Book book;
    book.title = tree.titleOf(folder);
    const Folder& f = tree.folder(folder);
    // Chapters: the folder's own files first, then each subfolder; a folder of files alone is one.
    if (!f.folders.empty()) {
        if (!f.files.empty()) {
            book.chapters.push_back({0, "In this folder", folder, {folder}, 0, true, false, 0});
        }
        for (const u32 sub : f.folders) {
            Chapter ch;
            ch.title = tree.titleOf(sub);
            ch.folder = sub;
            ch.families = tree.familiesUnder(sub);
            ch.plain = ch.families.size() == 1 && ch.families[0] == sub;
            if (!ch.families.empty()) {
                book.chapters.push_back(std::move(ch));
            }
        }
    } else if (!f.files.empty()) {
        book.chapters.push_back({0, book.title, folder, {folder}, 0, true, true, 0});
    }
    for (usize i = 0; i < book.chapters.size(); ++i) {
        Chapter& ch = book.chapters[i];
        ch.no = u32(i + 1);
        for (const u32 fam : ch.families) {
            Family family;
            family.folder = fam;
            family.chapter = i;
            family.files = bySize(tree, tree.folder(fam).files);
            if (fam == ch.folder) {
                family.title = tree.titleOf(fam);
            } else {
                const std::string inner = tree.folder(fam).path.substr(
                    tree.folder(ch.folder).path.size() + (ch.folder == 0 ? 0 : 1));
                family.title = folderTitle(inner);
            }
            ch.count += u32(family.files.size());
            book.families.push_back(std::move(family));
        }
    }
    book.pages.push_back(pageOfKind(Kind::Inside));
    book.pages.push_back(pageOfKind(Kind::Cover));
    // Contents: the mosaic, then a heading for each chapter and a line for each folder, flowed onto
    // as many pages as it takes, an even number.
    for (usize c = 0; c < book.chapters.size(); ++c) {
        if (!book.chapters[c].plain) {
            book.contents.push_back({ContentsItem::ChapterHead, c});
        }
        for (usize i = 0; i < book.families.size(); ++i) {
            if (book.families[i].chapter == c) {
                book.contents.push_back({ContentsItem::FamilyLine, i});
            }
        }
    }
    book.contents.push_back({ContentsItem::IndexLine, 0});
    {
        Page page = pageOfKind(Kind::Contents);
        page.first = true;
        float room = 300.0f;
        for (usize i = 0; i < book.contents.size(); ++i) {
            const float h = book.contents[i].kind == ContentsItem::ChapterHead ? 36.0f : 42.0f;
            if (h > room && !page.items.empty()) {
                book.pages.push_back(page);
                page = pageOfKind(Kind::Contents);
                room = 560.0f;
            }
            page.items.push_back(i);
            room -= h;
        }
        book.pages.push_back(page);
        if (book.pages.size() % 2 != 0) {
            book.pages.push_back(pageOfKind(Kind::Glance));
        }
    }
    const auto pad = [&] {
        if (book.pages.size() % 2 != 0) {
            book.pages.push_back(pageOfKind(Kind::Notes));
        }
    };
    for (usize c = 0; c < book.chapters.size(); ++c) {
        Chapter& ch = book.chapters[c];
        if (!ch.plain) {
            pad();
            ch.page = u32(book.pages.size());
            book.pages.push_back(pageOfKind(Kind::Opener, c));
            book.pages.push_back(pageOfKind(Kind::Composition, c));
        }
        for (usize i = 0; i < book.families.size(); ++i) {
            Family& fam = book.families[i];
            if (fam.chapter != c) {
                continue;
            }
            pad();
            fam.page = u32(book.pages.size());
            if (ch.plain && ch.page == 0) {
                ch.page = fam.page;
            }
            book.pages.push_back(pageOfKind(Kind::Plate, c, i));
            for (usize from = 0; from < std::max<usize>(1, fam.files.size()); from += kRows) {
                Page table = pageOfKind(Kind::Table, c, i, from);
                for (usize r = from; r < std::min(fam.files.size(), from + kRows); ++r) {
                    book.pageOf[fam.files[r]] = u32(book.pages.size());
                }
                book.pages.push_back(table);
            }
        }
    }
    pad();
    book.indexAt = u32(book.pages.size());
    // The index: every name with its pages; files known only by a number, by folder.
    std::map<std::string, std::vector<u32>, bool (*)(const std::string&, const std::string&)>
        byName([](const std::string& a, const std::string& b) { return compareNatural(a, b) < 0; });
    std::map<u32, std::pair<usize, std::vector<u32>>> byFolder; // family -> count, pages
    for (usize i = 0; i < book.families.size(); ++i) {
        for (const u32 file : book.families[i].files) {
            const u32 page = book.pageOf[file];
            const File& ff = tree.file(file);
            if (numbery(ff)) {
                auto& [n, pages] = byFolder[u32(i)];
                ++n;
                if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
                    pages.push_back(page);
                }
                continue;
            }
            auto& pages = byName[ff.names.label];
            if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
                pages.push_back(page);
            }
        }
    }
    std::vector<Line> lines;
    std::string letter;
    for (const auto& [name, pages] : byName) {
        const std::string head =
            !name.empty() && std::isalpha(static_cast<unsigned char>(name[0]))
                ? std::string(1, char(std::toupper(static_cast<unsigned char>(name[0]))))
                : "0\xE2\x80\x93"
                  "9";
        if (head != letter) {
            lines.push_back({head, "", {}});
            letter = head;
        }
        lines.push_back({"", name, pages});
    }
    if (!byFolder.empty()) {
        lines.push_back({"Known by number", "", {}});
        for (const auto& [fam, entry] : byFolder) {
            lines.push_back({"",
                             book.families[fam].title + ", " + grouped(entry.first) + " numbered",
                             entry.second});
        }
    }
    // Two columns a page, 30 lines each; an even number of pages.
    std::vector<std::vector<Line>> columns;
    std::vector<Line> column;
    for (const Line& line : lines) {
        if (column.size() >= 30) {
            columns.push_back(std::move(column));
            column.clear();
        }
        column.push_back(line);
    }
    if (!column.empty() || columns.empty()) {
        columns.push_back(std::move(column));
    }
    for (usize i = 0; i < columns.size(); i += 2) {
        Page page = pageOfKind(Kind::Index);
        page.first = i == 0;
        page.columns.push_back(columns[i]);
        if (i + 1 < columns.size()) {
            page.columns.push_back(columns[i + 1]);
        }
        book.pages.push_back(std::move(page));
    }
    pad();
    return book;
}

} // namespace

class Catalogue : public Draw {
public:
    Catalogue(const LayoutInput& input, Display& display, LayoutResult& result, const Book& book)
        : Draw(input, display, result), book_(book) {}

    // The spread's scale and where it stands in the view.
    float s = 1.0f, ox = 0.0f, oy = 0.0f;

    TextStyle st(float size, u16 weight = 400, float tracking = 0.0f, bool upper = false) const {
        return {size * s, weight, tracking, upper};
    }
    float X(int side, float x) const { return ox + (float(side) * kPageW + x) * s; }
    float Y(float y) const { return oy + y * s; }

    void spread(u32 index) {
        for (int side = 0; side < 2; ++side) {
            const u32 at = index * 2 + u32(side);
            if (at >= book_.pages.size()) {
                continue;
            }
            page(at, side);
        }
        // The fold: a faint line down the middle.
        out.line(X(1, 0.0f), Y(0.0f), X(1, 0.0f), Y(kPageH), palette.rule, 1.0f);
        tabs(index);
    }

    void page(u32 at, int side) {
        const Page& p = book_.pages[at];
        out.rect(X(side, 0.0f), Y(0.0f), kPageW * s, kPageH * s, 0, palette.on, 1.0f, 0.0f,
                 GeoItemSheet);
        runningHead(p, side);
        if (at >= 2) {
            // The folio, at the outer foot.
            const TextStyle folio = st(10.0f, 500);
            const std::string n = grouped(at);
            const float w = textWidth(n, folio);
            out.text(n, folio, palette.muted,
                     side == 0 ? X(0, kMargin) : X(1, kPageW - kMargin) - w, Y(kPageH - 26.0f), w);
        }
        switch (p.kind) {
        case Kind::Inside:
            inside(side);
            break;
        case Kind::Cover:
            cover(side);
            break;
        case Kind::Contents:
            contents(p, side);
            break;
        case Kind::Glance:
            glance(side);
            break;
        case Kind::Opener:
            opener(p, side);
            break;
        case Kind::Composition:
            composition(p, side);
            break;
        case Kind::Plate:
            plate(p, side);
            break;
        case Kind::Table:
            table(p, side);
            break;
        case Kind::Notes:
            notes(side);
            break;
        case Kind::Index:
            index(p, side);
            break;
        }
    }

    void runningHead(const Page& p, int side) {
        std::string text;
        switch (p.kind) {
        case Kind::Contents:
        case Kind::Glance:
            text = "Contents";
            break;
        case Kind::Index:
            text = "Index";
            break;
        case Kind::Opener:
        case Kind::Composition:
            text =
                side == 0 ? book_.title : std::format("Chapter {}", book_.chapters[p.chapter].no);
            break;
        case Kind::Plate:
        case Kind::Table: {
            const Chapter& ch = book_.chapters[p.chapter];
            text = side == 0 ? (ch.only ? book_.title : ch.title) : book_.families[p.family].title;
            break;
        }
        default:
            return;
        }
        const TextStyle head = st(10.0f, 600, 0.14f, true);
        const float baseline = Y(30.0f);
        const u32 g = headGroup();
        out.group(g);
        out.text(text, head, palette.muted, X(side, kMargin), baseline, kText * s,
                 side == 0 ? 0 : 2);
        out.group(0);
        out.line(X(side, kMargin), Y(38.0f), X(side, kMargin + kText), Y(38.0f), palette.rule,
                 1.0f);
    }

    /// One group for the running heads of the spread, so both pages' heads share a line.
    u32 headGroup() {
        if (headGroup_ == 0) {
            headGroup_ = newGroup();
        }
        return headGroup_;
    }

    void eyebrow(const std::string& text, int side, float y) {
        out.text(text, st(10.0f, 600, 0.16f, true), palette.muted, X(side, kMargin), Y(y),
                 kText * s);
    }

    /// Parts standing on baselines at one scale, wrapping into up to `maxRows` rows, in a box `w` ×
    /// `h` at (x, y) page points; names under them with `labels`. Returns the millimetres a point.
    /// The fewest millimetres a page point at which these files' pictures stay sharp in the view:
    /// no more than kMaxPointsPerPixel view points a pixel, the page being drawn `s` times its own.
    double sharpest(const std::vector<u32>& files) const {
        double mm = 0;
        for (const u32 f : files) {
            if (const auto& p = tree.file(f).picture; p && p->mmPerPixel > 0) {
                mm = std::max(mm, p->mmPerPixel * double(s) / double(kMaxPointsPerPixel));
            }
        }
        return mm;
    }

    double stand(const std::vector<u32>& files, int side, float x, float y, float w, float h,
                 usize maxRows, bool labels) {
        const std::vector<u32> ok = drawable(tree, files);
        if (ok.empty()) {
            return 0;
        }
        const float labH = labels ? 24.0f : 0.0f, gap = 12.0f;
        // The scale that fits the row of them in maxRows rows.
        // Each part's width with its ink either side: its shadow is drawn too.
        const auto inkW = [&](u32 f) {
            const Drawn d = *drawnOf(tree.file(f));
            return d.w + d.left + d.right;
        };
        double totalW = 0, tallest = 0;
        for (const u32 f : ok) {
            const Drawn d = *drawnOf(tree.file(f));
            totalW += inkW(f);
            tallest = std::max(tallest, d.h + d.below);
        }
        const usize rows = std::min<usize>(maxRows, std::max<usize>(1, ok.size()));
        const double rowW = double(w) * double(rows) - double(gap) * double(ok.size());
        const double rowH = (double(h) - double(rows) * double(labH)) / double(rows) - 8.0;
        double mm = std::max(totalW / std::max(1.0, rowW), tallest / std::max(1.0, rowH));
        mm = std::max(mm, sharpest(ok));
        // Fill rows left to right, each row centred. The last row takes what is left, so while a
        // row is wider than the box, draw them all smaller and fill again.
        std::vector<std::vector<u32>> lines;
        for (int tries = 0; tries < 24; ++tries) {
            lines.assign(1, {});
            float used = 0, widest = 0;
            for (const u32 f : ok) {
                const float pw = float(inkW(f) / mm);
                if (!lines.back().empty() && used + pw > w && lines.size() < rows) {
                    lines.emplace_back();
                    used = 0;
                }
                lines.back().push_back(f);
                used += pw + gap;
                widest = std::max(widest, used - gap);
            }
            if (widest <= w + 0.01f) {
                break;
            }
            mm *= std::max(1.02, double(widest) / double(w));
        }
        const float lineH = h / float(lines.size());
        for (usize r = 0; r < lines.size(); ++r) {
            float width = -gap;
            for (const u32 f : lines[r]) {
                width += float(inkW(f) / mm) + gap;
            }
            float px = x + (w - width) / 2.0f;
            const float base = y + float(r + 1) * lineH - labH;
            const u32 g = newGroup(), lg = newGroup();
            for (const u32 f : lines[r]) {
                const Drawn d = *drawnOf(tree.file(f));
                const float pw = float(inkW(f) / mm);
                out.target(GeoTargetPage, book_.pageOf.count(f) ? book_.pageOf.at(f) : 0);
                out.key(keyOf("part", f));
                part(f, X(side, px + float(d.left / mm)), Y(base) - float(d.below / mm) * s, mm / s,
                     g);
                lastLine(Y(base));
                out.key(0);
                if (labels) {
                    out.group(lg);
                    out.text(tree.file(f).names.label, st(9.0f), palette.on, X(side, px - 6.0f),
                             Y(base + 13.0f), (pw + 12.0f) * s, 1);
                    out.group(0);
                }
                out.target(GeoTargetNone, 0);
                px += pw + gap;
            }
        }
        return mm;
    }

    void inside(int side) {
        eyebrow("Frontispiece", side, 70.0f);
        if (book_.families.size() == 1) {
            // One folder: its typical part, large, and what it is.
            const Family& fam = book_.families[0];
            const FamilyScale split =
                familyScale(tree, drawable(tree, fam.files), 1.0f, 1.0f, 0.8, 3.0);
            const u32 t = typicalOf(tree, split.normal.empty() ? fam.files : split.normal);
            if (t != kNone) {
                figure(t, side, 90.0f, 360.0f);
                figureCaption(t, side, 476.0f);
            }
        } else {
            // A part from each folder, those within 8 times the middle one's size, to one scale.
            std::vector<u32> reps;
            for (const Family& fam : book_.families) {
                if (const u32 t = typicalOf(tree, fam.files); t != kNone) {
                    reps.push_back(t);
                }
            }
            std::vector<double> sizes;
            for (const u32 t : reps) {
                sizes.push_back(tree.file(t).size());
            }
            std::sort(sizes.begin(), sizes.end());
            const double aim = sizes.empty() ? 1.0 : std::max(1e-6, sizes[(sizes.size() - 1) / 2]);
            std::vector<u32> pick;
            for (const u32 t : reps) {
                if (std::abs(std::log(std::max(1e-6, tree.file(t).size()) / aim)) < std::log(8.0)) {
                    pick.push_back(t);
                }
            }
            pick = bySize(tree, pick);
            if (pick.size() < 3) {
                std::vector<u32> all;
                for (const Family& fam : book_.families) {
                    all.insert(all.end(), fam.files.begin(), fam.files.end());
                }
                pick = sampleOf(tree, all, 7);
            }
            const usize left = reps.size() > pick.size() ? reps.size() - pick.size() : 0;
            const double mm =
                stand(pick, side, kMargin, 90.0f, kText, 360.0f, pick.size() > 5 ? 2 : 1, true);
            caption("One part from each folder,", "drawn to one scale", side, 476.0f, mm);
            if (left > 0) {
                out.text(std::format("{} much larger or smaller {} left out", left,
                                     left == 1 ? "folder" : "folders"),
                         st(10.0f), palette.muted, X(side, kMargin), Y(491.0f), kText * s);
            }
        }
        const TextStyle body = st(10.5f);
        out.text("Set by GeometryView. Every picture is drawn from its file", body, palette.on,
                 X(side, kMargin), Y(520.0f), kText * s);
        out.text("and every size is measured from its geometry.", body, palette.on,
                 X(side, kMargin), Y(536.0f), kText * s);
        static const char* styles[] = {"Illustration", "Metal",     "Clay",
                                       "Blueprint",    "Technical", "Studio"};
        const std::pair<std::string, std::string> rows[] = {
            {"Drawn in", styles[std::min<u32>(in.style, 5)]},
            {"Pages", grouped(book_.pages.size() - 2) + " and a cover"},
            {"Set in", "IBM Plex Mono"}};
        float y = 580.0f;
        for (const auto& [k, v] : rows) {
            out.text(k, st(9.5f, 600, 0.08f, true), palette.muted, X(side, kMargin), Y(y),
                     120.0f * s);
            out.text(v, st(10.5f), palette.on, X(side, kMargin + 130.0f), Y(y),
                     (kText - 130.0f) * s);
            y += 16.0f;
        }
    }

    void cover(int side) {
        eyebrow("Parts catalogue", side, 90.0f);
        out.text(book_.title, st(30.0f, 600), palette.on, X(side, kMargin), Y(136.0f), kText * s);
        const bool one = book_.chapters.size() == 1 && book_.chapters[0].only;
        u32 total = 0;
        for (const Family& f : book_.families) {
            total += u32(f.files.size());
        }
        const std::string sub =
            one ? count(total, "part") + " \xC2\xB7 " +
                      rangeOf(tree,
                              book_.families.empty() ? std::vector<u32>{} : book_.families[0].files)
                : count(total, "part") + " in " + count(book_.families.size(), "folder");
        out.text(sub, st(12.0f), palette.muted, X(side, kMargin), Y(160.0f), kText * s);
        out.line(X(side, kMargin), Y(180.0f), X(side, kMargin + kText), Y(180.0f), palette.on,
                 1.5f);
        if (one && !book_.families.empty()) {
            stand(book_.families[0].files, side, kMargin, 200.0f, kText, 330.0f, 3, false);
        } else {
            float y = 210.0f;
            const u32 numbers = newGroup();
            for (usize i = 0; i < book_.chapters.size() && i < 9; ++i) {
                const Chapter& ch = book_.chapters[i];
                out.target(GeoTargetPage, ch.page);
                const u32 g = newGroup();
                out.group(g);
                out.text(std::format("{:02}", ch.no), st(11.0f, 600), palette.muted,
                         X(side, kMargin), Y(y), 30.0f * s);
                out.text(ch.title, st(13.0f, 600), palette.on, X(side, kMargin + 34.0f), Y(y),
                         (kText - 34.0f - 60.0f) * s);
                const std::string n = grouped(ch.count);
                const float nw = textWidth(n, st(11.0f));
                out.text(n, st(11.0f), palette.muted, X(side, kMargin + kText) - nw, Y(y), nw);
                out.group(0);
                out.target(GeoTargetNone, 0);
                (void)numbers;
                y += 30.0f;
            }
            if (book_.chapters.size() > 9) {
                out.text(std::format("and {} more chapters", book_.chapters.size() - 9), st(11.0f),
                         palette.muted, X(side, kMargin + 34.0f), Y(y), kText * s);
            }
        }
        out.text("GEOMETRYVIEW", st(10.0f, 600, 0.3f), palette.muted, X(side, kMargin),
                 Y(kPageH - 50.0f), kText * s, 2);
    }

    void contents(const Page& p, int side) {
        float y = 70.0f;
        if (p.first) {
            out.text("Contents", st(22.0f, 600), palette.on, X(side, kMargin), Y(84.0f), kText * s);
            // Every part in the folder, each fitted to its cell, in the order of the book.
            std::vector<u32> all;
            for (const Family& f : book_.families) {
                const std::vector<u32> ok = drawable(tree, f.files);
                all.insert(all.end(), ok.begin(), ok.end());
            }
            mosaic(all, side, kMargin, 100.0f, kText, 214.0f);
            y = 336.0f;
        }
        for (const usize i : p.items) {
            const ContentsItem& item = book_.contents[i];
            if (item.kind == ContentsItem::ChapterHead) {
                const Chapter& ch = book_.chapters[item.index];
                out.target(GeoTargetPage, ch.page);
                out.text(std::format("Chapter {} \xC2\xB7 {}", ch.no, ch.title),
                         st(10.5f, 600, 0.12f, true), palette.muted, X(side, kMargin), Y(y + 22.0f),
                         kText * s);
                out.target(GeoTargetNone, 0);
                y += 36.0f;
            } else if (item.kind == ContentsItem::FamilyLine) {
                const Family& fam = book_.families[item.index];
                leaderLine(fam.title, count(fam.files.size(), "part"), fam.page, side, y + 24.0f);
                y += 42.0f;
            } else {
                leaderLine("Index", "", book_.indexAt, side, y + 24.0f);
                y += 40.0f;
            }
        }
    }

    /// A contents line: a name, what it holds, dots, and the page it starts on, all turning to it.
    void leaderLine(const std::string& name, const std::string& sub, u32 page, int side,
                    float baseline) {
        out.target(GeoTargetPage, page);
        const u32 g = newGroup();
        out.group(g);
        const TextStyle nameStyle = st(13.0f, 600), subStyle = st(10.5f),
                        pageStyle = st(12.0f, 500);
        const std::string folio = grouped(page);
        const float fw = textWidth(folio, pageStyle);
        const float used = out.text(name, nameStyle, palette.on, X(side, kMargin), Y(baseline),
                                    (kText - 80.0f) * s);
        float x = X(side, kMargin) + used + 8.0f * s;
        if (!sub.empty()) {
            x += out.text(sub, subStyle, palette.muted, x, Y(baseline),
                          X(side, kMargin + kText) - fw - x) +
                 8.0f * s;
        }
        out.text(folio, pageStyle, palette.on, X(side, kMargin + kText) - fw, Y(baseline), fw);
        out.group(0);
        const float right = X(side, kMargin + kText) - fw - 6.0f * s;
        if (right > x + 6.0f) {
            out.line(x, Y(baseline - 1.0f), right, Y(baseline - 1.0f), palette.faint, 1.0f,
                     GeoItemDashed);
        }
        out.target(GeoTargetNone, 0);
    }

    /// Parts each fitted to a cell of a grid that fills `w` × `h`.
    void mosaic(const std::vector<u32>& files, int side, float x, float y, float w, float h) {
        if (files.empty()) {
            return;
        }
        const usize n = files.size();
        usize cols =
            std::max<usize>(1, usize(std::ceil(std::sqrt(double(n) * double(w) / double(h)))));
        usize rows = (n + cols - 1) / cols;
        while (rows > 1 && (n + cols) / cols < rows) {
            ++cols;
            rows = (n + cols - 1) / cols;
        }
        const float cw = w / float(cols), ch = h / float(rows);
        for (usize i = 0; i < n; ++i) {
            // Each part to fit its cell, its shadow and all.
            const Drawn d = *drawnOf(tree.file(files[i]));
            const double inkW = d.w + d.left + d.right, inkH = d.h + d.above + d.below;
            const double mm = std::max(
                {inkW / double(cw - 2.0f), inkH / double(ch - 2.0f), sharpest({files[i]})});
            const float pw = float(inkW / mm), ph = float(inkH / mm);
            const float cx = x + float(i % cols) * cw, cy = y + float(i / cols) * ch;
            out.target(GeoTargetPage, book_.pageOf.count(files[i]) ? book_.pageOf.at(files[i]) : 0);
            part(files[i], X(side, cx + (cw - pw) / 2.0f + float(d.left / mm)),
                 Y(cy + (ch + ph) / 2.0f - float(d.below / mm)), mm / s, 0, false);
            out.target(GeoTargetNone, 0);
        }
    }

    void glance(int side) {
        eyebrow("At a glance", side, 70.0f);
        out.text("Sizes, largest side, every part", st(13.0f, 600), palette.on, X(side, kMargin),
                 Y(96.0f), kText * s);
        // A histogram on a log scale of millimetres.
        std::vector<double> sizes;
        for (const Family& f : book_.families) {
            for (const u32 file : f.files) {
                if (const double size = tree.file(file).size(); size > 0) {
                    sizes.push_back(size);
                }
            }
        }
        if (sizes.empty()) {
            return;
        }
        const double lo =
            std::pow(10.0, std::floor(std::log10(*std::min_element(sizes.begin(), sizes.end()))));
        const double hi = std::pow(
            10.0, std::ceil(std::log10(*std::max_element(sizes.begin(), sizes.end()) * 1.0001)));
        constexpr usize bins = 24;
        std::vector<u32> counts(bins, 0);
        for (const double v : sizes) {
            const double t = (std::log10(v) - std::log10(lo)) / (std::log10(hi) - std::log10(lo));
            ++counts[std::min(bins - 1, usize(t * double(bins)))];
        }
        const u32 tallest = *std::max_element(counts.begin(), counts.end());
        const float base = 400.0f, room = 240.0f, bw = kText / float(bins);
        for (usize b = 0; b < bins; ++b) {
            if (counts[b] == 0) {
                continue;
            }
            const float h = room * float(counts[b]) / float(tallest);
            out.rect(X(side, kMargin + float(b) * bw + 1.0f), Y(base - h), (bw - 2.0f) * s, h * s,
                     palette.folders[0], palette.on, 1.0f);
        }
        out.line(X(side, kMargin), Y(base), X(side, kMargin + kText), Y(base), palette.on, 1.5f);
        for (double v = lo; v <= hi * 1.0001; v *= 10.0) {
            const float tx =
                kMargin +
                float((std::log10(v) - std::log10(lo)) / (std::log10(hi) - std::log10(lo))) * kText;
            const std::string label = millimetres(v);
            const float w = textWidth(label, st(9.0f));
            // Centred on its tick, but kept within the text at either end.
            const float lx =
                std::clamp(X(side, tx) - w / 2.0f, X(side, kMargin), X(side, kMargin + kText) - w);
            out.text(label, st(9.0f), palette.muted, lx, Y(base + 16.0f), w);
        }
        out.text(count(sizes.size(), "part") + " measured", st(10.5f), palette.muted,
                 X(side, kMargin), Y(base + 44.0f), kText * s);
    }

    void opener(const Page& p, int side) {
        const Chapter& ch = book_.chapters[p.chapter];
        eyebrow(std::format("Chapter {}", ch.no), side, 200.0f);
        out.target(GeoTargetFolder, ch.folder);
        out.text(ch.title, st(30.0f, 600), palette.on, X(side, kMargin), Y(250.0f), kText * s);
        out.target(GeoTargetNone, 0);
        out.text(count(ch.count, "part") + " in " + count(ch.families.size(), "folder"), st(12.0f),
                 palette.muted, X(side, kMargin), Y(276.0f), kText * s);
        out.line(X(side, kMargin), Y(296.0f), X(side, kMargin + 120.0f), Y(296.0f), palette.on,
                 1.5f);
    }

    void composition(const Page& p, int side) {
        eyebrow("In this chapter", side, 70.0f);
        float y = 100.0f;
        for (usize i = 0; i < book_.families.size(); ++i) {
            const Family& fam = book_.families[i];
            if (fam.chapter != p.chapter) {
                continue;
            }
            if (y > kPageH - 90.0f) {
                break;
            }
            leaderLine(fam.title, count(fam.files.size(), "part"), fam.page, side, y);
            y += 30.0f;
        }
    }

    /// A caption: `lead` in bold, then `rest`, on `y`; the scale bar at its right with `mm`.
    void caption(const std::string& lead, const std::string& rest, int side, float y, double mm) {
        const TextStyle bold = st(10.0f, 600), plain = st(10.0f);
        float barW = 0;
        if (mm > 0) {
            barW = scaleBarWidth(mm / s);
            scaleBar(mm / s, X(side, kMargin + kText) - barW, Y(y), palette.on, 0);
        }
        const u32 g = newGroup();
        out.group(g);
        const float leadW = textWidth(lead + " ", bold);
        out.text(lead, bold, palette.on, X(side, kMargin), Y(y), leadW);
        out.text(rest, plain, palette.muted, X(side, kMargin) + leadW, Y(y),
                 std::max(0.0f, kText * s - leadW - barW - 12.0f * s));
        out.group(0);
    }

    /// A figure: one part as large as the text's width and `h` allow, but no larger than its
    /// picture stays sharp (kMaxPointsPerPixel), centred; a click chooses it.
    void figure(u32 f, int side, float y, float h) {
        const File& file = tree.file(f);
        const Drawn d = *drawnOf(file);
        const double inkW = d.w + d.left + d.right, inkH = d.h + d.above + d.below;
        double mm = std::max(inkW / double(kText), inkH / double(h));
        if (file.picture && file.picture->mmPerPixel > 0.0) {
            // In the view's points: the page is drawn `s` times its own.
            mm = std::max(mm, file.picture->mmPerPixel * double(s) / double(kMaxPointsPerPixel));
        }
        const float pw = float(inkW / mm), ph = float(inkH / mm);
        out.target(GeoTargetFile, f);
        out.key(keyOf("figure", f));
        part(f, X(side, kMargin + (kText - pw) / 2.0f + float(d.left / mm)),
             Y(y + (h + ph) / 2.0f - float(d.below / mm)), mm / s, 0, false);
        out.key(0);
        out.target(GeoTargetNone, 0);
    }

    /// "Fig. M12 · 24 × 24 × 2.5 mm · 1 part · 11 KB".
    void figureCaption(u32 f, int side, float y) {
        const File& file = tree.file(f);
        std::string rest;
        if (!file.names.sub.empty()) {
            rest += file.names.sub + " \xC2\xB7 ";
        }
        if (file.facts && file.facts->hasBox) {
            const double sides[3] = {file.facts->size[0], file.facts->size[1], file.facts->size[2]};
            rest += dimensions(sides) + " \xC2\xB7 ";
        }
        rest += count(partsOf(file), "part");
        if (file.bytes > 0) {
            rest += " \xC2\xB7 " + bytesText(file.bytes);
        }
        caption("Fig. " + file.names.label, rest, side, y, 0);
    }

    void plate(const Page& p, int side) {
        const Family& fam = book_.families[p.family];
        eyebrow("Plate", side, 70.0f);
        out.target(GeoTargetFolder, fam.folder);
        out.text(fam.title, st(20.0f, 600), palette.on, X(side, kMargin), Y(96.0f),
                 (kText - 80.0f) * s);
        out.target(GeoTargetNone, 0);
        const std::string meta =
            count(fam.files.size(), "part") + " \xC2\xB7 " + rangeOf(tree, fam.files);
        out.text(meta, st(11.0f), palette.muted, X(side, kMargin), Y(116.0f), kText * s);
        // The plate: up to 13 across their sizes, smallest to largest, in one row to one scale;
        // the far larger ones left out, and said so.
        const FamilyScale split =
            familyScale(tree, drawable(tree, fam.files), 1.0f, 1.0f, 0.8, 6.0);
        const std::vector<u32>& normal = split.normal.empty() ? fam.files : split.normal;
        const std::vector<u32> pick = sampleOf(tree, normal, 13);
        const double mm = stand(pick, side, kMargin, 136.0f, kText, 150.0f, 1, pick.size() <= 9);
        if (mm > 0) {
            caption("Plate.",
                    pick.size() < fam.files.size()
                        ? std::format("{} of {}, smallest to largest, to one scale", pick.size(),
                                      fam.files.size())
                        : std::string("Every part, smallest to largest, to one scale"),
                    side, 306.0f, mm);
        }
        if (!split.oversize.empty()) {
            out.text(std::format("{} larger {} left out", split.oversize.size(),
                                 split.oversize.size() == 1 ? "file" : "files"),
                     st(10.0f), palette.muted, X(side, kMargin), Y(321.0f), kText * s);
        }
        // The figure: the row pointed at, or the part chosen, if this folder's; else its typical
        // part.
        u32 fig = typicalOf(tree, normal);
        for (const u32 f : {in.hoverKind == GeoTargetFile ? in.hover : kNone, in.selected}) {
            if (f != kNone && std::ranges::find(fam.files, f) != fam.files.end() &&
                drawnOf(tree.file(f))) {
                fig = f;
                break;
            }
        }
        if (fig != kNone) {
            figure(fig, side, 340.0f, 250.0f);
            figureCaption(fig, side, 610.0f);
        }
    }

    void table(const Page& p, int side) {
        const Family& fam = book_.families[p.family];
        const TextStyle head = st(9.5f, 600, 0.1f, true), cell = st(10.5f), num = st(10.5f, 500);
        // The board's columns: a picture, the file, its size, parts, triangles and bytes on disk.
        constexpr usize kCols = 6;
        const float cols[kCols] = {kMargin,          kMargin + 32.0f,  kMargin + 178.0f,
                                   kMargin + 286.0f, kMargin + 324.0f, kMargin + 368.0f};
        const char* heads[kCols] = {"", "File", "Size, mm", "Parts", "Tris", "On disk"};
        float y = 70.0f;
        if (p.from > 0) {
            // "Ple120, continued · 24–46 of 244".
            const usize last = std::min(fam.files.size(), p.from + kRows);
            out.text(std::format("{}, continued \xC2\xB7 {}\xE2\x80\x93{} of {}", fam.title,
                                 p.from + 1, last, fam.files.size()),
                     st(10.0f), palette.muted, X(side, kMargin), Y(y), kText * s);
        }
        y += 20.0f;
        const u32 hg = newGroup();
        out.group(hg);
        for (usize c = 1; c < kCols; ++c) {
            out.text(heads[c], head, palette.muted, X(side, cols[c]), Y(y),
                     ((c + 1 < kCols ? cols[c + 1] : kMargin + kText) - cols[c] - 4.0f) * s,
                     c >= 3 ? 2 : 0);
        }
        out.group(0);
        out.line(X(side, kMargin), Y(y + 6.0f), X(side, kMargin + kText), Y(y + 6.0f), palette.on,
                 1.0f);
        y += 26.0f;
        const float rowH = 21.0f;
        for (usize r = p.from; r < std::min(fam.files.size(), p.from + kRows); ++r) {
            const u32 f = fam.files[r];
            const File& file = tree.file(f);
            const bool selected = in.selected == f;
            const bool hovered = in.hoverKind == GeoTargetFile && in.hover == f;
            const bool faded = in.matches && !in.matches->contains(f);
            const usize first = out.items().size();
            out.target(GeoTargetFile, f);
            out.key(keyOf("part", f));
            if (selected || hovered) {
                out.rect(X(side, kMargin - 6.0f), Y(y - 15.0f), (kText + 12.0f) * s, rowH * s,
                         selected ? palette.wash : palette.lift, 0, 0.0f, 3.0f);
            }
            // The part small, at most 0.4 pt a millimetre, so rows keep their sizes apart.
            if (const auto d = drawnOf(file)) {
                const double mm = std::max({d->w / 28.0, (d->h + d->below) / 17.0, 2.5});
                const float pw = float(d->w / mm);
                part(f, X(side, kMargin + (28.0f - pw) / 2.0f),
                     Y(y + 3.0f) - float(d->below / mm) * s, mm / s, 0, false);
            }
            const u32 g = newGroup();
            out.group(g);
            std::string name = file.names.label;
            if (!file.names.sub.empty()) {
                name += "  " + file.names.sub;
            }
            out.text(name, cell, palette.on, X(side, cols[1]), Y(y),
                     (cols[2] - cols[1] - 6.0f) * s);
            std::string size = "\xE2\x80\x94", parts = "\xE2\x80\x94", tris = "\xE2\x80\x94";
            const std::string disk = file.bytes > 0 ? bytesText(file.bytes) : "\xE2\x80\x94";
            if (file.facts && file.facts->hasBox) {
                const double sides[3] = {file.facts->size[0], file.facts->size[1],
                                         file.facts->size[2]};
                size = dimensions(sides, true);
            }
            if (file.facts) {
                // In thousands from 10,000, so a million fits its column.
                parts = file.facts->parts >= 10'000 ? kilo(file.facts->parts)
                                                    : grouped(file.facts->parts);
                if (file.facts->triangles > 0) {
                    tris = kilo(file.facts->triangles);
                }
            }
            if (file.failed()) {
                size = "couldn't read";
            }
            out.text(size, cell, palette.on, X(side, cols[2]), Y(y),
                     (cols[3] - cols[2] - 6.0f) * s);
            const auto right = [&](const std::string& text, usize c) {
                const float end = c + 1 < kCols ? cols[c + 1] - 6.0f : kMargin + kText;
                out.text(text, num, palette.on, X(side, cols[c]), Y(y), (end - cols[c]) * s, 2);
            };
            right(parts, 3);
            right(tris, 4);
            right(disk, 5);
            out.group(0);
            if (faded) {
                for (usize i = first; i < out.items().size(); ++i) {
                    out.item(i).flags |= GeoItemFading;
                }
            }
            out.target(GeoTargetNone, 0);
            out.key(0);
            result.parts.push_back({GeoTargetFile, f, X(side, kMargin - 6.0f), Y(y - 15.0f),
                                    (kText + 12.0f) * s, rowH * s});
            out.line(X(side, kMargin), Y(y + 6.0f), X(side, kMargin + kText), Y(y + 6.0f),
                     palette.rule, 0.75f);
            y += rowH;
        }
    }

    void notes(int side) {
        eyebrow("Notes", side, 70.0f);
        for (float y = 110.0f; y < kPageH - 60.0f; y += 24.0f) {
            out.line(X(side, kMargin), Y(y), X(side, kMargin + kText), Y(y), palette.rule, 0.75f);
        }
    }

    void index(const Page& p, int side) {
        float top = 70.0f;
        if (p.first) {
            out.text("Index", st(22.0f, 600), palette.on, X(side, kMargin), Y(84.0f), kText * s);
            top = 110.0f;
        }
        const float colW = (kText - 20.0f) / 2.0f;
        for (usize c = 0; c < p.columns.size(); ++c) {
            float y = top;
            const float cx = kMargin + float(c) * (colW + 20.0f);
            for (const Line& line : p.columns[c]) {
                if (!line.letter.empty()) {
                    out.text(line.letter, st(10.0f, 600, 0.12f, true), palette.muted, X(side, cx),
                             Y(y + 4.0f), colW * s);
                    y += 18.0f;
                    continue;
                }
                std::string pages;
                for (const u32 page : line.pages) {
                    pages += (pages.empty() ? "" : ", ") + grouped(page);
                }
                const TextStyle entry = st(10.5f), folio = st(10.5f, 500);
                const float pw = std::min(colW * 0.45f * s, textWidth(pages, folio));
                out.target(GeoTargetPage, line.pages.empty() ? 0 : line.pages.front());
                const u32 g = newGroup();
                out.group(g);
                out.text(line.name, entry, palette.on, X(side, cx), Y(y), colW * s - pw - 6.0f * s);
                out.text(pages, folio, palette.muted, X(side, cx + colW) - pw, Y(y), pw, 2);
                out.group(0);
                out.target(GeoTargetNone, 0);
                y += 16.5f;
            }
        }
    }

    /// The thumb index: a tab for each chapter down the right page's edge, the one in view darker.
    void tabs(u32 spreadIndex) {
        if (book_.chapters.size() < 2) {
            return;
        }
        const u32 at = spreadIndex * 2;
        const float tabH = std::min(48.0f, (kPageH - 80.0f) / float(book_.chapters.size()));
        for (usize i = 0; i < book_.chapters.size(); ++i) {
            const Chapter& ch = book_.chapters[i];
            const u32 end =
                i + 1 < book_.chapters.size() ? book_.chapters[i + 1].page : book_.indexAt;
            const bool on = at + 1 >= ch.page && at < end;
            const float y = 40.0f + float(i) * tabH;
            out.target(GeoTargetPage, ch.page);
            out.rect(X(2, 0.0f), Y(y), 22.0f * s, (tabH - 3.0f) * s, on ? palette.on : palette.lift,
                     palette.on, 1.0f, 3.0f);
            out.text(std::format("{:02}", ch.no), st(8.5f, 600), on ? palette.lift : palette.on,
                     X(2, 2.0f), Y(y + tabH / 2.0f + 3.0f), 18.0f * s, 1);
            out.target(GeoTargetNone, 0);
        }
    }

private:
    const Book& book_;
    u32 headGroup_ = 0;
};

u32 catalogueSpreads(const Tree& tree, u32 folder) {
    const Book book = bookOf(tree, folder);
    return u32((book.pages.size() + 1) / 2);
}

u32 catalogueIndexSpread(const Tree& tree, u32 folder) {
    return bookOf(tree, folder).indexAt / 2;
}

LayoutResult layoutCatalogue(const LayoutInput& input, Display& display, u32 spreadIndex) {
    LayoutResult result;
    const Tree& tree = *input.tree;
    if (tree.folder(input.folder).count == 0) {
        return layoutPlates(input, display); // nothing to set: as Plates says it
    }
    const Book book = bookOf(tree, input.folder);
    Catalogue c(input, display, result, book);
    // The spread fitted to the view, with room for the thumb index on the right.
    c.s =
        std::min((input.width - 60.0f) / (2.0f * kPageW + 24.0f), (input.height - 16.0f) / kPageH);
    c.ox = (input.width - (2.0f * kPageW + 24.0f) * c.s) / 2.0f;
    c.oy = (input.height - kPageH * c.s) / 2.0f;
    const u32 spreads = u32((book.pages.size() + 1) / 2);
    c.spread(std::min(spreadIndex, spreads == 0 ? 0 : spreads - 1));
    result.contentHeight = input.height; // the spread fits the view; pages turn, they do not scroll
    return result;
}

} // namespace geo::gallery
