#include "cache/blob_writer.h"
#include "render/looks.h"
#include "render/style.h"
#include "scene/scene.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <unistd.h>

using namespace geo;
using namespace geo::render;
using format::NodeKind;

namespace {

// drive
//   stator: Stator_Core (occurrence 0), widget (occurrence 1)
//   rotor: gizmo (occurrence 2), Magnet 1 (occurrence 3)
struct Drive {
    scene::Scene scene;

    explicit Drive(u32 documents = 1) {
        import::ModelData model;
        const auto node = [&](u32 parent, u32 end, u32 first, u32 last, NodeKind kind,
                              std::string_view name) {
            model.nodes.push_back({parent, end, first, last, u32(model.strings.size()),
                                   u32(name.size()), u32(kind), 0});
            model.strings += name;
        };
        node(format::kNoParent, 7, 0, 4, NodeKind::Assembly, "drive");
        node(0, 4, 0, 2, NodeKind::Assembly, "stator");
        node(1, 3, 0, 1, NodeKind::Part, "Stator_Core");
        node(1, 4, 1, 2, NodeKind::Part, "widget");
        node(0, 7, 2, 4, NodeKind::Assembly, "rotor");
        node(4, 6, 2, 3, NodeKind::Part, "gizmo");
        node(4, 7, 3, 4, NodeKind::Part, "Magnet 1");
        model.definitions = {{{0, 0, 0}, {1, 1, 1}, 1, 0}};
        const std::array<float, 12> identity = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        model.occurrences = {{identity, 0, 2, 0xFF2040A0u, 0},
                             {identity, 0, 3, 0, 0},
                             {identity, 0, 5, 0xFF2040A0u, 0},
                             {identity, 0, 6, 0xFF00FF00u, 0}};

        char path[] = "/tmp/geolooksXXXXXX";
        const int fd = mkstemp(path);
        REQUIRE(fd >= 0);
        import::BlobStream stream(fd, nullptr);
        REQUIRE(stream.writeModel(model));
        REQUIRE(stream.finish({}));
        for (u32 d = 0; d < documents; ++d) {
            std::vector<std::shared_ptr<scene::Segment>> segments;
            std::string error;
            REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
            scene::Document& document = scene.document(scene.addDocument());
            for (auto& segment : segments) {
                REQUIRE(document.add(std::move(segment), error));
            }
        }
        close(fd);
        unlink(path);
    }

    Look lookOf(const LookAssignment& assignment, u32 occurrence) const {
        return assignment.looks[assignment.ofOccurrence[occurrence]];
    }
};

} // namespace

TEST_CASE("names suggest materials, first keyword winning") {
    CHECK(materialFromName("Stator_Core") == MaterialClass::LaminationCool);
    CHECK(materialFromName("rotor lamination stack") == MaterialClass::LaminationWarm);
    CHECK(materialFromName("Back_Iron") == MaterialClass::LaminationWarm);
    CHECK(materialFromName("Winding A") == MaterialClass::Copper);
    CHECK(materialFromName("Magnet 3") == MaterialClass::Magnet);
    CHECK(materialFromName("encoder_pcb") == MaterialClass::Pcb);
    CHECK(materialFromName("M3 bolt") == MaterialClass::Steel);
    CHECK(materialFromName("Housing") == MaterialClass::Aluminium);
    CHECK(materialFromName("frame_core.1") == MaterialClass::LaminationCool);
    CHECK_FALSE(materialFromName("corewidget").has_value());
    CHECK_FALSE(materialFromName("widget").has_value());
}

TEST_CASE("each style turns a look into its own texels") {
    const Look copper{Look::Kind::Material, MaterialClass::Copper};
    // Illustration draws copper in mustard bands, packed red first with opaque alpha.
    const std::array<u32, 3> bands = bodyTexels(styleNamed(StyleName::Illustration), copper);
    CHECK(bands[0] == 0xFF29B8F4u);
    CHECK(bands[2] == 0xFF146D9Au);
    // Metal stores the finish in the middle texel: fully metallic, roughness 0.38.
    const std::array<u32, 3> finish = bodyTexels(styleNamed(StyleName::Metal), copper);
    CHECK((finish[1] & 0xFF) == 255);
    CHECK(((finish[1] >> 8) & 0xFF) == 97);
    // Blueprint and technical draw every part alike.
    const Look magnet{Look::Kind::Material, MaterialClass::Magnet};
    for (const StyleName name : {StyleName::Blueprint, StyleName::Technical}) {
        CHECK(bodyTexels(styleNamed(name), copper) == bodyTexels(styleNamed(name), magnet));
        CHECK_FALSE(styleNamed(name).colorModes);
    }
    // A file colour gets darker bands derived from it.
    Look file{Look::Kind::Color};
    file.color = 0xFF0000FFu; // pure red
    const std::array<u32, 3> red = bodyTexels(styleNamed(StyleName::Illustration), file);
    CHECK((red[0] & 0xFF) == 255);
    CHECK((red[1] & 0xFF) == 214); // 16% toward black
}

TEST_CASE("style uniforms are in pixels at the display's scale") {
    LookSettings settings;
    settings.wobble = 2.0f;
    const StyleUniforms u = makeStyleUniforms(styleNamed(StyleName::Illustration), settings, 2.0f);
    CHECK(u.silhouette == doctest::Approx(4.8f));
    CHECK(u.wobble == doctest::Approx(2.8f));
    CHECK(u.grain == doctest::Approx(1.0f));
    // Metal has no paper grain and no hand-drawn wobble, whatever the settings.
    const StyleUniforms metal = makeStyleUniforms(styleNamed(StyleName::Metal), settings, 2.0f);
    CHECK(metal.grain == 0.0f);
    CHECK(metal.wobble == 0.0f);
}

TEST_CASE("a line scale thins the ink and soft shadows, but not grain, grid or sampling") {
    LookSettings settings;
    settings.wobble = 2.0f;
    for (const StyleName name : {StyleName::Illustration, StyleName::Metal, StyleName::Blueprint}) {
        CAPTURE(int(name));
        const Style& style = styleNamed(name);
        const StyleUniforms full = makeStyleUniforms(style, settings, 2.0f);
        const StyleUniforms thin =
            makeStyleUniforms(style, settings, Raster{.pixelsPerPoint = 2.0f, .lineScale = 0.5f});
        CHECK(thin.silhouette == doctest::Approx(full.silhouette * 0.5f));
        CHECK(thin.partLine == doctest::Approx(full.partLine * 0.5f));
        CHECK(thin.crease == doctest::Approx(full.crease * 0.5f));
        CHECK(thin.highlight == doctest::Approx(full.highlight * 0.5f));
        CHECK(thin.ghostLine == doctest::Approx(full.ghostLine * 0.5f));
        CHECK(thin.wobble == doctest::Approx(full.wobble * 0.5f));
        if (full.shadowSoft > 1.0f) {
            CHECK(thin.shadowSoft == doctest::Approx(full.shadowSoft * 0.5f));
        }
        CHECK(thin.grain == full.grain);
        CHECK(thin.gridPitch == full.gridPitch);
        CHECK(thin.wobblePeriod == full.wobblePeriod);
        CHECK(thin.pixelsPerPoint == full.pixelsPerPoint);
        CHECK(thin.fold == full.fold);
    }
    // A soft shadow stays soft however small the picture: at 1 it would turn crisp.
    const StyleUniforms tiny = makeStyleUniforms(styleNamed(StyleName::Technical), settings,
                                                 Raster{.pixelsPerPoint = 1.0f, .lineScale = 0.1f});
    CHECK(tiny.shadowSoft > 1.0f);
}

TEST_CASE("studio draws polished metal on a dark sheet, with no lines but highlights") {
    const Style& studio = styleNamed(StyleName::Studio);
    CHECK(studio.physical);
    CHECK(studio.colorModes);
    CHECK(studio.dark);
    CHECK(styleNamed(StyleName::Blueprint).dark);
    CHECK_FALSE(styleNamed(StyleName::Metal).dark);
    LookSettings settings;
    settings.wobble = 2.0f;
    const StyleUniforms u = makeStyleUniforms(studio, settings, 2.0f);
    CHECK(u.silhouette == 0.0f);
    CHECK(u.partLine == 0.0f);
    CHECK(u.crease == 0.0f);
    CHECK(u.fold == 0.0f);
    CHECK_FALSE(drawsOutlines(u));
    CHECK(u.highlight > 0.0f);
    CHECK(u.ghostLine > 0.0f); // ghosts drawn as lines have no skin to show them otherwise
    CHECK(u.grain == 0.0f);
    CHECK(u.wobble == 0.0f);
    // Polished: less rough than Metal's satin, but never a mirror.
    const Look steel{Look::Kind::Material, MaterialClass::Steel};
    const u32 polished = (bodyTexels(studio, steel)[1] >> 8) & 0xFF;
    const u32 satin = (bodyTexels(styleNamed(StyleName::Metal), steel)[1] >> 8) & 0xFF;
    CHECK(polished < satin);
    CHECK(polished >= 38); // 0.15
    CHECK((bodyTexels(studio, Look{})[1] & 0xFF) == 255); // mono is stainless, a metal
}

TEST_CASE("the other styles keep their lines and their satin metal") {
    for (const StyleName name : {StyleName::Illustration, StyleName::Metal, StyleName::Clay,
                                 StyleName::Blueprint, StyleName::Technical}) {
        CAPTURE(int(name));
        const StyleUniforms u = makeStyleUniforms(styleNamed(name), LookSettings{}, 1.0f);
        CHECK(drawsOutlines(u));
        CHECK(u.fold == 0.85f);
        CHECK(u.metalDiffuse == 0.4f);
        CHECK(u.ghostLine == u.crease); // dashed ghost lines as wide as creases, as before
    }
}

TEST_CASE("material mode reads names, then cycles by sub-assembly") {
    const Drive drive;
    const LookAssignment looks = assignLooks(drive.scene, ColorMode::Material);
    REQUIRE(looks.ofOccurrence.size() == 4);
    CHECK(drive.lookOf(looks, 0).kind == Look::Kind::Material);
    CHECK(drive.lookOf(looks, 0).material == MaterialClass::LaminationCool);
    CHECK(drive.lookOf(looks, 3).material == MaterialClass::Magnet);
    // widget and gizmo tell nothing, so they take their groups' places in the cycle.
    CHECK(drive.lookOf(looks, 1).kind == Look::Kind::Cycle);
    CHECK(drive.lookOf(looks, 1).byGroup);
    CHECK(drive.lookOf(looks, 1).index == 0);
    CHECK(drive.lookOf(looks, 2).index == 1);
}

TEST_CASE("parts, file and mono modes") {
    const Drive drive;
    const LookAssignment parts = assignLooks(drive.scene, ColorMode::Parts);
    for (u32 o = 0; o < 4; ++o) {
        CHECK(drive.lookOf(parts, o).index == o);
    }

    const LookAssignment file = assignLooks(drive.scene, ColorMode::File);
    CHECK(file.ofOccurrence[0] == file.ofOccurrence[2]); // the same stored colour
    CHECK(drive.lookOf(file, 1).kind == Look::Kind::Neutral);
    CHECK(drive.lookOf(file, 3).color == 0xFF00FF00u);
    CHECK(file.looks.size() == 3);

    const LookAssignment mono = assignLooks(drive.scene, ColorMode::Mono);
    CHECK(mono.looks.size() == 1);
}

TEST_CASE("a document given a first colour starts the cycle there, leaving the count alone") {
    const Drive drive(3);
    const std::array<std::optional<u32>, 3> first = {std::nullopt, 2u, std::nullopt};
    const LookAssignment parts = assignLooks(drive.scene, ColorMode::Parts, first);
    REQUIRE(parts.ofOccurrence.size() == 12);
    for (u32 o = 0; o < 4; ++o) {
        CHECK(drive.lookOf(parts, o).index == o);         // counting from 0
        CHECK(drive.lookOf(parts, 4 + o).index == 2 + o); // its own count, from tomato
        CHECK(drive.lookOf(parts, 8 + o).index == 4 + o); // on from the first document
    }
    // A one-part file with a first colour is drawn in that colour of the cycle: tomato.
    CHECK(swatch(styleNamed(StyleName::Illustration), drive.lookOf(parts, 4)) ==
          swatch(styleNamed(StyleName::Illustration), Look{Look::Kind::Cycle, {}, false, 2}));

    // Material mode's fallback counts groups the same way; names still win.
    const LookAssignment material = assignLooks(drive.scene, ColorMode::Material, first);
    CHECK(drive.lookOf(material, 5).index == 2);
    CHECK(drive.lookOf(material, 6).index == 3);
    CHECK(drive.lookOf(material, 9).index == 2); // groups 2 and 3, after the first document's
    CHECK(drive.lookOf(material, 4).material == MaterialClass::LaminationCool);

    // File and mono modes have no cycle to start.
    const LookAssignment mono = assignLooks(drive.scene, ColorMode::Mono, first);
    CHECK(mono.looks.size() == 1);
    const LookAssignment file = assignLooks(drive.scene, ColorMode::File, first);
    CHECK(drive.lookOf(file, 7).color == 0xFF00FF00u);
}

TEST_CASE("swatches follow each occurrence's look and the style") {
    const Drive drive;
    const LookAssignment material = assignLooks(drive.scene, ColorMode::Material);
    const std::vector<u32> drawn = swatchColors(styleNamed(StyleName::Illustration), material);
    REQUIRE(drawn.size() == 4);
    CHECK(drawn[0] != drawn[3]); // a stator core and a magnet
    for (const u32 color : drawn) {
        CHECK((color >> 24) == 0xFFu); // opaque, so zero can mean unknown
    }
    CHECK(swatchColors(styleNamed(StyleName::Metal), material)[3] != drawn[3]);

    const LookAssignment mono = assignLooks(drive.scene, ColorMode::Mono);
    const std::vector<u32> same = swatchColors(styleNamed(StyleName::Illustration), mono);
    CHECK(std::all_of(same.begin(), same.end(), [&](u32 c) { return c == same[0]; }));
}
