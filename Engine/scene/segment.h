// One validated cache segment, read-only. On the Mac it is a mapping of the cache file, so geometry
// exists once in memory and is backed by a clean file. Where files cannot be mapped, as in the
// browser, it holds a copy of the bytes, aligned as a mapping would be, or it is resident: its
// bytes were streamed to the GPU, and memory keeps only the sections the CPU reads.
#pragma once

#include "base/types.h"
#include "format/blob.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace geo::scene {

/// A segment's bytes, kept outside this process's memory by whoever placed them there, such as a
/// renderer on the GPU. They are released with the last segment holding them.
class Placement {
public:
    virtual ~Placement() = default;
    /// Who placed it, so they can recognise their own.
    virtual const void* owner() const { return nullptr; }
};

/// Where a streamed segment's bytes go.
class SegmentSink {
public:
    virtual ~SegmentSink() = default;
    /// Somewhere to keep `size` bytes, or null to keep the segment in memory instead.
    virtual std::shared_ptr<Placement> place(u64 size) = 0;
    /// Writes bytes at `offset` within a placement. The bytes need not outlive the call.
    virtual void write(Placement& placement, u64 offset, std::span<const u8> bytes) = 0;
    /// A file of `size` bytes is about to be placed, segment by segment, so room for it can be
    /// made once, before the first.
    virtual void expect(u64 size) { (void)size; }
};

class Segment {
public:
    /// Maps [offset, offset + size) of `fd`, which stays usable after the descriptor closes.
    /// Returns null and sets `error` if it cannot be mapped or fails validation.
    static std::shared_ptr<Segment> open(int fd, u64 offset, u64 size, std::string& error);

    /// Copies a segment's bytes into memory it owns, aligned to the cache's 16 KB, so every
    /// renderer can use them as it would a mapping. Returns null and sets `error` if they fail
    /// validation.
    static std::shared_ptr<Segment> copy(std::span<const u8> bytes, std::string& error);

    /// A segment whose bytes are at `placement`, of which `held` holds copies of the sections the
    /// CPU reads, at `memory`'s pointers, as parseSegmentParts takes them. Returns null and sets
    /// `error` if they fail validation.
    static std::shared_ptr<Segment> resident(const format::SegmentHeader& header, u64 size,
                                             std::shared_ptr<u8[]> held,
                                             const format::SectionBytes& memory,
                                             std::shared_ptr<Placement> placement,
                                             std::string& error);

    ~Segment();
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    const format::SegmentView& view() const { return view_; }
    /// The segment's bytes, or null for a resident segment, whose bytes are at its placement.
    void* data() const { return data_; }
    usize size() const { return size_; }
    /// Whether the bytes are a mapping of a file, rather than a copy.
    bool mapped() const { return mapped_; }
    Placement* placement() const { return placement_.get(); }

private:
    Segment() = default;

    void* data_ = nullptr;
    usize size_ = 0;
    bool mapped_ = false;
    std::shared_ptr<u8[]> held_;
    std::shared_ptr<Placement> placement_;
    format::SegmentView view_;
};

/// Reads a cache file as its bytes arrive, in order, and hands over each segment as soon as its
/// last byte is in. With a sink, each segment's bytes go where the sink places them, and memory
/// keeps only the sections the CPU reads; without one, or where the sink declines, segments are
/// copied into memory whole.
class CacheStream {
public:
    using Ready = std::function<void(std::shared_ptr<Segment>)>;

    CacheStream(SegmentSink* sink, Ready ready);
    ~CacheStream();
    CacheStream(const CacheStream&) = delete;
    CacheStream& operator=(const CacheStream&) = delete;

    /// The file's real size, where the host knows it, as from a File or a Content-Length. The
    /// header must then agree, so what it asks to be placed is bounded by bytes that exist.
    void expectSize(u64 size) { expectedSize_ = size; }

    /// Takes the file's next bytes. Returns false, and sets `error`, once the file proves invalid;
    /// nothing more is taken after that.
    bool append(std::span<const u8> bytes, std::string& error);
    /// After the last bytes: whether the whole file arrived.
    bool finish(std::string& error);

    u64 received() const { return received_; }
    /// The file's size, from its header, or 0 until the header is in.
    u64 size() const { return fileSize_; }

private:
    struct Current;
    bool takeHeader(std::span<const u8>& bytes, std::string& error);
    bool beginSegment(std::string& error);
    bool endSegment(std::string& error);

    SegmentSink* sink_;
    Ready ready_;
    std::vector<u8> prefix_;
    std::vector<format::SegmentEntry> entries_; // in file order
    usize next_ = 0;                            // the entry to read next
    u64 received_ = 0;
    u64 fileSize_ = 0;
    u64 expectedSize_ = 0;
    bool failed_ = false;
    std::unique_ptr<Current> current_;
};

/// Maps every segment of a finished cache file. Returns false and sets `error` if the file is not
/// valid, which callers treat as a cache miss.
bool openCacheFile(int fd, std::vector<std::shared_ptr<Segment>>& segments, std::string& error);

/// Copies every segment of a finished cache file held in memory, validating it as openCacheFile
/// does.
bool readCacheFile(std::span<const u8> file, std::vector<std::shared_ptr<Segment>>& segments,
                   std::string& error);

} // namespace geo::scene
