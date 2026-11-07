#include "scene/scene.h"

#include <cmath>
#include <format>
#include <simd/simd.h>

namespace geo::scene {

using format::DefinitionRecord;

bool Document::attachModel(const std::shared_ptr<MappedSegment>& segment, std::string& error) {
    if (model_ != nullptr) {
        error = "A cache file has two models.";
        return false;
    }
    const format::SegmentView& view = segment->view();
    definitions_.assign(view.definitionInfos.size(), {});
    idBases_.resize(view.occurrences.size());
    u64 ids = 0;
    for (usize i = 0; i < view.occurrences.size(); ++i) {
        idBases_[i] = u32(ids);
        ids += view.definitionInfos[view.occurrences[i].definition].faceCount;
    }
    if (ids >= u64(1) << 31) {
        error = "This model has too many faces to pick.";
        return false;
    }
    idCount_ = u32(ids);
    model_ = segment;
    return true;
}

bool Document::attachGeometry(const MappedSegment& segment, std::string& error) {
    for (const DefinitionRecord& record : segment.view().definitions) {
        if (record.definition >= definitions_.size()) {
            error = "Geometry arrived for a definition that does not exist.";
            return false;
        }
        if (record.faceCount != model().definitionInfos[record.definition].faceCount) {
            error = "Geometry disagrees with the model about a face count.";
            return false;
        }
        DefinitionState& state = definitions_[record.definition];
        if (state.record == nullptr) {
            ++readyCount_;
        } else if (record.level <= state.record->level) {
            continue; // a coarser or equal version never replaces a finer one
        }
        state.record = &record;
        state.segment = &segment;
    }
    return true;
}

bool Document::add(std::shared_ptr<MappedSegment> segment, std::string& error) {
    if (segment->view().hasModel() && !attachModel(segment, error)) {
        return false;
    }
    segments_.push_back(std::move(segment));
    if (model_ == nullptr) {
        return true; // geometry that arrived first is attached with the model below
    }
    if (segments_.back() != model_) {
        return attachGeometry(*segments_.back(), error);
    }
    for (const std::shared_ptr<MappedSegment>& earlier : segments_) {
        if (!attachGeometry(*earlier, error)) {
            return false;
        }
    }
    return true;
}

u32 Scene::occurrenceBase(u32 document) const {
    u32 base = 0;
    for (u32 d = 0; d < document; ++d) {
        if (documents_[d]->hasModel()) {
            base += u32(documents_[d]->model().occurrences.size());
        }
    }
    return base;
}

u32 Scene::occurrenceCount() const {
    return occurrenceBase(documentCount());
}

std::vector<u32> Scene::faceCounts() const {
    std::vector<u32> counts;
    for (const std::unique_ptr<Document>& document : documents_) {
        if (!document->hasModel()) {
            continue;
        }
        const format::SegmentView& model = document->model();
        for (const format::OccurrenceRecord& occurrence : model.occurrences) {
            counts.push_back(model.definitionInfos[occurrence.definition].faceCount);
        }
    }
    return counts;
}

std::pair<u32, u32> Scene::idRange(u32 document, u32 occurrence) const {
    u32 base = 1;
    for (u32 d = 0; d < document; ++d) {
        base += documents_[d]->idCount();
    }
    const Document& doc = *documents_[document];
    const u32 first = base + doc.idBase(occurrence);
    const u32 faces =
        doc.model().definitionInfos[doc.model().occurrences[occurrence].definition].faceCount;
    return {first, first + faces};
}

std::optional<PickTarget> Scene::resolve(u32 id) const {
    if (id == 0) {
        return std::nullopt;
    }
    u32 index = id - 1;
    u32 globalBase = 0;
    for (u32 d = 0; d < documents_.size(); ++d) {
        const Document& document = *documents_[d];
        if (!document.hasModel()) {
            continue;
        }
        const u32 occurrences = u32(document.model().occurrences.size());
        if (index >= document.idCount()) {
            index -= document.idCount();
            globalBase += occurrences;
            continue;
        }
        // The last occurrence whose first id is at or before this one.
        u32 low = 0, high = occurrences;
        while (high - low > 1) {
            const u32 middle = (low + high) / 2;
            if (document.idBase(middle) <= index) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const u32 face = index - document.idBase(low);
        const format::SegmentView& model = document.model();
        if (face >= model.definitionInfos[model.occurrences[low].definition].faceCount) {
            return std::nullopt;
        }
        return PickTarget{d, low, globalBase + low, face};
    }
    return std::nullopt;
}

u32 Scene::addDocument() {
    documents_.push_back(std::make_unique<Document>());
    return u32(documents_.size()) - 1;
}

bool Scene::hasGeometry() const {
    for (const std::unique_ptr<Document>& document : documents_) {
        if (document->hasModel() && !document->model().occurrences.empty()) {
            return true;
        }
    }
    return false;
}

std::optional<Aabb> Scene::bounds() const {
    Aabb bounds{simd_make_double3(INFINITY, INFINITY, INFINITY),
                simd_make_double3(-INFINITY, -INFINITY, -INFINITY)};
    bool any = false;
    for (const std::unique_ptr<Document>& document : documents_) {
        if (!document->hasModel()) {
            continue;
        }
        const format::SegmentView& model = document->model();
        for (const format::OccurrenceRecord& occurrence : model.occurrences) {
            const format::DefinitionInfo& info = model.definitionInfos[occurrence.definition];
            const DefinitionRecord* geometry = document->geometry(occurrence.definition);
            const auto& lo = geometry != nullptr ? geometry->boundsMin : info.boundsMin;
            const auto& hi = geometry != nullptr ? geometry->boundsMax : info.boundsMax;
            if (lo[0] > hi[0] || !std::isfinite(lo[0]) || !std::isfinite(hi[0])) {
                continue;
            }
            const std::array<float, 12>& t = occurrence.transform;
            for (int corner = 0; corner < 8; ++corner) {
                const float x = corner & 1 ? hi[0] : lo[0];
                const float y = corner & 2 ? hi[1] : lo[1];
                const float z = corner & 4 ? hi[2] : lo[2];
                const Vec3 world = simd_make_double3(t[0] * x + t[3] * y + t[6] * z + t[9],
                                                     t[1] * x + t[4] * y + t[7] * z + t[10],
                                                     t[2] * x + t[5] * y + t[8] * z + t[11]);
                bounds.min = simd_min(bounds.min, world);
                bounds.max = simd_max(bounds.max, world);
                any = true;
            }
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return bounds;
}

} // namespace geo::scene
