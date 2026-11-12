// The gallery's core: names, text set in IBM Plex Mono, the folder tree, and Plates laid out with
// every alignment group on one line.
#include "gallery.h"

#include "gallery/checks.h"
#include "gallery/names.h"
#include "gallery/text.h"
#include "gallery/tree.h"
#include "gallery/view.h"

#include <doctest/doctest.h>
#include <format>
#include <string>
#include <vector>

using namespace geo;
using namespace geo::gallery;

TEST_CASE("folder names read as headings, for any library") {
    CHECK(folderTitle("hexnut_iso4032") == "Hexnut ISO 4032");
    CHECK(folderTitle("dgbb_iso15") == "DGBB ISO 15");
    CHECK(folderTitle("stepper_nema") == "Stepper Nema");
    CHECK(folderTitle("din912 screws") == "DIN 912 Screws");
    CHECK(folderTitle("gt2") == "GT2");
}

TEST_CASE("file names drop their extension and ordering prefix, and an id gives way") {
    CHECK(fileNames("M6.step").label == "M6");
    CHECK(fileNames("01_bracket.STEP").label == "bracket");
    CHECK(fileNames("007 - plate.stl").label == "plate");
    CHECK(fileNames("0042.step").label == "#0042"); // all prefix: it is the name
    const FileNames named =
        fileNames("00840788_8bd96a8cce47d02d56a633b9_step_000.step", "3010 Muffin Fan");
    CHECK(named.label == "3010 Muffin Fan");
    CHECK(named.sub == "#00840788");
    CHECK(named.named);
    const FileNames unnamed = fileNames("00840788_8bd96a8cce47d02d56a633b9_step_000.step", "");
    CHECK(unnamed.label == "#00840788");
    CHECK(unnamed.sub == "unnamed");
    CHECK_FALSE(unnamed.named);
    CHECK(fileNames("demo_drive.step").label == "demo_drive");
    CHECK(compareNatural("part2", "part10") < 0);
    CHECK(compareNatural("M10", "m9") > 0);
    CHECK(compareNatural("a", "a") == 0);
}

TEST_CASE("text is set from IBM Plex Mono's fixed advance, and cut to fit") {
    const TextStyle tracked{10.0f, 600, 0.1f, true};
    CHECK(textWidth("ABC", tracked) == doctest::Approx(21.0));
    CHECK(textWidth("\xC2\xB7", TextStyle{10.0f}) == doctest::Approx(6.0)); // one character
    CHECK(setText("abc", tracked, 0.0f) == "ABC");
    const std::string cut = setText("a long folder name", TextStyle{10.0f}, 40.0f);
    CHECK(cut.ends_with("\xE2\x80\xA6"));
    CHECK(textWidth(cut, TextStyle{10.0f}) <= 40.0f);
    CHECK(number(84.66) == "84.7");
    CHECK(number(605.2) == "605");
    CHECK(number(4.254) == "4.25");
    CHECK(millimetres(1200.0) == "1.2 m");
    CHECK(count(1855, "file") == "1,855 files");
    CHECK(count(1, "file") == "1 file");
    const double sides[3] = {40, 30, 26.04};
    CHECK(dimensions(sides) == "40 \xC3\x97 30 \xC3\x97 26 mm");
}

namespace {

// A library: three folders of parts under the root, one of them nested, and two loose files.
std::vector<Entry> library(usize big = 50) {
    std::vector<Entry> entries;
    for (int i = 1; i <= 6; ++i) {
        entries.push_back({std::format("fastener/hexnut_iso4032/M{}.step", i * 2)});
    }
    for (usize i = 1; i <= big; ++i) {
        entries.push_back({std::format("bearing/{:04}.step", 600 + i)});
    }
    for (int i = 1; i <= 3; ++i) {
        entries.push_back({std::format("motor/nema{}.step", 10 + i * 3)});
    }
    entries.push_back({"loose_bracket.stl"});
    entries.push_back({"spacer.step"});
    entries.push_back({"drafts", true});
    return entries;
}

// Facts and a picture for each file: a box growing with its index, drawn as the thumbnailer would.
void measure(Gallery& g) {
    for (u32 f = 0; f < g.tree().fileCount(); ++f) {
        Facts facts;
        const double s = 4.0 + double(f % 17) * 3.0;
        facts.size = {s, s * 0.8, s * 0.6};
        facts.hasBox = true;
        facts.estimated = false;
        facts.parts = 1;
        g.setFacts(f, facts);
        Picture p;
        p.width = p.height = 512;
        p.mmPerPixel = s * 1.3 / 512.0;
        p.part = {80.0f + float(f % 5), 100.0f, 340.0f, 300.0f - float(f % 3) * 20.0f};
        p.ink = {70.0f, 95.0f, 370.0f, 340.0f};
        g.setPicture(f, p);
    }
}

void checkAligned(const Gallery& g) {
    const std::vector<Spread> spread = spreads(g.display().items(), nullptr, nullptr);
    CHECK(spread.size() > 4);
    for (const Spread& s : spread) {
        CAPTURE(s.group);
        CAPTURE(s.column);
        CHECK(s.spread < 0.01f);
    }
}

} // namespace

TEST_CASE("a tree holds folders and files in reading order, counted and coloured") {
    Tree tree;
    const std::vector<Entry> entries = library();
    tree.build("Parts", entries);
    const Folder& root = tree.folder(0);
    CHECK(root.count == 61);
    REQUIRE(root.folders.size() == 4);
    CHECK(tree.folder(root.folders[0]).name == "bearing");
    CHECK(tree.folder(root.folders[1]).name == "drafts");
    CHECK(tree.folder(root.folders[1]).count == 0);
    CHECK(tree.file(root.files[0]).name == "loose_bracket.stl");
    const u32 nuts = tree.folderAt("fastener/hexnut_iso4032");
    REQUIRE(nuts != kNone);
    CHECK(tree.file(tree.folder(nuts).files.front()).names.label == "M2");
    CHECK(tree.file(tree.folder(nuts).files.back()).names.label == "M12");
    CHECK(tree.titleOf(nuts) == "Hexnut ISO 4032");
    CHECK(tree.titleOf(0) == "Parts");
    // Folders that hold files take turns at the colours; a folder of folders takes its first's.
    CHECK(tree.folder(tree.folderAt("fastener")).colour == tree.folder(nuts).colour);
    CHECK(tree.familiesUnder(0).size() == 4);
    CHECK(tree.filesUnder(tree.folderAt("fastener")).size() == 6);
    CHECK(tree.fileAt("motor/nema13.step") != kNone);
}

TEST_CASE("plates lay out with every row and column on one line, in every style") {
    for (const u32 style : {0u, 3u, 5u}) {
        CAPTURE(style);
        Gallery g;
        const std::vector<Entry> entries = library();
        g.setEntries("Parts", entries);
        measure(g);
        g.setStyle(style);
        g.setViewport(1080.0f, 720.0f);
        REQUIRE(g.layout());
        CHECK_FALSE(g.layout()); // nothing changed
        CHECK(g.result().plates.size() == 3); // drafts holds nothing, so has no plate
        CHECK(g.result().parts.size() == 2); // the root's own files, on a shelf
        CHECK(g.result().covers.size() > 4);
        checkAligned(g);
        // Hovered, a plate lifts, and the rest stay aligned.
        g.hover(GeoTargetFolder, g.result().plates[1].index);
        REQUIRE(g.layout());
        checkAligned(g);
    }
}

TEST_CASE("a folder of files stands on a shelf at one scale, its labels on shared lines") {
    Gallery g;
    const std::vector<Entry> entries = library();
    g.setEntries("Parts", entries);
    measure(g);
    REQUIRE(g.setFolder("bearing"));
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    CHECK(g.result().plates.empty());
    CHECK(g.result().parts.size() == 50);
    checkAligned(g);
    // Pictures stand on their row's baseline: a picture item's line is its part's bottom.
    usize pictures = 0;
    for (const GeoGalleryItem& item : g.display().items()) {
        pictures += item.kind == GeoItemPicture ? 1 : 0;
    }
    CHECK(pictures == 50);
}

TEST_CASE("a folder of many files falls into size bands, each cut short until shown whole") {
    Gallery g;
    const std::vector<Entry> entries = library(200);
    g.setEntries("Parts", entries);
    measure(g);
    REQUIRE(g.setFolder("bearing"));
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    const usize before = g.result().parts.size();
    CHECK(before < 200);
    REQUIRE_FALSE(g.result().bands.empty());
    const GeoGalleryAction more = g.click(GeoTargetMore, 0, false);
    CHECK(more.kind == GeoActionHandled);
    g.layout();
    CHECK(g.result().parts.size() > before);
    checkAligned(g);
}

TEST_CASE("files not measured yet wait in a list, and failures keep their place") {
    Gallery g;
    const std::vector<Entry> entries = library();
    g.setEntries("Parts", entries);
    g.setFailure(g.tree().fileAt("spacer.step"), "Unexpected end of file");
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    bool reading = false, failed = false;
    for (const GeoGalleryItem& item : g.display().items()) {
        failed |= item.kind == GeoItemRect && (item.flags & GeoItemHatched) != 0;
        reading |= item.kind == GeoItemText;
    }
    CHECK(reading);
    CHECK(failed);
}

TEST_CASE("clicks, keys and hits move through plates, into folders and back") {
    Gallery g;
    const std::vector<Entry> entries = library();
    g.setEntries("Parts", entries);
    measure(g);
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    const LayoutResult::Place plate = g.result().plates[2];
    const GeoGalleryHit hit = g.hit(plate.x + plate.w / 2, plate.y + plate.h / 2);
    CHECK(hit.target_kind == GeoTargetFolder);
    CHECK(hit.target == plate.index);

    // Arrows focus a plate, ⏎ goes in, Esc comes back out onto it.
    CHECK(g.key(GeoKeyRight).kind == GeoActionHandled);
    g.layout();
    CHECK(g.key(GeoKeyRight).kind == GeoActionHandled);
    g.layout();
    const GeoGalleryAction in = g.key(GeoKeyEnter);
    REQUIRE(in.kind == GeoActionEnterFolder);
    const u32 entered = in.target;
    CHECK(g.folder() == entered);
    g.layout();
    const GeoGalleryAction out = g.key(GeoKeyEscape);
    CHECK(out.kind == GeoActionEnterFolder);
    CHECK(g.folder() == 0);
    g.layout();
    // At the top, Esc leaves the gallery once nothing is focused.
    CHECK(g.key(GeoKeyEscape).kind == GeoActionHandled);
    CHECK(g.key(GeoKeyEscape).kind == GeoActionLeave);

    // A file is selected by a click, opened by a second.
    REQUIRE(g.setFolder("motor"));
    g.layout();
    const u32 file = g.result().parts.front().index;
    CHECK(g.click(GeoTargetFile, file, false).kind == GeoActionHandled);
    CHECK(g.selected() == file);
    CHECK(g.click(GeoTargetFile, file, true).kind == GeoActionOpenFile);
    CHECK(g.key(GeoKeySpace).kind == GeoActionPeek);
    CHECK(g.key(GeoKeyEnter).kind == GeoActionOpenFile);
}

TEST_CASE("the files wanted first are those drawn in view") {
    Gallery g;
    const std::vector<Entry> entries = library();
    g.setEntries("Parts", entries);
    measure(g);
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    const std::vector<GeoGalleryWant> wanted = g.wanted();
    CHECK(wanted.size() == 61);
    CHECK(wanted.front().band == 1);
    CHECK(wanted.back().band == 3);
    for (usize i = 1; i < wanted.size(); ++i) {
        CHECK(wanted[i - 1].band <= wanted[i].band);
    }
}

TEST_CASE("the C interface lays out and reports alignment as painted") {
    GeoGallery* g = geo_gallery_create();
    const GeoGalleryEntry entries[] = {
        {"a/one.step", false, 10, 0}, {"a/two.step", false, 10, 0}, {"b/three.stl", false, 10, 0}};
    geo_gallery_set_entries(g, "Root", entries, 3);
    CHECK(geo_gallery_file_count(g) == 3);
    const uint32_t two = geo_gallery_file_at(g, "a/two.step");
    REQUIRE(two != UINT32_MAX);
    const GeoGalleryFacts facts{{10, 8, 6}, true, false, 1, "Two", 1200};
    for (uint32_t f = 0; f < 3; ++f) {
        geo_gallery_set_facts(g, f, &facts);
    }
    geo_gallery_set_viewport(g, 900, 600);
    CHECK(geo_gallery_layout(g));
    CHECK(geo_gallery_item_count(g) > 10);
    CHECK(std::string(geo_gallery_folder_title(g)) == "Root");
    CHECK(std::string(geo_gallery_folder_subtitle(g)) == "3 files \xC2\xB7 2 folders");
    // Painted where laid out: nothing over; one item moved by a point: its group over.
    const uint32_t n = geo_gallery_item_count(g);
    std::vector<float> lines(n), columns(n);
    const GeoGalleryItem* items = geo_gallery_items(g);
    for (uint32_t i = 0; i < n; ++i) {
        lines[i] = items[i].line;
        columns[i] = items[i].column_line;
    }
    uint32_t checked = 0;
    GeoGalleryGroupSpread over[8];
    CHECK(geo_gallery_check(g, lines.data(), columns.data(), 0.5f, over, 8, &checked) == 0);
    CHECK(checked > 0);
    // Moved by a point, an item of a group of several puts its group over.
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t members = 0;
        for (uint32_t j = 0; j < n; ++j) {
            members += items[j].group == items[i].group ? 1 : 0;
        }
        if (items[i].group != 0 && members >= 2) {
            lines[i] += 1.0f;
            break;
        }
    }
    CHECK(geo_gallery_check(g, lines.data(), columns.data(), 0.5f, over, 8, &checked) == 1);
    geo_gallery_destroy(g);
}

TEST_CASE("a gallery with nothing in it says so") {
    Gallery g;
    g.setViewport(800.0f, 600.0f);
    CHECK(g.layout());
    CHECK(g.display().textBuffer().find("No STEP or STL files here.") != std::string::npos);
    CHECK(g.wanted().empty());
    CHECK(g.key(GeoKeyEscape).kind == GeoActionLeave);
    GeoGallery* c = geo_gallery_create();
    geo_gallery_layout(c);
    CHECK(geo_gallery_item_count(c) == 3);
    CHECK(std::string(geo_gallery_folder(c)).empty());
    geo_gallery_destroy(c);
}

TEST_CASE("an empty folder says so") {
    Gallery g;
    const std::vector<Entry> entries = library();
    g.setEntries("Parts", entries);
    REQUIRE(g.setFolder("drafts"));
    g.setViewport(900.0f, 600.0f);
    g.layout();
    bool said = false;
    const std::string& text = g.display().textBuffer();
    said = text.find("No STEP or STL files here.") != std::string::npos &&
           text.find("DRAFTS") != std::string::npos;
    CHECK(said);
}

// ---- G3: Sheet, views, motion, search, Peek ----------------------------------------------------

namespace {

Gallery measured(usize big = 50) {
    Gallery g;
    const std::vector<Entry> entries = library(big);
    g.setEntries("Parts", entries);
    measure(g);
    g.setViewport(1080.0f, 720.0f);
    return g;
}

usize countText(const Gallery& g, std::string_view needle) {
    usize n = 0;
    const std::string& text = g.display().textBuffer();
    for (usize at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("sheet lays every file out in cells, every row and column on one line") {
    for (const u32 style : {0u, 3u, 5u}) {
        for (const u32 scale : {u32(GeoScaleFit), u32(GeoScaleFolder), u32(GeoScaleTrue)}) {
            CAPTURE(style);
            CAPTURE(scale);
            Gallery g = measured();
            g.setStyle(style);
            REQUIRE(g.setView(GeoViewSheet));
            g.setSheet(scale, 128.0f, GeoSortName);
            g.layout();
            CHECK(g.result().parts.size() == 61); // every file once
            CHECK(g.result().plates.empty());
            checkAligned(g);
        }
    }
}

TEST_CASE("sheet's headings stay at the top until the next reaches them") {
    Gallery g = measured();
    g.setView(GeoViewSheet);
    g.layout();
    std::vector<std::pair<float, float>> headings; // top, until
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.sticky_top != 0.0f || item.sticky_until != 0.0f) {
            if (headings.empty() || headings.back().first != item.sticky_top) {
                headings.push_back({item.sticky_top, item.sticky_until});
            }
        }
    }
    // "In this folder", then bearing, fastener and motor; drafts holds nothing.
    REQUIRE(headings.size() == 4);
    for (usize i = 0; i + 1 < headings.size(); ++i) {
        CHECK(headings[i].first < headings[i + 1].first);
        CHECK(headings[i].second > 0.0f);
    }
    CHECK(countText(g, "IN THIS FOLDER") == 1);
}

TEST_CASE("a folder of files alone falls into size bands on the sheet") {
    Gallery g = measured();
    REQUIRE(g.setFolder("bearing"));
    g.setView(GeoViewSheet);
    g.layout();
    CHECK(countText(g, "BY SIZE, SINCE THIS FOLDER HAS NO SUBFOLDERS") == 1);
    CHECK(countText(g, "Under 20 mm") + countText(g, "20 to 100 mm") >= 1);
    checkAligned(g);
}

TEST_CASE("true scale marks parts too small to see, drawn larger") {
    Gallery g;
    std::vector<Entry> entries = {{"big.step"}, {"tiny.step"}};
    g.setEntries("Parts", entries);
    measure(g);
    // One part a thousand times the other's size.
    Facts huge;
    huge.size = {2000, 1600, 1200};
    huge.hasBox = true;
    huge.estimated = false;
    huge.parts = 1;
    g.setFacts(g.tree().fileAt("big.step"), huge);
    Picture p;
    p.width = p.height = 512;
    p.mmPerPixel = 2000 * 1.3 / 512.0;
    p.part = {80, 100, 340, 300};
    p.ink = {70, 95, 370, 340};
    g.setPicture(g.tree().fileAt("big.step"), p);
    g.setViewport(1080.0f, 720.0f);
    g.setView(GeoViewSheet);
    g.setSheet(GeoScaleTrue, 128.0f, GeoSortName);
    g.layout();
    CHECK(countText(g, "DETAIL") == 1);
}

TEST_CASE("views: only those enabled, each folder remembering its own") {
    Gallery g = measured();
    CHECK_FALSE(g.setView(GeoViewLadders));
    CHECK(g.view() == GeoViewPlates);
    CHECK(g.setView(GeoViewSheet));
    // Into a folder: the view in use carries on.
    g.layout();
    g.click(GeoTargetFolder, g.tree().folderAt("motor"), false);
    CHECK(g.view() == GeoViewSheet);
    CHECK(g.setView(GeoViewPlates));
    g.layout();
    // Back at the top, the root shows the view it last showed.
    g.key(GeoKeyFolderUp);
    CHECK(g.view() == GeoViewSheet);
    g.setViews(1u << GeoViewPlates); // Sheet turned off: back to Plates
    CHECK(g.view() == GeoViewPlates);
}

TEST_CASE("switching views keeps the selection, and flies parts in both views to their places") {
    Gallery g = measured();
    REQUIRE(g.setFolder("motor"));
    g.layout();
    const u32 file = g.result().parts[1].index;
    g.click(GeoTargetFile, file, false);
    g.layout();
    REQUIRE(g.setView(GeoViewSheet));
    REQUIRE(g.layout());
    CHECK(g.selected() == file);
    const GeoGalleryTransition t = g.transition();
    CHECK(t.kind == GeoTransitionSwitch);
    CHECK(t.duration == 520.0f);
    CHECK(t.fade_out == 260.0f);
    REQUIRE_FALSE(g.tracks().empty());
    for (const GeoGalleryTrack& track : g.tracks()) {
        if (track.kind == GeoTrackFly) {
            CHECK(track.delay == 0.0f);
            CHECK(track.duration == 520.0f);
        } else {
            CHECK(track.delay == doctest::Approx(182.0f));
            CHECK(track.duration == doctest::Approx(338.0f));
        }
    }
    // The three motors are in both views: they fly.
    usize flying = 0;
    for (const GeoGalleryTrack& track : g.tracks()) {
        flying += track.kind == GeoTrackFly ? 1 : 0;
    }
    CHECK(flying == 3);
    // Played once: a layout for a hover plays nothing.
    g.hover(GeoTargetFile, file);
    REQUIRE(g.layout());
    CHECK(g.transition().kind == GeoTransitionNone);
    CHECK(g.tracks().empty());
}

TEST_CASE("into a folder it grows from its plate, and back out it shrinks into it") {
    Gallery g = measured();
    g.layout();
    const LayoutResult::Place plate = g.result().plates[1];
    g.click(GeoTargetFolder, plate.index, false);
    REQUIRE(g.layout());
    GeoGalleryTransition t = g.transition();
    CHECK(t.kind == GeoTransitionInto);
    CHECK(t.duration == 420.0f);
    CHECK(t.rect[0] == plate.x);
    CHECK(t.rect[2] == plate.w);
    g.key(GeoKeyEscape);
    REQUIRE(g.layout());
    t = g.transition();
    CHECK(t.kind == GeoTransitionOut);
    CHECK(t.rect[0] == plate.x);
    CHECK(t.rect[3] == plate.h);
}

TEST_CASE("a search marks what it found, and Esc steps back: Peek, search, selection, folder") {
    Gallery g = measured();
    g.setSearch("m12");
    g.layout();
    REQUIRE(g.matches().has_value());
    CHECK(g.matches()->size() == 1);
    CHECK(countText(g, "1 match") == 1);
    usize faded = 0;
    for (const GeoGalleryItem& item : g.display().items()) {
        faded += (item.flags & GeoItemFading) != 0 ? 1 : 0;
    }
    CHECK(faded > 0);
    checkAligned(g);

    REQUIRE(g.setFolder("motor"));
    g.layout();
    const u32 file = g.result().parts[0].index;
    g.click(GeoTargetFile, file, false);
    CHECK(g.key(GeoKeySpace).kind == GeoActionPeek);
    g.layout();
    usize fixed = 0;
    for (const GeoGalleryItem& item : g.display().items()) {
        fixed += (item.flags & GeoItemFixed) != 0 ? 1 : 0;
    }
    CHECK(fixed > 4);
    CHECK(countText(g, "triangles") == 1);
    CHECK(countText(g, "Open  \xE2\x8F\x8E") == 1);
    CHECK(countText(g, "\xC3\x82") == 0); // no text decoded twice
    g.setSearch("nema");
    CHECK(g.key(GeoKeyEscape).kind == GeoActionHandled); // Peek
    CHECK(g.peeking() == kNone);
    CHECK(g.key(GeoKeyEscape).kind == GeoActionHandled); // the search
    CHECK(g.search().empty());
    CHECK(g.key(GeoKeyEscape).kind == GeoActionHandled); // the selection
    CHECK(g.selected() == kNone);
    CHECK(g.key(GeoKeyEscape).kind == GeoActionEnterFolder); // up a folder
}

TEST_CASE("arrows move the selection through the sheet's cells") {
    Gallery g = measured();
    REQUIRE(g.setFolder("bearing"));
    g.setView(GeoViewSheet);
    g.layout();
    CHECK(g.key(GeoKeyRight).kind == GeoActionHandled);
    const u32 first = g.selected();
    REQUIRE(first != kNone);
    g.layout();
    g.key(GeoKeyRight);
    g.layout();
    CHECK(g.selected() != first);
    const u32 second = g.selected();
    g.key(GeoKeyDown);
    g.layout();
    CHECK(g.selected() != second);
    CHECK(g.reveal() < 0.0f); // it shows
}

TEST_CASE("Peek: the card stays, Open opens, anywhere else closes, and arrows move it") {
    Gallery g = measured();
    REQUIRE(g.setFolder("motor"));
    g.layout();
    const u32 first = g.result().parts[0].index;
    g.click(GeoTargetFile, first, false);
    REQUIRE(g.key(GeoKeySpace).kind == GeoActionPeek);
    g.layout();
    // Only Peek's own items can be hit; the parts under the scrim cannot.
    const GeoGalleryHit scrim = g.hit(1.0f, 1.0f);
    CHECK(scrim.target_kind == GeoTargetNone);
    const GeoGalleryHit card = g.hit(540.0f, 360.0f);
    CHECK((card.target_kind == GeoTargetPeek || card.target_kind == GeoTargetFile));
    CHECK(g.click(GeoTargetPeek, 0, false).kind == GeoActionNone);
    CHECK(g.peeking() == first);
    // Arrows move the selection, and Peek with it.
    g.key(GeoKeyRight);
    g.layout();
    CHECK(g.selected() != first);
    CHECK(g.peeking() == g.selected());
    const u32 second = g.selected();
    // Open opens the file in Peek.
    const GeoGalleryAction open = g.click(GeoTargetFile, second, false);
    CHECK(open.kind == GeoActionOpenFile);
    CHECK(open.target == second);
    CHECK(g.peeking() == kNone);
    // Anywhere else closes it.
    g.key(GeoKeySpace);
    CHECK(g.click(GeoTargetNone, 0, false).kind == GeoActionHandled);
    CHECK(g.peeking() == kNone);
}

TEST_CASE("Peek's stats share their baselines, and its card fits the view") {
    for (const float height : {420.0f, 720.0f, 1200.0f}) {
        CAPTURE(height);
        Gallery g = measured();
        g.setViewport(1080.0f, height);
        REQUIRE(g.setFolder("motor"));
        g.layout();
        g.click(GeoTargetFile, g.result().parts[0].index, false);
        g.key(GeoKeySpace);
        g.layout();
        checkAligned(g);
        for (const GeoGalleryItem& item : g.display().items()) {
            if ((item.flags & GeoItemFixed) != 0) {
                CHECK(item.y >= 0.0f);
                CHECK(item.y + item.height <= height + 0.01f);
            }
        }
    }
}

TEST_CASE("back up from a folder shown as a sheet, it still shrinks into its plate") {
    Gallery g = measured();
    g.layout();
    const LayoutResult::Place plate = g.result().plates[0];
    g.click(GeoTargetFolder, plate.index, false);
    REQUIRE(g.layout());
    REQUIRE(g.setView(GeoViewSheet));
    g.layout();
    g.key(GeoKeyEscape);
    REQUIRE(g.layout());
    CHECK(g.view() == GeoViewPlates);
    const GeoGalleryTransition t = g.transition();
    CHECK(t.kind == GeoTransitionOut);
    CHECK(t.rect[0] == plate.x);
    CHECK(t.rect[2] == plate.w);
}

TEST_CASE("in the sheet, every part's shadow stays above its labels") {
    for (const u32 scale : {u32(GeoScaleFit), u32(GeoScaleFolder), u32(GeoScaleTrue)}) {
        CAPTURE(scale);
        Gallery g = measured();
        g.setView(GeoViewSheet);
        g.setSheet(scale, 128.0f, GeoSortName);
        g.layout();
        const std::vector<GeoGalleryItem>& items = g.display().items();
        usize checked = 0;
        for (usize i = 0; i < items.size(); ++i) {
            if (items[i].kind != GeoItemPicture) {
                continue;
            }
            const Picture& p = *g.tree().file(items[i].file).picture;
            const float k = items[i].width / float(p.width);
            const float inkBottom = items[i].y + (p.ink[1] + p.ink[3]) * k;
            // The file's label is the next text with its target.
            for (usize j = i + 1; j < items.size(); ++j) {
                if (items[j].kind == GeoItemText && items[j].target == items[i].file) {
                    CHECK(inkBottom <= items[j].baseline - 1.025f * 10.5f + 0.01f);
                    ++checked;
                    break;
                }
            }
        }
        CHECK(checked > 20);
        checkAligned(g);
    }
}

TEST_CASE("a file being read shows its bar; a changed one its Δ until seen, and nothing moves") {
    Gallery g = measured();
    REQUIRE(g.setFolder("motor"));
    g.layout();
    const u32 file = g.result().parts[1].index;
    const auto boxes = [&] {
        std::vector<std::array<float, 4>> out;
        for (const GeoGalleryItem& item : g.display().items()) {
            if (item.kind == GeoItemPicture || (item.flags & GeoItemDashed) != 0) {
                out.push_back({item.x, item.y, item.width, item.height});
            }
        }
        return out;
    };
    const auto before = boxes();
    g.setChanged(file, true);
    REQUIRE(g.layout());
    CHECK(boxes() == before); // nothing moves
    CHECK(countText(g, "\xCE\x94") == 1);
    usize stale = 0;
    for (const GeoGalleryItem& item : g.display().items()) {
        stale += (item.flags & GeoItemStale) != 0 ? 1 : 0;
    }
    CHECK(stale == 1);
    for (const Spread& spread : spreads(g.display().items(), nullptr, nullptr)) {
        CHECK(spread.spread < 0.01f); // the marks align on nothing
    }
    // Seen: hovered, the Δ goes; the picture stays faded until a new one comes.
    g.hover(GeoTargetFile, file);
    g.layout();
    CHECK(countText(g, "\xCE\x94") == 0);
    Picture p = *g.tree().file(file).picture;
    g.setPicture(file, p);
    g.layout();
    for (const GeoGalleryItem& item : g.display().items()) {
        CHECK((item.flags & GeoItemStale) == 0);
    }

    // A file not drawn yet, being read: its box and a bar, which a picture ends.
    Gallery r;
    std::vector<Entry> entries = {{"a.step"}, {"b.step"}};
    r.setEntries("Parts", entries);
    Facts facts;
    facts.size = {40, 30, 20};
    facts.hasBox = true;
    facts.estimated = true;
    facts.parts = 1;
    r.setFacts(0, facts);
    r.setFacts(1, facts);
    r.setViewport(1080.0f, 720.0f);
    r.setReading(0, 0.5f);
    r.layout();
    usize bars = 0;
    for (const GeoGalleryItem& item : r.display().items()) {
        bars += item.kind == GeoItemRect && item.height == 5.0f ? 1 : 0;
    }
    CHECK(bars == 1);
    CHECK_FALSE(r.layout()); // nothing new
    r.setReading(0, 0.51f);  // the same sixteenth: no layout
    CHECK_FALSE(r.layout());
}

TEST_CASE("a poster prints the view on one sheet, its title above and a rule below") {
    for (const u32 view : {u32(GeoViewPlates), u32(GeoViewSheet)}) {
        for (const u32 style : {0u, 3u}) {
            CAPTURE(view);
            CAPTURE(style);
            Gallery g = measured();
            g.setStyle(style);
            g.setView(view);
            g.click(GeoTargetFile, g.tree().fileAt("motor/nema13.step"), false);
            g.hover(GeoTargetFile, g.tree().fileAt("bearing/0608.step"));
            g.setPoster(true, "11 November 2026");
            g.setViewport(1380.0f, 840.0f);
            REQUIRE(g.layout());
            checkAligned(g);
            CHECK(countText(g, "11 NOVEMBER 2026") == 1);
            CHECK(countText(g, "GEOMETRYVIEW") == 1);
            CHECK(countText(g, view == GeoViewPlates ? "PLATE 1" : "SHEET 1") == 1);
            for (const GeoGalleryItem& item : g.display().items()) {
                CHECK(item.sticky_until == 0.0f);          // nothing scrolls on paper
                CHECK((item.flags & GeoItemSticker) == 0); // nothing hovered or selected
            }
            if (view == GeoViewSheet) {
                CHECK(g.result().parts.size() == 61); // every file
            } else {
                // Every folder holding files is a plate: bearing, the hex nuts and motor. The top's
                // own files, beside its folders, get none.
                CHECK(g.result().plates.size() == 3);
            }
        }
    }
}
