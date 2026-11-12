// The library service: turns the files a gallery wants into facts and pictures, from its caches
// where it can and by importing and drawing where it must.
//
// Each wanted file goes through what it still needs, in the order the gallery wants (library::Queue
// bands: open, visible, near, prefetch):
//   look-up    its facts and picture, kept beside the import cache, by the file's cache key;
//   structure  a STEP file's model alone, for an estimated box while it waits to import;
//   import     through the helper, its OCCT workers counted by the ImportGate;
//   picture    from its cache file, on a thread of its own, by a Painter.
// Facts and pictures outlive the cache files they came from, so a folder seen before shows at once
// without importing, and only a new drawing style imports again.
//
// Everything is decided on one scheduling thread, which also tells the listener; jobs run on
// threads of their own and report back to it.
#pragma once

#include "base/json.h"
#include "base/types.h"
#include "ipc/import_job.h"
#include "library/gate.h"
#include "library/queue.h"
#include "scene/facts.h"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace geo::library {

/// How pictures are drawn: one spec for a whole gallery.
struct PictureSpec {
    u32 style = 0;     // a GeoStyle
    u32 colorMode = 1; // a GeoColorMode
    u32 pixels = 256;  // the picture's side
    float pixelsPerPoint = 2.0f;
    float lineScale = 0.5f;
    // Peek's turntable: degrees about the upright added to the usual view, and a scale held for
    // every turn, in millimetres a pixel. Both 0 for the gallery's pictures, which fit their file.
    float turn = 0.0f;
    double worldPerPixel = 0.0;
    // Drawn this many times as large and averaged down: smooth outlines and shadow edges.
    u32 supersample = 1;

    bool operator==(const PictureSpec&) const = default;
};

/// What is kept beside a picture, and handed over with it.
struct PictureNote {
    u32 width = 0;
    u32 height = 0;
    bool premultiplied = true;
    double worldPerPixel = 0.0;    // millimetres a pixel
    std::array<u32, 4> partRect{}; // the part's box, x y width height, without its shadow
    std::array<u32, 4> inkRect{};  // everything drawn, its shadow included
    i32 firstColor = -1;           // the colour its parts start from, or -1

    json::Value toJson() const;
    static bool fromJson(const json::Value& value, PictureNote& note);
    bool operator==(const PictureNote&) const = default;
};

/// Draws pictures: the engine's thumbnailer on the Mac. Called on one thread only.
class Painter {
public:
    virtual ~Painter() = default;
    /// Draws a cache file into a PNG at `pngPath`. Returns false and sets `error` if it cannot.
    virtual bool paint(const std::string& blobPath, const PictureSpec& spec, i32 firstColor,
                       const std::string& pngPath, PictureNote& note, std::string& error) = 0;
    /// Lets go of what it holds between pictures.
    virtual void trim() {}
};

/// Told what the service learns, on its scheduling thread.
class ServiceListener {
public:
    virtual ~ServiceListener() = default;
    /// A file's facts: estimated while its geometry is unread, then measured.
    virtual void facts(const std::string& path, const scene::Facts& facts) = 0;
    /// A file's picture, drawn in the current spec.
    virtual void picture(const std::string& path, const std::string& pngPath,
                         const PictureNote& note) = 0;
    /// An import's progress, in slices.
    virtual void progress(const std::string& path, u32 done, u32 total) {
        (void)path, (void)done, (void)total;
    }
    /// A file that cannot be shown, and why. It is not tried again until it changes.
    virtual void failed(const std::string& path, const std::string& message) = 0;
    /// A frame of a turntable asked for with Service::turn: `frame` of `frames`, turned
    /// frame × 360 / frames degrees.
    /// A file's detail picture, asked for with Service::detail.
    virtual void detail(const std::string& path, const std::string& pngPath,
                        const PictureNote& note) {
        (void)path, (void)pngPath, (void)note;
    }
    virtual void turnFrame(const std::string& path, u32 frame, u32 frames,
                           const std::string& pngPath, const PictureNote& note) {
        (void)path, (void)frame, (void)frames, (void)pngPath, (void)note;
    }
};

struct ServiceConfig {
    ipc::ImportPaths paths;       // the helper and the import cache
    std::string galleryDirectory; // facts, thumbs and roots under it
    u32 slots = 2;                // imports at once
    u32 workers = 2;              // OCCT workers each
    ImportGate* gate = nullptr;   // the process's, when null
    u64 blobBudget = 0;           // the import cache's budget; ipc::cacheBudget when zero
    u64 pictureBudget = 0;        // min(free space / 100, 2 GB) when zero
    std::string pictureVersion;   // changes when pictures are drawn differently
    double evictionSeconds = 30.0;
};

/// A file the gallery wants, and how much.
struct FileWant {
    std::string path;
    Band band = Band::Visible;
    double order = 0.0;
    u64 bytes = 0;
    /// The colour a one-part file starts from (its folder's), or -1 for the cycle's own.
    i32 folderColor = -1;
    /// When the file last changed, from a scan; a change, or one of size, looks it up again.
    double modified = 0.0;
};

enum class Pressure : u8 { Normal, Warning, Critical };

class Service {
public:
    Service(ServiceConfig config, Painter& painter, ServiceListener& listener);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    /// Replaces what is wanted. Files no longer wanted stop waiting; their running imports finish
    /// unless a file in view needs the slot.
    void want(std::vector<FileWant> files);
    /// A new drawing style: pictures are drawn again, those in view first.
    void setPicture(const PictureSpec& spec);
    /// Whether files are imported ahead of being near the view.
    void setPrefetch(bool allowed);
    /// Files drawn large: each gets a detail picture (twice the pixels at twice the pixels a point,
    /// so the same picture, sharper), after the pictures in view, read back if kept, its file
    /// imported first if its cache file has gone. Replaces the last set.
    void detail(std::vector<std::string> paths);
    /// The spec a detail picture is drawn in.
    PictureSpec detailSpec() const;
    /// Forgets what it has told: every file is looked up again when next wanted, and its facts,
    /// picture and failure told again from the caches. Imports and pictures under way finish.
    void reset();
    /// Peek's turntable: `frames` pictures of one file, `pixels` square, turned evenly about the
    /// upright at one scale, ahead of any other picture; the file is imported again first if its
    /// cache file has gone. Frames drawn before come from the cache. 0 frames stops it.
    void turn(const std::string& path, u32 frames, u32 pixels);
    /// Memory running short: one import at a time on a warning; on a critical warning, imports not
    /// for a file being opened are cancelled and prefetching stops.
    void setPressure(Pressure pressure);

    struct Stats {
        u32 helpers = 0;    // helper processes started: structure passes and imports
        u32 imports = 0;    // imports that completed
        u32 structures = 0; // structure passes that completed
        u32 pictures = 0;   // pictures drawn
        u32 lookups = 0;    // files whose facts and picture came from the gallery's cache
        u32 failures = 0;
        u32 cancelled = 0;
        u32 evictions = 0;
    };
    Stats stats() const;

    /// Blocks until nothing waits or runs, for tests and tools.
    void waitIdle();

    /// The key of a picture drawn in `spec` with parts starting at `firstColor`.
    static std::string pictureKey(const PictureSpec& spec, i32 firstColor,
                                  const std::string& version);

private:
    struct FileState;
    struct Event;
    class ImportRun;

    // On the scheduling thread.
    void run();
    void apply(Event& event);
    bool lookUpSome();
    void lookUp(const std::string& path, FileState& state);
    void reroute();
    void startJobs();
    void startImport(const std::string& path, ImportGate::Ticket ticket);
    void startStructure(const std::string& path);
    void startPicture(const std::string& path);
    bool startTurnFrame();
    bool startDetail();
    bool detailKept(const std::string& path) const;
    void cancelForOpens();
    void maybeEvict();
    void updatePrefetch();
    void publishFacts(const std::string& path, FileState& state);
    i32 firstColorOf(const FileState& state) const;
    std::string factsPath(const std::string& key) const;
    std::string picturePath(const std::string& key, const std::string& pictureKey) const;
    std::string notePath(const std::string& key, const std::string& pictureKey) const;
    bool idleNow() const;

    // From any thread.
    void post(Event event);
    void jobEnded();

    // The picture thread.
    void paintLoop();

    ServiceConfig config_;
    Painter& painter_;
    ServiceListener& listener_;
    ImportGate& gate_;
    u64 gateListener_ = 0;

    mutable std::mutex mutex_; // guards the inbox, the counters and the picture handoff
    std::condition_variable changed_;
    std::condition_variable idle_;
    std::vector<Event> inbox_;
    bool stopping_ = false;
    bool idle_now_ = false;
    Stats stats_;
    u32 jobs_ = 0; // job threads still running

    // The picture thread's one job.
    struct PictureJob;
    std::unique_ptr<PictureJob> pictureJob_;
    std::condition_variable pictureReady_;

    // The scheduling thread's own.
    std::unordered_map<std::string, FileState> files_;
    std::vector<FileWant> wants_;
    std::unordered_map<std::string, usize> wantIndex_;
    Queue lookups_;
    Queue structures_;
    Queue imports_;
    Queue pictures_;
    PictureSpec spec_;
    struct Turn {
        std::string path;
        u32 frames = 0;
        u32 pixels = 0;
        u32 next = 0;       // the next frame to draw
        u32 generation = 0; // which turntable this is: frames of an earlier one are not told
    };
    std::optional<Turn> turn_;
    u32 turnGeneration_ = 0;
    // Files drawn large, and those whose detail picture has been told in the current spec.
    std::vector<std::string> details_;
    std::unordered_set<std::string> detailsTold_;
    // The picture thread's one slot is taken: a job handed over and not yet done, whatever kind.
    bool slotBusy_ = false;
    bool routesDirty_ = false;
    bool prefetchAllowed_ = true;
    Pressure pressure_ = Pressure::Normal;
    u64 blobBudget_ = 0;
    u64 pictureBudget_ = 0;
    u64 blobBytes_ = 0;
    double lastEviction_ = 0.0;
    bool evicting_ = false;
    std::unordered_map<std::string, std::shared_ptr<ipc::ImportCancel>> running_;

    std::thread scheduler_;
    std::thread painterThread_;
};

} // namespace geo::library
