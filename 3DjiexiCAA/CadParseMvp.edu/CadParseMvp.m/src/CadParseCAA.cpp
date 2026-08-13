// 本文件是解析器与 CATIA V5R21 PublicInterfaces 的边界。
// 它负责引用计数、Session/Document 生命周期、类型指纹采集和确定性规格树遍历。
#include "CadParseCAA.h"

#include "CATBaseUnknown.h"
#include "CATDocument.h"
#include "CATDocumentServices.h"
#include "CATIContainer.h"
#include "CATIDocRoots.h"
#include "CATInit.h"
#include "CATIPrtContainer.h"
#include "CATIPrtPart.h"
#include "CATIProduct.h"
#include "CATIMovable.h"
#include "CATISpecObject.h"
#include "CATIShapeFeatureBody.h"
#include "CATITPSComponent.h"
#include "CATITPS.h"
#include "CATITPSDocument.h"
#include "CATITPSGeometryList.h"
#include "CATITPSList.h"
#include "CATITPSSemanticValidity.h"
#include "CATITPSSet.h"
#include "CATITPSText.h"
#include "CATITPSTextContent.h"
#include "CATIGeometricalElement.h"
#include "CATIMechanicalFeature.h"
#include "CATIShapeFeatureProperties.h"
#include "CATISketch.h"
#include "CATLISTV_CATBaseUnknown.h"
#include "CATLISTV_CATISpecObject.h"
#include "CATICkeInst.h"
#include "CATICkeParm.h"
#include "CATICkeType.h"
#include "CATSession.h"
#include "CATSessionServices.h"
#include "CATUnicodeString.h"
#include "CATIAHole.h"
#include "CATIAPad.h"
#include "CATIAPocket.h"
#include "CATIAPrism.h"
#include "CATIAChamfer.h"
#include "CATIADraft.h"
#include "CATIADraftDomain.h"
#include "CATIADraftDomains.h"
#include "CATIAConstRadEdgeFillet.h"
#include "CATIAVarRadEdgeFillet.h"
#include "CATIAFaceFillet.h"
#include "CATIATritangentFillet.h"
#include "CATIAEdgeFillet.h"
#include "CATIAFillet.h"
#include "CATIAShell.h"
#include "CATIAThickness.h"
#include "CATIARevolution.h"
#include "CATIASweep.h"
#include "CATISweep.h"
#include "CATIAPattern.h"
#include "CATIARectPattern.h"
#include "CATIACircPattern.h"
#include "CATIAUserPattern.h"
#include "CATIABooleanShape.h"
#include "CATIABody.h"
#include "CATIAReferences.h"
#include "CATIAReference.h"
#include "CATIAIntParam.h"
#include "CATIALinearRepartition.h"
#include "CATIAAngularRepartition.h"
#include "CATIARepartition.h"
#include "CATIALimit.h"
#include "CATIALength.h"
#include "CATIAAngle.h"
#include "CATIAStrParam.h"
#include "CATBody.h"
#include "CATCell.h"
#include "CATBoundaryIterator.h"
#include "CATBoundedCellsIterator.h"
#include "CATFace.h"
#include "CATEdge.h"
#include "CATVertex.h"
#include "CATDomain.h"
#include "CATICGMBodyTessellator.h"
#include "CATCGMTessPointIter.h"
#include "CATCGMTessStripeIter.h"
#include "CATCGMTessFanIter.h"
#include "CATCGMTessPolyIter.h"
#include "CATCGMTessTrianIter.h"
#include "CATGeometry.h"
#include "CATSurface.h"
#include "CATCurve.h"
#include "CATSurLimits.h"
#include "CATCrvLimits.h"
#include "CATSurParam.h"
#include "CATCrvParam.h"
#include "CATPlane.h"
#include "CATElementarySurface.h"
#include "CATCylinder.h"
#include "CATCone.h"
#include "CATSphere.h"
#include "CATTorus.h"
#include "CATNurbsSurface.h"
#include "CATKnotVector.h"
#include "CATLine.h"
#include "CATConic.h"
#include "CATCircle.h"
#include "CATNurbsCurve.h"
#include "CATMathPoint.h"
#include "CATMathVector.h"
#include "CATMathDirection.h"
#include "CATMathBox.h"
#include "CATMathPlane.h"
#include "CATMathTransformation.h"
#include "CATHoleDefs.h"
#include "CATLimitDefs.h"
#include "ListPOfCATCell.h"
#include "CATPrismDefs.h"
#include "CATSafeArray.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace cadparse
{
// 通用 CAA 接口指针 RAII 守卫。
// 模板参数 T 保留具体接口类型；守卫接管一个已持有引用，并在析构时调用一次 Release。
template <class T>
class CaaInterfaceGuard
{
public:
  // 用途：接管 pointer 当前代表的 CAA 引用；允许传入空指针。
  explicit CaaInterfaceGuard(T* pointer = 0) : _pointer(pointer) {}
  // 用途：释放构造时接管的引用；不释放空指针。
  ~CaaInterfaceGuard() { if (_pointer) _pointer->Release(); }
  // 用途：返回借用指针供当前作用域调用；调用者不能额外 Release。
  T* Get() const { return _pointer; }
  // 用途：把受保护的输出槽交给 QueryInterface/getter；即使调用抛异常也能释放已写入引用。
  T*& Out() { return _pointer; }

private:
  // 用途：禁止复制守卫，避免两个析构函数对同一引用重复 Release。
  CaaInterfaceGuard(const CaaInterfaceGuard&);
  // 用途：禁止赋值，保持引用清理责任唯一。
  CaaInterfaceGuard& operator=(const CaaInterfaceGuard&);
  T* _pointer;
};

// CATBSTR 专用 RAII；R21 CATBSTR.h 要求由 CATFreeString 而不是 SysFreeString 释放。
class CaaBstrGuard
{
public:
  // 用途：创建空字符串输出槽，供 Automation getter 写入。
  CaaBstrGuard() : _value(0) {}
  // 用途：按照 CATBSTR Public 契约释放已返回字符串。
  ~CaaBstrGuard() { if (_value) CATFreeString(_value); }
  // 用途：返回 getter 所需的引用输出槽。
  CATBSTR& Out() { return _value; }
  // 用途：借用已返回字符串进行 UTF-8 转换。
  CATBSTR Get() const { return _value; }
private:
  CaaBstrGuard(const CaaBstrGuard&);
  CaaBstrGuard& operator=(const CaaBstrGuard&);
  CATBSTR _value;
};

// ListComponents 返回的堆分配列表专用守卫；该列表按 R21 API 契约使用 delete 销毁。
class SpecListGuard
{
public:
  // 用途：接管 CATISpecObject 列表对象的所有权。
  explicit SpecListGuard(CATListValCATISpecObject_var* list) : _list(list) {}
  // 用途：释放整个列表包装对象；列表内的 _var 元素自行管理各自引用。
  ~SpecListGuard() { delete _list; }

private:
  // 用途：禁止复制列表所有者，避免重复 delete。
  SpecListGuard(const SpecListGuard&);
  // 用途：禁止列表守卫赋值，保持唯一所有权。
  SpecListGuard& operator=(const SpecListGuard&);
  CATListValCATISpecObject_var* _list;
};

// CATIContainer::ListMembersHere 输出序列的引用清理守卫。
// 序列本身由调用栈保存，但其中每个 CATBaseUnknown_ptr 都需要显式 Release。
class BaseUnknownSequenceGuard
{
public:
  // 用途：借用序列对象，并承担其所有非空成员引用的清理责任。
  explicit BaseUnknownSequenceGuard(SEQUENCE(CATBaseUnknown_ptr)& sequence)
    : _sequence(sequence) {}
  // 用途：遍历序列、逐个 Release，并置零以避免悬空指针被再次使用。
  ~BaseUnknownSequenceGuard()
  {
    CATLONG32 index = 0;
    for (index = 0; index < _sequence.length(); ++index)
    {
      if (_sequence[index])
      {
        _sequence[index]->Release();
        _sequence[index] = 0;
      }
    }
  }

private:
  // 用途：禁止复制清理守卫，避免同一序列成员被释放两次。
  BaseUnknownSequenceGuard(const BaseUnknownSequenceGuard&);
  // 用途：禁止赋值；引用成员本身也不适合重新绑定。
  BaseUnknownSequenceGuard& operator=(const BaseUnknownSequenceGuard&);
  SEQUENCE(CATBaseUnknown_ptr)& _sequence;
};

// 用途：通过 R21 ConvertToUTF8 API 把 CATUnicodeString 复制为独立 std::string。
// 每个 Unicode 字符最多预留四个 UTF-8 字节，并额外保留终止零字节空间。
std::string UnicodeToUtf8(const CATUnicodeString& value)
{
  const size_t capacity = static_cast<size_t>(value.GetLengthInChar() + 1) * 4 + 1;
  std::vector<char> buffer(capacity, 0);
  size_t byte_count = 0;
  value.ConvertToUTF8(&buffer[0], &byte_count);
  if (byte_count >= buffer.size())
    byte_count = buffer.size() - 1;
  buffer[byte_count] = 0;
  return std::string(&buffer[0], byte_count);
}

// 用途：把 CGM 拓扑维度转换成面向 IR 的稳定分类文本。
// 未知维度保留为 unknown，不猜测为某种拓扑单元。
static const char* TopologyCellKind(short dimension)
{
  if (dimension == 0) return "vertex";
  if (dimension == 1) return "edge";
  if (dimension == 2) return "face";
  if (dimension == 3) return "volume";
  return "unknown";
}

// 用途：按统一规则生成拓扑单元编号；编号只依赖 body_id、维度前缀和原生枚举顺序。
static std::string MakeTopologyCellId(const std::string& body_id, const char* prefix, long index)
{
  std::ostringstream id;
  id << body_id << "_" << prefix;
  if (index < 10) id << "00000";
  else if (index < 100) id << "0000";
  else if (index < 1000) id << "000";
  else if (index < 10000) id << "00";
  else if (index < 100000) id << "0";
  id << index;
  return id.str();
}

// 用途：在当前运行内把 CATCell 指针转换成已经分配好的稳定 IR 编号。
// 指针只用于内存索引，绝不写入 JSON。
static std::string LookupCellId(CATCell* cell, const std::map<CATCell*, std::string>& ids)
{
  if (!cell) return "";
  std::map<CATCell*, std::string>::const_iterator it = ids.find(cell);
  return it == ids.end() ? "" : it->second;
}

// 用途：删除 CGM 边界迭代器并把指针清空，匹配 CATBoundaryIterator 头文件契约。
class BoundaryIteratorGuard
{
public:
  explicit BoundaryIteratorGuard(CATBoundaryIterator* iterator) : _iterator(iterator) {}
  ~BoundaryIteratorGuard() { if (_iterator) CATRemove(_iterator); }
  CATBoundaryIterator* Get() const { return _iterator; }

private:
  BoundaryIteratorGuard(const BoundaryIteratorGuard&);
  BoundaryIteratorGuard& operator=(const BoundaryIteratorGuard&);
  CATBoundaryIterator* _iterator;
};

class BoundedCellsIteratorGuard
{
public:
  explicit BoundedCellsIteratorGuard(CATBoundedCellsIterator* iterator) : _iterator(iterator) {}
  ~BoundedCellsIteratorGuard() { if (_iterator) CATRemove(_iterator); }
  CATBoundedCellsIterator* Get() const { return _iterator; }

private:
  BoundedCellsIteratorGuard(const BoundedCellsIteratorGuard&);
  BoundedCellsIteratorGuard& operator=(const BoundedCellsIteratorGuard&);
  CATBoundedCellsIterator* _iterator;
};

// 用途：释放 CGM Tessellator；它继承 IUnknown，构造函数注释要求调用 Release。
class CgmTessellatorGuard
{
public:
  explicit CgmTessellatorGuard(CATICGMBodyTessellator* tessellator)
    : _tessellator(tessellator) {}
  ~CgmTessellatorGuard() { if (_tessellator) _tessellator->Release(); }
  CATICGMBodyTessellator* Get() const { return _tessellator; }

private:
  CgmTessellatorGuard(const CgmTessellatorGuard&);
  CgmTessellatorGuard& operator=(const CgmTessellatorGuard&);
  CATICGMBodyTessellator* _tessellator;
};

// 用途：把一个拓扑单元的直接边界单元编号收集到记录中。
static void FillBoundaryCellIds(CATCell* cell, const std::map<CATCell*, std::string>& ids,
                                NativeTopologyCellRecord& record)
{
  if (!cell) return;
  CATBoundaryIterator* raw_iterator = 0;
  try { raw_iterator = cell->CreateBoundaryIterator(); }
  catch (...) { raw_iterator = 0; }
  BoundaryIteratorGuard iterator_guard(raw_iterator);
  CATBoundaryIterator* iterator = iterator_guard.Get();
  if (!iterator) return;
  try
  {
    CATSide side = CATSideUnknown;
    CATDomain* domain = 0;
    short new_domain = 0;
    CATCell* boundary = 0;
    while ((boundary = iterator->Next(&side, &domain, &new_domain)) != 0)
    {
      const std::string id = LookupCellId(boundary, ids);
      if (!id.empty() &&
          std::find(record.boundary_cell_ids.begin(),
                    record.boundary_cell_ids.end(), id) == record.boundary_cell_ids.end())
        record.boundary_cell_ids.push_back(id);
    }
  }
  catch (...)
  {
    record.geometry_status = "boundary_partial";
  }
}

// 用途：把当前 Body 内与 cell 相邻的单元编号收集到记录中；异常只影响该字段。
static void FillAdjacentCellIds(CATBody* body, CATCell* cell,
                                const std::map<CATCell*, std::string>& ids,
                                NativeTopologyCellRecord& record)
{
  if (!body || !cell) return;
  try
  {
    ListPOfCATCell neighbours;
    if (SUCCEEDED(cell->CellNeighbours(body, neighbours)))
    {
      int index = 1;
      for (index = 1; index <= neighbours.Size(); ++index)
      {
        const std::string id = LookupCellId(neighbours[index], ids);
        if (!id.empty() &&
            std::find(record.adjacent_cell_ids.begin(),
                      record.adjacent_cell_ids.end(), id) == record.adjacent_cell_ids.end())
          record.adjacent_cell_ids.push_back(id);
      }
    }
  }
  catch (...) {}
}

static std::string CatSideName(CATSide side);

static void FillMaterialSide(CATBody* body, CATCell* cell,
                             NativeTopologyCellRecord& record)
{
  if (!body || !cell || record.dimension != 2) return;
  CATBoundedCellsIterator* raw_iterator = 0;
  try { raw_iterator = cell->CreateBoundedCellsIterator(body); }
  catch (...) { raw_iterator = 0; }
  BoundedCellsIteratorGuard iterator_guard(raw_iterator);
  CATBoundedCellsIterator* iterator = iterator_guard.Get();
  if (!iterator) return;
  try
  {
    CATSide side = CATSideUnknown;
    CATDomain* domain = 0;
    CATCell* bounded = iterator->Next(&side, &domain);
    if (bounded)
      record.material_side = CatSideName(side);
  }
  catch (...) {}
}

// 用途：把 strip/fan/polygon 这类多点图元转换成等价三角数量。
static long EstimateTrianglesFromPointGroups(CATLONG32 group_count, CATLONG32 point_count)
{
  if (group_count <= 0 || point_count <= 0) return 0;
  return point_count > 2 * group_count ?
    static_cast<long>(point_count - 2 * group_count) : 0;
}

struct TessPointData
{
  TessPointData()
  {
    xyz[0] = xyz[1] = xyz[2] = 0.0;
  }
  double xyz[3];
};

static void BuildTessPointMap(CATCGMTessPointIter* points, std::map<int, TessPointData>& out_points)
{
  if (!points) return;
  try
  {
    points->Reset();
    while (!points->IsExhausted())
    {
      TessPointData data;
      const double* xyz = points->GetPointXyz();
      if (xyz)
      {
        data.xyz[0] = xyz[0];
        data.xyz[1] = xyz[1];
        data.xyz[2] = xyz[2];
      }
      out_points[static_cast<int>(points->GetPointNu())] = data;
      points->GoToNext();
    }
  }
  catch (...) {}
}

static bool LookupTessPoint(const std::map<int, TessPointData>& points, int rank,
                            double* out_xyz)
{
  std::map<int, TessPointData>::const_iterator found = points.find(rank);
  if (found == points.end()) return false;
  out_xyz[0] = found->second.xyz[0];
  out_xyz[1] = found->second.xyz[1];
  out_xyz[2] = found->second.xyz[2];
  return true;
}

static void ComputeTriangleNormal(const double* vertices, NativeMeshTriangleRecord& record)
{
  const double ux = vertices[3] - vertices[0];
  const double uy = vertices[4] - vertices[1];
  const double uz = vertices[5] - vertices[2];
  const double vx = vertices[6] - vertices[0];
  const double vy = vertices[7] - vertices[1];
  const double vz = vertices[8] - vertices[2];
  double nx = uy * vz - uz * vy;
  double ny = uz * vx - ux * vz;
  double nz = ux * vy - uy * vx;
  const double norm = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (norm <= 1.0e-12) return;
  record.normal[0] = nx / norm;
  record.normal[1] = ny / norm;
  record.normal[2] = nz / norm;
  record.normal_available = true;
}

static bool AppendMeshTriangle(ParseContext& context,
                               const NativeMeshFaceMapRecord& map_record,
                               const std::map<int, TessPointData>& points,
                               int a, int b, int c,
                               const char* source_primitive,
                               long& next_triangle,
                               long& triangle_in_face)
{
  double p0[3] = { 0.0, 0.0, 0.0 };
  double p1[3] = { 0.0, 0.0, 0.0 };
  double p2[3] = { 0.0, 0.0, 0.0 };
  if (!LookupTessPoint(points, a, p0) ||
      !LookupTessPoint(points, b, p1) ||
      !LookupTessPoint(points, c, p2))
    return false;

  NativeMeshTriangleRecord triangle;
  ++next_triangle;
  ++triangle_in_face;
  triangle.triangle_id = MakeTopologyCellId(map_record.body_id, "T", next_triangle);
  triangle.mesh_map_id = map_record.mesh_map_id;
  triangle.body_id = map_record.body_id;
  triangle.face_cell_id = map_record.face_cell_id;
  triangle.triangle_index = next_triangle;
  triangle.triangle_index_in_face = triangle_in_face;
  triangle.vertex_ranks[0] = a;
  triangle.vertex_ranks[1] = b;
  triangle.vertex_ranks[2] = c;
  int i = 0;
  for (; i < 3; ++i)
  {
    triangle.vertices_mm[i] = p0[i];
    triangle.vertices_mm[3 + i] = p1[i];
    triangle.vertices_mm[6 + i] = p2[i];
  }
  ComputeTriangleNormal(triangle.vertices_mm, triangle);
  triangle.source_primitive = source_primitive ? source_primitive : "unknown";
  triangle.value_source = "typed_caa_public_body_tessellator_triangle_payload";
  context.mesh_triangles.push_back(triangle);
  return true;
}

static void WriteJsonPoint(std::ostream& out, const CATMathPoint& point)
{
  double xyz[3] = { 0.0, 0.0, 0.0 };
  point.GetCoord(xyz);
  out << '[' << std::setprecision(15) << xyz[0] << ',' << xyz[1] << ',' << xyz[2] << ']';
}

static void WriteJsonVector(std::ostream& out, const CATMathVector& vector)
{
  double xyz[3] = { 0.0, 0.0, 0.0 };
  vector.GetCoord(xyz);
  out << '[' << std::setprecision(15) << xyz[0] << ',' << xyz[1] << ',' << xyz[2] << ']';
}

static void WriteJsonDirection(std::ostream& out, const CATMathDirection& direction)
{
  double xyz[3] = { 0.0, 0.0, 0.0 };
  direction.GetCoord(xyz);
  out << '[' << std::setprecision(15) << xyz[0] << ',' << xyz[1] << ',' << xyz[2] << ']';
}

static void WriteJsonDoubleArray(std::ostream& out, const double* values, long count)
{
  out << '[';
  long i = 0;
  for (; i < count; ++i)
  {
    if (i) out << ',';
    out << std::setprecision(15) << (values ? values[i] : 0.0);
  }
  out << ']';
}

static void WriteJsonLongArray(std::ostream& out, const std::vector<long>& values)
{
  out << '[';
  size_t i = 0;
  for (; i < values.size(); ++i)
  {
    if (i) out << ',';
    out << values[i];
  }
  out << ']';
}

static void WriteKnotVectorJson(std::ostream& out, const char* name,
                                const CATKnotVector* knot_vector)
{
  out << '"' << name << "\":";
  if (!knot_vector)
  {
    out << "null";
    return;
  }
  const short knot_count = knot_vector->GetNumberOfKnots();
  const short control_count = knot_vector->GetNumberOfControlPoints();
  const double* knots = 0;
  knot_vector->GetKnots(knots);
  std::vector<long> multiplicities;
  short i = 1;
  for (; i <= knot_count; ++i)
    multiplicities.push_back(static_cast<long>(knot_vector->GetKnotMultiplicity(i)));
  out << "{\"degree\":" << knot_vector->GetDegree()
      << ",\"original_degree\":" << knot_vector->GetOriginalDegree()
      << ",\"periodic\":" << (knot_vector->IsPeriodic() ? "true" : "false")
      << ",\"uniform\":" << (knot_vector->IsUniform() ? "true" : "false")
      << ",\"control_point_count\":" << control_count
      << ",\"knot_count\":" << knot_count
      << ",\"index_offset\":" << knot_vector->GetIndexOffset()
      << ",\"knots\":";
  WriteJsonDoubleArray(out, knots, knot_count);
  out << ",\"multiplicities\":";
  WriteJsonLongArray(out, multiplicities);
  out << '}';
}

static std::string NurbsCurveJson(CATNurbsCurve* nurbs)
{
  if (!nurbs) return "";
  const CATKnotVector* knots = 0;
  try { knots = nurbs->GetKnotVector(); } catch (...) { knots = 0; }
  const short control_count = knots ? knots->GetNumberOfControlPoints() : 0;
  int original_degree = 0;
  try { nurbs->GetOriginalDegree(original_degree); } catch (...) {}
  double parameter_coefficient = 1.0;
  double parameter_shift = 0.0;
  try { nurbs->GetOriginalParametrisationDATA(parameter_coefficient, parameter_shift); } catch (...) {}
  std::ostringstream out;
  out << "{\"rational\":" << (nurbs->IsRational() ? "true" : "false")
      << ",\"original_degree\":" << original_degree
      << ",\"original_parameterization\":{\"coefficient\":" << std::setprecision(15)
      << parameter_coefficient << ",\"shift\":" << parameter_shift << '}'
      << ",\"control_point_count\":" << control_count
      << ",\"control_points\":[";
  short i = 1;
  for (; i <= control_count; ++i)
  {
    if (i > 1) out << ',';
    CATMathPoint point;
    try { nurbs->GetOneControlPoint(i, point); } catch (...) {}
    WriteJsonPoint(out, point);
  }
  out << "],\"weights\":[";
  for (i = 1; i <= control_count; ++i)
  {
    if (i > 1) out << ',';
    double weight = 1.0;
    try { weight = nurbs->GetOneWeight(i); } catch (...) {}
    out << std::setprecision(15) << weight;
  }
  out << "],";
  WriteKnotVectorJson(out, "knot_vector", knots);
  out << '}';
  return out.str();
}

static std::string NurbsSurfaceJson(CATNurbsSurface* nurbs)
{
  if (!nurbs) return "";
  const CATKnotVector* u_knots = 0;
  const CATKnotVector* v_knots = 0;
  try { u_knots = nurbs->GetKnotVectorU(); } catch (...) { u_knots = 0; }
  try { v_knots = nurbs->GetKnotVectorV(); } catch (...) { v_knots = 0; }
  const short u_count = u_knots ? u_knots->GetNumberOfControlPoints() : 0;
  const short v_count = v_knots ? v_knots->GetNumberOfControlPoints() : 0;
  int original_degree_u = 0;
  int original_degree_v = 0;
  try { nurbs->GetOriginalDegrees(original_degree_u, original_degree_v); } catch (...) {}
  double coefficient_u = 1.0;
  double shift_u = 0.0;
  double coefficient_v = 1.0;
  double shift_v = 0.0;
  try
  {
    nurbs->GetOriginalParametrisationDATA(coefficient_u, shift_u, coefficient_v, shift_v);
  }
  catch (...) {}
  std::ostringstream out;
  out << "{\"rational\":" << (nurbs->IsRational() ? "true" : "false")
      << ",\"original_degree_u\":" << original_degree_u
      << ",\"original_degree_v\":" << original_degree_v
      << ",\"original_parameterization\":{\"u\":{\"coefficient\":" << std::setprecision(15)
      << coefficient_u << ",\"shift\":" << shift_u << "},\"v\":{\"coefficient\":"
      << coefficient_v << ",\"shift\":" << shift_v << "}}"
      << ",\"control_point_count_u\":" << u_count
      << ",\"control_point_count_v\":" << v_count
      << ",\"control_points\":[";
  short u = 1;
  for (; u <= u_count; ++u)
  {
    if (u > 1) out << ',';
    out << '[';
    short v = 1;
    for (; v <= v_count; ++v)
    {
      if (v > 1) out << ',';
      CATMathPoint point;
      try { nurbs->GetOneControlPoint(u, v, point); } catch (...) {}
      WriteJsonPoint(out, point);
    }
    out << ']';
  }
  out << "],\"weights\":[";
  for (u = 1; u <= u_count; ++u)
  {
    if (u > 1) out << ',';
    out << '[';
    short v = 1;
    for (; v <= v_count; ++v)
    {
      if (v > 1) out << ',';
      double weight = 1.0;
      try { weight = nurbs->GetOneWeight(u, v); } catch (...) {}
      out << std::setprecision(15) << weight;
    }
    out << ']';
  }
  out << "],";
  WriteKnotVectorJson(out, "u_knot_vector", u_knots);
  out << ',';
  WriteKnotVectorJson(out, "v_knot_vector", v_knots);
  out << '}';
  return out.str();
}

static std::string GeometryBoundingBoxJson(CATGeometry* geometry)
{
  if (!geometry) return "";
  try
  {
    CATMathBox box;
    geometry->GetBoundingBox(box);
    CATMathPoint low;
    CATMathPoint high;
    box.GetLow(low);
    box.GetHigh(high);
    std::ostringstream out;
    out << "{\"min\":";
    WriteJsonPoint(out, low);
    out << ",\"max\":";
    WriteJsonPoint(out, high);
    out << '}';
    return out.str();
  }
  catch (...) {}
  return "";
}

static std::string SurfaceParameterDomainJson(CATSurface* surface)
{
  if (!surface) return "";
  try
  {
    CATSurLimits limits;
    surface->GetLimits(limits);
    CATSurParam low;
    CATSurParam high;
    limits.GetLow(low);
    limits.GetHigh(high);
    std::ostringstream out;
    out << "{\"kind\":\"surface_uv_limits\",\"u_min\":" << std::setprecision(15)
        << low.GetParamU() << ",\"u_max\":" << high.GetParamU()
        << ",\"v_min\":" << low.GetParamV() << ",\"v_max\":" << high.GetParamV()
        << ",\"u_low_patch\":" << low.GetPatchNumberU()
        << ",\"u_high_patch\":" << high.GetPatchNumberU()
        << ",\"v_low_patch\":" << low.GetPatchNumberV()
        << ",\"v_high_patch\":" << high.GetPatchNumberV()
        << ",\"source_api\":\"CATSurface.GetLimits\"}";
    return out.str();
  }
  catch (...) {}
  return "";
}

static std::string CurveParameterDomainJson(CATCurve* curve)
{
  if (!curve) return "";
  try
  {
    CATCrvLimits limits;
    curve->GetLimits(limits);
    CATCrvParam low;
    CATCrvParam high;
    limits.GetLow(low);
    limits.GetHigh(high);
    std::ostringstream out;
    out << "{\"kind\":\"curve_limits\",\"t_min\":" << std::setprecision(15)
        << low.GetParam() << ",\"t_max\":" << high.GetParam()
        << ",\"low_arc\":" << low.GetArcNumber()
        << ",\"high_arc\":" << high.GetArcNumber()
        << ",\"source_api\":\"CATCurve.GetLimits\"}";
    return out.str();
  }
  catch (...) {}
  return "";
}

template <class InterfaceT>
static InterfaceT* QueryGeometryInterface(CATGeometry* geometry, const IID& iid)
{
  if (!geometry) return 0;
  InterfaceT* typed = 0;
  try
  {
    if (SUCCEEDED(geometry->QueryInterface(iid, reinterpret_cast<void**>(&typed))) && typed)
      return typed;
  }
  catch (...) {}
  return 0;
}

template <class InterfaceT>
static void ReleaseGeometryInterface(InterfaceT*& pointer)
{
  if (pointer)
  {
    pointer->Release();
    pointer = 0;
  }
}

static std::string ElementaryAxisJson(CATElementarySurface* surface)
{
  if (!surface) return "";
  CATMathPoint origin;
  CATMathVector first;
  CATMathVector second;
  CATMathVector third;
  surface->GetAxis(origin, first, second, third);
  std::ostringstream out;
  out << "\"origin\":";
  WriteJsonPoint(out, origin);
  out << ",\"x_direction\":";
  WriteJsonVector(out, first);
  out << ",\"y_direction\":";
  WriteJsonVector(out, second);
  out << ",\"axis_direction\":";
  WriteJsonVector(out, third);
  return out.str();
}

static void DecodeExactCellGeometry(ParseContext& context, CATCell* cell,
                                    NativeTopologyCellRecord& record)
{
  if (!cell || (record.dimension != 2 && record.dimension != 1 && record.dimension != 0))
    return;
  CATOrientation orientation = CATOrientationUnknown;
  CATGeometry* geometry = 0;
  try { geometry = cell->GetGeometry(&orientation); }
  catch (...)
  {
    record.geometry_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "brep_geometry", "CELL_GEOMETRY_GET_FAILED",
                            "CATCell::GetGeometry raised an exception", record.cell_id));
    return;
  }
  if (!geometry)
  {
    record.geometry_status = "unavailable";
    return;
  }

  record.geometry_orientation = orientation == CATOrientationPositive ? "positive" :
    (orientation == CATOrientationNegative ? "negative" : "unknown");
  record.bounding_box_json = GeometryBoundingBoxJson(geometry);

  if (record.dimension == 2)
  {
    CATSurface* domain_surface = QueryGeometryInterface<CATSurface>(geometry, IID_CATSurface);
    if (domain_surface)
    {
      record.parameter_domain_json = SurfaceParameterDomainJson(domain_surface);
      ReleaseGeometryInterface(domain_surface);
    }
    CATPlane* plane = QueryGeometryInterface<CATPlane>(geometry, IID_CATPlane);
    if (plane)
    {
      CATMathPoint origin;
      CATMathDirection first;
      CATMathDirection second;
      plane->GetAxis(origin, first, second);
      std::ostringstream out;
      out << "{\"origin\":";
      WriteJsonPoint(out, origin);
      out << ",\"u_direction\":";
      WriteJsonDirection(out, first);
      out << ",\"v_direction\":";
      WriteJsonDirection(out, second);
      out << ",\"normal\":";
      CATMathPoint normal_origin;
      CATMathVector normal;
      plane->GetNormal(normal_origin, normal);
      WriteJsonVector(out, normal);
      out << '}';
      record.exact_geometry_type = "plane";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(plane);
      return;
    }
    CATCylinder* cylinder = QueryGeometryInterface<CATCylinder>(geometry, IID_CATCylinder);
    if (cylinder)
    {
      CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(geometry, IID_CATElementarySurface);
      std::ostringstream out;
      out << '{';
      const std::string axis_json = ElementaryAxisJson(elementary);
      if (!axis_json.empty()) out << axis_json << ',';
      out << "\"radius_mm\":" << std::setprecision(15) << cylinder->GetRadius()
          << ",\"start_length_mm\":" << cylinder->GetStartLength()
          << ",\"end_length_mm\":" << cylinder->GetEndLength()
          << ",\"start_angle_rad\":" << cylinder->GetStartAngle()
          << ",\"end_angle_rad\":" << cylinder->GetEndAngle() << '}';
      record.exact_geometry_type = "cylinder";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(elementary);
      ReleaseGeometryInterface(cylinder);
      return;
    }
    CATCone* cone = QueryGeometryInterface<CATCone>(geometry, IID_CATCone);
    if (cone)
    {
      CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(geometry, IID_CATElementarySurface);
      std::ostringstream out;
      out << '{';
      const std::string axis_json = ElementaryAxisJson(elementary);
      if (!axis_json.empty()) out << axis_json << ',';
      out << "\"start_radius_mm\":" << std::setprecision(15) << cone->GetStartRadius()
          << ",\"cone_angle_rad\":" << cone->GetConeAngle()
          << ",\"start_angle_rad\":" << cone->GetStartAngle()
          << ",\"end_angle_rad\":" << cone->GetEndAngle()
          << ",\"start_rule_length_mm\":" << cone->GetStartRuleLength()
          << ",\"end_rule_length_mm\":" << cone->GetEndRuleLength() << '}';
      record.exact_geometry_type = "cone";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(elementary);
      ReleaseGeometryInterface(cone);
      return;
    }
    CATSphere* sphere = QueryGeometryInterface<CATSphere>(geometry, IID_CATSphere);
    if (sphere)
    {
      CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(geometry, IID_CATElementarySurface);
      std::ostringstream out;
      out << '{';
      const std::string axis_json = ElementaryAxisJson(elementary);
      if (!axis_json.empty()) out << axis_json << ',';
      out << "\"radius_mm\":" << std::setprecision(15) << sphere->GetRadius()
          << ",\"meridian_start_angle_rad\":" << sphere->GetMeridianStartAngle()
          << ",\"meridian_end_angle_rad\":" << sphere->GetMeridianEndAngle()
          << ",\"parallel_start_angle_rad\":" << sphere->GetParallelStartAngle()
          << ",\"parallel_end_angle_rad\":" << sphere->GetParallelEndAngle() << '}';
      record.exact_geometry_type = "sphere";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(elementary);
      ReleaseGeometryInterface(sphere);
      return;
    }
    CATTorus* torus = QueryGeometryInterface<CATTorus>(geometry, IID_CATTorus);
    if (torus)
    {
      CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(geometry, IID_CATElementarySurface);
      std::ostringstream out;
      out << '{';
      const std::string axis_json = ElementaryAxisJson(elementary);
      if (!axis_json.empty()) out << axis_json << ',';
      out << "\"major_radius_mm\":" << std::setprecision(15) << torus->GetMajorRadius()
          << ",\"minor_radius_mm\":" << torus->GetMinorRadius()
          << ",\"major_start_angle_rad\":" << torus->GetMajorStartAngle()
          << ",\"major_end_angle_rad\":" << torus->GetMajorEndAngle()
          << ",\"minor_start_angle_rad\":" << torus->GetMinorStartAngle()
          << ",\"minor_end_angle_rad\":" << torus->GetMinorEndAngle() << '}';
      record.exact_geometry_type = "torus";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(elementary);
      ReleaseGeometryInterface(torus);
      return;
    }
    CATNurbsSurface* nurbs = QueryGeometryInterface<CATNurbsSurface>(geometry, IID_CATNurbsSurface);
    if (nurbs)
    {
      record.exact_geometry_type = "nurbs_surface";
      record.geometry_parameters_json = NurbsSurfaceJson(nurbs);
      record.geometry_status = record.geometry_parameters_json.empty() ? "partial" : "exact";
      ReleaseGeometryInterface(nurbs);
      return;
    }
    CATSurface* surface = QueryGeometryInterface<CATSurface>(geometry, IID_CATSurface);
    if (surface)
    {
      const CATSurface* geometric_rep = 0;
      try { geometric_rep = surface->GetGeometricRep(); } catch (...) { geometric_rep = 0; }
      CATGeometry* rep_geometry = const_cast<CATSurface*>(geometric_rep);
      if (rep_geometry && rep_geometry != surface)
      {
        CATPlane* rep_plane = QueryGeometryInterface<CATPlane>(rep_geometry, IID_CATPlane);
        if (rep_plane)
        {
          CATMathPoint origin;
          CATMathDirection first;
          CATMathDirection second;
          rep_plane->GetAxis(origin, first, second);
          CATMathPoint normal_origin;
          CATMathVector normal;
          rep_plane->GetNormal(normal_origin, normal);
          std::ostringstream out;
          out << "{\"origin\":";
          WriteJsonPoint(out, origin);
          out << ",\"u_direction\":";
          WriteJsonDirection(out, first);
          out << ",\"v_direction\":";
          WriteJsonDirection(out, second);
          out << ",\"normal\":";
          WriteJsonVector(out, normal);
          out << ",\"surface_representation\":\"CATSurface.GetGeometricRep\"}";
          record.exact_geometry_type = "plane";
          record.geometry_parameters_json = out.str();
          record.geometry_status = "exact";
          ReleaseGeometryInterface(rep_plane);
          ReleaseGeometryInterface(surface);
          return;
        }
        CATCylinder* rep_cylinder = QueryGeometryInterface<CATCylinder>(rep_geometry, IID_CATCylinder);
        if (rep_cylinder)
        {
          CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(rep_geometry, IID_CATElementarySurface);
          std::ostringstream out;
          out << '{';
          const std::string axis_json = ElementaryAxisJson(elementary);
          if (!axis_json.empty()) out << axis_json << ',';
          out << "\"radius_mm\":" << std::setprecision(15) << rep_cylinder->GetRadius()
              << ",\"start_length_mm\":" << rep_cylinder->GetStartLength()
              << ",\"end_length_mm\":" << rep_cylinder->GetEndLength()
              << ",\"start_angle_rad\":" << rep_cylinder->GetStartAngle()
              << ",\"end_angle_rad\":" << rep_cylinder->GetEndAngle()
              << ",\"surface_representation\":\"CATSurface.GetGeometricRep\"}";
          record.exact_geometry_type = "cylinder";
          record.geometry_parameters_json = out.str();
          record.geometry_status = "exact";
          ReleaseGeometryInterface(elementary);
          ReleaseGeometryInterface(rep_cylinder);
          ReleaseGeometryInterface(surface);
          return;
        }
        CATTorus* rep_torus = QueryGeometryInterface<CATTorus>(rep_geometry, IID_CATTorus);
        if (rep_torus)
        {
          CATElementarySurface* elementary = QueryGeometryInterface<CATElementarySurface>(rep_geometry, IID_CATElementarySurface);
          std::ostringstream out;
          out << '{';
          const std::string axis_json = ElementaryAxisJson(elementary);
          if (!axis_json.empty()) out << axis_json << ',';
          out << "\"major_radius_mm\":" << std::setprecision(15) << rep_torus->GetMajorRadius()
              << ",\"minor_radius_mm\":" << rep_torus->GetMinorRadius()
              << ",\"major_start_angle_rad\":" << rep_torus->GetMajorStartAngle()
              << ",\"major_end_angle_rad\":" << rep_torus->GetMajorEndAngle()
              << ",\"minor_start_angle_rad\":" << rep_torus->GetMinorStartAngle()
              << ",\"minor_end_angle_rad\":" << rep_torus->GetMinorEndAngle()
              << ",\"surface_representation\":\"CATSurface.GetGeometricRep\"}";
          record.exact_geometry_type = "torus";
          record.geometry_parameters_json = out.str();
          record.geometry_status = "exact";
          ReleaseGeometryInterface(elementary);
          ReleaseGeometryInterface(rep_torus);
          ReleaseGeometryInterface(surface);
          return;
        }
      }
      record.exact_geometry_type = "other_surface";
      record.geometry_status = "partial";
      ReleaseGeometryInterface(surface);
      return;
    }
  }
  else if (record.dimension == 1)
  {
    CATCurve* domain_curve = QueryGeometryInterface<CATCurve>(geometry, IID_CATCurve);
    if (domain_curve)
    {
      record.parameter_domain_json = CurveParameterDomainJson(domain_curve);
      ReleaseGeometryInterface(domain_curve);
    }
    CATLine* line = QueryGeometryInterface<CATLine>(geometry, IID_CATLine);
    if (line)
    {
      CATMathPoint origin;
      CATMathDirection direction;
      line->GetOrigin(origin);
      line->GetDirection(direction);
      std::ostringstream out;
      out << "{\"origin\":";
      WriteJsonPoint(out, origin);
      out << ",\"direction\":";
      WriteJsonDirection(out, direction);
      out << '}';
      record.exact_geometry_type = "line";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(line);
      return;
    }
    CATCircle* circle = QueryGeometryInterface<CATCircle>(geometry, IID_CATCircle);
    if (circle)
    {
      CATConic* conic = QueryGeometryInterface<CATConic>(geometry, IID_CATConic);
      CATMathPoint center;
      CATMathPlane support;
      CATMathVector u;
      CATMathVector v;
      CATMathVector normal;
      if (conic)
      {
        conic->GetOrigin(center);
        conic->GetSupport(support);
        support.GetFirstDirection(u);
        support.GetSecondDirection(v);
        support.GetNormal(normal);
      }
      std::ostringstream out;
      out << "{\"center\":";
      WriteJsonPoint(out, center);
      out << ",\"u_direction\":";
      WriteJsonVector(out, u);
      out << ",\"v_direction\":";
      WriteJsonVector(out, v);
      out << ",\"normal\":";
      WriteJsonVector(out, normal);
      out << ",\"radius_mm\":" << std::setprecision(15) << circle->GetRadius()
          << ",\"start_angle_rad\":" << circle->GetStartAngle()
          << ",\"end_angle_rad\":" << circle->GetEndAngle() << '}';
      record.exact_geometry_type = "circle";
      record.geometry_parameters_json = out.str();
      record.geometry_status = "exact";
      ReleaseGeometryInterface(conic);
      ReleaseGeometryInterface(circle);
      return;
    }
    CATNurbsCurve* nurbs = QueryGeometryInterface<CATNurbsCurve>(geometry, IID_CATNurbsCurve);
    if (nurbs)
    {
      record.exact_geometry_type = "nurbs_curve";
      record.geometry_parameters_json = NurbsCurveJson(nurbs);
      record.geometry_status = record.geometry_parameters_json.empty() ? "partial" : "exact";
      ReleaseGeometryInterface(nurbs);
      return;
    }
    CATCurve* curve = QueryGeometryInterface<CATCurve>(geometry, IID_CATCurve);
    if (curve)
    {
      record.exact_geometry_type = "other_curve";
      record.geometry_status = "partial";
      ReleaseGeometryInterface(curve);
      return;
    }
  }
  record.exact_geometry_type = record.dimension == 0 ? "point" : "unknown";
  if (record.geometry_status.empty()) record.geometry_status = record.dimension == 0 ? "partial" : "unknown";
}

// 用途：读取单个 Face 的 CGM 三角化统计，并生成 Face→Triangle Range 映射记录。
static void AppendMeshFaceMap(ParseContext& context, CATICGMBodyTessellator* tessellator,
                              CATFace* face, const std::string& body_id,
                              const std::string& face_id, long primitive_index,
                              long& next_triangle)
{
  NativeMeshFaceMapRecord record;
  record.mesh_map_id = MakeTopologyCellId(body_id, "M", primitive_index);
  record.body_id = body_id;
  record.face_cell_id = face_id;
  record.primitive_index = primitive_index;
  record.triangle_start = next_triangle;
  record.value_source = "typed_caa_public_body_tessellator";
  if (!tessellator || !face)
  {
    record.tessellation_status = "unavailable";
    context.mesh_face_maps.push_back(record);
    return;
  }

  try
  {
    CATBoolean planar = FALSE;
    CATCGMTessPointIter* points = 0;
    CATCGMTessStripeIter* strips = 0;
    CATCGMTessFanIter* fans = 0;
    CATCGMTessPolyIter* polygons = 0;
    CATCGMTessTrianIter* triangles = 0;
    short side = 0;
    tessellator->GetFace(face, planar, &points, &strips, &fans, &polygons, &triangles, &side);
    record.planar = planar ? true : false;
    record.face_orientation_side = side;
    if (points) record.point_count = static_cast<long>(points->GetNbPoint());
    if (triangles) record.isolated_triangle_count = static_cast<long>(triangles->GetNbTrian());
    CATLONG32 strip_points = 0;
    if (strips)
    {
      record.strip_count = static_cast<long>(strips->GetNbStri(strip_points));
      record.estimated_triangle_count += EstimateTrianglesFromPointGroups(record.strip_count, strip_points);
    }
    CATLONG32 fan_points = 0;
    if (fans)
    {
      record.fan_count = static_cast<long>(fans->GetNbFan(fan_points));
      record.estimated_triangle_count += EstimateTrianglesFromPointGroups(record.fan_count, fan_points);
    }
    CATLONG32 polygon_points = 0;
    if (polygons)
    {
      record.polygon_count = static_cast<long>(polygons->GetNbPoly(polygon_points));
      record.estimated_triangle_count += EstimateTrianglesFromPointGroups(record.polygon_count, polygon_points);
    }
    record.estimated_triangle_count += record.isolated_triangle_count;
    std::map<int, TessPointData> point_map;
    BuildTessPointMap(points, point_map);
    long triangle_in_face = 0;
    long failed_triangle_count = 0;
    if (triangles)
    {
      triangles->Reset();
      while (!triangles->IsExhausted())
      {
        int ranks[3] = { 0, 0, 0 };
        triangles->GetTrianNuPts(ranks);
        if (!AppendMeshTriangle(context, record, point_map, ranks[0], ranks[1], ranks[2],
                                "triangle", next_triangle, triangle_in_face))
          ++failed_triangle_count;
        triangles->GoToNext();
      }
    }
    if (strips)
    {
      strips->Reset();
      while (!strips->IsExhausted())
      {
        const CATLONG32 count = strips->GetStriNbPts();
        if (count >= 3)
        {
          std::vector<int> ranks(static_cast<size_t>(count), 0);
          strips->GetStriNuPts(&ranks[0]);
          CATLONG32 i = 0;
          for (i = 0; i < count - 2; ++i)
          {
            const int a = (i % 2 == 0) ? ranks[static_cast<size_t>(i)] : ranks[static_cast<size_t>(i + 1)];
            const int b = (i % 2 == 0) ? ranks[static_cast<size_t>(i + 1)] : ranks[static_cast<size_t>(i)];
            const int c = ranks[static_cast<size_t>(i + 2)];
            if (!AppendMeshTriangle(context, record, point_map, a, b, c,
                                    "strip", next_triangle, triangle_in_face))
              ++failed_triangle_count;
          }
        }
        strips->GoToNext();
      }
    }
    if (fans)
    {
      fans->Reset();
      while (!fans->IsExhausted())
      {
        const CATLONG32 count = fans->GetFanNbPts();
        if (count >= 3)
        {
          std::vector<int> ranks(static_cast<size_t>(count), 0);
          fans->GetFanNuPts(&ranks[0]);
          CATLONG32 i = 1;
          for (; i < count - 1; ++i)
            if (!AppendMeshTriangle(context, record, point_map, ranks[0],
                                    ranks[static_cast<size_t>(i)],
                                    ranks[static_cast<size_t>(i + 1)],
                                    "fan", next_triangle, triangle_in_face))
              ++failed_triangle_count;
        }
        fans->GoToNext();
      }
    }
    if (polygons)
    {
      polygons->Reset();
      while (!polygons->IsExhausted())
      {
        const CATLONG32 count = polygons->GetPolyNbPts();
        if (count >= 3)
        {
          std::vector<int> ranks(static_cast<size_t>(count), 0);
          polygons->GetPolyNuPts(&ranks[0]);
          CATLONG32 i = 1;
          for (; i < count - 1; ++i)
            if (!AppendMeshTriangle(context, record, point_map, ranks[0],
                                    ranks[static_cast<size_t>(i)],
                                    ranks[static_cast<size_t>(i + 1)],
                                    "polygon", next_triangle, triangle_in_face))
              ++failed_triangle_count;
        }
        polygons->GoToNext();
      }
    }
    record.triangle_count = triangle_in_face;
    record.tessellation_status = failed_triangle_count == 0 ? "success" : "partial";
    if (failed_triangle_count != 0)
      record.diagnostic_ids.push_back(
        context.AddDiagnostic("warning", "mesh_face_mapping", "FACE_TRIANGLE_PAYLOAD_PARTIAL",
                              "Some tessellated triangle point ranks could not be resolved", face_id));
  }
  catch (...)
  {
    record.tessellation_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "mesh_face_mapping", "FACE_TESSELLATION_FAILED",
                            "CATICGMBodyTessellator::GetFace failed for a face", ""));
  }
  context.mesh_face_maps.push_back(record);
}

// 用途：将 R21 Public CATTopology 枚举出来的单元写成纯数据记录。
// cell 指针只在当前函数内读取维度和 domain 数，不写入 IR，也不作为 ID 决胜依据。
static void AppendTopologyCell(ParseContext& context, const std::string& body_id,
                               const char* prefix, long index, CATCell* cell,
                               CATBody* body,
                               const std::map<CATCell*, std::string>& cell_ids)
{
  NativeTopologyCellRecord record;
  record.cell_id = MakeTopologyCellId(body_id, prefix, index);
  record.body_id = body_id;
  record.topology_index = index;
  record.runtime_cell_pointer = cell;
  record.stable_id_method = "cat_topology_dimension_order_revision_local";
  record.value_source = "typed_caa_public_cat_topology";
  if (!cell)
  {
    record.cell_kind = "unknown";
    record.dimension = -1;
    context.topology_cells.push_back(record);
    return;
  }
  try
  {
    const short dimension = cell->GetDimension();
    record.dimension = static_cast<long>(dimension);
    record.cell_kind = TopologyCellKind(dimension);
  }
  catch (...)
  {
    record.cell_kind = "unknown";
    record.dimension = -1;
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_DIMENSION_FAILED",
                            "CATCell::GetDimension raised an exception", ""));
  }
  try { record.domain_count = static_cast<long>(cell->GetNbDomains()); }
  catch (...)
  {
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_DOMAIN_COUNT_FAILED",
                            "CATCell::GetNbDomains raised an exception", ""));
  }
  try { record.internal_domain_count = static_cast<long>(cell->GetNbInternalDomains()); }
  catch (...)
  {
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_INTERNAL_DOMAIN_COUNT_FAILED",
                            "CATCell::GetNbInternalDomains raised an exception", ""));
  }
  try
  {
    CATMathPoint center;
    cell->EstimateCenter(center);
    center.GetCoord(record.center_mm);
    record.has_center = true;
  }
  catch (...)
  {
    record.geometry_status = "center_unavailable";
  }
  try
  {
    if (record.dimension == 2)
    {
      CATFace* face = static_cast<CATFace*>(cell);
      record.area_mm2 = face->CalcArea();
      record.area_mm2_available = true;
      record.measure_status = "success";
    }
    else if (record.dimension == 1)
    {
      CATEdge* edge = static_cast<CATEdge*>(cell);
      record.length_mm = edge->CalcLength();
      record.length_mm_available = true;
      record.measure_status = "success";
    }
    else
      record.measure_status = "not_applicable";
  }
  catch (...)
  {
    record.measure_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_MEASURE_FAILED",
                            "CATFace::CalcArea or CATEdge::CalcLength failed", ""));
  }
  DecodeExactCellGeometry(context, cell, record);
  if (record.geometry_status.empty()) record.geometry_status = record.has_center ? "success" : "partial";
  FillBoundaryCellIds(cell, cell_ids, record);
  FillAdjacentCellIds(body, cell, cell_ids, record);
  FillMaterialSide(body, cell, record);
  context.topology_cells.push_back(record);
}

// 用途：读取 ResultOUT cell 的中心、面积或长度；该函数只写 Result cell 明细，不写最终 Face 映射。
static void FillNativeFeatureResultCellGeometry(CATCell* cell,
                                                NativeFeatureResultCellRecord& record,
                                                ParseContext& context)
{
  if (!cell)
  {
    record.read_status = "unavailable";
    return;
  }
  try
  {
    const short dimension = cell->GetDimension();
    record.dimension = static_cast<long>(dimension);
    record.cell_kind = TopologyCellKind(dimension);
  }
  catch (...)
  {
    record.dimension = -1;
    record.cell_kind = "unknown";
    record.read_status = "partial";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_CELL_DIMENSION_FAILED",
                            "CATCell::GetDimension failed for ResultOUT cell", record.source_feature_id));
  }
  try
  {
    CATMathPoint center;
    cell->EstimateCenter(center);
    center.GetCoord(record.center_mm);
    record.has_center = true;
  }
  catch (...)
  {
    record.read_status = "partial";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_CELL_CENTER_FAILED",
                            "CATCell::EstimateCenter failed for ResultOUT cell", record.source_feature_id));
  }
  try
  {
    if (record.dimension == 2)
    {
      CATFace* face = static_cast<CATFace*>(cell);
      record.area_mm2 = face->CalcArea();
      record.area_mm2_available = true;
    }
    else if (record.dimension == 1)
    {
      CATEdge* edge = static_cast<CATEdge*>(cell);
      record.length_mm = edge->CalcLength();
      record.length_mm_available = true;
    }
  }
  catch (...)
  {
    record.read_status = "partial";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_CELL_MEASURE_FAILED",
                            "CATFace::CalcArea or CATEdge::CalcLength failed for ResultOUT cell",
                            record.source_feature_id));
  }
  if (record.read_status.empty()) record.read_status = "success";
}

// 用途：将 ResultOUT body 中的 cell 指针转为 Result cell 稳定编号，指针只用于本轮内存索引。
static void BuildResultCellIdMap(const std::string& result_id,
                                 const std::vector<CATCell*>& faces,
                                 const std::vector<CATCell*>& edges,
                                 const std::vector<CATCell*>& vertices,
                                 const std::vector<CATCell*>& volumes,
                                 std::map<CATCell*, std::string>& ids)
{
  long index = 1;
  std::vector<CATCell*>::const_iterator it = faces.begin();
  for (; it != faces.end(); ++it, ++index)
    ids[*it] = MakeTopologyCellId(result_id, "RF", index);
  index = 1;
  it = edges.begin();
  for (; it != edges.end(); ++it, ++index)
    ids[*it] = MakeTopologyCellId(result_id, "RE", index);
  index = 1;
  it = vertices.begin();
  for (; it != vertices.end(); ++it, ++index)
    ids[*it] = MakeTopologyCellId(result_id, "RV", index);
  index = 1;
  it = volumes.begin();
  for (; it != volumes.end(); ++it, ++index)
    ids[*it] = MakeTopologyCellId(result_id, "RS", index);
}

// 用途：输出一条 ResultOUT cell 明细，并记录该 cell 在 ResultOUT 内的直接边界关系。
static void AppendNativeFeatureResultCell(ParseContext& context,
                                          const NativeFeatureResultRecord& result_record,
                                          const char* prefix,
                                          long index,
                                          CATCell* cell,
                                          const std::map<CATCell*, std::string>& result_cell_ids)
{
  NativeFeatureResultCellRecord record;
  record.result_cell_id = MakeTopologyCellId(result_record.result_id, prefix, index);
  record.result_id = result_record.result_id;
  record.source_feature_id = result_record.source_feature_id;
  record.source_kind = result_record.source_kind;
  record.result_cell_index = index;
  record.runtime_cell_pointer = cell;
  record.stable_id_method = "cat_feature_result_dimension_order_revision_local";
  record.value_source = "typed_caa_public_shape_feature_body_resultout";
  FillNativeFeatureResultCellGeometry(cell, record, context);
  if (cell)
  {
    CATBoundaryIterator* raw_iterator = 0;
    try { raw_iterator = cell->CreateBoundaryIterator(); }
    catch (...) { raw_iterator = 0; }
    BoundaryIteratorGuard iterator_guard(raw_iterator);
    CATBoundaryIterator* iterator = iterator_guard.Get();
    if (iterator)
    {
      try
      {
        CATSide side = CATSideUnknown;
        CATDomain* domain = 0;
        short new_domain = 0;
        CATCell* boundary = 0;
        while ((boundary = iterator->Next(&side, &domain, &new_domain)) != 0)
        {
          const std::string boundary_id = LookupCellId(boundary, result_cell_ids);
          if (!boundary_id.empty() &&
              std::find(record.boundary_result_cell_ids.begin(),
                        record.boundary_result_cell_ids.end(), boundary_id) ==
              record.boundary_result_cell_ids.end())
            record.boundary_result_cell_ids.push_back(boundary_id);
        }
      }
      catch (...)
      {
        record.read_status = "partial";
        record.diagnostic_ids.push_back(
          context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_CELL_BOUNDARY_FAILED",
                                "CATBoundaryIterator failed for ResultOUT cell",
                                record.source_feature_id));
      }
    }
  }
  if (record.read_status.empty()) record.read_status = "success";
  context.native_feature_result_cells.push_back(record);
}

// 用途：计算两个三维点之间的距离；调用方保证两个记录都已经有中心点。
static double Distance3(const double a[3], const double b[3])
{
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 用途：给 ResultOUT Face 查找最终主实体 Face 的几何指纹候选；这是候选映射，不冒充 Generic Naming 权威结果。
static void AppendNativeFeatureTopologyLink(ParseContext& context,
                                            const NativeFeatureResultCellRecord& result_cell)
{
  if (result_cell.dimension != 2)
    return;

  NativeFeatureTopologyLinkRecord link;
  std::ostringstream id;
  id << "NFTL";
  const long index = static_cast<long>(context.native_feature_topology_links.size() + 1);
  if (index < 10) id << "00000";
  else if (index < 100) id << "0000";
  else if (index < 1000) id << "000";
  else if (index < 10000) id << "00";
  else if (index < 100000) id << "0";
  id << index;
  link.link_id = id.str();
  link.source_feature_id = result_cell.source_feature_id;
  link.result_id = result_cell.result_id;
  link.result_cell_id = result_cell.result_cell_id;
  link.mapping_direction = "result_cell_to_final_face";
  link.mapping_method = "caa_resultout_to_final_face_geometry_fingerprint_candidate";
  link.mapping_status = "unmatched";

  if (result_cell.runtime_cell_pointer)
  {
    std::vector<NativeTopologyCellRecord>::const_iterator exact = context.topology_cells.begin();
    for (; exact != context.topology_cells.end(); ++exact)
    {
      if (exact->dimension != 2) continue;
      if (exact->runtime_cell_pointer == result_cell.runtime_cell_pointer)
      {
        link.final_cell_id = exact->cell_id;
        link.final_body_id = exact->body_id;
        link.mapping_method = "catia_resultout_final_cell_pointer_identity";
        link.mapping_status = "runtime_matched";
        link.authority = "runtime_cell_identity";
        link.relation_kind = "survives_to_final";
        link.confidence = 1.0;
        link.candidate_count = 1;
        link.candidate_final_cell_ids.push_back(exact->cell_id);
        context.native_feature_topology_links.push_back(link);
        return;
      }
    }
  }

  if (!result_cell.has_center || !result_cell.area_mm2_available)
  {
    link.mapping_status = "insufficient_result_fingerprint";
    context.native_feature_topology_links.push_back(link);
    return;
  }

  double best_center = 0.0;
  double best_area = 0.0;
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension != 2 || !cell->has_center || !cell->area_mm2_available)
      continue;
    const double center_residual = Distance3(result_cell.center_mm, cell->center_mm);
    const double area_residual = std::fabs(result_cell.area_mm2 - cell->area_mm2);
    const double area_tolerance = std::max(0.001, std::fabs(result_cell.area_mm2) * 0.000001);
    if (center_residual <= 0.001 && area_residual <= area_tolerance)
    {
      if (link.candidate_count == 0 || center_residual < best_center ||
          (center_residual == best_center && area_residual < best_area))
      {
        best_center = center_residual;
        best_area = area_residual;
        link.final_cell_id = cell->cell_id;
        link.final_body_id = cell->body_id;
        link.center_residual_mm = center_residual;
        link.measure_residual = area_residual;
      }
      link.candidate_final_cell_ids.push_back(cell->cell_id);
      ++link.candidate_count;
    }
  }

  if (link.candidate_count == 1)
  {
    link.mapping_status = "candidate";
    link.confidence = 0.75;
  }
  else if (link.candidate_count > 1)
  {
    link.mapping_status = "ambiguous";
    link.confidence = 0.35;
    link.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "feature_topology_mapping",
                            "FEATURE_RESULT_FINAL_FACE_AMBIGUOUS",
                            "ResultOUT face matched multiple final faces by geometry fingerprint",
                            result_cell.source_feature_id));
  }
  context.native_feature_topology_links.push_back(link);
}

// 用途：把 Face 边界中的每个 Loop/Wire 作为独立记录输出，便于后续区分开口环、内环和边界拓扑。
static void AppendFaceWires(ParseContext& context, CATFace* face,
                            const std::string& body_id, const std::string& face_id,
                            long face_index, const std::map<CATCell*, std::string>& cell_ids,
                            long& next_wire_index, long& next_coedge_index)
{
  if (!face) return;
  CATBoundaryIterator* raw_iterator = 0;
  try { raw_iterator = face->CreateBoundaryIterator(); }
  catch (...) { raw_iterator = 0; }
  BoundaryIteratorGuard iterator_guard(raw_iterator);
  CATBoundaryIterator* iterator = iterator_guard.Get();
  if (!iterator) return;

  NativeTopologyWireRecord current;
  bool has_current = false;
  long coedge_index_in_wire = 0;
  try
  {
    CATSide side = CATSideUnknown;
    CATDomain* domain = 0;
    short new_domain = 0;
    CATCell* boundary = 0;
    while ((boundary = iterator->Next(&side, &domain, &new_domain)) != 0)
    {
      if (!has_current || new_domain)
      {
        if (has_current)
        {
          current.edge_count = static_cast<long>(current.edge_cell_ids.size());
          context.topology_wires.push_back(current);
        }
        current = NativeTopologyWireRecord();
        ++next_wire_index;
        current.wire_id = MakeTopologyCellId(body_id, "W", next_wire_index);
        current.body_id = body_id;
        current.wire_index = next_wire_index;
        current.wire_kind = "face_loop";
        current.owning_face_id = face_id;
        current.owning_face_topology_index = face_index;
        current.closed_status = "unknown";
        current.value_source = "typed_caa_public_boundary_iterator";
        has_current = true;
        coedge_index_in_wire = 0;
      }
      const std::string edge_id = LookupCellId(boundary, cell_ids);
      if (!edge_id.empty())
      {
        current.edge_cell_ids.push_back(edge_id);
        NativeTopologyCoedgeRecord coedge;
        ++next_coedge_index;
        ++coedge_index_in_wire;
        coedge.coedge_id = MakeTopologyCellId(body_id, "C", next_coedge_index);
        coedge.body_id = body_id;
        coedge.wire_id = current.wire_id;
        coedge.owning_face_id = face_id;
        coedge.edge_cell_id = edge_id;
        coedge.coedge_index = next_coedge_index;
        coedge.coedge_index_in_wire = coedge_index_in_wire;
        coedge.edge_orientation_side = static_cast<short>(side);
        coedge.orientation_status = side == CATSideUnknown ? "unknown" : "from_cat_boundary_iterator_side";
        coedge.value_source = "typed_caa_public_boundary_iterator";
        context.topology_coedges.push_back(coedge);
      }
    }
    if (has_current)
    {
      current.edge_count = static_cast<long>(current.edge_cell_ids.size());
      context.topology_wires.push_back(current);
    }
  }
  catch (...)
  {
    context.AddDiagnostic("warning", "topology", "FACE_WIRE_ENUMERATION_FAILED",
                          "CATBoundaryIterator failed while grouping face loops", "");
  }
}

static void UpdateFaceWireIds(ParseContext& context, const std::string& face_id)
{
  if (face_id.empty()) return;
  std::vector<std::string> wire_ids;
  std::vector<NativeTopologyWireRecord>::const_iterator wire = context.topology_wires.begin();
  for (; wire != context.topology_wires.end(); ++wire)
    if (wire->owning_face_id == face_id) wire_ids.push_back(wire->wire_id);
  if (wire_ids.empty()) return;
  std::vector<NativeTopologyCellRecord>::iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->cell_id == face_id)
    {
      cell->outer_wire_id = wire_ids[0];
      cell->inner_wire_ids.clear();
      size_t index = 1;
      for (; index < wire_ids.size(); ++index) cell->inner_wire_ids.push_back(wire_ids[index]);
      std::vector<NativeTopologyWireRecord>::iterator wire_record = context.topology_wires.begin();
      for (; wire_record != context.topology_wires.end(); ++wire_record)
      {
        if (wire_record->owning_face_id != face_id) continue;
        wire_record->wire_kind = wire_record->wire_id == cell->outer_wire_id ?
          "outer_loop" : "inner_loop";
      }
      return;
    }
  }
}

static void AddUniqueString(std::vector<std::string>& values, const std::string& value)
{
  if (value.empty()) return;
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

static std::string CatSideName(CATSide side)
{
  if (side == CATSideLeft) return "left";
  if (side == CATSideRight) return "right";
  if (side == CATSideFull) return "full";
  return "unknown";
}

static NativeTopologyCellRecord* FindTopologyCell(ParseContext& context,
                                                  const std::string& cell_id)
{
  std::vector<NativeTopologyCellRecord>::iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
    if (cell->cell_id == cell_id) return &(*cell);
  return 0;
}

static NativeTopologyWireRecord* FindTopologyWire(ParseContext& context,
                                                  const std::string& wire_id)
{
  std::vector<NativeTopologyWireRecord>::iterator wire = context.topology_wires.begin();
  for (; wire != context.topology_wires.end(); ++wire)
    if (wire->wire_id == wire_id) return &(*wire);
  return 0;
}

static bool EdgesShareVertex(const std::string& first_edge,
                             const std::string& second_edge,
                             const std::map<std::string, std::set<std::string> >& edge_vertices)
{
  std::map<std::string, std::set<std::string> >::const_iterator first =
    edge_vertices.find(first_edge);
  std::map<std::string, std::set<std::string> >::const_iterator second =
    edge_vertices.find(second_edge);
  if (first == edge_vertices.end() || second == edge_vertices.end()) return false;
  std::set<std::string>::const_iterator vertex = first->second.begin();
  for (; vertex != first->second.end(); ++vertex)
    if (second->second.find(*vertex) != second->second.end()) return true;
  return false;
}

static void FinalizeBrepTopologyGraph(ParseContext& context)
{
  std::map<std::string, std::vector<size_t> > coedges_by_wire;
  std::map<std::string, std::set<std::string> > edge_to_faces;
  std::map<std::string, std::set<std::string> > edge_to_vertices;
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension != 1) continue;
    std::vector<std::string>::const_iterator boundary = cell->boundary_cell_ids.begin();
    for (; boundary != cell->boundary_cell_ids.end(); ++boundary)
      edge_to_vertices[cell->cell_id].insert(*boundary);
  }
  size_t i = 0;
  for (; i < context.topology_coedges.size(); ++i)
  {
    NativeTopologyCoedgeRecord& coedge = context.topology_coedges[i];
    coedges_by_wire[coedge.wire_id].push_back(i);
    if (!coedge.edge_cell_id.empty() && !coedge.owning_face_id.empty())
      edge_to_faces[coedge.edge_cell_id].insert(coedge.owning_face_id);
    NativeTopologyCellRecord* face = FindTopologyCell(context, coedge.owning_face_id);
    if (face) AddUniqueString(face->boundary_cell_ids, coedge.edge_cell_id);
  }

  std::map<std::string, std::vector<size_t> >::iterator wire_group = coedges_by_wire.begin();
  for (; wire_group != coedges_by_wire.end(); ++wire_group)
  {
    std::vector<size_t>& indices = wire_group->second;
    if (indices.empty()) continue;
    size_t count = indices.size();
    size_t j = 0;
    for (; j < count; ++j)
    {
      NativeTopologyCoedgeRecord& coedge = context.topology_coedges[indices[j]];
      coedge.previous_coedge_id = context.topology_coedges[indices[(j + count - 1) % count]].coedge_id;
      coedge.next_coedge_id = context.topology_coedges[indices[(j + 1) % count]].coedge_id;
      if (coedge.orientation_status.empty() || coedge.orientation_status == "unknown")
        coedge.orientation_status = "from_cat_boundary_iterator_side";
    }
    NativeTopologyWireRecord* wire = FindTopologyWire(context, wire_group->first);
    if (wire)
    {
      bool closed_by_vertices = !wire->edge_cell_ids.empty();
      size_t edge_index = 0;
      for (; edge_index < wire->edge_cell_ids.size(); ++edge_index)
      {
        const std::string& edge_id = wire->edge_cell_ids[edge_index];
        const std::string& next_edge_id =
          wire->edge_cell_ids[(edge_index + 1) % wire->edge_cell_ids.size()];
        if (!EdgesShareVertex(edge_id, next_edge_id, edge_to_vertices))
        {
          closed_by_vertices = false;
          break;
        }
      }
      wire->closed_status = closed_by_vertices ?
        "closed_by_edge_vertex_continuity" : "ordered_by_cat_boundary_iterator_unverified";
      wire->edge_count = static_cast<long>(wire->edge_cell_ids.size());
    }
  }

  std::map<std::string, std::set<std::string> >::iterator edge_faces = edge_to_faces.begin();
  for (; edge_faces != edge_to_faces.end(); ++edge_faces)
  {
    NativeTopologyCellRecord* edge = FindTopologyCell(context, edge_faces->first);
    if (!edge) continue;
    std::set<std::string>::const_iterator face = edge_faces->second.begin();
    for (; face != edge_faces->second.end(); ++face)
      AddUniqueString(edge->adjacent_cell_ids, *face);
    for (face = edge_faces->second.begin(); face != edge_faces->second.end(); ++face)
    {
      NativeTopologyCellRecord* face_cell = FindTopologyCell(context, *face);
      if (!face_cell) continue;
      std::set<std::string>::const_iterator other = edge_faces->second.begin();
      for (; other != edge_faces->second.end(); ++other)
        if (*other != *face) AddUniqueString(face_cell->adjacent_cell_ids, *other);
    }
  }
}

// 用途：按 CATTopology::GetAllCells 的原生顺序枚举指定维度拓扑单元，并预先建立指针到稳定 ID 的运行期索引。
static void LoadCellsByDimension(ParseContext& context, CATBody* body, short dimension,
                                 const char* prefix, const std::string& body_id,
                                 std::vector<CATCell*>& output,
                                 std::map<CATCell*, std::string>& cell_ids)
{
  if (!body) return;
  CATLISTP(CATCell) cells;
  try
  {
    body->GetAllCells(cells, dimension);
  }
  catch (...)
  {
    context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_ENUMERATION_FAILED",
                          "CATTopology::GetAllCells raised an exception", "");
    return;
  }
  int index = 0;
  for (index = 1; index <= cells.Size(); ++index)
  {
    CATCell* cell = cells[index];
    output.push_back(cell);
    if (cell) cell_ids[cell] = MakeTopologyCellId(body_id, prefix, static_cast<long>(index));
  }
}

// 用途：从 Part 的主实体 CATBody 读取真实拓扑数量和 Face/Edge/Vertex/Volume 列表。
// 当前只建立几何出口，不建立 Feature->Face 映射，因此能力矩阵仍如实保持映射 not_available。
static void CollectPartMainSolidTopology(CATISpecObject* part_spec,
                                         const std::string& part_feature_id,
                                         ParseContext& context)
{
  if (!part_spec) return;
  CATIPrtPart* part_interface = 0;
  try
  {
    if (FAILED(part_spec->QueryInterface(IID_CATIPrtPart,
                                         reinterpret_cast<void**>(&part_interface))) ||
        !part_interface)
      return;
  }
  catch (...)
  {
    context.AddDiagnostic("warning", "topology", "CATIPRTPART_QUERY_EXCEPTION",
                          "CATIPrtPart QueryInterface raised an exception", part_feature_id);
    return;
  }
  CaaInterfaceGuard<CATIPrtPart> part_guard(part_interface);
  CATBody_var body_var = NULL_var;
  try
  {
    body_var = part_interface->GetSolid();
  }
  catch (...)
  {
    context.AddDiagnostic("warning", "topology", "PART_MAIN_SOLID_READ_FAILED",
                          "CATIPrtPart::GetSolid raised an exception", part_feature_id);
    return;
  }
  if (body_var == NULL_var)
  {
    context.AddDiagnostic("info", "topology", "PART_MAIN_SOLID_UNAVAILABLE",
                          "CATIPrtPart::GetSolid returned null", part_feature_id);
    return;
  }
  CATBody* body = body_var;
  if (!body) return;

  NativeTopologyBodyRecord body_record;
  body_record.body_id = "TB000001";
  body_record.source_feature_id = part_feature_id;
  body_record.source_kind = "catiprtpart_main_solid";
  body_record.read_status = "success";
  body_record.value_source = "typed_caa_public_cat_body";
  body_record.stability_scope = "same_input_same_r21_parser_revision";
  try
  {
    int vertices = 0;
    int edges = 0;
    int faces = 0;
    int volumes = 0;
    body->GetCellNumbers(&vertices, &edges, &faces, &volumes);
    body_record.vertex_count = static_cast<long>(vertices);
    body_record.edge_count = static_cast<long>(edges);
    body_record.face_count = static_cast<long>(faces);
    body_record.volume_count = static_cast<long>(volumes);
  }
  catch (...)
  {
    body_record.read_status = "partial";
    body_record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "topology", "TOPOLOGY_CELL_COUNT_FAILED",
                            "CATTopology::GetCellNumbers raised an exception", part_feature_id));
  }
  context.topology_bodies.push_back(body_record);

  std::vector<CATCell*> faces;
  std::vector<CATCell*> edges;
  std::vector<CATCell*> vertices;
  std::vector<CATCell*> volumes;
  std::map<CATCell*, std::string> cell_ids;
  LoadCellsByDimension(context, body, 2, "F", body_record.body_id, faces, cell_ids);
  LoadCellsByDimension(context, body, 1, "E", body_record.body_id, edges, cell_ids);
  LoadCellsByDimension(context, body, 0, "V", body_record.body_id, vertices, cell_ids);
  LoadCellsByDimension(context, body, 3, "S", body_record.body_id, volumes, cell_ids);

  CATICGMBodyTessellator* tessellator = 0;
  try
  {
    // 用途：0.1mm sag 只用于输出轻量化三角证据，不参与几何识别或尺寸测量。
    tessellator = CATCGMCreateBodyTessellator(body, 0.1);
    if (tessellator) tessellator->Run();
  }
  catch (...)
  {
    if (tessellator) { tessellator->Release(); tessellator = 0; }
    context.AddDiagnostic("warning", "mesh_face_mapping", "BODY_TESSELLATION_FAILED",
                          "CATCGMCreateBodyTessellator or Run failed", part_feature_id);
  }
  CgmTessellatorGuard tessellator_guard(tessellator);

  long next_wire_index = 0;
  long next_coedge_index = 0;
  long next_triangle = 0;
  std::vector<CATCell*>::iterator it = faces.begin();
  long index = 1;
  for (; it != faces.end(); ++it, ++index)
  {
    AppendTopologyCell(context, body_record.body_id, "F", index, *it, body, cell_ids);
    CATFace* face = static_cast<CATFace*>(*it);
    const std::string face_id = LookupCellId(*it, cell_ids);
    AppendFaceWires(context, face, body_record.body_id,
                    face_id, index, cell_ids, next_wire_index, next_coedge_index);
    UpdateFaceWireIds(context, face_id);
    AppendMeshFaceMap(context, tessellator_guard.Get(), face, body_record.body_id,
                      face_id, index, next_triangle);
  }
  it = edges.begin();
  index = 1;
  for (; it != edges.end(); ++it, ++index)
    AppendTopologyCell(context, body_record.body_id, "E", index, *it, body, cell_ids);
  it = vertices.begin();
  index = 1;
  for (; it != vertices.end(); ++it, ++index)
    AppendTopologyCell(context, body_record.body_id, "V", index, *it, body, cell_ids);
  it = volumes.begin();
  index = 1;
  for (; it != volumes.end(); ++it, ++index)
    AppendTopologyCell(context, body_record.body_id, "S", index, *it, body, cell_ids);

  FinalizeBrepTopologyGraph(context);
}

// 用途：从 CATBody 安全读取拓扑数量，并写入特征 ResultOUT 摘要。
// body 是借用指针，只在当前函数内读数量，不跨越文档生命周期保存。
static void FillNativeFeatureResultCounts(CATBody* body,
                                          NativeFeatureResultRecord& record,
                                          ParseContext& context)
{
  if (!body)
  {
    record.read_status = "unavailable";
    return;
  }
  try
  {
    int vertices = 0;
    int edges = 0;
    int faces = 0;
    int volumes = 0;
    body->GetCellNumbers(&vertices, &edges, &faces, &volumes);
    record.vertex_count = static_cast<long>(vertices);
    record.edge_count = static_cast<long>(edges);
    record.face_count = static_cast<long>(faces);
    record.volume_count = static_cast<long>(volumes);
    record.read_status = "success";
  }
  catch (...)
  {
    record.read_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_CELL_COUNT_FAILED",
                            "CATTopology::GetCellNumbers failed for feature ResultOUT", record.source_feature_id));
  }
}

// 用途：把形状特征 ResultOUT 的 Face/Edge/Vertex/Volume 明细写入独立 JSONL 上下文。
// 这些 cell 属于该特征的结果体；只有后续 link 记录能表达它是否疑似对应最终主实体 Face。
static void CollectNativeFeatureResultCells(CATBody* body,
                                            const NativeFeatureResultRecord& result_record,
                                            ParseContext& context)
{
  if (!body || result_record.read_status != "success")
    return;

  std::vector<CATCell*> faces;
  std::vector<CATCell*> edges;
  std::vector<CATCell*> vertices;
  std::vector<CATCell*> volumes;
  std::map<CATCell*, std::string> result_cell_ids;
  LoadCellsByDimension(context, body, 2, "RF", result_record.result_id, faces, result_cell_ids);
  LoadCellsByDimension(context, body, 1, "RE", result_record.result_id, edges, result_cell_ids);
  LoadCellsByDimension(context, body, 0, "RV", result_record.result_id, vertices, result_cell_ids);
  LoadCellsByDimension(context, body, 3, "RS", result_record.result_id, volumes, result_cell_ids);

  BuildResultCellIdMap(result_record.result_id, faces, edges, vertices, volumes, result_cell_ids);

  std::vector<CATCell*>::iterator it = faces.begin();
  long index = 1;
  for (; it != faces.end(); ++it, ++index)
  {
    const size_t before = context.native_feature_result_cells.size();
    AppendNativeFeatureResultCell(context, result_record, "RF", index, *it, result_cell_ids);
    if (context.native_feature_result_cells.size() > before)
      AppendNativeFeatureTopologyLink(context, context.native_feature_result_cells.back());
  }
  it = edges.begin();
  index = 1;
  for (; it != edges.end(); ++it, ++index)
    AppendNativeFeatureResultCell(context, result_record, "RE", index, *it, result_cell_ids);
  it = vertices.begin();
  index = 1;
  for (; it != vertices.end(); ++it, ++index)
    AppendNativeFeatureResultCell(context, result_record, "RV", index, *it, result_cell_ids);
  it = volumes.begin();
  index = 1;
  for (; it != volumes.end(); ++it, ++index)
    AppendNativeFeatureResultCell(context, result_record, "RS", index, *it, result_cell_ids);
}

// 用途：读取单个形状特征的 ResultOUT 拓扑摘要。
// 该摘要用于后续映射算法输入，但当前不宣称这些 cell 已与最终主实体 Face 对齐。
static void CollectNativeFeatureResultTopology(CATISpecObject* spec,
                                               const std::string& feature_id,
                                               const TypeFingerprint& fingerprint,
                                               ParseContext& context)
{
  if (!spec) return;
  CATIShapeFeatureBody* shape_body = 0;
  try
  {
    if (FAILED(spec->QueryInterface(IID_CATIShapeFeatureBody,
                                    reinterpret_cast<void**>(&shape_body))) ||
        !shape_body)
      return;
  }
  catch (...)
  {
    context.AddDiagnostic("warning", "result_topology", "SHAPE_FEATURE_BODY_QUERY_EXCEPTION",
                          "CATIShapeFeatureBody QueryInterface raised an exception", feature_id);
    return;
  }
  CaaInterfaceGuard<CATIShapeFeatureBody> shape_guard(shape_body);

  NativeFeatureResultRecord record;
  std::ostringstream id;
  id << "NFR";
  const long index = static_cast<long>(context.native_feature_results.size() + 1);
  if (index < 10) id << "00000";
  else if (index < 100) id << "0000";
  else if (index < 1000) id << "000";
  else if (index < 10000) id << "00";
  else if (index < 100000) id << "0";
  id << index;
  record.result_id = id.str();
  record.source_feature_id = feature_id;
  record.source_kind = fingerprint.startup_type.empty() ?
    fingerprint.native_type : fingerprint.startup_type;
  record.read_status = "unavailable";
  record.value_source = "typed_caa_public_shape_feature_body";
  record.final_body_mapping_status = "not_available";

  CATISpecObject_var result_out = NULL_var;
  try
  {
    result_out = shape_body->GetResultOUT();
  }
  catch (...)
  {
    record.read_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_OUT_FAILED",
                            "CATIShapeFeatureBody::GetResultOUT raised an exception", feature_id));
    context.native_feature_results.push_back(record);
    return;
  }
  if (result_out == NULL_var)
  {
    context.native_feature_results.push_back(record);
    return;
  }

  CATIGeometricalElement* geometrical = 0;
  CATISpecObject* result_spec = result_out;
  if (!result_spec)
  {
    context.native_feature_results.push_back(record);
    return;
  }
  try
  {
    if (FAILED(result_spec->QueryInterface(IID_CATIGeometricalElement,
                                           reinterpret_cast<void**>(&geometrical))) ||
        !geometrical)
    {
      context.native_feature_results.push_back(record);
      return;
    }
  }
  catch (...)
  {
    record.read_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_GEOMETRY_QUERY_EXCEPTION",
                            "CATIGeometricalElement QueryInterface raised an exception", feature_id));
    context.native_feature_results.push_back(record);
    return;
  }
  CaaInterfaceGuard<CATIGeometricalElement> geometry_guard(geometrical);
  CATBody_var result_body = NULL_var;
  try
  {
    result_body = geometrical->GetBodyResult();
  }
  catch (...)
  {
    record.read_status = "failed";
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("warning", "result_topology", "FEATURE_RESULT_BODY_FAILED",
                            "CATIGeometricalElement::GetBodyResult raised an exception", feature_id));
    context.native_feature_results.push_back(record);
    return;
  }
  if (result_body != NULL_var)
  {
    CATBody* body = result_body;
    FillNativeFeatureResultCounts(body, record, context);
    CollectNativeFeatureResultCells(body, record, context);
  }
  context.native_feature_results.push_back(record);
}

// 用途：释放 CATITPSList 中按 Item 返回的组件接口；局部守卫只接管一个 Query/Item 引用。
template <class T>
class TpsInterfaceGuard
{
public:
  explicit TpsInterfaceGuard(T* pointer = 0) : _pointer(pointer) {}
  ~TpsInterfaceGuard() { if (_pointer) _pointer->Release(); }
  T* Get() const { return _pointer; }
  T*& Out() { return _pointer; }

private:
  TpsInterfaceGuard(const TpsInterfaceGuard&);
  TpsInterfaceGuard& operator=(const TpsInterfaceGuard&);
  T* _pointer;
};

// 用途：读取单个 TPS 组件的公开语义观测；仅记录已验证接口，不按未覆盖类型猜测公差含义。
static void AppendFtaSemanticRecord(CATITPSComponent* component,
                                    const FtaSetRecord& set_record,
                                    unsigned int component_index,
                                    ParseContext& context)
{
  FtaSemanticRecord record;
  std::ostringstream id;
  id << set_record.fta_set_id << "_TPS";
  const long one_based_index = static_cast<long>(component_index + 1);
  if (one_based_index < 10) id << "00000";
  else if (one_based_index < 100) id << "0000";
  else if (one_based_index < 1000) id << "000";
  else if (one_based_index < 10000) id << "00";
  else if (one_based_index < 100000) id << "0";
  id << one_based_index;
  record.fta_semantic_id = id.str();
  record.fta_set_id = set_record.fta_set_id;
  record.component_index = one_based_index;
  record.read_status = component ? "partial" : "unavailable";
  record.component_kind = "unknown_tps_component";
  record.validation_text_status = "unavailable";
  record.topology_mapping_status = "not_available";
  record.value_source = "typed_caa_public_tps_component";

  if (!component)
  {
    context.fta_semantics.push_back(record);
    return;
  }

  CATITPS * tps = 0;
  if (SUCCEEDED(component->QueryInterface(IID_CATITPS, reinterpret_cast<void**>(&tps))) && tps)
  {
    TpsInterfaceGuard<CATITPS> tps_guard(tps);
    record.supported_interface_keys.push_back("CATITPS");
    record.component_kind = "tps";
  }

  CATITPSSemanticValidity* semantic = 0;
  if (SUCCEEDED(component->QueryInterface(IID_CATITPSSemanticValidity,
                                          reinterpret_cast<void**>(&semantic))) && semantic)
  {
    TpsInterfaceGuard<CATITPSSemanticValidity> semantic_guard(semantic);
    record.supported_interface_keys.push_back("CATITPSSemanticValidity");
    int count = 0;
    IID** iid_list = 0;
    if (SUCCEEDED(semantic->GetUnderstandingSemanticsItf(&count, &iid_list)))
    {
      record.semantic_interface_count = static_cast<long>(count);
      delete [] iid_list;
    }
    count = 0;
    iid_list = 0;
    if (SUCCEEDED(semantic->GetAllSemanticsItf(&count, &iid_list)))
    {
      record.all_semantic_interface_count = static_cast<long>(count);
      delete [] iid_list;
    }
    wchar_t* diagnostic = 0;
    CATTPSStatus status = CATTPSStatusUnknown;
    if (SUCCEEDED(semantic->Check(&diagnostic, &status)))
    {
      record.semantic_check_status_raw = static_cast<long>(status);
      if (diagnostic)
      {
        record.semantic_check_diagnostic = "available_but_not_converted";
        delete [] diagnostic;
      }
    }
  }

  CATITPSText* text = 0;
  if (SUCCEEDED(component->QueryInterface(IID_CATITPSText, reinterpret_cast<void**>(&text))) && text)
  {
    TpsInterfaceGuard<CATITPSText> text_guard(text);
    record.supported_interface_keys.push_back("CATITPSText");
  }

  CATITPSTextContent* text_content = 0;
  if (SUCCEEDED(component->QueryInterface(IID_CATITPSTextContent,
                                          reinterpret_cast<void**>(&text_content))) && text_content)
  {
    TpsInterfaceGuard<CATITPSTextContent> text_content_guard(text_content);
    record.supported_interface_keys.push_back("CATITPSTextContent");
    CATUnicodeString validation_text;
    if (SUCCEEDED(text_content->GetValidationString(validation_text)))
    {
      record.validation_text = UnicodeToUtf8(validation_text);
      record.validation_text_status = "success";
    }
    else
      record.validation_text_status = "failed";
  }

  if (!record.supported_interface_keys.empty())
    record.read_status = "success";
  context.fta_semantics.push_back(record);
}

// 用途：读取一个 TPS Set 中已验证可访问的数量信息，并枚举每个 TPS 组件的第一层语义观测。
// 逐类 GD&T 参数和 TPS->拓扑映射仍需对应专用接口和样件验证，不能在这里猜。
static void FillFtaSetCounts(CATITPSSet* set_interface, FtaSetRecord& record,
                             ParseContext& context)
{
  if (!set_interface)
  {
    record.read_status = "failed";
    return;
  }
  record.read_status = "success";
  record.value_source = "typed_caa_public_tps";
  record.semantic_detail_status = "partial";
  record.topology_mapping_status = "not_available";

  CATITPSList* tps_list = 0;
  if (SUCCEEDED(set_interface->GetTPSs(&tps_list)) && tps_list)
  {
    TpsInterfaceGuard<CATITPSList> list_guard(tps_list);
    unsigned int count = 0;
    if (SUCCEEDED(tps_list->Count(&count))) record.tps_count = static_cast<long>(count);
    else
      record.diagnostic_ids.push_back(
        context.AddDiagnostic("warning", "fta", "TPS_SET_TPS_COUNT_FAILED",
                              "CATITPSList::Count failed for TPS list", ""));
    unsigned int item_index = 0;
    for (; item_index < count; ++item_index)
    {
      CATITPSComponent* component = 0;
      if (SUCCEEDED(tps_list->Item(item_index, &component)) && component)
      {
        TpsInterfaceGuard<CATITPSComponent> component_guard(component);
        AppendFtaSemanticRecord(component, record, item_index, context);
      }
      else
      {
        record.diagnostic_ids.push_back(
          context.AddDiagnostic("warning", "fta", "TPS_COMPONENT_ITEM_FAILED",
                                "CATITPSList::Item failed for a TPS component", ""));
      }
    }
  }
  else
  {
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("info", "fta", "TPS_SET_TPS_LIST_UNAVAILABLE",
                            "CATITPSSet::GetTPSs returned no list", ""));
  }

  CATITPSGeometryList* geometry_list = 0;
  if (SUCCEEDED(set_interface->GetGeometries(&geometry_list)) && geometry_list)
  {
    TpsInterfaceGuard<CATITPSGeometryList> geometry_guard(geometry_list);
    unsigned int count = 0;
    if (SUCCEEDED(geometry_list->Count(&count))) record.geometry_count = static_cast<long>(count);
    else
      record.diagnostic_ids.push_back(
        context.AddDiagnostic("warning", "fta", "TPS_SET_GEOMETRY_COUNT_FAILED",
                              "CATITPSGeometryList::Count failed for geometry list", ""));
  }
  else
  {
    record.diagnostic_ids.push_back(
      context.AddDiagnostic("info", "fta", "TPS_SET_GEOMETRY_LIST_UNAVAILABLE",
                            "CATITPSSet::GetGeometries returned no list", ""));
  }
}

// 用途：从 CATDocument 查询 R21 Public CATITPSDocument，并枚举文档内 TPS Set。
// 如果文档不支持 FTA/TPS 接口，只记录 not_available，不生成假 Annotation。
static void CollectFtaSets(CATDocument* document, ParseContext& context,
                           const std::string& document_feature_id)
{
  context.runtime_info["fta_extraction_status"] = "not_available";
  if (!document) return;
  CATITPSDocument* tps_document = 0;
  try
  {
    if (FAILED(document->QueryInterface(IID_CATITPSDocument,
                                        reinterpret_cast<void**>(&tps_document))) ||
        !tps_document)
    {
      context.AddDiagnostic("info", "fta", "TPS_DOCUMENT_UNSUPPORTED",
                            "CATDocument does not expose CATITPSDocument", document_feature_id);
      return;
    }
  }
  catch (...)
  {
    context.runtime_info["fta_extraction_status"] = "failed";
    context.AddDiagnostic("warning", "fta", "TPS_DOCUMENT_QUERY_EXCEPTION",
                          "CATITPSDocument QueryInterface raised an exception", document_feature_id);
    return;
  }

  TpsInterfaceGuard<CATITPSDocument> tps_document_guard(tps_document);
  CATITPSList* sets = 0;
  HRESULT result = E_FAIL;
  try
  {
    result = tps_document->GetSets(&sets, CATTPSSSMRecursive, FALSE);
  }
  catch (...)
  {
    context.runtime_info["fta_extraction_status"] = "failed";
    context.AddDiagnostic("warning", "fta", "TPS_GET_SETS_EXCEPTION",
                          "CATITPSDocument::GetSets raised an exception", document_feature_id);
    return;
  }
  if (FAILED(result) || !sets)
  {
    context.runtime_info["fta_extraction_status"] = "partial";
    context.AddDiagnostic("info", "fta", "TPS_SETS_EMPTY_OR_UNAVAILABLE",
                          "CATITPSDocument::GetSets returned no set list", document_feature_id);
    return;
  }
  TpsInterfaceGuard<CATITPSList> sets_guard(sets);
  unsigned int count = 0;
  if (FAILED(sets->Count(&count)))
  {
    context.runtime_info["fta_extraction_status"] = "partial";
    context.AddDiagnostic("warning", "fta", "TPS_SET_COUNT_FAILED",
                          "CATITPSList::Count failed for set list", document_feature_id);
    return;
  }
  context.runtime_info["fta_extraction_status"] = "complete";

  unsigned int index = 0;
  for (index = 0; index < count; ++index)
  {
    CATITPSComponent* component = 0;
    if (FAILED(sets->Item(index, &component)) || !component) continue;
    TpsInterfaceGuard<CATITPSComponent> component_guard(component);
    CATITPSSet* set_interface = 0;
    if (FAILED(component->QueryInterface(IID_CATITPSSet,
                                         reinterpret_cast<void**>(&set_interface))) ||
        !set_interface)
    {
      context.AddDiagnostic("warning", "fta", "TPS_SET_QUERY_FAILED",
                            "TPS set item does not expose CATITPSSet", document_feature_id);
      continue;
    }
    TpsInterfaceGuard<CATITPSSet> set_guard(set_interface);
    FtaSetRecord record;
    std::ostringstream id;
    id << "FTA";
    if (index + 1 < 10) id << "00000";
    else if (index + 1 < 100) id << "0000";
    else if (index + 1 < 1000) id << "000";
    else if (index + 1 < 10000) id << "00";
    else if (index + 1 < 100000) id << "0";
    id << (index + 1);
    record.fta_set_id = id.str();
    record.set_index = static_cast<long>(index + 1);
    FillFtaSetCounts(set_interface, record, context);
    context.fta_sets.push_back(record);
  }
}

// 用途：从 CATICkeParm::Name 返回的限定路径中取参数叶名称，归属仍由真实 parent_of 图决定。
static std::string ParameterLeafName(const std::string& qualified_name)
{
  const std::string::size_type separator = qualified_name.find_last_of("/\\");
  return separator == std::string::npos ? qualified_name : qualified_name.substr(separator + 1);
}

// 用途：把 Automation Public 接口返回的 UTF-16 CATBSTR 转为独立 UTF-8 字符串。
static std::string BstrToUtf8(const CATBSTR value)
{
  if (!value) return "";
  const int wide_length = static_cast<int>(SysStringLen(value));
  if (wide_length == 0) return "";
  const int byte_length = WideCharToMultiByte(CP_UTF8, 0, value, wide_length,
                                               0, 0, 0, 0);
  if (byte_length <= 0) return "";
  std::vector<char> buffer(static_cast<size_t>(byte_length));
  WideCharToMultiByte(CP_UTF8, 0, value, wide_length, &buffer[0], byte_length, 0, 0);
  return std::string(&buffer[0], static_cast<size_t>(byte_length));
}

// 用途：从 CATIALength 的真实 Value 属性读取毫米数；调用者负责接口引用生命周期。
static bool ReadLengthValue(CATIALength* length, double& value)
{
  return length && SUCCEEDED(length->get_Value(value));
}

// 用途：从 CATIAAngle 的真实 Value 属性读取角度原值；R21 当前样件不依赖该可选字段。
static bool ReadAngleValue(CATIAAngle* angle, double& value)
{
  return angle && SUCCEEDED(angle->get_Value(value));
}

static void AddNativeParameterField(NativeFeatureParameterData& data,
                                    const char* name,
                                    const char* value_type,
                                    const char* availability,
                                    const char* source_api,
                                    const char* reason_code,
                                    const std::string& raw_value,
                                    const char* raw_unit,
                                    bool has_numeric_value,
                                    double numeric_value,
                                    const char* normalized_unit)
{
  NativeFeatureParameterField field;
  field.name = name ? name : "";
  field.value_type = value_type ? value_type : "";
  field.availability = availability ? availability : "";
  field.source_api = source_api ? source_api : "";
  field.reason_code = reason_code ? reason_code : "";
  field.raw_value = raw_value;
  field.raw_unit = raw_unit ? raw_unit : "";
  field.has_numeric_value = has_numeric_value;
  field.numeric_value = numeric_value;
  field.normalized_unit = normalized_unit ? normalized_unit : "";
  data.parameters.push_back(field);
}

static void AddUnavailableParameter(NativeFeatureParameterData& data,
                                    const char* name,
                                    const char* value_type,
                                    const char* source_api,
                                    const char* reason_code)
{
  AddNativeParameterField(data, name, value_type, "not_available", source_api,
                          reason_code, "", "", false, 0.0, "");
}

static void AddBoolParameter(NativeFeatureParameterData& data, const char* name,
                             bool value, const char* source_api)
{
  AddNativeParameterField(data, name, "boolean", "available", source_api, "OK",
                          value ? "true" : "false", "", false, value ? 1.0 : 0.0, "");
}

static void AddLongParameter(NativeFeatureParameterData& data, const char* name,
                             long value, const char* source_api)
{
  std::ostringstream text;
  text << value;
  AddNativeParameterField(data, name, "integer", "available", source_api, "OK",
                          text.str(), "", true, static_cast<double>(value), "count");
}

static void AddDoubleParameter(NativeFeatureParameterData& data, const char* name,
                               double value, const char* value_type,
                               const char* normalized_unit,
                               const char* source_api)
{
  std::ostringstream text;
  text << std::setprecision(15) << value;
  AddNativeParameterField(data, name, value_type, "available", source_api, "OK",
                          text.str(), normalized_unit, true, value, normalized_unit);
}

static void AddEnumParameter(NativeFeatureParameterData& data, const char* name,
                             int value, const char* source_api)
{
  std::ostringstream text;
  text << value;
  AddNativeParameterField(data, name, "enum_raw", "available", source_api, "OK",
                          text.str(), "", true, static_cast<double>(value), "");
}

static void AddLengthParameter(NativeFeatureParameterData& data, const char* name,
                               CATIALength* length, const char* source_api,
                               bool& ok)
{
  double value = 0.0;
  if (ReadLengthValue(length, value))
  {
    std::ostringstream text;
    text << value;
    AddNativeParameterField(data, name, "length", "available", source_api, "OK",
                            text.str(), "mm", true, value, "mm");
  }
  else
  {
    ok = false;
    AddUnavailableParameter(data, name, "length", source_api, "VALUE_READ_FAILED");
  }
}

static void AddAngleParameter(NativeFeatureParameterData& data, const char* name,
                              CATIAAngle* angle, const char* source_api,
                              bool& ok)
{
  double value = 0.0;
  if (ReadAngleValue(angle, value))
  {
    std::ostringstream text;
    text << value;
    AddNativeParameterField(data, name, "angle", "available", source_api, "OK",
                            text.str(), "deg", true, value, "deg");
  }
  else
  {
    ok = false;
    AddUnavailableParameter(data, name, "angle", source_api, "VALUE_READ_FAILED");
  }
}

static long ReferenceCollectionCount(CATIAReferences* references)
{
  if (!references) return -1;
  CATLONG count = 0;
  if (FAILED(references->get_Count(count))) return -1;
  return static_cast<long>(count);
}

static void AddReferenceCollectionField(NativeFeatureParameterData& data,
                                        const char* name,
                                        CATIAReferences* references,
                                        const char* source_api,
                                        bool required,
                                        bool& ok)
{
  NativeFeatureReferenceField field;
  field.name = name ? name : "";
  field.source_api = source_api ? source_api : "";
  field.count = ReferenceCollectionCount(references);
  if (field.count >= 0)
  {
    field.availability = "available";
    field.reason_code = "OK";
  }
  else
  {
    if (required) ok = false;
    field.availability = "not_available";
    field.reason_code = references ? "COUNT_READ_FAILED" : "REFERENCE_COLLECTION_NULL";
  }
  data.references.push_back(field);
}

static void AddSingleReferenceField(NativeFeatureParameterData& data,
                                    const char* name,
                                    CATIAReference* reference,
                                    const char* source_api,
                                    bool required,
                                    bool& ok)
{
  NativeFeatureReferenceField field;
  field.name = name ? name : "";
  field.source_api = source_api ? source_api : "";
  field.count = reference ? 1 : 0;
  if (reference)
  {
    field.availability = "available";
    field.reason_code = "OK";
    CaaBstrGuard display_name;
    if (SUCCEEDED(reference->get_DisplayName(display_name.Out())) && display_name.Get())
      field.display_names.push_back(BstrToUtf8(display_name.Get()));
  }
  else
  {
    if (required) ok = false;
    field.availability = "not_available";
    field.reason_code = "REFERENCE_NULL";
  }
  data.references.push_back(field);
}

static void AddSpecObjectReferenceField(NativeFeatureParameterData& data,
                                        const char* name,
                                        const CATISpecObject_var& reference,
                                        const char* source_api,
                                        bool required,
                                        bool& ok)
{
  NativeFeatureReferenceField field;
  field.name = name ? name : "";
  field.source_api = source_api ? source_api : "";
  field.count = reference != NULL_var ? 1 : 0;
  if (reference != NULL_var)
  {
    field.availability = "available";
    field.reason_code = "OK";
    try
    {
      const std::string display_name = UnicodeToUtf8(reference->GetDisplayName());
      if (!display_name.empty()) field.display_names.push_back(display_name);
      else
      {
        const std::string name_value = UnicodeToUtf8(reference->GetName());
        if (!name_value.empty()) field.display_names.push_back(name_value);
      }
    }
    catch (...) {}
  }
  else
  {
    if (required) ok = false;
    field.availability = "not_available";
    field.reason_code = "REFERENCE_NULL";
  }
  data.references.push_back(field);
}

static bool ReadIntParamValue(CATIAIntParam* param, long& value)
{
  CATLONG raw_value = 0;
  if (!param || FAILED(param->get_Value(raw_value))) return false;
  value = static_cast<long>(raw_value);
  return true;
}

// 用途：把 SAFEARRAY(VARIANT) 的常见数值类型无损转换为 double。
static bool VariantToDouble(const CATVariant& value, double& output)
{
  if (V_VT(&value) == VT_R8) { output = V_R8(&value); return true; }
  if (V_VT(&value) == VT_R4) { output = V_R4(&value); return true; }
  if (V_VT(&value) == VT_I4) { output = V_I4(&value); return true; }
  if (V_VT(&value) == VT_I2) { output = V_I2(&value); return true; }
  return false;
}

static bool ExtractVector3FromSafeArray(CATSafeArrayVariant* array, double output[3])
{
  if (!array) return false;
  CATVariant* values = 0;
  if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values))) || !values)
    return false;
  bool valid = true;
  int index = 0;
  for (; index < 3; ++index)
    if (!VariantToDouble(values[index], output[index])) valid = false;
  SafeArrayUnaccessData(array);
  return valid;
}

static void AddVector3Parameters(NativeFeatureParameterData& data,
                                 const char* prefix,
                                 const double values[3],
                                 const char* value_type,
                                 const char* normalized_unit,
                                 const char* source_api)
{
  const char* base = prefix ? prefix : "vector";
  const char* suffixes[3] = { "_x", "_y", "_z" };
  int index = 0;
  for (; index < 3; ++index)
  {
    std::string name = std::string(base) + suffixes[index];
    AddDoubleParameter(data, name.c_str(), values[index], value_type,
                       normalized_unit, source_api);
  }
}

static void AddUnavailableVector3(NativeFeatureParameterData& data,
                                  const char* prefix,
                                  const char* value_type,
                                  const char* source_api,
                                  const char* reason_code,
                                  bool& ok)
{
  ok = false;
  const char* base = prefix ? prefix : "vector";
  const char* suffixes[3] = { "_x", "_y", "_z" };
  int index = 0;
  for (; index < 3; ++index)
  {
    std::string name = std::string(base) + suffixes[index];
    AddUnavailableParameter(data, name.c_str(), value_type, source_api, reason_code);
  }
}

static void AddSafeArrayVector3(NativeFeatureParameterData& data,
                                const char* prefix,
                                CATSafeArrayVariant* array,
                                const char* value_type,
                                const char* normalized_unit,
                                const char* source_api,
                                bool& ok)
{
  double values[3] = { 0.0, 0.0, 0.0 };
  if (ExtractVector3FromSafeArray(array, values))
    AddVector3Parameters(data, prefix, values, value_type, normalized_unit, source_api);
  else
    AddUnavailableVector3(data, prefix, value_type, source_api,
                          "SAFEARRAY_VECTOR_READ_FAILED", ok);
}

static void ReadBaseRepartitionFields(NativeFeatureParameterData& data,
                                      const char* prefix,
                                      CATIARepartition* repartition,
                                      const char* source_api,
                                      bool required,
                                      bool& ok)
{
  std::string base = prefix ? prefix : "repartition";
  CaaInterfaceGuard<CATIAIntParam> count_guard;
  long count = 0;
  if (repartition &&
      SUCCEEDED(repartition->get_InstancesCount(count_guard.Out())) &&
      ReadIntParamValue(count_guard.Get(), count))
  {
    AddLongParameter(data, (base + "_instances_count").c_str(), count, source_api);
  }
  else
  {
    if (required) ok = false;
    AddUnavailableParameter(data, (base + "_instances_count").c_str(), "integer",
                            source_api, "VALUE_READ_FAILED");
  }
}

static void AddLinearRepartitionFields(NativeFeatureParameterData& data,
                                       const char* prefix,
                                       CATIALinearRepartition* repartition,
                                       const char* source_api,
                                       bool required,
                                       bool& ok)
{
  std::string base = prefix ? prefix : "linear_repartition";
  ReadBaseRepartitionFields(data, base.c_str(), repartition, source_api, required, ok);
  CaaInterfaceGuard<CATIALength> spacing_guard;
  if (repartition &&
      SUCCEEDED(repartition->get_Spacing(spacing_guard.Out())))
    AddLengthParameter(data, (base + "_spacing").c_str(), spacing_guard.Get(),
                       source_api, ok);
  else
  {
    if (required) ok = false;
    AddUnavailableParameter(data, (base + "_spacing").c_str(), "length",
                            source_api, "VALUE_READ_FAILED");
  }
}

static void AddAngularRepartitionFields(NativeFeatureParameterData& data,
                                        const char* prefix,
                                        CATIAAngularRepartition* repartition,
                                        const char* source_api,
                                        bool required,
                                        bool& ok)
{
  std::string base = prefix ? prefix : "angular_repartition";
  ReadBaseRepartitionFields(data, base.c_str(), repartition, source_api, required, ok);
  CaaInterfaceGuard<CATIAAngle> angular_guard;
  if (repartition &&
      SUCCEEDED(repartition->get_AngularSpacing(angular_guard.Out())))
    AddAngleParameter(data, (base + "_angular_spacing").c_str(), angular_guard.Get(),
                      source_api, ok);
  else
  {
    if (required) ok = false;
    AddUnavailableParameter(data, (base + "_angular_spacing").c_str(), "angle",
                            source_api, "VALUE_READ_FAILED");
  }
  CaaInterfaceGuard<CATIAAngle> instance_guard;
  if (repartition &&
      SUCCEEDED(repartition->get_InstanceSpacing(instance_guard.Out())))
    AddAngleParameter(data, (base + "_instance_spacing").c_str(), instance_guard.Get(),
                      source_api, ok);
  else
    AddUnavailableParameter(data, (base + "_instance_spacing").c_str(), "angle",
                            source_api, "VALUE_READ_FAILED");
}

static CATIAReference* ReferenceCollectionItem(CATIAReferences* references,
                                               CATLONG item_index)
{
  if (!references) return 0;
  CATVariant index;
  VariantInit(&index);
  V_VT(&index) = VT_I4;
  V_I4(&index) = item_index;
  CATIAReference* item = 0;
  if (FAILED(references->Item(index, item))) return 0;
  return item;
}

static void AddBaseFilletFields(NativeFeatureParameterData& data,
                                CATIAFillet* fillet,
                                CATIAEdgeFillet* edge,
                                bool& ok)
{
  if (edge)
  {
    CatFilletEdgePropagation edge_propagation;
    if (SUCCEEDED(edge->get_EdgePropagation(edge_propagation)))
      AddEnumParameter(data, "edge_propagation", static_cast<int>(edge_propagation),
                       "CATIAEdgeFillet.get_EdgePropagation");
    else AddUnavailableParameter(data, "edge_propagation", "enum_raw",
                                 "CATIAEdgeFillet.get_EdgePropagation", "VALUE_READ_FAILED");
    CaaInterfaceGuard<CATIAReferences> keep_guard;
    if (SUCCEEDED(edge->get_EdgesToKeep(keep_guard.Out())))
      AddReferenceCollectionField(data, "edges_to_keep", keep_guard.Get(),
                                  "CATIAEdgeFillet.get_EdgesToKeep", false, ok);
  }
  if (fillet)
  {
    CatFilletBoundaryRelimitation boundary;
    CatFilletTrimSupport trim;
    if (SUCCEEDED(fillet->get_FilletBoundaryRelimitation(boundary)))
      AddEnumParameter(data, "boundary_relimitation", static_cast<int>(boundary),
                       "CATIAFillet.get_FilletBoundaryRelimitation");
    else AddUnavailableParameter(data, "boundary_relimitation", "enum_raw",
                                 "CATIAFillet.get_FilletBoundaryRelimitation",
                                 "VALUE_READ_FAILED");
    if (SUCCEEDED(fillet->get_FilletTrimSupport(trim)))
      AddEnumParameter(data, "trim_support", static_cast<int>(trim),
                       "CATIAFillet.get_FilletTrimSupport");
    else AddUnavailableParameter(data, "trim_support", "enum_raw",
                                 "CATIAFillet.get_FilletTrimSupport", "VALUE_READ_FAILED");
  }
}

// 用途：按 CATIAHole Automation 契约预分配三个 Variant，并读取原点或方向数组。
static bool ReadHoleVector(CATIAHole* hole, bool origin, double output[3])
{
  if (!hole) return false;
  CATSafeArrayVariant* array = SafeArrayCreateVector(VT_VARIANT, 0, 3);
  if (!array) return false;
  HRESULT read_result = E_FAIL;
  try
  {
    read_result = origin ? hole->GetOrigin(*array) : hole->GetDirection(*array);
  }
  catch (...)
  {
    SafeArrayDestroy(array);
    return false;
  }
  if (FAILED(read_result))
  {
    SafeArrayDestroy(array);
    return false;
  }
  CATVariant* values = 0;
  if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values))) || !values)
  {
    SafeArrayDestroy(array);
    return false;
  }
  bool valid = true;
  int index = 0;
  for (index = 0; index < 3; ++index)
    if (!VariantToDouble(values[index], output[index])) valid = false;
  SafeArrayUnaccessData(array);
  SafeArrayDestroy(array);
  return valid;
}

// 用途：按 CATIAPrism Automation 契约读取绝对方向数组。
static bool ReadPrismDirection(CATIAPrism* prism, double output[3])
{
  if (!prism) return false;
  CATSafeArrayVariant* array = SafeArrayCreateVector(VT_VARIANT, 0, 3);
  if (!array) return false;
  HRESULT read_result = E_FAIL;
  try { read_result = prism->GetDirection(*array); }
  catch (...)
  {
    SafeArrayDestroy(array);
    return false;
  }
  if (FAILED(read_result))
  {
    SafeArrayDestroy(array);
    return false;
  }
  CATVariant* values = 0;
  if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values))) || !values)
  {
    SafeArrayDestroy(array);
    return false;
  }
  bool valid = true;
  int index = 0;
  for (index = 0; index < 3; ++index)
    if (!VariantToDouble(values[index], output[index])) valid = false;
  SafeArrayUnaccessData(array);
  SafeArrayDestroy(array);
  return valid;
}

// 用途：供 Prism 终止边界读取复用已有 Limit 枚举映射。
static std::string LimitModeName(CatLimitMode mode, bool& known);

// 用途：把 R21 CatHoleType 的真实原始枚举映射为稳定 Schema 名称。
static std::string HoleTypeName(CatHoleType type, bool& known)
{
  known = true;
  if (type == catSimpleHole) return "simple";
  if (type == catTaperedHole) return "tapered";
  if (type == catCounterboredHole) return "counterbored";
  if (type == catCountersunkHole) return "countersunk";
  if (type == catCounterdrilledHole) return "counterdrilled";
  known = false;
  return "unknown";
}

// 用途：把 R21 CatPrismExtrusionDirection 映射为稳定 Schema 名称，同时保留 raw 枚举。
static std::string PrismDirectionTypeName(CatPrismExtrusionDirection type, bool& known)
{
  known = true;
  if (type == catNormalToSketchDirection) return "normal_to_sketch";
  if (type == catNotNormalToSketchDirection) return "not_normal_to_sketch";
  known = false;
  return "unknown";
}

// 用途：把 R21 CatPrismOrientation 映射为稳定 Schema 名称，同时保留 raw 枚举。
static std::string PrismOrientationName(CatPrismOrientation orientation, bool& known)
{
  known = true;
  if (orientation == catRegularOrientation) return "regular";
  if (orientation == catInverseOrientation) return "inverse";
  known = false;
  return "unknown";
}

// 用途：读取 Prism 的一个终止边界；非 Offset 终止不伪造尺寸。
static bool ReadPrismLimit(CATIALimit* limit, NativePrismLimitData& output, std::string& error)
{
  if (!limit)
  {
    error = "CATIALimit is null";
    return false;
  }
  CatLimitMode mode = catOffsetLimit;
  if (FAILED(limit->get_LimitMode(mode)))
  {
    error = "CATIALimit.LimitMode read failed";
    return false;
  }
  bool known_mode = false;
  output.mode_raw = static_cast<int>(mode);
  output.mode = LimitModeName(mode, known_mode);
  output.dimension_mm.Clear(mode == catOffsetLimit ? "unavailable" : "not_applicable");
  output.limiting_element_status = "not_attempted";
  if (!known_mode) return true;
  if (mode == catOffsetLimit)
  {
    CaaInterfaceGuard<CATIALength> dimension_guard;
    double dimension = 0.0;
    if (FAILED(limit->get_Dimension(dimension_guard.Out())) || !dimension_guard.Get())
    {
      error = "CATIALimit.Dimension read failed";
      return false;
    }
    if (!ReadLengthValue(dimension_guard.Get(), dimension))
    {
      error = "CATIALimit.Dimension.Value read failed";
      return false;
    }
    output.dimension_mm.Set(dimension, "success");
    output.limiting_element_status = "not_applicable";
  }
  else
  {
    output.dimension_mm.Clear("not_applicable");
    output.limiting_element_status = "supported_but_not_resolved_to_ir";
  }
  return true;
}

// 用途：把 R21 CatLimitMode 的真实原始枚举映射为稳定 Schema 名称。
static std::string LimitModeName(CatLimitMode mode, bool& known)
{
  known = true;
  if (mode == catOffsetLimit) return "offset";
  if (mode == catUpToNextLimit) return "up_to_next";
  if (mode == catUpToLastLimit) return "up_to_last";
  if (mode == catUpToPlaneLimit) return "up_to_plane";
  if (mode == catUpToSurfaceLimit) return "up_to_surface";
  if (mode == catUpThruNextLimit) return "up_thru_next";
  known = false;
  return "unknown";
}

// 用途：创建未打开的 SessionGuard，并选定本 Batch 使用的稳定 Session 名称。
SessionGuard::SessionGuard() : _open(false), _name("CadParseMvpSession") {}

// 用途：在作用域结束时删除已成功创建的 CATIA Session。
SessionGuard::~SessionGuard()
{
  if (_open)
    Delete_Session(const_cast<char*>(_name.c_str()));
}

// 用途：调用本机 R21 Create_Session 初始化 CAA 运行环境。
// 成功后 _open 变为 true；失败时不取得清理责任，并在 error 中返回文档级原因。
bool SessionGuard::Open(std::string& error)
{
  CATSession* session = 0;
  const HRESULT result = Create_Session(const_cast<char*>(_name.c_str()), session);
  if (FAILED(result) || !session)
  {
    error = "CAA session initialization failed";
    return false;
  }
  _open = true;
  return true;
}

// 用途：创建空 DocumentGuard；0 表示当前不持有 CATDocument。
DocumentGuard::DocumentGuard() : _document(0) {}

// 用途：若文档已打开，则通过 CATDocumentServices::Remove 关闭并释放它。
DocumentGuard::~DocumentGuard()
{
  if (_document)
  {
    CATDocumentServices::Remove(*_document);
    _document = 0;
  }
}

// 用途：以不区分大小写方式检查路径是否以 .CATPart 结尾。
static bool EndsWithCatPart(const std::string& path)
{
  if (path.size() < 8) return false;
  std::string suffix = path.substr(path.size() - 8);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
  return suffix == ".catpart";
}

static bool EndsWithCatProduct(const std::string& path)
{
  if (path.size() < 11) return false;
  std::string suffix = path.substr(path.size() - 11);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
  return suffix == ".catproduct";
}

// 用途：先校验文件存在性和扩展名，再通过 R21 文档服务以只读标志打开 CATPart/CATProduct。
// 成功时本守卫取得 _document 的关闭责任；失败返回 false 且不会留下半打开文档。
bool DocumentGuard::OpenReadOnly(const std::string& path, std::string& error)
{
  struct _stat file_status;
  if (_stat(path.c_str(), &file_status) != 0)
  {
    error = "input file does not exist";
    return false;
  }
  if (!EndsWithCatPart(path) && !EndsWithCatProduct(path))
  {
    error = "input file is not a CATPart or CATProduct";
    return false;
  }
  const CATUnicodeString storage_name(path.c_str());
  const HRESULT result = CATDocumentServices::OpenDocument(storage_name, _document, TRUE);
  if (FAILED(result) || !_document)
  {
    error = "CATPart open failed";
    return false;
  }
  return true;
}

// 用途：返回当前文档的借用指针；所有权仍属于 DocumentGuard。
CATDocument* DocumentGuard::Get() const { return _document; }

static std::string MakeProductReferenceId(const std::string& part_number)
{
  std::ostringstream id;
  id << "PRDREF_";
  if (part_number.empty()) id << "unnamed";
  else
  {
    std::string::const_iterator ch = part_number.begin();
    for (; ch != part_number.end(); ++ch)
    {
      if ((*ch >= 'A' && *ch <= 'Z') || (*ch >= 'a' && *ch <= 'z') ||
          (*ch >= '0' && *ch <= '9') || *ch == '_' || *ch == '-')
        id << *ch;
      else
        id << '_';
    }
  }
  return id.str();
}

static bool HasProductReference(const ParseContext& context, const std::string& reference_id)
{
  std::vector<ProductReferenceRecord>::const_iterator record =
    context.product_references.begin();
  for (; record != context.product_references.end(); ++record)
    if (record->reference_id == reference_id) return true;
  return false;
}

static void AddProductReference(CATIProduct* product, ParseContext& context,
                                const std::string& reference_id,
                                const std::string& source_document)
{
  if (!product || HasProductReference(context, reference_id)) return;
  ProductReferenceRecord record;
  record.reference_id = reference_id;
  record.source_document = source_document;
  record.read_status = "partial";
  record.value_source = "CATIProduct";
  try
  {
    record.part_number = UnicodeToUtf8(product->GetPartNumber());
    record.display_name = record.part_number;
    record.child_count = product->GetChildrenCount();
    CATUnicodeString rep_name;
    if (SUCCEEDED(product->GetDefaultRepName(rep_name, CATPrd3D, TRUE)))
      record.default_representation = UnicodeToUtf8(rep_name);
    if (!record.default_representation.empty()) record.representation_count = 1;
  }
  catch (...)
  {
    record.diagnostic_ids.push_back(context.AddDiagnostic(
      "warning", "product", "PRODUCT_REFERENCE_READ_EXCEPTION",
      "CATIProduct reference attribute read raised an exception", reference_id));
  }
  context.product_references.push_back(record);
}

static bool MountProductReferenceSpecTree(CATIProduct* instance_product,
                                          CATIProduct* reference_product,
                                          ParseContext& context,
                                          const ProductInstanceRecord& instance);

static bool ReadProductAbsTransform(CATIProduct* product, ProductInstanceRecord& instance,
                                    ParseContext& context)
{
  if (!product) return false;
  CATIMovable* movable = 0;
  if (FAILED(product->QueryInterface(IID_CATIMovable, reinterpret_cast<void**>(&movable))) ||
      !movable)
  {
    instance.diagnostic_ids.push_back(context.AddDiagnostic(
      "warning", "product", "PRODUCT_MOVABLE_UNAVAILABLE",
      "CATIProduct instance does not expose CATIMovable", instance.instance_id));
    return false;
  }
  CaaInterfaceGuard<CATIMovable> movable_guard(movable);
  CATMathTransformation position;
  if (FAILED(movable->GetAbsPosition(position)))
  {
    instance.diagnostic_ids.push_back(context.AddDiagnostic(
      "warning", "product", "PRODUCT_ABS_POSITION_FAILED",
      "CATIMovable::GetAbsPosition failed", instance.instance_id));
    return false;
  }
  double coeff[12];
  if (FAILED(position.GetCoefficients(coeff, 12)))
  {
    instance.diagnostic_ids.push_back(context.AddDiagnostic(
      "warning", "product", "PRODUCT_TRANSFORM_COEFFICIENTS_FAILED",
      "CATMathTransformation::GetCoefficients failed", instance.instance_id));
    return false;
  }
  instance.transform_4x4.clear();
  instance.transform_4x4.push_back(coeff[0]);
  instance.transform_4x4.push_back(coeff[3]);
  instance.transform_4x4.push_back(coeff[6]);
  instance.transform_4x4.push_back(coeff[9]);
  instance.transform_4x4.push_back(coeff[1]);
  instance.transform_4x4.push_back(coeff[4]);
  instance.transform_4x4.push_back(coeff[7]);
  instance.transform_4x4.push_back(coeff[10]);
  instance.transform_4x4.push_back(coeff[2]);
  instance.transform_4x4.push_back(coeff[5]);
  instance.transform_4x4.push_back(coeff[8]);
  instance.transform_4x4.push_back(coeff[11]);
  instance.transform_4x4.push_back(0.0);
  instance.transform_4x4.push_back(0.0);
  instance.transform_4x4.push_back(0.0);
  instance.transform_4x4.push_back(1.0);
  instance.transform_status = "resolved_absolute";
  instance.transform_value_source = "CATIMovable::GetAbsPosition";
  return true;
}

static void CrawlProductInstance(CATIProduct* product, ParseContext& context,
                                 const std::string& source_document,
                                 const std::string& parent_instance_id,
                                 const std::string& parent_path,
                                 long depth, long child_index)
{
  if (!product) return;
  std::string instance_name;
  try
  {
    CATUnicodeString name;
    if (SUCCEEDED(product->GetPrdInstanceName(name))) instance_name = UnicodeToUtf8(name);
  }
  catch (...) {}
  CATIProduct_var reference = product->GetReferenceProduct();
  CATIProduct* reference_ptr = reference;
  if (!reference_ptr) reference_ptr = product;
  std::string part_number;
  try { part_number = UnicodeToUtf8(reference_ptr->GetPartNumber()); }
  catch (...) { part_number = ""; }
  const std::string reference_id = MakeProductReferenceId(part_number);
  AddProductReference(reference_ptr, context, reference_id, source_document);

  ProductInstanceRecord instance;
  std::ostringstream id;
  id << "PRDINS_";
  if (context.product_instances.size() + 1 < 10) id << "00000";
  else if (context.product_instances.size() + 1 < 100) id << "0000";
  else if (context.product_instances.size() + 1 < 1000) id << "000";
  else if (context.product_instances.size() + 1 < 10000) id << "00";
  else if (context.product_instances.size() + 1 < 100000) id << "0";
  id << (context.product_instances.size() + 1);
  instance.instance_id = id.str();
  instance.parent_instance_id = parent_instance_id;
  instance.reference_id = reference_id;
  instance.instance_name = instance_name.empty() ? part_number : instance_name;
  instance.depth = depth;
  instance.child_index = child_index;
  instance.read_status = "partial";
  instance.value_source = "CATIProduct";
  instance.transform_status = depth == 0 ? "identity_root" : "not_resolved_from_public_interface";
  instance.transform_value_source = depth == 0 ? "CATProduct_root_identity" : "not_available";
  instance.tree_path = parent_path.empty() ? instance.instance_name : parent_path + "/" + instance.instance_name;
  ReadProductAbsTransform(product, instance, context);
  try { instance.child_count = product->GetChildrenCount(); }
  catch (...) { instance.child_count = 0; }
  MountProductReferenceSpecTree(product, reference_ptr, context, instance);
  context.product_instances.push_back(instance);
  const std::string current_id = instance.instance_id;

  CATListValCATBaseUnknown_var* children = 0;
  try { children = product->GetChildren("CATIProduct"); }
  catch (...) { children = 0; }
  if (!children)
  {
    if (instance.child_count > 0)
      context.AddDiagnostic("warning", "product", "PRODUCT_CHILD_LIST_UNAVAILABLE",
                            "CATIProduct::GetChildren returned null", current_id);
    return;
  }
  const int count = children->Size();
  int index = 1;
  for (; index <= count; ++index)
  {
    CATBaseUnknown_var child_unknown = (*children)[index];
    CATBaseUnknown* child_base = child_unknown;
    if (!child_base) continue;
    CATIProduct* child_product = 0;
    if (SUCCEEDED(child_base->QueryInterface(IID_CATIProduct,
        reinterpret_cast<void**>(&child_product))) && child_product)
    {
      CaaInterfaceGuard<CATIProduct> child_guard(child_product);
      CrawlProductInstance(child_product, context, source_document, current_id,
                           instance.tree_path, depth + 1, index);
    }
  }
  delete children;
}

static bool CollectProductStructure(CATDocument* document, ParseContext& context,
                                    const std::string& source_document)
{
  if (!document) return false;
  CATIDocRoots* doc_roots = 0;
  if (FAILED(document->QueryInterface(IID_CATIDocRoots, reinterpret_cast<void**>(&doc_roots))) ||
      !doc_roots)
    return false;
  CaaInterfaceGuard<CATIDocRoots> roots_guard(doc_roots);
  CATListValCATBaseUnknown_var* roots = 0;
  try { roots = doc_roots->GiveDocRoots(); }
  catch (...) { roots = 0; }
  if (!roots || roots->Size() == 0)
  {
    delete roots;
    return false;
  }
  CATBaseUnknown_var root_unknown = (*roots)[1];
  delete roots;
  CATBaseUnknown* root_base = root_unknown;
  if (!root_base) return false;
  CATIProduct* product = 0;
  if (FAILED(root_base->QueryInterface(IID_CATIProduct,
      reinterpret_cast<void**>(&product))) || !product)
    return false;
  CaaInterfaceGuard<CATIProduct> product_guard(product);
  CrawlProductInstance(product, context, source_document, "", "", 0, 0);
  context.runtime_info["product_structure_status"] =
    context.product_instances.empty() ? "not_available" : "partial";
  return !context.product_instances.empty();
}

// 适配没有 CATISpecObject 的静态节点，例如文档和容器入口。
class StaticObjectView : public INativeObjectView
{
public:
  // 用途：从已知常量和名称构造一个纯数据类型指纹。
  StaticObjectView(const char* native_type, const char* kind, const std::string& name)
  {
    _fingerprint.native_type = native_type;
    _fingerprint.container_kind = kind;
    _fingerprint.internal_name = name;
    _fingerprint.display_name = name;
  }

  // 用途：返回本视图持有的类型指纹只读引用。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：为静态节点补充通用 object_kind 属性；该操作不会访问 CAA 对象，因此总是成功。
  bool ReadBasicAttributes(FeatureRecord& output, std::string&) const
  {
    output.attributes["object_kind"] = _fingerprint.container_kind;
    return true;
  }

private:
  TypeFingerprint _fingerprint;
};

// CATISpecObject 的只读适配器；借用原生指针，只把可验证字段复制到 TypeFingerprint/IR。
class SpecObjectView : public INativeObjectView, public IStringParameterView,
                       public INativeHoleView, public INativeFeatureParameterView
{
private:
  // Pad/Pocket 使用独立能力代理，避免同一个 SpecObjectView 同时返回多个互斥 CapabilityId。
  class PrismCapabilityView : public INativePrismView
  {
  public:
    // 用途：先创建空代理，宿主构造函数体内再绑定，避免 VS2008 禁止在初始化列表传 this。
    PrismCapabilityView() : _owner(0), _capability_id("") {}
    // 用途：绑定宿主对象和稳定能力编号；宿主负责实际 CAA 查询。
    void Bind(const SpecObjectView* owner, const char* capability_id)
    {
      _owner = owner;
      _capability_id = capability_id;
    }
    // 用途：返回 NativePad 或 NativePocket，供 Decoder 严格确认能力来源。
    const char* GetCapabilityId() const { return _capability_id; }
    // 用途：把读取请求转交给宿主 SpecObjectView，不保存任何 CAA 指针。
    NativePrismReadStatus ReadNativePrism(const char* requested_capability,
                                          NativePrismData& output,
                                          std::string& error) const
    {
      if (!_owner || std::strcmp(requested_capability, _capability_id) != 0)
      {
        error = "Native Prism capability mismatch";
        return NativePrismInterfaceUnsupported;
      }
      return _owner->ReadNativePrism(requested_capability, output, error);
    }

  private:
    const SpecObjectView* _owner;
    const char* _capability_id;
  };

public:
  // 用途：绑定一个借用 CATISpecObject，并立即构建稳定类型指纹。
  // context 用于记录指纹读取和接口探测产生的诊断/统计。
  SpecObjectView(CATISpecObject* spec, ParseContext& context)
    : _spec(spec)
  {
    _pad_capability.Bind(this, "NativePad");
    _pocket_capability.Bind(this, "NativePocket");
    BuildFingerprint(context);
  }

  // 用途：返回构造阶段已经复制完成的类型指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：向参数 Decoder 暴露本适配器已有的 IStringParameterView，不依赖 /GR RTTI。
  const IStringParameterView* GetStringParameterView() const { return this; }

  // 用途：按能力标识暴露 CAA 原生适配器；新增能力不需要修改 Crawler 或对象视图接口。
  const INativeCapabilityView* FindCapability(const char* capability_id) const
  {
    if (!capability_id) return 0;
    if (std::string(capability_id) == "NativeHole")
      return static_cast<const INativeHoleView*>(this);
    if (std::string(capability_id) == "NativePad") return &_pad_capability;
    if (std::string(capability_id) == "NativePocket") return &_pocket_capability;
    if (std::string(capability_id) == "NativeFeatureParameters")
      return static_cast<const INativeFeatureParameterView*>(this);
    return 0;
  }

  // 用途：读取经过 R21 PublicInterfaces 验证的基础状态和容器可访问性。
  // 任意 CAA 异常都转成 false+error，由 Registry 的 Generic/Opaque 链隔离。
  bool ReadBasicAttributes(FeatureRecord& output, std::string& error) const
  {
    if (!_spec)
    {
      error = "null CATISpecObject";
      return false;
    }
    try
    {
      output.update_status = _spec->IsUpToDate() ? "up_to_date" : "not_up_to_date";
      const CATIContainer_var container = _spec->GetFeatContainer();
      output.attributes["container_accessible"] = container == NULL_var ? "false" : "true";
      output.attributes["result_summary"] = "not_exposed_by_verified_mvp_interface";
      return true;
    }
    catch (...)
    {
      error = "CATISpecObject basic attribute read failed";
      return false;
    }
  }

  // 用途：查询 R21 Public CATICkeParm，验证其类型确为 String，再通过 Value()->AsString() 读取真实值。
  StringParameterReadStatus ReadStringParameter(ParameterValueData& parameter,
                                                std::string& error) const
  {
    if (!_spec)
    {
      error = "null CATISpecObject";
      return StringParameterInterfaceUnsupported;
    }
    CATICkeParm* raw_parameter = 0;
    try
    {
      const HRESULT query = _spec->QueryInterface(IID_CATICkeParm,
                                                   reinterpret_cast<void**>(&raw_parameter));
      if (FAILED(query) || !raw_parameter)
      {
        error = "CATICkeParm is not supported";
        return StringParameterInterfaceUnsupported;
      }
    }
    catch (...)
    {
      error = "CATICkeParm QueryInterface raised an exception";
      return StringParameterQueryException;
    }

    CaaInterfaceGuard<CATICkeParm> parameter_guard(raw_parameter);
    try
    {
      const CATICkeType_var parameter_type = raw_parameter->Type();
      if (parameter_type == NULL_var || static_cast<int>(parameter_type->IsaString()) == 0)
      {
        error = "CATICkeParm type is not String";
        return StringParameterInterfaceUnsupported;
      }
      const CATICkeInst_var value = raw_parameter->Value();
      if (value == NULL_var)
      {
        error = "CATICkeParm Value returned null";
        return StringParameterValueException;
      }
      parameter.parameter_kind = "string";
      parameter.value_status = "success";
      parameter.value_source = "typed_caa_value";
      parameter.value_text = UnicodeToUtf8(value->AsString());
    }
    catch (...)
    {
      error = "CATICkeParm typed String value read raised an exception";
      return StringParameterValueException;
    }
    // Name/Show/只读和隐藏状态是辅助信息；它们不可访问时不能否定已经成功取得的真实值。
    try { parameter.parameter_name = ParameterLeafName(UnicodeToUtf8(raw_parameter->Name())); }
    catch (...) { parameter.parameter_name = ParameterLeafName(_fingerprint.display_name); }
    try { parameter.raw_display_text = UnicodeToUtf8(raw_parameter->Show()); }
    catch (...) { parameter.raw_display_text.clear(); }
    try { parameter.is_read_only = static_cast<int>(raw_parameter->IsReadOnly()) == 0 ? "false" : "true"; }
    catch (...) { parameter.is_read_only = "unknown"; }
    try { parameter.is_hidden = static_cast<int>(raw_parameter->IsHidden()) == 0 ? "false" : "true"; }
    catch (...) { parameter.is_hidden = "unknown"; }
    return StringParameterReadSuccess;
  }

  // 用途：在当前 CATISpecObject 上直接查询 R21 Public CATIAHole，并读取真实设计参数。
  NativeHoleReadStatus ReadNativeHole(NativeHoleData& output, std::string& error) const
  {
    if (!_spec)
    {
      error = "null CATISpecObject";
      return NativeHoleInterfaceUnsupported;
    }
    CaaInterfaceGuard<CATIAHole> hole_guard;
    try
    {
      const HRESULT query = _spec->QueryInterface(IID_CATIAHole,
        reinterpret_cast<void**>(&hole_guard.Out()));
      if (FAILED(query) || !hole_guard.Get())
      {
        error = "CATIAHole is not supported";
        return NativeHoleInterfaceUnsupported;
      }
    }
    catch (...)
    {
      error = "CATIAHole QueryInterface raised an exception";
      return NativeHoleInterfaceQueryException;
    }

    CATIAHole* raw_hole = hole_guard.Get();
    output.semantic_kind = "part_design_hole";
    output.value_source = "typed_caa_value";
    output.interface_key = "CATIAHole";
    try
    {
      CatHoleType hole_type = catSimpleHole;
      if (FAILED(raw_hole->get_Type(hole_type)))
      {
        error = "CATIAHole.Type read failed";
        return NativeHoleRequiredValueReadException;
      }
      bool known_hole_type = false;
      output.hole_type_raw = static_cast<int>(hole_type);
      output.hole_type = HoleTypeName(hole_type, known_hole_type);
      output.field_status["hole_type"] = known_hole_type ? "success" : "unknown_enum";

      CaaInterfaceGuard<CATIALength> diameter_guard;
      if (FAILED(raw_hole->get_Diameter(diameter_guard.Out())) || !diameter_guard.Get())
      {
        error = "CATIAHole.Diameter interface read failed";
        return NativeHoleRequiredValueReadException;
      }
      if (!ReadLengthValue(diameter_guard.Get(), output.diameter_mm))
      {
        error = "CATIAHole.Diameter.Value read failed";
        return NativeHoleRequiredValueReadException;
      }
      output.field_status["diameter_mm"] = "success";

      if (!ReadHoleVector(raw_hole, true, output.origin_mm))
      {
        error = "CATIAHole.GetOrigin failed";
        return NativeHoleRequiredValueReadException;
      }
      if (!ReadHoleVector(raw_hole, false, output.direction))
      {
        error = "CATIAHole.GetDirection failed";
        return NativeHoleRequiredValueReadException;
      }
      output.field_status["origin_mm"] = "success";
      output.field_status["direction"] = "success";

      CaaInterfaceGuard<CATIALimit> limit_guard;
      if (FAILED(raw_hole->get_BottomLimit(limit_guard.Out())) || !limit_guard.Get())
      {
        error = "CATIAHole.BottomLimit read failed";
        return NativeHoleRequiredValueReadException;
      }
      CatLimitMode limit_mode = catOffsetLimit;
      if (FAILED(limit_guard.Get()->get_LimitMode(limit_mode)))
      {
        error = "CATIALimit.LimitMode read failed";
        return NativeHoleRequiredValueReadException;
      }
      bool known_limit_mode = false;
      output.bottom_limit.mode_raw = static_cast<int>(limit_mode);
      output.bottom_limit.mode = LimitModeName(limit_mode, known_limit_mode);
      output.field_status["bottom_limit.mode"] = known_limit_mode ? "success" : "unknown_enum";
      if (limit_mode == catOffsetLimit)
      {
        CaaInterfaceGuard<CATIALength> depth_guard;
        double depth = 0.0;
        if (FAILED(limit_guard.Get()->get_Dimension(depth_guard.Out())) || !depth_guard.Get())
        {
          error = "CATIALimit.Dimension read failed for offset Hole";
          return NativeHoleRequiredValueReadException;
        }
        if (!ReadLengthValue(depth_guard.Get(), depth))
        {
          error = "CATIALimit.Dimension.Value read failed for offset Hole";
          return NativeHoleRequiredValueReadException;
        }
        output.bottom_limit.depth_mm.Set(depth, "success");
      }
      else
        output.bottom_limit.depth_mm.Clear("not_applicable");

      output.head.kind = "none";
      output.head.diameter_mm.Clear("not_applicable");
      output.head.depth_mm.Clear("not_applicable");
      output.head.angle_deg.Clear("not_applicable");
      if (hole_type == catCounterboredHole || hole_type == catCounterdrilledHole)
      {
        CaaInterfaceGuard<CATIALength> head_diameter_guard;
        CaaInterfaceGuard<CATIALength> head_depth_guard;
        double head_diameter = 0.0;
        double head_depth = 0.0;
        if (FAILED(raw_hole->get_HeadDiameter(head_diameter_guard.Out())) ||
            !head_diameter_guard.Get())
        {
          error = "CATIAHole.HeadDiameter read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (!ReadLengthValue(head_diameter_guard.Get(), head_diameter))
        {
          error = "CATIAHole.HeadDiameter.Value read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (FAILED(raw_hole->get_HeadDepth(head_depth_guard.Out())) || !head_depth_guard.Get())
        {
          error = "CATIAHole.HeadDepth read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (!ReadLengthValue(head_depth_guard.Get(), head_depth))
        {
          error = "CATIAHole.HeadDepth.Value read failed";
          return NativeHoleRequiredValueReadException;
        }
        output.head.kind = hole_type == catCounterboredHole ? "counterbore" : "counterdrill";
        output.head.diameter_mm.Set(head_diameter, "success");
        output.head.depth_mm.Set(head_depth, "success");
      }
      if (hole_type == catTaperedHole || hole_type == catCounterdrilledHole ||
          hole_type == catCountersunkHole)
      {
        CaaInterfaceGuard<CATIAAngle> head_angle_guard;
        double head_angle = 0.0;
        if (FAILED(raw_hole->get_HeadAngle(head_angle_guard.Out())) || !head_angle_guard.Get())
        {
          error = "CATIAHole.HeadAngle read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (!ReadAngleValue(head_angle_guard.Get(), head_angle))
        {
          error = "CATIAHole.HeadAngle.Value read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (hole_type == catTaperedHole) output.head.kind = "taper";
        else if (hole_type == catCountersunkHole) output.head.kind = "countersink";
        output.head.angle_deg.Set(head_angle, "typed_caa_angle_value");
      }
      if (hole_type == catCountersunkHole)
      {
        CaaInterfaceGuard<CATIALength> head_depth_guard;
        double head_depth = 0.0;
        if (FAILED(raw_hole->get_HeadDepth(head_depth_guard.Out())) || !head_depth_guard.Get() ||
            !ReadLengthValue(head_depth_guard.Get(), head_depth))
        {
          error = "CATIAHole countersink HeadDepth read failed";
          return NativeHoleRequiredValueReadException;
        }
        output.head.depth_mm.Set(head_depth, "success");
      }

      CatHoleThreadingMode threading_mode = catSmoothHoleThreading;
      if (FAILED(raw_hole->get_ThreadingMode(threading_mode)))
      {
        error = "CATIAHole.ThreadingMode read failed";
        return NativeHoleRequiredValueReadException;
      }
      output.thread.mode_raw = static_cast<int>(threading_mode);
      output.thread.enabled = threading_mode == catThreadedHoleThreading;
      output.field_status["thread.mode"] =
        (threading_mode == catThreadedHoleThreading || threading_mode == catSmoothHoleThreading) ?
        "success" : "unknown_enum";
      if (!output.thread.enabled)
      {
        output.thread.description.Clear("not_applicable");
        output.thread.diameter_mm.Clear("not_applicable");
        output.thread.depth_mm.Clear("not_applicable");
        output.thread.pitch_mm.Clear("not_applicable");
      }
      else
      {
        CaaInterfaceGuard<CATIALength> thread_diameter_guard;
        CaaInterfaceGuard<CATIALength> thread_depth_guard;
        CaaInterfaceGuard<CATIALength> thread_pitch_guard;
        CaaInterfaceGuard<CATIAStrParam> description_guard;
        double thread_diameter = 0.0;
        double thread_depth = 0.0;
        double thread_pitch = 0.0;
        if (FAILED(raw_hole->get_ThreadDiameter(thread_diameter_guard.Out())) ||
            !thread_diameter_guard.Get())
        {
          error = "CATIAHole.ThreadDiameter read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (FAILED(raw_hole->get_ThreadDepth(thread_depth_guard.Out())) ||
            !thread_depth_guard.Get())
        {
          error = "CATIAHole.ThreadDepth read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (FAILED(raw_hole->get_ThreadPitch(thread_pitch_guard.Out())) ||
            !thread_pitch_guard.Get())
        {
          error = "CATIAHole.ThreadPitch read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (FAILED(raw_hole->get_HoleThreadDescription(description_guard.Out())) ||
            !description_guard.Get())
        {
          error = "CATIAHole.HoleThreadDescription read failed";
          return NativeHoleRequiredValueReadException;
        }
        if (!ReadLengthValue(thread_diameter_guard.Get(), thread_diameter) ||
            !ReadLengthValue(thread_depth_guard.Get(), thread_depth) ||
            !ReadLengthValue(thread_pitch_guard.Get(), thread_pitch))
        {
          error = "CATIAHole threaded numeric Value read failed";
          return NativeHoleRequiredValueReadException;
        }
        CaaBstrGuard description;
        if (FAILED(description_guard.Get()->get_Value(description.Out())))
        {
          error = "CATIAHole thread description Value read failed";
          return NativeHoleRequiredValueReadException;
        }
        const std::string description_utf8 = BstrToUtf8(description.Get());
        output.thread.diameter_mm.Set(thread_diameter, "success");
        output.thread.depth_mm.Set(thread_depth, "success");
        output.thread.pitch_mm.Set(thread_pitch, "success");
        output.thread.description.Set(description_utf8, "success");
      }

      CaaBstrGuard alias;
      if (SUCCEEDED(raw_hole->get_Name(alias.Out())))
      {
        output.has_automation_alias = true;
        output.automation_alias = BstrToUtf8(alias.Get());
        output.automation_alias_status = "success";
      }
      else
        output.automation_alias_status = "automation_alias_unavailable";
    }
    catch (...)
    {
      error = "CATIAHole required value read raised an exception";
      return NativeHoleRequiredValueReadException;
    }
    return NativeHoleReadSuccess;
  }

  // 用途：在当前 CATISpecObject 上查询 R21 Public CATIAPad/CATIAPocket，再通过 CATIAPrism 读取公共拉伸参数。
  NativePrismReadStatus ReadNativePrism(const char* requested_capability,
                                        NativePrismData& output,
                                        std::string& error) const
  {
    if (!_spec)
    {
      error = "null CATISpecObject";
      return NativePrismInterfaceUnsupported;
    }
    const bool wants_pad = requested_capability &&
      std::strcmp(requested_capability, "NativePad") == 0;
    const bool wants_pocket = requested_capability &&
      std::strcmp(requested_capability, "NativePocket") == 0;
    if (!wants_pad && !wants_pocket)
    {
      error = "unknown Native Prism capability";
      return NativePrismInterfaceUnsupported;
    }

    CaaInterfaceGuard<CATIAPrism> prism_guard;
    try
    {
      if (wants_pad)
      {
        CaaInterfaceGuard<CATIAPad> pad_guard;
        const HRESULT query = _spec->QueryInterface(IID_CATIAPad,
          reinterpret_cast<void**>(&pad_guard.Out()));
        if (FAILED(query) || !pad_guard.Get())
        {
          error = "CATIAPad is not supported";
          return NativePrismInterfaceUnsupported;
        }
        CATIAPrism* prism = 0;
        const HRESULT prism_query = pad_guard.Get()->QueryInterface(IID_CATIAPrism,
          reinterpret_cast<void**>(&prism));
        if (FAILED(prism_query) || !prism)
        {
          error = "CATIAPad does not expose CATIAPrism";
          return NativePrismRequiredValueReadException;
        }
        prism_guard.Out() = prism;
        output.semantic_kind = "part_design_pad";
        output.material_operation = "add_material";
        output.interface_key = "CATIAPad";
      }
      else
      {
        CaaInterfaceGuard<CATIAPocket> pocket_guard;
        const HRESULT query = _spec->QueryInterface(IID_CATIAPocket,
          reinterpret_cast<void**>(&pocket_guard.Out()));
        if (FAILED(query) || !pocket_guard.Get())
        {
          error = "CATIAPocket is not supported";
          return NativePrismInterfaceUnsupported;
        }
        CATIAPrism* prism = 0;
        const HRESULT prism_query = pocket_guard.Get()->QueryInterface(IID_CATIAPrism,
          reinterpret_cast<void**>(&prism));
        if (FAILED(prism_query) || !prism)
        {
          error = "CATIAPocket does not expose CATIAPrism";
          return NativePrismRequiredValueReadException;
        }
        prism_guard.Out() = prism;
        output.semantic_kind = "part_design_pocket";
        output.material_operation = "remove_material";
        output.interface_key = "CATIAPocket";
      }
    }
    catch (...)
    {
      error = wants_pad ? "CATIAPad QueryInterface raised an exception" :
        "CATIAPocket QueryInterface raised an exception";
      return NativePrismInterfaceQueryException;
    }

    CATIAPrism* raw_prism = prism_guard.Get();
    output.value_source = "typed_caa_value";
    try
    {
      CatPrismExtrusionDirection direction_type = catNormalToSketchDirection;
      if (FAILED(raw_prism->get_DirectionType(direction_type)))
      {
        error = "CATIAPrism.DirectionType read failed";
        return NativePrismRequiredValueReadException;
      }
      bool known_direction_type = false;
      output.direction_type_raw = static_cast<int>(direction_type);
      output.direction_type = PrismDirectionTypeName(direction_type, known_direction_type);
      output.field_status["direction_type"] =
        known_direction_type ? "success" : "unknown_enum";

      CatPrismOrientation orientation = catRegularOrientation;
      if (FAILED(raw_prism->get_DirectionOrientation(orientation)))
      {
        error = "CATIAPrism.DirectionOrientation read failed";
        return NativePrismRequiredValueReadException;
      }
      bool known_orientation = false;
      output.direction_orientation_raw = static_cast<int>(orientation);
      output.direction_orientation = PrismOrientationName(orientation, known_orientation);
      output.field_status["direction_orientation"] =
        known_orientation ? "success" : "unknown_enum";

      if (!ReadPrismDirection(raw_prism, output.direction))
      {
        error = "CATIAPrism.GetDirection failed";
        return NativePrismRequiredValueReadException;
      }
      output.field_status["direction"] = "success";

      CAT_VARIANT_BOOL value = FALSE;
      if (FAILED(raw_prism->get_IsSymmetric(value)))
      {
        error = "CATIAPrism.IsSymmetric read failed";
        return NativePrismRequiredValueReadException;
      }
      output.is_symmetric = value != FALSE;
      if (FAILED(raw_prism->get_IsThin(value)))
      {
        error = "CATIAPrism.IsThin read failed";
        return NativePrismRequiredValueReadException;
      }
      output.is_thin = value != FALSE;
      if (FAILED(raw_prism->get_NeutralFiber(value)))
      {
        output.neutral_fiber = false;
        output.field_status["neutral_fiber"] = "unavailable";
      }
      else
      {
        output.neutral_fiber = value != FALSE;
        output.field_status["neutral_fiber"] = "success";
      }
      if (FAILED(raw_prism->get_MergeEnd(value)))
      {
        output.merge_end = false;
        output.field_status["merge_end"] = "unavailable";
      }
      else
      {
        output.merge_end = value != FALSE;
        output.field_status["merge_end"] = "success";
      }
      output.field_status["is_symmetric"] = "success";
      output.field_status["is_thin"] = "success";

      CaaInterfaceGuard<CATIALimit> first_limit_guard;
      CaaInterfaceGuard<CATIALimit> second_limit_guard;
      if (FAILED(raw_prism->get_FirstLimit(first_limit_guard.Out())) ||
          !first_limit_guard.Get())
      {
        error = "CATIAPrism.FirstLimit read failed";
        return NativePrismRequiredValueReadException;
      }
      if (FAILED(raw_prism->get_SecondLimit(second_limit_guard.Out())) ||
          !second_limit_guard.Get())
      {
        error = "CATIAPrism.SecondLimit read failed";
        return NativePrismRequiredValueReadException;
      }
      if (!ReadPrismLimit(first_limit_guard.Get(), output.first_limit, error))
      {
        error = std::string("CATIAPrism.FirstLimit ") + error;
        return NativePrismRequiredValueReadException;
      }
      if (!ReadPrismLimit(second_limit_guard.Get(), output.second_limit, error))
      {
        error = std::string("CATIAPrism.SecondLimit ") + error;
        return NativePrismRequiredValueReadException;
      }
      output.field_status["first_limit"] = "success";
      output.field_status["second_limit"] = "success";
    }
    catch (...)
    {
      error = "CATIAPrism required value read raised an exception";
      return NativePrismRequiredValueReadException;
    }
    return NativePrismReadSuccess;
  }

  NativeFeatureParameterReadStatus ReadNativeFeatureParameters(const char* canonical_family,
                                                               NativeFeatureParameterData& output,
                                                               std::string& error) const
  {
    if (!_spec)
    {
      error = "null CATISpecObject";
      return NativeFeatureParameterInterfaceUnsupported;
    }
    const std::string family = canonical_family ? canonical_family : "";
    output.family = family;
    output.semantic_kind = "part_design_native_feature_parameters";
    output.value_source = "typed_caa_value";
    output.decode_status = "partial";
    output.reason_code = "PARTIAL_PUBLIC_API_PAYLOAD";
    bool ok = true;
    bool interface_supported = false;

    try
    {
      if (family == "plane")
      {
        CaaInterfaceGuard<CATIMechanicalFeature> mechanical_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIMechanicalFeature,
            reinterpret_cast<void**>(&mechanical_guard.Out()))) || !mechanical_guard.Get())
        {
          error = "CATIMechanicalFeature is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        CATISpecObject_var part_spec = NULL_var;
        try { part_spec = mechanical_guard.Get()->GetPart(); }
        catch (...)
        {
          error = "CATIMechanicalFeature.GetPart raised an exception";
          return NativeFeatureParameterInterfaceQueryException;
        }
        if (part_spec == NULL_var)
        {
          error = "CATIMechanicalFeature.GetPart returned null";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        CaaInterfaceGuard<CATIPrtPart> part_guard;
        if (FAILED(part_spec->QueryInterface(IID_CATIPrtPart,
            reinterpret_cast<void**>(&part_guard.Out()))) || !part_guard.Get())
        {
          error = "Feature part does not expose CATIPrtPart";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        CATListValCATISpecObject_var reference_planes = part_guard.Get()->GetReferencePlanes();
        int reference_index = 0;
        int plane_index = 1;
        for (; plane_index <= reference_planes.Size(); ++plane_index)
        {
          CATISpecObject_var candidate = reference_planes[plane_index];
          CATBaseUnknown* candidate_base = candidate;
          if (candidate_base && _spec->IsEqual(candidate_base))
          {
            reference_index = plane_index;
            break;
          }
        }
        if (reference_index < 1 || reference_index > 3)
        {
          error = "GSMPlane is not one of CATIPrtPart.GetReferencePlanes";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIMechanicalFeature+CATIPrtPart.GetReferencePlanes";
        output.semantic_kind = "reference_plane";
        double origin_xyz[3] = { 0.0, 0.0, 0.0 };
        double u_xyz[3] = { 1.0, 0.0, 0.0 };
        double v_xyz[3] = { 0.0, 1.0, 0.0 };
        double normal_xyz[3] = { 0.0, 0.0, 1.0 };
        if (reference_index == 2)
        {
          u_xyz[0] = 0.0; u_xyz[1] = 1.0; u_xyz[2] = 0.0;
          v_xyz[0] = 0.0; v_xyz[1] = 0.0; v_xyz[2] = 1.0;
          normal_xyz[0] = 1.0; normal_xyz[1] = 0.0; normal_xyz[2] = 0.0;
        }
        else if (reference_index == 3)
        {
          u_xyz[0] = 0.0; u_xyz[1] = 0.0; u_xyz[2] = 1.0;
          v_xyz[0] = 1.0; v_xyz[1] = 0.0; v_xyz[2] = 0.0;
          normal_xyz[0] = 0.0; normal_xyz[1] = 1.0; normal_xyz[2] = 0.0;
        }
        AddEnumParameter(output, "reference_plane_index", reference_index,
                         "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "origin_x", origin_xyz[0], "length", "mm", "CATIPrtPart.GetReferencePlanes");
        AddDoubleParameter(output, "origin_y", origin_xyz[1], "length", "mm", "CATIPrtPart.GetReferencePlanes");
        AddDoubleParameter(output, "origin_z", origin_xyz[2], "length", "mm", "CATIPrtPart.GetReferencePlanes");
        AddDoubleParameter(output, "u_direction_x", u_xyz[0], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "u_direction_y", u_xyz[1], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "u_direction_z", u_xyz[2], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "v_direction_x", v_xyz[0], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "v_direction_y", v_xyz[1], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "v_direction_z", v_xyz[2], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "normal_x", normal_xyz[0], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "normal_y", normal_xyz[1], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
        AddDoubleParameter(output, "normal_z", normal_xyz[2], "unit_vector_component", "", "CATIPrtPart.GetReferencePlanes documented order");
      }
      else if (family == "draft")
      {
        CaaInterfaceGuard<CATIADraft> draft_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIADraft,
            reinterpret_cast<void**>(&draft_guard.Out()))) || !draft_guard.Get())
        {
          error = "CATIADraft is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIADraft";
        CatDraftMode mode;
        if (SUCCEEDED(draft_guard.Get()->get_Mode(mode)))
          AddEnumParameter(output, "mode", static_cast<int>(mode), "CATIADraft.get_Mode");
        else { ok = false; AddUnavailableParameter(output, "mode", "enum_raw",
                                                   "CATIADraft.get_Mode", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIAReference> parting_guard;
        if (SUCCEEDED(draft_guard.Get()->get_PartingElement(parting_guard.Out())))
          AddSingleReferenceField(output, "parting_element", parting_guard.Get(),
                                  "CATIADraft.get_PartingElement", false, ok);

        CaaInterfaceGuard<CATIADraftDomains> domains_guard;
        if (FAILED(draft_guard.Get()->get_DraftDomains(domains_guard.Out())) ||
            !domains_guard.Get())
        {
          ok = false;
          AddUnavailableParameter(output, "draft_domain_count", "integer",
                                  "CATIADraft.get_DraftDomains", "VALUE_READ_FAILED");
        }
        else
        {
          CATLONG domain_count = 0;
          if (SUCCEEDED(domains_guard.Get()->get_Count(domain_count)))
            AddLongParameter(output, "draft_domain_count", static_cast<long>(domain_count),
                             "CATIADraftDomains.get_Count");
          else
          {
            ok = false;
            AddUnavailableParameter(output, "draft_domain_count", "integer",
                                    "CATIADraftDomains.get_Count", "VALUE_READ_FAILED");
          }
          if (domain_count <= 0) ok = false;
          CATLONG domain_index = 1;
          for (; domain_index <= domain_count; ++domain_index)
          {
            CATVariant index;
            VariantInit(&index);
            V_VT(&index) = VT_I4;
            V_I4(&index) = domain_index;
            CaaInterfaceGuard<CATIADraftDomain> domain_guard;
            if (FAILED(domains_guard.Get()->Item(index, domain_guard.Out())) ||
                !domain_guard.Get())
            {
              ok = false;
              continue;
            }
            std::ostringstream prefix;
            prefix << "domain_" << domain_index << "_";
            CaaInterfaceGuard<CATIAAngle> draft_angle_guard;
            if (SUCCEEDED(domain_guard.Get()->get_DraftAngle(draft_angle_guard.Out())))
              AddAngleParameter(output, (prefix.str() + "draft_angle").c_str(),
                                draft_angle_guard.Get(),
                                "CATIADraftDomain.get_DraftAngle.Value", ok);
            else { ok = false; AddUnavailableParameter(output,
                                                       (prefix.str() + "draft_angle").c_str(),
                                                       "angle", "CATIADraftDomain.get_DraftAngle",
                                                       "VALUE_READ_FAILED"); }
            CaaInterfaceGuard<CATIAReferences> faces_guard;
            if (SUCCEEDED(domain_guard.Get()->get_FacesToDraft(faces_guard.Out())))
              AddReferenceCollectionField(output, (prefix.str() + "faces_to_draft").c_str(),
                                          faces_guard.Get(),
                                          "CATIADraftDomain.get_FacesToDraft", true, ok);
            else { ok = false; AddReferenceCollectionField(output,
                                                           (prefix.str() + "faces_to_draft").c_str(),
                                                           0, "CATIADraftDomain.get_FacesToDraft",
                                                           true, ok); }
            CaaInterfaceGuard<CATIAReference> neutral_guard;
            if (SUCCEEDED(domain_guard.Get()->get_NeutralElement(neutral_guard.Out())))
              AddSingleReferenceField(output, (prefix.str() + "neutral_element").c_str(),
                                      neutral_guard.Get(),
                                      "CATIADraftDomain.get_NeutralElement", true, ok);
            else { ok = false; AddSingleReferenceField(output,
                                                       (prefix.str() + "neutral_element").c_str(),
                                                       0, "CATIADraftDomain.get_NeutralElement",
                                                       true, ok); }
            CatDraftNeutralPropagationMode neutral_mode;
            if (SUCCEEDED(domain_guard.Get()->get_NeutralPropagationMode(neutral_mode)))
              AddEnumParameter(output, (prefix.str() + "neutral_propagation_mode").c_str(),
                               static_cast<int>(neutral_mode),
                               "CATIADraftDomain.get_NeutralPropagationMode");
            CatDraftMultiselectionMode multisel_mode;
            if (SUCCEEDED(domain_guard.Get()->get_MultiselectionMode(multisel_mode)))
              AddEnumParameter(output, (prefix.str() + "multiselection_mode").c_str(),
                               static_cast<int>(multisel_mode),
                               "CATIADraftDomain.get_MultiselectionMode");
            CATSafeArrayVariant* pulling = SafeArrayCreateVector(VT_VARIANT, 0, 3);
            if (pulling && SUCCEEDED(domain_guard.Get()->GetPullingDirection(*pulling)))
            {
              CATVariant* values = 0;
              if (SUCCEEDED(SafeArrayAccessData(pulling, reinterpret_cast<void**>(&values))) &&
                  values)
              {
                int coord = 0;
                for (; coord < 3; ++coord)
                {
                  double component = 0.0;
                  if (VariantToDouble(values[coord], component))
                  {
                    const char* suffix = coord == 0 ? "x" : (coord == 1 ? "y" : "z");
                    AddDoubleParameter(output, (prefix.str() + "pulling_direction_" + suffix).c_str(),
                                       component, "unit_vector_component", "",
                                       "CATIADraftDomain.GetPullingDirection");
                  }
                  else ok = false;
                }
                SafeArrayUnaccessData(pulling);
              }
              else ok = false;
            }
            else ok = false;
            if (pulling) SafeArrayDestroy(pulling);
            CaaInterfaceGuard<CATIAReference> pulling_element_guard;
            if (SUCCEEDED(domain_guard.Get()->get_PullingDirectionElement(pulling_element_guard.Out())))
              AddSingleReferenceField(output, (prefix.str() + "pulling_direction_element").c_str(),
                                      pulling_element_guard.Get(),
                                      "CATIADraftDomain.get_PullingDirectionElement",
                                      false, ok);
          }
        }
      }
      else if (family == "chamfer")
      {
        CaaInterfaceGuard<CATIAChamfer> chamfer_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIAChamfer,
            reinterpret_cast<void**>(&chamfer_guard.Out()))) || !chamfer_guard.Get())
        {
          error = "CATIAChamfer is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIAChamfer";
        CatChamferMode mode;
        CatChamferPropagation propagation;
        CatChamferOrientation orientation;
        if (SUCCEEDED(chamfer_guard.Get()->get_Mode(mode)))
          AddEnumParameter(output, "mode", static_cast<int>(mode), "CATIAChamfer.get_Mode");
        else { ok = false; AddUnavailableParameter(output, "mode", "enum_raw", "CATIAChamfer.get_Mode", "VALUE_READ_FAILED"); }
        if (SUCCEEDED(chamfer_guard.Get()->get_Propagation(propagation)))
          AddEnumParameter(output, "propagation", static_cast<int>(propagation), "CATIAChamfer.get_Propagation");
        else { ok = false; AddUnavailableParameter(output, "propagation", "enum_raw", "CATIAChamfer.get_Propagation", "VALUE_READ_FAILED"); }
        if (SUCCEEDED(chamfer_guard.Get()->get_Orientation(orientation)))
          AddEnumParameter(output, "orientation", static_cast<int>(orientation), "CATIAChamfer.get_Orientation");
        else { ok = false; AddUnavailableParameter(output, "orientation", "enum_raw", "CATIAChamfer.get_Orientation", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIALength> length1_guard;
        CaaInterfaceGuard<CATIALength> length2_guard;
        CaaInterfaceGuard<CATIAAngle> angle_guard;
        if (SUCCEEDED(chamfer_guard.Get()->get_Length1(length1_guard.Out())))
          AddLengthParameter(output, "length1", length1_guard.Get(), "CATIAChamfer.get_Length1.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "length1", "length", "CATIAChamfer.get_Length1", "VALUE_READ_FAILED"); }
        if (SUCCEEDED(chamfer_guard.Get()->get_Length2(length2_guard.Out())))
          AddLengthParameter(output, "length2", length2_guard.Get(), "CATIAChamfer.get_Length2.Value", ok);
        else AddUnavailableParameter(output, "length2", "length", "CATIAChamfer.get_Length2", "VALUE_READ_FAILED");
        if (SUCCEEDED(chamfer_guard.Get()->get_Angle(angle_guard.Out())))
          AddAngleParameter(output, "angle", angle_guard.Get(), "CATIAChamfer.get_Angle.Value", ok);
        else AddUnavailableParameter(output, "angle", "angle", "CATIAChamfer.get_Angle", "VALUE_READ_FAILED");
        CaaInterfaceGuard<CATIAReferences> refs_guard;
        if (SUCCEEDED(chamfer_guard.Get()->get_ElementsToChamfer(refs_guard.Out())))
          AddReferenceCollectionField(output, "elements_to_chamfer", refs_guard.Get(),
                                      "CATIAChamfer.get_ElementsToChamfer", true, ok);
        else { ok = false; AddReferenceCollectionField(output, "elements_to_chamfer", 0,
                                                       "CATIAChamfer.get_ElementsToChamfer", true, ok); }
      }
      else if (family == "fillet")
      {
        CaaInterfaceGuard<CATIAConstRadEdgeFillet> const_guard;
        if (SUCCEEDED(_spec->QueryInterface(IID_CATIAConstRadEdgeFillet,
            reinterpret_cast<void**>(&const_guard.Out()))) && const_guard.Get())
        {
          interface_supported = true;
          output.interface_key = "CATIAConstRadEdgeFillet";
          AddNativeParameterField(output, "radius_mode", "enum_text", "available",
                                  "CATIAConstRadEdgeFillet", "OK", "constant", "",
                                  false, 0.0, "");
          CaaInterfaceGuard<CATIALength> radius_guard;
          if (SUCCEEDED(const_guard.Get()->get_Radius(radius_guard.Out())))
            AddLengthParameter(output, "radius", radius_guard.Get(),
                               "CATIAConstRadEdgeFillet.get_Radius.Value", ok);
          else { ok = false; AddUnavailableParameter(output, "radius", "length",
                                                     "CATIAConstRadEdgeFillet.get_Radius", "VALUE_READ_FAILED"); }
          CaaInterfaceGuard<CATIAReferences> objects_guard;
          if (SUCCEEDED(const_guard.Get()->get_ObjectsToFillet(objects_guard.Out())))
            AddReferenceCollectionField(output, "objects_to_fillet", objects_guard.Get(),
                                        "CATIAConstRadEdgeFillet.get_ObjectsToFillet", true, ok);
          else { ok = false; AddReferenceCollectionField(output, "objects_to_fillet", 0,
                                                         "CATIAConstRadEdgeFillet.get_ObjectsToFillet", true, ok); }
          AddBaseFilletFields(output, const_guard.Get(), const_guard.Get(), ok);
        }
        else
        {
          CaaInterfaceGuard<CATIAVarRadEdgeFillet> var_guard;
          if (SUCCEEDED(_spec->QueryInterface(IID_CATIAVarRadEdgeFillet,
              reinterpret_cast<void**>(&var_guard.Out()))) && var_guard.Get())
          {
            interface_supported = true;
            output.interface_key = "CATIAVarRadEdgeFillet";
            AddNativeParameterField(output, "radius_mode", "enum_text", "available",
                                    "CATIAVarRadEdgeFillet", "OK", "variable", "",
                                    false, 0.0, "");
            CaaInterfaceGuard<CATIAReferences> edges_guard;
            if (SUCCEEDED(var_guard.Get()->get_EdgesToFillet(edges_guard.Out())))
              AddReferenceCollectionField(output, "edges_to_fillet", edges_guard.Get(),
                                          "CATIAVarRadEdgeFillet.get_EdgesToFillet", true, ok);
            else { ok = false; AddReferenceCollectionField(output, "edges_to_fillet", 0,
                                                           "CATIAVarRadEdgeFillet.get_EdgesToFillet", true, ok); }
            CatFilletVariation variation;
            if (SUCCEEDED(var_guard.Get()->get_FilletVariation(variation)))
              AddEnumParameter(output, "fillet_variation", static_cast<int>(variation),
                               "CATIAVarRadEdgeFillet.get_FilletVariation");
            else { ok = false; AddUnavailableParameter(output, "fillet_variation", "enum_raw",
                                                       "CATIAVarRadEdgeFillet.get_FilletVariation", "VALUE_READ_FAILED"); }
            CatFilletBitangencyType bitangency;
            if (SUCCEEDED(var_guard.Get()->get_BitangencyType(bitangency)))
              AddEnumParameter(output, "bitangency_type", static_cast<int>(bitangency),
                               "CATIAVarRadEdgeFillet.get_BitangencyType");
            CaaInterfaceGuard<CATIAReferences> vertices_guard;
            if (SUCCEEDED(var_guard.Get()->get_ImposedVertices(vertices_guard.Out())))
            {
              AddReferenceCollectionField(output, "imposed_vertices", vertices_guard.Get(),
                                          "CATIAVarRadEdgeFillet.get_ImposedVertices", true, ok);
              CATLONG vertex_count = 0;
              if (vertices_guard.Get() &&
                  SUCCEEDED(vertices_guard.Get()->get_Count(vertex_count)))
              {
                CATLONG index = 1;
                for (; index <= vertex_count; ++index)
                {
                  CaaInterfaceGuard<CATIAReference> vertex_guard(ReferenceCollectionItem(vertices_guard.Get(), index));
                  CaaInterfaceGuard<CATIALength> vertex_radius_guard;
                  std::ostringstream name;
                  name << "imposed_vertex_" << index << "_radius";
                  if (vertex_guard.Get() &&
                      SUCCEEDED(var_guard.Get()->ImposedVertexRadius(vertex_guard.Get(),
                                                                     vertex_radius_guard.Out())))
                    AddLengthParameter(output, name.str().c_str(), vertex_radius_guard.Get(),
                                       "CATIAVarRadEdgeFillet.ImposedVertexRadius.Value", ok);
                  else { ok = false; AddUnavailableParameter(output, name.str().c_str(), "length",
                                                             "CATIAVarRadEdgeFillet.ImposedVertexRadius", "VALUE_READ_FAILED"); }
                }
              }
            }
            else { ok = false; AddReferenceCollectionField(output, "imposed_vertices", 0,
                                                           "CATIAVarRadEdgeFillet.get_ImposedVertices", true, ok); }
            CaaInterfaceGuard<CATIAReference> spine_guard;
            if (SUCCEEDED(var_guard.Get()->get_FilletSpine(spine_guard.Out())))
              AddSingleReferenceField(output, "fillet_spine", spine_guard.Get(),
                                      "CATIAVarRadEdgeFillet.get_FilletSpine", false, ok);
            AddBaseFilletFields(output, var_guard.Get(), var_guard.Get(), ok);
          }
          else
          {
            CaaInterfaceGuard<CATIAFaceFillet> face_guard;
            if (SUCCEEDED(_spec->QueryInterface(IID_CATIAFaceFillet,
                reinterpret_cast<void**>(&face_guard.Out()))) && face_guard.Get())
            {
              interface_supported = true;
              output.interface_key = "CATIAFaceFillet";
              AddNativeParameterField(output, "radius_mode", "enum_text", "available",
                                      "CATIAFaceFillet", "OK", "face_fillet", "",
                                      false, 0.0, "");
              CaaInterfaceGuard<CATIALength> radius_guard;
              if (SUCCEEDED(face_guard.Get()->get_Radius(radius_guard.Out())))
                AddLengthParameter(output, "radius", radius_guard.Get(),
                                   "CATIAFaceFillet.get_Radius.Value", ok);
              else { ok = false; AddUnavailableParameter(output, "radius", "length",
                                                         "CATIAFaceFillet.get_Radius", "VALUE_READ_FAILED"); }
              CaaInterfaceGuard<CATIAReference> first_face_guard;
              CaaInterfaceGuard<CATIAReference> second_face_guard;
              if (SUCCEEDED(face_guard.Get()->get_FirstFace(first_face_guard.Out())))
                AddSingleReferenceField(output, "first_face", first_face_guard.Get(),
                                        "CATIAFaceFillet.get_FirstFace", true, ok);
              else { ok = false; AddSingleReferenceField(output, "first_face", 0,
                                                         "CATIAFaceFillet.get_FirstFace", true, ok); }
              if (SUCCEEDED(face_guard.Get()->get_SecondFace(second_face_guard.Out())))
                AddSingleReferenceField(output, "second_face", second_face_guard.Get(),
                                        "CATIAFaceFillet.get_SecondFace", true, ok);
              else { ok = false; AddSingleReferenceField(output, "second_face", 0,
                                                         "CATIAFaceFillet.get_SecondFace", true, ok); }
              AddBaseFilletFields(output, face_guard.Get(), 0, ok);
            }
            else
            {
              CaaInterfaceGuard<CATIATritangentFillet> tri_guard;
              if (SUCCEEDED(_spec->QueryInterface(IID_CATIATritangentFillet,
                  reinterpret_cast<void**>(&tri_guard.Out()))) && tri_guard.Get())
              {
                interface_supported = true;
                output.interface_key = "CATIATritangentFillet";
                AddNativeParameterField(output, "radius_mode", "enum_text", "available",
                                        "CATIATritangentFillet", "OK", "tritangent", "",
                                        false, 0.0, "");
                CaaInterfaceGuard<CATIAReference> first_face_guard;
                CaaInterfaceGuard<CATIAReference> second_face_guard;
                CaaInterfaceGuard<CATIAReference> remove_face_guard;
                if (SUCCEEDED(tri_guard.Get()->get_FirstFace(first_face_guard.Out())))
                  AddSingleReferenceField(output, "first_face", first_face_guard.Get(),
                                          "CATIATritangentFillet.get_FirstFace", true, ok);
                else { ok = false; AddSingleReferenceField(output, "first_face", 0,
                                                           "CATIATritangentFillet.get_FirstFace", true, ok); }
                if (SUCCEEDED(tri_guard.Get()->get_SecondFace(second_face_guard.Out())))
                  AddSingleReferenceField(output, "second_face", second_face_guard.Get(),
                                          "CATIATritangentFillet.get_SecondFace", true, ok);
                else { ok = false; AddSingleReferenceField(output, "second_face", 0,
                                                           "CATIATritangentFillet.get_SecondFace", true, ok); }
                if (SUCCEEDED(tri_guard.Get()->get_FaceToRemove(remove_face_guard.Out())))
                  AddSingleReferenceField(output, "face_to_remove", remove_face_guard.Get(),
                                          "CATIATritangentFillet.get_FaceToRemove", true, ok);
                else { ok = false; AddSingleReferenceField(output, "face_to_remove", 0,
                                                           "CATIATritangentFillet.get_FaceToRemove", true, ok); }
                AddBaseFilletFields(output, tri_guard.Get(), 0, ok);
              }
              else
              {
                error = "CATIA fillet typed interfaces are not supported";
                return NativeFeatureParameterInterfaceUnsupported;
              }
            }
          }
        }
      }
      else if (family == "shell")
      {
        CaaInterfaceGuard<CATIAShell> shell_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIAShell,
            reinterpret_cast<void**>(&shell_guard.Out()))) || !shell_guard.Get())
        {
          error = "CATIAShell is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIAShell";
        CaaInterfaceGuard<CATIALength> internal_guard;
        CaaInterfaceGuard<CATIALength> external_guard;
        if (SUCCEEDED(shell_guard.Get()->get_InternalThickness(internal_guard.Out())))
          AddLengthParameter(output, "internal_thickness", internal_guard.Get(),
                             "CATIAShell.get_InternalThickness.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "internal_thickness", "length",
                                                   "CATIAShell.get_InternalThickness", "VALUE_READ_FAILED"); }
        if (SUCCEEDED(shell_guard.Get()->get_ExternalThickness(external_guard.Out())))
          AddLengthParameter(output, "external_thickness", external_guard.Get(),
                             "CATIAShell.get_ExternalThickness.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "external_thickness", "length",
                                                   "CATIAShell.get_ExternalThickness", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIAReferences> faces_guard;
        if (SUCCEEDED(shell_guard.Get()->get_FacesToRemove(faces_guard.Out())))
          AddReferenceCollectionField(output, "faces_to_remove", faces_guard.Get(),
                                      "CATIAShell.get_FacesToRemove", true, ok);
        else AddReferenceCollectionField(output, "faces_to_remove", 0,
                                         "CATIAShell.get_FacesToRemove", false, ok);
      }
      else if (family == "thickness")
      {
        CaaInterfaceGuard<CATIAThickness> thickness_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIAThickness,
            reinterpret_cast<void**>(&thickness_guard.Out()))) || !thickness_guard.Get())
        {
          error = "CATIAThickness is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIAThickness";
        CaaInterfaceGuard<CATIALength> offset_guard;
        if (SUCCEEDED(thickness_guard.Get()->get_Offset(offset_guard.Out())))
          AddLengthParameter(output, "offset", offset_guard.Get(),
                             "CATIAThickness.get_Offset.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "offset", "length",
                                                   "CATIAThickness.get_Offset", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIAReferences> faces_guard;
        if (SUCCEEDED(thickness_guard.Get()->get_FacesToThicken(faces_guard.Out())))
          AddReferenceCollectionField(output, "faces_to_thicken", faces_guard.Get(),
                                      "CATIAThickness.get_FacesToThicken", true, ok);
        else { ok = false; AddReferenceCollectionField(output, "faces_to_thicken", 0,
                                                       "CATIAThickness.get_FacesToThicken", true, ok); }
      }
      else if (family == "shaft" || family == "groove")
      {
        CaaInterfaceGuard<CATIARevolution> revolution_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIARevolution,
            reinterpret_cast<void**>(&revolution_guard.Out()))) || !revolution_guard.Get())
        {
          error = "CATIARevolution is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIARevolution";
        CaaInterfaceGuard<CATIAAngle> first_guard;
        CaaInterfaceGuard<CATIAAngle> second_guard;
        if (SUCCEEDED(revolution_guard.Get()->get_FirstAngle(first_guard.Out())))
          AddAngleParameter(output, "first_angle", first_guard.Get(),
                            "CATIARevolution.get_FirstAngle.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "first_angle", "angle",
                                                   "CATIARevolution.get_FirstAngle", "VALUE_READ_FAILED"); }
        if (SUCCEEDED(revolution_guard.Get()->get_SecondAngle(second_guard.Out())))
          AddAngleParameter(output, "second_angle", second_guard.Get(),
                            "CATIARevolution.get_SecondAngle.Value", ok);
        else { ok = false; AddUnavailableParameter(output, "second_angle", "angle",
                                                   "CATIARevolution.get_SecondAngle", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIAReference> axis_guard;
        if (SUCCEEDED(revolution_guard.Get()->get_RevoluteAxis(axis_guard.Out())))
          AddSingleReferenceField(output, "revolute_axis", axis_guard.Get(),
                                  "CATIARevolution.get_RevoluteAxis", true, ok);
        else
        {
          CaaInterfaceGuard<CATIShapeFeatureProperties> properties_guard;
          CATISpecObject_var profile_sketch = NULL_var;
          CATISpecObject_var center_line = NULL_var;
          if (SUCCEEDED(_spec->QueryInterface(IID_CATIShapeFeatureProperties,
              reinterpret_cast<void**>(&properties_guard.Out()))) && properties_guard.Get())
          {
            try
            {
              CATLISTV(CATISpecObject_var) sketches =
                properties_guard.Get()->GiveMeYourFavoriteSketches();
              if (sketches.Size() > 0) profile_sketch = sketches[1];
            }
            catch (...) { profile_sketch = NULL_var; }
          }
          AddSpecObjectReferenceField(output, "profile_sketch", profile_sketch,
                                      "CATIShapeFeatureProperties.GiveMeYourFavoriteSketches",
                                      true, ok);
          if (profile_sketch != NULL_var)
          {
            CaaInterfaceGuard<CATISketch> sketch_guard;
            if (SUCCEEDED(profile_sketch->QueryInterface(IID_CATISketch,
                reinterpret_cast<void**>(&sketch_guard.Out()))) && sketch_guard.Get())
            {
              try { center_line = sketch_guard.Get()->GetCurrentCenterLine(); }
              catch (...) { center_line = NULL_var; }
            }
          }
          AddSpecObjectReferenceField(output, "revolute_axis", center_line,
                                      "CATISketch.GetCurrentCenterLine", true, ok);
        }
        CAT_VARIANT_BOOL value = FALSE;
        if (SUCCEEDED(revolution_guard.Get()->get_IsThin(value)))
          AddBoolParameter(output, "is_thin", value != FALSE, "CATIARevolution.get_IsThin");
        if (SUCCEEDED(revolution_guard.Get()->get_NeutralFiber(value)))
          AddBoolParameter(output, "neutral_fiber", value != FALSE, "CATIARevolution.get_NeutralFiber");
        if (SUCCEEDED(revolution_guard.Get()->get_MergeEnd(value)))
          AddBoolParameter(output, "merge_end", value != FALSE, "CATIARevolution.get_MergeEnd");
      }
      else if (family == "rib" || family == "slot")
      {
        CaaInterfaceGuard<CATIASweep> sweep_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIASweep,
            reinterpret_cast<void**>(&sweep_guard.Out()))) || !sweep_guard.Get())
        {
          error = "CATIASweep is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIASweep";
        CaaInterfaceGuard<CATISweep> public_sweep_guard;
        if (SUCCEEDED(_spec->QueryInterface(IID_CATISweep,
            reinterpret_cast<void**>(&public_sweep_guard.Out()))) &&
            public_sweep_guard.Get())
        {
          CATISpecObject_var profile_sketch = NULL_var;
          CATISpecObject_var center_curve_sketch = NULL_var;
          try { profile_sketch = public_sweep_guard.Get()->GetProfile(); }
          catch (...) { profile_sketch = NULL_var; }
          AddSpecObjectReferenceField(output, "profile_sketch", profile_sketch,
                                      "CATISweep.GetProfile", true, ok);
          try { center_curve_sketch = public_sweep_guard.Get()->GetCenterCurve(); }
          catch (...) { center_curve_sketch = NULL_var; }
          AddSpecObjectReferenceField(output, "center_curve_sketch", center_curve_sketch,
                                      "CATISweep.GetCenterCurve", true, ok);
        }
        else
        {
          ok = false;
          AddSpecObjectReferenceField(output, "profile_sketch", NULL_var,
                                      "CATISweep.GetProfile", true, ok);
          AddSpecObjectReferenceField(output, "center_curve_sketch", NULL_var,
                                      "CATISweep.GetCenterCurve", true, ok);
        }
        CaaInterfaceGuard<CATIAReference> center_guard;
        if (SUCCEEDED(sweep_guard.Get()->get_CenterCurveElement(center_guard.Out())))
          AddSingleReferenceField(output, "center_curve_element", center_guard.Get(),
                                  "CATIASweep.get_CenterCurveElement", true, ok);
        else { ok = false; AddSingleReferenceField(output, "center_curve_element", 0,
                                                   "CATIASweep.get_CenterCurveElement", true, ok); }
        CaaInterfaceGuard<CATIAReference> surface_guard;
        if (SUCCEEDED(sweep_guard.Get()->get_ReferenceSurfaceElement(surface_guard.Out())))
          AddSingleReferenceField(output, "reference_surface_element", surface_guard.Get(),
                                  "CATIASweep.get_ReferenceSurfaceElement", false, ok);
        CaaInterfaceGuard<CATIAReference> pulling_guard;
        if (SUCCEEDED(sweep_guard.Get()->get_PullingDirElement(pulling_guard.Out())))
          AddSingleReferenceField(output, "pulling_direction_element", pulling_guard.Get(),
                                  "CATIASweep.get_PullingDirElement", false, ok);
        CatMergeMode merge_mode;
        if (SUCCEEDED(sweep_guard.Get()->get_MergeMode(merge_mode)))
          AddEnumParameter(output, "merge_mode", static_cast<int>(merge_mode),
                           "CATIASweep.get_MergeMode");
        CAT_VARIANT_BOOL value = FALSE;
        if (SUCCEEDED(sweep_guard.Get()->get_IsThin(value)))
          AddBoolParameter(output, "is_thin", value != FALSE, "CATIASweep.get_IsThin");
        if (SUCCEEDED(sweep_guard.Get()->get_NeutralFiber(value)))
          AddBoolParameter(output, "neutral_fiber", value != FALSE, "CATIASweep.get_NeutralFiber");
        if (SUCCEEDED(sweep_guard.Get()->get_MergeEnd(value)))
          AddBoolParameter(output, "merge_end", value != FALSE, "CATIASweep.get_MergeEnd");
        if (SUCCEEDED(sweep_guard.Get()->get_MoveProfileToPath(value)))
          AddBoolParameter(output, "move_profile_to_path", value != FALSE,
                           "CATIASweep.get_MoveProfileToPath");
        if (SUCCEEDED(sweep_guard.Get()->get_NormalAxisDirReverse(value)))
          AddBoolParameter(output, "normal_axis_dir_reverse", value != FALSE,
                           "CATIASweep.get_NormalAxisDirReverse");
        if (SUCCEEDED(sweep_guard.Get()->get_AnchorDirReverse(value)))
          AddBoolParameter(output, "anchor_dir_reverse", value != FALSE,
                           "CATIASweep.get_AnchorDirReverse");
      }
      else if (family == "rectangular_pattern" || family == "circular_pattern" ||
               family == "user_pattern")
      {
        CaaInterfaceGuard<CATIAPattern> pattern_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIAPattern,
            reinterpret_cast<void**>(&pattern_guard.Out()))) || !pattern_guard.Get())
        {
          error = "CATIAPattern is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIAPattern";
        CaaInterfaceGuard<CATIABase> item_guard;
        if (SUCCEEDED(pattern_guard.Get()->get_ItemToCopy(item_guard.Out())) && item_guard.Get())
          AddNativeParameterField(output, "item_to_copy", "reference", "available",
                                  "CATIAPattern.get_ItemToCopy", "OK", "present", "",
                                  false, 0.0, "");
        else { ok = false; AddUnavailableParameter(output, "item_to_copy", "reference",
                                                   "CATIAPattern.get_ItemToCopy", "VALUE_READ_FAILED"); }
        CaaInterfaceGuard<CATIAAngle> rotation_guard;
        if (SUCCEEDED(pattern_guard.Get()->get_RotationAngle(rotation_guard.Out())))
          AddAngleParameter(output, "rotation_angle", rotation_guard.Get(),
                            "CATIAPattern.get_RotationAngle.Value", ok);

        if (family == "rectangular_pattern")
        {
          CaaInterfaceGuard<CATIARectPattern> rect_guard;
          if (SUCCEEDED(_spec->QueryInterface(IID_CATIARectPattern,
              reinterpret_cast<void**>(&rect_guard.Out()))) && rect_guard.Get())
          {
            output.interface_key = "CATIARectPattern";
            CaaInterfaceGuard<CATIAIntParam> first_row_guard;
            long row = 0;
            if (SUCCEEDED(rect_guard.Get()->get_FirstDirectionRow(first_row_guard.Out())) &&
                ReadIntParamValue(first_row_guard.Get(), row))
              AddLongParameter(output, "first_direction_row_count", row,
                               "CATIARectPattern.get_FirstDirectionRow.Value");
            else { ok = false; AddUnavailableParameter(output, "first_direction_row_count", "integer",
                                                       "CATIARectPattern.get_FirstDirectionRow", "VALUE_READ_FAILED"); }
            CaaInterfaceGuard<CATIAIntParam> second_row_guard;
            if (SUCCEEDED(rect_guard.Get()->get_SecondDirectionRow(second_row_guard.Out())) &&
                ReadIntParamValue(second_row_guard.Get(), row))
              AddLongParameter(output, "second_direction_row_count", row,
                               "CATIARectPattern.get_SecondDirectionRow.Value");
            CAT_VARIANT_BOOL aligned = FALSE;
            if (SUCCEEDED(rect_guard.Get()->get_FirstOrientation(aligned)))
              AddBoolParameter(output, "first_orientation_aligned", aligned != FALSE,
                               "CATIARectPattern.get_FirstOrientation");
            if (SUCCEEDED(rect_guard.Get()->get_SecondOrientation(aligned)))
              AddBoolParameter(output, "second_orientation_aligned", aligned != FALSE,
                               "CATIARectPattern.get_SecondOrientation");
            CaaInterfaceGuard<CATIALinearRepartition> first_repartition_guard;
            if (SUCCEEDED(rect_guard.Get()->get_FirstDirectionRepartition(first_repartition_guard.Out())) &&
                first_repartition_guard.Get())
              AddLinearRepartitionFields(output, "first_direction_repartition",
                                         first_repartition_guard.Get(),
                                         "CATIARectPattern.get_FirstDirectionRepartition", true, ok);
            else { ok = false; AddUnavailableParameter(output, "first_direction_repartition_instances_count",
                                                       "integer", "CATIARectPattern.get_FirstDirectionRepartition",
                                                       "VALUE_READ_FAILED"); }
            CaaInterfaceGuard<CATIALinearRepartition> second_repartition_guard;
            if (SUCCEEDED(rect_guard.Get()->get_SecondDirectionRepartition(second_repartition_guard.Out())) &&
                second_repartition_guard.Get())
              AddLinearRepartitionFields(output, "second_direction_repartition",
                                         second_repartition_guard.Get(),
                                         "CATIARectPattern.get_SecondDirectionRepartition", false, ok);
            CATSafeArrayVariant* first_direction = SafeArrayCreateVector(VT_VARIANT, 0, 3);
            if (first_direction && SUCCEEDED(rect_guard.Get()->GetFirstDirection(*first_direction)))
              AddSafeArrayVector3(output, "first_direction", first_direction,
                                  "unit_vector_component", "",
                                  "CATIARectPattern.GetFirstDirection", ok);
            else
              AddUnavailableVector3(output, "first_direction", "unit_vector_component",
                                    "CATIARectPattern.GetFirstDirection",
                                    "VALUE_READ_FAILED", ok);
            if (first_direction) SafeArrayDestroy(first_direction);
            CATSafeArrayVariant* second_direction = SafeArrayCreateVector(VT_VARIANT, 0, 3);
            if (second_direction && SUCCEEDED(rect_guard.Get()->GetSecondDirection(*second_direction)))
              AddSafeArrayVector3(output, "second_direction", second_direction,
                                  "unit_vector_component", "",
                                  "CATIARectPattern.GetSecondDirection", ok);
            else
              AddUnavailableVector3(output, "second_direction", "unit_vector_component",
                                    "CATIARectPattern.GetSecondDirection",
                                    "VALUE_READ_FAILED", ok);
            if (second_direction) SafeArrayDestroy(second_direction);
            CatRectangularPatternParameters params;
            if (SUCCEEDED(rect_guard.Get()->get_FirstRectangularPatternParameters(params)))
              AddEnumParameter(output, "first_rectangular_parameters", static_cast<int>(params),
                               "CATIARectPattern.get_FirstRectangularPatternParameters");
            if (SUCCEEDED(rect_guard.Get()->get_SecondRectangularPatternParameters(params)))
              AddEnumParameter(output, "second_rectangular_parameters", static_cast<int>(params),
                               "CATIARectPattern.get_SecondRectangularPatternParameters");
          }
          else ok = false;
        }
        else if (family == "circular_pattern")
        {
          CaaInterfaceGuard<CATIACircPattern> circ_guard;
          if (SUCCEEDED(_spec->QueryInterface(IID_CATIACircPattern,
              reinterpret_cast<void**>(&circ_guard.Out()))) && circ_guard.Get())
          {
            output.interface_key = "CATIACircPattern";
            CaaInterfaceGuard<CATIAIntParam> radial_row_guard;
            long row = 0;
            if (SUCCEEDED(circ_guard.Get()->get_RadialDirectionRow(radial_row_guard.Out())) &&
                ReadIntParamValue(radial_row_guard.Get(), row))
              AddLongParameter(output, "radial_direction_row_count", row,
                               "CATIACircPattern.get_RadialDirectionRow.Value");
            CaaInterfaceGuard<CATIAIntParam> angular_row_guard;
            if (SUCCEEDED(circ_guard.Get()->get_AngularDirectionRow(angular_row_guard.Out())) &&
                ReadIntParamValue(angular_row_guard.Get(), row))
              AddLongParameter(output, "angular_direction_row_count", row,
                               "CATIACircPattern.get_AngularDirectionRow.Value");
            CAT_VARIANT_BOOL aligned = FALSE;
            if (SUCCEEDED(circ_guard.Get()->get_RadialAlignment(aligned)))
              AddBoolParameter(output, "radial_alignment", aligned != FALSE,
                               "CATIACircPattern.get_RadialAlignment");
            if (SUCCEEDED(circ_guard.Get()->get_RotationOrientation(aligned)))
              AddBoolParameter(output, "rotation_orientation", aligned != FALSE,
                               "CATIACircPattern.get_RotationOrientation");
            CaaInterfaceGuard<CATIALinearRepartition> radial_repartition_guard;
            if (SUCCEEDED(circ_guard.Get()->get_RadialRepartition(radial_repartition_guard.Out())) &&
                radial_repartition_guard.Get())
              AddLinearRepartitionFields(output, "radial_repartition",
                                         radial_repartition_guard.Get(),
                                         "CATIACircPattern.get_RadialRepartition", false, ok);
            CaaInterfaceGuard<CATIAAngularRepartition> angular_repartition_guard;
            if (SUCCEEDED(circ_guard.Get()->get_AngularRepartition(angular_repartition_guard.Out())) &&
                angular_repartition_guard.Get())
              AddAngularRepartitionFields(output, "angular_repartition",
                                          angular_repartition_guard.Get(),
                                          "CATIACircPattern.get_AngularRepartition", true, ok);
            else { ok = false; AddUnavailableParameter(output, "angular_repartition_instances_count",
                                                       "integer", "CATIACircPattern.get_AngularRepartition",
                                                       "VALUE_READ_FAILED"); }
            CATSafeArrayVariant* rotation_center = SafeArrayCreateVector(VT_VARIANT, 0, 3);
            if (rotation_center && SUCCEEDED(circ_guard.Get()->GetRotationCenter(*rotation_center)))
              AddSafeArrayVector3(output, "rotation_center", rotation_center,
                                  "length", "mm",
                                  "CATIACircPattern.GetRotationCenter", ok);
            else
              AddUnavailableVector3(output, "rotation_center", "length",
                                    "CATIACircPattern.GetRotationCenter",
                                    "VALUE_READ_FAILED", ok);
            if (rotation_center) SafeArrayDestroy(rotation_center);
            CATSafeArrayVariant* rotation_axis = SafeArrayCreateVector(VT_VARIANT, 0, 3);
            if (rotation_axis && SUCCEEDED(circ_guard.Get()->GetRotationAxis(*rotation_axis)))
              AddSafeArrayVector3(output, "rotation_axis", rotation_axis,
                                  "unit_vector_component", "",
                                  "CATIACircPattern.GetRotationAxis", ok);
            else
              AddUnavailableVector3(output, "rotation_axis", "unit_vector_component",
                                    "CATIACircPattern.GetRotationAxis",
                                    "VALUE_READ_FAILED", ok);
            if (rotation_axis) SafeArrayDestroy(rotation_axis);
            CatCircularPatternParameters params;
            if (SUCCEEDED(circ_guard.Get()->get_CircularPatternParameters(params)))
              AddEnumParameter(output, "circular_pattern_parameters", static_cast<int>(params),
                               "CATIACircPattern.get_CircularPatternParameters");
          }
          else ok = false;
        }
        else
        {
          CaaInterfaceGuard<CATIAUserPattern> user_guard;
          if (SUCCEEDED(_spec->QueryInterface(IID_CATIAUserPattern,
              reinterpret_cast<void**>(&user_guard.Out()))) && user_guard.Get())
          {
            output.interface_key = "CATIAUserPattern";
            CaaInterfaceGuard<CATIABase> positions_guard;
            CaaInterfaceGuard<CATIABase> anchor_guard;
            if (SUCCEEDED(user_guard.Get()->get_FeatureToLocatePositions(positions_guard.Out())) &&
                positions_guard.Get())
              AddNativeParameterField(output, "feature_to_locate_positions", "reference",
                                      "available", "CATIAUserPattern.get_FeatureToLocatePositions",
                                      "OK", "present", "", false, 0.0, "");
            else { ok = false; AddUnavailableParameter(output, "feature_to_locate_positions",
                                                       "reference", "CATIAUserPattern.get_FeatureToLocatePositions",
                                                       "VALUE_READ_FAILED"); }
            if (SUCCEEDED(user_guard.Get()->get_AnchorPoint(anchor_guard.Out())) && anchor_guard.Get())
              AddNativeParameterField(output, "anchor_point", "reference", "available",
                                      "CATIAUserPattern.get_AnchorPoint", "OK", "present",
                                      "", false, 0.0, "");
          }
          else ok = false;
        }
      }
      else if (family == "add" || family == "remove" || family == "assemble" ||
               family == "intersect")
      {
        CaaInterfaceGuard<CATIABooleanShape> boolean_guard;
        if (FAILED(_spec->QueryInterface(IID_CATIABooleanShape,
            reinterpret_cast<void**>(&boolean_guard.Out()))) || !boolean_guard.Get())
        {
          error = "CATIABooleanShape is not supported";
          return NativeFeatureParameterInterfaceUnsupported;
        }
        interface_supported = true;
        output.interface_key = "CATIABooleanShape";
        AddNativeParameterField(output, "boolean_operation", "enum_text", "available",
                                "CATISpecObject.GetType", "OK", family, "", false, 0.0, "");
        CaaInterfaceGuard<CATIABody> body_guard;
        if (SUCCEEDED(boolean_guard.Get()->get_Body(body_guard.Out())) && body_guard.Get())
          AddNativeParameterField(output, "operand_body", "reference", "available",
                                  "CATIABooleanShape.get_Body", "OK", "present", "",
                                  false, 0.0, "");
        else { ok = false; AddUnavailableParameter(output, "operand_body", "reference",
                                                   "CATIABooleanShape.get_Body", "VALUE_READ_FAILED"); }
      }
    }
    catch (...)
    {
      error = output.interface_key.empty() ? "Native feature parameter QueryInterface raised an exception" :
        output.interface_key + " value read raised an exception";
      output.reason_code = "CAA_EXCEPTION";
      return interface_supported ? NativeFeatureParameterReadPartial :
        NativeFeatureParameterInterfaceQueryException;
    }

    if (!interface_supported)
    {
      error = "No supported R21 Public native feature parameter interface matched";
      return NativeFeatureParameterInterfaceUnsupported;
    }
    output.decode_status = ok ? "complete" : "partial";
    output.reason_code = ok ? "OK" : "PARTIAL_PUBLIC_API_PAYLOAD";
    output.evidence["api_source"] = output.interface_key;
    output.evidence["decoder_version"] = "1.0.0";
    output.evidence["reason_code"] = output.reason_code;
    return ok ? NativeFeatureParameterReadSuccess : NativeFeatureParameterReadPartial;
  }

private:
  // R21 的受控接口探测器，只接受代码中显式列出的已验证接口键。
  class R21InterfaceProbeService : public InterfaceProbeService
  {
  public:
    // 用途：绑定待探测的借用 CATISpecObject，不增加也不释放其引用计数。
    explicit R21InterfaceProbeService(CATISpecObject* spec) : _spec(spec) {}

    // 用途：探测一个白名单接口；成功时追加键并立刻释放 QueryInterface 返回的临时引用。
    // 未知 key 不会被猜测，直接计入探测失败。
    std::string Probe(const char* key, TypeFingerprint& fingerprint, ParseStatistics& statistics)
    {
      if (std::strcmp(key, "CATISpecObject") == 0)
      {
        fingerprint.supported_interface_keys.push_back(key);
        statistics.RecordProbe(key, fingerprint.native_type, "unselected", "supported");
        return "supported";
      }
      const IID* iid = 0;
      if (std::strcmp(key, "CATIPrtPart") == 0) iid = &IID_CATIPrtPart;
      else if (std::strcmp(key, "CATIContainer") == 0) iid = &IID_CATIContainer;
      else if (std::strcmp(key, "CATIPrtContainer") == 0) iid = &IID_CATIPrtContainer;
      else if (std::strcmp(key, "CATIAHole") == 0) iid = &IID_CATIAHole;
      else if (std::strcmp(key, "CATIAPad") == 0) iid = &IID_CATIAPad;
      else if (std::strcmp(key, "CATIAPocket") == 0) iid = &IID_CATIAPocket;
      if (!iid)
      {
        statistics.RecordProbe(key, fingerprint.native_type, "unselected", "not_attempted");
        return "not_attempted";
      }
      void* result = 0;
      try
      {
        if (SUCCEEDED(_spec->QueryInterface(*iid, &result)) && result)
        {
          fingerprint.supported_interface_keys.push_back(key);
          // QueryInterface 成功会增加引用计数；这里只验证存在性，必须立即配对 Release。
          static_cast<CATBaseUnknown*>(result)->Release();
          statistics.RecordProbe(key, fingerprint.native_type, "unselected", "supported");
          return "supported";
        }
      }
      catch (...)
      {
        statistics.RecordProbe(key, fingerprint.native_type, "unselected", "exception");
        return "exception";
      }
      statistics.RecordProbe(key, fingerprint.native_type, "unselected", "unsupported");
      return "unsupported";
    }

  private:
    CATISpecObject* _spec;
  };

  // 用途：从 CATISpecObject 读取 StartUp/SuperType/名称，并执行固定接口白名单探测。
  // 任何不可用字段只产生 warning；未验证的 native runtime type 保持为空，不进行猜测。
  void BuildFingerprint(ParseContext& context)
  {
    if (!_spec) return;
    // TODO(R21_API_VERIFY)：尚未确认 R21 公开接口中存在有文档依据的原生运行时类型读取方法。
    try
    {
      _fingerprint.startup_type = UnicodeToUtf8(_spec->GetType());
      const std::string super_type = UnicodeToUtf8(_spec->GetSuperType());
      if (!super_type.empty()) _fingerprint.super_types.push_back(super_type);
      _fingerprint.internal_name = UnicodeToUtf8(_spec->GetName());
      _fingerprint.display_name = UnicodeToUtf8(_spec->GetDisplayName());
      _fingerprint.container_kind = "feature";
    }
    catch (...)
    {
      context.AddDiagnostic("warning", "fingerprint", "SPEC_FINGERPRINT_READ_FAILED",
                            "one or more CATISpecObject type fields were unavailable", "");
    }
    R21InterfaceProbeService probes(_spec);
    probes.Probe("CATISpecObject", _fingerprint, context.statistics);
    probes.Probe("CATIPrtPart", _fingerprint, context.statistics);
    probes.Probe("CATIContainer", _fingerprint, context.statistics);
    probes.Probe("CATIPrtContainer", _fingerprint, context.statistics);
    // Hole 专用探测只对已预筛选候选执行；Typed Decoder 随后仍会再次查询并读取必需值。
    if (_fingerprint.startup_type == "Hole")
      probes.Probe("CATIAHole", _fingerprint, context.statistics);
    // Pad/Pocket 专用探测只对候选执行；这里只证明接口存在，参数读取仍由 Decoder 完成。
    if (_fingerprint.startup_type == "Pad")
      probes.Probe("CATIAPad", _fingerprint, context.statistics);
    if (_fingerprint.startup_type == "Pocket")
      probes.Probe("CATIAPocket", _fingerprint, context.statistics);
    if (std::find(_fingerprint.supported_interface_keys.begin(),
                  _fingerprint.supported_interface_keys.end(), "CATIPrtPart") !=
        _fingerprint.supported_interface_keys.end())
      _fingerprint.container_kind = "part";
  }

  CATISpecObject* _spec;
  TypeFingerprint _fingerprint;
  PrismCapabilityView _pad_capability;
  PrismCapabilityView _pocket_capability;
};

static std::string ProductTreeFeatureId(long index)
{
  std::ostringstream id;
  id << "PTREE_";
  if (index < 10) id << "00000";
  else if (index < 100) id << "0000";
  else if (index < 1000) id << "000";
  else if (index < 10000) id << "00";
  else if (index < 100000) id << "0";
  id << index;
  return id.str();
}

static std::string NativeTreeDisplayText(const TypeFingerprint& fingerprint)
{
  if (!fingerprint.display_name.empty()) return fingerprint.display_name;
  if (!fingerprint.internal_name.empty()) return fingerprint.internal_name;
  if (!fingerprint.startup_type.empty()) return fingerprint.startup_type;
  return "unnamed";
}

static bool ProductSpecTreeHasChildren(CATISpecObject* spec)
{
  if (!spec) return false;
  try
  {
    CATListValCATISpecObject_var* children = spec->ListComponents();
    if (!children) return false;
    SpecListGuard guard(children);
    return children->Size() > 0;
  }
  catch (...) { return false; }
}

static bool AppendProductSpecTreeNode(CATISpecObject* spec, ParseContext& context,
                                      const ProductInstanceRecord& instance,
                                      const std::string& parent_node_id,
                                      const std::string& parent_path,
                                      long source_index, long& local_index,
                                      std::set<CATISpecObject*>& path_guard)
{
  if (!spec) return false;
  if (path_guard.find(spec) != path_guard.end())
  {
    context.AddDiagnostic("warning", "product_native_tree",
                          "PRODUCT_REFERENCE_SPEC_TREE_CYCLE",
                          "CATISpecObject cycle detected while mounting product reference tree",
                          std::string("instance:") + instance.instance_id);
    return true;
  }
  path_guard.insert(spec);
  try
  {
    SpecObjectView view(spec, context);
    const TypeFingerprint& fingerprint = view.GetFingerprint();
    const std::string feature_id = ProductTreeFeatureId(local_index++);
    const std::string display_text = NativeTreeDisplayText(fingerprint);
    const std::string tree_path = parent_path.empty() ? display_text :
      parent_path + "/" + display_text;

    NativeTreeNodeRecord node;
    node.node_id = std::string("instance:") + instance.instance_id +
      "/feature:" + feature_id;
    node.parent_id = parent_node_id;
    node.display_text = display_text;
    node.display_name = fingerprint.display_name;
    node.internal_name = fingerprint.internal_name;
    node.startup_type = fingerprint.startup_type.empty() ?
      fingerprint.native_type : fingerprint.startup_type;
    node.document_kind = "catpart";
    node.node_kind = "native_feature";
    node.source_index = source_index;
    node.traversal_index = static_cast<long>(context.native_tree_nodes.size() + 1);
    node.tree_path = tree_path;
    node.instance_id = instance.instance_id;
    node.parent_instance_id = instance.parent_instance_id;
    node.reference_id = instance.reference_id;
    node.source_feature_id = feature_id;
    node.source_node_id = feature_id;
    node.has_children = ProductSpecTreeHasChildren(spec);
    node.properties_available = true;
    node.attributes["value_source"] = "CATISpecObject::ListComponents";
    node.attributes["mounted_from_product_reference"] = "true";
    node.attributes["container_kind"] = fingerprint.container_kind;
    if (!fingerprint.native_type.empty()) node.attributes["native_type"] = fingerprint.native_type;
    if (!fingerprint.super_types.empty()) node.attributes["super_type"] = fingerprint.super_types[0];
    context.native_tree_nodes.push_back(node);

    CATListValCATISpecObject_var* children = spec->ListComponents();
    if (children)
    {
      SpecListGuard children_guard(children);
      int index = 0;
      for (index = 1; index <= children->Size(); ++index)
      {
        CATISpecObject_var child = (*children)[index];
        if (child != NULL_var)
        {
          CATISpecObject* child_pointer = child;
          AppendProductSpecTreeNode(child_pointer, context, instance, node.node_id,
                                    tree_path, index, local_index, path_guard);
        }
      }
    }
    path_guard.erase(spec);
    return true;
  }
  catch (...)
  {
    path_guard.erase(spec);
    context.AddDiagnostic("warning", "product_native_tree",
                          "PRODUCT_REFERENCE_SPEC_TREE_NODE_FAILED",
                          "CATISpecObject node could not be mounted; scan continued",
                          std::string("instance:") + instance.instance_id);
    return false;
  }
}

static bool QueryProductSpecObject(CATIProduct* product, CATISpecObject** spec)
{
  if (!product || !spec) return false;
  *spec = 0;
  try
  {
    if (SUCCEEDED(product->QueryInterface(IID_CATISpecObject,
        reinterpret_cast<void**>(spec))) && *spec)
      return true;
  }
  catch (...) { *spec = 0; }
  return false;
}

static bool MountProductReferenceSpecTree(CATIProduct* instance_product,
                                          CATIProduct* reference_product,
                                          ParseContext& context,
                                          const ProductInstanceRecord& instance)
{
  if (instance.instance_id.empty()) return false;
  CATISpecObject* spec = 0;
  std::string source_api;
  if (QueryProductSpecObject(reference_product, &spec))
    source_api = "reference_product.QueryInterface(CATISpecObject)";
  else if (QueryProductSpecObject(instance_product, &spec))
    source_api = "instance_product.QueryInterface(CATISpecObject)";
  if (!spec)
  {
    context.AddDiagnostic("info", "product_native_tree",
                          "PRODUCT_REFERENCE_SPEC_TREE_UNAVAILABLE",
                          "CATIProduct instance/reference did not expose CATISpecObject; BOM node preserved",
                          std::string("instance:") + instance.instance_id);
    return false;
  }

  CaaInterfaceGuard<CATISpecObject> spec_guard(spec);
  const std::string reference_node_id = std::string("instance:") + instance.instance_id +
    "/reference:" + instance.reference_id;
  NativeTreeNodeRecord reference_node;
  reference_node.node_id = reference_node_id;
  reference_node.parent_id = std::string("instance:") + instance.instance_id;
  reference_node.display_text = instance.reference_id.empty() ? instance.instance_name :
    instance.reference_id;
  reference_node.display_name = reference_node.display_text;
  reference_node.internal_name = instance.reference_id;
  reference_node.startup_type = "CATIProductReference";
  reference_node.document_kind = "catpart";
  reference_node.node_kind = "product_reference";
  reference_node.source_index = 0;
  reference_node.traversal_index = static_cast<long>(context.native_tree_nodes.size() + 1);
  reference_node.tree_path = instance.tree_path + "/" + reference_node.display_text;
  reference_node.instance_id = instance.instance_id;
  reference_node.parent_instance_id = instance.parent_instance_id;
  reference_node.reference_id = instance.reference_id;
  reference_node.source_node_id = instance.reference_id;
  reference_node.has_children = true;
  reference_node.properties_available = true;
  reference_node.attributes["value_source"] = source_api;
  context.native_tree_nodes.push_back(reference_node);

  long local_index = 1;
  std::set<CATISpecObject*> path_guard;
  return AppendProductSpecTreeNode(spec, context, instance, reference_node_id,
                                   reference_node.tree_path, 1, local_index, path_guard);
}

// 基础 Typed Decoder：封装所有核心节点共有的“读取基础属性并标记 typed success”行为。
class CoreDecoder : public IFeatureDecoder
{
public:
  // 用途：保存由静态字符串提供的稳定 ID 和显式优先级。
  CoreDecoder(const char* id, int priority) : _id(id), _priority(priority) {}
  // 用途：返回构造时绑定的 Decoder ID；Core Decoder 使用字符串常量，生命周期覆盖整个进程。
  const char* GetDecoderId() const { return _id; }
  // 用途：返回用于 Registry 决胜的显式优先级。
  int GetPriority() const { return _priority; }
  // 用途：执行 Typed Decoder 的公共读取逻辑；失败时交回 Registry 继续 Generic/Opaque 兜底。
  DecodeResult Decode(const INativeObjectView& view, ParseContext& context, FeatureRecord& output)
  {
    std::string error;
    if (!view.ReadBasicAttributes(output, error))
      return DecodeResult(false, "failed", error.c_str());
    output.decoder_id = _id;
    output.decode_level = "typed";
    output.decode_status = "success";
    return DecodeResult(true, "typed");
  }

protected:
  const char* _id;
  int _priority;
};

// 文档根节点 Decoder，依据 crawler 明确赋予的 container_kind 匹配。
class DocumentDecoder : public CoreDecoder
{
public:
  // 用途：创建优先级 400、稳定 ID 为 document 的 Decoder。
  DocumentDecoder() : CoreDecoder("document", 400) {}
  // 用途：只匹配 container_kind 为 document 的静态文档视图。
  bool Match(const TypeFingerprint& fp, const INativeObjectView&) const
  { return fp.container_kind == "document"; }
};

// Part Decoder，优先使用已验证 CATIPrtPart 接口键而不是显示名称。
class PartDecoder : public CoreDecoder
{
public:
  // 用途：创建高优先级 Part Decoder，使接口证据优先于通用容器匹配。
  PartDecoder() : CoreDecoder("part", 700) {}
  // 用途：检查 supported_interface_keys 中是否存在 CATIPrtPart。
  bool Match(const TypeFingerprint& fp, const INativeObjectView&) const
  { return std::find(fp.supported_interface_keys.begin(), fp.supported_interface_keys.end(),
                     "CATIPrtPart") != fp.supported_interface_keys.end(); }
};

// 已验证容器入口的 Typed Decoder。
class ContainerDecoder : public CoreDecoder
{
public:
  // 用途：创建 ID 为 container、优先级 350 的 Decoder。
  ContainerDecoder() : CoreDecoder("container", 350) {}
  // 用途：匹配 crawler 明确标记为 container 的静态入口节点。
  bool Match(const TypeFingerprint& fp, const INativeObjectView&) const
  { return fp.container_kind == "container"; }
};

// Body 基础 Decoder；R21 PublicInterfaces 未提供已确认 marker，因此只使用保守 StartUp 类型匹配。
class BodyDecoder : public CoreDecoder
{
public:
  // 用途：创建 ID 为 body、优先级 500 的 Decoder。
  BodyDecoder() : CoreDecoder("body", 500) {}
  // TODO(R21_API_VERIFY)：已安装的公开接口中未找到 CATIBody 标记接口。
  // 用途：匹配本机资料中已知的 Body/MechanicalTool StartUp 类型文本。
  bool Match(const TypeFingerprint& fp, const INativeObjectView&) const
  { return fp.startup_type == "Body" || fp.startup_type == "MechanicalTool"; }
};

// HybridBody 基础 Decoder；同样不假设不存在证据的专用 marker 接口。
class HybridBodyDecoder : public CoreDecoder
{
public:
  // 用途：创建 ID 为 hybrid_body、优先级 500 的 Decoder。
  HybridBodyDecoder() : CoreDecoder("hybrid_body", 500) {}
  // TODO(R21_API_VERIFY)：已安装的公开接口中未找到 CATIHybridBody 标记接口。
  // 用途：匹配已确认的 HybridBody/GeometricalSet StartUp 类型文本。
  bool Match(const TypeFingerprint& fp, const INativeObjectView&) const
  { return fp.startup_type == "HybridBody" || fp.startup_type == "GeometricalSet"; }
};

// 用途：创建 MVP 的五个基础 Typed Decoder，同时登记到 Registry 和所有权 vector。
// Registry 仅借用指针；owned_decoders 是唯一负责最终 delete 的容器。
void RegisterCoreDecoders(FeatureTypeRegistry& registry,
                          std::vector<IFeatureDecoder*>& owned_decoders)
{
  owned_decoders.push_back(new KnowledgewareStringParameterDecoder());
    owned_decoders.push_back(new NativeHoleDecoder());
    owned_decoders.push_back(new NativePadDecoder());
    owned_decoders.push_back(new NativePocketDecoder());
    owned_decoders.push_back(new NativeFeatureParameterDecoder());
    owned_decoders.push_back(new StartupTypeCanonicalDecoder());
  owned_decoders.push_back(new DocumentDecoder());
  owned_decoders.push_back(new PartDecoder());
  owned_decoders.push_back(new ContainerDecoder());
  owned_decoders.push_back(new BodyDecoder());
  owned_decoders.push_back(new HybridBodyDecoder());
  // C++03 没有范围 for，使用 iterator 按固定顺序注册；匹配结果仍不依赖注册顺序。
  std::vector<IFeatureDecoder*>::iterator it = owned_decoders.begin();
  for (; it != owned_decoders.end(); ++it) registry.Register(*it);
}

// 用途：释放 RegisterCoreDecoders 创建的全部 Decoder，并清空所有权容器。
void DeleteCoreDecoders(std::vector<IFeatureDecoder*>& owned_decoders)
{
  std::vector<IFeatureDecoder*>::iterator it = owned_decoders.begin();
  for (; it != owned_decoders.end(); ++it) delete *it;
  owned_decoders.clear();
}

// 用途：创建一次遍历所需的 Crawler，并以引用保存 Registry、上下文和两个输出集合。
// 这些引用不转移所有权，调用者必须保证它们覆盖整个 Crawl 生命周期。
UniversalFeatureCrawler::UniversalFeatureCrawler(FeatureTypeRegistry& registry, ParseContext& context,
                                                 std::vector<FeatureRecord>& features,
                                                 std::vector<RelationRecord>& relations)
  : _registry(registry), _context(context), _features(features), _relations(relations)
{
}

// 用途：先为对象建立基础 FeatureRecord，再执行 Decoder，并按 parent_id 建立正式关系。
// 返回新分配的稳定 feature_id，供递归子节点作为 parent_id 使用。
std::string UniversalFeatureCrawler::AddObject(INativeObjectView& view,
                                               const std::string& parent_id,
                                               const std::string& tree_path,
                                               long native_enumeration_index,
                                               long container_enumeration_index)
{
  FeatureRecord record;
  record.feature_id = _ids.Next();
  record.parent_id = parent_id;
  record.native_enumeration_index = native_enumeration_index;
  record.container_enumeration_index = container_enumeration_index;
  record.traversal_index = static_cast<long>(_features.size() + 1);
  record.tree_path = tree_path;
  record.update_status = "unknown";
  record.visibility = "unknown";
  // 即使 Decode 随后失败，类型观察和基础记录也已经建立，满足“不丢对象”的约束。
  _catalog.Observe(view.GetFingerprint());
  _registry.DecodeObject(view, _context, record);
  if (record.update_status == "not_up_to_date")
  {
    ++_context.statistics.not_up_to_date_count;
    const std::string type = record.fingerprint.native_type.empty() ?
      record.fingerprint.startup_type : record.fingerprint.native_type;
    ++_context.statistics.not_up_to_date_by_native_type[type.empty() ? "unknown" : type];
    ++_context.statistics.not_up_to_date_by_decoder[record.decoder_id];
    _context.statistics.not_up_to_date_feature_ids.push_back(record.feature_id);
  }
  _features.push_back(record);
  if (!parent_id.empty())
  {
    RelationRecord relation;
    relation.kind = "parent_of";
    relation.from_id = parent_id;
    relation.to_id = record.feature_id;
    _relations.push_back(relation);
    relation.kind = "contains";
    _relations.push_back(relation);
  }
  return record.feature_id;
}

// 用途：递归访问一个规格对象，严格保留 ListComponents 返回的原生顺序。
// visited 以运行期指针识别循环，但指针仅用于本次遍历控制，绝不写入输出。
bool UniversalFeatureCrawler::VisitSpec(CATISpecObject* spec, const std::string& parent_id,
                                        const std::string& parent_path,
                                        long native_enumeration_index,
                                        long container_enumeration_index)
{
  // 重复到达同一个原生对象时直接返回，防止循环引用或多入口造成无限递归。
  if (!spec || _visited.find(spec) != _visited.end()) return true;
  _visited.insert(spec);

  try
  {
    SpecObjectView view(spec, _context);
    const TypeFingerprint& fp = view.GetFingerprint();
    _unknown_types.Observe(fp);
    _context.statistics.unknown_native_type_count = static_cast<long>(_unknown_types.Count());
    std::string segment = fp.display_name.empty() ? fp.internal_name : fp.display_name;
    if (segment.empty()) segment = fp.startup_type.empty() ? "unnamed" : fp.startup_type;
    const std::string path = parent_path + "/" + segment;
    const std::string id = AddObject(view, parent_id, path, native_enumeration_index,
                                     container_enumeration_index);
    if (std::find(fp.supported_interface_keys.begin(), fp.supported_interface_keys.end(),
                  "CATIPrtPart") != fp.supported_interface_keys.end())
    {
      CollectPartMainSolidTopology(spec, id, _context);
    }
    CollectNativeFeatureResultTopology(spec, id, fp, _context);

    CATListValCATISpecObject_var* children = spec->ListComponents();
    if (!children) return true;
    // ListComponents 返回堆对象，立即建立守卫，保证后续任何异常路径都能 delete。
    SpecListGuard children_guard(children);
    int index = 0;
    for (index = 1; index <= children->Size(); ++index)
    {
      CATISpecObject_var child = (*children)[index];
      if (child != NULL_var)
      {
        CATISpecObject* child_pointer = child;
        VisitSpec(child_pointer, id, path, index, container_enumeration_index);
      }
    }
    return true;
  }
  catch (...)
  {
    // 对象级异常转成诊断并返回 false；上层可决定入口失败是否为文档级致命错误。
    _context.AddDiagnostic("warning", "discovery", "OBJECT_TRAVERSAL_FAILED",
                           "CATISpecObject traversal failed; scan continued", parent_id);
    return false;
  }
}

// 用途：从 CATDocument 根开始执行 MVP 完整发现链路，并枚举已验证 Part 容器与规格对象入口。
// document 是 DocumentGuard 拥有的借用指针；函数不关闭文档。入口级失败通过 error 返回 false。
bool UniversalFeatureCrawler::Crawl(CATDocument* document, std::string& error)
{
  if (!document)
  {
    error = "null CATDocument";
    return false;
  }
  try
  {
  // 文档和容器不是 CATISpecObject，先用 StaticObjectView 为它们建立同样完整的基础 IR。
  StaticObjectView document_view("CATDocument", "document", UnicodeToUtf8(document->DisplayName()));
  const std::string document_id = AddObject(document_view, "", "/document", 0, 0);
  CollectFtaSets(document, _context, document_id);
  const bool has_product_structure =
    CollectProductStructure(document, _context, UnicodeToUtf8(document->DisplayName()));

  CATInit* init = 0;
  // QueryInterface 成功会返回持有引用；守卫必须在紧邻成功检查后接管它。
  if (FAILED(document->QueryInterface(IID_CATInit, reinterpret_cast<void**>(&init))) || !init)
  {
    error = "CATInit is unavailable on CATPart document";
    return false;
  }
  CaaInterfaceGuard<CATInit> init_guard(init);
  // GetRootContainer 返回的 CATBaseUnknown 引用由 root_guard 负责释放。
  CATBaseUnknown* root = init->GetRootContainer("CATIPrtContainer");
  if (!root)
  {
    if (has_product_structure)
    {
      _context.statistics.relation_count = static_cast<long>(_relations.size());
      return true;
    }
    error = "CATIPrtContainer root is unavailable";
    return false;
  }
  CaaInterfaceGuard<CATBaseUnknown> root_guard(root);

  CATIPrtContainer* part_container = 0;
  const HRESULT root_result = root->QueryInterface(IID_CATIPrtContainer,
                                                    reinterpret_cast<void**>(&part_container));
  if (FAILED(root_result) || !part_container)
  {
    if (has_product_structure)
    {
      _context.statistics.relation_count = static_cast<long>(_relations.size());
      return true;
    }
    error = "CATIPrtContainer query failed";
    return false;
  }
  CaaInterfaceGuard<CATIPrtContainer> part_container_guard(part_container);

  // 把已验证的 Part Spec Container 作为独立 IR 节点，后续 Feature 都挂在它下面。
  StaticObjectView container_view("CATIPrtContainer", "container", "PartSpecContainer");
  const std::string container_id = AddObject(container_view, document_id,
                                              "/document/PartSpecContainer", 1, 1);
  ++_context.statistics.container_count;

  CATISpecObject_var part = NULL_var;
  // _var 是 CAA 智能引用包装，离开作用域时自动管理 GetPart 返回对象的引用计数。
  try
  {
    part = part_container->GetPart();
  }
  catch (...)
  {
    _context.AddDiagnostic("warning", "discovery", "PART_ENTRY_EXCEPTION",
                           "CATIPrtContainer::GetPart raised an exception", container_id);
    error = "Part root access failed";
    return false;
  }
  if (part != NULL_var)
  {
    CATISpecObject* part_pointer = part;
    if (!VisitSpec(part_pointer, container_id, "/document/PartSpecContainer", 1, 1))
    {
      error = "Part root traversal failed";
      return false;
    }
  }
  else
  {
    _context.AddDiagnostic("warning", "discovery", "PART_ENTRY_UNAVAILABLE",
                           "CATIPrtContainer::GetPart returned null", container_id);
    error = "Part root is unavailable";
    return false;
  }

  CATIContainer* generic_container = 0;
  // CATIContainer 是补充入口：存在时枚举当前容器成员，不存在时仅记录 info 而不猜测替代 API。
  if (SUCCEEDED(root->QueryInterface(IID_CATIContainer,
                                     reinterpret_cast<void**>(&generic_container))) && generic_container)
  {
    CaaInterfaceGuard<CATIContainer> generic_container_guard(generic_container);
    try
    {
      SEQUENCE(CATBaseUnknown_ptr) members;
      // 先建立序列守卫，再调用枚举；异常时已经返回的成员引用也能被释放。
      BaseUnknownSequenceGuard members_guard(members);
      const CATLONG32 count = generic_container->ListMembersHere("CATISpecObject", members);
      CATLONG32 index = 0;
      for (index = 0; index < count; ++index)
      {
        CATBaseUnknown* member = members[index];
        if (!member) continue;
        CATISpecObject* member_spec = 0;
        if (SUCCEEDED(member->QueryInterface(IID_CATISpecObject,
                                             reinterpret_cast<void**>(&member_spec))) && member_spec)
        {
          // 临时 QueryInterface 引用由局部守卫释放；立即按枚举器原始位置访问。
          CaaInterfaceGuard<CATISpecObject> member_spec_guard(member_spec);
          VisitSpec(member_spec, container_id, "/document/PartSpecContainer",
                    static_cast<long>(index + 1), 1);
        }
      }
    }
    catch (...)
    {
      _context.AddDiagnostic("warning", "discovery", "CONTAINER_ENUMERATION_EXCEPTION",
                             "CATIContainer member enumeration failed", container_id);
      error = "supplemental container enumeration failed";
      return false;
    }
  }
  else
    _context.AddDiagnostic("info", "discovery", "APPLICATIVE_CONTAINER_UNAVAILABLE",
                           "root container does not expose CATIContainer", container_id);

  if (_context.statistics.not_up_to_date_count > 0)
  {
    std::ostringstream message;
    message << _context.statistics.not_up_to_date_count
            << " enumerated objects are not up to date; see coverage feature IDs";
    _context.AddDiagnostic("warning", "document", "MODEL_CONTAINS_STALE_OBJECTS",
                           message.str().c_str(), document_id);
  }

  _context.statistics.relation_count = static_cast<long>(_relations.size());
  return true;
  }
  catch (...)
  {
    // 最外层 catch 是文档级安全网；所有已创建的 RAII 守卫仍会按栈展开顺序执行清理。
    error = "CAA traversal raised an unhandled exception";
    return false;
  }
}
}
