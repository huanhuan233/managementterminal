import zipfile
from pathlib import Path

from fastapi.testclient import TestClient

from app.catia_worker.server import CatiaWorkerServerSettings, WorkerService, create_app


def test_worker_resolves_relative_work_dir_before_external_process_changes_cwd(tmp_path, monkeypatch):
    """相对任务目录必须先固定为绝对路径，避免 CAA 子进程从仓库根目录解析到错误位置。"""
    monkeypatch.chdir(tmp_path)
    service = WorkerService(CatiaWorkerServerSettings(work_dir=Path("relative-worker")))

    assert service.settings.work_dir == (tmp_path / "relative-worker").resolve()


def test_worker_health_reports_readiness_without_sensitive_paths(tmp_path):
    app = create_app(
        CatiaWorkerServerSettings(
            work_dir=tmp_path,
            token="secret",
            caa_rade_root="D:/fake/rade",
            caa_prereq_root="D:/fake/catia",
        )
    )

    with TestClient(app) as client:
        unauthorized = client.get("/health")
        response = client.get("/health", headers={"Authorization": "Bearer secret"})

    assert unauthorized.status_code == 401
    assert response.status_code == 200
    assert "D:/fake" not in response.text
    assert response.json()["max_concurrency"] == 1


def test_worker_rejects_non_catia_and_empty_upload(tmp_path):
    app = create_app(CatiaWorkerServerSettings(work_dir=tmp_path))

    with TestClient(app) as client:
        wrong = client.post("/v1/jobs", files={"source_file": ("part.step", b"STEP")})
        empty = client.post("/v1/jobs", files={"source_file": ("part.CATPart", b"")})

    assert wrong.status_code == 415
    assert wrong.json()["detail"]["code"] == "unsupported_format"
    assert empty.status_code == 400
    assert empty.json()["detail"]["code"] == "empty_source_file"


def test_worker_accepts_catproduct_and_preserves_extension_for_caa(tmp_path):
    from app.catia_worker.server import _resolve_job_source

    async def noop_processor(*_args, **_kwargs):
        return None

    app = create_app(CatiaWorkerServerSettings(work_dir=tmp_path), noop_processor)

    with TestClient(app) as client:
        response = client.post("/v1/jobs", files={"source_file": ("assy.CATProduct", b"CATProduct")})

    assert response.status_code == 202
    job_id = response.json()["worker_job_id"]
    job_root = tmp_path / job_id
    assert (job_root / "assy.CATProduct").read_bytes() == b"CATProduct"
    assert _resolve_job_source(job_root) == job_root / "assy.CATProduct"


def test_worker_accepts_catproduct_zip_and_resolves_bundle_entry(tmp_path):
    from app.catia_worker.server import _resolve_job_source

    job_root = tmp_path / "job"
    job_root.mkdir()
    with zipfile.ZipFile(job_root / "source.zip", "w") as archive:
        archive.writestr("assembly/top.CATProduct", b"CATProduct")
        archive.writestr("assembly/parts/part1.CATPart", b"CATPart")

    source = _resolve_job_source(job_root)

    assert source == job_root / "source-bundle" / "assembly" / "top.CATProduct"
    assert (job_root / "source-bundle" / "assembly" / "parts" / "part1.CATPart").read_bytes() == b"CATPart"


def test_worker_job_id_does_not_reuse_source_file_name(tmp_path):
    app = create_app(CatiaWorkerServerSettings(work_dir=tmp_path, enabled=False))

    with TestClient(app) as client:
        response = client.post("/v1/jobs", files={"source_file": ("零件 (1).CATPart", b"CATPart")})

    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "catia_worker_disabled"
