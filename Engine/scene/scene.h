// What is open: one document per file, each a model plus whatever geometry has arrived so far.
// Geometry can arrive in any order, and pick ids are fixed by the model, so a document can be shown
// and used while it is still importing.
#pragma once

#include "base/types.h"
#include "math/geometry.h"
#include "scene/mapped_segment.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace geo::scene {

class Document {
public:
    /// Adds a segment. Returns false and sets `error` if it contradicts the model.
    bool add(std::shared_ptr<MappedSegment> segment, std::string& error);

    bool hasModel() const { return model_ != nullptr; }
    const format::SegmentView& model() const { return model_->view(); }
    const std::vector<std::shared_ptr<MappedSegment>>& segments() const { return segments_; }

    /// The geometry to draw for a definition, or null if none has arrived.
    const format::DefinitionRecord* geometry(u32 definition) const {
        return definitions_[definition].record;
    }
    const MappedSegment* geometrySegment(u32 definition) const {
        return definitions_[definition].segment;
    }
    /// True once every definition has geometry.
    bool isComplete() const { return readyCount_ == definitions_.size(); }

    /// The first pick id of an occurrence, counted from zero.
    u32 idBase(u32 occurrence) const { return idBases_[occurrence]; }
    u32 idCount() const { return idCount_; }

private:
    struct DefinitionState {
        const format::DefinitionRecord* record = nullptr;
        const MappedSegment* segment = nullptr;
    };

    bool attachModel(const std::shared_ptr<MappedSegment>& segment, std::string& error);
    bool attachGeometry(const MappedSegment& segment, std::string& error);

    std::vector<std::shared_ptr<MappedSegment>> segments_;
    std::shared_ptr<MappedSegment> model_;
    std::vector<DefinitionState> definitions_;
    std::vector<u32> idBases_;
    u32 idCount_ = 0;
    usize readyCount_ = 0;
};

/// Everything shown together. A definition or occurrence's position across all documents is its
/// global index, which is what the GPU tables use.
class Scene {
public:
    Scene() = default;

    /// Starts a new empty document, returning its index.
    u32 addDocument();
    Document& document(u32 index) { return *documents_[index]; }
    const Document& document(u32 index) const { return *documents_[index]; }
    u32 documentCount() const { return u32(documents_.size()); }
    void clear() { documents_.clear(); }

    bool hasGeometry() const;
    /// Bounds of everything placed, using estimates for definitions still importing.
    std::optional<Aabb> bounds() const;

private:
    std::vector<std::unique_ptr<Document>> documents_;
};

} // namespace geo::scene
