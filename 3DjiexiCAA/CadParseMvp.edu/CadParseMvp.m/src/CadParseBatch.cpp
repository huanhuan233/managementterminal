// 本文件提供无界面的 Batch 进程入口，串起参数解析、CAA Session、文档遍历、IR 输出和退出码。
// main 是最外层异常边界；更细的对象级失败由 Registry/Crawler 在内部隔离。
#include "CadParseCAA.h"
#include "CadParseIR.h"

#include <iostream>
#include <sys/stat.h>
#include <windows.h>

#ifndef CAD_PARSE_GIT_COMMIT
#define CAD_PARSE_GIT_COMMIT "unknown"
#endif
#ifndef CAD_PARSE_BUILD_TIMESTAMP_UTC
#define CAD_PARSE_BUILD_TIMESTAMP_UTC "unknown"
#endif

namespace cadparse
{
// 命令行参数的纯数据表示；不保存 argv 裸指针，解析后可独立使用。
struct BatchOptions
{
  // 用途：创建默认选项；默认输出紧凑 JSON，并执行正常 CATPart 解析而不是自测。
  BatchOptions() : pretty(false), self_test(false), include_source_path(false) {}
  std::string input;
  std::string output;
  bool pretty;
  bool self_test;
  bool include_source_path;
};

// 用途：把 argc/argv 转换为 BatchOptions，并验证必须成对出现的参数。
// argv 内存由 C 运行库拥有，本函数只把所需文本复制进 std::string。
static bool ParseArguments(int argc, char** argv, BatchOptions& options, std::string& error)
{
  int i = 1;
  for (; i < argc; ++i)
  {
    const std::string argument = argv[i];
    if (argument == "--input" && i + 1 < argc) options.input = argv[++i];
    else if (argument == "--output" && i + 1 < argc) options.output = argv[++i];
    else if (argument == "--pretty") options.pretty = true;
    else if (argument == "--include-source-path") options.include_source_path = true;
    // 解析器始终只读打开；接受该显式开关是为了让命令含义清楚且与外部调用契约一致。
    else if (argument == "--read-only") {}
    else if (argument == "--self-test") options.self_test = true;
    else
    {
      error = std::string("unknown or incomplete argument: ") + argument;
      return false;
    }
  }
  if (!options.self_test && (options.input.empty() || options.output.empty()))
  {
    error = "--input and --output are required";
    return false;
  }
  return true;
}

static bool EndsWithNoCase(const std::string& value, const char* suffix)
{
  const std::string ending(suffix);
  if (value.size() < ending.size()) return false;
  const std::string tail = value.substr(value.size() - ending.size());
  std::string::size_type i = 0;
  for (; i < tail.size(); ++i)
  {
    char a = tail[i];
    char b = ending[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}
}

// 用途：执行一次 Batch 任务并把每类文档级失败映射为稳定非零退出码。
// 函数内的局部 RAII 对象保证任意 return 路径都会先关闭 Document，再清理 Session。
static int RunBatch(int argc, char** argv)
{
  using namespace cadparse;
  BatchOptions options;
  std::string error;
  if (!ParseArguments(argc, argv, options, error))
  {
    std::cerr << error << std::endl;
    std::cerr << "usage: CadParseMvp --input <file.CATPart> --output <directory> [--read-only] [--pretty] [--include-source-path]"
              << std::endl;
    return 2;
  }
  if (options.self_test)
  {
    // --self-test 不打开 CATIA 文档，可用于快速验证 API 无关模块。
    SelfTestSuite suite;
    const int failures = suite.RunAll();
    if (failures)
    {
      std::cerr << "self-test failures: " << failures << std::endl;
      return 1;
    }
    std::cout << "self-test passed" << std::endl;
    return 0;
  }

  const DWORD total_start = GetTickCount();
  ParseContext context;
  context.metadata.schema_version = CAD_PARSE_SCHEMA_VERSION;
  context.metadata.parser_version = CAD_PARSE_PARSER_VERSION;
  context.metadata.registry_version = CAD_PARSE_REGISTRY_VERSION;
  context.metadata.decoder_bundle_version = CAD_PARSE_DECODER_BUNDLE_VERSION;
  context.metadata.parser_git_commit = CAD_PARSE_GIT_COMMIT;
  context.metadata.parser_git_commit_source = std::string(CAD_PARSE_GIT_COMMIT) == "unknown" ?
    "build did not provide CAD_PARSE_GIT_COMMIT" : "embedded by build_r21_x86.bat";
  context.metadata.build_timestamp_utc = CAD_PARSE_BUILD_TIMESTAMP_UTC;
  context.metadata.build_timestamp_source = std::string(CAD_PARSE_BUILD_TIMESTAMP_UTC) == "unknown" ?
    "build did not provide CAD_PARSE_BUILD_TIMESTAMP_UTC" : "embedded by build_r21_x86.bat";
  context.metadata.execution_started_utc = UtcNowIso8601();
  context.metadata.input_file_name = SourcePathForOutput(options.input, false);
  context.metadata.document_kind = EndsWithNoCase(options.input, ".CATProduct") ?
    "catproduct" : "catpart";
  context.metadata.include_source_path = options.include_source_path;
  context.metadata.input_source_path = SourcePathForOutput(options.input, options.include_source_path);
  struct _stat input_status;
  if (_stat(options.input.c_str(), &input_status) == 0)
    context.metadata.input_size_bytes = static_cast<unsigned long>(input_status.st_size);
  context.metadata.input_sha256 = Sha256File(options.input, error);
  if (context.metadata.input_sha256.empty())
  {
    std::cerr << error << std::endl;
    return 9;
  }
  context.metadata.runtime_catia_release = "V5R21";
  context.metadata.runtime_service_pack = "unknown";
  context.metadata.runtime_hotfix = "unknown";
  context.metadata.runtime_value_source = "parser build target; no verified R21 Public SP/HF runtime API";
  ReadSourceFileHint(options.input, context.metadata);
  context.metadata.discovery_entrypoints.push_back("CATDocument");
  if (context.metadata.document_kind == "catproduct")
  {
    context.metadata.discovery_entrypoints.push_back("CATIProduct::GetChildren(CATIProduct)");
    context.metadata.discovery_entrypoints.push_back("CATIProduct::GetReferenceProduct");
    context.metadata.discovery_coverage_scope =
      "CATProduct BOM reachable through verified R21 Public ProductStructure entrypoints; referenced CATPart feature mounting requires validated reference document traversal";
  }
  else
  {
    context.metadata.discovery_entrypoints.push_back("CATIPrtContainer::GetPart");
    context.metadata.discovery_entrypoints.push_back("CATIContainer::ListMembersHere(CATISpecObject)");
    context.metadata.discovery_coverage_scope =
      "objects reachable through verified R21 Public CATPart entrypoints; not guaranteed exhaustive";
  }

  SessionGuard session;
  if (!session.Open(error))
  {
    std::cerr << error << std::endl;
    return 10;
  }

  const DWORD open_start = GetTickCount();
  DocumentGuard document;
  if (!document.OpenReadOnly(options.input, error))
  {
    std::cerr << error << std::endl;
    return 11;
  }
  context.statistics.document_open_ms = static_cast<long>(GetTickCount() - open_start);

  FeatureTypeRegistry registry;
  // Registry 只借用这些 Decoder 指针，因此本函数用单独 vector 跟踪并显式 delete 所有权。
  std::vector<IFeatureDecoder*> decoders;
  RegisterCoreDecoders(registry, decoders);
  std::vector<FeatureRecord> features;
  std::vector<RelationRecord> relations;

  const DWORD traversal_start = GetTickCount();
  UniversalFeatureCrawler crawler(registry, context, features, relations);
  if (!crawler.Crawl(document.Get(), error))
  {
    // Crawler 失败发生在 Decoder RAII 之外，返回前必须释放工厂创建的 Decoder。
    DeleteCoreDecoders(decoders);
    std::cerr << error << std::endl;
    return 12;
  }
  context.statistics.traversal_ms = static_cast<long>(GetTickCount() - traversal_start);
  DeleteCoreDecoders(decoders);

  std::vector<ParameterRecord> parameters;
  ParameterRecordBuilder::Build(features, relations, context, parameters);
  std::vector<BusinessFeatureRecord> business_features;
  DeclaredBusinessFeatureAggregator::Aggregate(features, relations, parameters,
                                               context, business_features);

  if (!CoverageTracker::Validate(context.statistics))
  {
    std::cerr << "coverage conservation failed" << std::endl;
    return 13;
  }

  JsonArtifactWriter writer(options.pretty);
  context.statistics.total_ms = static_cast<long>(GetTickCount() - total_start);
  // Writer 在 staging 内只完整写一次核心文件，最后用目录改名提交；output_ms 不含 Manifest 自身。
  if (!writer.Write(features, relations, parameters, business_features,
                    context, options.output, error))
  {
    std::cerr << error << std::endl;
    return 14;
  }

  std::cout << "parsed " << features.size() << " objects; parameters=" << parameters.size()
            << "; declared_business_features=" << business_features.size()
            << "; output=" << options.output << std::endl;
  return 0;
}

// 用途：作为操作系统调用的程序入口，并捕获所有越过内部边界的 CAA/C++ 异常。
// 返回值直接成为进程退出码，便于脚本可靠判断成功或失败。
int main(int argc, char** argv)
{
  try
  {
    return RunBatch(argc, argv);
  }
  catch (...)
  {
    std::cerr << "unhandled CAA/native exception at batch boundary" << std::endl;
    return 15;
  }
}
