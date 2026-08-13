// 本文件定义解析器各层共享的纯数据契约和扩展接口。
// 这里不保存任何 CATIA 裸指针，注册、解码和结果写出都通过这些契约解耦。
#ifndef CAD_PARSE_CONTRACTS_H
#define CAD_PARSE_CONTRACTS_H

#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

// 所有产品和结构版本集中在一个定义点，避免清单中的版本号长期漂移。
#define CAD_PARSE_SCHEMA_VERSION "cad_parse_mvp_v11"
#define CAD_PARSE_PARSER_VERSION "1.11.0"
#define CAD_PARSE_REGISTRY_VERSION "1.11.0"
#define CAD_PARSE_DECODER_BUNDLE_VERSION "1.11.0"

namespace cadparse
{
// 类型指纹汇总注册中心选择解码器时使用的稳定类型信息。
struct TypeFingerprint
{
  // 各字段来自已验证的 CAA 类型信息；无法可靠取得的字段保持为空。
  std::string native_type;
  std::string startup_type;
  std::vector<std::string> super_types;
  std::vector<std::string> supported_interface_keys;
  std::string container_kind;
  std::string internal_name;
  std::string display_name;
};

// 字符串参数的类型化读取结果及可选规范化结果。
struct ParameterValueData
{
  // 用途：初始化为“尚无规范化数值”，避免把默认零值误认为真实参数。
  ParameterValueData()
    : has_normalized_numeric_value(false), normalized_numeric_value(0.0) {}

  std::string parameter_kind;
  std::string parameter_name;
  std::string value_status;
  std::string value_source;
  std::string value_text;
  std::string raw_display_text;
  bool has_normalized_numeric_value;
  double normalized_numeric_value;
  std::string normalized_unit;
  std::string normalization_status;
  std::string is_read_only;
  std::string is_hidden;
};

// 可空数值用于区分“真实的零”与“不适用或不可读取”，并保留字段级状态。
struct OptionalNativeHoleNumber
{
  // 用途：默认构造为空值，避免用零伪装尚未读取的原生孔参数。
  OptionalNativeHoleNumber() : has_value(false), value(0.0), status("unavailable") {}
  // 用途：记录通过专用接口读取成功的数值与状态。
  void Set(double new_value, const char* new_status)
  { has_value = true; value = new_value; status = new_status; }
  // 用途：清空数值并保留“不适用”或“不可用”等真实原因。
  void Clear(const char* new_status)
  { has_value = false; value = 0.0; status = new_status; }

  bool has_value;
  double value;
  std::string status;
};

// 可空字符串用于保留真实空字符串，同时区分接口未提供该字段的情况。
struct OptionalNativeHoleString
{
  // 用途：默认构造为不可用，不把空文本自动解释成读取成功。
  OptionalNativeHoleString() : has_value(false), status("unavailable") {}
  // 用途：记录接口返回的原始字符串；空字符串也可以是合法成功值。
  void Set(const std::string& new_value, const char* new_status)
  { has_value = true; value = new_value; status = new_status; }
  // 用途：清空字符串并记录不适用或不可用原因。
  void Clear(const char* new_status)
  { has_value = false; value.clear(); status = new_status; }

  bool has_value;
  std::string value;
  std::string status;
};

// 原生孔的终止方式，以及只在偏移终止方式下有意义的深度。
struct NativeHoleBottomLimitData
{
  // 用途：初始化为未知枚举，防止读取失败时出现看似合法的默认类型。
  NativeHoleBottomLimitData() : mode_raw(-1) {}
  std::string mode;
  int mode_raw;
  OptionalNativeHoleNumber depth_mm;
};

// 原生孔头部的专用参数；不同孔型不适用的字段保持空值。
struct NativeHoleHeadData
{
  std::string kind;
  OptionalNativeHoleNumber diameter_mm;
  OptionalNativeHoleNumber depth_mm;
  OptionalNativeHoleNumber angle_deg;
};

// CATIA 原生螺纹属性；描述必须来自接口，不能由直径和螺距拼接。
struct NativeHoleThreadData
{
  // 用途：默认按未确认状态初始化，只有适配器成功读取后才写入最终值。
  NativeHoleThreadData() : enabled(false), mode_raw(-1) {}
  bool enabled;
  int mode_raw;
  OptionalNativeHoleString description;
  OptionalNativeHoleNumber diameter_mm;
  OptionalNativeHoleNumber depth_mm;
  OptionalNativeHoleNumber pitch_mm;
};

// 与 API 无关的类型化原生孔载荷；这里只保存纯数据，不保存 CAA 指针或 COM 句柄。
struct NativeHoleData
{
  // 用途：为原始枚举和必需数值提供明确的未读取初值。
  NativeHoleData() : hole_type_raw(-1), diameter_mm(0.0), has_automation_alias(false)
  {
    origin_mm[0] = origin_mm[1] = origin_mm[2] = 0.0;
    direction[0] = direction[1] = direction[2] = 0.0;
  }

  std::string semantic_kind;
  std::string value_source;
  std::string interface_key;
  std::string hole_type;
  int hole_type_raw;
  double diameter_mm;
  double origin_mm[3];
  double direction[3];
  NativeHoleBottomLimitData bottom_limit;
  NativeHoleHeadData head;
  NativeHoleThreadData thread;
  bool has_automation_alias;
  std::string automation_alias;
  std::string automation_alias_status;
  std::map<std::string, std::string> field_status;
};

// 类型化载荷的通用所有权与序列化协议；每种特征仍由独立强类型派生类保存数据。
class ITypedPayload
{
public:
  virtual ~ITypedPayload() {}
  // 用途：返回稳定载荷类型标识，供消费者在不使用 RTTI 的情况下确认具体类型。
  virtual const char* GetPayloadTypeId() const = 0;
  // 用途：创建独立深副本，使 FeatureRecord 的复制不共享可释放指针。
  virtual ITypedPayload* Clone() const = 0;
  // 用途：写出一个完整 JSON 属性；中央写出器只负责调用，不包含特征类型分支。
  virtual void WriteJsonProperty(std::ostream& output) const = 0;
};

// 原生孔的强类型载荷实现；JSON 字段名保持 native_hole，与既有 Schema 兼容。
class NativeHolePayload : public ITypedPayload
{
public:
  explicit NativeHolePayload(const NativeHoleData& data) : _data(data) {}
  const char* GetPayloadTypeId() const { return "native_hole"; }
  ITypedPayload* Clone() const { return new NativeHolePayload(*this); }
  void WriteJsonProperty(std::ostream& output) const;
  // 用途：返回只读强类型数据，消费者先检查载荷类型标识后再调用。
  const NativeHoleData& GetData() const { return _data; }

private:
  NativeHoleData _data;
};

// Prism 终止边界；Pad 和 Pocket 都复用该结构，但不把两者混为同一种业务语义。
struct NativePrismLimitData
{
  // 用途：默认按未知终止方式初始化，避免读取失败时出现合法枚举假象。
  NativePrismLimitData() : mode_raw(-1) {}
  std::string mode;
  int mode_raw;
  OptionalNativeHoleNumber dimension_mm;
  std::string limiting_element_status;
};

// 与 API 无关的 Pad/Pocket 公共 Prism 载荷；这里只保存真实接口读取到的纯数据。
struct NativePrismData
{
  // 用途：为枚举、方向和布尔状态提供明确未读取初值。
  NativePrismData()
    : direction_type_raw(-1), direction_orientation_raw(-1), is_symmetric(false),
      is_thin(false), neutral_fiber(false), merge_end(false)
  {
    direction[0] = direction[1] = direction[2] = 0.0;
  }

  std::string semantic_kind;
  std::string material_operation;
  std::string value_source;
  std::string interface_key;
  std::string direction_type;
  int direction_type_raw;
  std::string direction_orientation;
  int direction_orientation_raw;
  double direction[3];
  bool is_symmetric;
  bool is_thin;
  bool neutral_fiber;
  bool merge_end;
  NativePrismLimitData first_limit;
  NativePrismLimitData second_limit;
  std::map<std::string, std::string> field_status;
};

// Pad/Pocket 的强类型载荷；JSON 字段名 native_prism 用于承载这类草图拉伸设计语义。
class NativePrismPayload : public ITypedPayload
{
public:
  explicit NativePrismPayload(const NativePrismData& data) : _data(data) {}
  const char* GetPayloadTypeId() const { return "native_prism"; }
  ITypedPayload* Clone() const { return new NativePrismPayload(*this); }
  void WriteJsonProperty(std::ostream& output) const;
  // 用途：返回只读 Prism 数据，消费者先检查载荷类型标识后再调用。
  const NativePrismData& GetData() const { return _data; }

private:
  NativePrismData _data;
};

struct NativeFeatureParameterField
{
  NativeFeatureParameterField() : has_numeric_value(false), numeric_value(0.0) {}
  std::string name;
  std::string value_type;
  std::string availability;
  std::string source_api;
  std::string reason_code;
  std::string raw_value;
  std::string raw_unit;
  bool has_numeric_value;
  double numeric_value;
  std::string normalized_unit;
};

struct NativeFeatureReferenceField
{
  NativeFeatureReferenceField() : count(-1) {}
  std::string name;
  std::string availability;
  std::string source_api;
  std::string reason_code;
  long count;
  std::vector<std::string> display_names;
};

struct NativeFeatureParameterData
{
  std::string family;
  std::string semantic_kind;
  std::string value_source;
  std::string interface_key;
  std::string decode_status;
  std::string reason_code;
  std::vector<NativeFeatureParameterField> parameters;
  std::vector<NativeFeatureReferenceField> references;
  std::map<std::string, std::string> evidence;
};

class NativeFeatureParameterPayload : public ITypedPayload
{
public:
  explicit NativeFeatureParameterPayload(const NativeFeatureParameterData& data) : _data(data) {}
  const char* GetPayloadTypeId() const { return "native_feature_parameters"; }
  ITypedPayload* Clone() const { return new NativeFeatureParameterPayload(*this); }
  void WriteJsonProperty(std::ostream& output) const;
  const NativeFeatureParameterData& GetData() const { return _data; }

private:
  NativeFeatureParameterData _data;
};

// 单个被枚举对象在 features.jsonl 中对应一条特征记录。
// 记录只包含纯数据，因此离开 CATIA 会话后仍然有效。
struct FeatureRecord
{
  // 用途：为枚举序号和可选载荷设置明确初值，避免出现未初始化状态。
  // 采用 C++03 初始化列表，兼容目标编译器。
  FeatureRecord()
    : native_enumeration_index(0), container_enumeration_index(0), traversal_index(0),
      has_parameter(false), _typed_payload(0) {}
  // 用途：深复制可选类型化载荷，保证记录副本具有独立所有权。
  FeatureRecord(const FeatureRecord& other);
  // 用途：释放本记录独占的类型化载荷。
  ~FeatureRecord();
  // 用途：采用深复制替换当前内容，并正确释放旧载荷。
  FeatureRecord& operator=(const FeatureRecord& other);
  // 用途：接管调用方创建的载荷指针；传入空指针表示清除载荷。
  void SetTypedPayload(ITypedPayload* payload);
  // 用途：返回借用载荷指针，其生命周期不超过当前 FeatureRecord。
  const ITypedPayload* GetTypedPayload() const { return _typed_payload; }
  // 用途：清除并释放当前载荷，供专用解码失败后的通用回退使用。
  void ClearTypedPayload();

  std::string feature_id;
  std::string parent_id;
  long native_enumeration_index;
  long container_enumeration_index;
  long traversal_index;
  TypeFingerprint fingerprint;
  std::string tree_path;
  std::string update_status;
  std::string visibility;
  std::string decoder_id;
  std::string decoder_version;
  std::string decode_level;
  std::string decode_status;
  std::map<std::string, std::string> attributes;
  std::vector<std::string> diagnostic_ids;
  bool has_parameter;
  ParameterValueData parameter;

private:
  ITypedPayload* _typed_payload;
};

// 表示两个已存在中间表示对象之间的有向关系。
struct RelationRecord
{
  std::string kind;
  std::string from_id;
  std::string to_id;
};

// 表示解析诊断；对象编号可将问题关联回对应特征记录。
struct DiagnosticRecord
{
  std::string diagnostic_id;
  std::string severity;
  std::string stage;
  std::string code;
  std::string message;
  std::string feature_id;
};

// parameters.jsonl 的消费索引；参数编号与原始特征编号保持一致。
struct ParameterRecord
{
  // 用途：初始化为“尚无规范化数值”，保留原始字符串作为事实来源。
  ParameterRecord()
    : has_normalized_numeric_value(false), normalized_numeric_value(0.0) {}

  std::string parameter_id;
  std::string owner_feature_id;
  std::string parent_id;
  std::string tree_path;
  std::string parameter_name;
  std::string parameter_kind;
  std::string value_status;
  std::string value_source;
  std::string value_text;
  std::string raw_display_text;
  bool has_normalized_numeric_value;
  double normalized_numeric_value;
  std::string normalized_unit;
  std::string normalization_status;
  std::string decoder_id;
  std::string ownership_status;
  std::vector<std::string> diagnostic_ids;
};

// 记录声明式业务特征的一条分类证据。
struct BusinessFeatureEvidence
{
  std::string kind;
  std::string value;
};

// 业务特征中的参数摘要，可通过参数编号反查参数索引和原始对象。
struct BusinessParameterData
{
  BusinessParameterData()
    : has_normalized_numeric_value(false), normalized_numeric_value(0.0) {}

  std::string parameter_id;
  std::string raw_value;
  bool has_normalized_numeric_value;
  double normalized_numeric_value;
  std::string normalized_unit;
  std::string value_status;
};

// business_features.jsonl 的聚合记录；它不是新的 CAA 对象，不计入对象总数。
struct BusinessFeatureRecord
{
  BusinessFeatureRecord()
    : geometry_recognition_performed(false), native_part_design_feature_confirmed(false) {}

  std::string business_feature_id;
  std::string source_feature_id;
  std::string feature_kind;
  std::string display_name;
  std::string normalized_name;
  std::string recognition_method;
  std::string classification_status;
  std::string confidence;
  std::string container_id;
  std::string tree_path;
  std::vector<std::string> parameter_ids;
  std::map<std::string, BusinessParameterData> parameters;
  std::vector<BusinessFeatureEvidence> evidence;
  bool geometry_recognition_performed;
  bool native_part_design_feature_confirmed;
  std::vector<std::string> diagnostic_ids;
};

// CAA 原生结果体摘要；这是从 R21 Public CATBody/CATTopology 读取的真实拓扑出口。
// 这里仍不保存 CATBody 指针，body_id 只是本次解析版本内的稳定编号。
struct NativeTopologyBodyRecord
{
  // 用途：初始化为不可用状态，只有适配器成功读取 CATBody 后才改成 success。
  NativeTopologyBodyRecord()
    : vertex_count(0), edge_count(0), face_count(0), volume_count(0) {}

  std::string body_id;
  std::string source_feature_id;
  std::string source_kind;
  std::string read_status;
  std::string value_source;
  long vertex_count;
  long edge_count;
  long face_count;
  long volume_count;
  std::string stability_scope;
  std::vector<std::string> diagnostic_ids;
};

// CAA 原生拓扑单元摘要；用于证明 Face/Edge/Vertex 已经从 CATBody 真实枚举。
// 当前阶段不写入内存地址，也不把这些 cell_id 声称为跨模型版本稳定。
struct NativeTopologyCellRecord
{
  // 用途：提供明确初值，避免读取失败路径出现伪造的维度或顺序。
  NativeTopologyCellRecord()
    : topology_index(0), dimension(-1), domain_count(0), internal_domain_count(0),
      has_center(false), area_mm2_available(false), area_mm2(0.0),
      length_mm_available(false), length_mm(0.0), runtime_cell_pointer(0) {}

  std::string cell_id;
  std::string body_id;
  std::string cell_kind;
  long topology_index;
  long dimension;
  long domain_count;
  long internal_domain_count;
  bool has_center;
  double center_mm[3];
  bool area_mm2_available;
  double area_mm2;
  bool length_mm_available;
  double length_mm;
  std::string geometry_status;
  std::string exact_geometry_type;
  std::string geometry_parameters_json;
  std::string parameter_domain_json;
  std::string bounding_box_json;
  std::string geometry_orientation;
  std::string material_side;
  std::string measure_status;
  std::vector<std::string> boundary_cell_ids;
  std::vector<std::string> adjacent_cell_ids;
  std::string outer_wire_id;
  std::vector<std::string> inner_wire_ids;
  std::string stable_id_method;
  std::string value_source;
  const void* runtime_cell_pointer;
  std::vector<std::string> diagnostic_ids;
};

// CAA 原生 Wire/Loop 摘要；用于把 Face 边界环和独立 Wire 从单纯 cell 数量中拆出来。
// wire_id 是本次解析内稳定编号，不代表 CATIA 跨版本持久名称。
struct NativeTopologyWireRecord
{
  // 用途：初始化为未知状态，只有通过 CATBoundaryIterator/CATDomain 读取后才写入完整信息。
  NativeTopologyWireRecord()
    : wire_index(0), owning_face_topology_index(0), edge_count(0), closed_status("unknown") {}

  std::string wire_id;
  std::string body_id;
  long wire_index;
  std::string wire_kind;
  std::string owning_face_id;
  long owning_face_topology_index;
  long edge_count;
  std::string closed_status;
  std::vector<std::string> edge_cell_ids;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// CAA 原生 Coedge 摘要；每条记录表示一个 Face Loop 中对 Edge 的一次有向使用。
// R21 Public CATBoundaryIterator 暴露 CATSide，但不提供跨会话持久 coedge identity。
struct NativeTopologyCoedgeRecord
{
  NativeTopologyCoedgeRecord()
    : coedge_index(0), coedge_index_in_wire(0), edge_orientation_side(0) {}

  std::string coedge_id;
  std::string body_id;
  std::string wire_id;
  std::string owning_face_id;
  std::string edge_cell_id;
  std::string previous_coedge_id;
  std::string next_coedge_id;
  long coedge_index;
  long coedge_index_in_wire;
  short edge_orientation_side;
  std::string orientation_status;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// CAA 三角化到 Face 的映射摘要；这是给 GLB/轻量化阶段使用的 Face→Triangle Range 契约。
struct NativeMeshFaceMapRecord
{
  // 用途：给三角范围和迭代器计数提供明确零值。
  NativeMeshFaceMapRecord()
    : primitive_index(0), triangle_start(0), triangle_count(0), point_count(0),
      isolated_triangle_count(0), strip_count(0), fan_count(0), polygon_count(0),
      estimated_triangle_count(0), face_orientation_side(0), planar(false) {}

  std::string mesh_map_id;
  std::string body_id;
  std::string face_cell_id;
  long primitive_index;
  long triangle_start;
  long triangle_count;
  long point_count;
  long isolated_triangle_count;
  long strip_count;
  long fan_count;
  long polygon_count;
  long estimated_triangle_count;
  short face_orientation_side;
  bool planar;
  std::string tessellation_status;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// CAA 原生三角形 Sidecar；每条三角形直接继承 B-Rep Face ID，避免后续按距离猜测。
struct NativeMeshTriangleRecord
{
  NativeMeshTriangleRecord()
    : triangle_index(0), triangle_index_in_face(0), normal_available(false)
  {
    vertex_ranks[0] = vertex_ranks[1] = vertex_ranks[2] = 0;
    int i = 0;
    for (; i < 9; ++i) vertices_mm[i] = 0.0;
    normal[0] = normal[1] = normal[2] = 0.0;
  }

  std::string triangle_id;
  std::string mesh_map_id;
  std::string body_id;
  std::string face_cell_id;
  long triangle_index;
  long triangle_index_in_face;
  int vertex_ranks[3];
  double vertices_mm[9];
  bool normal_available;
  double normal[3];
  std::string source_primitive;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// FTA/TPS 集合级摘要；当前只记录 R21 Public 接口能可靠取得的集合和数量。
// 详细公差语义和 TPS->Face 映射必须等后续逐类接口验证后再输出。
struct FtaSetRecord
{
  // 用途：初始化计数为零，避免在接口失败时留下随机值。
  FtaSetRecord() : set_index(0), tps_count(0), geometry_count(0) {}

  std::string fta_set_id;
  long set_index;
  std::string read_status;
  std::string value_source;
  long tps_count;
  long geometry_count;
  std::string semantic_detail_status;
  std::string topology_mapping_status;
  std::vector<std::string> diagnostic_ids;
};

// FTA/TPS 组件级语义摘要；比 Set 数量更细，但仍不声称已完成全部 GD&T 逐类参数解析。
struct FtaSemanticRecord
{
  // 用途：初始化为未读取；每个 TPS 组件按所在 Set 和组件顺序生成本轮稳定编号。
  FtaSemanticRecord()
    : component_index(0), semantic_interface_count(0), all_semantic_interface_count(0),
      semantic_check_status_raw(-1) {}

  std::string fta_semantic_id;
  std::string fta_set_id;
  long component_index;
  std::string read_status;
  std::string component_kind;
  std::vector<std::string> supported_interface_keys;
  long semantic_interface_count;
  long all_semantic_interface_count;
  std::string validation_text;
  std::string validation_text_status;
  long semantic_check_status_raw;
  std::string semantic_check_diagnostic;
  std::string topology_mapping_status;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// 原生设计特征 ResultOUT 拓扑摘要；用于区分“特征有结果体”和“已映射到最终 Face”。
// 当前只读取 ResultOUT 对应 CATBody 的数量，不把这些面伪装成最终主实体面。
struct NativeFeatureResultRecord
{
  // 用途：初始化计数为零，读取失败时仍能输出明确状态而不是随机数。
  NativeFeatureResultRecord()
    : vertex_count(0), edge_count(0), face_count(0), volume_count(0) {}

  std::string result_id;
  std::string source_feature_id;
  std::string source_kind;
  std::string read_status;
  std::string value_source;
  long vertex_count;
  long edge_count;
  long face_count;
  long volume_count;
  std::string final_body_mapping_status;
  std::vector<std::string> diagnostic_ids;
};

// 原生设计特征 ResultOUT 内部 cell 明细；它是特征结果体的真实拓扑，不等同于最终主实体 Face。
// result_cell_id 只在本次解析内稳定，用于后续和最终拓扑候选匹配。
struct NativeFeatureResultCellRecord
{
  // 用途：初始化 Result cell 的数值状态，读取失败时不输出伪造测量。
  NativeFeatureResultCellRecord()
    : result_cell_index(0), dimension(-1), has_center(false),
      area_mm2_available(false), area_mm2(0.0), length_mm_available(false), length_mm(0.0),
      runtime_cell_pointer(0) {}

  std::string result_cell_id;
  std::string result_id;
  std::string source_feature_id;
  std::string source_kind;
  long result_cell_index;
  long dimension;
  std::string cell_kind;
  bool has_center;
  double center_mm[3];
  bool area_mm2_available;
  double area_mm2;
  bool length_mm_available;
  double length_mm;
  std::vector<std::string> boundary_result_cell_ids;
  std::string read_status;
  std::string stable_id_method;
  std::string value_source;
  const void* runtime_cell_pointer;
  std::vector<std::string> diagnostic_ids;
};

// ResultOUT cell 到最终主实体 cell 的映射尝试记录；当前只记录可审计候选，不冒充 CATIA Generic Naming 权威映射。
struct NativeFeatureTopologyLinkRecord
{
  // 用途：初始化为未匹配；只有满足明确几何指纹条件时才写入候选 final_cell_id。
  NativeFeatureTopologyLinkRecord()
    : confidence(0.0), center_residual_mm(0.0), measure_residual(0.0), candidate_count(0) {}

  std::string link_id;
  std::string source_feature_id;
  std::string result_id;
  std::string result_cell_id;
  std::string final_cell_id;
  std::string final_body_id;
  std::string mapping_direction;
  std::string mapping_status;
  std::string mapping_method;
  std::string authority;
  std::string persistent_reference;
  std::string relation_kind;
  double confidence;
  double center_residual_mm;
  double measure_residual;
  long candidate_count;
  std::vector<std::string> candidate_final_cell_ids;
  std::vector<std::string> diagnostic_ids;
};

struct FtaTopologyLinkRecord
{
  FtaTopologyLinkRecord() : confidence(0.0) {}

  std::string fta_link_id;
  std::string fta_semantic_id;
  std::string final_cell_id;
  std::string final_body_id;
  std::string geometry_reference_kind;
  std::string mapping_status;
  std::string mapping_method;
  std::string authority;
  double confidence;
  std::vector<std::string> diagnostic_ids;
};

struct ProductReferenceRecord
{
  ProductReferenceRecord() : child_count(0), representation_count(0) {}

  std::string reference_id;
  std::string part_number;
  std::string display_name;
  std::string source_document;
  std::string default_representation;
  std::string read_status;
  std::string value_source;
  long child_count;
  long representation_count;
  std::vector<std::string> diagnostic_ids;
};

struct ProductInstanceRecord
{
  ProductInstanceRecord() : depth(0), child_index(0), child_count(0), transform_4x4(16, 0.0)
  {
    transform_4x4[0] = 1.0;
    transform_4x4[5] = 1.0;
    transform_4x4[10] = 1.0;
    transform_4x4[15] = 1.0;
  }

  std::string instance_id;
  std::string parent_instance_id;
  std::string reference_id;
  std::string instance_name;
  std::string tree_path;
  long depth;
  long child_index;
  long child_count;
  std::vector<double> transform_4x4;
  std::string transform_status;
  std::string transform_value_source;
  std::string read_status;
  std::string value_source;
  std::vector<std::string> diagnostic_ids;
};

// 原生树节点是显示层事实记录；未知对象也必须保留，不依赖语义 Decoder 成功。
struct NativeTreeNodeRecord
{
  NativeTreeNodeRecord()
    : source_index(0), traversal_index(0), has_children(false),
      has_geometry(false), properties_available(false) {}

  std::string node_id;
  std::string parent_id;
  std::string display_text;
  std::string display_name;
  std::string internal_name;
  std::string startup_type;
  std::string document_kind;
  std::string node_kind;
  long source_index;
  long traversal_index;
  std::string tree_path;
  std::string instance_id;
  std::string parent_instance_id;
  std::string reference_id;
  std::string source_feature_id;
  std::string topology_id;
  std::vector<std::string> topology_ids;
  std::vector<std::string> mesh_face_ids;
  std::string source_node_id;
  bool has_children;
  bool has_geometry;
  bool properties_available;
  std::map<std::string, std::string> attributes;
  std::vector<std::string> diagnostic_ids;
};

// CATIA 属性页的扁平字段输出；读取失败的高级属性不能阻断主树输出。
struct NodePropertyRecord
{
  NodePropertyRecord() : display_order(0), read_only(true) {}

  std::string node_id;
  std::string tab_id;
  std::string tab_label;
  std::string group_id;
  std::string group_label;
  std::string field_key;
  std::string field_label;
  std::string value;
  std::string unit;
  std::string value_type;
  std::string source;
  long display_order;
  bool read_only;
};

// 解码器执行终态；候选判断与执行结果分离，避免把 StartUp 预筛选误当成类型化成功。
enum DecoderOutcome
{
  DecoderOutcomeNotMatched = 0,
  DecoderOutcomeUnsupported = 1,
  DecoderOutcomeSuccess = 2,
  DecoderOutcomePartial = 3,
  DecoderOutcomeException = 4,
  DecoderOutcomeRejected = 5,
  DecoderOutcomeConflict = 6
};

// 解码器返回的统一结果，用于选择类型化、通用或不透明兜底路径。
struct DecodeResult
{
  // 用途：构造解码结果；旧调用未显式给出终态时按 success 推导成功或拒绝。
  // 入参使用 const char* 以兼容 C++03，成员仍以 std::string 保存。
  DecodeResult(bool ok = true, const char* result_level = "typed", const char* detail = "",
               DecoderOutcome result_outcome = DecoderOutcomeSuccess)
    : success(ok), level(result_level), message(detail),
      outcome(ok ? result_outcome :
        (result_outcome == DecoderOutcomeSuccess ? DecoderOutcomeRejected : result_outcome)) {}

  bool success;
  std::string level;
  std::string message;
  DecoderOutcome outcome;
};

// 保存单次解析的统计值，并负责校验各类数量守恒。
struct ParseStatistics
{
  // 用途：把本轮所有计数器和耗时初始化为零。
  ParseStatistics();
  // 用途：校验对象总数等于类型化、通用、不透明和失败对象之和。
  // 返回假时批处理应以校验失败退出。
  bool IsConserved() const;
  // 用途：校验参数总数等于成功、部分成功、不可用和失败之和。
  bool IsParameterConserved() const;
  // 用途：校验声明式业务特征总数等于各分类数量之和。
  bool IsBusinessFeatureConserved() const;
  // 用途：校验所有原生孔候选都恰好进入成功、部分成功、不支持或异常之一。
  bool IsNativeHoleConserved() const;
  // 用途：按接口、原生类型、解码器和探测结果累计接口探测统计。
  void RecordProbe(const std::string& interface_key, const std::string& native_type,
                   const std::string& decoder_id, const std::string& result);

  long enumerated_total;
  long typed_count;
  long generic_count;
  long opaque_count;
  long failed_count;
  long container_count;
  long relation_count;
  long unknown_native_type_count;
  long probe_supported_count;
  long probe_unsupported_count;
  long probe_exception_count;
  long probe_not_attempted_count;
  long not_up_to_date_count;
  long parameter_total;
  long parameter_value_success;
  long parameter_value_partial;
  long parameter_value_unavailable;
  long parameter_failed;
  long declared_business_feature_total;
  long declared_boss_count;
  long declared_hole_count;
  long declared_slot_count;
  long declared_unknown_count;
  long business_feature_with_parameter_count;
  long business_feature_with_all_values_count;
  long business_feature_with_partial_values_count;
  long business_feature_without_values_count;
  long orphan_parameter_count;
  long ambiguous_parameter_owner_count;
  long native_hole_candidate_count;
  long native_hole_success_count;
  long native_hole_partial_count;
  long native_hole_unsupported_count;
  long native_hole_exception_count;
  long document_open_ms;
  long traversal_ms;
  long decoder_ms;
  long output_ms;
  long total_ms;
  std::map<std::string, long> decoder_hits;
  std::map<std::string, long> decoder_outcome_counts;
  std::map<std::string, long> probe_outcome_counts;
  std::map<std::string, long> not_up_to_date_by_native_type;
  std::map<std::string, long> not_up_to_date_by_decoder;
  std::vector<std::string> not_up_to_date_feature_ids;
};

// 保存清单文件所需的版本、输入文件、运行环境和追溯元数据。
struct ParseMetadata
{
  ParseMetadata() : input_size_bytes(0), include_source_path(false) {}

  std::string schema_version;
  std::string parser_version;
  std::string registry_version;
  std::string decoder_bundle_version;
  std::string parser_git_commit;
  std::string parser_git_commit_source;
  std::string build_timestamp_utc;
  std::string build_timestamp_source;
  std::string execution_started_utc;
  std::string execution_finished_utc;
  std::string input_file_name;
  std::string document_kind;
  std::string input_source_path;
  unsigned long input_size_bytes;
  std::string input_sha256;
  bool include_source_path;
  std::string runtime_catia_release;
  std::string runtime_service_pack;
  std::string runtime_hotfix;
  std::string runtime_value_source;
  std::string source_hint_release;
  std::string source_hint_service_pack;
  std::string source_hint_hotfix;
  std::string source_hint_value_source;
  std::string source_hint_confidence;
  std::vector<std::string> discovery_entrypoints;
  std::string discovery_coverage_scope;
};

// 汇总单次解析的统计、诊断、运行信息和元数据。
class ParseContext
{
public:
  // 用途：生成稳定诊断编号，将诊断加入上下文并返回编号供对象引用。
  // 文本入参使用 const char*，以兼容现有调用和 C++03 编译器。
  std::string AddDiagnostic(const char* severity, const char* stage, const char* code,
                            const char* message, const std::string& feature_id);

  ParseStatistics statistics;
  std::vector<DiagnosticRecord> diagnostics;
  std::vector<NativeTopologyBodyRecord> topology_bodies;
  std::vector<NativeTopologyCellRecord> topology_cells;
  std::vector<NativeTopologyWireRecord> topology_wires;
  std::vector<NativeTopologyCoedgeRecord> topology_coedges;
  std::vector<NativeMeshFaceMapRecord> mesh_face_maps;
  std::vector<NativeMeshTriangleRecord> mesh_triangles;
  std::vector<FtaSetRecord> fta_sets;
  std::vector<FtaSemanticRecord> fta_semantics;
  std::vector<FtaTopologyLinkRecord> fta_topology_links;
  std::vector<ProductReferenceRecord> product_references;
  std::vector<ProductInstanceRecord> product_instances;
  std::vector<NativeTreeNodeRecord> native_tree_nodes;
  std::vector<NodePropertyRecord> node_properties;
  std::vector<NativeFeatureResultRecord> native_feature_results;
  std::vector<NativeFeatureResultCellRecord> native_feature_result_cells;
  std::vector<NativeFeatureTopologyLinkRecord> native_feature_topology_links;
  std::map<std::string, std::string> runtime_info;
  ParseMetadata metadata;
};

// 字符串参数专用接口的读取结果，用于区分成功、不支持和异常。
enum StringParameterReadStatus
{
  StringParameterReadSuccess = 0,
  StringParameterInterfaceUnsupported = 1,
  StringParameterQueryException = 2,
  StringParameterValueException = 3
};

// CAA 适配器查询原生孔专用接口及读取必需值时返回的对象级结果。
enum NativeHoleReadStatus
{
  NativeHoleReadSuccess = 0,
  NativeHoleInterfaceUnsupported = 1,
  NativeHoleInterfaceQueryException = 2,
  NativeHoleRequiredValueReadException = 3
};

// 原生 Prism 读取状态；Pad/Pocket 使用同一套对象级隔离结果。
enum NativePrismReadStatus
{
  NativePrismReadSuccess = 0,
  NativePrismInterfaceUnsupported = 1,
  NativePrismInterfaceQueryException = 2,
  NativePrismRequiredValueReadException = 3
};

enum NativeFeatureParameterReadStatus
{
  NativeFeatureParameterReadSuccess = 0,
  NativeFeatureParameterInterfaceUnsupported = 1,
  NativeFeatureParameterInterfaceQueryException = 2,
  NativeFeatureParameterReadPartial = 3
};

// 解码器的候选判断只是预筛选，不等价于专用接口已经确认成功。
enum DecoderMatchStatus
{
  DecoderNotCandidate = 0,
  DecoderCandidate = 1
};

// 通用原生解码器调用上下文，后续特征扩展不需要向爬取流程增加分支。
struct DecoderContext
{
  std::string feature_id;
  std::string startup_type;
};

// 字符串参数的 API 无关视图；具体 CAA 接口查询只在适配器层发生。
class IStringParameterView
{
public:
  virtual ~IStringParameterView() {}
  // 用途：读取字符串参数的真实值，并返回可精确分类的读取状态。
  virtual StringParameterReadStatus ReadStringParameter(ParameterValueData& parameter,
                                                        std::string& error) const = 0;
};

// 所有原生能力共用的查询边界。能力对象由原生适配器拥有，调用方只可在当前对象生命周期内借用。
class INativeCapabilityView
{
public:
  virtual ~INativeCapabilityView() {}
  // 用途：返回稳定能力标识；调用方必须先比较该标识，再转换到具体能力接口。
  virtual const char* GetCapabilityId() const = 0;
  // 用途：返回进程内稳定类型令牌；Decoder 必须同时核对编号和令牌后才能向下转换。
  virtual const void* GetCapabilityTypeToken() const { return 0; }
};

// 原生孔的 API 无关适配边界；具体接口查询和引用释放只在 CAA 层发生。
class INativeHoleView : public INativeCapabilityView
{
public:
  virtual ~INativeHoleView() {}
  // 用途：声明本视图提供原生孔能力，避免中央对象视图增加专用 Getter。
  const char* GetCapabilityId() const { return "NativeHole"; }
  // 用途：提供原生孔强类型令牌，防止仅凭同名字符串执行不安全转换。
  static const void* TypeToken();
  const void* GetCapabilityTypeToken() const { return TypeToken(); }
  // 用途：读取并验证专用原生孔数据，返回值决定类型化结果或通用回退。
  virtual NativeHoleReadStatus ReadNativeHole(NativeHoleData& output,
                                              std::string& error) const = 0;
};

// 原生 Pad/Pocket 的 API 无关适配边界；具体 CATIAPad/CATIAPocket/CATIAPrism 查询留在 CAA 层。
class INativePrismView : public INativeCapabilityView
{
public:
  virtual ~INativePrismView() {}
  // 用途：返回 NativePad 或 NativePocket，便于 Decoder 严格确认能力种类。
  virtual const char* GetCapabilityId() const = 0;
  // 用途：提供 Prism 强类型令牌，防止仅凭同名字符串执行不安全转换。
  static const void* TypeToken();
  const void* GetCapabilityTypeToken() const { return TypeToken(); }
  // 用途：读取 Pad/Pocket 的真实 Prism 参数，requested_capability 必须是 NativePad 或 NativePocket。
  virtual NativePrismReadStatus ReadNativePrism(const char* requested_capability,
                                                NativePrismData& output,
                                                std::string& error) const = 0;
};

class INativeFeatureParameterView : public INativeCapabilityView
{
public:
  virtual ~INativeFeatureParameterView() {}
  const char* GetCapabilityId() const { return "NativeFeatureParameters"; }
  static const void* TypeToken();
  const void* GetCapabilityTypeToken() const { return TypeToken(); }
  virtual NativeFeatureParameterReadStatus ReadNativeFeatureParameters(
    const char* canonical_family,
    NativeFeatureParameterData& output,
    std::string& error) const = 0;
};

// 原生对象的最小 API 无关视图。
// 解码器通过该视图读取对象，不直接依赖 CAA 头文件或对象指针。
class INativeObjectView
{
public:
  // 用途：通过虚析构保证派生视图可以安全释放。
  virtual ~INativeObjectView() {}
  // 用途：返回构造完成的稳定类型指纹。
  virtual const TypeFingerprint& GetFingerprint() const = 0;
  // 用途：返回可选的字符串参数视图，避免使用 C++ 运行时类型识别。
  virtual const IStringParameterView* GetStringParameterView() const { return 0; }
  // 用途：按稳定能力标识查询可选原生视图；新增能力不再修改本接口的方法列表。
  virtual const INativeCapabilityView* FindCapability(const char*) const { return 0; }
  // 用途：读取所有对象都应具备的基础属性。
  // 返回假时通过 error 说明原因，注册中心随后可进入不透明兜底。
  virtual bool ReadBasicAttributes(FeatureRecord& output, std::string& error) const = 0;
};

// 结果写出接口，统一负责生成结构化文件，业务代码不手工拼接 JSON。
class IArtifactWriter
{
public:
  // 用途：通过虚析构安全释放具体写出器。
  virtual ~IArtifactWriter() {}
  // 用途：把本轮对象、关系、参数、业务特征和统计写入目标目录。
  // 返回假时在 error 中给出失败原因。
  virtual bool Write(const std::vector<FeatureRecord>& features,
                     const std::vector<RelationRecord>& relations,
                     const std::vector<ParameterRecord>& parameters,
                     const std::vector<BusinessFeatureRecord>& business_features,
                     ParseContext& context,
                     const std::string& output_dir,
                     std::string& error) = 0;
};

// 所有对象解码器共同遵循的 API 无关契约。
// 解码器只处理一个对象，不负责全局遍历或结果文件写出。
class IFeatureDecoder
{
public:
  // 用途：通过虚析构安全释放具体解码器。
  virtual ~IFeatureDecoder() {}
  // 用途：返回稳定解码器编号，用于统计、冲突决胜和结果追溯。
  virtual const char* GetDecoderId() const = 0;
  // 用途：返回显式优先级；数值越大，匹配优先级越高。
  virtual int GetPriority() const = 0;
  // 用途：判断当前对象是否是该解码器的候选。
  virtual bool Match(const TypeFingerprint& fingerprint,
                     const INativeObjectView& object_view) const = 0;
  // 用途：读取对象并填充输出记录，同时把对象级诊断写入上下文。
  // 解码失败由注册中心统一进入通用或不透明兜底。
  virtual DecodeResult Decode(const INativeObjectView& object_view,
                              ParseContext& context,
                              FeatureRecord& output) = 0;
  // 用途：声明本解码器失败后注册中心能否继续尝试下一候选；默认立即统一回退。
  virtual bool ContinueTypedAfterFailure() const { return false; }
};

// 原生设计特征解码器的通用扩展协议；注册中心仍只依赖基础解码器接口。
class INativeFeatureDecoder : public IFeatureDecoder
{
public:
  virtual ~INativeFeatureDecoder() {}
  // 用途：标识解码器所属特征家族，供诊断和后续注册目录稳定扩展。
  virtual const char* GetFeatureFamily() const = 0;
  // 用途：执行低成本候选预筛选；专用接口确认仍必须发生在解码函数中。
  virtual DecoderMatchStatus GetMatchStatus(const TypeFingerprint&,
                                            const INativeObjectView&) const = 0;
  // 用途：统一规定专用解码失败后是否继续尝试其他类型化解码器。
  virtual bool ContinueTypedAfterFailure() const { return false; }
};

// 使用 R21 公开原生孔接口确认并读取 Part Design Hole 的唯一专用解码器。
class NativeHoleDecoder : public INativeFeatureDecoder
{
public:
  const char* GetDecoderId() const;
  int GetPriority() const;
  const char* GetFeatureFamily() const;
  DecoderMatchStatus GetMatchStatus(const TypeFingerprint&,
                                    const INativeObjectView&) const;
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  // 用途：专用接口不支持或读取失败后，允许其他已注册 Typed Decoder 继续确认。
  bool ContinueTypedAfterFailure() const { return true; }
};

// 使用 R21 公开 CATIAPad/CATIAPrism 接口确认并读取 Part Design Pad。
class NativePadDecoder : public INativeFeatureDecoder
{
public:
  const char* GetDecoderId() const;
  int GetPriority() const;
  const char* GetFeatureFamily() const;
  DecoderMatchStatus GetMatchStatus(const TypeFingerprint&,
                                    const INativeObjectView&) const;
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  bool ContinueTypedAfterFailure() const { return true; }
};

// 使用 R21 公开 CATIAPocket/CATIAPrism 接口确认并读取 Part Design Pocket。
class NativePocketDecoder : public INativeFeatureDecoder
{
public:
  const char* GetDecoderId() const;
  int GetPriority() const;
  const char* GetFeatureFamily() const;
  DecoderMatchStatus GetMatchStatus(const TypeFingerprint&,
                                    const INativeObjectView&) const;
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  bool ContinueTypedAfterFailure() const { return true; }
};

class NativeFeatureParameterDecoder : public INativeFeatureDecoder
{
public:
  const char* GetDecoderId() const;
  int GetPriority() const;
  const char* GetFeatureFamily() const;
  DecoderMatchStatus GetMatchStatus(const TypeFingerprint&,
                                    const INativeObjectView&) const;
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  bool ContinueTypedAfterFailure() const { return true; }
};

// 使用 R21 CATICkeParm 接口读取知识工程字符串参数的专用解码器。
class KnowledgewareStringParameterDecoder : public IFeatureDecoder
{
public:
  // 用途：返回字符串参数解码器的稳定编号。
  const char* GetDecoderId() const;
  // 用途：让类型化字符串参数读取优先于通用兜底。
  int GetPriority() const;
  // 用途：通过类型指纹筛选字符串参数候选。
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  // 用途：调用字符串参数视图读取真实值；失败后由注册中心统一回退。
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  // 用途：当前字符串接口不可用时允许其他类型化能力继续尝试。
  bool ContinueTypedAfterFailure() const { return true; }
};

// 基于 CATIA StartUp 类型指纹识别尚无参数级适配器的原生特征身份。
class StartupTypeCanonicalDecoder : public IFeatureDecoder
{
public:
  const char* GetDecoderId() const;
  int GetPriority() const;
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
  bool ContinueTypedAfterFailure() const { return true; }
};

// 未命中专用类型时读取基础属性的通用解码器。
class GenericFeatureDecoder : public IFeatureDecoder
{
public:
  // 用途：返回通用解码器的稳定编号。
  const char* GetDecoderId() const;
  // 用途：使通用解码器低于所有专用解码器。
  int GetPriority() const;
  // 用途：接受所有尚未被专用解码器接管的对象。
  bool Match(const TypeFingerprint&, const INativeObjectView&) const;
  // 用途：读取基础属性；失败时交给不透明对象记录器。
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord&);
};

// 基础属性也无法读取时，保留对象存在性、树位置和失败信息。
class OpaqueObjectRecorder
{
public:
  // 用途：生成不透明对象记录，并把失败原因写入诊断。
  DecodeResult Record(const INativeObjectView&, ParseContext&, FeatureRecord&,
                      const std::string& stage, const std::string& reason);
};

// 保存编译期注册的解码器，并提供确定、可复现的选择逻辑。
// 注册中心不拥有解码器内存，调用方负责保证对象生命周期。
class DecoderRegistry
{
public:
  // 用途：注册一个非空解码器指针。
  void Register(IFeatureDecoder* decoder);
  // 用途：按优先级和稳定编号收集全部候选；返回指针仍由调用方拥有。
  void FindCandidates(const TypeFingerprint&, const INativeObjectView&, ParseContext&,
                      std::vector<IFeatureDecoder*>& candidates) const;

private:
  std::vector<IFeatureDecoder*> _decoders;
};

// 根据对象视图构造稳定类型指纹。
class FeatureTypeFingerprintBuilder
{
public:
  // 用途：读取并返回对象已经建立的类型指纹。
  static std::string StableKey(const TypeFingerprint& fingerprint);
};

// 统计解析过程中实际观察到的稳定类型指纹。
class FeatureTypeCatalog
{
public:
  // 用途：记录一个类型指纹的稳定键。
  void Observe(const TypeFingerprint& fingerprint);
  // 用途：返回不同稳定类型指纹的数量。
  size_t Count() const;

private:
  std::set<std::string> _keys;
};

// 封装解码器优先级和稳定编号的确定性比较规则。
class DecoderMatchEngine
{
public:
  // 用途：判断候选是否应替换当前最佳解码器。
  static bool IsBetter(const IFeatureDecoder* candidate, const IFeatureDecoder* current);
};

// 只探测已注册且经 R21 资料验证的接口集合。
class InterfaceProbeService
{
public:
  // 用途：记录一次接口探测结果及其统计维度。
  virtual ~InterfaceProbeService() {}
  // 用途：把已确认支持的接口键加入类型指纹，并保持去重。
  virtual std::string Probe(const char* interface_key, TypeFingerprint& fingerprint,
                            ParseStatistics& statistics) = 0;
};

// 收集最终进入通用或不透明路径的未知原生类型。
class UnknownTypeCollector
{
public:
  // 用途：记录一个未知原生类型；空值统一记为未知。
  void Observe(const TypeFingerprint& fingerprint);
  // 用途：返回未知原生类型的不同取值数量。
  size_t Count() const;

private:
  std::set<std::string> _unknown_types;
};

// 集中执行对象、参数、业务特征和原生孔的守恒校验。
class CoverageTracker
{
public:
  // 用途：返回所有守恒校验的综合结果。
  static bool Validate(const ParseStatistics& statistics);
};

// 在完整匹配“数字加明确单位”时生成辅助规范化值。
class ParameterValueNormalizer
{
public:
  static void Normalize(ParameterValueData& parameter);
};

// 沿父子关系解析最近业务祖先，并区分孤立参数和多归属歧义。
class ParameterOwnershipResolver
{
public:
  static std::string Resolve(const std::string& parameter_id,
                             const std::vector<FeatureRecord>& features,
                             const std::vector<RelationRecord>& relations,
                             std::string& status);
};

// 从原始特征构建 parameters.jsonl 消费索引并更新覆盖率。
class ParameterRecordBuilder
{
public:
  static void Build(const std::vector<FeatureRecord>& features,
                    const std::vector<RelationRecord>& relations,
                    ParseContext& context,
                    std::vector<ParameterRecord>& output);
};

// 提供实例名称规范化和声明式业务特征分类规则。
class BusinessFeatureRuleCatalog
{
public:
  static std::string NormalizeInstanceName(const std::string& name);
  static std::string KindFromName(const std::string& normalized_name);
};

// 将 GSMTool 声明节点及其参数聚合为独立的声明式业务特征记录。
class DeclaredBusinessFeatureAggregator
{
public:
  static void Aggregate(const std::vector<FeatureRecord>& features,
                        const std::vector<RelationRecord>& relations,
                        const std::vector<ParameterRecord>& parameters,
                        ParseContext& context,
                        std::vector<BusinessFeatureRecord>& output);
};

// 协调专用解码器、通用解码器和不透明记录器，爬取流程无需类型分支。
class FeatureTypeRegistry
{
public:
  // 用途：注册内置专用解码器，并保存通用和不透明兜底实现。
  FeatureTypeRegistry();
  // 用途：允许后续模块以同一协议增加专用解码器。
  void Register(IFeatureDecoder* decoder);
  // 用途：为单个对象选择解码器；失败时按统一规则进入通用或不透明路径。
  DecodeResult DecodeObject(const INativeObjectView&, ParseContext&, FeatureRecord&);

private:
  DecoderRegistry _registry;
  GenericFeatureDecoder _generic;
  OpaqueObjectRecorder _opaque;
};

// 生成单次解析范围内稳定、连续且不依赖指针地址的特征编号。
class FeatureIdGenerator
{
public:
  // 用途：把下一个编号初始化为一。
  FeatureIdGenerator() : _next(0) {}
  // 用途：返回形如 F000001 的编号并推进计数器。
  std::string Next();

private:
  long _next;
};

// 对 UTF-8 文本执行 JSON 字符串转义。
std::string JsonEscape(const std::string& value);

// 计算内存文本的 SHA-256 小写十六进制摘要。
std::string Sha256String(const std::string& value);
// 计算文件的 SHA-256；读取失败时通过 error 返回原因。
std::string Sha256File(const std::string& path, std::string& error);
// 根据 include_source_path 决定返回完整路径还是仅文件名。
std::string SourcePathForOutput(const std::string& path, bool include_source_path);
// 返回当前协调世界时的 ISO-8601 文本。
std::string UtcNowIso8601();
// 从 CATPart 文件头提取版本提示；该提示不得冒充 CATIA 运行时版本。
void ReadSourceFileHint(const std::string& path, ParseMetadata& metadata);

// 运行不依赖 CATIA 许可证的核心自测，供批处理的 --self-test 参数调用。
class SelfTestSuite
{
public:
  // 用途：执行全部核心自测；成功返回零，失败返回非零。
  int RunAll();
};
}

#endif
