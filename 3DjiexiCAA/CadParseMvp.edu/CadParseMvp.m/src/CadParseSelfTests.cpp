// 本文件实现不依赖 CATIA 许可证的核心自测，通过伪对象和伪 Decoder 验证 Registry、兜底和 IR。
// 测试沿用 C++03 与极小 TestRunner，不引入 VS2008 环境中不存在的第三方测试框架。
#include "CadParseContracts.h"
#include "CadParseIR.h"

#include <fstream>
#include <limits>
#include <cstdio>
#include <iostream>
#include <set>
#include <sstream>

using namespace std;

namespace cadparse
{
// 可控制“基础属性是否可读”的伪原生对象视图，用来驱动 Generic 和 Opaque 路径。
class FakeView : public INativeObjectView
{
public:
  // 用途：构造指定 native_type 的伪对象，并放入包含中文、引号的 UTF-8 显示名。
  FakeView(const char* type, bool readable = true) : _readable(readable)
  {
    _fingerprint.native_type = type;
    _fingerprint.display_name = "\xE4\xB8\xAD\xE6\x96\x87\"name";
  }

  // 用途：返回伪对象的稳定类型指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：按 _readable 开关模拟基础属性成功或失败，供兜底测试精确控制路径。
  bool ReadBasicAttributes(FeatureRecord& record, std::string& error) const
  {
    if (!_readable)
    {
      error = "read failure";
      return false;
    }
    record.attributes["name"] = _fingerprint.display_name;
    return true;
  }

private:
  TypeFingerprint _fingerprint;
  bool _readable;
};

// 可分别模拟成功、接口不支持和读取异常的 String 参数视图。
// 该视图不依赖 CAA，用来验证 Decoder 的匹配与错误隔离契约。
class FakeStringParameterView : public INativeObjectView, public IStringParameterView
{
public:
  // 用途：创建一个 String 参数测试对象，并指定读取结果和真实字符串值。
  FakeStringParameterView(StringParameterReadStatus status, const std::string& value,
                          const std::string& name)
    : _status(status), _value(value)
  {
    _fingerprint.startup_type = "String";
    _fingerprint.super_types.push_back("Literal");
    _fingerprint.display_name = name;
    _fingerprint.internal_name = name;
  }

  // 用途：返回测试对象的 String/Literal 类型指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：让 String Decoder 取得当前伪参数视图，不依赖编译器 RTTI。
  const IStringParameterView* GetStringParameterView() const { return this; }

  // 用途：为 Generic 兜底提供一个始终成功的基础属性读取路径。
  bool ReadBasicAttributes(FeatureRecord& record, std::string&) const
  {
    record.attributes["generic_read"] = "success";
    return true;
  }

  // 用途：按照构造时指定的结果模拟类型化 String 参数接口读取。
  StringParameterReadStatus ReadStringParameter(ParameterValueData& parameter,
                                                std::string& error) const
  {
    if (_status != StringParameterReadSuccess)
    {
      error = _status == StringParameterInterfaceUnsupported ? "interface unsupported" :
              "parameter read exception";
      return _status;
    }
    parameter.parameter_kind = "string";
    parameter.parameter_name = _fingerprint.display_name;
    parameter.value_status = "success";
    parameter.value_source = "typed_caa_value";
    parameter.value_text = _value;
    parameter.raw_display_text = "";
    parameter.is_read_only = "unknown";
    parameter.is_hidden = "unknown";
    return StringParameterReadSuccess;
  }

private:
  TypeFingerprint _fingerprint;
  StringParameterReadStatus _status;
  std::string _value;
};

// API 无关的 Hole 伪视图：用固定结构化数据覆盖成功、不支持和异常三条适配器路径。
class FakeNativeHoleView : public INativeObjectView, public INativeHoleView
{
public:
  // 用途：构造 Hole 或 Pocket 候选，并显式指定专用接口读取结果；名称不参与确认。
  FakeNativeHoleView(const char* startup_type, const char* display_name,
                     NativeHoleReadStatus status, const NativeHoleData& data)
    : _status(status), _data(data)
  {
    _fingerprint.startup_type = startup_type;
    _fingerprint.display_name = display_name;
    _fingerprint.internal_name = display_name;
  }

  // 用途：返回候选预筛选所需的稳定类型指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：按统一能力协议暴露原生孔视图，避免 Core 使用 RTTI。
  const INativeCapabilityView* FindCapability(const char* capability_id) const
  { return capability_id && std::string(capability_id) == "NativeHole" ? this : 0; }

  // 用途：模拟真实 CAA Adapter 的接口确认和结构化参数读取结果。
  NativeHoleReadStatus ReadNativeHole(NativeHoleData& output, std::string& error) const
  {
    output = _data;
    if (_status != NativeHoleReadSuccess)
      error = "fake native hole read failure";
    return _status;
  }

  // 用途：为专用 Decoder 失败后的 Generic 回退提供可验证的干净基础记录。
  bool ReadBasicAttributes(FeatureRecord& record, std::string&) const
  {
    record.attributes["generic_read"] = "success";
    return true;
  }

private:
  TypeFingerprint _fingerprint;
  NativeHoleReadStatus _status;
  NativeHoleData _data;
};

// API 无关的 Prism 伪能力：一个对象可以精确暴露 NativePad 或 NativePocket。
class FakeNativePrismCapability : public INativePrismView
{
public:
  // 用途：绑定能力名称、读取状态和待返回的结构化 Prism 数据。
  FakeNativePrismCapability(const char* capability_id, NativePrismReadStatus status,
                            const NativePrismData& data)
    : _capability_id(capability_id), _status(status), _data(data) {}

  // 用途：返回 NativePad 或 NativePocket，供 Decoder 验证能力来源。
  const char* GetCapabilityId() const { return _capability_id.c_str(); }

  // 用途：模拟 CAA Adapter 的 Pad/Pocket 接口确认和公共 Prism 参数读取。
  NativePrismReadStatus ReadNativePrism(const char* requested_capability,
                                        NativePrismData& output,
                                        std::string& error) const
  {
    if (!requested_capability || requested_capability != _capability_id)
    {
      error = "fake native prism capability mismatch";
      return NativePrismInterfaceUnsupported;
    }
    output = _data;
    if (_status != NativePrismReadSuccess)
      error = "fake native prism read failure";
    return _status;
  }

private:
  std::string _capability_id;
  NativePrismReadStatus _status;
  NativePrismData _data;
};

// API 无关的 Prism 伪对象：StartUp 只做候选，能力对象决定是否真正 Typed。
class FakeNativePrismView : public INativeObjectView
{
public:
  // 用途：创建 Pad/Pocket 候选，并可选择是否暴露对应原生能力。
  FakeNativePrismView(const char* startup_type, const char* display_name,
                      const char* exposed_capability, NativePrismReadStatus status,
                      const NativePrismData& data)
    : _capability(exposed_capability, status, data), _exposed_capability(exposed_capability)
  {
    _fingerprint.startup_type = startup_type;
    _fingerprint.display_name = display_name;
    _fingerprint.internal_name = display_name;
  }

  // 用途：返回候选预筛选所需的稳定类型指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }

  // 用途：按统一能力协议暴露 Pad/Pocket 伪视图。
  const INativeCapabilityView* FindCapability(const char* capability_id) const
  {
    return capability_id && std::string(capability_id) == _exposed_capability
      ? &_capability : 0;
  }

  // 用途：为专用 Decoder 失败后的 Generic 回退提供可验证的干净基础记录。
  bool ReadBasicAttributes(FeatureRecord& record, std::string&) const
  {
    record.attributes["generic_read"] = "success";
    return true;
  }

private:
  TypeFingerprint _fingerprint;
  FakeNativePrismCapability _capability;
  std::string _exposed_capability;
};

// 仅用于协议自测的合成能力，用来证明新增能力不需要扩展中央对象视图的方法列表。
class SyntheticCapability : public INativeCapabilityView
{
public:
  // 返回固定能力标识，测试不依赖 CATIA 或 CAA 头文件。
  const char* GetCapabilityId() const { return "SyntheticCapability"; }
};

// 仅用于协议自测的合成对象；它通过统一查找方法公开能力，而不是新增专用 Getter。
class SyntheticCapabilityObjectView : public INativeObjectView
{
public:
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }
  const INativeCapabilityView* FindCapability(const char* capability_id) const
  {
    return capability_id && std::string(capability_id) == _capability.GetCapabilityId()
      ? &_capability : 0;
  }
  bool ReadBasicAttributes(FeatureRecord&, std::string&) const { return true; }

private:
  TypeFingerprint _fingerprint;
  SyntheticCapability _capability;
};

// 合成载荷只用于验证通用所有权和写出协议，不代表任何生产特征类型。
class SyntheticPayload : public ITypedPayload
{
public:
  explicit SyntheticPayload(int value) : _value(value) { ++live_count; }
  SyntheticPayload(const SyntheticPayload& other) : _value(other._value) { ++live_count; }
  ~SyntheticPayload() { --live_count; }
  const char* GetPayloadTypeId() const { return "synthetic"; }
  ITypedPayload* Clone() const { return new SyntheticPayload(*this); }
  void WriteJsonProperty(std::ostream& output) const
  { output << "\"synthetic\":{\"value\":" << _value << '}'; }
  int Value() const { return _value; }
  static int live_count;

private:
  int _value;
};

int SyntheticPayload::live_count = 0;

// 专门抛异常的 Hole 伪视图，用来锁定所有虚调用都处于对象级错误边界内。
class ThrowingNativeHoleView : public INativeObjectView, public INativeHoleView
{
public:
  // 用途：mode=1 在取得视图时抛出，mode=2 在读取专用值时抛出。
  explicit ThrowingNativeHoleView(int mode) : _mode(mode)
  { _fingerprint.startup_type = "Hole"; }
  // 用途：返回 Hole 候选指纹。
  const TypeFingerprint& GetFingerprint() const { return _fingerprint; }
  // 用途：模拟能力查询工厂异常。
  const INativeCapabilityView* FindCapability(const char*) const
  { if (_mode == 1) throw "view exception"; return this; }
  // 用途：模拟专用 CAA 读取异常。
  NativeHoleReadStatus ReadNativeHole(NativeHoleData&, std::string&) const
  { throw "read exception"; }
  // 用途：让异常对象仍可完成 Generic 回退。
  bool ReadBasicAttributes(FeatureRecord&, std::string&) const { return true; }
private:
  int _mode;
  TypeFingerprint _fingerprint;
};

// 可配置 ID、优先级和抛异常行为的伪 Typed Decoder。
class TypedDecoder : public IFeatureDecoder
{
public:
  // 用途：创建测试 Decoder；id 使用字符串常量，测试期间不转移其内存所有权。
  TypedDecoder(const char* id, int priority, bool throws_on_decode = false)
    : _id(id), _priority(priority), _throws_on_decode(throws_on_decode) {}

  // 用途：返回测试指定的稳定 Decoder ID。
  const char* GetDecoderId() const { return _id; }
  // 用途：返回测试指定的匹配优先级。
  int GetPriority() const { return _priority; }

  // 用途：只匹配 native_type 为 Known 的伪对象。
  bool Match(const TypeFingerprint& fingerprint, const INativeObjectView&) const
  {
    return fingerprint.native_type == "Known";
  }

  // 用途：模拟成功 Typed Decode，或按开关抛出异常验证 Registry 的异常隔离。
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord& record)
  {
    if (_throws_on_decode)
      throw "decoder failure";
    record.decoder_id = _id;
    record.decode_level = "typed";
    record.decode_status = "success";
    return DecodeResult(true, "typed");
  }

private:
  const char* _id;
  int _priority;
  bool _throws_on_decode;
};

// 故意先污染 output 再返回失败的 Decoder，用于验证 Generic 前会恢复干净基础记录。
class DirtyFailingDecoder : public IFeatureDecoder
{
public:
  // 用途：返回污染测试 Decoder 的稳定 ID。
  const char* GetDecoderId() const { return "dirty"; }
  // 用途：提供高于 Generic 的优先级，确保该 Decoder 先被执行。
  int GetPriority() const { return 100; }
  // 用途：只匹配 Known 对象，使测试路径确定。
  bool Match(const TypeFingerprint& fingerprint, const INativeObjectView&) const
  { return fingerprint.native_type == "Known"; }
  // 用途：写入不应泄漏的半成品属性后主动返回失败结果。
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord& record)
  {
    record.decoder_id = "dirty";
    record.attributes["partial_typed_value"] = "must_not_leak";
    return DecodeResult(false, "failed", "intentional partial failure");
  }
};

// 可配置终态的测试解码器，用来验证注册中心的续试、冲突和统计规则。
class OutcomeDecoder : public IFeatureDecoder
{
public:
  // 用途：保存稳定编号、优先级、返回终态以及失败后是否允许继续尝试。
  OutcomeDecoder(const char* id, int priority, DecoderOutcome outcome,
                 bool continue_after_failure)
    : _id(id), _priority(priority), _outcome(outcome),
      _continue_after_failure(continue_after_failure) {}
  // 用途：返回测试指定的稳定解码器编号。
  const char* GetDecoderId() const { return _id; }
  // 用途：返回测试指定的解码器优先级。
  int GetPriority() const { return _priority; }
  // 用途：让测试对象固定进入候选集合，以便只验证执行状态机。
  bool Match(const TypeFingerprint& fingerprint, const INativeObjectView&) const
  { return fingerprint.native_type == "Known"; }
  // 用途：声明当前失败是否允许注册中心继续尝试下一个候选。
  bool ContinueTypedAfterFailure() const { return _continue_after_failure; }
  // 用途：按配置返回成功、部分、不支持或异常等终态，并仅在成功时写入正式结果。
  DecodeResult Decode(const INativeObjectView&, ParseContext&, FeatureRecord& record)
  {
    if (_outcome == DecoderOutcomeException) throw "configured exception";
    if (_outcome == DecoderOutcomeSuccess || _outcome == DecoderOutcomePartial)
    {
      record.decoder_id = _id;
      record.decode_level = "typed";
      record.decode_status = _outcome == DecoderOutcomeSuccess ? "success" : "partial";
      return DecodeResult(true, "typed", "", _outcome);
    }
    return DecodeResult(false, "failed", "configured outcome", _outcome);
  }
private:
  const char* _id;
  int _priority;
  DecoderOutcome _outcome;
  bool _continue_after_failure;
};

// 最小测试运行器：累计失败数并把失败用例名称写到标准错误流。
class TestRunner
{
public:
  // 用途：创建尚无失败的测试运行器。
  TestRunner() : _failures(0) {}

  // 用途：断言 condition；失败时递增计数并输出便于定位的用例名称。
  void Check(bool condition, const char* name)
  {
    if (!condition)
    {
      ++_failures;
      cerr << "FAILED: " << name << endl;
    }
  }

  // 用途：返回累计失败数，供进程入口转换为退出码。
  int Failures() const { return _failures; }

private:
  int _failures;
};

// 用途：以二进制方式读取完整文件，避免 Windows 文本换行转换影响 Golden 比较。
static std::string ReadWholeFile(const std::string& path)
{
  std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

// 用途：快速构造确定字段和值顺序的 FeatureRecord，供 JSONL Golden Output 测试使用。
static FeatureRecord MakeFeature(const char* id, const char* parent, long index,
                                 const char* type, const char* name, const char* decoder)
{
  FeatureRecord record;
  record.feature_id = id;
  record.parent_id = parent;
  record.traversal_index = index;
  record.fingerprint.native_type = type;
  record.fingerprint.display_name = name;
  record.fingerprint.container_kind = index == 1 ? "document" : "feature";
  record.tree_path = std::string("/") + name;
  record.update_status = "unknown";
  record.visibility = "unknown";
  record.decoder_id = decoder;
  record.decode_level = "typed";
  record.decode_status = "success";
  return record;
}

// 用途：构造声明式 GSMTool 候选，供参数归属与业务聚合测试复用。
static FeatureRecord MakeGsmTool(const char* id, const char* parent, long index,
                                 const std::string& name)
{
  FeatureRecord record = MakeFeature(id, parent, index, "", name.c_str(), "generic");
  record.fingerprint.startup_type = "GSMTool";
  return record;
}

// 用途：构造已经由 String Typed Decoder 成功读取的参数 Feature。
static FeatureRecord MakeStringParameter(const char* id, const char* parent, long index,
                                         const std::string& name, const std::string& value)
{
  FeatureRecord record = MakeFeature(id, parent, index, "", name.c_str(),
                                     "KnowledgewareStringParameterDecoder");
  record.fingerprint.startup_type = "String";
  record.has_parameter = true;
  record.parameter.parameter_kind = "string";
  record.parameter.parameter_name = name;
  record.parameter.value_status = "success";
  record.parameter.value_source = "typed_caa_value";
  record.parameter.value_text = value;
  return record;
}

// 用途：构造一个字段完整的盲孔载荷，供 NativeHoleDecoder 与 JSON Golden 测试复用。
static NativeHoleData MakeBlindHoleData()
{
  NativeHoleData data;
  data.semantic_kind = "part_design_hole";
  data.value_source = "typed_caa_value";
  data.interface_key = "CATIAHole";
  data.hole_type = "simple";
  data.hole_type_raw = 0;
  data.diameter_mm = 10.0;
  data.origin_mm[0] = -65.0;
  data.origin_mm[1] = -25.0;
  data.origin_mm[2] = 0.0;
  data.direction[0] = 0.0;
  data.direction[1] = 0.0;
  data.direction[2] = 1.0;
  data.bottom_limit.mode = "offset";
  data.bottom_limit.mode_raw = 0;
  data.bottom_limit.depth_mm.Set(12.0, "success");
  data.head.kind = "none";
  data.thread.enabled = false;
  data.thread.mode_raw = 1;
  data.thread.description.status = "not_applicable";
  data.thread.diameter_mm.status = "not_applicable";
  data.thread.depth_mm.status = "not_applicable";
  data.thread.pitch_mm.status = "not_applicable";
  return data;
}

// 用途：在不使用 RTTI 的情况下取得原生孔强类型载荷；类型不符时返回空指针。
static const NativeHoleData* GetNativeHoleData(const FeatureRecord& record)
{
  const ITypedPayload* payload = record.GetTypedPayload();
  if (!payload || std::string(payload->GetPayloadTypeId()) != "native_hole") return 0;
  return &static_cast<const NativeHolePayload*>(payload)->GetData();
}

// 用途：构造一个字段完整的 Pad/Pocket Prism 载荷，供原生 Prism Decoder 测试复用。
static NativePrismData MakeNativePrismData(const char* semantic_kind,
                                           const char* material_operation,
                                           const char* interface_key)
{
  NativePrismData data;
  data.semantic_kind = semantic_kind;
  data.material_operation = material_operation;
  data.value_source = "typed_caa_value";
  data.interface_key = interface_key;
  data.direction_type = "normal_to_sketch";
  data.direction_type_raw = 0;
  data.direction_orientation = "regular";
  data.direction_orientation_raw = 0;
  data.direction[0] = 0.0;
  data.direction[1] = 0.0;
  data.direction[2] = 1.0;
  data.is_symmetric = false;
  data.is_thin = false;
  data.neutral_fiber = false;
  data.merge_end = false;
  data.first_limit.mode = "offset";
  data.first_limit.mode_raw = 0;
  data.first_limit.dimension_mm.Set(25.0, "success");
  data.first_limit.limiting_element_status = "not_applicable";
  data.second_limit.mode = "offset";
  data.second_limit.mode_raw = 0;
  data.second_limit.dimension_mm.Set(0.0, "success");
  data.second_limit.limiting_element_status = "not_applicable";
  data.field_status["direction"] = "success";
  data.field_status["first_limit"] = "success";
  data.field_status["second_limit"] = "success";
  return data;
}

// 用途：在不使用 RTTI 的情况下取得原生 Prism 强类型载荷；类型不符时返回空指针。
static const NativePrismData* GetNativePrismData(const FeatureRecord& record)
{
  const ITypedPayload* payload = record.GetTypedPayload();
  if (!payload || std::string(payload->GetPayloadTypeId()) != "native_prism") return 0;
  return &static_cast<const NativePrismPayload*>(payload)->GetData();
}

// 用途：追加一条 parent_of 关系，模拟 Crawler 已确认的真实树边。
static void AddParentRelation(std::vector<RelationRecord>& relations,
                              const char* parent, const char* child)
{
  RelationRecord relation;
  relation.kind = "parent_of";
  relation.from_id = parent;
  relation.to_id = child;
  relations.push_back(relation);
}

// 用途：顺序执行全部 API 无关自测并返回失败数量；0 表示所有不变量均满足。
int SelfTestSuite::RunAll()
{
  TestRunner tests;

  // 验证 ID 在单次 revision 内唯一、固定宽度且从 F000001 开始。
  FeatureIdGenerator ids;
  std::set<std::string> generated_ids;
  int i = 0;
  for (i = 0; i < 1000; ++i)
    generated_ids.insert(ids.Next());
  tests.Check(generated_ids.size() == 1000 && *generated_ids.begin() == "F000001",
              "ID generation is unique and revision-local");

  // 验证 UTF-8 字节保持不变，同时正确转义引号、反斜杠和换行。
  const std::string raw = "\xE4\xB8\xAD\xE6\x96\x87\"\\\n";
  const std::string escaped = "\xE4\xB8\xAD\xE6\x96\x87\\\"\\\\\\n";
  tests.Check(JsonEscape(raw) == escaped,
              "JSON escaping preserves UTF-8 and escapes quote slash newline");

  // 验证新能力仅通过统一查询入口接入；中央对象视图不需要新增专用方法。
  SyntheticCapabilityObjectView synthetic_view;
  const INativeCapabilityView* synthetic_capability =
    synthetic_view.FindCapability("SyntheticCapability");
  tests.Check(synthetic_capability &&
              std::string(synthetic_capability->GetCapabilityId()) == "SyntheticCapability" &&
              synthetic_view.FindCapability("MissingCapability") == 0,
              "Capability lookup is extensible without a dedicated object-view getter");

  // 验证 FeatureRecord 深复制并独占释放通用载荷，避免异常和回退路径泄漏或共享悬空指针。
  {
    FeatureRecord payload_source;
    payload_source.SetTypedPayload(new SyntheticPayload(17));
    FeatureRecord payload_copy(payload_source);
    const ITypedPayload* copied_base = payload_copy.GetTypedPayload();
    const SyntheticPayload* copied = copied_base &&
      std::string(copied_base->GetPayloadTypeId()) == "synthetic"
      ? static_cast<const SyntheticPayload*>(copied_base) : 0;
    tests.Check(copied && copied->Value() == 17 && SyntheticPayload::live_count == 2,
                "Typed payload clone produces independent owned data");
  }
  tests.Check(SyntheticPayload::live_count == 0,
              "Typed payload ownership releases every clone");

  // 验证存在专用匹配时选择 Typed Decoder，而不是 Generic。
  ParseContext typed_context;
  FeatureTypeRegistry typed_registry;
  TypedDecoder typed_decoder("known", 20);
  typed_registry.Register(&typed_decoder);
  FeatureRecord typed;
  typed.feature_id = "F000001";
  FakeView known_view("Known");
  typed_registry.DecodeObject(known_view, typed_context, typed);
  tests.Check(typed.decoder_id == "known" && typed.decode_level == "typed",
              "Registry selects specialized decoder");

  // 验证未知对象没有专用匹配时仍产生 Generic 记录。
  ParseContext generic_context;
  FeatureTypeRegistry generic_registry;
  FeatureRecord generic;
  generic.feature_id = "F000002";
  FakeView unknown_view("Unknown");
  generic_registry.DecodeObject(unknown_view, generic_context, generic);
  tests.Check(generic.decoder_id == "generic" && generic.decode_level == "generic",
              "Unknown feature uses Generic decoder");

  // 验证连基础属性都不可读时进入 Opaque，且失败阶段被保留。
  ParseContext opaque_context;
  FeatureTypeRegistry opaque_registry;
  FeatureRecord opaque;
  opaque.feature_id = "F000003";
  FakeView unreadable_view("Unknown", false);
  opaque_registry.DecodeObject(unreadable_view, opaque_context, opaque);
  tests.Check(opaque.decoder_id == "opaque" && opaque.decode_level == "opaque" &&
              opaque.attributes["failure_stage"] == "generic",
              "Unreadable feature preserves an Opaque record");

  // 验证同优先级 Decoder 无论注册顺序如何都以稳定 ID 决胜，并产生冲突诊断。
  ParseContext tie_context_a;
  FeatureTypeRegistry tie_registry_a;
  TypedDecoder z_decoder("z", 10);
  TypedDecoder a_decoder("a", 10);
  tie_registry_a.Register(&z_decoder);
  tie_registry_a.Register(&a_decoder);
  FeatureRecord tie_a;
  tie_a.feature_id = "F000004";
  tie_registry_a.DecodeObject(known_view, tie_context_a, tie_a);

  ParseContext tie_context_b;
  FeatureTypeRegistry tie_registry_b;
  tie_registry_b.Register(&a_decoder);
  tie_registry_b.Register(&z_decoder);
  FeatureRecord tie_b;
  tie_b.feature_id = "F000004";
  tie_registry_b.DecodeObject(known_view, tie_context_b, tie_b);
  tests.Check(tie_a.decoder_id == "a" && tie_b.decoder_id == "a" &&
              !tie_context_a.diagnostics.empty() && !tie_context_b.diagnostics.empty(),
              "Equal priority conflict is deterministic and diagnosed");

  // 验证 Typed Decoder 抛异常不会越过对象边界，而会自动降级到 Generic。
  ParseContext exception_context;
  FeatureTypeRegistry exception_registry;
  TypedDecoder throwing_decoder("throwing", 50, true);
  exception_registry.Register(&throwing_decoder);
  FeatureRecord isolated;
  isolated.feature_id = "F000005";
  exception_registry.DecodeObject(known_view, exception_context, isolated);
  tests.Check(isolated.decode_level == "generic" && !exception_context.diagnostics.empty(),
              "Decoder exception is isolated and falls back to Generic");

  // 验证失败 Typed Decoder 写入的半成品不会污染随后成功的 Generic 结果。
  ParseContext dirty_context;
  FeatureTypeRegistry dirty_registry;
  DirtyFailingDecoder dirty_decoder;
  dirty_registry.Register(&dirty_decoder);
  FeatureRecord clean_fallback;
  clean_fallback.feature_id = "F000006";
  dirty_registry.DecodeObject(known_view, dirty_context, clean_fallback);
  tests.Check(clean_fallback.decode_level == "generic" &&
              clean_fallback.attributes.find("partial_typed_value") == clean_fallback.attributes.end(),
              "Failed typed decoder cannot leak partial state into Generic fallback");

  // 验证高优先级解码器返回 Unsupported 且允许续试时，下一个专用解码器可以正式接管。
  ParseContext continue_context;
  FeatureTypeRegistry continue_registry;
  OutcomeDecoder unsupported_decoder("unsupported_high", 200,
                                     DecoderOutcomeUnsupported, true);
  OutcomeDecoder succeeding_decoder("success_low", 100,
                                    DecoderOutcomeSuccess, false);
  continue_registry.Register(&unsupported_decoder);
  continue_registry.Register(&succeeding_decoder);
  FeatureRecord continued;
  continued.feature_id = "F000007";
  continue_registry.DecodeObject(known_view, continue_context, continued);
  tests.Check(continued.decoder_id == "success_low" && continued.decode_level == "typed" &&
              continue_context.statistics.decoder_outcome_counts["unsupported_high\x1funsupported"] == 1 &&
              continue_context.statistics.decoder_outcome_counts["success_low\x1fsuccess"] == 1,
              "Unsupported decoder continues to the next typed decoder");

  // 验证不允许续试的失败会立即进入 Generic，后续候选不能覆盖统一回退结果。
  ParseContext stop_context;
  FeatureTypeRegistry stop_registry;
  OutcomeDecoder rejected_decoder("rejected_high", 200,
                                  DecoderOutcomeRejected, false);
  stop_registry.Register(&rejected_decoder);
  stop_registry.Register(&succeeding_decoder);
  FeatureRecord stopped;
  stopped.feature_id = "F000008";
  stop_registry.DecodeObject(known_view, stop_context, stopped);
  tests.Check(stopped.decoder_id == "generic" && stopped.decode_level == "generic" &&
              stop_context.statistics.decoder_outcome_counts["success_low\x1fsuccess"] == 0,
              "Rejected decoder without continuation falls back immediately");

  // 验证同优先级多个解码器成功时只保留稳定编号较小者，并产生正式成功冲突诊断。
  ParseContext success_conflict_context;
  FeatureTypeRegistry success_conflict_registry;
  OutcomeDecoder success_z("success_z", 300, DecoderOutcomeSuccess, false);
  OutcomeDecoder success_a("success_a", 300, DecoderOutcomeSuccess, false);
  success_conflict_registry.Register(&success_z);
  success_conflict_registry.Register(&success_a);
  FeatureRecord conflicted;
  conflicted.feature_id = "F000009";
  success_conflict_registry.DecodeObject(known_view, success_conflict_context, conflicted);
  bool has_success_conflict = false;
  for (size_t conflict_index = 0;
       conflict_index < success_conflict_context.diagnostics.size(); ++conflict_index)
    if (success_conflict_context.diagnostics[conflict_index].code == "DECODER_SUCCESS_CONFLICT")
      has_success_conflict = true;
  tests.Check(conflicted.decoder_id == "success_a" && has_success_conflict,
              "Equal-priority double success is deterministic and diagnosed");

  // 验证四种最终分类的算术守恒，少计任何一类都必须被发现。
  ParseStatistics valid_coverage;
  valid_coverage.enumerated_total = 4;
  valid_coverage.typed_count = 1;
  valid_coverage.generic_count = 1;
  valid_coverage.opaque_count = 1;
  valid_coverage.failed_count = 1;
  ParseStatistics invalid_coverage = valid_coverage;
  invalid_coverage.failed_count = 0;
  tests.Check(valid_coverage.IsConserved() && !invalid_coverage.IsConserved(),
              "Coverage conservation detects mismatch");

  // 验证未知类型集合去重，以及 CoverageTracker 可复用同一守恒规则。
  UnknownTypeCollector unknown_types;
  TypeFingerprint unknown_a;
  unknown_a.startup_type = "Pad";
  TypeFingerprint unknown_b = unknown_a;
  unknown_types.Observe(unknown_a);
  unknown_types.Observe(unknown_b);
  tests.Check(unknown_types.Count() == 1 && CoverageTracker::Validate(valid_coverage),
              "Unknown type collection is distinct and coverage validation is reusable");

  // 验证 String 参数只有在类型化接口和值读取都成功后才计入 Typed。
  ParseContext string_context;
  FeatureTypeRegistry string_registry;
  KnowledgewareStringParameterDecoder string_decoder;
  string_registry.Register(&string_decoder);
  FeatureRecord string_feature;
  string_feature.feature_id = "F100001";
  FakeStringParameterView string_view(StringParameterReadSuccess, "20 mm",
                                      "\xE5\xAD\x94\xE7\x9B\xB4\xE5\xBE\x84");
  string_registry.DecodeObject(string_view, string_context, string_feature);
  tests.Check(string_feature.decode_level == "typed" && string_feature.has_parameter &&
              string_feature.parameter.value_text == "20 mm" &&
              string_feature.parameter.value_source == "typed_caa_value",
              "String parameter decoder records a typed CAA value");

  // 验证接口不支持是正常能力结果：进入 Generic，但不增加 probe_exception_count。
  ParseContext unsupported_context;
  FeatureTypeRegistry unsupported_registry;
  unsupported_registry.Register(&string_decoder);
  FeatureRecord unsupported_feature;
  unsupported_feature.feature_id = "F100002";
  FakeStringParameterView unsupported_view(StringParameterInterfaceUnsupported, "", "P");
  unsupported_registry.DecodeObject(unsupported_view, unsupported_context, unsupported_feature);
  tests.Check(unsupported_feature.decode_level == "generic" && unsupported_feature.has_parameter &&
              unsupported_feature.parameter.value_source == "unavailable" &&
              unsupported_context.statistics.probe_exception_count == 0,
              "Unsupported String interface falls back without becoming an exception");

  // 验证参数读取异常被隔离到当前对象，并留下明确错误码。
  ParseContext parameter_exception_context;
  FeatureTypeRegistry parameter_exception_registry;
  parameter_exception_registry.Register(&string_decoder);
  FeatureRecord parameter_exception_feature;
  parameter_exception_feature.feature_id = "F100003";
  FakeStringParameterView parameter_exception_view(StringParameterValueException, "", "P");
  parameter_exception_registry.DecodeObject(parameter_exception_view,
                                             parameter_exception_context,
                                             parameter_exception_feature);
  tests.Check(parameter_exception_feature.decode_level == "generic" &&
              !parameter_exception_context.diagnostics.empty() &&
              parameter_exception_context.diagnostics[0].code == "PARAM_VALUE_READ_EXCEPTION",
              "String parameter read exceptions are isolated and diagnosed");

  // 验证空字符串是合法的成功值，不能仅凭 empty 自动改成 unavailable。
  ParseContext empty_context;
  FeatureTypeRegistry empty_registry;
  empty_registry.Register(&string_decoder);
  FeatureRecord empty_feature;
  empty_feature.feature_id = "F100004";
  FakeStringParameterView empty_view(StringParameterReadSuccess, "", "Empty");
  empty_registry.DecodeObject(empty_view, empty_context, empty_feature);
  tests.Check(empty_feature.decode_level == "typed" &&
              empty_feature.parameter.value_status == "success" &&
              empty_feature.parameter.value_text.empty(),
              "Empty String parameter values remain successful typed values");

  // 验证 StartUp 只负责候选预筛选：必须由 Hole 专用视图确认后才能成为 Typed。
  NativeHoleDecoder native_hole_decoder;
  NativeHoleData blind_data = MakeBlindHoleData();
  ParseContext hole_context;
  FeatureTypeRegistry hole_registry;
  hole_registry.Register(&native_hole_decoder);
  FeatureRecord renamed_hole;
  renamed_hole.feature_id = "F400001";
  FakeNativeHoleView renamed_hole_view("Hole", "CoolingPort_A",
                                       NativeHoleReadSuccess, blind_data);
  hole_registry.DecodeObject(renamed_hole_view, hole_context, renamed_hole);
  const NativeHoleData* renamed_hole_data = GetNativeHoleData(renamed_hole);
  tests.Check(renamed_hole.decode_level == "typed" && renamed_hole_data &&
              renamed_hole.decoder_id == "NativeHoleDecoder" &&
              renamed_hole_data->diameter_mm == 10.0 &&
              hole_context.statistics.native_hole_candidate_count == 1 &&
              hole_context.statistics.native_hole_success_count == 1,
              "Native Hole is typed only after its dedicated interface succeeds");

  // 验证仅有 Hole StartUp 但接口不支持时回退 Generic，unsupported 不算异常。
  ParseContext hole_unsupported_context;
  FeatureTypeRegistry hole_unsupported_registry;
  hole_unsupported_registry.Register(&native_hole_decoder);
  FeatureRecord unsupported_hole;
  unsupported_hole.feature_id = "F400002";
  FakeNativeHoleView unsupported_hole_view("Hole", "HoleCandidate",
                                           NativeHoleInterfaceUnsupported, blind_data);
  hole_unsupported_registry.DecodeObject(unsupported_hole_view,
                                         hole_unsupported_context, unsupported_hole);
  tests.Check(unsupported_hole.decode_level == "generic" && !GetNativeHoleData(unsupported_hole) &&
              hole_unsupported_context.statistics.native_hole_unsupported_count == 1 &&
              hole_unsupported_context.statistics.native_hole_exception_count == 0,
              "Unsupported Hole interface falls back without becoming an exception");

  // 验证 Pocket 即使拥有相同伪视图也不会成为 Hole 候选，更不会产生 native_hole 载荷。
  ParseContext pocket_context;
  FeatureTypeRegistry pocket_registry;
  pocket_registry.Register(&native_hole_decoder);
  FeatureRecord pocket;
  pocket.feature_id = "F400003";
  FakeNativeHoleView pocket_view("Pocket", "Pocket_Control",
                                 NativeHoleInterfaceUnsupported, blind_data);
  pocket_registry.DecodeObject(pocket_view, pocket_context, pocket);
  tests.Check(pocket.decode_level == "generic" && !GetNativeHoleData(pocket) &&
              pocket_context.statistics.native_hole_candidate_count == 0,
              "Pocket is not misclassified as a Native Hole");

  // 验证 Pad 必须通过 NativePad 能力确认后才会从 Generic 提升为 Typed。
  NativePadDecoder native_pad_decoder;
  NativePrismData pad_data = MakeNativePrismData("part_design_pad", "add_material",
                                                 "CATIAPad");
  ParseContext pad_context;
  FeatureTypeRegistry pad_registry;
  pad_registry.Register(&native_pad_decoder);
  FeatureRecord pad_feature;
  pad_feature.feature_id = "F410001";
  FakeNativePrismView pad_view("Pad", "Pad_Base", "NativePad",
                               NativePrismReadSuccess, pad_data);
  pad_registry.DecodeObject(pad_view, pad_context, pad_feature);
  const NativePrismData* decoded_pad = GetNativePrismData(pad_feature);
  tests.Check(pad_feature.decode_level == "typed" && decoded_pad &&
              pad_feature.decoder_id == "NativePadDecoder" &&
              decoded_pad->interface_key == "CATIAPad" &&
              decoded_pad->material_operation == "add_material",
              "Native Pad is typed only after CATIAPad capability succeeds");

  // 验证 Pocket 必须通过 NativePocket 能力确认，并保留去材料语义。
  NativePocketDecoder native_pocket_decoder;
  NativePrismData pocket_data = MakeNativePrismData("part_design_pocket",
                                                    "remove_material", "CATIAPocket");
  ParseContext native_pocket_context;
  FeatureTypeRegistry native_pocket_registry;
  native_pocket_registry.Register(&native_pocket_decoder);
  FeatureRecord native_pocket_feature;
  native_pocket_feature.feature_id = "F410002";
  FakeNativePrismView native_pocket_view("Pocket", "Pocket_Control", "NativePocket",
                                         NativePrismReadSuccess, pocket_data);
  native_pocket_registry.DecodeObject(native_pocket_view, native_pocket_context,
                                      native_pocket_feature);
  const NativePrismData* decoded_pocket = GetNativePrismData(native_pocket_feature);
  tests.Check(native_pocket_feature.decode_level == "typed" && decoded_pocket &&
              native_pocket_feature.decoder_id == "NativePocketDecoder" &&
              decoded_pocket->interface_key == "CATIAPocket" &&
              decoded_pocket->material_operation == "remove_material",
              "Native Pocket is typed only after CATIAPocket capability succeeds");

  // 验证只有 StartUp=Pad 但不支持 NativePad 能力时必须回退 Generic。
  ParseContext pad_unsupported_context;
  FeatureTypeRegistry pad_unsupported_registry;
  pad_unsupported_registry.Register(&native_pad_decoder);
  FeatureRecord unsupported_pad;
  unsupported_pad.feature_id = "F410003";
  FakeNativePrismView unsupported_pad_view("Pad", "PadCandidate", "NativePocket",
                                           NativePrismInterfaceUnsupported, pad_data);
  pad_unsupported_registry.DecodeObject(unsupported_pad_view, pad_unsupported_context,
                                        unsupported_pad);
  tests.Check(unsupported_pad.decode_level == "generic" &&
              !GetNativePrismData(unsupported_pad) &&
              pad_unsupported_context.statistics.probe_exception_count == 0,
              "Unsupported Pad capability falls back without exception");

  // 验证 Pocket 不会被 Pad Decoder 抢占，避免 Part Design 反例被误判。
  ParseContext pad_vs_pocket_context;
  FeatureTypeRegistry pad_vs_pocket_registry;
  pad_vs_pocket_registry.Register(&native_pad_decoder);
  FeatureRecord pad_vs_pocket;
  pad_vs_pocket.feature_id = "F410004";
  FakeNativePrismView pad_vs_pocket_view("Pocket", "Pocket_Control", "NativePocket",
                                         NativePrismReadSuccess, pocket_data);
  pad_vs_pocket_registry.DecodeObject(pad_vs_pocket_view, pad_vs_pocket_context,
                                      pad_vs_pocket);
  tests.Check(pad_vs_pocket.decode_level == "generic" &&
              !GetNativePrismData(pad_vs_pocket),
              "NativePadDecoder does not claim Pocket startup candidates");

  // 验证 QueryInterface 异常与必需字段异常分别进入 exception/partial，并保持对象级隔离。
  ParseContext hole_error_context;
  FeatureTypeRegistry hole_error_registry;
  hole_error_registry.Register(&native_hole_decoder);
  FeatureRecord query_error_hole;
  query_error_hole.feature_id = "F400004";
  FakeNativeHoleView query_error_view("Hole", "Q", NativeHoleInterfaceQueryException,
                                      blind_data);
  hole_error_registry.DecodeObject(query_error_view, hole_error_context, query_error_hole);
  FeatureRecord required_error_hole;
  required_error_hole.feature_id = "F400005";
  FakeNativeHoleView required_error_view("Hole", "R", NativeHoleRequiredValueReadException,
                                         blind_data);
  hole_error_registry.DecodeObject(required_error_view, hole_error_context, required_error_hole);
  tests.Check(query_error_hole.decode_level == "generic" &&
              required_error_hole.decode_level == "generic" &&
              hole_error_context.statistics.native_hole_exception_count == 1 &&
              hole_error_context.statistics.native_hole_partial_count == 1 &&
              hole_error_context.statistics.IsNativeHoleConserved(),
              "Native Hole query and required-value failures are isolated and conserved");

  // 验证 Native View 的两处虚调用即使直接抛异常，也各自形成 exception 终态并继续 Generic。
  ParseContext throwing_hole_context;
  FeatureTypeRegistry throwing_hole_registry;
  throwing_hole_registry.Register(&native_hole_decoder);
  FeatureRecord throwing_get_hole;
  throwing_get_hole.feature_id = "F400011";
  ThrowingNativeHoleView throwing_get_view(1);
  throwing_hole_registry.DecodeObject(throwing_get_view, throwing_hole_context,
                                      throwing_get_hole);
  FeatureRecord throwing_read_hole;
  throwing_read_hole.feature_id = "F400012";
  ThrowingNativeHoleView throwing_read_view(2);
  throwing_hole_registry.DecodeObject(throwing_read_view, throwing_hole_context,
                                      throwing_read_hole);
  tests.Check(throwing_get_hole.decode_level == "generic" &&
              throwing_read_hole.decode_level == "generic" &&
              throwing_hole_context.statistics.native_hole_candidate_count == 2 &&
              throwing_hole_context.statistics.native_hole_exception_count == 2 &&
              throwing_hole_context.statistics.IsNativeHoleConserved(),
              "Thrown Native Hole view calls preserve terminal statistics and fallback");

  // 验证 Up To Last 深度明确为不适用，且可选字段不适用不降低成功结果。
  NativeHoleData through_data = blind_data;
  through_data.bottom_limit.mode = "up_to_last";
  through_data.bottom_limit.mode_raw = 2;
  through_data.bottom_limit.depth_mm.Clear("not_applicable");
  ParseContext through_context;
  FeatureTypeRegistry through_registry;
  through_registry.Register(&native_hole_decoder);
  FeatureRecord through_hole;
  through_hole.feature_id = "F400006";
  FakeNativeHoleView through_view("Hole", "Through", NativeHoleReadSuccess, through_data);
  through_registry.DecodeObject(through_view, through_context, through_hole);
  const NativeHoleData* through_hole_data = GetNativeHoleData(through_hole);
  tests.Check(through_hole_data &&
              !through_hole_data->bottom_limit.depth_mm.has_value &&
              through_hole_data->bottom_limit.depth_mm.status == "not_applicable" &&
              through_context.statistics.native_hole_partial_count == 0,
              "Up To Last depth is null and not_applicable without becoming partial");

  // 验证未知枚举保留 raw 值并产生诊断，但不会用猜测名称替换接口结果。
  NativeHoleData unknown_enum_data = blind_data;
  unknown_enum_data.hole_type = "unknown";
  unknown_enum_data.hole_type_raw = 99;
  unknown_enum_data.field_status["hole_type"] = "unknown_enum";
  ParseContext unknown_enum_context;
  FeatureTypeRegistry unknown_enum_registry;
  unknown_enum_registry.Register(&native_hole_decoder);
  FeatureRecord unknown_enum_hole;
  unknown_enum_hole.feature_id = "F400007";
  FakeNativeHoleView unknown_enum_view("Hole", "UnknownEnum", NativeHoleReadSuccess,
                                       unknown_enum_data);
  unknown_enum_registry.DecodeObject(unknown_enum_view, unknown_enum_context, unknown_enum_hole);
  tests.Check(unknown_enum_hole.decode_level == "typed" &&
              GetNativeHoleData(unknown_enum_hole) &&
              GetNativeHoleData(unknown_enum_hole)->hole_type_raw == 99 &&
              !unknown_enum_context.diagnostics.empty() &&
              unknown_enum_context.diagnostics[0].code == "NATIVE_HOLE_ENUM_UNKNOWN",
              "Unknown Native Hole enum preserves raw value and emits a diagnostic");

  // 验证零向量不能进入 Typed Payload，且失败数据不会污染 Generic 结果。
  NativeHoleData invalid_direction_data = blind_data;
  invalid_direction_data.direction[0] = 0.0;
  invalid_direction_data.direction[1] = 0.0;
  invalid_direction_data.direction[2] = 0.0;
  ParseContext invalid_direction_context;
  FeatureTypeRegistry invalid_direction_registry;
  invalid_direction_registry.Register(&native_hole_decoder);
  FeatureRecord invalid_direction_hole;
  invalid_direction_hole.feature_id = "F400008";
  FakeNativeHoleView invalid_direction_view("Hole", "BadDirection", NativeHoleReadSuccess,
                                            invalid_direction_data);
  invalid_direction_registry.DecodeObject(invalid_direction_view, invalid_direction_context,
                                          invalid_direction_hole);
  tests.Check(invalid_direction_hole.decode_level == "generic" &&
              !GetNativeHoleData(invalid_direction_hole) &&
              invalid_direction_context.statistics.native_hole_partial_count == 1 &&
              invalid_direction_context.statistics.IsNativeHoleConserved(),
              "Invalid Hole direction falls back without leaking a Typed payload");

  // 验证可选深度出现 NaN 时不会生成非法 JSON，而是按必需语义降级到 Generic。
  NativeHoleData invalid_optional_data = blind_data;
  const double nonfinite_depth = std::numeric_limits<double>::quiet_NaN();
  invalid_optional_data.bottom_limit.depth_mm.Set(nonfinite_depth, "success");
  ParseContext invalid_optional_context;
  FeatureTypeRegistry invalid_optional_registry;
  invalid_optional_registry.Register(&native_hole_decoder);
  FeatureRecord invalid_optional_hole;
  invalid_optional_hole.feature_id = "F400009";
  FakeNativeHoleView invalid_optional_view("Hole", "BadDepth", NativeHoleReadSuccess,
                                           invalid_optional_data);
  invalid_optional_registry.DecodeObject(invalid_optional_view, invalid_optional_context,
                                         invalid_optional_hole);
  tests.Check(invalid_optional_hole.decode_level == "generic" &&
              !GetNativeHoleData(invalid_optional_hole) &&
              invalid_optional_context.statistics.native_hole_partial_count == 1,
              "Non-finite optional Hole value cannot reach JSON output");

  // 验证沉孔头部和螺纹孔字段保留真实数值及接口返回的描述，不由 Decoder 拼接。
  NativeHoleData counterbore_data = blind_data;
  counterbore_data.hole_type = "counterbored";
  counterbore_data.hole_type_raw = 2;
  counterbore_data.head.kind = "counterbore";
  counterbore_data.head.diameter_mm.Set(18.0, "success");
  counterbore_data.head.depth_mm.Set(5.0, "success");
  NativeHoleData threaded_data = blind_data;
  threaded_data.diameter_mm = 8.376;
  threaded_data.thread.enabled = true;
  threaded_data.thread.mode_raw = 0;
  threaded_data.thread.description.Set("M10x1.5", "success");
  threaded_data.thread.diameter_mm.Set(10.0, "success");
  threaded_data.thread.depth_mm.Set(10.0, "success");
  threaded_data.thread.pitch_mm.Set(1.5, "success");
  tests.Check(counterbore_data.head.diameter_mm.has_value &&
              counterbore_data.head.depth_mm.value == 5.0 &&
              threaded_data.thread.enabled &&
              threaded_data.thread.description.value == "M10x1.5" &&
              threaded_data.thread.pitch_mm.value == 1.5,
              "Counterbore and threaded Hole typed payloads preserve dedicated values");

  // 验证完整数值+单位字符串可规范化，同时原始值保持不变。
  ParameterValueData numeric_parameter;
  numeric_parameter.value_text = " -1.25e2 mm ";
  ParameterValueNormalizer::Normalize(numeric_parameter);
  tests.Check(numeric_parameter.has_normalized_numeric_value &&
              numeric_parameter.normalized_numeric_value == -125.0 &&
              numeric_parameter.normalized_unit == "mm" &&
              numeric_parameter.value_text == " -1.25e2 mm ",
              "Numeric String values with explicit units are normalized without data loss");

  // 验证复杂复合字符串只保留原文，不伪造数值或几何结构。
  ParameterValueData complex_parameter;
  complex_parameter.value_text = "bbox=(0,0,0);(10,20,30)";
  ParameterValueNormalizer::Normalize(complex_parameter);
  tests.Check(!complex_parameter.has_normalized_numeric_value &&
              complex_parameter.value_text == "bbox=(0,0,0);(10,20,30)",
              "Complex String parameter values are preserved verbatim");

  // 验证探测统计把 supported、unsupported、exception 分开，并保留汇总维度。
  ParseStatistics probe_statistics;
  probe_statistics.RecordProbe("CATICkeParm", "String", "unselected", "supported");
  probe_statistics.RecordProbe("CATIPrtPart", "String", "unselected", "unsupported");
  probe_statistics.RecordProbe("CATIContainer", "String", "unselected", "exception");
  tests.Check(probe_statistics.probe_supported_count == 1 &&
              probe_statistics.probe_unsupported_count == 1 &&
              probe_statistics.probe_exception_count == 1 &&
              probe_statistics.probe_outcome_counts.size() == 3,
              "Interface probe outcomes are counted without treating unsupported as failure");

  // 验证参数 Owner 从 parent_of 真实关系向上找到最近业务 GSMTool，而不是解析字符串路径。
  std::vector<FeatureRecord> ownership_features;
  ownership_features.push_back(MakeGsmTool("F200001", "", 1, "\xE5\xAD\x94.1"));
  ownership_features.push_back(MakeGsmTool("F200002", "F200001", 2,
                                          "\xE7\x89\xB9\xE5\xBE\x81\xE5\xB1\x9E\xE6\x80\xA7"));
  ownership_features.push_back(MakeStringParameter("F200003", "F200002", 3,
                                                  "\xE5\xAD\x94\xE7\x9B\xB4\xE5\xBE\x84", "20 mm"));
  std::vector<RelationRecord> ownership_relations;
  AddParentRelation(ownership_relations, "F200001", "F200002");
  AddParentRelation(ownership_relations, "F200002", "F200003");
  std::vector<ParameterRecord> ownership_parameters;
  ParseContext ownership_context;
  ParameterRecordBuilder::Build(ownership_features, ownership_relations,
                                ownership_context, ownership_parameters);
  tests.Check(ownership_parameters.size() == 1 &&
              ownership_parameters[0].owner_feature_id == "F200001",
              "Parameter ownership resolves through real parent relations");

  // 验证没有任何业务祖先的参数保留为孤立参数并计入 Coverage。
  std::vector<FeatureRecord> orphan_features;
  orphan_features.push_back(MakeStringParameter("F210001", "", 1, "P", "v"));
  std::vector<RelationRecord> no_relations;
  std::vector<ParameterRecord> orphan_parameters;
  ParseContext orphan_context;
  ParameterRecordBuilder::Build(orphan_features, no_relations, orphan_context, orphan_parameters);
  tests.Check(orphan_parameters.size() == 1 && orphan_parameters[0].owner_feature_id.empty() &&
              orphan_context.statistics.orphan_parameter_count == 1,
              "Orphan parameters remain indexed with an empty owner");

  // 验证同一参数存在两个业务祖先时标记歧义，不能静默挑选一个 Owner。
  std::vector<FeatureRecord> ambiguous_features;
  ambiguous_features.push_back(MakeGsmTool("F220001", "", 1, "\xE5\xAD\x94.1"));
  ambiguous_features.push_back(MakeGsmTool("F220002", "", 2, "\xE6\xA7\xBD.1"));
  ambiguous_features.push_back(MakeStringParameter("F220003", "", 3, "P", "v"));
  std::vector<RelationRecord> ambiguous_relations;
  AddParentRelation(ambiguous_relations, "F220001", "F220003");
  AddParentRelation(ambiguous_relations, "F220002", "F220003");
  std::vector<ParameterRecord> ambiguous_parameters;
  ParseContext ambiguous_context;
  ParameterRecordBuilder::Build(ambiguous_features, ambiguous_relations,
                                ambiguous_context, ambiguous_parameters);
  tests.Check(ambiguous_parameters[0].owner_feature_id.empty() &&
              ambiguous_parameters[0].ownership_status == "ambiguous" &&
              ambiguous_context.statistics.ambiguous_parameter_owner_count == 1,
              "Ambiguous parameter owners are reported instead of guessed");

  // 验证 CATIA 实例后缀只在完整的 .数字 结尾时移除，.10 不会被当成 .1 的前缀。
  tests.Check(BusinessFeatureRuleCatalog::NormalizeInstanceName("\xE5\xAD\x94.1") == "\xE5\xAD\x94" &&
              BusinessFeatureRuleCatalog::NormalizeInstanceName("\xE5\xAD\x94.10") == "\xE5\xAD\x94" &&
              BusinessFeatureRuleCatalog::NormalizeInstanceName("A.1x") == "A.1x",
              "Business feature instance suffix normalization is exact");

  // 验证聚合保持原生遍历顺序，并形成 boss/hole/slot 三类声明式记录。
  std::vector<FeatureRecord> aggregate_features;
  aggregate_features.push_back(MakeGsmTool("F300001", "", 1, "\xE5\x87\xB8\xE5\x8F\xB0.1"));
  aggregate_features.push_back(MakeStringParameter("F300002", "F300001", 2,
                                                  "\xE7\x89\xB9\xE5\xBE\x81\xE7\xB1\xBB\xE5\x9E\x8B",
                                                  "\xE5\x87\xB8\xE5\x8F\xB0"));
  aggregate_features.push_back(MakeGsmTool("F300003", "", 3, "\xE5\xAD\x94.2"));
  aggregate_features.push_back(MakeStringParameter("F300004", "F300003", 4,
                                                  "\xE7\x89\xB9\xE5\xBE\x81\xE7\xB1\xBB\xE5\x9E\x8B",
                                                  "\xE5\xAD\x94"));
  aggregate_features.push_back(MakeGsmTool("F300005", "", 5, "\xE6\xA7\xBD.10"));
  aggregate_features.push_back(MakeStringParameter("F300006", "F300005", 6,
                                                  "\xE7\x89\xB9\xE5\xBE\x81\xE7\xB1\xBB\xE5\x9E\x8B",
                                                  "\xE6\xA7\xBD"));
  std::vector<RelationRecord> aggregate_relations;
  AddParentRelation(aggregate_relations, "F300001", "F300002");
  AddParentRelation(aggregate_relations, "F300003", "F300004");
  AddParentRelation(aggregate_relations, "F300005", "F300006");
  std::vector<ParameterRecord> aggregate_parameters;
  ParseContext aggregate_context;
  ParameterRecordBuilder::Build(aggregate_features, aggregate_relations,
                                aggregate_context, aggregate_parameters);
  std::vector<BusinessFeatureRecord> business_features;
  DeclaredBusinessFeatureAggregator::Aggregate(aggregate_features, aggregate_relations,
                                               aggregate_parameters, aggregate_context,
                                               business_features);
  tests.Check(business_features.size() == 3 &&
              business_features[0].feature_kind == "declared_boss" &&
              business_features[1].feature_kind == "declared_hole" &&
              business_features[2].feature_kind == "declared_slot" &&
              business_features[2].source_feature_id == "F300005",
              "Declared business aggregation classifies records in traversal order");

  // 验证名称和“特征类型”参数冲突时输出 declared_unknown/ambiguous，而不是强行分类。
  aggregate_features[3].parameter.value_text = "\xE6\xA7\xBD";
  aggregate_parameters.clear();
  business_features.clear();
  ParseContext conflicting_context;
  ParameterRecordBuilder::Build(aggregate_features, aggregate_relations,
                                conflicting_context, aggregate_parameters);
  DeclaredBusinessFeatureAggregator::Aggregate(aggregate_features, aggregate_relations,
                                               aggregate_parameters, conflicting_context,
                                               business_features);
  tests.Check(business_features[1].feature_kind == "declared_unknown" &&
              business_features[1].classification_status == "ambiguous",
              "Conflicting declared feature evidence becomes ambiguous");

  // 验证参数和业务特征两套新增守恒关系。
  ParseStatistics derived_statistics;
  derived_statistics.parameter_total = 4;
  derived_statistics.parameter_value_success = 1;
  derived_statistics.parameter_value_partial = 1;
  derived_statistics.parameter_value_unavailable = 1;
  derived_statistics.parameter_failed = 1;
  derived_statistics.declared_business_feature_total = 4;
  derived_statistics.declared_boss_count = 1;
  derived_statistics.declared_hole_count = 1;
  derived_statistics.declared_slot_count = 1;
  derived_statistics.declared_unknown_count = 1;
  tests.Check(derived_statistics.IsParameterConserved() &&
              derived_statistics.IsBusinessFeatureConserved(),
              "Parameter and declared business feature statistics are conserved");

  // 验证 SHA-256 标准已知向量，避免 Manifest 写入未经验证的摘要。
  tests.Check(Sha256String("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256 matches the standard abc vector");

  // 验证默认路径策略只保留文件名，不把本机绝对目录写入交付数据。
  tests.Check(SourcePathForOutput("D:\\secret\\models\\part.CATPart", false) ==
              "part.CATPart" &&
              SourcePathForOutput("D:\\secret\\models\\part.CATPart", true) ==
              "D:\\secret\\models\\part.CATPart",
              "Source path output is redacted unless explicitly enabled");

  // 构造两节点伪对象树和一条关系，连续写入两个目录以比较确定性输出。
  std::vector<FeatureRecord> features;
  features.push_back(MakeFeature("F000001", "", 1, "Document", "demo", "document"));
  features.push_back(MakeFeature("F000002", "F000001", 2, "Part", "Part1", "part"));
  std::vector<RelationRecord> relations;
  RelationRecord relation;
  relation.kind = "parent_of";
  relation.from_id = "F000001";
  relation.to_id = "F000002";
  relations.push_back(relation);
  ParseContext output_context;
  output_context.statistics.enumerated_total = 2;
  output_context.statistics.typed_count = 2;

  JsonArtifactWriter writer(false);
  std::string write_error;
  const std::string output_a = "selftest_output_a";
  const std::string output_b = "selftest_output_b";
  const bool wrote_a = writer.Write(features, relations, output_context, output_a, write_error);
  const bool wrote_b = writer.Write(features, relations, output_context, output_b, write_error);
  tests.Check(wrote_a && wrote_b,
              "Artifact writer creates the complete output set");
  tests.Check(ReadWholeFile(output_a + "\\features.jsonl") ==
              ReadWholeFile(output_b + "\\features.jsonl") &&
              ReadWholeFile(output_a + "\\relations.jsonl") ==
              ReadWholeFile(output_b + "\\relations.jsonl") &&
              ReadWholeFile(output_a + "\\parameters.jsonl") ==
              ReadWholeFile(output_b + "\\parameters.jsonl") &&
              ReadWholeFile(output_a + "\\business_features.jsonl") ==
              ReadWholeFile(output_b + "\\business_features.jsonl"),
              "Output order is deterministic across consecutive runs");

  // Golden 文本锁定字段集合、字段顺序、对象顺序以及每行一个 JSON 对象的格式。
  const std::string expected_features =
    "{\"feature_id\":\"F000001\",\"parent_id\":\"\",\"native_enumeration_index\":0,"
    "\"container_enumeration_index\":0,\"traversal_index\":1,"
    "\"native_type\":\"Document\",\"startup_type\":\"\",\"super_types\":[],"
    "\"display_name\":\"demo\",\"internal_name\":\"\",\"container_kind\":\"document\","
    "\"tree_path\":\"/demo\",\"supported_interface_keys\":[],\"update_status\":\"unknown\","
    "\"visibility\":\"unknown\",\"decoder_id\":\"document\",\"decoder_version\":\"\",\"decode_level\":\"typed\","
    "\"decode_status\":\"success\",\"attributes\":{},\"diagnostic_ids\":[]}\n"
    "{\"feature_id\":\"F000002\",\"parent_id\":\"F000001\",\"native_enumeration_index\":0,"
    "\"container_enumeration_index\":0,\"traversal_index\":2,"
    "\"native_type\":\"Part\",\"startup_type\":\"\",\"super_types\":[],"
    "\"display_name\":\"Part1\",\"internal_name\":\"\",\"container_kind\":\"feature\","
    "\"tree_path\":\"/Part1\",\"supported_interface_keys\":[],\"update_status\":\"unknown\","
    "\"visibility\":\"unknown\",\"decoder_id\":\"part\",\"decoder_version\":\"\",\"decode_level\":\"typed\","
    "\"decode_status\":\"success\",\"attributes\":{},\"diagnostic_ids\":[]}\n";
  tests.Check(ReadWholeFile(output_a + "\\features.jsonl") == expected_features,
              "Fake object tree matches golden JSONL");
  tests.Check(ReadWholeFile(output_a + "\\parser.log").find(
                "decoder_match feature_id=F000001 decoder=document") != std::string::npos,
              "Parser log records deterministic decoder matches");
  const std::string native_tree_json = ReadWholeFile(output_a + "\\native_tree_nodes.jsonl");
  tests.Check(native_tree_json.find("\"node_id\":\"feature:F000001\"") != std::string::npos &&
              native_tree_json.find("\"parent_id\":\"feature:F000001\"") != std::string::npos &&
              native_tree_json.find("\"feature_id\":\"F000002\"") != std::string::npos,
              "Native tree writer preserves parent_id and feature identity");
  tests.Check(native_tree_json.find("\"source_index\":1") < native_tree_json.find("\"source_index\":2"),
              "Native tree source_index preserves traversal order");
  tests.Check(ReadWholeFile(output_a + "\\node_properties.jsonl").find(
                "\"node_id\":\"feature:F000001\"") != std::string::npos,
              "Node properties JSONL writer emits read-only basic properties");
  tests.Check(ReadWholeFile(output_a + "\\manifest.json").find(
                "\"native_tree_node_count\":2") != std::string::npos,
              "Manifest reports native tree node count");

  // 验证中央 Writer 只调用通用载荷协议，新增合成载荷时不需要增加类型分支。
  FeatureRecord synthetic_json_feature = MakeFeature(
    "F300001", "", 1, "Synthetic", "Synthetic", "SyntheticDecoder");
  synthetic_json_feature.SetTypedPayload(new SyntheticPayload(23));
  std::vector<FeatureRecord> synthetic_json_features;
  synthetic_json_features.push_back(synthetic_json_feature);
  ParseContext synthetic_json_context;
  synthetic_json_context.statistics.enumerated_total = 1;
  synthetic_json_context.statistics.typed_count = 1;
  tests.Check(writer.Write(synthetic_json_features, std::vector<RelationRecord>(),
                           synthetic_json_context, "selftest_output_synthetic_payload", write_error),
              "Synthetic payload artifact writes successfully");
  tests.Check(ReadWholeFile("selftest_output_synthetic_payload\\features.jsonl").find(
                "\"synthetic\":{\"value\":23}") != std::string::npos,
              "Central writer serializes a new payload without a dedicated branch");

  // 验证 Hole JSON 使用数字数组、布尔值和 null，而不是字符串化数值或伪造 0。
  FeatureRecord hole_json_feature = MakeFeature("F400010", "", 1, "", "CoolingPort_A",
                                                "NativeHoleDecoder");
  hole_json_feature.fingerprint.startup_type = "Hole";
  hole_json_feature.decoder_version = "1.0.0";
  hole_json_feature.SetTypedPayload(new NativeHolePayload(through_data));
  std::vector<FeatureRecord> hole_json_features;
  hole_json_features.push_back(hole_json_feature);
  std::vector<RelationRecord> hole_json_relations;
  ParseContext hole_json_context;
  hole_json_context.statistics.enumerated_total = 1;
  hole_json_context.statistics.typed_count = 1;
  hole_json_context.statistics.native_hole_candidate_count = 1;
  hole_json_context.statistics.native_hole_success_count = 1;
  tests.Check(writer.Write(hole_json_features, hole_json_relations, hole_json_context,
                           "selftest_output_native_hole", write_error),
              "Native Hole JSON artifact writes successfully");
  const std::string hole_json = ReadWholeFile("selftest_output_native_hole\\features.jsonl");
  tests.Check(hole_json.find("\"origin_mm\":[-65,-25,0]") != std::string::npos &&
              hole_json.find("\"direction\":[0,0,1]") != std::string::npos &&
              hole_json.find("\"depth_mm\":null") != std::string::npos &&
              hole_json.find("\"enabled\":false") != std::string::npos,
              "Native Hole JSON preserves number arrays boolean and null types");
  tests.Check(ReadWholeFile("selftest_output_native_hole\\parser.log").find(
                "schema=cad_parse_mvp_v11") != std::string::npos,
              "Product, FTA and completion artifact exports advance the parser schema to v11");

  // 验证 CATProduct 中同一 Reference 的多个 Instance 不会在统一树输出时被合并。
  ParseContext product_tree_context;
  ProductReferenceRecord reference;
  reference.reference_id = "R_SHARED";
  reference.part_number = "SharedPart";
  reference.read_status = "partial";
  product_tree_context.product_references.push_back(reference);
  ProductInstanceRecord root_instance;
  root_instance.instance_id = "I_ROOT";
  root_instance.reference_id = "ASM";
  root_instance.instance_name = "Assembly";
  root_instance.child_count = 2;
  product_tree_context.product_instances.push_back(root_instance);
  ProductInstanceRecord first_instance;
  first_instance.instance_id = "I001";
  first_instance.parent_instance_id = "I_ROOT";
  first_instance.reference_id = "R_SHARED";
  first_instance.instance_name = "Shared.1";
  first_instance.child_index = 1;
  product_tree_context.product_instances.push_back(first_instance);
  ProductInstanceRecord second_instance = first_instance;
  second_instance.instance_id = "I002";
  second_instance.instance_name = "Shared.2";
  second_instance.child_index = 2;
  product_tree_context.product_instances.push_back(second_instance);
  tests.Check(writer.Write(std::vector<FeatureRecord>(), std::vector<RelationRecord>(),
                           product_tree_context, "selftest_output_product_tree", write_error),
              "Product native tree JSON artifact writes successfully");
  const std::string product_tree_json =
    ReadWholeFile("selftest_output_product_tree\\native_tree_nodes.jsonl");
  tests.Check(product_tree_json.find("\"node_id\":\"instance:I001\"") != std::string::npos &&
              product_tree_json.find("\"node_id\":\"instance:I002\"") != std::string::npos &&
              product_tree_json.find("\"reference_id\":\"R_SHARED\"") != std::string::npos,
              "Repeated Product instances preserve unique node ids under the same Reference");

  // 验证派生记录存在悬空来源 ID 时 Writer 拒绝生成正式结果。
  std::vector<BusinessFeatureRecord> invalid_business;
  BusinessFeatureRecord dangling;
  dangling.source_feature_id = "F999999";
  dangling.feature_kind = "declared_unknown";
  invalid_business.push_back(dangling);
  std::vector<ParameterRecord> no_parameters;
  ParseContext invalid_reference_context = output_context;
  invalid_reference_context.statistics.declared_business_feature_total = 1;
  invalid_reference_context.statistics.declared_unknown_count = 1;
  tests.Check(!writer.Write(features, relations, no_parameters, invalid_business,
                            invalid_reference_context, "selftest_invalid_reference", write_error),
              "Artifact writer rejects dangling business feature sources");

  // 验证 staging 创建失败时请求的输出目录不会出现一套貌似完整的半成品。
  const char* blocker = "selftest_transaction_blocker";
  {
    std::ofstream blocker_file(blocker, std::ios::out | std::ios::binary | std::ios::trunc);
    blocker_file << "not a directory";
  }
  ParseContext transaction_context = output_context;
  const bool transaction_wrote = writer.Write(features, relations, transaction_context,
                                               std::string(blocker) + "\\result", write_error);
  std::ifstream incomplete((std::string(blocker) + "\\result\\features.jsonl").c_str(),
                           std::ios::in | std::ios::binary);
  tests.Check(!transaction_wrote && !incomplete,
              "Artifact transaction failure leaves no complete-looking requested output");
  std::remove(blocker);

  // 所有 Check 都执行后一次性返回失败数，便于同一次运行看到多个独立问题。
  return tests.Failures();
}
}
