#include "library/service.h"

#include "base/time.h"
#include "ipc/cache_eviction.h"
#include "library/scan.h"

#define XXH_INLINE_ALL
#include "base/xxhash.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace geo::library {
namespace {

constexpr usize kLookupsPerTurn = 128;
constexpr double kProgressSeconds = 0.1;
constexpr i64 kPartialAgeSeconds = 2 * 3600;
// A blob the gallery imported is dated this far back, so it is evicted before any file opened.
constexpr i64 kGalleryAgeSeconds = 365 * 24 * 3600;
constexpr u64 kMaxPictureBudget = 2ull << 30;
constexpr std::string_view kPictureSuffixes[] = {".png", ".json"};
constexpr std::string_view kFactsSuffixes[] = {".json"};

bool exists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0;
}

u64 sizeOf(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 ? u64(info.st_size) : 0;
}

bool readJson(const std::string& path, json::Value& value) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    std::string error;
    return json::parse(text.str(), value, error);
}

// Written beside its final name and renamed into place, so a reader never sees half of it.
bool writeJson(const std::string& path, const json::Value& value) {
    std::string temporary = path + ".XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        return false;
    }
    const std::string text = value.write();
    const bool written = write(fd, text.data(), text.size()) == ssize_t(text.size());
    close(fd);
    if (!written || rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

void makeDirectory(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

// The key of a cache file: its name without the suffix.
std::string keyOf(const std::string& blobPath) {
    const usize slash = blobPath.rfind('/');
    std::string name = blobPath.substr(slash == std::string::npos ? 0 : slash + 1);
    return name.substr(0, name.find('.'));
}

// A STEP file, by its name: a structure pass reads only those.
bool isStep(std::string_view path) {
    if (path.size() < 4) {
        return false;
    }
    std::string tail(path.substr(path.size() - 4));
    for (char& c : tail) {
        c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return isModelFile(path) && tail != ".stl";
}

u64 pictureBudgetFor(const std::string& directory) {
    struct statfs volume{};
    if (statfs(directory.c_str(), &volume) != 0) {
        return kMaxPictureBudget;
    }
    return std::min(u64(volume.f_bavail) * u64(volume.f_bsize) / 100, kMaxPictureBudget);
}

json::Value rect(const std::array<u32, 4>& r) {
    json::Value array = json::Value::array();
    for (const u32 v : r) {
        array.push(v);
    }
    return array;
}

bool readRect(const json::Value& value, std::array<u32, 4>& r) {
    if (value.size() != 4) {
        return false;
    }
    for (usize i = 0; i < 4; ++i) {
        r[i] = u32(value[i].asNumber());
    }
    return true;
}

} // namespace

json::Value PictureNote::toJson() const {
    json::Value value = json::Value::object();
    value.set("width", width);
    value.set("height", height);
    value.set("premultiplied", premultiplied);
    value.set("world_per_pixel", worldPerPixel);
    value.set("part", rect(partRect));
    value.set("ink", rect(inkRect));
    value.set("first_color", firstColor);
    return value;
}

bool PictureNote::fromJson(const json::Value& value, PictureNote& note) {
    PictureNote read;
    read.width = u32(value["width"].asNumber());
    read.height = u32(value["height"].asNumber());
    read.premultiplied = value["premultiplied"].asBool(true);
    read.worldPerPixel = value["world_per_pixel"].asNumber();
    read.firstColor = i32(value["first_color"].asNumber(-1));
    if (read.width == 0 || !readRect(value["part"], read.partRect) ||
        !readRect(value["ink"], read.inkRect)) {
        return false;
    }
    note = read;
    return true;
}

// ---- State and events ------------------------------------------------------------------------

struct Service::FileState {
    std::string key; // the import cache's key, once looked up
    std::string blobPath;
    std::optional<scene::Facts> facts;
    bool looked = false;
    bool blobReady = false;
    bool structured = false; // a structure pass has run, or none is needed
    bool failed = false;
    bool pictureCurrent = false;
    bool importing = false;
    bool structuring = false;
    bool painting = false;
    i32 folderColor = -1;
    u64 bytes = 0;
    double modified = 0.0;
};

struct Service::Event {
    enum class Kind : u8 {
        Want,
        Spec,
        Prefetch,
        Pressure,
        Gate,
        Estimated,
        Progress,
        ImportDone,
        StructureDone,
        PictureDone,
        Evicted,
        Turn,
    };
    Kind kind = Kind::Gate;
    std::vector<FileWant> wants;
    PictureSpec spec;
    bool flag = false;
    Pressure pressure = Pressure::Normal;
    std::string path;
    std::optional<scene::Facts> facts;
    bool ok = false;
    bool cancelled = false;
    bool fromCache = false;
    bool missingBlob = false;
    std::string message;
    std::string pngPath;
    std::string pictureKey;
    PictureNote note;
    u32 done = 0;
    u32 total = 0;
    u64 bytes = 0;
    i32 turnFrame = -1; // a turntable's frame, for PictureDone
};

struct Service::PictureJob {
    std::string path;
    std::string blobPath;
    std::string key;
    PictureSpec spec;
    i32 folderColor = -1;
    std::optional<scene::Facts> facts; // measured ones; read from the blob otherwise
    i32 turnFrame = -1;                // a turntable's frame, or -1 for the gallery's picture
};

// Receives an import's segments: the model's, for an estimated box, and its progress.
class Service::ImportRun final : public ipc::ImportListener {
public:
    ImportRun(Service& service, std::string path) : service_(service), path_(std::move(path)) {}

    void segmentReady(std::shared_ptr<scene::Segment> segment) override {
        if (estimated_ || !segment->view().hasModel()) {
            return;
        }
        estimated_ = true;
        Event event;
        event.kind = Event::Kind::Estimated;
        event.path = path_;
        const std::shared_ptr<scene::Segment> model[] = {std::move(segment)};
        event.facts = scene::measure(model);
        service_.post(std::move(event));
    }
    void progress(u32 done, u32 total) override {
        const double now = nowSeconds();
        if (now - lastProgress_ < kProgressSeconds && done != total) {
            return;
        }
        lastProgress_ = now;
        Event event;
        event.kind = Event::Kind::Progress;
        event.path = path_;
        event.done = done;
        event.total = total;
        service_.post(std::move(event));
    }
    void warning(const std::string&) override {}

private:
    Service& service_;
    std::string path_;
    bool estimated_ = false;
    double lastProgress_ = 0.0;
};

// ---- Public ----------------------------------------------------------------------------------

Service::Service(ServiceConfig config, Painter& painter, ServiceListener& listener)
    : config_(std::move(config)), painter_(painter), listener_(listener),
      gate_(config_.gate != nullptr ? *config_.gate : ImportGate::shared()), lookups_(1u << 30),
      structures_(1), imports_(std::max(1u, config_.slots)), pictures_(1) {
    const std::string& gallery = config_.galleryDirectory;
    makeDirectory(gallery);
    makeDirectory(gallery + "/facts");
    makeDirectory(gallery + "/thumbs");
    makeDirectory(gallery + "/roots");
    // What interrupted writes left behind, from a crash or a forced quit.
    ipc::sweepPartialFiles(config_.paths.cacheDirectory, ipc::kBlobSuffixes, kPartialAgeSeconds);
    ipc::sweepPartialFiles(gallery + "/thumbs", kPictureSuffixes, kPartialAgeSeconds);
    ipc::sweepPartialFiles(gallery + "/facts", kFactsSuffixes, kPartialAgeSeconds);
    blobBudget_ = config_.blobBudget != 0 ? config_.blobBudget
                                          : ipc::cacheBudget(config_.paths.cacheDirectory);
    pictureBudget_ = config_.pictureBudget != 0 ? config_.pictureBudget : pictureBudgetFor(gallery);
    blobBytes_ = ipc::cacheBytes(config_.paths.cacheDirectory);
    lastEviction_ = nowSeconds() - config_.evictionSeconds;
    updatePrefetch();

    gateListener_ = gate_.listen([this] {
        Event event;
        event.kind = Event::Kind::Gate;
        post(std::move(event));
    });
    scheduler_ = std::thread([this] { run(); });
    painterThread_ = std::thread([this] { paintLoop(); });
}

Service::~Service() {
    gate_.unlisten(gateListener_);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    pictureReady_.notify_all();
    scheduler_.join();
    for (auto& [path, cancel] : running_) {
        cancel->cancel();
    }
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&] { return jobs_ == 0; });
    }
    painterThread_.join();
}

void Service::want(std::vector<FileWant> files) {
    Event event;
    event.kind = Event::Kind::Want;
    event.wants = std::move(files);
    post(std::move(event));
}

void Service::setPicture(const PictureSpec& spec) {
    Event event;
    event.kind = Event::Kind::Spec;
    event.spec = spec;
    post(std::move(event));
}

void Service::turn(const std::string& path, u32 frames, u32 pixels) {
    Event event;
    event.kind = Event::Kind::Turn;
    event.path = path;
    event.done = frames;
    event.total = pixels;
    post(std::move(event));
}

void Service::setPrefetch(bool allowed) {
    Event event;
    event.kind = Event::Kind::Prefetch;
    event.flag = allowed;
    post(std::move(event));
}

void Service::setPressure(Pressure pressure) {
    Event event;
    event.kind = Event::Kind::Pressure;
    event.pressure = pressure;
    post(std::move(event));
}

Service::Stats Service::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void Service::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return idle_now_ && inbox_.empty(); });
}

std::string Service::pictureKey(const PictureSpec& spec, i32 firstColor,
                                const std::string& version) {
    std::string text =
        std::format("{}|{}|{}|{:.3f}|{:.4f}|{}|{}", spec.style, spec.colorMode, spec.pixels,
                    spec.pixelsPerPoint, spec.lineScale, firstColor, version);
    // A turntable's frame: its turn and scale too. The gallery's pictures keep their keys.
    if (spec.turn != 0.0f || spec.worldPerPixel != 0.0) {
        text += std::format("|turn {:.2f}|{:.6g}", spec.turn, spec.worldPerPixel);
    }
    return std::format("{:016x}", XXH3_64bits(text.data(), text.size()));
}

void Service::post(Event event) {
    {
        std::lock_guard lock(mutex_);
        inbox_.push_back(std::move(event));
        idle_now_ = false;
    }
    changed_.notify_all();
}

void Service::jobEnded() {
    // Told under the lock: once it is released this job no longer touches the service, which may
    // then be destroyed.
    std::lock_guard lock(mutex_);
    --jobs_;
    idle_.notify_all();
}

// ---- The scheduling thread ---------------------------------------------------------------------

void Service::run() {
    bool moreLookups = false;
    while (true) {
        std::vector<Event> events;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || !inbox_.empty() || moreLookups; });
            if (stopping_) {
                return;
            }
            events.swap(inbox_);
        }
        for (Event& event : events) {
            apply(event);
        }
        if (routesDirty_) {
            reroute();
        }
        moreLookups = lookUpSome();
        if (routesDirty_) {
            reroute();
        }
        startJobs();
        const bool idle = idleNow();
        {
            std::lock_guard lock(mutex_);
            idle_now_ = idle && inbox_.empty();
        }
        if (idle) {
            idle_.notify_all();
        }
    }
}

bool Service::idleNow() const {
    std::lock_guard lock(mutex_);
    return jobs_ == 0 && lookups_.waitingCount() == 0 && !evicting_;
}

void Service::apply(Event& event) {
    switch (event.kind) {
    case Event::Kind::Want: {
        wants_ = std::move(event.wants);
        wantIndex_.clear();
        for (usize i = 0; i < wants_.size(); ++i) {
            const FileWant& want = wants_[i];
            wantIndex_[want.path] = i;
            FileState& state = files_[want.path];
            if ((state.bytes != want.bytes && state.bytes != 0) ||
                (state.modified != want.modified && state.modified != 0.0)) {
                // The file changed: everything about it is looked up again.
                state.looked = false;
                state.failed = false;
                state.facts.reset();
                state.pictureCurrent = false;
                state.blobReady = false;
                state.structured = false;
            }
            state.bytes = want.bytes;
            state.modified = want.modified;
            if (state.folderColor != want.folderColor) {
                state.folderColor = want.folderColor;
                state.pictureCurrent = false;
                state.looked = false;
            }
        }
        routesDirty_ = true;
        break;
    }
    case Event::Kind::Spec:
        if (event.spec == spec_) {
            break;
        }
        spec_ = event.spec;
        // Pictures in the new spec may be kept already: each file is looked up again, those in
        // view first.
        for (auto& [path, state] : files_) {
            state.pictureCurrent = false;
            state.looked = false;
        }
        routesDirty_ = true;
        break;
    case Event::Kind::Prefetch:
        prefetchAllowed_ = event.flag;
        updatePrefetch();
        break;
    case Event::Kind::Pressure:
        pressure_ = event.pressure;
        // A critical warning starts nothing until memory recovers.
        imports_.setSlots(pressure_ == Pressure::Normal    ? std::max(1u, config_.slots)
                          : pressure_ == Pressure::Warning ? 1u
                                                           : 0u);
        if (pressure_ == Pressure::Critical) {
            for (auto& [path, cancel] : running_) {
                if (imports_.bandOf(path) != Band::Open) {
                    cancel->cancel();
                }
            }
        }
        updatePrefetch();
        break;
    case Event::Kind::Gate:
        if (gate_.opens() != 0) {
            cancelForOpens();
        }
        break;
    case Event::Kind::Estimated: {
        FileState& state = files_[event.path];
        if (event.facts && (!state.facts || state.facts->estimated)) {
            state.facts = std::move(event.facts);
            listener_.facts(event.path, *state.facts);
        }
        break;
    }
    case Event::Kind::Progress:
        listener_.progress(event.path, event.done, event.total);
        break;
    case Event::Kind::ImportDone: {
        FileState& state = files_[event.path];
        state.importing = false;
        running_.erase(event.path);
        if (event.cancelled) {
            imports_.stopped(event.path);
            std::lock_guard lock(mutex_);
            ++stats_.cancelled;
        } else {
            imports_.finish(event.path);
        }
        if (event.ok) {
            state.blobReady = true;
            state.structured = true;
            state.facts = std::move(event.facts);
            writeJson(factsPath(state.key), state.facts->toJson());
            listener_.facts(event.path, *state.facts);
            blobBytes_ += event.bytes;
            {
                std::lock_guard lock(mutex_);
                stats_.imports += event.fromCache ? 0 : 1;
            }
            updatePrefetch();
            maybeEvict();
        } else if (!event.cancelled) {
            state.failed = true;
            json::Value failure = json::Value::object();
            failure.set("version", scene::Facts::kVersion);
            failure.set("failed", event.message);
            if (!state.key.empty()) {
                writeJson(factsPath(state.key), failure);
            }
            listener_.failed(event.path, event.message);
            std::lock_guard lock(mutex_);
            ++stats_.failures;
        }
        routesDirty_ = true;
        break;
    }
    case Event::Kind::StructureDone: {
        FileState& state = files_[event.path];
        state.structuring = false;
        state.structured = true;
        structures_.finish(event.path);
        if (event.ok && event.facts && !state.facts) {
            state.facts = std::move(event.facts);
            // Kept, so a relaunch lays the file out before it imports.
            writeJson(factsPath(state.key), state.facts->toJson());
            listener_.facts(event.path, *state.facts);
        }
        if (event.ok) {
            std::lock_guard lock(mutex_);
            ++stats_.structures;
        }
        routesDirty_ = true;
        break;
    }
    case Event::Kind::Turn:
        if (event.done == 0 || event.path.empty()) {
            turn_.reset(); // a frame being drawn finishes, unheard
        } else if (!turn_ || turn_->path != event.path || turn_->frames != event.done ||
                   turn_->pixels != event.total) {
            const bool painting = turn_ && turn_->painting;
            turn_ = Turn{event.path, event.done, event.total, 0, painting};
        }
        routesDirty_ = true;
        break;
    case Event::Kind::PictureDone: {
        if (event.turnFrame >= 0) {
            // A turntable's frame: told if it is still wanted; the next one follows.
            FileState& state = files_[event.path];
            if (turn_ && turn_->path == event.path) {
                turn_->painting = false;
                if (event.ok) {
                    listener_.turnFrame(event.path, u32(event.turnFrame), turn_->frames,
                                        event.pngPath, event.note);
                    turn_->next = std::max(turn_->next, u32(event.turnFrame) + 1);
                    std::lock_guard lock(mutex_);
                    ++stats_.pictures;
                } else if (event.missingBlob) {
                    state.blobReady = false; // evicted: imported again, then drawn
                } else {
                    turn_.reset(); // it cannot be drawn turned: Peek keeps its picture
                }
            } else if (turn_) {
                turn_->painting = false;
            }
            routesDirty_ = true;
            break;
        }
        FileState& state = files_[event.path];
        state.painting = false;
        pictures_.finish(event.path);
        if (event.facts && (!state.facts || state.facts->estimated)) {
            state.facts = std::move(event.facts);
            writeJson(factsPath(state.key), state.facts->toJson());
            listener_.facts(event.path, *state.facts);
        }
        if (event.ok) {
            // A picture of a spec since replaced is kept, but not shown.
            if (event.pictureKey ==
                pictureKey(spec_, firstColorOf(state), config_.pictureVersion)) {
                state.pictureCurrent = true;
                listener_.picture(event.path, event.pngPath, event.note);
            }
            std::lock_guard lock(mutex_);
            ++stats_.pictures;
        } else if (event.missingBlob) {
            state.blobReady = false; // evicted: imported again
        } else {
            state.failed = true;
            listener_.failed(event.path, event.message);
            std::lock_guard lock(mutex_);
            ++stats_.failures;
        }
        routesDirty_ = true;
        break;
    }
    case Event::Kind::Evicted:
        evicting_ = false;
        lastEviction_ = nowSeconds();
        blobBytes_ = event.bytes;
        updatePrefetch();
        break;
    }
}

i32 Service::firstColorOf(const FileState& state) const {
    return state.facts && state.facts->parts == 1 ? state.folderColor : -1;
}

std::string Service::factsPath(const std::string& key) const {
    return std::format("{}/facts/{}.json", config_.galleryDirectory, key);
}

std::string Service::picturePath(const std::string& key, const std::string& picture) const {
    return std::format("{}/thumbs/{}.{}.png", config_.galleryDirectory, key, picture);
}

std::string Service::notePath(const std::string& key, const std::string& picture) const {
    return std::format("{}/thumbs/{}.{}.json", config_.galleryDirectory, key, picture);
}

bool Service::lookUpSome() {
    for (usize i = 0; i < kLookupsPerTurn; ++i) {
        const std::optional<std::string> path = lookups_.next();
        if (!path) {
            return false;
        }
        lookups_.finish(*path);
        lookUp(*path, files_[*path]);
        routesDirty_ = true;
    }
    return lookups_.waitingCount() != 0;
}

void Service::lookUp(const std::string& path, FileState& state) {
    state.looked = true;
    std::string error;
    const std::string blobPath = ipc::cachePathFor(path, config_.paths, error);
    if (blobPath.empty()) {
        state.failed = true;
        listener_.failed(path, error);
        std::lock_guard lock(mutex_);
        ++stats_.failures;
        return;
    }
    state.blobPath = blobPath;
    state.key = keyOf(blobPath);
    state.blobReady = exists(blobPath);

    json::Value kept;
    if (!state.facts && readJson(factsPath(state.key), kept)) {
        if (kept["failed"].isString()) {
            state.failed = true;
            listener_.failed(path, kept["failed"].asString());
            return;
        }
        scene::Facts facts;
        if (scene::Facts::fromJson(kept, facts)) {
            state.structured = true;
            state.facts = std::move(facts);
            listener_.facts(path, *state.facts);
        }
    }
    if (!state.facts) {
        return;
    }

    const std::string picture = pictureKey(spec_, firstColorOf(state), config_.pictureVersion);
    PictureNote note;
    json::Value noted;
    const std::string png = picturePath(state.key, picture);
    if (readJson(notePath(state.key, picture), noted) && PictureNote::fromJson(noted, note) &&
        exists(png)) {
        state.pictureCurrent = true;
        listener_.picture(path, png, note);
        std::lock_guard lock(mutex_);
        ++stats_.lookups;
    }
}

void Service::reroute() {
    routesDirty_ = false;
    std::vector<Want> lookups, structures, imports, pictures;
    for (const FileWant& want : wants_) {
        const Want job{want.path, want.band, want.order, want.bytes};
        const FileState& state = files_[want.path];
        if (!state.looked) {
            lookups.push_back(job);
            continue;
        }
        if (state.failed) {
            continue;
        }
        const bool needsFacts = !state.facts || state.facts->estimated;
        if (!needsFacts && state.pictureCurrent) {
            continue;
        }
        if (state.blobReady && !state.importing) {
            pictures.push_back(job);
            continue;
        }
        imports.push_back(job);
        if (!state.facts && !state.structured && !state.importing && isStep(want.path)) {
            structures.push_back(job);
        }
    }
    // Peek's turntable needs its file's cache file: looked up, or imported first of all.
    if (turn_) {
        const FileState& state = files_[turn_->path];
        const auto other = [&](const Want& w) { return w.id == turn_->path; };
        const Want job{turn_->path, Band::Open, -1.0, state.bytes};
        if (!state.looked) {
            std::erase_if(lookups, other);
            lookups.push_back(job);
        } else if (!state.blobReady && !state.importing && !state.failed) {
            std::erase_if(imports, other);
            imports.push_back(job);
        }
    }
    lookups_.want(lookups);
    structures_.want(structures);
    imports_.want(imports);
    pictures_.want(pictures);
}

void Service::startJobs() {
    for (const std::string& path : imports_.toCancel()) {
        if (const auto it = running_.find(path); it != running_.end()) {
            it->second->cancel();
        }
    }
    // The job first, then its ticket: a ticket taken for nothing would wake this thread again
    // when returned.
    while (const std::optional<std::string> path = imports_.next()) {
        ImportGate::Ticket ticket = gate_.tryEnter(config_.workers);
        if (!ticket) {
            imports_.stopped(*path); // waits again, in its place
            break;
        }
        startImport(*path, std::move(ticket));
    }
    // A structure pass is quick and needs no OCCT workers, but waits while a file is being opened.
    if (gate_.opens() == 0) {
        if (const std::optional<std::string> path = structures_.next()) {
            startStructure(*path);
        }
    }
    // One picture at a time: Peek's turntable first, then the gallery's.
    if (pictures_.runningCount() == 0 && !(turn_ && turn_->painting)) {
        if (!startTurnFrame()) {
            if (const std::optional<std::string> path = pictures_.next()) {
                startPicture(*path);
            }
        }
    }
}

bool Service::startTurnFrame() {
    if (!turn_ || turn_->next >= turn_->frames) {
        return false;
    }
    const FileState& state = files_[turn_->path];
    if (!state.blobReady || state.importing || state.key.empty()) {
        return false;
    }
    PictureSpec spec = spec_;
    spec.pixels = turn_->pixels;
    // One scale for every turn: the box's diagonal fits whichever way it faces.
    if (state.facts && state.facts->hasBox) {
        const auto& f = *state.facts;
        const double dx = f.max[0] - f.min[0], dy = f.max[1] - f.min[1], dz = f.max[2] - f.min[2];
        spec.worldPerPixel = std::sqrt(dx * dx + dy * dy + dz * dz) * 1.1 / double(spec.pixels);
    }
    const i32 firstColor = firstColorOf(state);
    // Frames drawn before are read back, not drawn again.
    while (turn_->next < turn_->frames) {
        spec.turn = 360.0f * float(turn_->next) / float(turn_->frames);
        const std::string picture = pictureKey(spec, firstColor, config_.pictureVersion);
        json::Value noted;
        PictureNote note;
        const std::string png = picturePath(state.key, picture);
        if (!(readJson(notePath(state.key, picture), noted) && PictureNote::fromJson(noted, note) &&
              exists(png))) {
            break;
        }
        listener_.turnFrame(turn_->path, turn_->next, turn_->frames, png, note);
        ++turn_->next;
    }
    if (turn_->next >= turn_->frames) {
        return false;
    }
    auto job = std::make_unique<PictureJob>();
    job->path = turn_->path;
    job->blobPath = state.blobPath;
    job->key = state.key;
    job->spec = spec;
    job->folderColor = state.folderColor;
    job->turnFrame = i32(turn_->next);
    if (state.facts && !state.facts->estimated) {
        job->facts = state.facts;
    }
    turn_->painting = true;
    {
        std::lock_guard lock(mutex_);
        ++jobs_;
        pictureJob_ = std::move(job);
    }
    pictureReady_.notify_all();
    return true;
}

void Service::startImport(const std::string& path, ImportGate::Ticket ticket) {
    FileState& state = files_[path];
    state.importing = true;
    const std::shared_ptr<ipc::ImportCancel> cancel = std::make_shared<ipc::ImportCancel>();
    running_[path] = cancel;
    {
        std::lock_guard lock(mutex_);
        ++jobs_;
    }
    std::thread([this, path, blobPath = state.blobPath, cancel, workers = ticket.workers(),
                 ticket = std::move(ticket)]() mutable {
        Event done;
        done.kind = Event::Kind::ImportDone;
        done.path = path;
        // Another process importing this file holds its lock; when it lets go the file is cached.
        const int source = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (source >= 0) {
            while (flock(source, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK &&
                   !cancel->cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (cancel->cancelled()) {
            done.cancelled = true;
        } else if (ipc::isCached(path, config_.paths)) {
            scene::Facts facts;
            done.fromCache = true;
            done.ok = scene::measureCacheFile(blobPath, facts, done.message);
            done.facts = std::move(facts);
        } else {
            {
                std::lock_guard lock(mutex_);
                ++stats_.helpers;
            }
            ImportRun run(*this, path);
            ipc::ImportOptions options;
            options.workers = workers;
            const ipc::ImportOutcome outcome =
                ipc::importFile(path, config_.paths, run, cancel.get(), options);
            if (cancel->cancelled()) {
                done.cancelled = true;
            } else if (outcome.ok) {
                scene::Facts facts;
                done.ok = scene::measureCacheFile(outcome.blobPath, facts, done.message);
                done.facts = std::move(facts);
                done.fromCache = outcome.fromCache;
                done.bytes = sizeOf(outcome.blobPath);
                if (!outcome.fromCache) {
                    // Below anything opened, so a gallery's imports are evicted first.
                    timeval now{};
                    gettimeofday(&now, nullptr);
                    const timeval times[2] = {{now.tv_sec - kGalleryAgeSeconds, 0},
                                              {now.tv_sec - kGalleryAgeSeconds, 0}};
                    utimes(outcome.blobPath.c_str(), times);
                }
            } else {
                done.message = outcome.message;
            }
        }
        if (source >= 0) {
            close(source); // and its lock
        }
        ticket.release();
        post(std::move(done));
        jobEnded();
    }).detach();
}

void Service::startStructure(const std::string& path) {
    files_[path].structuring = true;
    {
        std::lock_guard lock(mutex_);
        ++jobs_;
        ++stats_.helpers;
    }
    std::thread([this, path] {
        Event done;
        done.kind = Event::Kind::StructureDone;
        done.path = path;
        struct Collect final : ipc::ImportListener {
            std::vector<std::shared_ptr<scene::Segment>> segments;
            void segmentReady(std::shared_ptr<scene::Segment> segment) override {
                segments.push_back(std::move(segment));
            }
            void progress(u32, u32) override {}
            void warning(const std::string&) override {}
        } collect;
        const ipc::ImportOutcome outcome = ipc::readStructure(path, config_.paths, collect);
        done.ok = outcome.ok;
        if (outcome.ok) {
            done.facts = scene::measure(collect.segments);
        }
        post(std::move(done));
        jobEnded();
    }).detach();
}

void Service::startPicture(const std::string& path) {
    FileState& state = files_[path];
    state.painting = true;
    auto job = std::make_unique<PictureJob>();
    job->path = path;
    job->blobPath = state.blobPath;
    job->key = state.key;
    job->spec = spec_;
    job->folderColor = state.folderColor;
    if (state.facts && !state.facts->estimated) {
        job->facts = state.facts;
    }
    {
        std::lock_guard lock(mutex_);
        ++jobs_;
        pictureJob_ = std::move(job);
    }
    pictureReady_.notify_all();
}

void Service::cancelForOpens() {
    // A file being opened gets the cores: imports not for files in view make way.
    for (auto& [path, cancel] : running_) {
        const std::optional<Band> band = imports_.bandOf(path);
        if (!band || *band >= Band::Near) {
            cancel->cancel();
        }
    }
}

void Service::updatePrefetch() {
    const bool roomy = blobBytes_ <= blobBudget_ / 10 * 7;
    const bool allowed = prefetchAllowed_ && pressure_ != Pressure::Critical && roomy;
    imports_.setPrefetch(allowed);
    structures_.setPrefetch(allowed);
}

void Service::maybeEvict() {
    if (evicting_ || nowSeconds() - lastEviction_ < config_.evictionSeconds) {
        return;
    }
    evicting_ = true;
    // Files open or in view are spared.
    std::vector<std::string> blobs, pictures;
    for (const FileWant& want : wants_) {
        if (want.band > Band::Visible) {
            continue;
        }
        const FileState& state = files_[want.path];
        if (state.key.empty()) {
            continue;
        }
        blobs.push_back(state.blobPath);
        const std::string picture = pictureKey(spec_, firstColorOf(state), config_.pictureVersion);
        pictures.push_back(picturePath(state.key, picture));
    }
    {
        std::lock_guard lock(mutex_);
        ++jobs_;
        ++stats_.evictions;
    }
    std::thread([this, blobs = std::move(blobs), pictures = std::move(pictures)] {
        const std::string& directory = config_.paths.cacheDirectory;
        ipc::evictCache(directory, blobBudget_, blobs);
        ipc::evictCache(config_.galleryDirectory + "/thumbs", pictureBudget_, pictures,
                        kPictureSuffixes);
        Event done;
        done.kind = Event::Kind::Evicted;
        done.bytes = ipc::cacheBytes(directory);
        post(std::move(done));
        jobEnded();
    }).detach();
}

// ---- The picture thread ------------------------------------------------------------------------

void Service::paintLoop() {
    bool painted = false;
    while (true) {
        std::unique_ptr<PictureJob> job;
        {
            std::unique_lock lock(mutex_);
            const bool woken = pictureReady_.wait_for(
                lock, std::chrono::seconds(2), [&] { return stopping_ || pictureJob_ != nullptr; });
            if (stopping_ && pictureJob_ == nullptr) {
                break;
            }
            if (!woken) {
                if (painted) {
                    painter_.trim(); // idle: let go of the last file and its buffers
                    painted = false;
                }
                continue;
            }
            job = std::move(pictureJob_);
        }
        painted = true;
        Event done;
        done.kind = Event::Kind::PictureDone;
        done.path = job->path;
        done.turnFrame = job->turnFrame;
        std::optional<scene::Facts> facts = job->facts;
        if (!facts) {
            scene::Facts measured;
            if (!scene::measureCacheFile(job->blobPath, measured, done.message)) {
                done.missingBlob = true;
                post(std::move(done));
                jobEnded();
                continue;
            }
            facts = measured;
            done.facts = std::move(measured);
        }
        const i32 firstColor = facts->parts == 1 ? job->folderColor : -1;
        const std::string picture = pictureKey(job->spec, firstColor, config_.pictureVersion);
        const std::string png = picturePath(job->key, picture);
        std::string temporary = png + ".XXXXXX";
        const int fd = mkstemp(temporary.data());
        if (fd >= 0) {
            close(fd);
        }
        PictureNote note;
        std::string error;
        if (fd >= 0 &&
            painter_.paint(job->blobPath, job->spec, firstColor, temporary, note, error) &&
            rename(temporary.c_str(), png.c_str()) == 0) {
            note.firstColor = firstColor;
            // The note last: it marks the picture whole.
            done.ok = writeJson(notePath(job->key, picture), note.toJson());
            done.pngPath = png;
            done.pictureKey = picture;
            done.note = note;
        } else {
            unlink(temporary.c_str());
            done.missingBlob = !exists(job->blobPath);
            done.message = error.empty() ? "This file's picture could not be drawn." : error;
        }
        post(std::move(done));
        jobEnded();
    }
}

} // namespace geo::library
