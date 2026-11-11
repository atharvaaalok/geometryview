// The gallery's library on the Mac: the library service, with the engine's thumbnailer as its
// painter and the system's memory pressure as its brake.
#include "library.h"

#include "base/json.h"
#include "library/gate.h"
#include "library/scan.h"
#include "library/service.h"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace geo;

namespace {

// Draws pictures with one thumbnailer, made on the picture thread when first needed.
class MetalPainter final : public library::Painter {
public:
    explicit MetalPainter(std::string shaderLibrary) : shaderLibrary_(std::move(shaderLibrary)) {}
    ~MetalPainter() override {
        if (thumbnailer_ != nullptr) {
            geo_thumbnailer_destroy(thumbnailer_);
        }
    }

    bool paint(const std::string& blobPath, const library::PictureSpec& spec, i32 firstColor,
               const std::string& pngPath, library::PictureNote& note,
               std::string& error) override {
        @autoreleasepool {
            if (thumbnailer_ == nullptr) {
                thumbnailer_ = geo_thumbnailer_create(shaderLibrary_.c_str());
                if (thumbnailer_ == nullptr) {
                    error = "Pictures cannot be drawn on this Mac.";
                    return false;
                }
            }
            GeoRenderOptions options = geo_default_render_options();
            options.width = options.height = spec.pixels;
            options.overlay = false;
            options.azimuth_degrees = 35.0;
            options.elevation_degrees = 30.0;
            options.look.style = GeoStyle(spec.style);
            options.look.color_mode = GeoColorMode(spec.colorMode);
            options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
            options.pixels_per_point = spec.pixelsPerPoint;
            options.line_scale = spec.lineScale;
            options.first_color = firstColor;
            options.transparent = true;
            GeoThumbnail picture{};
            char message[256] = {};
            if (!geo_thumbnailer_render(thumbnailer_, blobPath.c_str(), &options, &picture, message,
                                        sizeof(message))) {
                error = message;
                return false;
            }
            const bool written = geo_thumbnail_write_png(&picture, pngPath.c_str());
            note.width = picture.width;
            note.height = picture.height;
            note.premultiplied = picture.premultiplied;
            note.worldPerPixel = picture.world_per_pixel;
            std::copy(picture.part_rect, picture.part_rect + 4, note.partRect.begin());
            std::copy(picture.ink_rect, picture.ink_rect + 4, note.inkRect.begin());
            geo_thumbnail_free(&picture);
            if (!written) {
                error = "The picture could not be saved.";
            }
            return written;
        }
    }

    void trim() override {
        if (thumbnailer_ != nullptr) {
            geo_thumbnailer_trim(thumbnailer_);
        }
    }

private:
    std::string shaderLibrary_;
    GeoThumbnailer* thumbnailer_ = nullptr;
};

// Hands the service's news to the front end's callback.
class CallbackListener final : public library::ServiceListener {
public:
    CallbackListener(GeoLibraryCallback callback, void* context)
        : callback_(callback), context_(context) {}

    void facts(const std::string& path, const scene::Facts& facts) override {
        GeoLibraryEvent event{};
        event.kind = GeoLibraryFacts;
        event.path = path.c_str();
        GeoFacts& out = event.facts;
        std::copy(facts.min.begin(), facts.min.end(), out.min);
        std::copy(facts.max.begin(), facts.max.end(), out.max);
        out.has_box = facts.hasBox;
        out.estimated = facts.estimated;
        out.triangles = facts.triangles;
        out.faces = facts.faces;
        out.parts = facts.parts;
        out.bodies = facts.bodies;
        out.definitions = facts.definitions;
        out.name = facts.name.c_str();
        send(event);
    }
    void picture(const std::string& path, const std::string& pngPath,
                 const library::PictureNote& note) override {
        GeoLibraryEvent event{};
        event.kind = GeoLibraryPicture;
        event.path = path.c_str();
        GeoPicture& out = event.picture;
        out.png_path = pngPath.c_str();
        out.width = note.width;
        out.height = note.height;
        out.world_per_pixel = note.worldPerPixel;
        std::copy(note.partRect.begin(), note.partRect.end(), out.part_rect);
        std::copy(note.inkRect.begin(), note.inkRect.end(), out.ink_rect);
        out.first_color = note.firstColor;
        send(event);
    }
    void progress(const std::string& path, u32 done, u32 total) override {
        GeoLibraryEvent event{};
        event.kind = GeoLibraryProgress;
        event.path = path.c_str();
        event.done = done;
        event.total = total;
        send(event);
    }
    void failed(const std::string& path, const std::string& message) override {
        GeoLibraryEvent event{};
        event.kind = GeoLibraryFailed;
        event.path = path.c_str();
        event.message = message.c_str();
        send(event);
    }

private:
    void send(const GeoLibraryEvent& event) {
        if (callback_ != nullptr) {
            callback_(context_, &event);
        }
    }
    GeoLibraryCallback callback_;
    void* context_;
};

void copyError(const std::string& text, char* error, uint32_t capacity) {
    if (error != nullptr && capacity > 0) {
        const usize count = std::min<usize>(text.size(), capacity - 1);
        std::memcpy(error, text.data(), count);
        error[count] = '\0';
    }
}

// Hands entries to a scan callback, as C strings that last for the call.
bool deliver(std::span<const library::ScanEntry> entries, GeoScanCallback callback, void* context) {
    std::vector<GeoScanEntry> out;
    out.reserve(entries.size());
    for (const library::ScanEntry& entry : entries) {
        out.push_back({entry.path.c_str(), entry.bytes, entry.modified, entry.folder, entry.files});
    }
    return callback(context, out.data(), u32(out.size()));
}

} // namespace

struct GeoLibrary {
    std::string galleryDirectory;
    std::unique_ptr<MetalPainter> painter;
    std::unique_ptr<CallbackListener> listener;
    std::unique_ptr<library::Service> service;
    dispatch_queue_t pressureQueue = nullptr;
    dispatch_source_t pressure = nullptr;
};

GeoLibrary* geo_library_create(const GeoLibraryDescriptor* descriptor) {
    if (descriptor == nullptr || descriptor->cache_directory == nullptr ||
        descriptor->import_helper_path == nullptr || descriptor->shader_library_path == nullptr) {
        return nullptr;
    }
    auto library = std::make_unique<GeoLibrary>();
    library::ServiceConfig config;
    config.paths = {descriptor->import_helper_path, descriptor->cache_directory};
    config.galleryDirectory = std::string(descriptor->cache_directory) + "/gallery";
    // Measured on 496 CADBench files (Docs/measurements/gallery/g1-foundation.md): throughput
    // rises with imports at once, since most files have too few slices to keep many workers busy,
    // and two workers each keep a large file from taking one worker's time.
    config.slots =
        descriptor->slots != 0 ? descriptor->slots : std::max(2u, library::performanceCores() / 2);
    config.workers = descriptor->workers != 0 ? descriptor->workers : 2;
    // Pictures are drawn again when the engine draws differently.
    config.pictureVersion = std::format("{}|1", geo_engine_version());
    if (access(config.paths.cacheDirectory.c_str(), W_OK) != 0) {
        return nullptr;
    }
    library->galleryDirectory = config.galleryDirectory;
    library->painter = std::make_unique<MetalPainter>(descriptor->shader_library_path);
    library->listener =
        std::make_unique<CallbackListener>(descriptor->callback, descriptor->context);
    library->service = std::make_unique<library::Service>(std::move(config), *library->painter,
                                                          *library->listener);

    // Memory running short slows the gallery, and stops it when critical.
    library::Service* service = library->service.get();
    library->pressureQueue = dispatch_queue_create(
        "geometryview.library.pressure",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    library->pressure =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                               DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                                   DISPATCH_MEMORYPRESSURE_CRITICAL,
                               library->pressureQueue);
    dispatch_source_t source = library->pressure;
    dispatch_source_set_event_handler(library->pressure, ^{
        const uintptr_t level = dispatch_source_get_data(source);
        service->setPressure(
            (level & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0 ? library::Pressure::Critical
            : (level & DISPATCH_MEMORYPRESSURE_WARN) != 0   ? library::Pressure::Warning
                                                            : library::Pressure::Normal);
    });
    dispatch_activate(library->pressure);
    return library.release();
}

void geo_library_destroy(GeoLibrary* library) {
    if (library == nullptr) {
        return;
    }
    if (library->pressure != nullptr) {
        // Cancelled, and its serial queue drained, so its handler never reaches a destroyed
        // service.
        dispatch_source_cancel(library->pressure);
        dispatch_sync(library->pressureQueue, ^{
                      });
        library->pressure = nullptr;
        library->pressureQueue = nullptr;
    }
    library->service.reset();
    delete library;
}

void geo_library_want(GeoLibrary* library, const GeoLibraryWant* wants, uint32_t count) {
    std::vector<library::FileWant> files;
    files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const GeoLibraryWant& want = wants[i];
        if (want.path == nullptr) {
            continue;
        }
        files.push_back({want.path, library::Band(std::min<u32>(want.band, 3)), want.order,
                         want.bytes, want.folder_color, want.modified});
    }
    library->service->want(std::move(files));
}

void geo_library_set_picture(GeoLibrary* library, const GeoPictureSpec* spec) {
    library::PictureSpec picture;
    picture.style = u32(spec->style);
    picture.colorMode = u32(spec->color_mode);
    picture.pixels = std::clamp<u32>(spec->pixels, 16, 2048);
    picture.pixelsPerPoint = spec->pixels_per_point > 0 ? spec->pixels_per_point : 2.0f;
    picture.lineScale = std::clamp(spec->line_scale, 0.1f, 1.0f);
    library->service->setPicture(picture);
}

void geo_library_set_prefetch(GeoLibrary* library, bool allowed) {
    library->service->setPrefetch(allowed);
}

void geo_library_wait_idle(GeoLibrary* library) { library->service->waitIdle(); }

GeoLibraryStats geo_library_stats(GeoLibrary* library) {
    const library::Service::Stats stats = library->service->stats();
    return {stats.helpers, stats.imports,  stats.structures, stats.pictures,
            stats.lookups, stats.failures, stats.cancelled};
}

bool geo_library_scan(GeoLibrary* library, const char* root, GeoScanCallback callback,
                      void* context, GeoScanSummary* summary, char* error,
                      uint32_t error_capacity) {
    library::Listing listing;
    listing.root = root;
    const library::ScanResult result =
        library::scan(root, [&](std::span<const library::ScanEntry> batch) {
            listing.entries.insert(listing.entries.end(), batch.begin(), batch.end());
            return callback == nullptr || deliver(batch, callback, context);
        });
    if (summary != nullptr) {
        *summary = {result.files, result.folders, result.truncated, result.stopped};
    }
    if (!result.error.empty()) {
        copyError(result.error, error, error_capacity);
        return false;
    }
    if (!result.stopped) {
        listing.truncated = result.truncated;
        listing.scanned = double(time(nullptr));
        const std::string path = library::listingPath(library->galleryDirectory + "/roots", root);
        const std::string temporary = path + ".partial";
        std::ofstream(temporary) << listing.toJson().write();
        rename(temporary.c_str(), path.c_str());
    }
    return true;
}

bool geo_library_kept_listing(GeoLibrary* library, const char* root, GeoScanCallback callback,
                              void* context) {
    std::ifstream file(library::listingPath(library->galleryDirectory + "/roots", root));
    if (!file) {
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    json::Value value;
    std::string error;
    library::Listing listing;
    if (!json::parse(text.str(), value, error) || !library::Listing::fromJson(value, listing) ||
        listing.root != root) {
        return false;
    }
    deliver(listing.entries, callback, context);
    return true;
}
