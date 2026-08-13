"""可独立部署在 Windows CATIA 主机上的轻量 Worker HTTP 服务。"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import shutil
import sys
import zipfile
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Awaitable, Callable
from uuid import uuid4

from fastapi import Depends, FastAPI, File, Header, HTTPException, UploadFile
from fastapi.responses import FileResponse
from pydantic import AliasChoices, Field
from pydantic_settings import BaseSettings, SettingsConfigDict

from app.core.config import BACKEND_ROOT, REPOSITORY_ROOT


# 用途：生成不依赖本机时区的阶段时间，便于后端和 Worker 日志对齐。
def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


class CatiaWorkerServerSettings(BaseSettings):
    """用途：集中声明 Worker 部署参数；敏感值不进入健康检查响应。"""

    model_config = SettingsConfigDict(
        env_file=BACKEND_ROOT / ".env",
        env_file_encoding="utf-8",
        extra="ignore",
    )
    work_dir: Path = Field(
        default=REPOSITORY_ROOT / ".runtime" / "catia-worker",
        validation_alias=AliasChoices("CATIA_WORKER_SERVER_WORK_DIR", "work_dir"),
    )
    token: str = Field(default="", validation_alias=AliasChoices("CATIA_WORKER_TOKEN", "token"))
    enabled: bool = Field(default=True, validation_alias=AliasChoices("CATIA_WORKER_SERVER_ENABLED", "enabled"))
    max_concurrency: int = Field(default=1, validation_alias=AliasChoices("CATIA_WORKER_MAX_CONCURRENCY", "max_concurrency"))
    max_upload_mb: int = Field(default=200, validation_alias=AliasChoices("CAD_MAX_UPLOAD_MB", "max_upload_mb"))
    job_timeout_seconds: int = Field(default=1800, validation_alias=AliasChoices("CATIA_WORKER_JOB_TIMEOUT", "job_timeout_seconds"))
    caa_rade_root: str = Field(default="", validation_alias=AliasChoices("CAA_RADE_ROOT", "caa_rade_root"))
    caa_prereq_root: str = Field(default="", validation_alias=AliasChoices("CAA_PREREQ_ROOT", "caa_prereq_root"))


@dataclass
class WorkerJob:
    """用途：保存可持久化任务状态以及仅在进程内使用的取消信息。"""

    worker_job_id: str
    source_name: str
    source_sha256: str
    source_size_bytes: int
    status: str = "queued"
    stage: str = "queued_caa"
    progress: int = 0
    error_code: str | None = None
    error_message: str | None = None
    created_at: str = field(default_factory=_utc_now)
    started_at: str | None = None
    finished_at: str | None = None
    stages: list[dict] = field(default_factory=list)
    artifacts: list[dict] = field(default_factory=list)

    # 用途：只输出跨进程稳定字段，不把任务目录或进程句柄暴露给客户端。
    def as_dict(self) -> dict:
        return {
            key: value
            for key, value in self.__dict__.items()
            if key not in {"source_name"}
        }


Processor = Callable[[WorkerJob, Path, CatiaWorkerServerSettings], Awaitable[None]]


def _safe_source_name(filename: str | None, suffix: str) -> str:
    name = Path(filename or "").name
    if not name:
        return "source.CATProduct" if suffix == ".catproduct" else "source.CATPart"
    if Path(name).suffix.lower() != suffix:
        return "source.CATProduct" if suffix == ".catproduct" else "source.CATPart"
    return name


class WorkerService:
    """用途：管理串行队列、任务状态文件和本任务创建的外部进程。"""

    def __init__(self, settings: CatiaWorkerServerSettings, processor: Processor | None = None):
        # 用途：任务目录必须在启动时固定为绝对路径；CAA 子进程会切换工作目录，不能再次解释相对路径。
        settings.work_dir = settings.work_dir.resolve()
        self.settings = settings
        self.processor = processor or _process_job
        self.jobs: dict[str, WorkerJob] = {}
        self.tasks: dict[str, asyncio.Task[None]] = {}
        self.limiter = asyncio.Semaphore(max(1, settings.max_concurrency))
        settings.work_dir.mkdir(parents=True, exist_ok=True)

    # 用途：保存任务 JSON 时先写临时文件再替换，避免进程中断留下半截状态。
    def persist(self, job: WorkerJob) -> None:
        job_root = self.settings.work_dir / job.worker_job_id
        job_root.mkdir(parents=True, exist_ok=True)
        temporary = job_root / "job.json.tmp"
        temporary.write_text(json.dumps(job.as_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(job_root / "job.json")

    # 用途：保存上传字节并创建随机任务编号，源文件名只作为受控显示信息。
    async def create(self, upload: UploadFile) -> WorkerJob:
        suffix = Path(upload.filename or "").suffix.lower()
        if suffix not in {".catpart", ".catproduct", ".zip"}:
            raise HTTPException(415, detail={"code": "unsupported_format", "message": "Worker 只接受 CATPart/CATProduct/ZIP"})
        worker_job_id = str(uuid4())
        job_root = self.settings.work_dir / worker_job_id
        job_root.mkdir(parents=True)
        source_name = "source.zip" if suffix == ".zip" else _safe_source_name(upload.filename, suffix)
        source_path = job_root / source_name
        digest = hashlib.sha256()
        size = 0
        with source_path.open("wb") as stream:
            while chunk := await upload.read(1024 * 1024):
                size += len(chunk)
                if size > self.settings.max_upload_mb * 1024 * 1024:
                    stream.close()
                    source_path.unlink(missing_ok=True)
                    raise HTTPException(413, detail={"code": "source_too_large", "message": "CATPart 超过上传限制"})
                digest.update(chunk)
                stream.write(chunk)
        if size == 0:
            source_path.unlink(missing_ok=True)
            raise HTTPException(400, detail={"code": "empty_source_file", "message": "CATPart 不能为空"})
        job = WorkerJob(worker_job_id, Path(upload.filename or "source.CATPart").name, digest.hexdigest(), size)
        self.jobs[worker_job_id] = job
        self.persist(job)
        self.tasks[worker_job_id] = asyncio.create_task(self._run(job))
        return job

    # 用途：在限制并发的临界区运行 CAA，并将未预期异常转换为稳定错误码。
    async def _run(self, job: WorkerJob) -> None:
        async with self.limiter:
            job.status = "running"
            job.started_at = _utc_now()
            self.persist(job)
            try:
                await asyncio.wait_for(
                    self.processor(job, self.settings.work_dir / job.worker_job_id, self.settings),
                    timeout=max(1, self.settings.job_timeout_seconds),
                )
                job.status = "completed"
                job.stage = "completed"
                job.progress = 100
            except asyncio.CancelledError:
                job.status = "cancelled"
                job.stage = "cancelled"
                job.error_code = "catia_worker_cancelled"
                job.error_message = "任务已取消"
            except asyncio.TimeoutError:
                job.status = "failed"
                job.error_code = "catia_worker_timeout"
                job.error_message = "CATIA Worker 任务超时"
            except WorkerExecutionError as exc:
                job.status = "failed"
                job.stage = exc.stage
                job.error_code = exc.code
                job.error_message = str(exc)
            except Exception:
                job.status = "failed"
                job.error_code = "catia_worker_unexpected"
                job.error_message = "CATIA Worker 发生未预期错误，请查看任务日志"
            finally:
                job.finished_at = _utc_now()
                self.persist(job)


class WorkerExecutionError(RuntimeError):
    """用途：保留 CAA/导出阶段错误，不向 HTTP 层暴露完整命令和敏感路径。"""

    def __init__(self, code: str, stage: str, message: str):
        super().__init__(message)
        self.code = code
        self.stage = stage


# 用途：更新阶段并持久化；前端轮询可以看到真实 CAA 与导出进度。
def _set_stage(service_job: WorkerJob, settings: CatiaWorkerServerSettings, stage: str, progress: int) -> None:
    now = _utc_now()
    if service_job.stages and service_job.stages[-1].get("finished_at") is None:
        service_job.stages[-1]["finished_at"] = now
    service_job.stage = stage
    service_job.progress = progress
    service_job.stages.append({"stage": stage, "started_at": now, "finished_at": None})
    job_root = settings.work_dir / service_job.worker_job_id
    temporary = job_root / "job.json.tmp"
    temporary.write_text(json.dumps(service_job.as_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(job_root / "job.json")


# 用途：调用仓库已有 CAA Batch 与 Automation 导出器，不复制任何 CATIA 解析逻辑。
def _resolve_job_source(job_root: Path) -> Path:
    if (job_root / "source.zip").is_file():
        bundle_root = job_root / "source-bundle"
        if bundle_root.exists():
            shutil.rmtree(bundle_root)
        bundle_root.mkdir()
        resolved_root = bundle_root.resolve()
        with zipfile.ZipFile(job_root / "source.zip") as archive:
            entries = [item for item in archive.infolist() if not item.is_dir()]
            for entry in entries:
                destination = (bundle_root / entry.filename).resolve()
                try:
                    destination.relative_to(resolved_root)
                except ValueError as exc:
                    raise WorkerExecutionError("catia_source_bundle_invalid", "queued_caa", "CATProduct ZIP 包含越界路径") from exc
            archive.extractall(bundle_root)
        catproducts = sorted(
            path for path in bundle_root.rglob("*") if path.is_file() and path.suffix.lower() == ".catproduct"
        )
        if len(catproducts) != 1:
            raise WorkerExecutionError(
                "catia_source_bundle_invalid",
                "queued_caa",
                "CATProduct ZIP 必须包含且只能包含一个入口 CATProduct",
            )
        return catproducts[0]
    if (job_root / "source.CATProduct").is_file():
        return job_root / "source.CATProduct"
    if (job_root / "source.CATPart").is_file():
        return job_root / "source.CATPart"
    loose_sources = sorted(
        path
        for path in job_root.iterdir()
        if path.is_file() and path.suffix.lower() in {".catproduct", ".catpart"}
    )
    catproducts = [path for path in loose_sources if path.suffix.lower() == ".catproduct"]
    if len(catproducts) == 1:
        return catproducts[0]
    if len(loose_sources) == 1:
        return loose_sources[0]
    raise WorkerExecutionError("catia_source_missing", "queued_caa", "CATIA 源文件不存在")


async def _process_job(job: WorkerJob, job_root: Path, settings: CatiaWorkerServerSettings) -> None:
    source = _resolve_job_source(job_root)
    staging = job_root / "staging"
    artifacts = job_root / "artifacts"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    native = staging / "native-caa"
    exported_step = staging / "exported.stp"
    export_report = staging / "step-export.json"
    log_path = staging / "worker.log"
    if not settings.caa_rade_root or not settings.caa_prereq_root:
        raise WorkerExecutionError("catia_worker_unavailable", "running_caa", "Worker 未配置 CAA/RADE 环境")
    _set_stage(job, settings, "running_caa", 20)
    await _run_process(
        [
            "cmd.exe", "/d", "/c", str(REPOSITORY_ROOT / "3DjiexiCAA" / "tools" / "run_r21_x86.bat"),
            "--input", str(source), "--output", str(native), "--read-only",
        ],
        job,
        "caa_parse_failed",
        "running_caa",
        log_path,
        {"CAA_RADE_ROOT": settings.caa_rade_root, "CAA_PREREQ_ROOT": settings.caa_prereq_root},
    )
    _set_stage(job, settings, "exporting_step", 65)
    await _run_process(
        [
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(REPOSITORY_ROOT / "3DjiexiCAA" / "tools" / "export_catpart_step.ps1"),
            "-InputCatPart", str(source), "-OutputStep", str(exported_step), "-ReportPath", str(export_report),
        ],
        job,
        "catia_step_export_failed",
        "exporting_step",
        log_path,
    )
    if not exported_step.is_file() or exported_step.stat().st_size == 0:
        raise WorkerExecutionError("catia_step_export_failed", "exporting_step", "CATIA 未生成有效 STEP")
    _set_stage(job, settings, "publishing_artifacts", 90)
    archive_path = staging / "native_bundle.zip"
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(native.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(native).as_posix())
    manifest = {
        "schema_version": "catia_worker_job_v1",
        "worker_job_id": job.worker_job_id,
        "source_sha256": job.source_sha256,
        "source_size_bytes": job.source_size_bytes,
        "artifacts": [],
    }
    artifact_sources = [archive_path, exported_step, export_report, log_path]
    for path in artifact_sources:
        if not path.is_file():
            continue
        data = path.read_bytes()
        manifest["artifacts"].append(
            {"name": path.name, "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        )
    manifest_path = staging / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    manifest_data = manifest_path.read_bytes()
    manifest["artifacts"].append(
        {"name": "manifest.json", "size_bytes": len(manifest_data), "sha256": hashlib.sha256(manifest_data).hexdigest()}
    )
    if artifacts.exists():
        shutil.rmtree(artifacts)
    staging.replace(artifacts)
    job.artifacts = manifest["artifacts"]


# 用途：执行外部工具并保留完整日志；超时/取消只终止本任务创建的进程树。
async def _run_process(
    command: list[str],
    job: WorkerJob,
    error_code: str,
    stage: str,
    log_path: Path,
    environment: dict[str, str] | None = None,
) -> None:
    process = await asyncio.create_subprocess_exec(
        *command,
        cwd=str(REPOSITORY_ROOT),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        env={**os.environ, **(environment or {})},
    )
    try:
        output, _ = await process.communicate()
    except asyncio.CancelledError:
        await _terminate_process_tree(process)
        raise
    text = output.decode("utf-8", errors="replace")
    with log_path.open("a", encoding="utf-8") as stream:
        stream.write(f"[{stage}]\n{text}\n")
    if process.returncode != 0:
        raise WorkerExecutionError(error_code, stage, f"外部工具退出码 {process.returncode}")


# 用途：按父进程 PID 清理本任务子树，不扫描或杀死用户手工打开的 CATIA 进程。
async def _terminate_process_tree(process: asyncio.subprocess.Process) -> None:
    if process.returncode is not None:
        return
    if os.name == "nt":
        killer = await asyncio.create_subprocess_exec(
            "taskkill.exe", "/PID", str(process.pid), "/T", "/F",
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
        )
        await killer.communicate()
    if process.returncode is None:
        process.kill()
    await process.wait()


def create_app(
    settings: CatiaWorkerServerSettings | None = None,
    processor: Processor | None = None,
) -> FastAPI:
    """用途：创建可测试、可独立启动的 Worker 应用，不初始化 Web 主站数据库。"""

    worker_settings = settings or CatiaWorkerServerSettings()
    service = WorkerService(worker_settings, processor)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        yield
        for task in service.tasks.values():
            if not task.done():
                task.cancel()
        if service.tasks:
            await asyncio.gather(*service.tasks.values(), return_exceptions=True)

    app = FastAPI(title="CATIA R21 CAA Worker", lifespan=lifespan)
    app.state.service = service

    # 用途：可选 Bearer 认证；配置为空时允许仅由网络边界保护的本机开发模式。
    async def authorize(authorization: str | None = Header(default=None)) -> None:
        if worker_settings.token and authorization != f"Bearer {worker_settings.token}":
            raise HTTPException(401, detail={"code": "unauthorized", "message": "Worker 认证失败"})

    @app.get("/health", dependencies=[Depends(authorize)])
    async def health() -> dict:
        environment_ready = bool(worker_settings.caa_rade_root and worker_settings.caa_prereq_root and os.name == "nt")
        running = sum(not task.done() for task in service.tasks.values())
        return {
            "status": "ready" if worker_settings.enabled and environment_ready else "degraded",
            "accepting_jobs": worker_settings.enabled and environment_ready,
            "worker_version": "1.0.0",
            "catia_release": "V5R21",
            "max_concurrency": max(1, worker_settings.max_concurrency),
            "active_job_count": running,
            "queued_job_count": max(0, running - max(1, worker_settings.max_concurrency)),
        }

    @app.post("/v1/jobs", status_code=202, dependencies=[Depends(authorize)])
    async def create_job(source_file: UploadFile = File(...)) -> dict:
        if not worker_settings.enabled:
            raise HTTPException(503, detail={"code": "catia_worker_disabled", "message": "CATIA Worker 未启用"})
        job = await service.create(source_file)
        return {"worker_job_id": job.worker_job_id, "status": job.status, "stage": job.stage}

    @app.get("/v1/jobs/{worker_job_id}", dependencies=[Depends(authorize)])
    async def get_job(worker_job_id: str) -> dict:
        job = service.jobs.get(worker_job_id)
        if job is None:
            raise HTTPException(404, detail={"code": "job_not_found", "message": "Worker 任务不存在"})
        return job.as_dict()

    @app.get("/v1/jobs/{worker_job_id}/manifest", dependencies=[Depends(authorize)])
    async def get_manifest(worker_job_id: str):
        return _artifact_response(service, worker_job_id, "manifest.json")

    @app.get("/v1/jobs/{worker_job_id}/artifacts/{name}", dependencies=[Depends(authorize)])
    async def get_artifact(worker_job_id: str, name: str):
        return _artifact_response(service, worker_job_id, name)

    @app.post("/v1/jobs/{worker_job_id}/cancel", dependencies=[Depends(authorize)])
    async def cancel_job(worker_job_id: str) -> dict:
        job = service.jobs.get(worker_job_id)
        task = service.tasks.get(worker_job_id)
        if job is None or task is None:
            raise HTTPException(404, detail={"code": "job_not_found", "message": "Worker 任务不存在"})
        if not task.done():
            task.cancel()
        return {"worker_job_id": worker_job_id, "status": "cancelling"}

    return app


# 用途：只允许下载任务清单中登记的普通文件，避免任意路径读取。
def _artifact_response(service: WorkerService, worker_job_id: str, name: str) -> FileResponse:
    job = service.jobs.get(worker_job_id)
    allowed = {item["name"] for item in (job.artifacts if job else [])}
    if job is None or Path(name).name != name or name not in allowed:
        raise HTTPException(404, detail={"code": "artifact_not_found", "message": "Worker 产物不存在"})
    path = service.settings.work_dir / worker_job_id / "artifacts" / name
    if not path.is_file():
        raise HTTPException(404, detail={"code": "artifact_not_found", "message": "Worker 产物不存在"})
    return FileResponse(path, filename=name)


app = create_app()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("app.catia_worker.server:app", host="127.0.0.1", port=5182, reload=False)
