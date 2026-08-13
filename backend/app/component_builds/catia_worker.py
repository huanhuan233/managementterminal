"""Windows CATIA Worker 的 HTTP 客户端与稳定数据契约。"""

from __future__ import annotations

import asyncio
import hashlib
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Awaitable, Callable

import httpx


class CatiaWorkerError(RuntimeError):
    """用途：保留 Worker 故障类型和阶段，供编排层稳定映射错误码。"""

    def __init__(self, code: str, message: str, stage: str = "dispatching_caa"):
        super().__init__(message)
        self.code = code
        self.stage = stage


@dataclass(frozen=True)
class WorkerArtifact:
    """用途：描述一个经过大小和 SHA-256 校验的 Worker 产物。"""

    name: str
    sha256: str
    size_bytes: int

    @classmethod
    def from_payload(cls, payload: dict) -> "WorkerArtifact":
        """用途：校验服务端产物名称，禁止目录穿越后再建立强类型记录。"""
        name = str(payload.get("name") or "")
        if not name or Path(name).name != name or name in {".", ".."}:
            raise ValueError("Worker 产物名称不安全")
        sha256 = str(payload.get("sha256") or "").lower()
        if len(sha256) != 64 or any(char not in "0123456789abcdef" for char in sha256):
            raise ValueError("Worker 产物 SHA-256 无效")
        size_bytes = int(payload.get("size_bytes") or 0)
        if size_bytes < 0:
            raise ValueError("Worker 产物大小无效")
        return cls(name=name, sha256=sha256, size_bytes=size_bytes)


@dataclass(frozen=True)
class CatiaWorkerResult:
    """用途：向 Web 编排层返回稳定任务编号、最终状态和本地已验证产物。"""

    worker_job_id: str
    status: str
    stage: str
    artifacts: tuple[WorkerArtifact, ...]


StageCallback = Callable[[dict], Awaitable[None]]


def _catproduct_upload_path(source_path: Path) -> tuple[Path, tempfile.TemporaryDirectory[str] | None]:
    if source_path.suffix.lower() != ".catproduct":
        return source_path, None
    dependency_suffixes = {".catproduct", ".catpart", ".cgr"}
    candidates = sorted(
        path
        for path in source_path.parent.rglob("*")
        if path.is_file() and path.suffix.lower() in dependency_suffixes
    )
    if len(candidates) <= 1:
        return source_path, None
    temporary = tempfile.TemporaryDirectory()
    archive_path = Path(temporary.name) / f"{source_path.stem}-catia-bundle.zip"
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(source_path, source_path.name)
        for path in candidates:
            if path.resolve() == source_path.resolve():
                continue
            archive.write(path, path.relative_to(source_path.parent).as_posix())
    return archive_path, temporary


class CatiaWorkerClient:
    """用途：上传 CATPart 字节、轮询 Windows Worker，并校验下载产物。"""

    def __init__(
        self,
        base_url: str,
        token: str = "",
        connect_timeout_seconds: float = 5,
        request_timeout_seconds: float = 120,
        poll_interval_seconds: float = 1,
        job_timeout_seconds: float = 1800,
        http_client: httpx.AsyncClient | None = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.poll_interval_seconds = max(0.0, poll_interval_seconds)
        self.job_timeout_seconds = max(1.0, job_timeout_seconds)
        self._owns_http_client = http_client is None
        timeout = httpx.Timeout(request_timeout_seconds, connect=connect_timeout_seconds)
        self.http = http_client or httpx.AsyncClient(base_url=self.base_url, timeout=timeout)

    # 用途：生成统一认证头；空令牌时不发送 Authorization。
    def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.token}"} if self.token else {}

    # 用途：读取 Worker 可接单状态，不返回或记录敏感令牌。
    async def health(self) -> dict:
        try:
            response = await self.http.get(f"{self.base_url}/health", headers=self._headers())
            response.raise_for_status()
            return dict(response.json())
        except (httpx.HTTPError, ValueError) as exc:
            raise CatiaWorkerError("catia_worker_unavailable", "无法连接 Windows CATIA Worker") from exc

    # 用途：完成一次远程 CATPart 处理；HTTP 请求只上传字节，不共享本机绝对路径。
    async def process(
        self,
        source_path: Path,
        output_dir: Path,
        on_stage: StageCallback | None = None,
    ) -> CatiaWorkerResult:
        health = await self.health()
        if not health.get("accepting_jobs", False):
            raise CatiaWorkerError("catia_worker_rejected", "Windows CATIA Worker 当前不可接单")
        upload_path, temporary_upload = _catproduct_upload_path(source_path)
        try:
            with upload_path.open("rb") as stream:
                response = await self.http.post(
                    f"{self.base_url}/v1/jobs",
                    headers=self._headers(),
                    files={"source_file": (upload_path.name, stream, "application/octet-stream")},
                )
            if response.status_code != 202:
                raise CatiaWorkerError("catia_worker_rejected", f"Windows CATIA Worker 拒绝任务：HTTP {response.status_code}")
            worker_job_id = str(response.json().get("worker_job_id") or "")
            if not worker_job_id:
                raise CatiaWorkerError("catia_worker_rejected", "Windows CATIA Worker 未返回任务编号")
            payload = await self._poll_job(worker_job_id, on_stage)
            artifacts = tuple(WorkerArtifact.from_payload(item) for item in payload.get("artifacts", []))
            await self._download_artifacts(worker_job_id, artifacts, output_dir)
            return CatiaWorkerResult(
                worker_job_id=worker_job_id,
                status=str(payload.get("status") or "completed"),
                stage=str(payload.get("stage") or "completed"),
                artifacts=artifacts,
            )
        except CatiaWorkerError:
            raise
        except httpx.TimeoutException as exc:
            raise CatiaWorkerError("catia_worker_timeout", "Windows CATIA Worker 请求超时") from exc
        except httpx.HTTPError as exc:
            raise CatiaWorkerError("catia_worker_unavailable", "Windows CATIA Worker 通信失败") from exc
        except (ValueError, OSError) as exc:
            raise CatiaWorkerError("catia_worker_artifact_invalid", "Windows CATIA Worker 产物无效") from exc
        finally:
            if temporary_upload is not None:
                temporary_upload.cleanup()
            if self._owns_http_client:
                await self.http.aclose()

    # 用途：轮询持久化 Worker 任务，并把真实阶段反馈给 Web 任务状态。
    async def _poll_job(self, worker_job_id: str, on_stage: StageCallback | None) -> dict:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + self.job_timeout_seconds
        while True:
            if loop.time() >= deadline:
                raise CatiaWorkerError("catia_worker_timeout", "Windows CATIA Worker 任务超时")
            response = await self.http.get(
                f"{self.base_url}/v1/jobs/{worker_job_id}",
                headers=self._headers(),
            )
            response.raise_for_status()
            payload = dict(response.json())
            if on_stage is not None:
                await on_stage(payload)
            status = str(payload.get("status") or "")
            if status == "completed":
                return payload
            if status in {"failed", "cancelled"}:
                raise CatiaWorkerError(
                    str(payload.get("error_code") or "caa_parse_failed"),
                    str(payload.get("error_message") or "Windows CATIA Worker 处理失败"),
                    str(payload.get("stage") or "running_caa"),
                )
            await asyncio.sleep(self.poll_interval_seconds)

    # 用途：逐个下载受控产物，并在写入任务目录后核对大小和 SHA-256。
    async def _download_artifacts(
        self,
        worker_job_id: str,
        artifacts: tuple[WorkerArtifact, ...],
        output_dir: Path,
    ) -> None:
        output_dir.mkdir(parents=True, exist_ok=True)
        for artifact in artifacts:
            response = await self.http.get(
                f"{self.base_url}/v1/jobs/{worker_job_id}/artifacts/{artifact.name}",
                headers=self._headers(),
            )
            response.raise_for_status()
            data = response.content
            digest = hashlib.sha256(data).hexdigest()
            if len(data) != artifact.size_bytes or digest != artifact.sha256:
                raise CatiaWorkerError("catia_worker_artifact_invalid", f"Worker 产物校验失败：{artifact.name}")
            (output_dir / artifact.name).write_bytes(data)
