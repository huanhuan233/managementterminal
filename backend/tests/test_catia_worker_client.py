import hashlib
import json
import zipfile
from pathlib import Path

import httpx
import pytest

from app.component_builds.catia_worker import (
    CatiaWorkerClient,
    CatiaWorkerError,
    WorkerArtifact,
)


@pytest.mark.asyncio
async def test_http_worker_uploads_bytes_polls_and_downloads_verified_artifacts(tmp_path):
    source = tmp_path / "零件 (终版).CATPart"
    source.write_bytes(b"CATPart-test-bytes")
    native_zip = b"native-zip"
    exported_step = b"ISO-10303-21;"
    requests = []

    async def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        assert request.headers.get("authorization") == "Bearer secret-token"
        if request.url.path == "/health":
            return httpx.Response(200, json={"status": "ready", "accepting_jobs": True})
        if request.url.path == "/v1/jobs" and request.method == "POST":
            body = await request.aread()
            assert b"CATPart-test-bytes" in body
            return httpx.Response(202, json={"worker_job_id": "job-001", "status": "queued"})
        if request.url.path == "/v1/jobs/job-001":
            return httpx.Response(
                200,
                json={
                    "worker_job_id": "job-001",
                    "status": "completed",
                    "stage": "completed",
                    "progress": 100,
                    "artifacts": [
                        {
                            "name": "native_bundle.zip",
                            "sha256": hashlib.sha256(native_zip).hexdigest(),
                            "size_bytes": len(native_zip),
                        },
                        {
                            "name": "exported.stp",
                            "sha256": hashlib.sha256(exported_step).hexdigest(),
                            "size_bytes": len(exported_step),
                        },
                    ],
                },
            )
        if request.url.path.endswith("/native_bundle.zip"):
            return httpx.Response(200, content=native_zip)
        if request.url.path.endswith("/exported.stp"):
            return httpx.Response(200, content=exported_step)
        return httpx.Response(404)

    http = httpx.AsyncClient(transport=httpx.MockTransport(handler), base_url="http://worker")
    client = CatiaWorkerClient(
        base_url="http://worker",
        token="secret-token",
        poll_interval_seconds=0,
        job_timeout_seconds=5,
        http_client=http,
    )

    health = await client.health()
    result = await client.process(source, tmp_path / "result")

    assert health["accepting_jobs"] is True
    assert result.worker_job_id == "job-001"
    assert (tmp_path / "result" / "native_bundle.zip").read_bytes() == native_zip
    assert (tmp_path / "result" / "exported.stp").read_bytes() == exported_step
    assert len(requests) == 6
    await http.aclose()


@pytest.mark.asyncio
async def test_http_worker_rejects_checksum_mismatch(tmp_path):
    source = tmp_path / "part.CATPart"
    source.write_bytes(b"source")

    async def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/health":
            return httpx.Response(200, json={"status": "ready", "accepting_jobs": True})
        if request.url.path == "/v1/jobs":
            return httpx.Response(202, json={"worker_job_id": "job-002", "status": "queued"})
        if request.url.path == "/v1/jobs/job-002":
            return httpx.Response(
                200,
                json={
                    "worker_job_id": "job-002",
                    "status": "completed",
                    "stage": "completed",
                    "progress": 100,
                    "artifacts": [
                        {
                            "name": "exported.stp",
                            "sha256": "0" * 64,
                            "size_bytes": 4,
                        }
                    ],
                },
            )
        return httpx.Response(200, content=b"STEP")

    http = httpx.AsyncClient(transport=httpx.MockTransport(handler), base_url="http://worker")
    client = CatiaWorkerClient(base_url="http://worker", poll_interval_seconds=0, http_client=http)

    with pytest.raises(CatiaWorkerError) as error:
        await client.process(source, tmp_path / "result")

    assert error.value.code == "catia_worker_artifact_invalid"
    await http.aclose()


@pytest.mark.asyncio
async def test_http_worker_uploads_catproduct_with_local_dependency_bundle(tmp_path):
    source = tmp_path / "5621C04000G23.CATProduct"
    source.write_bytes(b"CATProduct-root")
    (tmp_path / "R_5621C04000G23.CATPart").write_bytes(b"CATPart-ref")
    requests = []

    async def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        if request.url.path == "/health":
            return httpx.Response(200, json={"status": "ready", "accepting_jobs": True})
        if request.url.path == "/v1/jobs" and request.method == "POST":
            body = await request.aread()
            assert b"catia-bundle.zip" in body
            start = body.find(b"PK\x03\x04")
            end = body.rfind(b"PK\x05\x06")
            assert start >= 0 and end >= start
            archive_bytes = body[start : end + 22]
            archive_path = tmp_path / "uploaded.zip"
            archive_path.write_bytes(archive_bytes)
            with zipfile.ZipFile(archive_path) as archive:
                assert sorted(archive.namelist()) == [
                    "5621C04000G23.CATProduct",
                    "R_5621C04000G23.CATPart",
                ]
            return httpx.Response(202, json={"worker_job_id": "job-zip", "status": "queued"})
        if request.url.path == "/v1/jobs/job-zip":
            return httpx.Response(200, json={"worker_job_id": "job-zip", "status": "completed", "artifacts": []})
        return httpx.Response(404)

    http = httpx.AsyncClient(transport=httpx.MockTransport(handler), base_url="http://worker")
    client = CatiaWorkerClient(base_url="http://worker", poll_interval_seconds=0, http_client=http)

    result = await client.process(source, tmp_path / "result")

    assert result.worker_job_id == "job-zip"
    assert len(requests) == 3
    await http.aclose()


def test_worker_artifact_rejects_path_traversal():
    with pytest.raises(ValueError):
        WorkerArtifact.from_payload({"name": "../secret.txt", "sha256": "0" * 64, "size_bytes": 1})
