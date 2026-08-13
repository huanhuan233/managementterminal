// 本文件把纯数据 IR 事务式序列化为 JSON/JSONL；请求目录只会出现完整的一套结果。
#include "CadParseIR.h"

#include <direct.h>
#include <errno.h>
#include <algorithm>
#include <ctime>
#include <float.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <windows.h>

namespace cadparse
{
namespace
{
// 用途：连接 Windows 目录与产物文件名，并兼容末尾已有分隔符的目录。
std::string JoinPath(const std::string& directory, const char* name)
{
  if (directory.empty()) return name;
  const char last = directory[directory.size() - 1];
  return directory + ((last == '\\' || last == '/') ? "" : "\\") + name;
}

// 用途：从左到右逐级创建目录；已存在目录不视为错误。
bool EnsureDirectory(const std::string& path, std::string& error)
{
  if (path.empty()) { error = "output directory is empty"; return false; }
  std::string current;
  std::string::size_type i = 0;
  if (path.size() > 1 && path[1] == ':') { current = path.substr(0, 2); i = 2; }
  for (; i <= path.size(); ++i)
  {
    if (i < path.size() && path[i] != '\\' && path[i] != '/') { current += path[i]; continue; }
    if (!current.empty() && current[current.size() - 1] != ':' &&
        _mkdir(current.c_str()) != 0 && errno != EEXIST)
    { error = std::string("cannot create output directory: ") + current; return false; }
    if (i < path.size() && (current.empty() || current[current.size() - 1] != '\\')) current += '\\';
  }
  return true;
}

// 用途：递归删除仅由本 Writer 生成的 staging/backup 目录，支持事务清理和回滚。
bool RemoveTree(const std::string& path)
{
  const DWORD attributes = GetFileAttributesA(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return true;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
  {
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(path.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
  }
  WIN32_FIND_DATAA data;
  HANDLE find = FindFirstFileA(JoinPath(path, "*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE)
  {
    do
    {
      const std::string name = data.cFileName;
      if (name == "." || name == "..") continue;
      const std::string child = JoinPath(path, name.c_str());
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(child);
      else { SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(child.c_str()); }
    } while (FindNextFileA(find, &data));
    FindClose(find);
  }
  SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  return RemoveDirectoryA(path.c_str()) != 0 || GetLastError() == ERROR_PATH_NOT_FOUND;
}

// 用途：原子切换 staging 和正式目录；旧结果先改名为 backup，失败时可恢复。
bool CommitStaging(const std::string& staging, const std::string& output_dir, std::string& error)
{
  const std::string backup = output_dir + ".cadparse_backup";
  RemoveTree(backup);
  const bool had_output = GetFileAttributesA(output_dir.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (had_output && !MoveFileA(output_dir.c_str(), backup.c_str()))
  { error = "cannot move previous output to transaction backup"; return false; }
  if (!MoveFileA(staging.c_str(), output_dir.c_str()))
  {
    if (had_output) MoveFileA(backup.c_str(), output_dir.c_str());
    error = "cannot commit transaction staging directory";
    return false;
  }
  if (had_output) RemoveTree(backup);
  return true;
}

// 用途：把字符串数组按现有顺序写成 JSON，所有元素统一转义。
void WriteStringArray(std::ostream& output, const std::vector<std::string>& values)
{
  output << '[';
  std::vector<std::string>::const_iterator it = values.begin();
  for (; it != values.end(); ++it) { if (it != values.begin()) output << ','; output << '"' << JsonEscape(*it) << '"'; }
  output << ']';
}

// 用途：把稳定排序的 string→string map 写成 JSON 对象。
void WriteStringMap(std::ostream& output, const std::map<std::string, std::string>& values)
{
  output << '{';
  std::map<std::string, std::string>::const_iterator it = values.begin();
  for (; it != values.end(); ++it) { if (it != values.begin()) output << ','; output << '"' << JsonEscape(it->first) << "\":\"" << JsonEscape(it->second) << '"'; }
  output << '}';
}

// 用途：把 double 数组按原始顺序写成 JSON number 数组。
void WriteDoubleArray(std::ostream& output, const std::vector<double>& values)
{
  output << '[';
  std::vector<double>::const_iterator it = values.begin();
  for (; it != values.end(); ++it)
  {
    if (it != values.begin()) output << ',';
    output << std::setprecision(15) << *it;
  }
  output << ']';
}

// 用途：把稳定排序的 string→long map 写成 JSON 对象。
void WriteCountMap(std::ostream& output, const std::map<std::string, long>& values)
{
  output << '{';
  std::map<std::string, long>::const_iterator it = values.begin();
  for (; it != values.end(); ++it) { if (it != values.begin()) output << ','; output << '"' << JsonEscape(it->first) << "\":" << it->second; }
  output << '}';
}

// 用途：写出可空数值，避免用 0 冒充没有规范化结果。
void WriteOptionalNumber(std::ostream& output, bool present, double value)
{
  if (present) output << std::setprecision(15) << value; else output << "null";
}

// 用途：写出可空字符串；合法空字符串仍输出 ""，未取得字段才输出 null。
void WriteOptionalString(std::ostream& output, const OptionalNativeHoleString& value)
{
  if (value.has_value) output << '"' << JsonEscape(value.value) << '"';
  else output << "null";
}

// 用途：写出 Native Hole 的纯数据载荷，保持 number/boolean/array/null 的 JSON 类型。
void WriteNativeHole(std::ostream& output, const NativeHoleData& hole)
{
  output << "{\"semantic_kind\":\"" << JsonEscape(hole.semantic_kind)
         << "\",\"value_source\":\"" << JsonEscape(hole.value_source)
         << "\",\"interface_key\":\"" << JsonEscape(hole.interface_key)
         << "\",\"hole_type\":\"" << JsonEscape(hole.hole_type)
         << "\",\"hole_type_raw\":" << hole.hole_type_raw
         << ",\"diameter_mm\":" << std::setprecision(15) << hole.diameter_mm
         << ",\"origin_mm\":[" << hole.origin_mm[0] << ',' << hole.origin_mm[1]
         << ',' << hole.origin_mm[2] << "]"
         << ",\"direction\":[" << hole.direction[0] << ',' << hole.direction[1]
         << ',' << hole.direction[2] << "]"
         << ",\"bottom_limit\":{\"mode\":\"" << JsonEscape(hole.bottom_limit.mode)
         << "\",\"mode_raw\":" << hole.bottom_limit.mode_raw << ",\"depth_mm\":";
  WriteOptionalNumber(output, hole.bottom_limit.depth_mm.has_value,
                      hole.bottom_limit.depth_mm.value);
  output << ",\"depth_status\":\"" << JsonEscape(hole.bottom_limit.depth_mm.status)
         << "\"},\"head\":{\"kind\":\"" << JsonEscape(hole.head.kind)
         << "\",\"diameter_mm\":";
  WriteOptionalNumber(output, hole.head.diameter_mm.has_value, hole.head.diameter_mm.value);
  output << ",\"diameter_status\":\"" << JsonEscape(hole.head.diameter_mm.status)
         << "\",\"depth_mm\":";
  WriteOptionalNumber(output, hole.head.depth_mm.has_value, hole.head.depth_mm.value);
  output << ",\"depth_status\":\"" << JsonEscape(hole.head.depth_mm.status)
         << "\",\"angle_deg\":";
  WriteOptionalNumber(output, hole.head.angle_deg.has_value, hole.head.angle_deg.value);
  output << ",\"angle_status\":\"" << JsonEscape(hole.head.angle_deg.status)
         << "\"},\"thread\":{\"enabled\":" << (hole.thread.enabled ? "true" : "false")
         << ",\"mode_raw\":" << hole.thread.mode_raw << ",\"description\":";
  WriteOptionalString(output, hole.thread.description);
  output << ",\"description_status\":\"" << JsonEscape(hole.thread.description.status)
         << "\",\"diameter_mm\":";
  WriteOptionalNumber(output, hole.thread.diameter_mm.has_value, hole.thread.diameter_mm.value);
  output << ",\"diameter_status\":\"" << JsonEscape(hole.thread.diameter_mm.status)
         << "\",\"depth_mm\":";
  WriteOptionalNumber(output, hole.thread.depth_mm.has_value, hole.thread.depth_mm.value);
  output << ",\"depth_status\":\"" << JsonEscape(hole.thread.depth_mm.status)
         << "\",\"pitch_mm\":";
  WriteOptionalNumber(output, hole.thread.pitch_mm.has_value, hole.thread.pitch_mm.value);
  output << ",\"pitch_status\":\"" << JsonEscape(hole.thread.pitch_mm.status)
         << "\"},\"automation_alias\":";
  if (hole.has_automation_alias)
    output << '"' << JsonEscape(hole.automation_alias) << '"';
  else
    output << "null";
  output << ",\"automation_alias_status\":\"" << JsonEscape(hole.automation_alias_status)
         << "\",\"field_status\":";
  WriteStringMap(output, hole.field_status);
  output << '}';
}

// 用途：写出 Prism 终止边界，区分真实尺寸值、不适用和不可访问状态。
void WriteNativePrismLimit(std::ostream& output, const NativePrismLimitData& limit)
{
  output << "{\"mode\":\"" << JsonEscape(limit.mode)
         << "\",\"mode_raw\":" << limit.mode_raw
         << ",\"dimension_mm\":";
  WriteOptionalNumber(output, limit.dimension_mm.has_value, limit.dimension_mm.value);
  output << ",\"dimension_status\":\"" << JsonEscape(limit.dimension_mm.status)
         << "\",\"limiting_element_status\":\""
         << JsonEscape(limit.limiting_element_status) << "\"}";
}

// 用途：写出 Native Pad/Pocket 的 Prism 载荷，所有数值保持 JSON number/null 类型。
void WriteNativePrism(std::ostream& output, const NativePrismData& prism)
{
  output << "{\"semantic_kind\":\"" << JsonEscape(prism.semantic_kind)
         << "\",\"material_operation\":\"" << JsonEscape(prism.material_operation)
         << "\",\"value_source\":\"" << JsonEscape(prism.value_source)
         << "\",\"interface_key\":\"" << JsonEscape(prism.interface_key)
         << "\",\"direction_type\":\"" << JsonEscape(prism.direction_type)
         << "\",\"direction_type_raw\":" << prism.direction_type_raw
         << ",\"direction_orientation\":\"" << JsonEscape(prism.direction_orientation)
         << "\",\"direction_orientation_raw\":" << prism.direction_orientation_raw
         << ",\"direction\":[" << std::setprecision(15) << prism.direction[0] << ','
         << prism.direction[1] << ',' << prism.direction[2] << "]"
         << ",\"is_symmetric\":" << (prism.is_symmetric ? "true" : "false")
         << ",\"is_thin\":" << (prism.is_thin ? "true" : "false")
         << ",\"neutral_fiber\":" << (prism.neutral_fiber ? "true" : "false")
         << ",\"merge_end\":" << (prism.merge_end ? "true" : "false")
         << ",\"first_limit\":";
  WriteNativePrismLimit(output, prism.first_limit);
  output << ",\"second_limit\":";
  WriteNativePrismLimit(output, prism.second_limit);
  output << ",\"field_status\":";
  WriteStringMap(output, prism.field_status);
  output << '}';
}

void WriteNativeFeatureParameterField(std::ostream& output,
                                      const NativeFeatureParameterField& field)
{
  output << "{\"name\":\"" << JsonEscape(field.name)
         << "\",\"value_type\":\"" << JsonEscape(field.value_type)
         << "\",\"availability\":\"" << JsonEscape(field.availability)
         << "\",\"source_api\":\"" << JsonEscape(field.source_api)
         << "\",\"reason_code\":\"" << JsonEscape(field.reason_code)
         << "\",\"raw_value\":\"" << JsonEscape(field.raw_value)
         << "\",\"raw_unit\":\"" << JsonEscape(field.raw_unit)
         << "\",\"normalized_value\":";
  WriteOptionalNumber(output, field.has_numeric_value, field.numeric_value);
  output << ",\"normalized_unit\":\"" << JsonEscape(field.normalized_unit) << "\"}";
}

void WriteNativeFeatureReferenceField(std::ostream& output,
                                      const NativeFeatureReferenceField& field)
{
  output << "{\"name\":\"" << JsonEscape(field.name)
         << "\",\"availability\":\"" << JsonEscape(field.availability)
         << "\",\"source_api\":\"" << JsonEscape(field.source_api)
         << "\",\"reason_code\":\"" << JsonEscape(field.reason_code)
         << "\",\"count\":" << field.count
         << ",\"display_names\":";
  WriteStringArray(output, field.display_names);
  output << '}';
}

void WriteNativeFeatureParameters(std::ostream& output,
                                  const NativeFeatureParameterData& data)
{
  output << "{\"family\":\"" << JsonEscape(data.family)
         << "\",\"semantic_kind\":\"" << JsonEscape(data.semantic_kind)
         << "\",\"value_source\":\"" << JsonEscape(data.value_source)
         << "\",\"interface_key\":\"" << JsonEscape(data.interface_key)
         << "\",\"decode_status\":\"" << JsonEscape(data.decode_status)
         << "\",\"reason_code\":\"" << JsonEscape(data.reason_code)
         << "\",\"parameters\":{";
  std::vector<NativeFeatureParameterField>::const_iterator parameter =
    data.parameters.begin();
  for (; parameter != data.parameters.end(); ++parameter)
  {
    if (parameter != data.parameters.begin()) output << ',';
    output << '"' << JsonEscape(parameter->name) << "\":";
    WriteNativeFeatureParameterField(output, *parameter);
  }
  output << "},\"references\":{";
  std::vector<NativeFeatureReferenceField>::const_iterator reference =
    data.references.begin();
  for (; reference != data.references.end(); ++reference)
  {
    if (reference != data.references.begin()) output << ',';
    output << '"' << JsonEscape(reference->name) << "\":";
    WriteNativeFeatureReferenceField(output, *reference);
  }
  output << "},\"evidence\":";
  WriteStringMap(output, data.evidence);
  output << '}';
}

// 用途：写出 Feature 内嵌的 String 参数结果；该字段仍属于原始 CAA 对象。
void WriteParameterValue(std::ostream& output, const ParameterValueData& parameter)
{
  output << "{\"parameter_kind\":\"" << JsonEscape(parameter.parameter_kind)
         << "\",\"parameter_name\":\"" << JsonEscape(parameter.parameter_name)
         << "\",\"value_status\":\"" << JsonEscape(parameter.value_status)
         << "\",\"value_source\":\"" << JsonEscape(parameter.value_source)
         << "\",\"value_text\":\"" << JsonEscape(parameter.value_text)
         << "\",\"raw_display_text\":\"" << JsonEscape(parameter.raw_display_text)
         << "\",\"normalized_numeric_value\":";
  WriteOptionalNumber(output, parameter.has_normalized_numeric_value, parameter.normalized_numeric_value);
  output << ",\"normalized_unit\":\"" << JsonEscape(parameter.normalized_unit)
         << "\",\"normalization_status\":\"" << JsonEscape(parameter.normalization_status)
         << "\",\"is_read_only\":\"" << JsonEscape(parameter.is_read_only)
         << "\",\"is_hidden\":\"" << JsonEscape(parameter.is_hidden) << "\"}";
}

// 用途：按 cad_parse_mvp_v2 Schema 写一个 Feature，不包含任何原生指针或句柄。
void WriteFeature(std::ostream& output, const FeatureRecord& record)
{
  output << "{\"feature_id\":\"" << JsonEscape(record.feature_id)
         << "\",\"parent_id\":\"" << JsonEscape(record.parent_id)
         << "\",\"native_enumeration_index\":" << record.native_enumeration_index
         << ",\"container_enumeration_index\":" << record.container_enumeration_index
         << ",\"traversal_index\":" << record.traversal_index
         << ",\"native_type\":\"" << JsonEscape(record.fingerprint.native_type)
         << "\",\"startup_type\":\"" << JsonEscape(record.fingerprint.startup_type)
         << "\",\"super_types\":";
  WriteStringArray(output, record.fingerprint.super_types);
  output << ",\"display_name\":\"" << JsonEscape(record.fingerprint.display_name)
         << "\",\"internal_name\":\"" << JsonEscape(record.fingerprint.internal_name)
         << "\",\"container_kind\":\"" << JsonEscape(record.fingerprint.container_kind)
         << "\",\"tree_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"supported_interface_keys\":";
  WriteStringArray(output, record.fingerprint.supported_interface_keys);
  output << ",\"update_status\":\"" << JsonEscape(record.update_status)
         << "\",\"visibility\":\"" << JsonEscape(record.visibility)
         << "\",\"decoder_id\":\"" << JsonEscape(record.decoder_id)
         << "\",\"decoder_version\":\"" << JsonEscape(record.decoder_version)
         << "\",\"decode_level\":\"" << JsonEscape(record.decode_level)
         << "\",\"decode_status\":\"" << JsonEscape(record.decode_status)
         << "\",\"attributes\":";
  WriteStringMap(output, record.attributes);
  if (record.has_parameter) { output << ",\"parameter\":"; WriteParameterValue(output, record.parameter); }
  // 类型化载荷自行写出完整属性，中央 Writer 无需知道 Synthetic、Hole 或后续特征类型。
  if (record.GetTypedPayload())
  {
    output << ',';
    record.GetTypedPayload()->WriteJsonProperty(output);
  }
  output << ",\"diagnostic_ids\":"; WriteStringArray(output, record.diagnostic_ids); output << '}';
}

struct CapabilityCounts
{
  CapabilityCounts()
    : required_count(0), resolved_count(0), history_confirmed_count(0),
      authoritative_history_count(0),
      runtime_identity_count(0), candidate_count(0), ambiguous_count(0),
      unmatched_count(0), failed_count(0), evidence_count(0),
      required_instance_count(0), resolved_instance_count(0),
      unresolved_instance_count(0), duplicate_instance_path_count(0),
      invalid_transform_count(0),
      required_feature_count(0), recognized_feature_count(0),
      supported_feature_count(0), fully_decoded_feature_count(0),
      partially_decoded_feature_count(0), unsupported_feature_count(0),
      solid_count(0), shell_count(0), face_count(0), loop_count(0),
      coedge_count(0), edge_count(0), vertex_count(0),
      analytic_surface_count(0), nurbs_surface_count(0), unknown_surface_count(0),
      analytic_curve_count(0), nurbs_curve_count(0), unknown_curve_count(0),
      invalid_reference_count(0), orientation_error_count(0),
      geometry_decode_failure_count(0),
      required_renderable_face_count(0), mapped_face_count(0),
      unmapped_face_count(0), ambiguous_face_count(0),
      triangle_count(0), mapped_triangle_count(0),
      coverage_ratio(0.0), feature_coverage_ratio(0.0),
      mandatory_parameter_coverage_ratio(0.0),
      mesh_mapping_coverage_ratio(0.0),
      runtime_coverage_ratio(0.0), authoritative_coverage_ratio(0.0) {}
  long required_count;
  long resolved_count;
  long history_confirmed_count;
  long authoritative_history_count;
  long runtime_identity_count;
  long candidate_count;
  long ambiguous_count;
  long unmatched_count;
  long failed_count;
  long evidence_count;
  long required_instance_count;
  long resolved_instance_count;
  long unresolved_instance_count;
  long duplicate_instance_path_count;
  long invalid_transform_count;
  long required_feature_count;
  long recognized_feature_count;
  long supported_feature_count;
  long fully_decoded_feature_count;
  long partially_decoded_feature_count;
  long unsupported_feature_count;
  long solid_count;
  long shell_count;
  long face_count;
  long loop_count;
  long coedge_count;
  long edge_count;
  long vertex_count;
  long analytic_surface_count;
  long nurbs_surface_count;
  long unknown_surface_count;
  long analytic_curve_count;
  long nurbs_curve_count;
  long unknown_curve_count;
  long invalid_reference_count;
  long orientation_error_count;
  long geometry_decode_failure_count;
  long required_renderable_face_count;
  long mapped_face_count;
  long unmapped_face_count;
  long ambiguous_face_count;
  long triangle_count;
  long mapped_triangle_count;
  double coverage_ratio;
  double feature_coverage_ratio;
  double mandatory_parameter_coverage_ratio;
  double mesh_mapping_coverage_ratio;
  double runtime_coverage_ratio;
  double authoritative_coverage_ratio;
};

struct CapabilityEvaluation
{
  std::string name;
  std::string status;
  std::string reason_code;
  CapabilityCounts counts;
};

static double Ratio(long numerator, long denominator)
{
  if (denominator <= 0) return 0.0;
  double value = static_cast<double>(numerator) / static_cast<double>(denominator);
  if (value < 0.0) return 0.0;
  if (value > 1.0) return 1.0;
  return value;
}

static void FinishGenericCounts(CapabilityCounts& counts)
{
  counts.coverage_ratio = Ratio(counts.resolved_count, counts.required_count);
}

static void FinishFeatureTopologyCounts(CapabilityCounts& counts)
{
  counts.runtime_coverage_ratio = Ratio(counts.runtime_identity_count + counts.authoritative_history_count,
                                        counts.required_count);
  counts.authoritative_coverage_ratio = Ratio(counts.authoritative_history_count,
                                              counts.required_count);
  // For backward compatibility, coverage_ratio remains in the schema but now means authoritative coverage.
  counts.coverage_ratio = counts.authoritative_coverage_ratio;
}

static bool IsNativeDesignTypeRecognized(const FeatureRecord& feature)
{
  if (feature.GetTypedPayload()) return true;
  std::map<std::string, std::string>::const_iterator canonical =
    feature.attributes.find("canonical_native_type");
  return canonical != feature.attributes.end() && !canonical->second.empty();
}

static CapabilityEvaluation EvaluateNativeFeatureTypeExtraction(
  const std::vector<FeatureRecord>& features)
{
  CapabilityEvaluation item;
  item.name = "native_feature_type_extraction";
  item.status = "not_available";
  item.reason_code = "NO_NATIVE_FEATURE_CANDIDATES";
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature)
  {
    if (IsNativeDesignTypeRecognized(*feature))
    {
      ++item.counts.required_feature_count;
      ++item.counts.recognized_feature_count;
    }
  }
  item.counts.required_count = item.counts.required_feature_count;
  item.counts.evidence_count = item.counts.required_count;
  item.counts.resolved_count = item.counts.recognized_feature_count;
  item.counts.feature_coverage_ratio =
    Ratio(item.counts.recognized_feature_count, item.counts.required_feature_count);
  item.counts.coverage_ratio = item.counts.feature_coverage_ratio;
  if (item.counts.required_feature_count > 0 &&
      item.counts.recognized_feature_count == item.counts.required_feature_count)
  {
    item.status = "complete";
    item.reason_code = "ALL_ENUMERATED_FEATURE_TYPES_RESOLVED";
  }
  return item;
}

static CapabilityEvaluation EvaluateNativeFeatureParameterExtraction(
  const std::vector<FeatureRecord>& features)
{
  CapabilityEvaluation item;
  item.name = "native_feature_parameter_extraction";
  item.status = "not_available";
  item.reason_code = "NO_NATIVE_FEATURE_CANDIDATES";
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature)
  {
    const ITypedPayload* payload = feature->GetTypedPayload();
    const std::string payload_type = payload ? payload->GetPayloadTypeId() : "";
    const bool parameter_payload = payload_type == "native_hole" ||
      payload_type == "native_prism" || payload_type == "native_feature_parameters";
    if (parameter_payload)
    {
      ++item.counts.required_feature_count;
      ++item.counts.supported_feature_count;
      if (feature->decode_status == "success")
        ++item.counts.fully_decoded_feature_count;
      else
        ++item.counts.partially_decoded_feature_count;
    }
    else if (feature->decode_level == "type_only")
    {
      ++item.counts.required_feature_count;
      ++item.counts.recognized_feature_count;
      ++item.counts.unsupported_feature_count;
    }
  }
  item.counts.required_count = item.counts.required_feature_count;
  item.counts.evidence_count = item.counts.required_count;
  item.counts.recognized_feature_count +=
    item.counts.fully_decoded_feature_count + item.counts.partially_decoded_feature_count;
  item.counts.resolved_count = item.counts.fully_decoded_feature_count;
  item.counts.feature_coverage_ratio =
    Ratio(item.counts.recognized_feature_count, item.counts.required_feature_count);
  item.counts.mandatory_parameter_coverage_ratio =
    Ratio(item.counts.fully_decoded_feature_count, item.counts.required_feature_count);
  item.counts.coverage_ratio = item.counts.mandatory_parameter_coverage_ratio;
  if (item.counts.required_feature_count > 0 &&
      item.counts.fully_decoded_feature_count == item.counts.required_feature_count &&
      item.counts.unsupported_feature_count == 0 &&
      item.counts.partially_decoded_feature_count == 0 &&
      item.counts.failed_count == 0)
  {
    item.status = "complete";
    item.reason_code = "ALL_REQUIRED_FEATURE_PAYLOADS_DECODED";
  }
  else if (item.counts.required_feature_count > 0)
  {
    item.status = "partial";
    item.reason_code = "DEDICATED_DECODER_COVERAGE_INCOMPLETE";
  }
  return item;
}

static CapabilityEvaluation MakeNativeFeatureFamilyCapability(
  const char* family_name,
  long required,
  long decoded,
  long partial,
  long type_only)
{
  CapabilityEvaluation item;
  item.name = std::string("native_feature_parameter_extraction_by_family.") + family_name;
  item.status = required == 0 ? "not_applicable" : "partial";
  item.reason_code = required == 0 ? "FAMILY_NOT_PRESENT" : "FAMILY_PAYLOAD_INCOMPLETE";
  item.counts.required_feature_count = required;
  item.counts.required_count = required;
  item.counts.fully_decoded_feature_count = decoded;
  item.counts.partially_decoded_feature_count = partial;
  item.counts.recognized_feature_count = decoded + partial + type_only;
  item.counts.supported_feature_count = decoded + partial;
  item.counts.unsupported_feature_count =
    required > decoded + partial ? required - decoded - partial : 0;
  item.counts.resolved_count = decoded;
  item.counts.evidence_count = required;
  item.counts.feature_coverage_ratio = Ratio(item.counts.recognized_feature_count, required);
  item.counts.mandatory_parameter_coverage_ratio = Ratio(decoded, required);
  item.counts.coverage_ratio = item.counts.mandatory_parameter_coverage_ratio;
  if (required > 0 && decoded == required && partial == 0)
  {
    item.status = "complete";
    item.reason_code = "FAMILY_PAYLOAD_COMPLETE";
  }
  return item;
}

static void AddNativeFeatureFamilyCapabilities(const std::vector<FeatureRecord>& features,
                                               std::vector<CapabilityEvaluation>& items)
{
  std::map<std::string, long> required;
  std::map<std::string, long> decoded;
  std::map<std::string, long> partial;
  std::map<std::string, long> type_only;
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature)
  {
    std::string family;
    const ITypedPayload* payload = feature->GetTypedPayload();
    const std::string payload_type = payload ? payload->GetPayloadTypeId() : "";
    if (payload_type == "native_hole") family = "hole";
    else if (payload_type == "native_prism" &&
             feature->decoder_id == "NativePadDecoder") family = "pad";
    else if (payload_type == "native_prism" &&
             feature->decoder_id == "NativePocketDecoder") family = "pocket";
    else if (payload_type == "native_feature_parameters")
    {
      const NativeFeatureParameterPayload* native_payload =
        static_cast<const NativeFeatureParameterPayload*>(payload);
      family = native_payload->GetData().family;
    }
    else
    {
      std::map<std::string, std::string>::const_iterator canonical =
        feature->attributes.find("canonical_native_type");
      if (canonical != feature->attributes.end()) family = canonical->second;
    }
    if (family.empty()) continue;
    ++required[family];
    if ((payload_type == "native_hole" || payload_type == "native_prism" ||
         payload_type == "native_feature_parameters") && feature->decode_status == "success")
      ++decoded[family];
    else if (payload_type == "native_feature_parameters" && feature->decode_status == "partial")
      ++partial[family];
    else if (feature->decode_level == "type_only") ++type_only[family];
  }
  std::map<std::string, long>::const_iterator it = required.begin();
  for (; it != required.end(); ++it)
    items.push_back(MakeNativeFeatureFamilyCapability(it->first.c_str(), it->second,
                                                      decoded[it->first], partial[it->first],
                                                      type_only[it->first]));
}

static CapabilityEvaluation EvaluateFinalBrepTopology(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "final_brep_topology_extraction";
  item.status = context.topology_cells.empty() ? "not_available" : "partial";
  item.reason_code = context.topology_cells.empty() ? "NO_PUBLIC_TOPOLOGY_CELLS" :
    "LOOP_COEDGE_IR_INCOMPLETE";
  item.counts.solid_count = static_cast<long>(context.topology_bodies.size());
  item.counts.loop_count = static_cast<long>(context.topology_wires.size());
  item.counts.coedge_count = static_cast<long>(context.topology_coedges.size());
  item.counts.required_count = static_cast<long>(context.topology_cells.size());
  item.counts.evidence_count =
    item.counts.required_count + item.counts.loop_count + item.counts.coedge_count;
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  std::set<std::string> cell_ids;
  std::set<std::string> face_ids;
  std::set<std::string> edge_ids;
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension == 2) ++item.counts.face_count;
    else if (cell->dimension == 1) ++item.counts.edge_count;
    else if (cell->dimension == 0) ++item.counts.vertex_count;
    else if (cell->dimension == 3) ++item.counts.shell_count;
    if (cell->cell_id.empty()) ++item.counts.invalid_reference_count;
    else
    {
      cell_ids.insert(cell->cell_id);
      if (cell->dimension == 2) face_ids.insert(cell->cell_id);
      else if (cell->dimension == 1) edge_ids.insert(cell->cell_id);
    }
  }
  std::set<std::string> wire_ids;
  std::set<std::string> faces_with_wires;
  std::map<std::string, std::set<std::string> > wire_edges;
  std::vector<NativeTopologyWireRecord>::const_iterator wire = context.topology_wires.begin();
  for (; wire != context.topology_wires.end(); ++wire)
  {
    if (wire->wire_id.empty()) ++item.counts.invalid_reference_count;
    else wire_ids.insert(wire->wire_id);
    if (face_ids.find(wire->owning_face_id) == face_ids.end())
      ++item.counts.invalid_reference_count;
    else
      faces_with_wires.insert(wire->owning_face_id);
    if (wire->edge_cell_ids.empty()) ++item.counts.invalid_reference_count;
    if (wire->closed_status != "closed_by_edge_vertex_continuity")
      ++item.counts.orientation_error_count;
    std::vector<std::string>::const_iterator edge = wire->edge_cell_ids.begin();
    for (; edge != wire->edge_cell_ids.end(); ++edge)
    {
      if (edge_ids.find(*edge) == edge_ids.end()) ++item.counts.invalid_reference_count;
      else wire_edges[wire->wire_id].insert(*edge);
    }
  }
  std::set<std::string> coedge_ids;
  std::map<std::string, std::set<std::string> > coedges_by_wire;
  std::map<std::string, std::set<std::string> > face_edges;
  std::map<std::string, std::set<std::string> > edge_faces;
  std::vector<NativeTopologyCoedgeRecord>::const_iterator coedge =
    context.topology_coedges.begin();
  for (; coedge != context.topology_coedges.end(); ++coedge)
  {
    if (coedge->coedge_id.empty())
      ++item.counts.invalid_reference_count;
    else
      coedge_ids.insert(coedge->coedge_id);
    if (wire_ids.find(coedge->wire_id) == wire_ids.end() ||
        face_ids.find(coedge->owning_face_id) == face_ids.end() ||
        edge_ids.find(coedge->edge_cell_id) == edge_ids.end())
      ++item.counts.invalid_reference_count;
    else
    {
      coedges_by_wire[coedge->wire_id].insert(coedge->coedge_id);
      face_edges[coedge->owning_face_id].insert(coedge->edge_cell_id);
      edge_faces[coedge->edge_cell_id].insert(coedge->owning_face_id);
      std::map<std::string, std::set<std::string> >::const_iterator wire_edge =
        wire_edges.find(coedge->wire_id);
      if (wire_edge == wire_edges.end() ||
          wire_edge->second.find(coedge->edge_cell_id) == wire_edge->second.end())
        ++item.counts.invalid_reference_count;
    }
  }
  coedge = context.topology_coedges.begin();
  for (; coedge != context.topology_coedges.end(); ++coedge)
  {
    std::map<std::string, std::set<std::string> >::const_iterator group =
      coedges_by_wire.find(coedge->wire_id);
    const bool previous_ok = group != coedges_by_wire.end() &&
      group->second.find(coedge->previous_coedge_id) != group->second.end();
    const bool next_ok = group != coedges_by_wire.end() &&
      group->second.find(coedge->next_coedge_id) != group->second.end();
    if (!previous_ok || !next_ok) ++item.counts.orientation_error_count;
  }
  cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension == 2)
    {
      std::map<std::string, std::set<std::string> >::const_iterator face_edge =
        face_edges.find(cell->cell_id);
      if (face_edge != face_edges.end())
      {
        std::set<std::string>::const_iterator edge = face_edge->second.begin();
        for (; edge != face_edge->second.end(); ++edge)
        {
          if (std::find(cell->boundary_cell_ids.begin(),
                        cell->boundary_cell_ids.end(), *edge) ==
              cell->boundary_cell_ids.end())
            ++item.counts.invalid_reference_count;
        }
      }
      std::vector<std::string>::const_iterator adjacent = cell->adjacent_cell_ids.begin();
      for (; adjacent != cell->adjacent_cell_ids.end(); ++adjacent)
      {
        NativeTopologyCellRecord const* other_cell = 0;
        std::vector<NativeTopologyCellRecord>::const_iterator lookup = context.topology_cells.begin();
        for (; lookup != context.topology_cells.end(); ++lookup)
          if (lookup->cell_id == *adjacent) { other_cell = &(*lookup); break; }
        if (!other_cell || other_cell->dimension != 2 ||
            std::find(other_cell->adjacent_cell_ids.begin(),
                      other_cell->adjacent_cell_ids.end(), cell->cell_id) ==
              other_cell->adjacent_cell_ids.end())
          ++item.counts.invalid_reference_count;
      }
    }
    else if (cell->dimension == 1)
    {
      std::map<std::string, std::set<std::string> >::const_iterator edge_face =
        edge_faces.find(cell->cell_id);
      if (edge_face != edge_faces.end())
      {
        std::set<std::string>::const_iterator face = edge_face->second.begin();
        for (; face != edge_face->second.end(); ++face)
        {
          if (std::find(cell->adjacent_cell_ids.begin(),
                        cell->adjacent_cell_ids.end(), *face) ==
              cell->adjacent_cell_ids.end())
            ++item.counts.invalid_reference_count;
        }
      }
    }
  }
  item.counts.resolved_count =
    item.counts.invalid_reference_count == 0 && item.counts.orientation_error_count == 0 ?
    item.counts.required_count : 0;
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0 &&
      item.counts.invalid_reference_count == 0 &&
      item.counts.orientation_error_count == 0 &&
      item.counts.face_count == static_cast<long>(faces_with_wires.size()) &&
      item.counts.loop_count > 0 &&
      item.counts.coedge_count > 0)
  {
    item.status = "complete";
    item.reason_code = "TOPOLOGY_LOOP_COEDGE_GRAPH_COMPLETE";
  }
  return item;
}

static CapabilityEvaluation EvaluateFinalBrepGeometry(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "final_brep_geometry_extraction";
  item.status = context.topology_cells.empty() ? "not_available" : "partial";
  item.reason_code = context.topology_cells.empty() ? "NO_PUBLIC_TOPOLOGY_CELLS" :
    "EXACT_GEOMETRY_PARTIAL";
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension == 2)
    {
      ++item.counts.required_count;
      const bool has_required_domain = !cell->parameter_domain_json.empty();
      const bool has_material_side = !cell->material_side.empty() &&
        cell->material_side != "unknown";
      if (cell->geometry_status == "exact" && has_required_domain && has_material_side)
      {
        ++item.counts.resolved_count;
        if (cell->exact_geometry_type == "nurbs_surface") ++item.counts.nurbs_surface_count;
        else ++item.counts.analytic_surface_count;
      }
      else if (cell->exact_geometry_type == "nurbs_surface")
        ++item.counts.nurbs_surface_count;
      else
        ++item.counts.unknown_surface_count;
    }
    else if (cell->dimension == 1)
    {
      ++item.counts.required_count;
      const bool has_required_domain = !cell->parameter_domain_json.empty();
      if (cell->geometry_status == "exact" && has_required_domain)
      {
        ++item.counts.resolved_count;
        if (cell->exact_geometry_type == "nurbs_curve") ++item.counts.nurbs_curve_count;
        else ++item.counts.analytic_curve_count;
      }
      else if (cell->exact_geometry_type == "nurbs_curve")
        ++item.counts.nurbs_curve_count;
      else
        ++item.counts.unknown_curve_count;
    }
  }
  item.counts.evidence_count = item.counts.required_count;
  item.counts.geometry_decode_failure_count = item.counts.required_count > item.counts.resolved_count ?
    item.counts.required_count - item.counts.resolved_count : 0;
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0 &&
      item.counts.geometry_decode_failure_count == 0 &&
      item.counts.unknown_surface_count == 0 &&
      item.counts.unknown_curve_count == 0)
  {
    item.status = "complete";
    item.reason_code = "EXACT_SURFACE_CURVE_PARAMETERS_COMPLETE";
  }
  return item;
}

static CapabilityEvaluation EvaluateMeshBrepFaceMapping(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "mesh_brep_face_mapping";
  item.status = context.mesh_face_maps.empty() ? "not_available" : "partial";
  item.reason_code = context.mesh_face_maps.empty() ? "NO_MESH_FACE_MAP" :
    "MESH_FACE_MAPPING_PARTIAL";
  std::set<std::string> face_ids;
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
    if (cell->dimension == 2) face_ids.insert(cell->cell_id);
  item.counts.required_renderable_face_count = static_cast<long>(face_ids.size());
  item.counts.required_count = item.counts.required_renderable_face_count;
  item.counts.evidence_count = static_cast<long>(context.mesh_face_maps.size());
  std::vector<NativeMeshFaceMapRecord>::const_iterator map = context.mesh_face_maps.begin();
  for (; map != context.mesh_face_maps.end(); ++map)
  {
    item.counts.triangle_count += map->triangle_count;
    if (map->tessellation_status == "success" &&
        face_ids.find(map->face_cell_id) != face_ids.end())
    {
      ++item.counts.mapped_face_count;
      item.counts.mapped_triangle_count += map->triangle_count;
    }
    else
      ++item.counts.unmapped_face_count;
  }
  if (item.counts.required_renderable_face_count > item.counts.mapped_face_count)
    item.counts.unmapped_face_count +=
      item.counts.required_renderable_face_count - item.counts.mapped_face_count;
  item.counts.resolved_count = item.counts.mapped_face_count;
  item.counts.mesh_mapping_coverage_ratio =
    Ratio(item.counts.mapped_face_count, item.counts.required_renderable_face_count);
  item.counts.coverage_ratio = item.counts.mesh_mapping_coverage_ratio;
  if (item.counts.required_renderable_face_count > 0 &&
      item.counts.unmapped_face_count == 0 &&
      item.counts.ambiguous_face_count == 0 &&
      item.counts.triangle_count > 0 &&
      item.counts.triangle_count == static_cast<long>(context.mesh_triangles.size()) &&
      item.counts.mapped_triangle_count == item.counts.triangle_count)
  {
    item.status = "complete";
    item.reason_code = "TRIANGLE_PAYLOAD_FACE_MAPPING_COMPLETE";
  }
  return item;
}

static CapabilityEvaluation EvaluateMeshGeneration(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "mesh_generation";
  item.status = context.mesh_face_maps.empty() ? "not_available" : "partial";
  item.reason_code = context.mesh_face_maps.empty() ? "NO_TESSELLATION_OUTPUT" :
    "TRIANGLE_PAYLOAD_PARTIAL";
  item.counts.required_count = static_cast<long>(context.mesh_face_maps.size());
  item.counts.evidence_count = static_cast<long>(context.mesh_triangles.size());
  std::vector<NativeMeshFaceMapRecord>::const_iterator map = context.mesh_face_maps.begin();
  for (; map != context.mesh_face_maps.end(); ++map)
  {
    item.counts.triangle_count += map->triangle_count;
    if (map->tessellation_status == "success" && map->triangle_count > 0)
      ++item.counts.resolved_count;
  }
  item.counts.mapped_triangle_count = static_cast<long>(context.mesh_triangles.size());
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0 &&
      item.counts.resolved_count == item.counts.required_count &&
      item.counts.triangle_count > 0 &&
      item.counts.triangle_count == static_cast<long>(context.mesh_triangles.size()))
  {
    item.status = "complete";
    item.reason_code = "TRIANGLE_PAYLOAD_GENERATION_COMPLETE";
  }
  return item;
}

static CapabilityEvaluation MakeCapability(const char* name, const char* status,
                                           long required, long resolved, long evidence,
                                           const char* reason_code)
{
  CapabilityEvaluation item;
  item.name = name;
  item.status = status;
  item.reason_code = reason_code;
  item.counts.required_count = required;
  item.counts.resolved_count = resolved;
  item.counts.evidence_count = evidence;
  FinishGenericCounts(item.counts);
  return item;
}

static std::string DisplayTextForFeature(const FeatureRecord& feature)
{
  if (!feature.fingerprint.display_name.empty()) return feature.fingerprint.display_name;
  if (!feature.fingerprint.internal_name.empty()) return feature.fingerprint.internal_name;
  if (!feature.fingerprint.startup_type.empty()) return feature.fingerprint.startup_type;
  return feature.feature_id;
}

static bool FeatureHasTopology(const ParseContext& context, const std::string& feature_id)
{
  std::vector<NativeFeatureTopologyLinkRecord>::const_iterator link =
    context.native_feature_topology_links.begin();
  for (; link != context.native_feature_topology_links.end(); ++link)
    if (link->source_feature_id == feature_id) return true;
  std::vector<NativeTopologyBodyRecord>::const_iterator body = context.topology_bodies.begin();
  for (; body != context.topology_bodies.end(); ++body)
    if (body->source_feature_id == feature_id) return true;
  return false;
}

static void CollectFeatureSelection(const ParseContext& context, const std::string& feature_id,
                                    std::vector<std::string>& topology_ids,
                                    std::vector<std::string>& mesh_face_ids)
{
  std::set<std::string> topology_seen;
  std::set<std::string> mesh_seen;
  std::vector<NativeFeatureTopologyLinkRecord>::const_iterator link =
    context.native_feature_topology_links.begin();
  for (; link != context.native_feature_topology_links.end(); ++link)
  {
    if (link->source_feature_id != feature_id) continue;
    if (!link->final_cell_id.empty() && topology_seen.insert(link->final_cell_id).second)
      topology_ids.push_back(link->final_cell_id);
  }
  std::vector<NativeMeshFaceMapRecord>::const_iterator mesh = context.mesh_face_maps.begin();
  for (; mesh != context.mesh_face_maps.end(); ++mesh)
  {
    if (topology_seen.find(mesh->face_cell_id) != topology_seen.end() &&
        !mesh->mesh_map_id.empty() && mesh_seen.insert(mesh->mesh_map_id).second)
      mesh_face_ids.push_back(mesh->mesh_map_id);
  }
}

static void BuildNativeTreeNodes(const std::vector<FeatureRecord>& features,
                                 ParseContext& context)
{
  long traversal = 0;
  const bool is_product = !context.product_instances.empty();
  const std::string document_kind = context.metadata.document_kind.empty() ?
    (is_product ? "catproduct" : "catpart") : context.metadata.document_kind;

  if (is_product)
  {
    std::vector<NativeTreeNodeRecord> mounted_reference_nodes = context.native_tree_nodes;
    context.native_tree_nodes.clear();
    NativeTreeNodeRecord root;
    root.node_id = "product:root";
    root.display_text = context.metadata.input_file_name.empty() ? "CATProduct" :
      context.metadata.input_file_name;
    root.display_name = root.display_text;
    root.internal_name = root.display_text;
    root.startup_type = "CATProduct";
    root.document_kind = "catproduct";
    root.node_kind = "catproduct";
    root.tree_path = "/";
    root.traversal_index = traversal++;
    root.has_children = !context.product_instances.empty();
    root.properties_available = true;
    root.attributes["value_source"] = "CATIProduct";
    context.native_tree_nodes.push_back(root);

    std::vector<ProductInstanceRecord>::const_iterator instance =
      context.product_instances.begin();
    for (; instance != context.product_instances.end(); ++instance)
    {
      NativeTreeNodeRecord node;
      node.node_id = std::string("instance:") + instance->instance_id;
      node.parent_id = instance->parent_instance_id.empty() ? root.node_id :
        std::string("instance:") + instance->parent_instance_id;
      node.display_text = instance->instance_name.empty() ? instance->reference_id :
        instance->instance_name;
      node.display_name = node.display_text;
      node.internal_name = instance->instance_name;
      node.startup_type = "CATIProduct";
      node.document_kind = "catproduct";
      node.node_kind = instance->child_count > 0 ? "product_assembly" : "product_instance";
      node.source_index = instance->child_index;
      node.traversal_index = traversal++;
      node.tree_path = instance->tree_path;
      node.instance_id = instance->instance_id;
      node.parent_instance_id = instance->parent_instance_id;
      node.reference_id = instance->reference_id;
      node.source_node_id = instance->instance_id;
      node.has_children = instance->child_count > 0;
      node.properties_available = true;
      node.attributes["read_status"] = instance->read_status;
      node.attributes["transform_status"] = instance->transform_status;
      node.diagnostic_ids = instance->diagnostic_ids;
      context.native_tree_nodes.push_back(node);
    }
    if (mounted_reference_nodes.empty())
    {
      context.AddDiagnostic("info", "native_tree", "PRODUCT_REFERENCE_FEATURE_TREE_NOT_MOUNTED",
                            "Product BOM is emitted; reference CATPart feature mounting requires validated reference document traversal",
                            "product:root");
    }
    else
    {
      std::vector<NativeTreeNodeRecord>::iterator mounted = mounted_reference_nodes.begin();
      for (; mounted != mounted_reference_nodes.end(); ++mounted)
      {
        mounted->traversal_index = traversal++;
        context.native_tree_nodes.push_back(*mounted);
      }
      context.runtime_info["product_reference_feature_tree_status"] = "mounted";
    }
    return;
  }

  if (!context.native_tree_nodes.empty()) return;

  std::map<std::string, bool> has_children;
  std::vector<FeatureRecord>::const_iterator child = features.begin();
  for (; child != features.end(); ++child)
    if (!child->parent_id.empty()) has_children[child->parent_id] = true;

  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature)
  {
    NativeTreeNodeRecord node;
    node.node_id = std::string("feature:") + feature->feature_id;
    if (!feature->parent_id.empty())
      node.parent_id = std::string("feature:") + feature->parent_id;
    node.display_text = DisplayTextForFeature(*feature);
    node.display_name = feature->fingerprint.display_name;
    node.internal_name = feature->fingerprint.internal_name;
    node.startup_type = feature->fingerprint.startup_type.empty() ?
      feature->fingerprint.native_type : feature->fingerprint.startup_type;
    node.document_kind = document_kind.empty() ? "catpart" : document_kind;
    node.node_kind = feature->decode_level == "generic" ? "generic" :
      (feature->decode_level == "opaque" ? "opaque" : "native_feature");
    node.source_index = feature->container_enumeration_index ?
      feature->container_enumeration_index :
      (feature->native_enumeration_index ? feature->native_enumeration_index :
       feature->traversal_index);
    node.traversal_index = feature->traversal_index;
    node.tree_path = feature->tree_path;
    node.source_feature_id = feature->feature_id;
    node.source_node_id = feature->feature_id;
    node.has_children = has_children.find(feature->feature_id) != has_children.end();
    node.has_geometry = FeatureHasTopology(context, feature->feature_id);
    node.properties_available = true;
    node.attributes = feature->attributes;
    node.diagnostic_ids = feature->diagnostic_ids;
    CollectFeatureSelection(context, feature->feature_id, node.topology_ids,
                            node.mesh_face_ids);
    context.native_tree_nodes.push_back(node);
  }
}

static void AddNodeProperty(ParseContext& context, const std::string& node_id,
                            const char* tab_id, const char* tab_label,
                            const char* group_id, const char* group_label,
                            const char* field_key, const char* field_label,
                            const std::string& value, const char* unit,
                            const char* value_type, const char* source,
                            long display_order)
{
  if (value.empty()) return;
  std::vector<NodePropertyRecord>::const_iterator existing =
    context.node_properties.begin();
  for (; existing != context.node_properties.end(); ++existing)
  {
    if (existing->node_id == node_id &&
        existing->tab_id == (tab_id ? tab_id : "") &&
        existing->group_id == (group_id ? group_id : "") &&
        existing->field_key == (field_key ? field_key : ""))
      return;
  }
  NodePropertyRecord property;
  property.node_id = node_id;
  property.tab_id = tab_id;
  property.tab_label = tab_label;
  property.group_id = group_id;
  property.group_label = group_label;
  property.field_key = field_key;
  property.field_label = field_label;
  property.value = value;
  property.unit = unit ? unit : "";
  property.value_type = value_type;
  property.source = source;
  property.display_order = display_order;
  property.read_only = true;
  context.node_properties.push_back(property);
}

static const ProductReferenceRecord* FindProductReference(const ParseContext& context,
                                                          const std::string& reference_id)
{
  std::vector<ProductReferenceRecord>::const_iterator reference =
    context.product_references.begin();
  for (; reference != context.product_references.end(); ++reference)
    if (reference->reference_id == reference_id) return &*reference;
  return 0;
}

static void BuildNodeProperties(ParseContext& context)
{
  std::vector<NativeTreeNodeRecord>::const_iterator node = context.native_tree_nodes.begin();
  for (; node != context.native_tree_nodes.end(); ++node)
  {
    long order = 1;
    AddNodeProperty(context, node->node_id, "properties", "\xE5\xB1\x9E\xE6\x80\xA7",
                    "identity", "\xE5\xB8\xB8\xE8\xA7\x84", "display_name",
                    "\xE6\x98\xBE\xE7\xA4\xBA\xE5\x90\x8D\xE7\xA7\xB0",
                    node->display_text, "", "string", "native_tree", order++);
    AddNodeProperty(context, node->node_id, "properties", "\xE5\xB1\x9E\xE6\x80\xA7",
                    "identity", "\xE5\xB8\xB8\xE8\xA7\x84", "node_kind",
                    "\xE8\x8A\x82\xE7\x82\xB9\xE7\xB1\xBB\xE5\x9E\x8B",
                    node->node_kind, "", "string", "native_tree", order++);
    AddNodeProperty(context, node->node_id, "mechanical", "\xE6\x9C\xBA\xE6\xA2\xB0",
                    "catia_identity", "CATIA", "startup_type", "StartUp",
                    node->startup_type, "", "string", "native_tree", order++);
    AddNodeProperty(context, node->node_id, "mechanical", "\xE6\x9C\xBA\xE6\xA2\xB0",
                    "catia_identity", "CATIA", "internal_name",
                    "\xE5\x86\x85\xE9\x83\xA8\xE5\x90\x8D\xE7\xA7\xB0",
                    node->internal_name, "", "string", "native_tree", order++);
    AddNodeProperty(context, node->node_id, "mechanical", "\xE6\x9C\xBA\xE6\xA2\xB0",
                    "catia_identity", "CATIA", "tree_path",
                    "\xE6\xA0\x91\xE8\xB7\xAF\xE5\xBE\x84",
                    node->tree_path, "", "string", "native_tree", order++);
    if (!node->instance_id.empty())
    {
      AddNodeProperty(context, node->node_id, "product", "\xE4\xBA\xA7\xE5\x93\x81",
                      "product_identity", "\xE4\xBA\xA7\xE5\x93\x81", "instance_id",
                      "\xE5\xAE\x9E\xE4\xBE\x8B ID", node->instance_id, "", "string",
                      "CATIProduct", order++);
      AddNodeProperty(context, node->node_id, "product", "\xE4\xBA\xA7\xE5\x93\x81",
                      "product_identity", "\xE4\xBA\xA7\xE5\x93\x81", "reference_id",
                      "Reference ID", node->reference_id, "", "string", "CATIProduct", order++);
      const ProductReferenceRecord* reference = FindProductReference(context, node->reference_id);
      if (reference)
      {
        AddNodeProperty(context, node->node_id, "product", "\xE4\xBA\xA7\xE5\x93\x81",
                        "product_identity", "\xE4\xBA\xA7\xE5\x93\x81", "part_number",
                        "\xE9\x9B\xB6\xE4\xBB\xB6\xE7\xBC\x96\xE5\x8F\xB7",
                        reference->part_number, "", "string", "CATIProduct", order++);
        AddNodeProperty(context, node->node_id, "product", "\xE4\xBA\xA7\xE5\x93\x81",
                        "product_identity", "\xE4\xBA\xA7\xE5\x93\x81", "default_representation",
                        "\xE5\x8F\x82\xE8\x80\x83\xE6\x8F\x8F\xE8\xBF\xB0",
                        reference->default_representation, "", "string", "CATIProduct", order++);
      }
    }
    if (!node->source_feature_id.empty())
      AddNodeProperty(context, node->node_id, "properties", "\xE5\xB1\x9E\xE6\x80\xA7",
                      "identity", "\xE5\xB8\xB8\xE8\xA7\x84", "feature_id",
                      "Feature ID", node->source_feature_id, "", "string", "native_tree", order++);
    std::map<std::string, std::string>::const_iterator attr = node->attributes.begin();
    for (; attr != node->attributes.end(); ++attr)
      AddNodeProperty(context, node->node_id, "properties", "\xE5\xB1\x9E\xE6\x80\xA7",
                      "attributes", "\xE5\xB1\x9E\xE6\x80\xA7", attr->first.c_str(),
                      attr->first.c_str(), attr->second, "", "string", "native_tree", order++);
  }
}

static CapabilityEvaluation EvaluateAnalyticSurfaceParameters(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "analytic_surface_parameter_extraction";
  item.status = "not_available";
  item.reason_code = "NO_ANALYTIC_SURFACES";
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension != 2) continue;
    if (cell->exact_geometry_type == "nurbs_surface") continue;
    ++item.counts.required_count;
    if (cell->geometry_status == "exact")
    {
      ++item.counts.resolved_count;
      ++item.counts.analytic_surface_count;
    }
    else
    {
      ++item.counts.geometry_decode_failure_count;
      ++item.counts.unknown_surface_count;
    }
  }
  item.counts.evidence_count = item.counts.required_count;
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0)
  {
    item.status = item.counts.geometry_decode_failure_count == 0 ? "complete" : "partial";
    item.reason_code = item.status == "complete" ?
      "ANALYTIC_SURFACE_PARAMETERS_COMPLETE" : "ANALYTIC_SURFACE_PARAMETERS_PARTIAL";
  }
  return item;
}

static CapabilityEvaluation EvaluateNurbsSurfaceParameters(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "nurbs_surface_parameter_extraction";
  item.status = "not_available";
  item.reason_code = "NO_NURBS_SURFACES_IN_SCOPE";
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension == 2 && cell->exact_geometry_type == "nurbs_surface")
    {
      ++item.counts.required_count;
      ++item.counts.nurbs_surface_count;
      if (cell->geometry_status == "exact") ++item.counts.resolved_count;
      else ++item.counts.geometry_decode_failure_count;
    }
  }
  item.counts.evidence_count = item.counts.required_count;
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0)
  {
    item.status = item.counts.geometry_decode_failure_count == 0 ? "complete" : "partial";
    item.reason_code = item.status == "complete" ?
      "NURBS_SURFACE_PARAMETERS_COMPLETE" : "NURBS_SURFACE_PARAMETERS_PARTIAL";
  }
  return item;
}

static CapabilityEvaluation EvaluateCurveParameters(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "curve_parameter_extraction";
  item.status = "not_available";
  item.reason_code = "NO_CURVES";
  std::vector<NativeTopologyCellRecord>::const_iterator cell = context.topology_cells.begin();
  for (; cell != context.topology_cells.end(); ++cell)
  {
    if (cell->dimension != 1) continue;
    ++item.counts.required_count;
    if (cell->geometry_status == "exact")
    {
      ++item.counts.resolved_count;
      if (cell->exact_geometry_type == "nurbs_curve") ++item.counts.nurbs_curve_count;
      else ++item.counts.analytic_curve_count;
    }
    else
    {
      ++item.counts.geometry_decode_failure_count;
      if (cell->exact_geometry_type == "nurbs_curve") ++item.counts.nurbs_curve_count;
      else ++item.counts.unknown_curve_count;
    }
  }
  item.counts.evidence_count = item.counts.required_count;
  item.counts.coverage_ratio = Ratio(item.counts.resolved_count, item.counts.required_count);
  if (item.counts.required_count > 0)
  {
    item.status = item.counts.geometry_decode_failure_count == 0 ? "complete" : "partial";
    item.reason_code = item.status == "complete" ?
      "CURVE_PARAMETERS_COMPLETE" : "CURVE_PARAMETERS_PARTIAL";
  }
  return item;
}

static bool IsPersistentHistoryAuthority(const NativeFeatureTopologyLinkRecord& link)
{
  return link.authority == "catia_persistent_naming" ||
         link.authority == "catia_selection_reference" ||
         link.authority == "verified_r21_public_equivalent" ||
         (link.authority == "catia_history_result" && !link.persistent_reference.empty());
}

static CapabilityEvaluation EvaluateFeatureTopologyMapping(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "native_feature_topology_mapping";
  item.status = "not_available";
  item.reason_code = "NO_FEATURE_TOPOLOGY_RELATIONS";
  item.counts.required_count = static_cast<long>(context.native_feature_topology_links.size());
  item.counts.evidence_count = item.counts.required_count;

  bool has_forward = false;
  bool has_reverse = false;
  bool all_authoritative = item.counts.required_count > 0;
  std::vector<NativeFeatureTopologyLinkRecord>::const_iterator link =
    context.native_feature_topology_links.begin();
  for (; link != context.native_feature_topology_links.end(); ++link)
  {
    if (link->mapping_direction == "result_cell_to_final_face") has_forward = true;
    if (link->mapping_direction == "final_face_to_source_feature") has_reverse = true;

    if (link->mapping_status == "confirmed" && IsPersistentHistoryAuthority(*link))
    {
      ++item.counts.history_confirmed_count;
      ++item.counts.authoritative_history_count;
      ++item.counts.resolved_count;
    }
    else if (link->mapping_status == "runtime_matched" ||
             link->authority == "runtime_cell_identity")
    {
      ++item.counts.runtime_identity_count;
      all_authoritative = false;
    }
    else if (link->mapping_status == "candidate")
    {
      ++item.counts.candidate_count;
      all_authoritative = false;
    }
    else if (link->mapping_status == "ambiguous")
    {
      ++item.counts.ambiguous_count;
      all_authoritative = false;
    }
    else if (link->mapping_status == "unmatched" ||
             link->mapping_status == "insufficient_result_fingerprint")
    {
      ++item.counts.unmatched_count;
      all_authoritative = false;
    }
    else
    {
      ++item.counts.failed_count;
      all_authoritative = false;
    }
  }

  FinishFeatureTopologyCounts(item.counts);

  const bool complete = item.counts.required_count > 0 &&
    item.counts.authoritative_coverage_ratio >= 1.0 &&
    item.counts.candidate_count == 0 &&
    item.counts.ambiguous_count == 0 &&
    item.counts.unmatched_count == 0 &&
    item.counts.failed_count == 0 &&
    item.counts.runtime_identity_count == 0 &&
    has_forward && has_reverse && all_authoritative;
  if (complete)
  {
    item.status = "complete";
    item.reason_code = "AUTHORITATIVE_HISTORY_COMPLETE";
  }
  else if (item.counts.required_count > 0)
  {
    item.status = "partial";
    if (item.counts.authoritative_history_count == 0 && item.counts.runtime_identity_count > 0)
      item.reason_code = "RUNTIME_IDENTITY_ONLY";
    else if (!has_forward || !has_reverse)
      item.reason_code = "MISSING_FORWARD_OR_REVERSE_MAPPING";
    else if (item.counts.candidate_count || item.counts.ambiguous_count ||
             item.counts.unmatched_count || item.counts.failed_count)
      item.reason_code = "UNRESOLVED_FEATURE_TOPOLOGY_RELATIONS";
    else
      item.reason_code = "AUTHORITATIVE_HISTORY_INCOMPLETE";
  }
  return item;
}

static bool EndsWithNoCase(const std::string& value, const char* suffix)
{
  const std::string ending(suffix);
  if (value.size() < ending.size()) return false;
  const std::string tail = value.substr(value.size() - ending.size());
  for (std::string::size_type i = 0; i < tail.size(); ++i)
  {
    char a = tail[i];
    char b = ending[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static bool IsCatProductInput(const ParseContext& context)
{
  return EndsWithNoCase(context.metadata.input_file_name, ".CATProduct") ||
         EndsWithNoCase(context.metadata.input_source_path, ".CATProduct");
}

static bool FiniteDouble(double value)
{
  return _finite(value) != 0;
}

static double AbsDouble(double value)
{
  return value < 0.0 ? -value : value;
}

static bool ProductMatrixIsRigidAbsolute(const std::vector<double>& matrix)
{
  if (matrix.size() != 16) return false;
  for (std::vector<double>::const_iterator value = matrix.begin(); value != matrix.end(); ++value)
  {
    if (!FiniteDouble(*value)) return false;
  }
  if (AbsDouble(matrix[12]) > 1.0e-6 || AbsDouble(matrix[13]) > 1.0e-6 ||
      AbsDouble(matrix[14]) > 1.0e-6 || AbsDouble(matrix[15] - 1.0) > 1.0e-6)
    return false;

  const double r00 = matrix[0], r01 = matrix[1], r02 = matrix[2];
  const double r10 = matrix[4], r11 = matrix[5], r12 = matrix[6];
  const double r20 = matrix[8], r21 = matrix[9], r22 = matrix[10];
  const double c0 = r00 * r00 + r10 * r10 + r20 * r20;
  const double c1 = r01 * r01 + r11 * r11 + r21 * r21;
  const double c2 = r02 * r02 + r12 * r12 + r22 * r22;
  const double d01 = r00 * r01 + r10 * r11 + r20 * r21;
  const double d02 = r00 * r02 + r10 * r12 + r20 * r22;
  const double d12 = r01 * r02 + r11 * r12 + r21 * r22;
  const double det =
    r00 * (r11 * r22 - r12 * r21) -
    r01 * (r10 * r22 - r12 * r20) +
    r02 * (r10 * r21 - r11 * r20);
  return AbsDouble(c0 - 1.0) <= 1.0e-6 &&
         AbsDouble(c1 - 1.0) <= 1.0e-6 &&
         AbsDouble(c2 - 1.0) <= 1.0e-6 &&
         AbsDouble(d01) <= 1.0e-6 &&
         AbsDouble(d02) <= 1.0e-6 &&
         AbsDouble(d12) <= 1.0e-6 &&
         AbsDouble(det - 1.0) <= 1.0e-6;
}

static void CountProductStructure(const ParseContext& context, CapabilityCounts& counts)
{
  std::map<std::string, long> path_counts;
  std::map<std::string, bool> ids;
  std::vector<ProductInstanceRecord>::const_iterator it = context.product_instances.begin();
  for (; it != context.product_instances.end(); ++it)
  {
    ids[it->instance_id] = true;
    if (it->depth > 0)
    {
      ++counts.required_instance_count;
      ++path_counts[it->tree_path];
    }
  }

  it = context.product_instances.begin();
  for (; it != context.product_instances.end(); ++it)
  {
    if (it->depth <= 0) continue;
    const bool duplicate_path = path_counts[it->tree_path] > 1;
    const bool parent_resolved = !it->parent_instance_id.empty() &&
      ids.find(it->parent_instance_id) != ids.end();
    const bool valid = !it->tree_path.empty() && !duplicate_path &&
      parent_resolved && !it->reference_id.empty();
    if (valid) ++counts.resolved_instance_count;
  }

  std::map<std::string, long>::const_iterator path = path_counts.begin();
  for (; path != path_counts.end(); ++path)
  {
    if (path->second > 1) counts.duplicate_instance_path_count += path->second;
  }
  counts.required_count = counts.required_instance_count;
  counts.resolved_count = counts.resolved_instance_count;
  counts.unresolved_instance_count =
    counts.required_instance_count >= counts.resolved_instance_count ?
      counts.required_instance_count - counts.resolved_instance_count : 0;
  counts.failed_count = counts.unresolved_instance_count + counts.duplicate_instance_path_count;
  counts.evidence_count = static_cast<long>(context.product_instances.size());
  FinishGenericCounts(counts);
}

static CapabilityEvaluation EvaluateProductStructure(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "product_structure_extraction";
  item.status = "not_applicable";
  item.reason_code = "CATPART_NOT_APPLICABLE";
  if (!IsCatProductInput(context)) return item;

  CountProductStructure(context, item.counts);
  if (item.counts.required_instance_count == 0)
  {
    item.status = "complete";
    item.reason_code = "ROOT_ONLY_PRODUCT";
  }
  else if (item.counts.unresolved_instance_count == 0 &&
           item.counts.duplicate_instance_path_count == 0)
  {
    item.status = "complete";
    item.reason_code = "PRODUCT_STRUCTURE_COMPLETE";
  }
  else
  {
    item.status = "partial";
    item.reason_code = item.counts.duplicate_instance_path_count > 0 ?
      "DUPLICATE_INSTANCE_PATH" : "UNRESOLVED_PRODUCT_INSTANCE";
  }
  return item;
}

static CapabilityEvaluation EvaluateInstanceTransform(const ParseContext& context)
{
  CapabilityEvaluation item;
  item.name = "instance_transform_extraction";
  item.status = "not_applicable";
  item.reason_code = "CATPART_NOT_APPLICABLE";
  if (!IsCatProductInput(context)) return item;

  std::map<std::string, long> path_counts;
  std::vector<ProductInstanceRecord>::const_iterator it = context.product_instances.begin();
  for (; it != context.product_instances.end(); ++it)
  {
    if (it->depth > 0)
    {
      ++item.counts.required_instance_count;
      ++path_counts[it->tree_path];
    }
  }
  item.counts.required_count = item.counts.required_instance_count;
  item.counts.evidence_count = static_cast<long>(context.product_instances.size());
  if (item.counts.required_instance_count == 0)
  {
    item.status = "not_applicable";
    item.reason_code = "ROOT_ONLY_PRODUCT";
    FinishGenericCounts(item.counts);
    return item;
  }

  it = context.product_instances.begin();
  for (; it != context.product_instances.end(); ++it)
  {
    if (it->depth <= 0) continue;
    const bool unique_path = path_counts[it->tree_path] == 1;
    const bool absolute = it->transform_status == "resolved_absolute";
    const bool rigid = ProductMatrixIsRigidAbsolute(it->transform_4x4);
    if (unique_path && absolute && rigid) ++item.counts.resolved_instance_count;
    else ++item.counts.invalid_transform_count;
  }
  item.counts.resolved_count = item.counts.resolved_instance_count;
  item.counts.unresolved_instance_count =
    item.counts.required_instance_count >= item.counts.resolved_instance_count ?
      item.counts.required_instance_count - item.counts.resolved_instance_count : 0;
  item.counts.failed_count = item.counts.unresolved_instance_count;
  FinishGenericCounts(item.counts);
  if (item.counts.unresolved_instance_count == 0)
  {
    item.status = "complete";
    item.reason_code = "ABSOLUTE_TRANSFORMS_COMPLETE";
  }
  else
  {
    item.status = "partial";
    item.reason_code = "MISSING_OR_INVALID_ABSOLUTE_TRANSFORM";
  }
  return item;
}

static CapabilityEvaluation MakeProductCompatibilityCapability(const CapabilityEvaluation& structure,
                                                               const CapabilityEvaluation& transform)
{
  CapabilityEvaluation item;
  item.name = "catproduct_instance_extraction";
  item.status = "not_applicable";
  item.reason_code = structure.reason_code;
  item.counts = structure.counts;
  if (structure.status == "not_applicable")
    return item;
  if (structure.status == "complete" &&
      (transform.status == "complete" || transform.status == "not_applicable"))
  {
    item.status = "complete";
    item.reason_code = transform.status == "not_applicable" ?
      "PRODUCT_STRUCTURE_ONLY_ROOT" : "PRODUCT_STRUCTURE_AND_TRANSFORMS_COMPLETE";
    if (transform.counts.required_instance_count > 0) item.counts = transform.counts;
  }
  else
  {
    item.status = "partial";
    item.reason_code = "PRODUCT_STRUCTURE_OR_TRANSFORM_INCOMPLETE";
    if (transform.counts.required_instance_count > 0) item.counts = transform.counts;
  }
  return item;
}

static void BuildCapabilityEvaluations(const std::vector<FeatureRecord>& features,
                                       const ParseContext& context,
                                       std::vector<CapabilityEvaluation>& items,
                                       long& native_hole_decoded,
                                       long& native_prism_decoded,
                                       long& native_generic)
{
  native_hole_decoded = 0;
  native_prism_decoded = 0;
  native_generic = 0;
  long payload_complete = 0;
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature)
  {
    const ITypedPayload* payload = feature->GetTypedPayload();
    if (payload && std::string(payload->GetPayloadTypeId()) == "native_hole")
    { ++native_hole_decoded; ++payload_complete; }
    if (payload && std::string(payload->GetPayloadTypeId()) == "native_prism")
    { ++native_prism_decoded; ++payload_complete; }
    if (feature->decode_level == "generic") ++native_generic;
  }

  items.push_back(MakeCapability("spec_tree_extraction", "partial",
                                 static_cast<long>(features.size()),
                                 static_cast<long>(features.size()),
                                 static_cast<long>(features.size()),
                                 "SPEC_TREE_PUBLIC_ENUMERATION_PARTIAL"));
  items.push_back(EvaluateNativeFeatureTypeExtraction(features));
  items.push_back(EvaluateNativeFeatureParameterExtraction(features));
  AddNativeFeatureFamilyCapabilities(features, items);
  items.push_back(MakeCapability("native_feature_extraction",
                                 features.empty() ? "not_available" : "partial",
                                 static_cast<long>(features.size()), payload_complete,
                                 static_cast<long>(features.size()),
                                 features.empty() ? "NO_NATIVE_FEATURES" : "TYPE_AND_PARTIAL_PAYLOAD_EXTRACTION"));
  items.push_back(EvaluateFinalBrepTopology(context));
  items.push_back(EvaluateFinalBrepGeometry(context));
  items.push_back(EvaluateAnalyticSurfaceParameters(context));
  items.push_back(EvaluateNurbsSurfaceParameters(context));
  items.push_back(EvaluateCurveParameters(context));
  items.push_back(MakeCapability("topology_extraction",
                                 context.topology_bodies.empty() ? "not_available" : "partial",
                                 static_cast<long>(context.topology_bodies.size()),
                                 static_cast<long>(context.topology_bodies.size()),
                                 static_cast<long>(context.topology_cells.size()),
                                 context.topology_bodies.empty() ? "NO_PUBLIC_TOPOLOGY_BODY" : "REVISION_LOCAL_TOPOLOGY_ONLY"));
  items.push_back(EvaluateFeatureTopologyMapping(context));
  items.push_back(EvaluateFeatureTopologyMapping(context));
  items.back().name = "feature_final_topology_history";

  const std::map<std::string, std::string>::const_iterator fta_status_it =
    context.runtime_info.find("fta_extraction_status");
  std::string fta_status = fta_status_it == context.runtime_info.end() ?
    "not_available" : fta_status_it->second;
  if (fta_status == "complete" && context.fta_sets.empty())
    fta_status = "not_available";
  items.push_back(MakeCapability("fta_extraction", fta_status.c_str(),
                                 static_cast<long>(context.fta_sets.size()),
                                 static_cast<long>(context.fta_semantics.size()),
                                 static_cast<long>(context.fta_semantics.size()),
                                 context.fta_sets.empty() ? "NO_TPS_SETS" : "TPS_SET_LEVEL_EXTRACTION"));
  items.push_back(MakeCapability("fta_topology_mapping", "not_available",
                                 static_cast<long>(context.fta_topology_links.size()), 0,
                                 static_cast<long>(context.fta_topology_links.size()),
                                 "NOT_IMPLEMENTED"));
  CapabilityEvaluation mesh_brep = EvaluateMeshBrepFaceMapping(context);
  CapabilityEvaluation legacy_mesh = mesh_brep;
  legacy_mesh.name = "mesh_face_mapping";
  items.push_back(legacy_mesh);
  items.push_back(EvaluateMeshGeneration(context));
  items.push_back(mesh_brep);
  items.push_back(MakeCapability("manufacturing_feature_recognition", "not_performed", 0, 0, 0,
                                 "OUT_OF_SCOPE_PHASE1"));
  const CapabilityEvaluation product_structure = EvaluateProductStructure(context);
  const CapabilityEvaluation instance_transform = EvaluateInstanceTransform(context);
  items.push_back(product_structure);
  items.push_back(instance_transform);
  items.push_back(MakeProductCompatibilityCapability(product_structure, instance_transform));
  items.push_back(MakeCapability("native_tree_nodes_export",
                                 context.native_tree_nodes.empty() ? "not_available" : "partial",
                                 static_cast<long>(context.native_tree_nodes.size()),
                                 static_cast<long>(context.native_tree_nodes.size()),
                                 static_cast<long>(context.native_tree_nodes.size()),
                                 context.native_tree_nodes.empty() ? "NO_NATIVE_TREE_NODES" :
                                 "UNKNOWN_NODES_PRESERVED_WITH_PARTIAL_SEMANTICS"));
  items.push_back(MakeCapability("node_properties_export",
                                 context.node_properties.empty() ? "not_available" : "partial",
                                 static_cast<long>(context.native_tree_nodes.size()),
                                 static_cast<long>(context.node_properties.size()),
                                 static_cast<long>(context.node_properties.size()),
                                 context.node_properties.empty() ? "NO_NODE_PROPERTIES" :
                                 "BASIC_PROPERTIES_ONLY_ADVANCED_R21_INTERFACES_PENDING"));
  items.push_back(MakeCapability("catia_ui_virtual_nodes", "not_guaranteed", 0, 0, 0,
                                 "CATINavigateObject_NOT_VALIDATED_IN_CURRENT_PUBLIC_HEADERS"));
  items.push_back(MakeCapability("decoder_registry_export", "complete", 1, 1, 1,
                                 "REGISTRY_EXPORTED"));
}

static void WriteCapabilityCounts(std::ostream& output, const CapabilityCounts& counts)
{
  output << "\"required_count\":" << counts.required_count
         << ",\"resolved_count\":" << counts.resolved_count
         << ",\"history_confirmed_count\":" << counts.history_confirmed_count
         << ",\"authoritative_history_count\":" << counts.authoritative_history_count
         << ",\"runtime_identity_count\":" << counts.runtime_identity_count
         << ",\"candidate_count\":" << counts.candidate_count
         << ",\"ambiguous_count\":" << counts.ambiguous_count
         << ",\"unmatched_count\":" << counts.unmatched_count
         << ",\"failed_count\":" << counts.failed_count
         << ",\"required_instance_count\":" << counts.required_instance_count
         << ",\"resolved_instance_count\":" << counts.resolved_instance_count
         << ",\"unresolved_instance_count\":" << counts.unresolved_instance_count
         << ",\"duplicate_instance_path_count\":" << counts.duplicate_instance_path_count
         << ",\"invalid_transform_count\":" << counts.invalid_transform_count
         << ",\"required_feature_count\":" << counts.required_feature_count
         << ",\"recognized_feature_count\":" << counts.recognized_feature_count
         << ",\"supported_feature_count\":" << counts.supported_feature_count
         << ",\"fully_decoded_feature_count\":" << counts.fully_decoded_feature_count
         << ",\"partially_decoded_feature_count\":" << counts.partially_decoded_feature_count
         << ",\"unsupported_feature_count\":" << counts.unsupported_feature_count
         << ",\"solid_count\":" << counts.solid_count
         << ",\"shell_count\":" << counts.shell_count
         << ",\"face_count\":" << counts.face_count
         << ",\"loop_count\":" << counts.loop_count
         << ",\"coedge_count\":" << counts.coedge_count
         << ",\"edge_count\":" << counts.edge_count
         << ",\"vertex_count\":" << counts.vertex_count
         << ",\"analytic_surface_count\":" << counts.analytic_surface_count
         << ",\"nurbs_surface_count\":" << counts.nurbs_surface_count
         << ",\"unknown_surface_count\":" << counts.unknown_surface_count
         << ",\"analytic_curve_count\":" << counts.analytic_curve_count
         << ",\"nurbs_curve_count\":" << counts.nurbs_curve_count
         << ",\"unknown_curve_count\":" << counts.unknown_curve_count
         << ",\"invalid_reference_count\":" << counts.invalid_reference_count
         << ",\"orientation_error_count\":" << counts.orientation_error_count
         << ",\"geometry_decode_failure_count\":" << counts.geometry_decode_failure_count
         << ",\"required_renderable_face_count\":" << counts.required_renderable_face_count
         << ",\"mapped_face_count\":" << counts.mapped_face_count
         << ",\"unmapped_face_count\":" << counts.unmapped_face_count
         << ",\"ambiguous_face_count\":" << counts.ambiguous_face_count
         << ",\"triangle_count\":" << counts.triangle_count
         << ",\"mapped_triangle_count\":" << counts.mapped_triangle_count
         << ",\"feature_coverage_ratio\":" << std::setprecision(15) << counts.feature_coverage_ratio
         << ",\"mandatory_parameter_coverage_ratio\":" << std::setprecision(15) << counts.mandatory_parameter_coverage_ratio
         << ",\"mesh_mapping_coverage_ratio\":" << std::setprecision(15) << counts.mesh_mapping_coverage_ratio
         << ",\"runtime_coverage_ratio\":" << std::setprecision(15) << counts.runtime_coverage_ratio
         << ",\"authoritative_coverage_ratio\":" << std::setprecision(15) << counts.authoritative_coverage_ratio
         << ",\"coverage_ratio\":" << std::setprecision(15) << counts.coverage_ratio;
}

// 用途：把每个实际遍历到的 CAA 规格对象投影为原生特征出口记录；未有专用 Decoder 的对象如实标记 generic，绝不伪造拓扑结果。
void WriteNativeFeature(std::ostream& output, const FeatureRecord& record)
{
  const ITypedPayload* payload = record.GetTypedPayload();
  const bool is_native_hole = payload && std::string(payload->GetPayloadTypeId()) == "native_hole";
  const bool is_native_prism = payload && std::string(payload->GetPayloadTypeId()) == "native_prism";
  const bool is_native_parameters = payload &&
    std::string(payload->GetPayloadTypeId()) == "native_feature_parameters";
  std::string canonical_native_type;
  if (is_native_hole) canonical_native_type = "hole";
  else if (is_native_prism && record.decoder_id == "NativePadDecoder") canonical_native_type = "pad";
  else if (is_native_prism && record.decoder_id == "NativePocketDecoder") canonical_native_type = "pocket";
  else if (is_native_parameters)
    canonical_native_type =
      static_cast<const NativeFeatureParameterPayload*>(payload)->GetData().family;
  else
  {
    std::map<std::string, std::string>::const_iterator canonical =
      record.attributes.find("canonical_native_type");
    if (canonical != record.attributes.end()) canonical_native_type = canonical->second;
  }
  const bool payload_complete = is_native_hole || is_native_prism ||
    (is_native_parameters && record.decode_status == "success");
  const bool payload_partial = is_native_parameters && record.decode_status == "partial";
  const bool type_only = record.decode_level == "type_only";
  const char* decoder_status = payload_complete ? "decoded" :
    (payload_partial ? "partial" :
    (type_only ? "type_only" :
    (record.decode_level == "generic" ? "generic" :
     (record.decode_level == "opaque" ? "unsupported" : "failed"))));
  const char* type_resolution_status = (payload_complete || payload_partial || type_only) ? "resolved" :
    (canonical_native_type.empty() ? "unresolved" : "resolved");
  const char* payload_extraction_status = payload_complete ? "complete" :
    (payload_partial ? "partial" :
    (type_only ? "not_implemented" : "not_available"));
  output << "{\"native_feature_id\":\"" << JsonEscape(record.feature_id)
         << "\",\"source_object_id\":\"" << JsonEscape(record.feature_id)
         << "\",\"part_id\":\"\",\"instance_id\":null,\"body_id\":\"\""
         << ",\"parent_feature_id\":";
  if (record.parent_id.empty()) output << "null";
  else output << '"' << JsonEscape(record.parent_id) << '"';
  output << ",\"name\":\"" << JsonEscape(record.fingerprint.display_name)
         << "\",\"startup_type\":\"" << JsonEscape(record.fingerprint.startup_type)
         << "\",\"canonical_native_type\":\"" << JsonEscape(canonical_native_type)
         << "\",\"decoder\":\"" << JsonEscape(record.decoder_id)
         << "\",\"decoder_version\":\"" << JsonEscape(record.decoder_version)
         << "\",\"payload_type\":\"" << JsonEscape(payload ? payload->GetPayloadTypeId() : "")
         << "\",\"payload_schema_version\":\"" << CAD_PARSE_SCHEMA_VERSION
         << "\",\"decoder_status\":\"" << decoder_status
         << "\",\"type_resolution_status\":\"" << type_resolution_status
         << "\",\"payload_extraction_status\":\"" << payload_extraction_status
         << "\",\"suppressed\":false,\"active\":true,\"parameters\":{}"
         << ",\"references\":[],\"result_topology_refs\":[]"
         << ",\"update_status\":\"" << JsonEscape(record.update_status)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  if (is_native_hole || is_native_prism || is_native_parameters)
  {
    output << ',';
    payload->WriteJsonProperty(output);
  }
  output << '}';
}

// 用途：写出一个 CAA 原生结果体摘要；它不是规格树 Feature，不参与对象守恒计数。
void WriteNativeTopologyBody(std::ostream& output, const NativeTopologyBodyRecord& record)
{
  output << "{\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"source_kind\":\"" << JsonEscape(record.source_kind)
         << "\",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"vertex_count\":" << record.vertex_count
         << ",\"edge_count\":" << record.edge_count
         << ",\"face_count\":" << record.face_count
         << ",\"volume_count\":" << record.volume_count
         << ",\"stability_scope\":\"" << JsonEscape(record.stability_scope)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出一个 CAA 原生拓扑单元摘要；cell_id 是本次解析内稳定编号，不是 CATIA 指针。
void WriteNativeTopologyCell(std::ostream& output, const NativeTopologyCellRecord& record)
{
  output << "{\"cell_id\":\"" << JsonEscape(record.cell_id)
         << "\",\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"cell_kind\":\"" << JsonEscape(record.cell_kind)
         << "\",\"topology_index\":" << record.topology_index
         << ",\"dimension\":" << record.dimension
         << ",\"domain_count\":" << record.domain_count
         << ",\"internal_domain_count\":" << record.internal_domain_count
         << ",\"center_mm\":";
  if (record.has_center)
    output << '[' << std::setprecision(15) << record.center_mm[0] << ','
           << record.center_mm[1] << ',' << record.center_mm[2] << ']';
  else
    output << "null";
  output << ",\"area_mm2\":";
  WriteOptionalNumber(output, record.area_mm2_available, record.area_mm2);
  output << ",\"length_mm\":";
  WriteOptionalNumber(output, record.length_mm_available, record.length_mm);
  output << ",\"orientation\":\"" << JsonEscape(record.geometry_orientation.empty() ? "unknown" : record.geometry_orientation) << "\""
         << ",\"material_side\":\"" << JsonEscape(record.material_side.empty() ? "unknown" : record.material_side) << "\""
         << ",\"geometry_type\":\"" << JsonEscape(record.exact_geometry_type.empty() ? record.geometry_status : record.exact_geometry_type)
         << "\",\"geometry_parameters\":";
  if (record.geometry_parameters_json.empty()) output << "{}";
  else output << record.geometry_parameters_json;
  output << ",\"parameter_domain\":";
  if (record.parameter_domain_json.empty()) output << "null";
  else output << record.parameter_domain_json;
  output
         << ",\"periodic\":\"unknown\""
         << ",\"closed\":\"unknown\""
         << ",\"centroid_mm\":";
  if (record.has_center)
    output << '[' << std::setprecision(15) << record.center_mm[0] << ','
           << record.center_mm[1] << ',' << record.center_mm[2] << ']';
  else
    output << "null";
  output << ",\"bounding_box_mm\":";
  if (record.bounding_box_json.empty()) output << "null";
  else output << record.bounding_box_json;
  output
         << ",\"outer_wire_id\":";
  if (record.outer_wire_id.empty()) output << "null";
  else output << '"' << JsonEscape(record.outer_wire_id) << '"';
  output << ",\"inner_wire_ids\":";
  WriteStringArray(output, record.inner_wire_ids);
  output
         << ",\"adjacencies\":[]"
         << ",\"normal_samples\":[]"
         << ",\"curvature_samples\":[]"
         << ",\"persistent_name\":\"\""
         << ",\"geometry_status\":\"" << JsonEscape(record.geometry_status)
         << "\",\"measure_status\":\"" << JsonEscape(record.measure_status)
         << "\",\"boundary_cell_ids\":";
  WriteStringArray(output, record.boundary_cell_ids);
  output << ",\"adjacent_cell_ids\":";
  WriteStringArray(output, record.adjacent_cell_ids);
  output
         << ",\"stable_id_method\":\"" << JsonEscape(record.stable_id_method)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出 Face 边界 Loop/Wire 摘要，保留其所属 Face 和边集合。
void WriteNativeTopologyWire(std::ostream& output, const NativeTopologyWireRecord& record)
{
  output << "{\"wire_id\":\"" << JsonEscape(record.wire_id)
         << "\",\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"wire_index\":" << record.wire_index
         << ",\"wire_kind\":\"" << JsonEscape(record.wire_kind)
         << "\",\"owning_face_id\":\"" << JsonEscape(record.owning_face_id)
         << "\",\"owning_face_topology_index\":" << record.owning_face_topology_index
         << ",\"edge_count\":" << record.edge_count
         << ",\"closed_status\":\"" << JsonEscape(record.closed_status)
         << "\",\"edge_cell_ids\":";
  WriteStringArray(output, record.edge_cell_ids);
  output << ",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteNativeTopologyCoedge(std::ostream& output, const NativeTopologyCoedgeRecord& record)
{
  output << "{\"coedge_id\":\"" << JsonEscape(record.coedge_id)
         << "\",\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"wire_id\":\"" << JsonEscape(record.wire_id)
         << "\",\"owning_face_id\":\"" << JsonEscape(record.owning_face_id)
         << "\",\"edge_cell_id\":\"" << JsonEscape(record.edge_cell_id)
         << "\",\"previous_coedge_id\":\"" << JsonEscape(record.previous_coedge_id)
         << "\",\"next_coedge_id\":\"" << JsonEscape(record.next_coedge_id)
         << "\",\"coedge_index\":" << record.coedge_index
         << ",\"coedge_index_in_wire\":" << record.coedge_index_in_wire
         << ",\"edge_orientation_side\":" << record.edge_orientation_side
         << ",\"orientation_status\":\"" << JsonEscape(record.orientation_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出 CAA Face 到轻量化三角范围的映射摘要，供后续 GLB Writer 或 Sidecar 对齐使用。
void WriteNativeMeshFaceMap(std::ostream& output, const NativeMeshFaceMapRecord& record)
{
  output << "{\"mesh_map_id\":\"" << JsonEscape(record.mesh_map_id)
         << "\",\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"face_cell_id\":\"" << JsonEscape(record.face_cell_id)
         << "\",\"primitive_index\":" << record.primitive_index
         << ",\"triangle_start\":" << record.triangle_start
         << ",\"triangle_count\":" << record.triangle_count
         << ",\"point_count\":" << record.point_count
         << ",\"isolated_triangle_count\":" << record.isolated_triangle_count
         << ",\"strip_count\":" << record.strip_count
         << ",\"fan_count\":" << record.fan_count
         << ",\"polygon_count\":" << record.polygon_count
         << ",\"estimated_triangle_count\":" << record.estimated_triangle_count
         << ",\"face_orientation_side\":" << record.face_orientation_side
         << ",\"planar\":" << (record.planar ? "true" : "false")
         << ",\"tessellation_status\":\"" << JsonEscape(record.tessellation_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteNativeMeshTriangle(std::ostream& output, const NativeMeshTriangleRecord& record)
{
  output << "{\"triangle_id\":\"" << JsonEscape(record.triangle_id)
         << "\",\"mesh_map_id\":\"" << JsonEscape(record.mesh_map_id)
         << "\",\"body_id\":\"" << JsonEscape(record.body_id)
         << "\",\"face_cell_id\":\"" << JsonEscape(record.face_cell_id)
         << "\",\"triangle_index\":" << record.triangle_index
         << ",\"triangle_index_in_face\":" << record.triangle_index_in_face
         << ",\"vertex_ranks\":[" << record.vertex_ranks[0] << ','
         << record.vertex_ranks[1] << ',' << record.vertex_ranks[2]
         << "],\"vertices_mm\":[";
  int i = 0;
  for (; i < 9; ++i)
  {
    if (i != 0) output << ',';
    output << std::setprecision(15) << record.vertices_mm[i];
  }
  output << "],\"normal\":";
  if (record.normal_available)
    output << '[' << std::setprecision(15) << record.normal[0] << ','
           << record.normal[1] << ',' << record.normal[2] << ']';
  else
    output << "null";
  output << ",\"source_primitive\":\"" << JsonEscape(record.source_primitive)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出一个 FTA/TPS Set 摘要；它只表示原生标注集合事实，不表示已完成语义解析或拓扑映射。
void WriteFtaSet(std::ostream& output, const FtaSetRecord& record)
{
  output << "{\"fta_set_id\":\"" << JsonEscape(record.fta_set_id)
         << "\",\"set_index\":" << record.set_index
         << ",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"tps_count\":" << record.tps_count
         << ",\"geometry_count\":" << record.geometry_count
         << ",\"semantic_detail_status\":\"" << JsonEscape(record.semantic_detail_status)
         << "\",\"topology_mapping_status\":\"" << JsonEscape(record.topology_mapping_status)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出一个 TPS 组件级语义观测；该记录不包含未验证的逐类 GD&T 参数。
void WriteFtaSemantic(std::ostream& output, const FtaSemanticRecord& record)
{
  output << "{\"fta_semantic_id\":\"" << JsonEscape(record.fta_semantic_id)
         << "\",\"fta_set_id\":\"" << JsonEscape(record.fta_set_id)
         << "\",\"component_index\":" << record.component_index
         << ",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"component_kind\":\"" << JsonEscape(record.component_kind)
         << "\",\"semantic_type\":\"" << JsonEscape(record.component_kind)
         << "\",\"semantic_payload\":{\"supported_interface_keys\":";
  WriteStringArray(output, record.supported_interface_keys);
  output << ",\"validation_text\":\"" << JsonEscape(record.validation_text)
         << "\",\"validation_text_status\":\"" << JsonEscape(record.validation_text_status)
         << "\",\"semantic_check_status_raw\":" << record.semantic_check_status_raw
         << ",\"semantic_check_diagnostic\":\"" << JsonEscape(record.semantic_check_diagnostic)
         << "\"},\"ttrs_id\":null,\"annotation_view_id\":null,\"capture_ids\":[]"
         << ",\"semantic_validity\":\"" << JsonEscape(record.semantic_check_diagnostic)
         << "\",\"supported_interface_keys\":";
  WriteStringArray(output, record.supported_interface_keys);
  output << ",\"semantic_interface_count\":" << record.semantic_interface_count
         << ",\"all_semantic_interface_count\":" << record.all_semantic_interface_count
         << ",\"validation_text\":\"" << JsonEscape(record.validation_text)
         << "\",\"validation_text_status\":\"" << JsonEscape(record.validation_text_status)
         << "\",\"semantic_check_status_raw\":" << record.semantic_check_status_raw
         << ",\"semantic_check_diagnostic\":\"" << JsonEscape(record.semantic_check_diagnostic)
         << "\",\"topology_mapping_status\":\"" << JsonEscape(record.topology_mapping_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteFtaTopologyLink(std::ostream& output, const FtaTopologyLinkRecord& record)
{
  output << "{\"fta_link_id\":\"" << JsonEscape(record.fta_link_id)
         << "\",\"fta_semantic_id\":\"" << JsonEscape(record.fta_semantic_id)
         << "\",\"referenced_geometry_kind\":\"" << JsonEscape(record.geometry_reference_kind)
         << "\",\"referenced_geometry_id\":\"\""
         << "\",\"final_cell_id\":\"" << JsonEscape(record.final_cell_id)
         << "\",\"final_body_id\":\"" << JsonEscape(record.final_body_id)
         << "\",\"geometry_reference_kind\":\"" << JsonEscape(record.geometry_reference_kind)
         << "\",\"reference_role\":\"target\""
         << "\",\"mapping_status\":\"" << JsonEscape(record.mapping_status)
         << "\",\"mapping_method\":\"" << JsonEscape(record.mapping_method)
         << "\",\"authority\":\"" << JsonEscape(record.authority)
         << "\",\"persistent_reference\":\"\""
         << "\",\"confidence\":" << std::setprecision(15) << record.confidence
         << ",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteProductReference(std::ostream& output, const ProductReferenceRecord& record)
{
  output << "{\"reference_id\":\"" << JsonEscape(record.reference_id)
         << "\",\"document_name\":\"" << JsonEscape(record.source_document)
         << "\",\"part_number\":\"" << JsonEscape(record.part_number)
         << "\",\"revision\":\"\""
         << ",\"definition_source\":\"" << JsonEscape(record.value_source)
         << "\",\"load_status\":\"" << JsonEscape(record.read_status)
         << "\",\"display_name\":\"" << JsonEscape(record.display_name)
         << "\",\"source_document\":\"" << JsonEscape(record.source_document)
         << "\",\"default_representation\":\"" << JsonEscape(record.default_representation)
         << "\",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"child_count\":" << record.child_count
         << ",\"representation_count\":" << record.representation_count
         << ",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteProductInstance(std::ostream& output, const ProductInstanceRecord& record)
{
  output << "{\"instance_id\":\"" << JsonEscape(record.instance_id)
         << "\",\"parent_instance_id\":\"" << JsonEscape(record.parent_instance_id)
         << "\",\"reference_id\":\"" << JsonEscape(record.reference_id)
         << "\",\"instance_name\":\"" << JsonEscape(record.instance_name)
         << "\",\"instance_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"suppressed\":false"
         << ",\"load_status\":\"" << JsonEscape(record.read_status)
         << "\",\"tree_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"depth\":" << record.depth
         << ",\"child_index\":" << record.child_index
         << ",\"child_count\":" << record.child_count
         << ",\"transform_4x4\":[";
  std::vector<double>::const_iterator value = record.transform_4x4.begin();
  for (; value != record.transform_4x4.end(); ++value)
  {
    if (value != record.transform_4x4.begin()) output << ',';
    output << std::setprecision(15) << *value;
  }
  output << "],\"transform_status\":\"" << JsonEscape(record.transform_status)
         << "\",\"transform_value_source\":\"" << JsonEscape(record.transform_value_source)
         << "\",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteNativeTreeNode(std::ostream& output, const NativeTreeNodeRecord& record)
{
  output << "{\"node_id\":\"" << JsonEscape(record.node_id)
         << "\",\"id\":\"" << JsonEscape(record.node_id)
         << "\",\"parent_id\":\"" << JsonEscape(record.parent_id)
         << "\",\"display_text\":\"" << JsonEscape(record.display_text)
         << "\",\"display_name\":\"" << JsonEscape(record.display_name)
         << "\",\"label\":\"" << JsonEscape(record.display_text)
         << "\",\"internal_name\":\"" << JsonEscape(record.internal_name)
         << "\",\"startup_type\":\"" << JsonEscape(record.startup_type)
         << "\",\"document_kind\":\"" << JsonEscape(record.document_kind)
         << "\",\"node_kind\":\"" << JsonEscape(record.node_kind)
         << "\",\"source_index\":" << record.source_index
         << ",\"traversal_index\":" << record.traversal_index
         << ",\"tree_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"instance_id\":\"" << JsonEscape(record.instance_id)
         << "\",\"parent_instance_id\":\"" << JsonEscape(record.parent_instance_id)
         << "\",\"reference_id\":\"" << JsonEscape(record.reference_id)
         << "\",\"feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"topology_id\":\"" << JsonEscape(record.topology_id)
         << "\",\"source_node_id\":\"" << JsonEscape(record.source_node_id)
         << "\",\"has_children\":" << (record.has_children ? "true" : "false")
         << ",\"has_geometry\":" << (record.has_geometry ? "true" : "false")
         << ",\"has_properties\":" << (record.properties_available ? "true" : "false")
         << ",\"properties_available\":" << (record.properties_available ? "true" : "false")
         << ",\"selection\":{\"mesh_face_ids\":";
  WriteStringArray(output, record.mesh_face_ids);
  output << ",\"topology_ids\":";
  WriteStringArray(output, record.topology_ids);
  output << "},\"attributes\":";
  WriteStringMap(output, record.attributes);
  output << ",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

void WriteNodeProperty(std::ostream& output, const NodePropertyRecord& record)
{
  output << "{\"node_id\":\"" << JsonEscape(record.node_id)
         << "\",\"tab_id\":\"" << JsonEscape(record.tab_id)
         << "\",\"tab_label\":\"" << JsonEscape(record.tab_label)
         << "\",\"group_id\":\"" << JsonEscape(record.group_id)
         << "\",\"group_label\":\"" << JsonEscape(record.group_label)
         << "\",\"field_key\":\"" << JsonEscape(record.field_key)
         << "\",\"field_label\":\"" << JsonEscape(record.field_label)
         << "\",\"value\":\"" << JsonEscape(record.value)
         << "\",\"unit\":\"" << JsonEscape(record.unit)
         << "\",\"value_type\":\"" << JsonEscape(record.value_type)
         << "\",\"source\":\"" << JsonEscape(record.source)
         << "\",\"display_order\":" << record.display_order
         << ",\"read_only\":" << (record.read_only ? "true" : "false")
         << '}';
}

// 用途：写出一个原生设计特征 ResultOUT 拓扑摘要；它不等同于最终主实体 Face 映射。
void WriteNativeFeatureResult(std::ostream& output, const NativeFeatureResultRecord& record)
{
  output << "{\"result_id\":\"" << JsonEscape(record.result_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"source_kind\":\"" << JsonEscape(record.source_kind)
         << "\",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"vertex_count\":" << record.vertex_count
         << ",\"edge_count\":" << record.edge_count
         << ",\"face_count\":" << record.face_count
         << ",\"volume_count\":" << record.volume_count
         << ",\"final_body_mapping_status\":\"" << JsonEscape(record.final_body_mapping_status)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出一个 ResultOUT cell 明细；它是原生特征结果体拓扑，不是最终主实体 Face 的替代品。
void WriteNativeFeatureResultCell(std::ostream& output, const NativeFeatureResultCellRecord& record)
{
  output << "{\"result_cell_id\":\"" << JsonEscape(record.result_cell_id)
         << "\",\"result_id\":\"" << JsonEscape(record.result_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"source_kind\":\"" << JsonEscape(record.source_kind)
         << "\",\"result_cell_index\":" << record.result_cell_index
         << ",\"dimension\":" << record.dimension
         << ",\"cell_kind\":\"" << JsonEscape(record.cell_kind)
         << "\",\"center_mm\":";
  if (record.has_center)
    output << '[' << std::setprecision(15) << record.center_mm[0] << ','
           << record.center_mm[1] << ',' << record.center_mm[2] << ']';
  else
    output << "null";
  output << ",\"area_mm2\":";
  WriteOptionalNumber(output, record.area_mm2_available, record.area_mm2);
  output << ",\"length_mm\":";
  WriteOptionalNumber(output, record.length_mm_available, record.length_mm);
  output << ",\"boundary_result_cell_ids\":";
  WriteStringArray(output, record.boundary_result_cell_ids);
  output << ",\"read_status\":\"" << JsonEscape(record.read_status)
         << "\",\"stable_id_method\":\"" << JsonEscape(record.stable_id_method)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写出 ResultOUT cell 到最终 Face 的候选映射尝试；candidate 不等于已完成权威映射。
void WriteNativeFeatureTopologyLink(std::ostream& output, const NativeFeatureTopologyLinkRecord& record)
{
  output << "{\"link_id\":\"" << JsonEscape(record.link_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"result_id\":\"" << JsonEscape(record.result_id)
         << "\",\"result_cell_id\":\"" << JsonEscape(record.result_cell_id)
         << "\",\"final_cell_id\":\"" << JsonEscape(record.final_cell_id)
         << "\",\"final_body_id\":\"" << JsonEscape(record.final_body_id)
         << "\",\"mapping_direction\":\"" << JsonEscape(record.mapping_direction)
         << "\",\"mapping_status\":\"" << JsonEscape(record.mapping_status)
         << "\",\"mapping_method\":\"" << JsonEscape(record.mapping_method)
         << "\",\"authority\":\"" << JsonEscape(record.authority.empty() ? record.mapping_method : record.authority)
         << "\",\"persistent_reference\":\"" << JsonEscape(record.persistent_reference)
         << "\",\"relation_kind\":\"" << JsonEscape(record.relation_kind.empty() ? "candidate" : record.relation_kind)
         << "\",\"confidence\":" << std::setprecision(15) << record.confidence
         << ",\"center_residual_mm\":" << record.center_residual_mm
         << ",\"measure_residual\":" << record.measure_residual
         << ",\"candidate_count\":" << record.candidate_count
         << ",\"candidate_final_cell_ids\":";
  WriteStringArray(output, record.candidate_final_cell_ids);
  output << ",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids);
  output << '}';
}

// 用途：写一条参数消费索引，parameter_id 始终复用原 Feature ID。
void WriteParameter(std::ostream& output, const ParameterRecord& record)
{
  output << "{\"parameter_id\":\"" << JsonEscape(record.parameter_id)
         << "\",\"owner_feature_id\":\"" << JsonEscape(record.owner_feature_id)
         << "\",\"parent_id\":\"" << JsonEscape(record.parent_id)
         << "\",\"tree_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"parameter_name\":\"" << JsonEscape(record.parameter_name)
         << "\",\"parameter_kind\":\"" << JsonEscape(record.parameter_kind)
         << "\",\"value_status\":\"" << JsonEscape(record.value_status)
         << "\",\"value_source\":\"" << JsonEscape(record.value_source)
         << "\",\"value_text\":\"" << JsonEscape(record.value_text)
         << "\",\"raw_display_text\":\"" << JsonEscape(record.raw_display_text)
         << "\",\"normalized_numeric_value\":";
  WriteOptionalNumber(output, record.has_normalized_numeric_value, record.normalized_numeric_value);
  output << ",\"normalized_unit\":\"" << JsonEscape(record.normalized_unit)
         << "\",\"normalization_status\":\"" << JsonEscape(record.normalization_status)
         << "\",\"decoder_id\":\"" << JsonEscape(record.decoder_id)
         << "\",\"ownership_status\":\"" << JsonEscape(record.ownership_status)
         << "\",\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids); output << '}';
}

// 用途：写一条声明式业务特征及其来源证据，明确没有执行几何识别。
void WriteBusinessFeature(std::ostream& output, const BusinessFeatureRecord& record)
{
  output << "{\"business_feature_id\":\"" << JsonEscape(record.business_feature_id)
         << "\",\"source_feature_id\":\"" << JsonEscape(record.source_feature_id)
         << "\",\"feature_kind\":\"" << JsonEscape(record.feature_kind)
         << "\",\"display_name\":\"" << JsonEscape(record.display_name)
         << "\",\"normalized_name\":\"" << JsonEscape(record.normalized_name)
         << "\",\"recognition_method\":\"" << JsonEscape(record.recognition_method)
         << "\",\"classification_status\":\"" << JsonEscape(record.classification_status)
         << "\",\"confidence\":\"" << JsonEscape(record.confidence)
         << "\",\"container_id\":\"" << JsonEscape(record.container_id)
         << "\",\"tree_path\":\"" << JsonEscape(record.tree_path)
         << "\",\"parameter_ids\":";
  WriteStringArray(output, record.parameter_ids);
  output << ",\"parameters\":{";
  std::map<std::string, BusinessParameterData>::const_iterator parameter = record.parameters.begin();
  for (; parameter != record.parameters.end(); ++parameter)
  {
    if (parameter != record.parameters.begin()) output << ',';
    output << '"' << JsonEscape(parameter->first) << "\":{\"parameter_id\":\""
           << JsonEscape(parameter->second.parameter_id) << "\",\"raw_value\":\""
           << JsonEscape(parameter->second.raw_value) << "\",\"normalized_numeric_value\":";
    WriteOptionalNumber(output, parameter->second.has_normalized_numeric_value,
                        parameter->second.normalized_numeric_value);
    output << ",\"normalized_unit\":\"" << JsonEscape(parameter->second.normalized_unit)
           << "\",\"value_status\":\"" << JsonEscape(parameter->second.value_status) << "\"}";
  }
  output << "},\"evidence\":[";
  std::vector<BusinessFeatureEvidence>::const_iterator evidence = record.evidence.begin();
  for (; evidence != record.evidence.end(); ++evidence)
  {
    if (evidence != record.evidence.begin()) output << ',';
    output << "{\"kind\":\"" << JsonEscape(evidence->kind) << "\",\"value\":\""
           << JsonEscape(evidence->value) << "\"}";
  }
  output << "],\"geometry_recognition_performed\":false,"
         << "\"native_part_design_feature_confirmed\":false,\"diagnostic_ids\":";
  WriteStringArray(output, record.diagnostic_ids); output << '}';
}

// 用途：以 binary+truncate 打开 staging 产物，失败时返回明确文件路径。
bool OpenOutput(std::ofstream& output, const std::string& path, std::string& error)
{
  output.open(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
  if (!output) { error = std::string("cannot write output file: ") + path; return false; }
  return true;
}

// 用途：刷新并关闭产物，同时检查延迟到 flush/close 才暴露的磁盘错误。
bool FinishOutput(std::ofstream& output, const char* artifact, std::string& error)
{
  output.flush();
  if (!output) { error = std::string("write failed for artifact: ") + artifact; output.close(); return false; }
  output.close();
  if (!output) { error = std::string("close failed for artifact: ") + artifact; return false; }
  return true;
}

// 用途：返回文件字节数；Manifest 产物统计使用磁盘实际值而不是内存估算。
unsigned long FileSize(const std::string& path)
{
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return data.nFileSizeLow;
}

// 用途：在写盘前验证所有关系和派生索引都能反查原始 Feature，禁止悬空引用进入正式结果。
bool ValidateReferences(const std::vector<FeatureRecord>& features,
                        const std::vector<RelationRecord>& relations,
                        const std::vector<ParameterRecord>& parameters,
                        const std::vector<BusinessFeatureRecord>& business_features,
                        std::string& error)
{
  std::set<std::string> feature_ids;
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature) feature_ids.insert(feature->feature_id);
  std::vector<RelationRecord>::const_iterator relation = relations.begin();
  for (; relation != relations.end(); ++relation)
    if (feature_ids.find(relation->from_id) == feature_ids.end() ||
        feature_ids.find(relation->to_id) == feature_ids.end())
    { error = "relation endpoint does not exist in features"; return false; }
  std::vector<ParameterRecord>::const_iterator parameter = parameters.begin();
  for (; parameter != parameters.end(); ++parameter)
  {
    if (feature_ids.find(parameter->parameter_id) == feature_ids.end())
    { error = "parameter_id does not exist in features"; return false; }
    if (!parameter->owner_feature_id.empty() &&
        feature_ids.find(parameter->owner_feature_id) == feature_ids.end())
    { error = "parameter owner_feature_id does not exist in features"; return false; }
  }
  std::vector<BusinessFeatureRecord>::const_iterator business = business_features.begin();
  for (; business != business_features.end(); ++business)
  {
    if (feature_ids.find(business->source_feature_id) == feature_ids.end())
    { error = "business source_feature_id does not exist in features"; return false; }
    std::vector<std::string>::const_iterator id = business->parameter_ids.begin();
    for (; id != business->parameter_ids.end(); ++id)
      if (feature_ids.find(*id) == feature_ids.end())
      { error = "business parameter_id does not exist in features"; return false; }
  }
  return true;
}
}

// 用途：让原生孔载荷自行写出兼容的 native_hole 属性，中央 Writer 不包含类型判断。
void NativeHolePayload::WriteJsonProperty(std::ostream& output) const
{
  output << "\"native_hole\":";
  WriteNativeHole(output, _data);
}

// 用途：让 Pad/Pocket 的 Prism 载荷自行写出 native_prism 属性，中央 Writer 不包含类型分支。
void NativePrismPayload::WriteJsonProperty(std::ostream& output) const
{
  output << "\"native_prism\":";
  WriteNativePrism(output, _data);
}

void NativeFeatureParameterPayload::WriteJsonProperty(std::ostream& output) const
{
  output << "\"native_feature_parameters\":";
  WriteNativeFeatureParameters(output, _data);
}

// 用途：创建 JSON Writer 并保存普通 JSON 是否采用易读空白。
JsonArtifactWriter::JsonArtifactWriter(bool pretty) : _pretty(pretty) {}

// 用途：为简单调用方建立参数/业务派生索引，再调用完整事务写出入口。
bool JsonArtifactWriter::Write(const std::vector<FeatureRecord>& features,
                               const std::vector<RelationRecord>& relations,
                               ParseContext& context,
                               const std::string& output_dir,
                               std::string& error)
{
  std::vector<ParameterRecord> parameters;
  std::vector<BusinessFeatureRecord> business_features;
  ParameterRecordBuilder::Build(features, relations, context, parameters);
  DeclaredBusinessFeatureAggregator::Aggregate(features, relations, parameters, context, business_features);
  return Write(features, relations, parameters, business_features, context, output_dir, error);
}

// 用途：一次写完 staging、计算统计与哈希、最后生成 Coverage/Manifest，再原子提交目录。
bool JsonArtifactWriter::Write(const std::vector<FeatureRecord>& features,
                               const std::vector<RelationRecord>& relations,
                               const std::vector<ParameterRecord>& parameters,
                               const std::vector<BusinessFeatureRecord>& business_features,
                               ParseContext& context,
                               const std::string& output_dir,
                               std::string& error)
{
  if (!CoverageTracker::Validate(context.statistics)) { error = "coverage conservation failed"; return false; }
  if (!ValidateReferences(features, relations, parameters, business_features, error)) return false;
  BuildNativeTreeNodes(features, context);
  BuildNodeProperties(context);
  const DWORD output_start = GetTickCount();
  const std::string staging = output_dir + ".cadparse_stage";
  const DWORD existing_output = GetFileAttributesA(output_dir.c_str());
  if (existing_output != INVALID_FILE_ATTRIBUTES && !(existing_output & FILE_ATTRIBUTE_DIRECTORY))
  { error = "output path exists and is not a directory"; return false; }
  RemoveTree(staging);
  if (!EnsureDirectory(staging, error)) return false;

  std::ofstream output;
  if (!OpenOutput(output, JoinPath(staging, "features.jsonl"), error)) return false;
  std::vector<FeatureRecord>::const_iterator feature = features.begin();
  for (; feature != features.end(); ++feature) { WriteFeature(output, *feature); output << '\n'; }
  if (!FinishOutput(output, "features.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_features.jsonl"), error)) return false;
  for (feature = features.begin(); feature != features.end(); ++feature)
  { WriteNativeFeature(output, *feature); output << '\n'; }
  if (!FinishOutput(output, "native_features.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "decoder_registry.json"), error)) return false;
  output << "{\"registry_version\":\"" << CAD_PARSE_REGISTRY_VERSION
         << "\",\"decoder_bundle_version\":\"" << CAD_PARSE_DECODER_BUNDLE_VERSION
         << "\",\"decoders\":["
         << "{\"decoder_id\":\"native_hole_decoder\",\"capability\":\"CATIAHole\",\"status\":\"registered\"},"
         << "{\"decoder_id\":\"native_pad_decoder\",\"capability\":\"CATIAPad\",\"status\":\"registered\"},"
         << "{\"decoder_id\":\"native_pocket_decoder\",\"capability\":\"CATIAPocket\",\"status\":\"registered\"},"
         << "{\"decoder_id\":\"native_feature_parameter_decoder\",\"capability\":\"NativeFeatureParameters\",\"status\":\"registered\"},"
         << "{\"decoder_id\":\"knowledgeware_string_parameter_decoder\",\"capability\":\"CATIAStrParam\",\"status\":\"registered\"}"
         << "]}\n";
  if (!FinishOutput(output, "decoder_registry.json", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_topology_bodies.jsonl"), error)) return false;
  std::vector<NativeTopologyBodyRecord>::const_iterator topology_body =
    context.topology_bodies.begin();
  for (; topology_body != context.topology_bodies.end(); ++topology_body)
  { WriteNativeTopologyBody(output, *topology_body); output << '\n'; }
  if (!FinishOutput(output, "native_topology_bodies.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_topology_cells.jsonl"), error)) return false;
  std::vector<NativeTopologyCellRecord>::const_iterator topology_cell =
    context.topology_cells.begin();
  for (; topology_cell != context.topology_cells.end(); ++topology_cell)
  { WriteNativeTopologyCell(output, *topology_cell); output << '\n'; }
  if (!FinishOutput(output, "native_topology_cells.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_topology_wires.jsonl"), error)) return false;
  std::vector<NativeTopologyWireRecord>::const_iterator topology_wire =
    context.topology_wires.begin();
  for (; topology_wire != context.topology_wires.end(); ++topology_wire)
  { WriteNativeTopologyWire(output, *topology_wire); output << '\n'; }
  if (!FinishOutput(output, "native_topology_wires.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_topology_coedges.jsonl"), error)) return false;
  std::vector<NativeTopologyCoedgeRecord>::const_iterator topology_coedge =
    context.topology_coedges.begin();
  for (; topology_coedge != context.topology_coedges.end(); ++topology_coedge)
  { WriteNativeTopologyCoedge(output, *topology_coedge); output << '\n'; }
  if (!FinishOutput(output, "native_topology_coedges.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_mesh_face_map.jsonl"), error)) return false;
  std::vector<NativeMeshFaceMapRecord>::const_iterator mesh_face =
    context.mesh_face_maps.begin();
  for (; mesh_face != context.mesh_face_maps.end(); ++mesh_face)
  { WriteNativeMeshFaceMap(output, *mesh_face); output << '\n'; }
  if (!FinishOutput(output, "native_mesh_face_map.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_mesh_triangles.jsonl"), error)) return false;
  std::vector<NativeMeshTriangleRecord>::const_iterator mesh_triangle =
    context.mesh_triangles.begin();
  for (; mesh_triangle != context.mesh_triangles.end(); ++mesh_triangle)
  { WriteNativeMeshTriangle(output, *mesh_triangle); output << '\n'; }
  if (!FinishOutput(output, "native_mesh_triangles.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "fta_sets.jsonl"), error)) return false;
  std::vector<FtaSetRecord>::const_iterator fta_set = context.fta_sets.begin();
  for (; fta_set != context.fta_sets.end(); ++fta_set)
  { WriteFtaSet(output, *fta_set); output << '\n'; }
  if (!FinishOutput(output, "fta_sets.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "fta_semantics.jsonl"), error)) return false;
  std::vector<FtaSemanticRecord>::const_iterator fta_semantic = context.fta_semantics.begin();
  for (; fta_semantic != context.fta_semantics.end(); ++fta_semantic)
  { WriteFtaSemantic(output, *fta_semantic); output << '\n'; }
  if (!FinishOutput(output, "fta_semantics.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "fta_topology_links.jsonl"), error)) return false;
  std::vector<FtaTopologyLinkRecord>::const_iterator fta_link = context.fta_topology_links.begin();
  for (; fta_link != context.fta_topology_links.end(); ++fta_link)
  { WriteFtaTopologyLink(output, *fta_link); output << '\n'; }
  if (!FinishOutput(output, "fta_topology_links.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "product_references.jsonl"), error)) return false;
  std::vector<ProductReferenceRecord>::const_iterator product_reference =
    context.product_references.begin();
  for (; product_reference != context.product_references.end(); ++product_reference)
  { WriteProductReference(output, *product_reference); output << '\n'; }
  if (!FinishOutput(output, "product_references.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "product_instances.jsonl"), error)) return false;
  std::vector<ProductInstanceRecord>::const_iterator product_instance =
    context.product_instances.begin();
  for (; product_instance != context.product_instances.end(); ++product_instance)
  { WriteProductInstance(output, *product_instance); output << '\n'; }
  if (!FinishOutput(output, "product_instances.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_tree_nodes.jsonl"), error)) return false;
  std::vector<NativeTreeNodeRecord>::const_iterator native_tree_node =
    context.native_tree_nodes.begin();
  for (; native_tree_node != context.native_tree_nodes.end(); ++native_tree_node)
  { WriteNativeTreeNode(output, *native_tree_node); output << '\n'; }
  if (!FinishOutput(output, "native_tree_nodes.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "node_properties.jsonl"), error)) return false;
  std::vector<NodePropertyRecord>::const_iterator node_property =
    context.node_properties.begin();
  for (; node_property != context.node_properties.end(); ++node_property)
  { WriteNodeProperty(output, *node_property); output << '\n'; }
  if (!FinishOutput(output, "node_properties.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_feature_results.jsonl"), error)) return false;
  std::vector<NativeFeatureResultRecord>::const_iterator feature_result =
    context.native_feature_results.begin();
  for (; feature_result != context.native_feature_results.end(); ++feature_result)
  { WriteNativeFeatureResult(output, *feature_result); output << '\n'; }
  if (!FinishOutput(output, "native_feature_results.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_feature_result_cells.jsonl"), error)) return false;
  std::vector<NativeFeatureResultCellRecord>::const_iterator result_cell =
    context.native_feature_result_cells.begin();
  for (; result_cell != context.native_feature_result_cells.end(); ++result_cell)
  { WriteNativeFeatureResultCell(output, *result_cell); output << '\n'; }
  if (!FinishOutput(output, "native_feature_result_cells.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "native_feature_topology_links.jsonl"), error)) return false;
  std::vector<NativeFeatureTopologyLinkRecord>::const_iterator topology_link =
    context.native_feature_topology_links.begin();
  for (; topology_link != context.native_feature_topology_links.end(); ++topology_link)
  { WriteNativeFeatureTopologyLink(output, *topology_link); output << '\n'; }
  if (!FinishOutput(output, "native_feature_topology_links.jsonl", error)) return false;

  // 用途：能力状态只从统一 evaluator 生成，避免 capabilities 与 matrix 各自套用不同规则。
  std::vector<CapabilityEvaluation> capabilities;
  long native_hole_decoded = 0;
  long native_prism_decoded = 0;
  long native_generic = 0;
  BuildCapabilityEvaluations(features, context, capabilities,
                             native_hole_decoded, native_prism_decoded, native_generic);

  if (!OpenOutput(output, JoinPath(staging, "capabilities.json"), error)) return false;
  output << "{\"spec_tree_extraction\":\"partial\""
         ;
  std::vector<CapabilityEvaluation>::const_iterator cap = capabilities.begin();
  for (; cap != capabilities.end(); ++cap)
  {
    if (cap->name == "spec_tree_extraction") continue;
    output << ",\"" << JsonEscape(cap->name) << "\":\"" << JsonEscape(cap->status) << "\"";
  }
  output
         << ",\"native_feature_record_count\":" << features.size()
         << ",\"native_hole_decoded_count\":" << native_hole_decoded
         << ",\"native_prism_decoded_count\":" << native_prism_decoded
         << ",\"native_generic_count\":" << native_generic
         << ",\"native_topology_body_count\":" << context.topology_bodies.size()
         << ",\"native_topology_cell_count\":" << context.topology_cells.size()
         << ",\"native_topology_wire_count\":" << context.topology_wires.size()
         << ",\"native_topology_coedge_count\":" << context.topology_coedges.size()
         << ",\"native_mesh_face_map_count\":" << context.mesh_face_maps.size()
         << ",\"native_mesh_triangle_count\":" << context.mesh_triangles.size()
         << ",\"fta_set_count\":" << context.fta_sets.size()
         << ",\"fta_semantic_count\":" << context.fta_semantics.size()
         << ",\"fta_topology_link_count\":" << context.fta_topology_links.size()
         << ",\"product_reference_count\":" << context.product_references.size()
         << ",\"product_instance_count\":" << context.product_instances.size()
         << ",\"native_tree_node_count\":" << context.native_tree_nodes.size()
         << ",\"node_property_count\":" << context.node_properties.size()
         << ",\"native_feature_result_count\":" << context.native_feature_results.size()
         << ",\"native_feature_result_cell_count\":" << context.native_feature_result_cells.size()
         << ",\"native_feature_topology_link_count\":" << context.native_feature_topology_links.size()
         << ",\"capability_metrics\":{";
  for (cap = capabilities.begin(); cap != capabilities.end(); ++cap)
  {
    if (cap != capabilities.begin()) output << ',';
    output << "\"" << JsonEscape(cap->name) << "\":{";
    WriteCapabilityCounts(output, cap->counts);
    output << ",\"reason_code\":\"" << JsonEscape(cap->reason_code) << "\"}";
  }
  output
         << "},\"notes\":[\"R21 Public CATIAHole, CATIAPad and CATIAPocket decoders are registered when their StartUp candidates expose the matching Public interface\",\"R21 Public CATIPrtPart::GetSolid and CATTopology cell enumeration emit revision-local body/cell topology when available\",\"R21 Public CATICGMBodyTessellator emits Face to triangle range evidence when tessellation succeeds\",\"CATIShapeFeatureBody ResultOUT cell identity with final solid is preserved only as runtime_cell_identity/survives_to_final evidence; it is not generated/modified/consumed history and has no persistent reference\",\"native_feature_topology_mapping.coverage_ratio is authoritative_coverage_ratio for schema compatibility; runtime_coverage_ratio is reported separately and cannot make the capability complete\",\"CATProduct capability is split into product_structure_extraction and instance_transform_extraction; root-only or CATPart rows are not transform evidence\",\"StartupTypeCanonicalDecoder performs type recognition only; payload_extraction_status remains not_implemented until a dedicated R21 Public decoder reads parameters\",\"R21 Public CATITPSDocument/CATITPSSet can emit FTA set-level counts when the document exposes TPS data\",\"FTA-to-topology mapping is still not emitted by this CAA revision\"]}\n";
  if (!FinishOutput(output, "capabilities.json", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "capability_matrix.json"), error)) return false;
  output << "{\"schema_version\":\"" << CAD_PARSE_SCHEMA_VERSION
         << "\",\"capabilities\":[";
  for (cap = capabilities.begin(); cap != capabilities.end(); ++cap)
  {
    if (cap != capabilities.begin()) output << ',';
    output << "{\"name\":\"" << JsonEscape(cap->name)
            << "\",\"status\":\"" << JsonEscape(cap->status)
            << "\",\"reason_code\":\"" << JsonEscape(cap->reason_code)
            << "\",\"evidence_count\":" << cap->counts.evidence_count << ',';
    WriteCapabilityCounts(output, cap->counts);
    output << '}';
  }
  output << "]}\n";
  if (!FinishOutput(output, "capability_matrix.json", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "relations.jsonl"), error)) return false;
  std::vector<RelationRecord>::const_iterator relation = relations.begin();
  for (; relation != relations.end(); ++relation)
    output << "{\"kind\":\"" << JsonEscape(relation->kind) << "\",\"from_id\":\""
           << JsonEscape(relation->from_id) << "\",\"to_id\":\"" << JsonEscape(relation->to_id) << "\"}\n";
  if (!FinishOutput(output, "relations.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "parameters.jsonl"), error)) return false;
  std::vector<ParameterRecord>::const_iterator parameter = parameters.begin();
  for (; parameter != parameters.end(); ++parameter) { WriteParameter(output, *parameter); output << '\n'; }
  if (!FinishOutput(output, "parameters.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "business_features.jsonl"), error)) return false;
  std::vector<BusinessFeatureRecord>::const_iterator business = business_features.begin();
  for (; business != business_features.end(); ++business) { WriteBusinessFeature(output, *business); output << '\n'; }
  if (!FinishOutput(output, "business_features.jsonl", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "diagnostics.json"), error)) return false;
  output << '[';
  std::vector<DiagnosticRecord>::const_iterator diagnostic = context.diagnostics.begin();
  for (; diagnostic != context.diagnostics.end(); ++diagnostic)
  {
    if (diagnostic != context.diagnostics.begin()) output << ',';
    output << "{\"diagnostic_id\":\"" << JsonEscape(diagnostic->diagnostic_id)
           << "\",\"severity\":\"" << JsonEscape(diagnostic->severity)
           << "\",\"stage\":\"" << JsonEscape(diagnostic->stage)
           << "\",\"code\":\"" << JsonEscape(diagnostic->code)
           << "\",\"message\":\"" << JsonEscape(diagnostic->message)
           << "\",\"feature_id\":\"" << JsonEscape(diagnostic->feature_id) << "\"}";
  }
  output << "]\n";
  if (!FinishOutput(output, "diagnostics.json", error)) return false;

  if (!OpenOutput(output, JoinPath(staging, "parser.log"), error)) return false;
  output << "schema=" << CAD_PARSE_SCHEMA_VERSION << "\ninput=" << JsonEscape(context.metadata.input_file_name)
         << "\nfeatures=" << features.size() << "\nparameters=" << parameters.size()
         << "\nbusiness_features=" << business_features.size() << "\ncoverage_conserved=true\n";
  feature = features.begin();
  for (; feature != features.end(); ++feature)
    output << "decoder_match feature_id=" << feature->feature_id
           << " decoder=" << feature->decoder_id << " level=" << feature->decode_level
           << " status=" << feature->decode_status << '\n';
  diagnostic = context.diagnostics.begin();
  for (; diagnostic != context.diagnostics.end(); ++diagnostic)
    output << "diagnostic id=" << diagnostic->diagnostic_id << " stage=" << diagnostic->stage
           << " code=" << diagnostic->code << " feature_id=" << diagnostic->feature_id
           << " message=" << JsonEscape(diagnostic->message) << '\n';
  if (!FinishOutput(output, "parser.log", error)) return false;

  context.statistics.output_ms = static_cast<long>(GetTickCount() - output_start);
  context.statistics.total_ms += context.statistics.output_ms;
  if (!OpenOutput(output, JoinPath(staging, "coverage.json"), error)) return false;
  output << "{\"enumerated_total\":" << context.statistics.enumerated_total
         << ",\"typed_count\":" << context.statistics.typed_count
         << ",\"generic_count\":" << context.statistics.generic_count
         << ",\"opaque_count\":" << context.statistics.opaque_count
         << ",\"failed_count\":" << context.statistics.failed_count
         << ",\"container_count\":" << context.statistics.container_count
         << ",\"relation_count\":" << context.statistics.relation_count
         << ",\"unknown_native_type_count\":" << context.statistics.unknown_native_type_count
         << ",\"probe_supported_count\":" << context.statistics.probe_supported_count
         << ",\"probe_unsupported_count\":" << context.statistics.probe_unsupported_count
         << ",\"probe_exception_count\":" << context.statistics.probe_exception_count
         << ",\"probe_not_attempted_count\":" << context.statistics.probe_not_attempted_count
         << ",\"probe_outcomes\":"; WriteCountMap(output, context.statistics.probe_outcome_counts);
  output << ",\"not_up_to_date_count\":" << context.statistics.not_up_to_date_count
         << ",\"not_up_to_date_by_native_type\":"; WriteCountMap(output, context.statistics.not_up_to_date_by_native_type);
  output << ",\"not_up_to_date_by_decoder\":"; WriteCountMap(output, context.statistics.not_up_to_date_by_decoder);
  output << ",\"not_up_to_date_feature_ids\":"; WriteStringArray(output, context.statistics.not_up_to_date_feature_ids);
  output << ",\"model_contains_stale_objects\":" << (context.statistics.not_up_to_date_count ? "true" : "false")
         << ",\"parameter_total\":" << context.statistics.parameter_total
         << ",\"parameter_value_success\":" << context.statistics.parameter_value_success
         << ",\"parameter_value_partial\":" << context.statistics.parameter_value_partial
         << ",\"parameter_value_unavailable\":" << context.statistics.parameter_value_unavailable
         << ",\"parameter_failed\":" << context.statistics.parameter_failed
         << ",\"declared_business_feature_total\":" << context.statistics.declared_business_feature_total
         << ",\"declared_boss_count\":" << context.statistics.declared_boss_count
         << ",\"declared_hole_count\":" << context.statistics.declared_hole_count
         << ",\"declared_slot_count\":" << context.statistics.declared_slot_count
         << ",\"declared_unknown_count\":" << context.statistics.declared_unknown_count
         << ",\"business_feature_with_parameter_count\":" << context.statistics.business_feature_with_parameter_count
         << ",\"business_feature_with_all_values_count\":" << context.statistics.business_feature_with_all_values_count
         << ",\"business_feature_with_partial_values_count\":" << context.statistics.business_feature_with_partial_values_count
         << ",\"business_feature_without_values_count\":" << context.statistics.business_feature_without_values_count
         << ",\"orphan_parameter_count\":" << context.statistics.orphan_parameter_count
         << ",\"ambiguous_parameter_owner_count\":" << context.statistics.ambiguous_parameter_owner_count
         << ",\"native_hole_candidate_count\":" << context.statistics.native_hole_candidate_count
         << ",\"native_hole_success_count\":" << context.statistics.native_hole_success_count
         << ",\"native_hole_partial_count\":" << context.statistics.native_hole_partial_count
         << ",\"native_hole_unsupported_count\":" << context.statistics.native_hole_unsupported_count
         << ",\"native_hole_exception_count\":" << context.statistics.native_hole_exception_count
         << ",\"document_open_ms\":" << context.statistics.document_open_ms
         << ",\"traversal_ms\":" << context.statistics.traversal_ms
         << ",\"decoder_ms\":" << context.statistics.decoder_ms
         << ",\"output_ms\":" << context.statistics.output_ms
         << ",\"total_ms\":" << context.statistics.total_ms
         << ",\"decoder_hits\":"; WriteCountMap(output, context.statistics.decoder_hits); output << "}\n";
  if (!FinishOutput(output, "coverage.json", error)) return false;

  context.metadata.execution_finished_utc = UtcNowIso8601();
  const char* names[] = { "features.jsonl", "native_features.jsonl", "native_tree_nodes.jsonl", "node_properties.jsonl", "decoder_registry.json", "native_feature_results.jsonl", "native_feature_result_cells.jsonl", "native_feature_topology_links.jsonl", "native_topology_bodies.jsonl", "native_topology_cells.jsonl", "native_topology_wires.jsonl", "native_topology_coedges.jsonl", "native_mesh_face_map.jsonl", "native_mesh_triangles.jsonl", "fta_sets.jsonl", "fta_semantics.jsonl", "fta_topology_links.jsonl", "product_references.jsonl", "product_instances.jsonl", "relations.jsonl", "parameters.jsonl", "business_features.jsonl", "capabilities.json", "capability_matrix.json", "diagnostics.json", "coverage.json", "parser.log" };
  std::map<std::string, std::string> artifact_hashes;
  std::map<std::string, unsigned long> artifact_sizes;
  int artifact = 0;
  const int artifact_count = sizeof(names) / sizeof(names[0]);
  for (; artifact < artifact_count; ++artifact)
  {
    const std::string path = JoinPath(staging, names[artifact]);
    std::string hash_error;
    const std::string hash = Sha256File(path, hash_error);
    if (hash.empty()) { error = hash_error; return false; }
    artifact_hashes[names[artifact]] = hash;
    artifact_sizes[names[artifact]] = FileSize(path);
  }
  const char* spacing = _pretty ? "\n  " : "";
  if (!OpenOutput(output, JoinPath(staging, "manifest.json"), error)) return false;
  output << '{' << spacing << "\"schema_version\":\"" << JsonEscape(context.metadata.schema_version)
         << "\"," << spacing << "\"parser_version\":\"" << JsonEscape(context.metadata.parser_version)
         << "\"," << spacing << "\"registry_version\":\"" << JsonEscape(context.metadata.registry_version)
         << "\"," << spacing << "\"decoder_bundle_version\":\"" << JsonEscape(context.metadata.decoder_bundle_version)
         << "\"," << spacing << "\"parser_git_commit\":\"" << JsonEscape(context.metadata.parser_git_commit)
         << "\"," << spacing << "\"parser_git_commit_source\":\"" << JsonEscape(context.metadata.parser_git_commit_source)
         << "\"," << spacing << "\"build_timestamp_utc\":\"" << JsonEscape(context.metadata.build_timestamp_utc)
         << "\"," << spacing << "\"build_timestamp_source\":\"" << JsonEscape(context.metadata.build_timestamp_source)
         << "\"," << spacing << "\"execution_started_utc\":\"" << JsonEscape(context.metadata.execution_started_utc)
         << "\"," << spacing << "\"execution_finished_utc\":\"" << JsonEscape(context.metadata.execution_finished_utc)
         << "\"," << spacing << "\"document_kind\":\"" << JsonEscape(context.metadata.document_kind)
         << "\"," << spacing << "\"input\":{\"file_name\":\"" << JsonEscape(context.metadata.input_file_name)
         << "\",\"size_bytes\":" << context.metadata.input_size_bytes
         << ",\"sha256\":\"" << JsonEscape(context.metadata.input_sha256)
         << "\",\"absolute_path_included\":" << (context.metadata.include_source_path ? "true" : "false");
  if (context.metadata.include_source_path) output << ",\"source_path\":\"" << JsonEscape(context.metadata.input_source_path) << '"';
  output << "}," << spacing << "\"runtime\":{\"catia_release\":\"" << JsonEscape(context.metadata.runtime_catia_release)
         << "\",\"service_pack\":\"" << JsonEscape(context.metadata.runtime_service_pack)
         << "\",\"hotfix\":\"" << JsonEscape(context.metadata.runtime_hotfix)
         << "\",\"value_source\":\"" << JsonEscape(context.metadata.runtime_value_source) << "\"},"
         << spacing << "\"source_file_hint\":{\"release\":\"" << JsonEscape(context.metadata.source_hint_release)
         << "\",\"service_pack\":\"" << JsonEscape(context.metadata.source_hint_service_pack)
         << "\",\"hotfix\":\"" << JsonEscape(context.metadata.source_hint_hotfix)
         << "\",\"value_source\":\"" << JsonEscape(context.metadata.source_hint_value_source)
         << "\",\"confidence\":\"" << JsonEscape(context.metadata.source_hint_confidence) << "\"},"
         << spacing << "\"discovery\":{\"entrypoints\":";
  WriteStringArray(output, context.metadata.discovery_entrypoints);
  output << ",\"coverage_scope\":\"" << JsonEscape(context.metadata.discovery_coverage_scope) << "\"},"
         << spacing << "\"model_contains_stale_objects\":" << (context.statistics.not_up_to_date_count ? "true" : "false")
         << ',' << spacing << "\"has_geometry\":" << ((!context.topology_cells.empty() || !context.mesh_face_maps.empty()) ? "true" : "false")
         << ',' << spacing << "\"native_tree_node_count\":" << context.native_tree_nodes.size()
         << ',' << spacing << "\"node_property_count\":" << context.node_properties.size()
         << ',' << spacing << "\"product_reference_count\":" << context.product_references.size()
         << ',' << spacing << "\"product_instance_count\":" << context.product_instances.size()
         << ',' << spacing << "\"artifacts\":{";
  artifact = 0;
  for (; artifact < artifact_count; ++artifact)
  {
    if (artifact) output << ',';
    output << '"' << names[artifact] << "\":{\"size_bytes\":" << artifact_sizes[names[artifact]]
           << ",\"sha256\":\"" << artifact_hashes[names[artifact]] << "\"}";
  }
  output << "}}\n";
  if (!FinishOutput(output, "manifest.json", error)) return false;
  if (!CommitStaging(staging, output_dir, error)) return false;
  return true;
}
}
