#include "occt/mesh_shape.h"

#include "base/time.h"

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomLib.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Interface_EntityIterator.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPConstruct.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <StepData_StepModel.hxx>
#include <StepRepr_RepresentationItem.hxx>
#include <StepShape_AdvancedFace.hxx>
#include <StepShape_FaceSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <Transfer_TransientProcess.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_XYZ.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <istream>
#include <streambuf>
#include <unordered_map>

namespace geo::import::worker {
namespace {

// Tolerances as multiples of the model's diagonal. See Docs/PLAN.md.
constexpr double kLinearOfDiagonal = 2e-4;
constexpr double kMinSizeOfDiagonal = 1.5e-3;
constexpr double kDefaultAngular = 0.35;
constexpr double kNormalTolerance = 1e-7;

// Reads a block of memory as a stream, without copying it.
class MemoryStream final : public std::streambuf {
public:
    explicit MemoryStream(std::string_view text) {
        char* first = const_cast<char*>(text.data());
        setg(first, first, first + text.size());
    }

protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode) override {
        char* base = direction == std::ios_base::beg   ? eback()
                     : direction == std::ios_base::cur ? gptr()
                                                       : egptr();
        char* target = base + offset;
        if (target < eback() || target > egptr()) {
            return pos_type(off_type(-1));
        }
        setg(eback(), target, egptr());
        return pos_type(off_type(target - eback()));
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }
};

template <typename T>
void append(std::vector<u8>& payload, const T* data, usize count) {
    const usize bytes = count * sizeof(T);
    const usize offset = payload.size();
    payload.resize(offset + bytes);
    std::memcpy(payload.data() + offset, data, bytes);
}

IMeshTools_Parameters meshParameters(double linear, double angular, double minSize) {
    IMeshTools_Parameters parameters;
    parameters.Deflection = linear;
    parameters.Angle = angular;
    parameters.MinSize = minSize;
    parameters.Relative = false;
    parameters.InParallel = false;
    return parameters;
}

// Scratch buffers kept between faces, so a request allocates a few times rather than per face.
struct FaceBuffers {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<u32> indices;
};

// Fills `normals` where the surface cannot say, as at the tip of a cone, from the triangles around
// the vertex.
void estimateNormals(FaceBuffers& face, const std::vector<u8>& missing) {
    std::vector<float> sums(face.normals.size(), 0.0f);
    for (usize t = 0; t + 2 < face.indices.size(); t += 3) {
        const float* a = face.positions.data() + usize(face.indices[t]) * 3;
        const float* b = face.positions.data() + usize(face.indices[t + 1]) * 3;
        const float* c = face.positions.data() + usize(face.indices[t + 2]) * 3;
        const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        for (usize corner = 0; corner < 3; ++corner) {
            float* sum = sums.data() + usize(face.indices[t + corner]) * 3;
            sum[0] += nx;
            sum[1] += ny;
            sum[2] += nz;
        }
    }
    for (usize v = 0; v < missing.size(); ++v) {
        if (missing[v] == 0) {
            continue;
        }
        const float* sum = sums.data() + v * 3;
        const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        const float scale = length > 0.0f ? 1.0f / length : 0.0f;
        face.normals[v * 3] = sum[0] * scale;
        face.normals[v * 3 + 1] = sum[1] * scale;
        face.normals[v * 3 + 2] = sum[2] * scale;
    }
}

// Reads one face's triangulation into `face`, with the shape's location applied. Vertices carry
// true surface normals, so smooth surfaces shade smoothly at any tessellation.
bool readFace(const TopoDS_Face& source, FaceBuffers& face) {
    TopLoc_Location location;
    const occ::handle<Poly_Triangulation>& mesh = BRep_Tool::Triangulation(source, location);
    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
        return false;
    }

    const bool reversed = source.Orientation() == TopAbs_REVERSED;
    const bool moved = !location.IsIdentity();
    const gp_Trsf transform = location.Transformation();
    const int nodeCount = mesh->NbNodes();
    const int triangleCount = mesh->NbTriangles();

    face.positions.resize(usize(nodeCount) * 3);
    face.normals.resize(usize(nodeCount) * 3);
    face.indices.resize(usize(triangleCount) * 3);
    for (int n = 0; n < nodeCount; ++n) {
        gp_Pnt point = mesh->Node(n + 1);
        if (moved) {
            point.Transform(transform);
        }
        float* out = face.positions.data() + usize(n) * 3;
        out[0] = float(point.X());
        out[1] = float(point.Y());
        out[2] = float(point.Z());
    }
    for (int t = 0; t < triangleCount; ++t) {
        int a = 0, b = 0, c = 0;
        mesh->Triangle(t + 1).Get(a, b, c);
        if (reversed) {
            std::swap(b, c);
        }
        u32* out = face.indices.data() + usize(t) * 3;
        out[0] = u32(a - 1);
        out[1] = u32(b - 1);
        out[2] = u32(c - 1);
    }

    TopLoc_Location surfaceLocation;
    const occ::handle<Geom_Surface> surface = BRep_Tool::Surface(source, surfaceLocation);
    const gp_Trsf surfaceTransform = surfaceLocation.Transformation();
    const float sign = reversed ? -1.0f : 1.0f;
    std::vector<u8> missing;

    const auto store = [&](int n, gp_Dir direction) {
        direction.Transform(surfaceTransform);
        float* out = face.normals.data() + usize(n) * 3;
        out[0] = sign * float(direction.X());
        out[1] = sign * float(direction.Y());
        out[2] = sign * float(direction.Z());
    };

    const occ::handle<Geom_Plane> plane = occ::down_cast<Geom_Plane>(surface);
    if (!plane.IsNull()) {
        gp_Dir direction = plane->Pln().Axis().Direction();
        if (!plane->Pln().Direct()) {
            direction.Reverse();
        }
        for (int n = 0; n < nodeCount; ++n) {
            store(n, direction);
        }
    } else if (mesh->HasUVNodes() && !surface.IsNull()) {
        for (int n = 0; n < nodeCount; ++n) {
            gp_Dir direction;
            if (GeomLib::NormEstim(surface, mesh->UVNode(n + 1), kNormalTolerance, direction) < 2) {
                store(n, direction);
            } else {
                missing.resize(usize(nodeCount), 0);
                missing[usize(n)] = 1;
            }
        }
    } else {
        missing.assign(usize(nodeCount), 1);
    }
    if (!missing.empty()) {
        estimateNormals(face, missing);
    }
    return true;
}

void appendFace(const TopoDS_Face& source, u32 sourceId, u32 group, FaceBuffers& buffers,
                Reply& reply, std::vector<u8>& payload) {
    if (!readFace(source, buffers)) {
        ++reply.unmeshedFaces;
        buffers.positions.clear();
        buffers.normals.clear();
        buffers.indices.clear();
    }
    const FaceHeader header{sourceId, group, u32(buffers.positions.size() / 3),
                            u32(buffers.indices.size() / 3)};
    append(payload, &header, 1);
    append(payload, buffers.positions.data(), buffers.positions.size());
    append(payload, buffers.normals.data(), buffers.normals.size());
    append(payload, buffers.indices.data(), buffers.indices.size());
    ++reply.faceCount;
}

void meshShape(const TopoDS_Shape& shape, const Request& request, Reply& reply) {
    double linear = request.linearDeflection;
    double minSize = request.minSize;
    if (linear <= 0.0) {
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        const double diagonal = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
        linear = diagonal * kLinearOfDiagonal;
        minSize = diagonal * kMinSizeOfDiagonal;
    }
    const double start = nowSeconds();
    const double angular =
        request.angularDeflection > 0.0 ? request.angularDeflection : kDefaultAngular;
    BRepMesh_IncrementalMesh mesher(shape, meshParameters(linear, angular, minSize));
    reply.meshSeconds = nowSeconds() - start;
}

// Which STEP face each result face came from. The transfer may have split a face or replaced it, so
// every face of the shape it found maps back to the entity.
using FaceIds = std::unordered_map<const void*, u32>;

FaceIds mapFaces(STEPControl_Reader& reader) {
    FaceIds ids;
    const occ::handle<StepData_StepModel> model = reader.StepModel();
    const occ::handle<Transfer_TransientProcess> process =
        reader.WS()->TransferReader()->TransientProcess();
    const int entities = model->NbEntities();
    for (int e = 1; e <= entities; ++e) {
        const occ::handle<Standard_Transient> entity = model->Value(e);
        if (!entity->IsKind(STANDARD_TYPE(StepShape_AdvancedFace)) &&
            !entity->IsKind(STANDARD_TYPE(StepShape_FaceSurface))) {
            continue;
        }
        const TopoDS_Shape shape =
            STEPConstruct::FindShape(process, occ::down_cast<StepRepr_RepresentationItem>(entity));
        if (shape.IsNull()) {
            continue;
        }
        const u32 id = u32(model->IdentLabel(entity));
        for (TopExp_Explorer face(shape, TopAbs_FACE); face.More(); face.Next()) {
            ids.emplace(face.Current().TShape().get(), id);
        }
    }
    return ids;
}

bool readText(STEPControl_Reader& reader, std::string_view text, bool productMode) {
    MemoryStream buffer(text);
    std::istream stream(&buffer);
    DESTEP_Parameters parameters;
    parameters.ReadProductMode = productMode;
    return reader.ReadStream("slice", parameters, stream) == IFSelect_RetDone;
}

} // namespace

void initializeOcct() {
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_Printer));
}

void meshSlice(std::string_view text, const Request& request, Reply& reply,
               std::vector<u8>& payload) {
    reply.status = u32(ReplyStatus::Failed);
    try {
        double lap = nowSeconds();
        STEPControl_Reader reader;
        if (!readText(reader, text, false)) {
            return;
        }
        reply.readSeconds = nowSeconds() - lap;
        lap = nowSeconds();
        reader.TransferRoots();
        const TopoDS_Shape shape = reader.OneShape();
        reply.transferSeconds = nowSeconds() - lap;
        if (shape.IsNull()) {
            return;
        }
        meshShape(shape, request, reply);

        const FaceIds ids = mapFaces(reader);
        FaceBuffers buffers;
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            const TopoDS_Face& face = TopoDS::Face(explorer.Current());
            const auto it = ids.find(face.TShape().get());
            if (it == ids.end()) {
                ++reply.unmappedFaces;
            }
            appendFace(face, it != ids.end() ? it->second : 0, 0, buffers, reply, payload);
        }
        reply.status = u32(ReplyStatus::Ok);
    } catch (const Standard_Failure&) {
        payload.clear();
        reply.faceCount = 0;
    }
}

void meshWholeFile(std::string_view text, const Request& request, Reply& reply,
                   std::vector<u8>& payload) {
    reply.status = u32(ReplyStatus::Failed);
    try {
        double lap = nowSeconds();
        STEPControl_Reader reader;
        if (!readText(reader, text, true)) {
            return;
        }
        reply.readSeconds = nowSeconds() - lap;
        lap = nowSeconds();
        reader.TransferRoots();
        const TopoDS_Shape shape = reader.OneShape();
        reply.transferSeconds = nowSeconds() - lap;
        if (shape.IsNull()) {
            return;
        }
        meshShape(shape, request, reply);

        FaceBuffers buffers;
        u32 group = 0;
        for (TopExp_Explorer solid(shape, TopAbs_SOLID); solid.More(); solid.Next(), ++group) {
            for (TopExp_Explorer face(solid.Current(), TopAbs_FACE); face.More(); face.Next()) {
                appendFace(TopoDS::Face(face.Current()), 0, group, buffers, reply, payload);
            }
        }
        // Faces outside any solid form one more body.
        bool hasLooseFaces = false;
        for (TopExp_Explorer face(shape, TopAbs_FACE, TopAbs_SOLID); face.More(); face.Next()) {
            appendFace(TopoDS::Face(face.Current()), 0, group, buffers, reply, payload);
            hasLooseFaces = true;
        }
        reply.groupCount = group + (hasLooseFaces ? 1 : 0);
        reply.status = u32(ReplyStatus::Ok);
    } catch (const Standard_Failure&) {
        payload.clear();
        reply.faceCount = 0;
    }
}

Truth measureFile(const char* path, double linear, double angular, double minSize) {
    Truth truth;
    try {
        STEPControl_Reader reader;
        if (reader.ReadFile(path) != IFSelect_RetDone) {
            return truth;
        }
        reader.TransferRoots();
        const TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull()) {
            return truth;
        }
        Reply reply{};
        Request request{};
        request.linearDeflection = linear;
        request.angularDeflection = angular;
        request.minSize = minSize;
        meshShape(shape, request, reply);

        double lo[3] = {INFINITY, INFINITY, INFINITY};
        double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            ++truth.faces;
            const TopoDS_Face& face = TopoDS::Face(explorer.Current());
            TopLoc_Location location;
            const occ::handle<Poly_Triangulation>& mesh = BRep_Tool::Triangulation(face, location);
            if (mesh.IsNull() || mesh->NbTriangles() == 0) {
                ++truth.unmeshedFaces;
                continue;
            }
            const gp_Trsf transform = location.Transformation();
            const auto node = [&](int n) { return mesh->Node(n).Transformed(transform); };
            for (int n = 1; n <= mesh->NbNodes(); ++n) {
                const gp_Pnt p = node(n);
                const double xyz[3] = {p.X(), p.Y(), p.Z()};
                for (int axis = 0; axis < 3; ++axis) {
                    lo[axis] = std::min(lo[axis], xyz[axis]);
                    hi[axis] = std::max(hi[axis], xyz[axis]);
                }
            }
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            for (int t = 1; t <= mesh->NbTriangles(); ++t) {
                int ia = 0, ib = 0, ic = 0;
                mesh->Triangle(t).Get(ia, ib, ic);
                if (reversed) {
                    std::swap(ib, ic);
                }
                const gp_Pnt a = node(ia), b = node(ib), c = node(ic);
                const gp_XYZ normal = (b.XYZ() - a.XYZ()).Crossed(c.XYZ() - a.XYZ());
                truth.area += 0.5 * normal.Modulus();
                truth.signedVolume += a.XYZ().Dot(normal) / 6.0;
                ++truth.triangles;
            }
        }
        for (usize axis = 0; axis < 3; ++axis) {
            truth.min[axis] = lo[axis];
            truth.max[axis] = hi[axis];
        }
        truth.ok = true;
    } catch (const Standard_Failure&) {
        truth.ok = false;
    }
    return truth;
}

} // namespace geo::import::worker
