// The gallery's core: names, text set in IBM Plex Mono, the folder tree, and Plates laid out with
// every alignment group on one line.
#include "gallery.h"

#include "gallery/checks.h"
#include "gallery/names.h"
#include "gallery/text.h"
#include "gallery/tree.h"
#include "gallery/view.h"

#include <cmath>
#include <doctest/doctest.h>
#include <format>
#include <map>
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
    CHECK(folderTitle("01 Organ Pipes") == "Organ Pipes"); // ordered as named, shown without
    CHECK(folderTitle("007_bells") == "Bells");
    CHECK(folderTitle("0042") == "0042"); // all prefix: it is the name
    CHECK(folderTitle("2024 parts") == "2024 Parts");
    CHECK(folderTitle("07 Wheels and Pinions") == "Wheels and Pinions");
    CHECK(folderTitle("bill_of_materials") == "Bill of Materials");
    CHECK(folderTitle("in stock") == "In Stock"); // the first word and the last are capitals
    CHECK(folderTitle("type_a") == "Type A");
    CHECK(folderTitle("Nuts And Bolts") == "Nuts And Bolts"); // as written
}

TEST_CASE("a title shrinks to fit before it is cut short") {
    const TextStyle big{30.0f, 600, 0.0f, false};
    const std::string title = "Things That Come in Sizes"; // 25 characters, 450 points at 30
    CHECK(fitted(title, big, 500.0f, 20.0f).size == 30.0f);
    CHECK(fitted(title, big, 360.0f, 20.0f).size == doctest::Approx(24.0f));
    const TextStyle least = fitted(title, big, 200.0f, 20.0f);
    CHECK(least.size == 20.0f);
    CHECK(setText(title, least, 200.0f).ends_with("\xE2\x80\xA6"));      // and below that, cut
    CHECK(fitted(title, TextStyle{12.0f}, 100.0f, 20.0f).size == 12.0f); // never grows
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
        CHECK_FALSE(g.layout());              // nothing changed
        CHECK(g.result().plates.size() == 3); // drafts holds nothing, so has no plate
        CHECK(g.result().parts.size() == 2);  // the root's own files, on a shelf
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

TEST_CASE("Peek turns a part that can turn: ← → turn it, ↑ ↓ still move, another file waits") {
    Gallery g = measured();
    REQUIRE(g.setFolder("bearing")); // rows of parts, so ↓ has somewhere to go
    g.layout();
    const u32 first = g.result().parts[0].index;
    g.click(GeoTargetFile, first, false);
    REQUIRE(g.key(GeoKeySpace).kind == GeoActionPeek);
    g.layout();
    const auto turntable = [&]() -> const GeoGalleryItem* {
        for (const GeoGalleryItem& item : g.display().items()) {
            if ((item.flags & GeoItemTurn) != 0) {
                return &item;
            }
        }
        return nullptr;
    };
    // Its frames still to come: the still picture, and arrows move file to file.
    CHECK(turntable() == nullptr);
    CHECK(countText(g, "Drag to turn") == 0);
    g.setPeekTurnable(true);
    REQUIRE(g.layout());
    const GeoGalleryItem* item = turntable();
    REQUIRE(item != nullptr);
    CHECK(item->kind == GeoItemPicture);
    CHECK(item->file == first);
    CHECK((item->flags & GeoItemFixed) != 0); // Peek's, over the view
    CHECK(countText(g, "Drag to turn") == 1);
    const GeoGalleryAction back = g.key(GeoKeyLeft), on = g.key(GeoKeyRight);
    CHECK(back.kind == GeoActionTurn);
    CHECK(back.target == 0);
    CHECK(on.kind == GeoActionTurn);
    CHECK(on.target == 1);
    CHECK(g.peeking() == first);
    // ↓ goes to the next file, whose turntable is yet to come.
    CHECK(g.key(GeoKeyDown).kind == GeoActionHandled);
    g.layout();
    CHECK(g.peeking() != first);
    CHECK_FALSE(g.peekTurnable());
    CHECK(turntable() == nullptr);
    CHECK(g.key(GeoKeyLeft).kind != GeoActionTurn);
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

TEST_CASE("on a phone, plates stand two abreast, the sheet three, and Peek fits") {
    Gallery g = measured();
    g.setViewport(374.0f, 620.0f); // a 390 pt phone, less 8 each side
    g.layout();
    REQUIRE(g.result().plates.size() >= 2);
    CHECK(g.result().plates[0].y == g.result().plates[1].y); // two abreast
    for (const auto& p : g.result().plates) {
        CHECK(p.x + p.w <= 374.0f);
    }
    g.setView(GeoViewSheet);
    g.layout();
    std::map<float, usize> rows;
    for (const auto& p : g.result().parts) {
        ++rows[p.y];
        CHECK(p.x + p.w <= 374.0f);
    }
    usize widest = 0;
    for (const auto& [y, n] : rows) {
        widest = std::max(widest, n);
    }
    CHECK(widest == 3);
    g.click(GeoTargetFile, g.result().parts[0].index, false);
    g.key(GeoKeySpace);
    g.layout();
    for (const GeoGalleryItem& item : g.display().items()) {
        if ((item.flags & GeoItemFixed) != 0) {
            CHECK(item.x >= 0.0f);
            CHECK(item.x + item.width <= 374.0f + 0.01f);
            CHECK(item.y + item.height <= 620.0f + 0.01f);
        }
    }
    checkAligned(g);
}

TEST_CASE("no two texts on one line overlap, from a phone's width to a wide window") {
    for (const float width : {374.0f, 500.0f, 1080.0f, 1800.0f}) {
        for (const u32 view :
             {u32(GeoViewPlates), u32(GeoViewSheet), u32(GeoViewLadders), u32(GeoViewAtlas)}) {
            for (const char* folder : {"", "bearing", "motor"}) {
                CAPTURE(width);
                CAPTURE(view);
                CAPTURE(folder);
                Gallery g = measured();
                g.setViews(0b1111);
                g.setViewport(width, 720.0f);
                REQUIRE(g.setFolder(folder));
                g.setView(view);
                g.layout();
                struct Span {
                    float baseline, left, right;
                    std::string text;
                };
                std::vector<Span> spans;
                const std::string& buffer = g.display().textBuffer();
                for (const GeoGalleryItem& item : g.display().items()) {
                    if (item.kind != GeoItemText) {
                        continue;
                    }
                    const std::string text = buffer.substr(item.text, item.text_length);
                    const float used =
                        float(characters(text)) * (0.6f * item.font_size + item.tracking);
                    const float left = item.align == 1   ? item.x + (item.width - used) / 2.0f
                                       : item.align == 2 ? item.x + item.width - used
                                                         : item.x;
                    spans.push_back({item.baseline, left, left + used, text});
                }
                for (usize a = 0; a < spans.size(); ++a) {
                    for (usize b = a + 1; b < spans.size(); ++b) {
                        if (std::fabs(spans[a].baseline - spans[b].baseline) > 0.5f) {
                            continue;
                        }
                        CAPTURE(spans[a].text);
                        CAPTURE(spans[b].text);
                        CHECK((spans[a].right <= spans[b].left + 0.5f ||
                               spans[b].right <= spans[a].left + 0.5f));
                    }
                }
            }
        }
    }
}

TEST_CASE("a folder that can no longer be read shows as last seen, faded, with one way back") {
    Gallery g = measured();
    g.layout();
    const usize shown = g.display().items().size();
    g.setLost(true);
    REQUIRE(g.layout());
    usize lost = 0, fixed = 0;
    for (const GeoGalleryItem& item : g.display().items()) {
        lost += (item.flags & GeoItemLost) != 0 ? 1 : 0;
        fixed += (item.flags & GeoItemFixed) != 0 ? 1 : 0;
    }
    CHECK(lost == shown); // everything last seen, faded
    CHECK(fixed > 3);     // the card over it
    CHECK(countText(g, "Allow reading this folder again.") == 1);
    // The way back, hit where it is drawn, in the view's points, however far down the view is.
    g.setScroll(120.0f);
    g.layout();
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.target_kind == GeoTargetReconnect && item.kind == GeoItemRect) {
            const GeoGalleryHit hit =
                g.hit(item.x + item.width / 2.0f, 120.0f + item.y + item.height / 2.0f);
            CHECK(hit.target_kind == GeoTargetReconnect);
            CHECK(g.click(hit.target_kind, hit.target, false).kind == GeoActionReconnect);
        }
    }
    g.setLost(false);
    g.layout();
    CHECK(countText(g, "Allow reading this folder again.") == 0);
}

TEST_CASE("tiny beside huge: a speck at the shelf's scale gets a detail bubble") {
    Gallery g;
    std::vector<Entry> entries = {{"axis-600.step"}, {"axis-400.step"}, {"washer-m1.step"}};
    g.setEntries("Parts", entries);
    const double sizes[3] = {600.0, 400.0, 1.2};
    for (u32 f = 0; f < 3; ++f) {
        const u32 file = g.tree().fileAt(entries[f].path);
        const double s = sizes[f];
        Facts facts;
        facts.size = {s, s * 0.2, s * 0.15};
        facts.hasBox = true;
        facts.estimated = false;
        facts.parts = 1;
        g.setFacts(file, facts);
        Picture p;
        p.width = p.height = 512;
        p.mmPerPixel = s * 1.3 / 512.0;
        p.part = {80.0f, 100.0f, 340.0f, 300.0f};
        p.ink = {70.0f, 95.0f, 370.0f, 340.0f};
        g.setPicture(file, p);
    }
    g.setViewport(1080.0f, 720.0f);
    g.layout();
    CHECK(countText(g, "DETAIL A ") == 1);
    CHECK(countText(g, "DETAIL B") == 0);
    // The washer drawn twice: at the shelf's scale and in its bubble, far larger.
    std::vector<float> washers;
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.kind == GeoItemPicture && item.file == g.tree().fileAt("washer-m1.step")) {
            washers.push_back(item.width);
        }
    }
    REQUIRE(washers.size() == 2);
    CHECK(std::max(washers[0], washers[1]) >= 10.0f * std::min(washers[0], washers[1]));
    for (const Spread& spread : spreads(g.display().items(), nullptr, nullptr)) {
        CHECK(spread.spread < 0.01f);
    }
}

namespace {

// A folder of two specks and a tall, narrow tower, as test-models/large is: the specks sort first
// and get detail bubbles, the first at the shelf's left edge; the tower's square picture is mostly
// clear margin, and it stands right after the second bubble.
Gallery sizesWithATower(std::vector<Entry>& entries) {
    Gallery g;
    entries = {{"washer-m1.step"}, {"washer-m2.step"}, {"tower.step"}};
    g.setEntries("Parts", entries);
    struct Spec {
        std::array<double, 3> size;
        std::array<float, 4> part;
    };
    const Spec specs[3] = {{{1.2, 1.2, 0.3}, {80.0f, 100.0f, 340.0f, 300.0f}},
                           {{2.0, 2.0, 0.5}, {80.0f, 100.0f, 340.0f, 300.0f}},
                           {{190.0, 190.0, 1196.0}, {200.0f, 40.0f, 112.0f, 432.0f}}};
    for (u32 f = 0; f < 3; ++f) {
        const u32 file = g.tree().fileAt(entries[f].path);
        Facts facts;
        facts.size = specs[f].size;
        facts.hasBox = true;
        facts.estimated = false;
        facts.parts = 1;
        g.setFacts(file, facts);
        const double longest = *std::max_element(specs[f].size.begin(), specs[f].size.end());
        Picture p;
        p.width = p.height = 512;
        p.mmPerPixel = longest / double(std::max(specs[f].part[2], specs[f].part[3]));
        p.part = specs[f].part;
        p.ink = {specs[f].part[0] - 10.0f, specs[f].part[1] - 5.0f, specs[f].part[2] + 30.0f,
                 specs[f].part[3] + 40.0f};
        g.setPicture(file, p);
    }
    g.setViews(0x1F);
    g.setViewport(1080.0f, 720.0f);
    return g;
}

} // namespace

TEST_CASE("a part's card stays inside the view, the leftmost one's too, in every view") {
    std::vector<Entry> entries;
    Gallery g = sizesWithATower(entries);
    for (const u32 view :
         {GeoViewPlates, GeoViewSheet, GeoViewLadders, GeoViewAtlas, GeoViewCatalogue}) {
        CAPTURE(view);
        REQUIRE(g.setView(view));
        for (const Entry& e : entries) {
            const u32 file = g.tree().fileAt(e.path);
            CAPTURE(e.path);
            for (const bool selecting : {true, false}) {
                CAPTURE(selecting);
                if (selecting) {
                    g.click(GeoTargetFile, file, false);
                    g.hover(GeoTargetNone, 0);
                } else {
                    g.click(GeoTargetNone, 0, false);
                    g.hover(GeoTargetFile, file);
                }
                g.layout();
                for (const GeoGalleryItem& item : g.display().items()) {
                    if ((item.flags & GeoItemSticker) == 0 || (item.flags & GeoItemFixed) != 0) {
                        continue;
                    }
                    CHECK(item.x >= 0.0f);
                    CHECK(item.x + item.width <= 1080.0f);
                }
            }
        }
    }
}

// ---- G6: Ladders
// ----------------------------------------------------------------------------------

TEST_CASE("ladders: a strip a folder, its parts on a ruler in a lane, every line aligned") {
    for (const u32 style : {0u, 3u, 5u}) {
        for (const bool shared : {false, true}) {
            CAPTURE(style);
            CAPTURE(shared);
            Gallery g = measured();
            g.setViews(0b111);
            g.setStyle(style);
            REQUIRE(g.setView(GeoViewLadders));
            g.setLadders(shared);
            g.layout();
            checkAligned(g);
            // Each family a strip in a lane: bearing, the hex nuts, motor, and the top's own files.
            CHECK(g.result().lanes.size() == 4);
            CHECK(g.result().parts.size() == 61); // every file once
            usize rulers = 0;
            for (const GeoGalleryItem& item : g.display().items()) {
                rulers += (item.flags & GeoItemRuler) != 0 ? 1 : 0;
            }
            CHECK(rulers >= 4);
            // Headings stay at the top, as Sheet's.
            CHECK(countText(g, "IN THIS FOLDER") == 1);
        }
    }
}

TEST_CASE("ladders' poster: every family wrapped on one plate, framed, with a title block") {
    for (const u32 style : {0u, 3u}) {
        CAPTURE(style);
        Gallery g = measured();
        g.setViews(0b11111);
        g.setStyle(style);
        REQUIRE(g.setView(GeoViewLadders));
        g.setPoster(true, "11 November 2026");
        g.setViewport(1380.0f, 840.0f);
        REQUIRE(g.layout());
        checkAligned(g);
        CHECK(g.result().lanes.empty());      // nothing scrolls on paper
        CHECK(g.result().parts.size() == 61); // every file once
        CHECK(countText(g, "LADDERS") == 1);
        CHECK(countText(g, "on one plate") + countText(g, "its own scale") == 1);
        CHECK(countText(g, "DRAWN BY") == 1);
        CHECK(countText(g, "1 of 1") == 1);
        // Everything inside the frame: the outer rule's rectangle, the first item it draws last.
        const GeoGalleryItem* frame = nullptr;
        for (const GeoGalleryItem& item : g.display().items()) {
            if (item.kind == GeoItemRect && item.line_width == 1.5f && item.fill == 0 &&
                (item.flags & GeoItemSheet) == 0 && item.y <= 2.0f) {
                frame = &item;
            }
        }
        REQUIRE(frame != nullptr);
        for (const GeoGalleryItem& item : g.display().items()) {
            const float x0 = std::min(item.x, item.x + item.width),
                        x1 = std::max(item.x, item.x + item.width);
            const float y0 = std::min(item.y, item.y + item.height),
                        y1 = std::max(item.y, item.y + item.height);
            CHECK(x0 >= frame->x - 0.01f);
            CHECK(x1 <= frame->x + frame->width + 0.01f);
            CHECK(y0 >= frame->y - 0.01f);
            CHECK(y1 <= frame->y + frame->height + 0.01f);
        }
    }
}

TEST_CASE("atlas' poster: the whole map in a frame, beside a cartouche with its legend") {
    for (const u32 style : {0u, 3u}) {
        CAPTURE(style);
        Gallery g = measured();
        g.setViews(0b11111);
        g.setStyle(style);
        REQUIRE(g.setView(GeoViewAtlas));
        g.setAtlasCamera(4.0f, -300.0f,
                         -200.0f); // zoomed in: the poster shows the whole map anyway
        g.setPoster(true, "11 November 2026");
        g.setViewport(1380.0f, 840.0f);
        REQUIRE(g.layout());
        checkAligned(g);
        CHECK(countText(g, "AN ATLAS OF") == 1);
        CHECK(countText(g, "BEARING") >= 1); // a region in the legend
        const GeoGalleryItem* frame = nullptr;
        for (const GeoGalleryItem& item : g.display().items()) {
            CHECK((item.flags & GeoItemFixed) == 0); // no map card
            if (item.kind == GeoItemRect && item.line_width == 1.5f && item.fill == 0 &&
                (item.flags & GeoItemSheet) == 0 && item.y <= 2.0f) {
                frame = &item;
            }
        }
        REQUIRE(frame != nullptr);
        for (const GeoGalleryItem& item : g.display().items()) {
            const float x0 = std::min(item.x, item.x + item.width),
                        x1 = std::max(item.x, item.x + item.width);
            const float y0 = std::min(item.y, item.y + item.height),
                        y1 = std::max(item.y, item.y + item.height);
            CHECK(x0 >= frame->x - 0.01f);
            CHECK(x1 <= frame->x + frame->width + 0.01f);
            CHECK(y0 >= frame->y - 0.01f);
            CHECK(y1 <= frame->y + frame->height + 0.01f);
        }
    }
}

TEST_CASE("shadows end above the rule or ruler under a part's foot, in Plates and Ladders") {
    for (const u32 view : {u32(GeoViewPlates), u32(GeoViewLadders)}) {
        for (const u32 style : {0u, 3u}) {
            CAPTURE(view);
            CAPTURE(style);
            Gallery g = measured();
            g.setViews(0b11111);
            g.setStyle(style);
            REQUIRE(g.setView(view));
            g.setViewport(1380.0f, 840.0f);
            g.layout();
            const auto items = g.display().items();
            usize checked = 0;
            for (const GeoGalleryItem& part : items) {
                if (part.kind != GeoItemPicture || (part.flags & GeoItemFixed) != 0) {
                    continue;
                }
                const Picture& p = *g.tree().file(part.file).picture;
                const float k = part.width / float(p.width);
                const float inkL = part.x + p.ink[0] * k, inkR = inkL + p.ink[2] * k;
                const float inkBottom = part.y + (p.ink[1] + p.ink[3]) * k;
                for (const GeoGalleryItem& rule : items) {
                    // A level rule (a line or a ruler) across the part, under its foot.
                    float top = 0;
                    if (rule.kind == GeoItemLine && rule.height == 0.0f && rule.width > 0.0f) {
                        top = rule.y - rule.line_width / 2.0f;
                    } else if (rule.kind == GeoItemRect && (rule.flags & GeoItemRuler) != 0) {
                        top = rule.y;
                    } else {
                        continue;
                    }
                    if (rule.lane != part.lane || rule.x > inkR || rule.x + rule.width < inkL ||
                        top < part.line - 0.01f || top > part.line + 40.0f) {
                        continue;
                    }
                    CAPTURE(part.file);
                    CAPTURE(inkBottom);
                    CAPTURE(top);
                    CHECK(inkBottom <= top + 0.01f);
                    ++checked;
                }
            }
            CHECK(checked > 3);
        }
    }
}

TEST_CASE("pictures drawn large are details; none is drawn past 1.25 times its pixels") {
    // A large window, where Catalogue's page is drawn well over its own size.
    for (const u32 view : {u32(GeoViewPlates), u32(GeoViewSheet), u32(GeoViewCatalogue)}) {
        CAPTURE(view);
        Gallery g = measured();
        g.setViews(0b11111);
        REQUIRE(g.setView(view));
        g.setViewport(2400.0f, 1500.0f);
        const u32 spreads = view == GeoViewCatalogue ? g.catalogueSpreads() : 1;
        std::unordered_set<u32> large;
        for (u32 s = 0; s < spreads; ++s) {
            if (view == GeoViewCatalogue) {
                g.setCatalogueSpread(s);
            }
            g.layout();
            for (const GeoGalleryItem& item : g.display().items()) {
                if (item.kind != GeoItemPicture || (item.flags & GeoItemTurn) != 0) {
                    continue;
                }
                const Picture& p = *g.tree().file(item.file).picture;
                const float k = item.width / float(p.width); // points a picture pixel
                CAPTURE(s);
                CAPTURE(k);
                CHECK(k <= kMaxPointsPerPixel * 1.001f);
                CHECK(((item.flags & GeoItemDetail) != 0) == (k > kDetailAbove));
                if ((item.flags & GeoItemDetail) != 0) {
                    large.insert(item.file);
                }
            }
            // Those in view are the details wanted, at most 32.
            const std::vector<u32> details = g.details();
            CHECK(details.size() <= 32);
            for (const u32 f : details) {
                CHECK(large.contains(f));
            }
        }
        if (view == GeoViewCatalogue) {
            CHECK(!large.empty()); // its figures, at least, are drawn large
        }
    }
}

TEST_CASE("ladders: a lane scrolled sideways is hit where its parts show, and only there") {
    Gallery g = measured(200); // a long bearing strip
    g.setViews(0b111);
    g.setView(GeoViewLadders);
    g.layout();
    const GeoGalleryLane* longest = nullptr;
    for (const GeoGalleryLane& lane : g.result().lanes) {
        if (longest == nullptr || lane.content_width > longest->content_width) {
            longest = &lane;
        }
    }
    REQUIRE(longest != nullptr);
    CHECK(longest->content_width > longest->rect[2]); // longer than the track
    CHECK(countText(g, "more \xE2\x86\x92") >= 1);
    // A part past the track's right end, scrolled into view.
    const LayoutResult::Place* far = nullptr;
    for (const auto& p : g.result().parts) {
        if (p.lane == longest->id && p.x > longest->rect[0] + longest->rect[2] + 100.0f) {
            far = &p;
            break;
        }
    }
    REQUIRE(far != nullptr);
    const float scroll = far->x - longest->rect[0] - 40.0f;
    g.setLaneScroll(longest->id, scroll);
    const GeoGalleryHit hit = g.hit(far->x - scroll + far->w / 2.0f, far->y + far->h / 2.0f);
    CHECK(hit.target_kind == GeoTargetFile);
    CHECK(hit.target == far->index);
    // The head column beside the lane hits nothing in it.
    const GeoGalleryHit head = g.hit(longest->rect[0] - 20.0f, far->y + far->h / 2.0f);
    CHECK(head.target != far->index);
}

TEST_CASE("ladders: calipers on the hovered part, and its difference to the chosen one") {
    Gallery g = measured();
    g.setViews(0b111);
    REQUIRE(g.setFolder("motor"));
    g.setView(GeoViewLadders);
    g.layout();
    const u32 a = g.result().parts[0].index, b = g.result().parts[2].index;
    g.hover(GeoTargetFile, a);
    g.layout();
    CHECK(countText(g, " mm") >= 2); // across the top and down the side
    g.click(GeoTargetFile, b, false);
    g.hover(GeoTargetFile, a);
    g.layout();
    CHECK(countText(g, " against ") == 1);
    CHECK(countText(g, "\xCE\x94 x ") == 1);
}

TEST_CASE("ladders: scales read as drawings write them, and stamps show whole") {
    Gallery g = measured();
    g.setViews(0b111);
    g.setView(GeoViewLadders);
    g.layout();
    usize stamps = 0;
    const std::string& text = g.display().textBuffer();
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.kind != GeoItemText || item.font_size != 10.0f || item.weight != 600) {
            continue;
        }
        const std::string s = text.substr(item.text, item.text_length);
        if (s.find(':') == std::string::npos) {
            continue;
        }
        ++stamps;
        CAPTURE(s);
        CHECK(s.find("\xE2\x80\xA6") == std::string::npos); // not cut
        // 1:n or n:1 with n one of 1, 1.5, 2, 2.5, 3, 4, 5, 6 or 8 times a power of ten.
        const std::string n = s.substr(0, 6) == "larger" ? s.substr(s.find(' ') + 1) : s;
        const std::string left = n.substr(0, n.find(':')), right = n.substr(n.find(':') + 1);
        const double v = std::stod(left == "1" ? right : left);
        double lead = v;
        while (lead >= 10.0) {
            lead /= 10.0;
        }
        CHECK((lead == 1.0 || lead == 1.5 || lead == 2.0 || lead == 2.5 || lead == 3.0 ||
               lead == 4.0 || lead == 5.0 || lead == 6.0 || lead == 8.0));
    }
    CHECK(stamps >= 4);
}

TEST_CASE("ladders: arrows along a strip scroll its lane to show the selection") {
    Gallery g = measured(200);
    g.setViews(0b111);
    REQUIRE(g.setFolder("bearing"));
    g.setView(GeoViewLadders);
    g.layout();
    g.key(GeoKeyRight);
    g.layout();
    CHECK_FALSE(g.revealLane().has_value()); // the first part shows
    std::optional<std::pair<u32, float>> reveal;
    for (int i = 0; i < 40 && !reveal; ++i) {
        g.key(GeoKeyRight);
        g.layout();
        reveal = g.revealLane();
    }
    REQUIRE(reveal.has_value()); // past the track's end
    g.setLaneScroll(reveal->first, reveal->second);
    CHECK_FALSE(g.revealLane().has_value()); // shown, centred
}

// ---- G7: Atlas
// --------------------------------------------------------------------------------------

TEST_CASE("atlas: the folder as a map, fitted, its regions named, its card in the corner") {
    for (const u32 style : {0u, 3u, 5u}) {
        CAPTURE(style);
        Gallery g = measured();
        g.setViews(0b1111);
        g.setStyle(style);
        REQUIRE(g.setView(GeoViewAtlas));
        g.layout();
        for (const Spread& spread : spreads(g.display().items(), nullptr, nullptr)) {
            CHECK(spread.spread < 0.01f);
        }
        CHECK(g.result().contentHeight == 720.0f); // it pans; it does not scroll
        CHECK(countText(g, "BEARING") == 1);       // the regions, named in capitals
        CHECK(countText(g, "MOTOR") == 1);
        CHECK(countText(g, "Regions \xC2\xB7 100%") == 1); // the card's level
        usize fixed = 0;
        for (const GeoGalleryItem& item : g.display().items()) {
            fixed += (item.flags & GeoItemFixed) != 0 ? 1 : 0;
            if ((item.flags & GeoItemFixed) == 0 && item.kind == GeoItemPicture) {
                CHECK(item.x + item.width > 0.0f); // drawn only where it shows
            }
        }
        CHECK(fixed > 4);
    }
}

TEST_CASE("atlas: each level in draws more, and zooming keeps the point under the pointer") {
    Gallery g = measured();
    g.setViews(0b1111);
    g.setView(GeoViewAtlas);
    g.layout();
    const usize far = g.result().parts.size();
    AtlasCamera cam = g.atlasCamera();
    // The map point under (300, 200) stays there as it zooms.
    const float mx = (300.0f - cam.x) / cam.k, my = (200.0f - cam.y) / cam.k;
    const float k = cam.k * 2.5f;
    g.setAtlasCamera(k, 300.0f - (300.0f - cam.x) * k / cam.k,
                     200.0f - (200.0f - cam.y) * k / cam.k);
    cam = g.atlasCamera();
    CHECK((mx * cam.k + cam.x) == doctest::Approx(300.0f));
    CHECK((my * cam.k + cam.y) == doctest::Approx(200.0f));
    g.layout();
    CHECK(countText(g, "Districts") == 1);
    // Closest: every part in view is named under its cell.
    g.setAtlasCamera(cam.fit * 6.0f, cam.x, cam.y);
    g.layout();
    CHECK(countText(g, "Parts \xC2\xB7 600%") == 1);
    usize named = 0;
    for (const auto& p : g.result().parts) {
        named += countText(g, g.tree().file(p.index).names.label) > 0 ? 1 : 0;
    }
    CHECK(named == g.result().parts.size());
    CHECK(far > 0);
    for (const Spread& spread : spreads(g.display().items(), nullptr, nullptr)) {
        CHECK(spread.spread < 0.01f);
    }
}

TEST_CASE("atlas: the card takes clicks, and a search drops pins") {
    Gallery g = measured();
    g.setViews(0b1111);
    g.setView(GeoViewAtlas);
    g.layout();
    // A click on the card selects nothing beneath it.
    const GeoGalleryHit card = g.hit(20.0f, 700.0f);
    CHECK(card.target_kind == GeoTargetPeek);
    CHECK(g.click(card.target_kind, card.target, false).kind == GeoActionNone);
    g.setSearch("m12");
    g.layout();
    CHECK(countText(g, "Districts") == 1); // a search shows at least the districts
}

// ---- G8: Catalogue
// ----------------------------------------------------------------------------------

namespace {

void checkNoOverlaps(const Gallery& g) {
    struct Span {
        float baseline, left, right;
        std::string text;
    };
    std::vector<Span> spans;
    const std::string& buffer = g.display().textBuffer();
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.kind != GeoItemText) {
            continue;
        }
        const std::string text = buffer.substr(item.text, item.text_length);
        const float used = float(characters(text)) * (0.6f * item.font_size + item.tracking);
        const float left = item.align == 1   ? item.x + (item.width - used) / 2.0f
                           : item.align == 2 ? item.x + item.width - used
                                             : item.x;
        spans.push_back({item.baseline, left, left + used, text});
    }
    for (usize a = 0; a < spans.size(); ++a) {
        for (usize b = a + 1; b < spans.size(); ++b) {
            if (std::fabs(spans[a].baseline - spans[b].baseline) > 0.5f) {
                continue;
            }
            CAPTURE(spans[a].text);
            CAPTURE(spans[b].text);
            CHECK(
                (spans[a].right <= spans[b].left + 0.5f || spans[b].right <= spans[a].left + 0.5f));
        }
    }
}

} // namespace

TEST_CASE("catalogue: the folder as a book, every spread aligned, every file in its tables once") {
    for (const u32 style : {0u, 3u, 5u}) {
        CAPTURE(style);
        Gallery g = measured();
        g.setViews(0b11111);
        g.setStyle(style);
        REQUIRE(g.setView(GeoViewCatalogue));
        g.layout();
        CHECK(countText(g, "FRONTISPIECE") == 1);
        CHECK(countText(g, "PARTS CATALOGUE") == 1);
        const u32 book = g.catalogueSpreads();
        CHECK(book >= 6);
        std::vector<u32> rows;
        for (u32 s = 0; s < book; ++s) {
            CAPTURE(s);
            g.setCatalogueSpread(s);
            g.layout();
            CHECK(g.result().contentHeight == 720.0f); // it turns; it does not scroll
            for (const Spread& spread : spreads(g.display().items(), nullptr, nullptr)) {
                CHECK(spread.spread < 0.01f);
            }
            checkNoOverlaps(g);
            for (const auto& p : g.result().parts) {
                rows.push_back(p.index);
            }
        }
        std::sort(rows.begin(), rows.end());
        CHECK(rows.size() == 61); // every file in a table, once
        CHECK(std::unique(rows.begin(), rows.end()) == rows.end());
        // The index is at the end.
        g.setCatalogueSpread(g.catalogueIndexSpread());
        g.layout();
        CHECK(countText(g, "Index") >= 1);
    }
}

TEST_CASE("catalogue: everything on a page stays on it, clear of the fold") {
    // Washers as ISO 7089 has them, M1 to M36: many small, then a few far larger, so the plate's
    // rows fill unevenly, and the folder of measured() besides.
    Gallery washers;
    std::vector<Entry> entries;
    const double sizes[] = {4, 6, 7, 9, 10, 12, 16, 20, 24, 30, 37, 44, 56, 56, 66};
    for (const double size : sizes) {
        entries.push_back({std::format("washer/M{}.step", size)});
    }
    washers.setEntries("Parts", entries);
    for (u32 f = 0; f < washers.tree().fileCount(); ++f) {
        Facts facts;
        const double side = sizes[f];
        facts.size = {side, side, side / 12.0};
        facts.hasBox = true;
        facts.estimated = false;
        facts.parts = 1;
        washers.setFacts(f, facts);
        Picture p;
        p.width = p.height = 512;
        p.mmPerPixel = side * 1.3 / 512.0;
        p.part = {60.0f, 160.0f, 394.0f, 200.0f};
        p.ink = {50.0f, 150.0f, 420.0f, 240.0f};
        washers.setPicture(f, p);
    }
    washers.setViewport(1080.0f, 720.0f);
    for (int which = 0; which < 2; ++which) {
        for (const u32 style : {0u, 3u}) {
            CAPTURE(which);
            CAPTURE(style);
            Gallery g = which == 0 ? measured() : washers;
            g.setViews(0b11111);
            g.setStyle(style);
            REQUIRE(g.setView(GeoViewCatalogue));
            g.layout();
            for (u32 s = 0; s < g.catalogueSpreads(); ++s) {
                CAPTURE(s);
                g.setCatalogueSpread(s);
                g.layout();
                // The pages are the sheets; what follows the fold is the thumb index, off the edge.
                std::vector<GeoGalleryItem> pages;
                for (const GeoGalleryItem& item : g.display().items()) {
                    if (item.kind == GeoItemRect && (item.flags & GeoItemSheet) != 0) {
                        pages.push_back(item);
                    }
                }
                REQUIRE(!pages.empty());
                for (const GeoGalleryItem& item : g.display().items()) {
                    if (item.kind == GeoItemLine && item.width == 0.0f &&
                        std::abs(item.x - (pages[0].x + pages[0].width)) < 0.01f) {
                        break; // the fold, then the tabs
                    }
                    if ((item.flags & GeoItemSheet) != 0) {
                        continue;
                    }
                    float x0 = std::min(item.x, item.x + item.width),
                          x1 = std::max(item.x, item.x + item.width);
                    float y0 = std::min(item.y, item.y + item.height),
                          y1 = std::max(item.y, item.y + item.height);
                    if (item.kind == GeoItemPicture) {
                        // A picture's ink: its bitmap has room around it.
                        const Picture& p = *g.tree().file(item.file).picture;
                        const float k = item.width / float(p.width);
                        x0 = item.x + p.ink[0] * k;
                        x1 = x0 + p.ink[2] * k;
                        y0 = item.y + p.ink[1] * k;
                        y1 = y0 + p.ink[3] * k;
                    }
                    // Within a page's text, 40 of its 504 points in from either edge.
                    const bool inside = std::ranges::any_of(pages, [&](const GeoGalleryItem& p) {
                        const float margin = p.width * 40.0f / 504.0f - 0.5f;
                        return x0 >= p.x + margin && x1 <= p.x + p.width - margin &&
                               y0 >= p.y - 0.5f && y1 <= p.y + p.height + 0.5f;
                    });
                    const std::string text =
                        g.display().textBuffer().substr(item.text, item.text_length);
                    CAPTURE(item.kind);
                    CAPTURE(text);
                    CAPTURE(x0);
                    CAPTURE(x1);
                    CAPTURE(y0);
                    CAPTURE(pages[0].x);
                    CAPTURE(pages[0].width);
                    CHECK(inside);
                }
            }
        }
    }
}

TEST_CASE("catalogue: arrows turn the book; a contents line turns to its page") {
    Gallery g = measured();
    g.setViews(0b11111);
    g.setView(GeoViewCatalogue);
    g.layout();
    CHECK(g.key(GeoKeyLeft).kind == GeoActionNone); // the front already
    CHECK(g.key(GeoKeyRight).kind == GeoActionHandled);
    CHECK(g.catalogueSpread() == 1);
    g.layout();
    // A contents line: a page target, turning to that page's spread.
    for (const GeoGalleryItem& item : g.display().items()) {
        if (item.target_kind == GeoTargetPage && item.target > 4) {
            CHECK(g.click(GeoTargetPage, item.target, false).kind == GeoActionHandled);
            CHECK(g.catalogueSpread() == item.target / 2);
            break;
        }
    }
    g.setCatalogueSpread(1000);
    CHECK(g.catalogueSpread() == g.catalogueSpreads() - 1); // the back cover at most
    CHECK(g.key(GeoKeyRight).kind == GeoActionNone);
}
