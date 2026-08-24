import io
import zipfile
from types import SimpleNamespace
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient

from app.cad.router import get_cad_service
from app.component_builds.fusion import FusionSources
from app.component_builds.repository import MemoryComponentBuildRepository
from app.component_builds.router import get_component_build_service, run_drawing_pipeline
from app.component_builds.service import ComponentBuildService
from app.core.config import Settings, get_settings
from app.drawing.schemas import DrawingError
from app.drawing.router import get_drawing_service
from app.main import app


PNG_BYTES = b"\x89PNG\r\n\x1a\n"


def find_node(nodes: list[dict], node_type: str) -> dict:
    for node in nodes:
        if node["node_type"] == node_type:
            return node
        found = find_node(node.get("children", []), node_type)
        if found:
            return found
    return {}


def find_schema_field(fields: list[dict], path: str) -> dict:
    for field in fields:
        if field["path"] == path:
            return field
        found = find_schema_field(field.get("children", []), path)
        if found:
            return found
        item = field.get("item")
        if item:
            found = find_schema_field(item.get("children", []), path)
            if found:
                return found
    return {}


class FakeSourceStatusReader:
    async def get_step_status(self, _revision_id):
        return {"status": "queued", "progress": 0}

    async def get_drawing_status(self, _task_id):
        return {"status": "created", "progress": 0}


class FakeFusionSourceReader:
    def __init__(self):
        self.sources = FusionSources([], [], [])

    async def read(self, _build):
        return self.sources


class FakeCadService:
    def __init__(self):
        self.model_id = uuid4()
        self.revision_id = uuid4()
        self.uploads = []
        self.parses = []

    async def create_model_from_upload(self, file, name):
        self.uploads.append((file.filename, name))
        return {"model_id": self.model_id, "revision_id": self.revision_id, "status": "queued"}

    async def create_source_from_upload(self, file, name, *, source_format, processing_route):
        self.uploads.append((file.filename, name, source_format, processing_route))
        return {
            "model_id": self.model_id,
            "revision_id": self.revision_id,
            "task_id": self.revision_id,
            "status": "queued",
            "source_format": source_format,
            "processing_route": processing_route,
            "source_sha256": "a" * 64,
        }


class FakeDrawingService:
    def __init__(self):
        self.task_id = uuid4()
        self.created = []
        self.created_payloads = []
        self.repository = self

    async def create_task(self, *, revision_id, drawing_file, target_code, target_dn):
        self.created.append((revision_id, drawing_file, target_code, target_dn))
        self.created_payloads.append(drawing_file.read_bytes())
        return SimpleNamespace(id=self.task_id)

    async def get_source_for_task(self, _task_id):
        if not self.created:
            return None
        return SimpleNamespace(file_path=str(self.created[-1][1]))


@pytest.fixture
def component_client(tmp_path, monkeypatch):
    repository = MemoryComponentBuildRepository()
    fusion_reader = FakeFusionSourceReader()
    build_service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=fusion_reader,
    )
    cad_service = FakeCadService()
    drawing_service = FakeDrawingService()
    settings = Settings(cad_spec_work_dir=tmp_path)
    scheduled = []

    def schedule(task_id, _settings, *, target_code, target_dn):
        scheduled.append((task_id, target_code, target_dn))

    monkeypatch.setattr("app.component_builds.router.schedule_drawing_pipeline", schedule)
    monkeypatch.setattr(
        "app.component_builds.router.schedule_step_pipeline",
        lambda revision_id, _service: cad_service.parses.append(revision_id),
        raising=False,
    )
    monkeypatch.setattr(
        "app.component_builds.router.schedule_ingest",
        lambda revision_id, _settings: cad_service.parses.append(revision_id),
        raising=False,
    )
    app.dependency_overrides[get_component_build_service] = lambda: build_service
    app.dependency_overrides[get_cad_service] = lambda: cad_service
    app.dependency_overrides[get_drawing_service] = lambda: drawing_service
    app.dependency_overrides[get_settings] = lambda: settings
    try:
        yield TestClient(app), cad_service, drawing_service, scheduled, build_service
    finally:
        app.dependency_overrides.clear()


def create_build(client: TestClient, *, step_name: str = "XMS06-DN80.stp", drawing_name: str = "XMS06.png"):
    return client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "XMS06",
            # This legacy value must not influence the generated component ID.
            "component_id": "user-supplied-id",
        },
        files={
            "step_file": (step_name, b"ISO-10303-21;", "application/octet-stream"),
            "drawing_file": (drawing_name, PNG_BYTES, "image/png"),
        },
    )


def find_parameter(data: dict, name: str) -> dict:
    return next(item for item in data["parameters"] if item.get("name") == name)


def test_create_build_links_step_and_drawing(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client

    response = create_build(client)

    assert response.status_code == 202
    assert response.json()["component_id"] == "flange-001"
    assert response.json()["catalog_path"] == "/连接与紧固类/法兰"
    assert response.json()["default_dn"] is None
    assert response.json()["default_pn"] is None
    assert response.json()["cad_revision_id"] == str(cad_service.revision_id)
    assert response.json()["drawing_task_id"] == str(drawing_service.task_id)
    assert drawing_service.created[0][0] == cad_service.revision_id
    assert drawing_service.created[0][1].exists()
    assert drawing_service.created[0][2:] == ("flange-001", None)
    assert scheduled == [(drawing_service.task_id, "flange-001", None)]


def test_create_build_without_sources_creates_editable_draft(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "待补资料法兰",
        },
    )

    assert response.status_code == 202
    assert response.json()["status"] == "draft"
    assert response.json()["cad_revision_id"] is None
    assert response.json()["drawing_task_id"] is None
    assert cad_service.uploads == []
    assert drawing_service.created == []
    assert scheduled == []


def test_create_build_accepts_step_without_drawing(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "仅 STEP 法兰",
        },
        files={"step_file": ("flange.stp", b"ISO-10303-21;", "application/octet-stream")},
    )

    assert response.status_code == 202
    assert response.json()["cad_revision_id"] == str(cad_service.revision_id)
    assert response.json()["drawing_task_id"] is None
    assert response.json()["source_format"] == "STEP"
    assert response.json()["processing_route"] == "step_cad_parse"
    assert response.json()["status"] == "queued"
    assert drawing_service.created == []
    assert scheduled == []
    assert cad_service.parses == [cad_service.revision_id]


@pytest.mark.parametrize("file_name", ["KUANG (2).stp", "框架.STEP"])
def test_unified_source_upload_routes_step_from_real_extension(component_client, file_name):
    client, cad_service, _, _, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "统一 STEP 零件",
            "processing_route": "catia_feature_center",
        },
        files={"source_file": (file_name, b"ISO-10303-21;", "application/octet-stream")},
    )

    assert response.status_code == 202
    assert response.json()["part_id"] == response.json()["id"]
    assert response.json()["task_id"] == str(cad_service.revision_id)
    assert response.json()["source_format"] == "STEP"
    assert response.json()["processing_route"] == "step_cad_parse"
    assert response.json()["status"] == "queued"
    assert cad_service.parses == [cad_service.revision_id]


def test_unified_source_upload_routes_catpart_case_insensitively(component_client):
    client, cad_service, _, _, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "support-frame",
            "part_type_code": "frame",
            "component_name": "机械框架",
        },
        files={"source_file": ("框架 (终版).CATPart", b"CATIA", "application/octet-stream")},
    )

    assert response.status_code == 202
    assert response.json()["source_format"] == "CATPART"
    assert response.json()["processing_route"] == "catia_feature_center"
    assert cad_service.uploads[0][0] == "框架 (终版).CATPart"


def test_unified_source_upload_routes_catproduct_case_insensitively(component_client):
    client, cad_service, _, _, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "support-frame",
            "part_type_code": "frame",
            "component_name": "assembly",
        },
        files={"source_file": ("assembly.CATProduct", b"CATProduct", "application/octet-stream")},
    )

    assert response.status_code == 202
    assert response.json()["source_format"] == "CATPRODUCT"
    assert response.json()["processing_route"] == "catia_feature_center"
    assert cad_service.uploads[0][0] == "assembly.CATProduct"


def test_unified_source_upload_routes_catproduct_zip_bundle(component_client):
    client, cad_service, _, _, _ = component_client
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("assembly/top.CATProduct", b"CATProduct")
        archive.writestr("assembly/parts/part1.CATPart", b"CATPart")

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "support-frame",
            "part_type_code": "frame",
            "component_name": "assembly bundle",
        },
        files={"source_file": ("assembly-bundle.zip", buffer.getvalue(), "application/zip")},
    )

    assert response.status_code == 202
    assert response.json()["source_format"] == "CATPRODUCT"
    assert response.json()["processing_route"] == "catia_feature_center"
    assert cad_service.uploads[0][0] == "assembly-bundle.zip"


def test_cart_is_explicitly_rejected(component_client):
    client, _, _, _, _ = component_client

    response = client.post(
        "/api/component-builds",
        data={
            "category_code": "support-frame",
            "part_type_code": "frame",
            "component_name": "错误文件",
        },
        files={"source_file": ("wrong.cart", b"bad", "application/octet-stream")},
    )

    assert response.status_code == 400
    assert response.json()["detail"]["code"] == "UNSUPPORTED_SOURCE_FORMAT"


def test_step_only_update_does_not_attach_a_previously_staged_drawing(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client
    created = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "先图纸法兰",
        },
        files={"drawing_file": ("flange.png", PNG_BYTES, "image/png")},
    )

    assert created.status_code == 202
    assert created.json()["cad_revision_id"] is None
    assert created.json()["drawing_task_id"] is None
    assert drawing_service.created == []

    updated = client.patch(
        f"/api/component-builds/{created.json()['id']}",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "先图纸法兰",
            "version": "1.0.0",
        },
        files={"step_file": ("flange.stp", b"ISO-10303-21;", "application/octet-stream")},
    )

    assert updated.status_code == 202
    assert updated.json()["cad_revision_id"] == str(cad_service.revision_id)
    assert updated.json()["drawing_task_id"] is None
    assert drawing_service.created_payloads == []
    assert scheduled == []


def test_edit_metadata_without_files_preserves_existing_sources(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client
    created = create_build(client).json()

    response = client.patch(
        f"/api/component-builds/{created['id']}",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "已改名法兰",
            "standard_number": "GB/T EDITED",
            "version": "1.1.0",
        },
    )

    assert response.status_code == 202
    assert response.json()["component_name"] == "已改名法兰"
    assert response.json()["standard_number"] == "GB/T EDITED"
    assert response.json()["version"] == "1.1.0"
    assert response.json()["cad_revision_id"] == str(cad_service.revision_id)
    assert response.json()["drawing_task_id"] == str(drawing_service.task_id)
    assert len(cad_service.uploads) == 1
    assert len(drawing_service.created) == 1
    assert len(scheduled) == 1


def test_replacing_step_does_not_reuse_existing_drawing_when_no_new_drawing_is_selected(component_client):
    client, cad_service, drawing_service, scheduled, _ = component_client
    created = create_build(client).json()
    cad_service.revision_id = uuid4()

    response = client.patch(
        f"/api/component-builds/{created['id']}",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "XMS06",
            "version": "1.0.0",
        },
        files={"step_file": ("replacement.stp", b"ISO-10303-21;", "application/octet-stream")},
    )

    assert response.status_code == 202
    assert response.json()["cad_revision_id"] == str(cad_service.revision_id)
    assert response.json()["drawing_task_id"] is None
    assert len(drawing_service.created) == 1
    assert len(scheduled) == 1


def test_catalog_endpoint_returns_categories_and_cascading_parts(component_client):
    client, _, _, _, _ = component_client

    response = client.get("/api/component-builds/catalog")

    assert response.status_code == 200
    categories = response.json()["categories"]
    assert [category["category_code"] for category in categories[:6]] == [
        "support-frame", "shaft-transmission", "roller", "connection-fastening", "drive-actuation", "functional"
    ]
    assert [library["library_code"] for library in response.json()["libraries"]] == [
        "MECHANICAL_COMPONENT_LIBRARY"
    ]
    assert any(part["part_type_code"] == "flange" for part in categories[3]["parts"])


def test_rejects_category_part_mismatch_before_a_build_is_created(component_client):
    client, _, _, _, build_service = component_client

    response = client.post(
        "/api/component-builds",
        data={"category_code": "roller", "part_type_code": "flange", "component_name": "错误分类"},
        files={
            "step_file": ("bad.stp", b"ISO-10303-21;", "application/octet-stream"),
            "drawing_file": ("bad.png", PNG_BYTES, "image/png"),
        },
    )

    assert response.status_code == 400
    assert response.json()["detail"]["code"] == "invalid_catalog_selection"
    assert build_service.repository.builds == {}


def test_tree_exposes_specialist_targets(component_client):
    client, cad_service, drawing_service, _, _ = component_client
    create_build(client)

    response = client.get("/api/component-builds/tree")

    assert response.status_code == 200
    step = find_node(response.json(), "reference_step")
    drawing = find_node(response.json(), "drawing")
    assert step["target"]["revision_id"] == str(cad_service.revision_id)
    assert drawing["target"]["revision_id"] == str(cad_service.revision_id)
    assert drawing["target"]["task_id"] == str(drawing_service.task_id)


def test_component_spec_initializes_from_template_with_chinese_labels(component_client):
    client, _, _, _, _ = component_client
    build = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "空白法兰",
        },
    ).json()

    response = client.get(f"/api/component-builds/{build['id']}/component-spec")

    assert response.status_code == 200
    payload = response.json()
    assert payload["data"]["schema_version"] == "1.2"
    assert payload["data"]["spec_type"] == "component"
    assert payload["data"]["identity"]["name"] is None
    assert payload["yaml"].startswith("schema_version:")
    assert payload["source_filename"] is None
    assert len(payload["data"]["parameters"]) == 1
    assert payload["data"]["parameters"][0]["name"] is None
    all_fields = [field for section in payload["schema"]["sections"] for field in section["fields"]]
    identity_name = find_schema_field(all_fields, "identity.name")
    assert identity_name["label"] == "当前对象的名称"
    assert identity_name["required"] is True
    assert identity_name["read_only"] is False
    parameters = find_schema_field(all_fields, "parameters")
    assert parameters["kind"] == "object_array"
    assert parameters["repeatable"] is True

    tree = client.get("/api/component-builds/tree").json()
    spec_node = find_node(tree, "component_spec")
    assert spec_node["build_id"] == build["id"]
    assert spec_node["disabled"] is False


def test_component_spec_drafts_are_saved_per_build(component_client):
    client, _, _, _, _ = component_client
    first = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "第一件",
        },
    ).json()
    second = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "第二件",
        },
    ).json()
    first_payload = client.get(f"/api/component-builds/{first['id']}/component-spec").json()
    first_payload["data"]["identity"]["name"] = "带颈对焊钢制管法兰"

    saved = client.put(
        f"/api/component-builds/{first['id']}/component-spec",
        json={"data": first_payload["data"]},
    )

    assert saved.status_code == 200
    assert saved.json()["saved"] is True
    assert client.get(f"/api/component-builds/{first['id']}/component-spec").json()["data"]["identity"]["name"] == "带颈对焊钢制管法兰"
    assert client.get(f"/api/component-builds/{second['id']}/component-spec").json()["data"]["identity"]["name"] is None


def test_component_spec_saves_and_restores_uploaded_yaml_document(component_client):
    client, _, _, _, _ = component_client
    build = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "Uploaded YAML",
        },
    ).json()
    original = client.get(f"/api/component-builds/{build['id']}/component-spec").json()
    data = original["data"]
    data["custom_extension"] = {"curve_policy": "all"}
    yaml_text = f"{original['yaml']}\n# preserve this upload comment\ncustom_extension:\n  curve_policy: all\n"

    saved = client.put(
        f"/api/component-builds/{build['id']}/component-spec",
        json={
            "data": data,
            "yaml": yaml_text,
            "source_filename": "flange-v1.3.yaml",
        },
    )
    restored = client.get(f"/api/component-builds/{build['id']}/component-spec")

    assert saved.status_code == 200
    assert saved.json()["yaml"] == yaml_text
    assert restored.status_code == 200
    assert restored.json()["data"]["custom_extension"] == {"curve_policy": "all"}
    assert restored.json()["yaml"] == yaml_text
    assert restored.json()["source_filename"] == "flange-v1.3.yaml"

    mismatch = client.put(
        f"/api/component-builds/{build['id']}/component-spec",
        json={
            "data": {**data, "custom_extension": {"curve_policy": "circles-only"}},
            "yaml": yaml_text,
            "source_filename": "bad.yaml",
        },
    )

    assert mismatch.status_code == 422
    assert client.get(f"/api/component-builds/{build['id']}/component-spec").json()["yaml"] == yaml_text


def test_component_spec_preview_uses_template_structure_and_comments(component_client):
    client, _, _, _, _ = component_client
    build = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "预览件",
        },
    ).json()
    spec = client.get(f"/api/component-builds/{build['id']}/component-spec").json()
    spec["data"]["identity"]["name"] = "带颈对焊钢制管法兰"

    response = client.post(
        f"/api/component-builds/{build['id']}/component-spec/preview",
        json={"data": spec["data"]},
    )

    assert response.status_code == 200
    yaml_text = response.json()["yaml"]
    assert yaml_text.index("schema_version:") < yaml_text.index("identity:") < yaml_text.index("coordinate_system:")
    assert 'name: "带颈对焊钢制管法兰"' in yaml_text
    assert "【必填】（人工） 当前对象的名称" in yaml_text
    assert "parameters:" in yaml_text
    assert "provenance:" in yaml_text


def test_component_spec_preview_returns_valid_submitted_yaml_unchanged(component_client):
    client, _, _, _, _ = component_client
    build = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "Uploaded preview",
        },
    ).json()
    spec = client.get(f"/api/component-builds/{build['id']}/component-spec").json()
    yaml_text = f"# preview comment\n{spec['yaml']}"

    response = client.post(
        f"/api/component-builds/{build['id']}/component-spec/preview",
        json={"data": spec["data"], "yaml": yaml_text},
    )

    assert response.status_code == 200
    assert response.json()["yaml"] == yaml_text


def test_component_fusion_endpoint_saves_xms06_draft(component_client):
    client, _, _, _, build_service = component_client
    created = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "XMS06-DN80",
            "standard_number": "HG/T 20592-2009",
        },
    ).json()
    build_service.fusion_source_reader.sources = FusionSources(
        drawing_facts=[
            {
                "fact_key": "product.pressure_class",
                "fact_type": "pressure_class",
                "normalized_value": "PN16",
                "confidence": 0.9,
                "metadata": {},
            },
            {
                "fact_key": "dimension.DN80.D",
                "fact_type": "dimension",
                "symbol": "D",
                "normalized_value": 200.0,
                "unit": "mm",
                "operator": "eq",
                "confidence": 0.85,
                "metadata": {"row_dn": 80},
            },
        ],
        measurements=[],
        features=[],
    )

    response = client.post(f"/api/component-builds/{created['id']}/fusion", json={"overwrite": False})

    assert response.status_code == 200
    assert response.json()["summary"]["filled"] > 0
    spec = client.get(f"/api/component-builds/{created['id']}/component-spec").json()
    assert find_parameter(spec["data"], "DN")["default"] == 80
    assert find_parameter(spec["data"], "flange_outer_diameter")["default"] == 200.0


def test_component_fusion_returns_conflict_without_sources(component_client):
    client, _, _, _, _ = component_client
    created = client.post(
        "/api/component-builds",
        data={
            "category_code": "connection-fastening",
            "part_type_code": "flange",
            "component_name": "空法兰",
        },
    ).json()

    response = client.post(f"/api/component-builds/{created['id']}/fusion", json={})

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "no_sources_available"


def test_step_retry_starts_existing_revision_parse(component_client):
    client, cad_service, _, _, _ = component_client
    build = create_build(client).json()

    response = client.post(f"/api/component-builds/{build['id']}/retry", json={"role": "reference_step"})

    assert response.status_code == 202
    assert response.json()["status"] == "parsing_sources"
    assert cad_service.parses == [cad_service.revision_id, cad_service.revision_id]


def test_query_and_drawing_retry_return_projected_build(component_client):
    client, _, drawing_service, scheduled, _ = component_client
    build = create_build(client).json()

    detail = client.get(f"/api/component-builds/{build['id']}")
    status = client.get(f"/api/component-builds/{build['id']}/status")
    retry = client.post(f"/api/component-builds/{build['id']}/retry", json={"role": "drawing"})

    assert detail.status_code == 200
    assert detail.json()["drawing_task_id"] == str(drawing_service.task_id)
    assert status.status_code == 200
    assert status.json()["sources"]["drawing"]["status"] == "created"
    assert retry.status_code == 202
    assert retry.json()["status"] == "parsing_sources"
    assert scheduled[-1] == (drawing_service.task_id, "flange-001", None)


def test_invalid_extension_is_rejected_before_a_build_is_created(component_client):
    client, _, _, _, build_service = component_client

    response = create_build(client, step_name="XMS06.txt")

    assert response.status_code == 400
    assert build_service.repository.builds == {}


def test_step_upload_failure_marks_build_failed_without_creating_drawing(component_client):
    client, cad_service, drawing_service, _, build_service = component_client

    async def fail_step_upload(_file, _name, **_kwargs):
        raise ValueError("invalid STEP payload")

    cad_service.create_source_from_upload = fail_step_upload

    response = create_build(client)

    build = next(iter(build_service.repository.builds.values()))
    assert response.status_code == 400
    assert build.status == "source_failed"
    assert build.error_code == "component_build_upload_failed"
    assert build.error_message == "invalid STEP payload"
    assert build.cad_revision_id is None
    assert drawing_service.created == []


def test_failure_persistence_error_does_not_mask_step_upload_error(component_client):
    client, cad_service, _, _, build_service = component_client

    async def fail_step_upload(_file, _name, **_kwargs):
        raise ValueError("invalid STEP payload")

    async def fail_failure_persistence(*_args, **_kwargs):
        raise RuntimeError("database is unavailable")

    cad_service.create_source_from_upload = fail_step_upload
    build_service.set_status = fail_failure_persistence

    response = create_build(client)

    assert response.status_code == 400
    assert response.json()["detail"] == {
        "code": "component_build_upload_failed",
        "message": "invalid STEP payload",
    }


def test_drawing_creation_failure_keeps_step_and_marks_build_failed(component_client):
    client, cad_service, drawing_service, _, build_service = component_client

    async def fail_drawing_create(**_kwargs):
        raise DrawingError("drawing_decode_failed", "drawing is invalid")

    drawing_service.create_task = fail_drawing_create

    response = create_build(client)

    build = next(iter(build_service.repository.builds.values()))
    assert response.status_code == 400
    assert build.status == "source_failed"
    assert build.error_code == "drawing_decode_failed"
    assert build.error_message == "drawing is invalid"
    assert build.cad_revision_id == cad_service.revision_id
    assert build.drawing_task_id is None


def test_missing_build_returns_not_found(component_client):
    client, _, _, _, _ = component_client

    response = client.get("/api/component-builds/00000000-0000-0000-0000-000000000000")

    assert response.status_code == 404


class FakeSessionContext:
    async def __aenter__(self):
        return object()

    async def __aexit__(self, _exc_type, _exc, _traceback):
        return False


@pytest.mark.asyncio
async def test_drawing_pipeline_persists_unexpected_layout_failure(monkeypatch, tmp_path):
    task_id = uuid4()
    layout_updates = []

    class Drawing:
        repository = SimpleNamespace(
            update_task_status=lambda *args: layout_updates.append(args),
        )
        extraction_repository = SimpleNamespace()

        async def start_layout(self, _task_id):
            raise RuntimeError("layout crashed")

    drawing = Drawing()

    async def update_task_status(*args):
        layout_updates.append(args)

    drawing.repository.update_task_status = update_task_status
    monkeypatch.setattr("app.component_builds.router.SessionLocal", lambda: FakeSessionContext())
    monkeypatch.setattr("app.component_builds.router.create_drawing_service", lambda _session, _settings: drawing)

    await run_drawing_pipeline(task_id, Settings(cad_spec_work_dir=tmp_path), target_code="xms06", target_dn=80)

    assert layout_updates == [(task_id, "failed", "layout_failed", "layout crashed")]


@pytest.mark.asyncio
async def test_drawing_pipeline_persists_unexpected_extraction_failure_separately(monkeypatch, tmp_path):
    task_id = uuid4()
    layout_updates = []
    extraction_updates = []

    class Drawing:
        repository = SimpleNamespace()
        extraction_repository = SimpleNamespace()

        async def start_layout(self, _task_id):
            return {"status": "layout_ready"}

        async def extract_drawing_facts(self, _task_id, *, target_code, target_dn):
            raise RuntimeError("extraction crashed")

    drawing = Drawing()

    async def update_task_status(*args):
        layout_updates.append(args)

    async def set_status(*args):
        extraction_updates.append(args)

    drawing.repository.update_task_status = update_task_status
    drawing.extraction_repository.set_status = set_status
    monkeypatch.setattr("app.component_builds.router.SessionLocal", lambda: FakeSessionContext())
    monkeypatch.setattr("app.component_builds.router.create_drawing_service", lambda _session, _settings: drawing)

    await run_drawing_pipeline(task_id, Settings(cad_spec_work_dir=tmp_path), target_code="xms06", target_dn=80)

    assert layout_updates == []
    assert extraction_updates == [(task_id, "failed", 100, "failed", "drawing_extraction_failed", "extraction crashed")]
