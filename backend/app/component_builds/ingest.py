"""统一零件上传的格式识别、后台编排和受控资产定位。"""

from __future__ import annotations

import asyncio
import json
import locale
import logging
import os
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from functools import partial
from types import SimpleNamespace
from pathlib import Path
from uuid import UUID

from pydantic_settings import BaseSettings, SettingsConfigDict

from app.cad.repository import CadRepository
from app.cad.service import CadService
from app.component_builds.catia_worker import CatiaWorkerClient, CatiaWorkerError
from app.core.config import REPOSITORY_ROOT, Settings
from app.db.session import SessionLocal
from app.feature_center.native_tree import (
    NativeTreeJsonError,
    build_native_tree_from_bundle,
    write_jsonl,
)


@dataclass(frozen=True)
class IngestSource:
    source_format: str
    processing_route: str
    extension: str


class IngestSourceError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class IngestStageError(RuntimeError):
    def __init__(self, code: str, stage: str, message: str):
        super().__init__(message)
        self.code = code
        self.stage = stage


_CATIA_LIMITER = asyncio.Semaphore(1)
_INGEST_TASKS: dict[UUID, asyncio.Task[None]] = {}
_LOCAL_PATH_PATTERN = re.compile(r"(?i)(?:[a-z]:[\\/]|\\\\)[^\r\n]*")
LOGGER = logging.getLogger(__name__)


class _IngestRuntimeSettings(BaseSettings):
    """用途：只读取零件编排所需外部工具路径，不改变全局 Settings 的用户改动。"""

    model_config = SettingsConfigDict(
        env_file=REPOSITORY_ROOT / "backend" / ".env",
        env_file_encoding="utf-8",
        extra="ignore",
    )
    caa_rade_root: str = ""
    caa_prereq_root: str = ""


# 用途：只根据已校验文件名识别真实格式，绝不接受客户端指定处理路线。
def identify_source(file_name: str) -> IngestSource:
    extension = Path(Path(file_name).name).suffix.lower()
    if extension in {".step", ".stp"}:
        return IngestSource("STEP", "step_cad_parse", extension)
    if extension == ".catpart":
        return IngestSource("CATPART", "catia_feature_center", extension)
    if extension == ".catproduct":
        return IngestSource("CATPRODUCT", "catia_feature_center", extension)
    if extension == ".zip":
        return IngestSource("CATPRODUCT", "catia_feature_center", extension)
    raise IngestSourceError(
        "UNSUPPORTED_SOURCE_FORMAT",
        "仅支持 STEP、STP、CATPart、CATProduct 或 CATProduct 依赖 ZIP 文件；不支持 .cart",
    )


# 用途：把浏览器请求的相对资产限定在当前任务目录内，阻止目录穿越和绝对路径泄露。
def safe_asset_path(task_root: Path, relative_path: str) -> Path:
    root = task_root.resolve()
    candidate_text = Path(relative_path)
    if candidate_text.is_absolute():
        raise IngestSourceError("VIEWER_ASSET_PATH_INVALID", "Viewer 资产路径必须是相对路径")
    candidate = (root / candidate_text).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise IngestSourceError("VIEWER_ASSET_PATH_INVALID", "Viewer 资产路径越过任务目录") from exc
    return candidate


# 用途：从对外错误消息中移除 Windows 绝对路径，完整堆栈只保留在服务日志边界。
def redact_local_paths(message: str) -> str:
    return _LOCAL_PATH_PATTERN.sub("<local_path>", message)


# 用途：顶层异常本身可能没有文本，仍需把异常类型写入持久化任务，避免前端只得到无法排查的空错误。
def unexpected_error_message(exc: Exception) -> str:
    detail = redact_local_paths(str(exc)).strip()
    return detail or f"未预期异常：{type(exc).__name__}"


# 用途：把上传任务交给独立数据库会话执行，HTTP 请求只负责入队和返回任务编号。
def schedule_ingest(revision_id: UUID, settings: Settings) -> None:
    existing = _INGEST_TASKS.get(revision_id)
    if existing is not None and not existing.done():
        LOGGER.info("同一 Revision 已在处理中，忽略重复入队：revision_id=%s", revision_id)
        return
    task = asyncio.create_task(run_ingest_pipeline(revision_id, settings))
    _INGEST_TASKS[revision_id] = task

    # 用途：仅移除当前登记的同一任务，避免旧任务回调误删后续重试任务。
    def forget(completed: asyncio.Task[None]) -> None:
        if _INGEST_TASKS.get(revision_id) is completed:
            _INGEST_TASKS.pop(revision_id, None)

    task.add_done_callback(forget)


# 用途：按持久化路由串联现有 STEP 或 CATPart 能力，并把每个真实阶段写回 Revision。
async def run_ingest_pipeline(revision_id: UUID, settings: Settings) -> None:
    async with SessionLocal() as session:
        repository = CadRepository(session)
        revision = await repository.get_revision(revision_id)
        if revision is None:
            return
        ingest = dict((revision.parse_manifest or {}).get("ingest", {}))
        route = ingest.get("processing_route")
        try:
            _prepare_generated_outputs(Path(settings.cad_work_dir) / str(revision_id), route)
            if route == "step_cad_parse":
                await _run_step_route(repository, revision_id, settings)
            elif route == "catia_feature_center":
                async with _CATIA_LIMITER:
                    await _run_catpart_route(repository, revision_id, settings)
            else:
                raise IngestStageError("PROCESSING_ROUTE_INVALID", "queued", "任务缺少合法处理路线")
        except IngestStageError as exc:
            await repository.set_revision_status(
                revision_id,
                status="failed",
                progress=100,
                status_message=exc.stage,
                error_code=exc.code,
                error_message=redact_local_paths(str(exc))[:1000],
            )
        except asyncio.CancelledError:
            await repository.set_revision_status(
                revision_id,
                status="failed",
                progress=100,
                status_message="interrupted",
                error_code="PART_INGEST_INTERRUPTED",
                error_message="处理进程被服务停止，可从原任务重试",
            )
            raise
        except Exception as exc:
            LOGGER.exception("零件后台编排发生未预期错误：revision_id=%s", revision_id)
            await repository.set_revision_status(
                revision_id,
                status="failed",
                progress=100,
                status_message="failed",
                error_code="PART_INGEST_UNEXPECTED",
                error_message=unexpected_error_message(exc)[:1000],
            )


# 用途：复用现有 FreeCAD 解析后再构建统一 Feature Center 轻量化资产。
async def _run_step_route(repository: CadRepository, revision_id: UUID, settings: Settings) -> None:
    before = await repository.get_revision(revision_id)
    ingest = dict((before.parse_manifest or {}).get("ingest", {})) if before else {}
    service = CadService(repository, settings)
    await service.parse_revision(revision_id)
    revision = await repository.get_revision(revision_id)
    if revision is None or revision.status != "completed":
        raise IngestStageError(
            revision.error_code or "STEP_PARSE_FAILED",
            "parsing",
            revision.error_message or "STEP 解析失败",
        )
    await repository.update_revision_manifest(revision_id, {"ingest": ingest})
    await _set_stage(repository, revision_id, "lightweighting", 80)
    await _build_feature_center(repository, revision_id, settings, Path(revision.source_file_path), None)


# 用途：先运行已有 CAA 原生解析，再用 Automation ExportData 导出 STEP，最后调用既有 Sidecar。
async def _run_catpart_route(repository: CadRepository, revision_id: UUID, settings: Settings) -> None:
    revision = await repository.get_revision(revision_id)
    if revision is None:
        return
    task_root = Path(settings.cad_work_dir) / str(revision_id)
    native_bundle = task_root / "native-caa"
    exported_step = task_root / "exported.stp"
    export_report = task_root / "step-export.json"
    mode = settings.catia_worker_mode.strip().lower()
    if mode == "disabled":
        raise IngestStageError("catia_worker_disabled", "dispatching_caa", "CATIA Worker 未启用")
    if mode == "http":
        await _run_remote_catpart_worker(repository, revision_id, settings, revision.source_file_path, task_root)
        await _set_stage(repository, revision_id, "feature_center_processing", 70)
        await _build_feature_center(repository, revision_id, settings, exported_step, native_bundle)
        return
    if mode != "local_process":
        raise IngestStageError("catia_worker_mode_invalid", "dispatching_caa", "CATIA Worker 模式无效")
    if os.name != "nt":
        raise IngestStageError("catia_worker_unavailable", "dispatching_caa", "local_process 仅允许在 Windows 主机运行")

    runtime = _IngestRuntimeSettings()
    rade_root = os.environ.get("CAA_RADE_ROOT") or runtime.caa_rade_root
    prereq_root = os.environ.get("CAA_PREREQ_ROOT") or runtime.caa_prereq_root
    if not rade_root or not prereq_root:
        raise IngestStageError("catia_worker_unavailable", "dispatching_caa", "未配置 CAA_RADE_ROOT 或 CAA_PREREQ_ROOT")

    await _set_stage(repository, revision_id, "running_caa", 15)
    await _run_command(
        [
            "cmd.exe", "/d", "/c", str(REPOSITORY_ROOT / "3DjiexiCAA" / "tools" / "run_r21_x86.bat"),
            "--input", str(revision.source_file_path), "--output", str(native_bundle), "--read-only",
        ],
        "caa_parse_failed",
        "running_caa",
        settings.freecad_timeout,
        {"CAA_RADE_ROOT": rade_root, "CAA_PREREQ_ROOT": prereq_root},
    )
    await _set_stage(repository, revision_id, "exporting_step", 45)
    await _run_command(
        [
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(REPOSITORY_ROOT / "3DjiexiCAA" / "tools" / "export_catpart_step.ps1"),
            "-InputCatPart", str(revision.source_file_path),
            "-OutputStep", str(exported_step),
            "-ReportPath", str(export_report),
        ],
        "catia_step_export_failed",
        "exporting_step",
        settings.freecad_timeout,
    )
    await _set_stage(repository, revision_id, "feature_center_processing", 70)
    await _build_feature_center(repository, revision_id, settings, exported_step, native_bundle)


# 用途：通过 HTTP Worker 上传 CATPart 字节并下载经哈希校验的原生 IR 与 STEP 产物。
async def _run_remote_catpart_worker(
    repository: CadRepository,
    revision_id: UUID,
    settings: Settings,
    source_path: str,
    task_root: Path,
) -> None:
    download_root = task_root / "worker-download"
    await _set_stage(repository, revision_id, "dispatching_caa", 8)
    client = CatiaWorkerClient(
        base_url=settings.catia_worker_url,
        token=settings.catia_worker_token,
        connect_timeout_seconds=settings.catia_worker_connect_timeout,
        request_timeout_seconds=settings.catia_worker_request_timeout,
        poll_interval_seconds=settings.catia_worker_poll_interval,
        job_timeout_seconds=settings.catia_worker_job_timeout,
    )

    async def report_stage(payload: dict) -> None:
        stage = str(payload.get("stage") or "queued_caa")
        progress = max(8, min(65, int(payload.get("progress") or 10)))
        await _set_stage(repository, revision_id, stage, progress)
        worker_job_id = str(payload.get("worker_job_id") or "")
        if worker_job_id:
            await repository.update_revision_manifest(revision_id, {"worker": {"worker_job_id": worker_job_id}})

    try:
        result = await client.process(Path(source_path), download_root, report_stage)
    except CatiaWorkerError as exc:
        raise IngestStageError(exc.code, exc.stage, str(exc)) from exc
    await repository.update_revision_manifest(
        revision_id,
        {
            "worker": {
                "worker_job_id": result.worker_job_id,
                "mode": "http",
                "status": result.status,
                "stage": result.stage,
            }
        },
    )
    native_archive = download_root / "native_bundle.zip"
    exported_step = download_root / "exported.stp"
    if not native_archive.is_file() or not exported_step.is_file() or exported_step.stat().st_size == 0:
        raise IngestStageError("catia_step_export_failed", "exporting_step", "Worker 未返回完整原生结果和 STEP")
    _extract_worker_archive(native_archive, task_root / "native-caa")
    shutil.copy2(exported_step, task_root / "exported.stp")


# 用途：只解压相对普通文件，拒绝 Worker ZIP 中的绝对路径、父目录和符号链接式穿越。
def _extract_worker_archive(archive_path: Path, output_dir: Path) -> None:
    output_root = output_dir.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            destination = (output_root / member.filename).resolve()
            try:
                destination.relative_to(output_root)
            except ValueError as exc:
                raise IngestStageError("catia_worker_artifact_invalid", "publishing_assets", "Worker ZIP 包含越界路径") from exc
        archive.extractall(output_root)


# 用途：调用仓库已有 Sidecar CLI 生成并校验 Bundle，不在 Web 层复制任何几何算法。
async def _build_feature_center(
    repository: CadRepository,
    revision_id: UUID,
    settings: Settings,
    step_path: Path,
    native_bundle: Path | None,
) -> None:
    task_root = Path(settings.cad_work_dir) / str(revision_id)
    bundle = task_root / "feature-center"
    command = [
        sys.executable,
        str(REPOSITORY_ROOT / "backend" / "scripts" / "feature_center.py"),
        "build", "--step", str(step_path), "--output", str(bundle),
        "--visual-review-mode", "disabled",
    ]
    if native_bundle is not None:
        command.extend(["--native-bundle", str(native_bundle)])
    await _run_command(command, "FEATURE_CENTER_FAILED", "feature_center_processing", settings.freecad_timeout)
    required = (
        "manifest.json",
        "lightweight/model.glb",
        "lightweight/face_mesh_map.json",
        "lightweight/feature_mesh_map.json",
        "canonical_features.jsonl",
        "feature_geometry_links.jsonl",
        "measurements.jsonl",
    )
    missing = [name for name in required if not (bundle / name).is_file()]
    if missing or (bundle / "lightweight" / "model.glb").stat().st_size == 0:
        raise IngestStageError("VIEWER_ASSET_MISSING", "lightweighting", ",".join(missing) or "model.glb 为空")
    feature_center_manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    native_feature_count = 0
    if native_bundle is not None and (native_bundle / "features.jsonl").is_file():
        native_feature_count = _count_jsonl_records(native_bundle / "features.jsonl")
    _publish_native_tree_assets(native_bundle, source_file.name)
    native_assets = _available_native_assets(native_bundle)
    recognized_feature_count = _count_jsonl_records(bundle / "canonical_features.jsonl")
    feature_face_mapping_count = _count_jsonl_records(bundle / "feature_geometry_links.jsonl")
    solid_count = _read_feature_center_solid_count(bundle / "parts.jsonl")
    await repository.update_revision_manifest(
        revision_id,
        {
            "viewer_asset": {
                "bundle_root": "feature-center",
                "glb": "feature-center/lightweight/model.glb",
                "scene_manifest": "feature-center/manifest.json",
                "face_mesh_map": "feature-center/lightweight/face_mesh_map.json",
                "feature_mesh_map": "feature-center/lightweight/feature_mesh_map.json",
                "selection_index": "feature-center/lightweight/selection_index.json"
                if (bundle / "lightweight" / "selection_index.json").is_file() else None,
            },
            "feature_center": {
                "available": (bundle / "canonical_features.jsonl").stat().st_size > 0,
                # 用途：Feature Center Bundle 可用不等于已经建立特征到面的映射，必须按真实链接记录单独声明。
                "mapping_available": feature_face_mapping_count > 0,
                "feature_face_mapping_count": feature_face_mapping_count,
                "bundle_available": True,
                "canonical_features": "feature-center/canonical_features.jsonl",
                "feature_geometry_links": "feature-center/feature_geometry_links.jsonl",
                "measurements": "feature-center/measurements.jsonl",
                "topology_faces": "feature-center/topology_faces.jsonl"
                if (bundle / "topology_faces.jsonl").is_file() else None,
                "topology_edges": "feature-center/topology_edges.jsonl"
                if (bundle / "topology_edges.jsonl").is_file() else None,
            },
            "native_semantics": {
                "available": bool(native_assets),
                **native_assets,
            },
            "viewer_summary": {
                "solid_count": solid_count,
                "native_feature_count": native_feature_count,
                "recognized_feature_count": recognized_feature_count,
            },
            "feature_center_manifest": {
                "lightweight": feature_center_manifest.get("lightweight") or {},
                "performance": feature_center_manifest.get("performance") or {},
            },
        },
    )
    # 用途：只有 Sidecar 产物、Viewer 必需资产和清单全部校验完成后，才把任务标记为可展示。
    await repository.set_revision_status(
        revision_id,
        status="completed",
        progress=100,
        status_message="ready",
        error_code=None,
        error_message=None,
    )


# 用途：确定性统计 JSONL 非空记录，不因文件末尾换行多算一条。
def _count_jsonl_records(path: Path) -> int:
    if not path.is_file():
        return 0
    with path.open("r", encoding="utf-8") as stream:
        return sum(1 for line in stream if line.strip())


# 用途：从 Feature Center 的真实 Part 摘要累计 Solid 数；Schema 缺字段时返回零而非猜测。
def _read_feature_center_solid_count(path: Path) -> int:
    if not path.is_file():
        return 0
    total = 0
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            total += int(record.get("solid_count") or record.get("geometry_summary", {}).get("solid_count") or 0)
    return total


def _available_native_assets(native_bundle: Path | None) -> dict[str, str]:
    if native_bundle is None:
        return {}
    candidates = {
        "features": "features.jsonl",
        "native_features": "native_features.jsonl",
        "topology_bodies": "native_topology_bodies.jsonl",
        "topology_cells": "native_topology_cells.jsonl",
        "topology_wires": "native_topology_wires.jsonl",
        "topology_coedges": "native_topology_coedges.jsonl",
        "mesh_face_map": "native_mesh_face_map.jsonl",
        "mesh_triangles": "native_mesh_triangles.jsonl",
        "feature_results": "native_feature_results.jsonl",
        "feature_result_cells": "native_feature_result_cells.jsonl",
        "feature_topology_links": "native_feature_topology_links.jsonl",
        "product_references": "product_references.jsonl",
        "product_instances": "product_instances.jsonl",
        "native_tree_nodes": "native_tree_nodes.jsonl",
        "node_properties": "node_properties.jsonl",
        "native_tree_diagnostics": "native_tree_diagnostics.jsonl",
        "capabilities": "capabilities.json",
    }
    return {
        key: f"native-caa/{file_name}"
        for key, file_name in candidates.items()
        if (native_bundle / file_name).is_file()
    }


def _publish_native_tree_assets(native_bundle: Path | None, source_file_name: str) -> None:
    if native_bundle is None or not native_bundle.is_dir():
        return
    diagnostics_path = native_bundle / "native_tree_diagnostics.jsonl"
    try:
        result = build_native_tree_from_bundle(native_bundle, source_file_name)
        write_jsonl(native_bundle / "native_tree_nodes.jsonl", result.nodes)
        write_jsonl(native_bundle / "node_properties.jsonl", result.properties)
        write_jsonl(diagnostics_path, result.diagnostics)
    except NativeTreeJsonError as exc:
        write_jsonl(diagnostics_path, [{
            "diagnostic_id": "NATIVE_TREE_JSON_INVALID",
            "severity": "error",
            "message": str(exc),
            "file_name": exc.file_name,
            "line_number": exc.line_number,
        }])


# 用途：重试时只清理本任务生成物，保留上传源文件和持久化任务记录。
def _prepare_generated_outputs(task_root: Path, route: str | None) -> None:
    generated = [task_root / "feature-center"]
    if route == "catia_feature_center":
        generated.extend([
            task_root / "native-caa",
            task_root / "exported.stp",
            task_root / "step-export.json",
        ])
    for path in generated:
        if path.is_dir():
            shutil.rmtree(path)
        elif path.is_file():
            path.unlink()


# 用途：更新处理中间阶段，同时保留上传时写入的格式、路线和源文件追溯信息。
async def _set_stage(repository: CadRepository, revision_id: UUID, stage: str, progress: int) -> None:
    await repository.set_revision_status(
        revision_id,
        status="processing",
        progress=progress,
        status_message=stage,
        error_code=None,
        error_message=None,
    )


# 用途：异步执行现有外部工具，统一处理超时、退出码和可读诊断。
async def _run_command(
    command: list[str],
    error_code: str,
    stage: str,
    timeout_seconds: int,
    environment: dict[str, str] | None = None,
) -> None:
    try:
        return_code, stdout, stderr = await asyncio.get_running_loop().run_in_executor(
            None,
            partial(_run_command_sync, command, timeout_seconds, environment),
        )
        process = SimpleNamespace(returncode=return_code)
    except (asyncio.TimeoutError, subprocess.TimeoutExpired) as exc:
        raise IngestStageError(error_code, stage, "处理超时") from exc
    except asyncio.CancelledError:
        # 用途：线程池中的子进程由超时清理逻辑负责回收；取消协程不能安全地跨线程终止其句柄。
        raise
    if process.returncode != 0:
        detail = _decode_process_output(stderr or stdout).strip()
        LOGGER.error("外部处理命令失败：stage=%s command=%s detail=%s", stage, command[0], detail)
        raise IngestStageError(error_code, stage, redact_local_paths(detail[-1000:]) or f"退出码 {process.returncode}")


# 用途：兼容 Windows 子进程通过管道输出 UTF-8 或本机代码页中文，避免错误信息出现乱码。
# 用途：通过标准 subprocess 在工作线程中执行外部工具，兼容 Windows SelectorEventLoop。
def _run_command_sync(
    command: list[str],
    timeout_seconds: int,
    environment: dict[str, str] | None,
) -> tuple[int, bytes, bytes]:
    process = subprocess.Popen(
        command,
        cwd=str(REPOSITORY_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, **(environment or {})},
    )
    try:
        stdout, stderr = process.communicate(timeout=max(1, timeout_seconds))
    except subprocess.TimeoutExpired:
        # 用途：超时后清理本任务启动的子进程，避免 CAA 或 FreeCAD 留在后台。
        if os.name == "nt":
            subprocess.run(
                ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        if process.returncode is None:
            process.kill()
        process.communicate()
        raise
    return process.returncode, stdout, stderr


def _decode_process_output(content: bytes) -> str:
    if not content:
        return ""
    try:
        return content.decode("utf-8")
    except UnicodeDecodeError:
        preferred = locale.getpreferredencoding(False) or "gbk"
        try:
            return content.decode(preferred)
        except (LookupError, UnicodeDecodeError):
            return content.decode("utf-8", errors="replace")


# 用途：终止本任务启动的完整外部进程树，防止超时或服务停止后残留 CATIA 子进程。
async def _terminate_process_tree(process: asyncio.subprocess.Process) -> None:
    if process.returncode is not None:
        return
    if os.name == "nt":
        killer = await asyncio.create_subprocess_exec(
            "taskkill.exe",
            "/PID",
            str(process.pid),
            "/T",
            "/F",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        await killer.communicate()
    if process.returncode is None:
        try:
            process.kill()
        except ProcessLookupError:
            pass
    await process.wait()
