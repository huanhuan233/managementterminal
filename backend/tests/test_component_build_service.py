import json
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from uuid import UUID, uuid4

import pytest

from app.component_builds.component_spec import component_spec_template
from app.component_builds.component_spec_document import pack_component_spec_document, unpack_component_spec_document
from app.component_builds.repository import MemoryComponentBuildRepository, SqlAlchemyComponentBuildRepository
from app.component_builds.fusion import FusionSourceUnavailable, FusionSources
from app.component_builds.service import ComponentBuildService, SqlAlchemySourceStatusReader
from app.db.models import CadModelRevision, CadSpecTask, ComponentBuild


def find_build_node(nodes: list[dict], build_id: str) -> dict:
    for node in nodes:
        if node.get("node_type") == "build" and node.get("build_id") == build_id:
            return node
        found = find_build_node(node.get("children", []), build_id)
        if found:
            return found
    return {}


class FakeSourceStatusReader:
    async def get_step_status(self, revision_id):
        return {"status": "processing", "progress": 40}

    async def get_drawing_status(self, task_id):
        return {"status": "review_ready"}


class ReadyModelFailedDrawingReader:
    async def get_step_status(self, _revision_id):
        return {
            "status": "completed",
            "status_message": "ready",
            "progress": 100,
            "processing_route": "step_cad_parse",
            "source_format": "STEP",
        }

    async def get_drawing_status(self, _task_id):
        return {"status": "failed", "error_code": "DRAWING_FAILED", "error_message": "图纸解析失败"}


@pytest.mark.asyncio
async def test_ready_model_does_not_hide_failed_drawing_status():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="frame-001",
        component_name="带图纸框架",
        component_type="frame",
        cad_revision_id=uuid4(),
        drawing_task_id=uuid4(),
    )
    service = ComponentBuildService(repository, source_status_reader=ReadyModelFailedDrawingReader())

    response = await service.get_status(build.id)

    assert response["status"] == "source_failed"
    assert response["error_code"] == "DRAWING_FAILED"
    assert response["sources"]["drawing"]["error_code"] == "DRAWING_FAILED"


class FakeFusionSourceReader:
    def __init__(self, sources: FusionSources):
        self.sources = sources
        self.build_ids = []

    async def read(self, build):
        self.build_ids.append(build.id)
        return self.sources


@pytest.mark.asyncio
async def test_get_component_spec_normalizes_legacy_data_and_matching_yaml():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="legacy-001",
        component_name="Legacy",
        component_type="shaft",
    )
    await repository.save_component_spec(build.id, {"identity": {"name": "Legacy"}})
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    response = await service.get_component_spec(build.id)

    assert response["data"]["identity"]["name"] == "Legacy"
    assert response["data"]["schema_version"] == "1.2"
    from app.component_builds.component_spec_document import validate_component_spec_yaml
    assert validate_component_spec_yaml(response["yaml"], response["data"]) == response["data"]


def test_component_build_has_source_links():
    columns = ComponentBuild.__table__.columns

    assert columns["cad_model_id"].nullable is True
    assert columns["cad_revision_id"].nullable is True
    assert columns["drawing_task_id"].nullable is True
    assert columns["component_id"].nullable is False


@pytest.mark.asyncio
async def test_fuse_component_spec_saves_normalized_draft_and_returns_report():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="flange-001",
        component_name="XMS06-DN80",
        component_type="flange",
        family="connection-fastening",
        standard_number="HG/T 20592-2009",
        version="1.0.0",
    )
    reader = FakeFusionSourceReader(
        FusionSources(
            drawing_facts=[
                {
                    "fact_key": "product.component_type_raw",
                    "fact_type": "product_info",
                    "normalized_value": "带颈对焊",
                    "confidence": 0.9,
                    "metadata": {},
                }
            ],
            measurements=[],
            features=[],
        )
    )
    service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=reader,
    )

    response = await service.fuse_component_spec(build.id)

    assert reader.build_ids == [build.id]
    assert response["status"] == "completed"
    assert response["component_spec"]["identity"]["id"] == "flange-001"
    stored = unpack_component_spec_document((await repository.get_component_spec(build.id)).data)
    assert stored.data["identity"]["id"] == "flange-001"
    assert stored.yaml is not None


@pytest.mark.asyncio
async def test_fuse_component_spec_preserves_existing_manual_value():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="flange-001",
        component_name="XMS06-DN80",
        component_type="flange",
    )
    existing = component_spec_template.blank_data()
    existing["identity"]["name"] = "人工名称"
    await repository.save_component_spec(build.id, existing)
    service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=FakeFusionSourceReader(
            FusionSources(
                drawing_facts=[
                    {
                        "fact_key": "product.component_type_raw",
                        "fact_type": "product_info",
                        "normalized_value": "带颈对焊",
                        "confidence": 0.9,
                        "metadata": {},
                    }
                ],
                measurements=[],
                features=[],
            )
        ),
    )

    response = await service.fuse_component_spec(build.id)

    assert response["component_spec"]["identity"]["name"] == "人工名称"


@pytest.mark.asyncio
async def test_fuse_component_spec_reads_envelope_and_preserves_unknown_values():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="flange-001",
        component_name="XMS06-DN80",
        component_type="flange",
    )
    existing = component_spec_template.blank_data()
    existing["identity"]["name"] = "Manual name"
    yaml_text = (
        f"{component_spec_template.render_yaml(existing)}\n"
        "# custom extension\ncustom_extension:\n  curve_policy: all\n"
    )
    existing["custom_extension"] = {"curve_policy": "all"}
    await repository.save_component_spec(
        build.id,
        pack_component_spec_document(existing, yaml_text, "manual.yaml"),
    )
    service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=FakeFusionSourceReader(
            FusionSources(
                drawing_facts=[
                    {
                        "fact_key": "product.component_type_raw",
                        "fact_type": "product_info",
                        "normalized_value": "weld neck",
                        "confidence": 0.9,
                        "metadata": {},
                    }
                ],
                measurements=[],
                features=[],
            )
        ),
    )

    response = await service.fuse_component_spec(build.id)
    stored = unpack_component_spec_document((await repository.get_component_spec(build.id)).data)

    assert response["component_spec"]["identity"]["name"] == "Manual name"


@pytest.mark.asyncio
async def test_viewer_contract_uses_controlled_urls_and_optional_feature_center():
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="frame-001",
        component_name="机械框架",
        component_type="frame",
        standard_number="RIB-03",
        version="03.1",
        cad_revision_id=revision_id,
    )
    repository.get_raw_revision = lambda _revision_id: _async_value(SimpleNamespace(
        id=revision_id,
        status="completed",
        status_message="ready",
        source_file_ext=".catpart",
        error_code=None,
        error_message=None,
        parse_manifest={
            "ingest": {"source_format": "CATPART", "processing_route": "catia_feature_center"},
            "viewer_asset": {
                "glb": "feature-center/lightweight/model.glb",
                "scene_manifest": "feature-center/manifest.json",
                "face_mesh_map": "feature-center/lightweight/face_mesh_map.json",
                "feature_mesh_map": "feature-center/lightweight/feature_mesh_map.json",
                "selection_index": "feature-center/lightweight/selection_index.json",
            },
            "feature_center": {
                "available": True,
                "mapping_available": True,
                "feature_face_mapping_count": 3,
                "canonical_features": "feature-center/canonical_features.jsonl",
                "feature_geometry_links": "feature-center/feature_geometry_links.jsonl",
            },
            "native_semantics": {
                "available": True,
                "features": "native-caa/features.jsonl",
                "feature_topology_links": "native-caa/native_feature_topology_links.jsonl",
                "capabilities": "native-caa/capabilities.json",
            },
        },
    ))
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    contract = await service.get_viewer_contract(build.id)

    assert contract["status"] == "ready"
    assert contract["source_format"] == "CATPART"
    assert contract["summary"]["part_number"] == "RIB-03"
    assert contract["summary"]["part_name"] == "机械框架"
    assert contract["summary"]["version"] == "03.1"
    assert contract["viewer_asset"]["glb_url"].startswith(f"/api/component-builds/{build.id}/viewer/assets/")
    assert "cad-work" not in contract["viewer_asset"]["glb_url"]
    assert contract["viewer_asset"]["selection_index_url"].endswith("feature-center/lightweight/selection_index.json")
    assert contract["feature_center"]["available"] is True
    assert contract["feature_center"]["mapping_available"] is True
    assert contract["native_semantics"]["feature_topology_links_url"].endswith(
        "native-caa/native_feature_topology_links.jsonl"
    )
    assert contract["native_semantics"]["capabilities_url"].endswith("native-caa/capabilities.json")
    assert contract["summary"]["feature_face_mapping_available"] is True


@pytest.mark.asyncio
async def test_viewer_contract_builds_catproduct_bom_from_native_product_instances(tmp_path):
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="assy-001",
        component_name="CATProduct Assy",
        component_type="assembly",
        cad_revision_id=revision_id,
    )
    native_dir = tmp_path / str(revision_id) / "native-caa"
    native_dir.mkdir(parents=True)
    records = [
        {
            "instance_id": "PRDINS_000001",
            "parent_instance_id": "",
            "instance_name": "RootProduct",
            "reference_id": "PRDREF_ROOT",
            "instance_path": "RootProduct",
            "depth": 0,
            "child_index": 0,
            "child_count": 2,
            "transform_4x4": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
        },
        {
            "instance_id": "PRDINS_000002",
            "parent_instance_id": "PRDINS_000001",
            "instance_name": "Bracket.1",
            "reference_id": "PRDREF_BRACKET",
            "instance_path": "RootProduct/Bracket.1",
            "depth": 1,
            "child_index": 1,
            "child_count": 0,
        },
        {
            "instance_id": "PRDINS_000003",
            "parent_instance_id": "PRDINS_000001",
            "instance_name": "Bolt.1",
            "reference_id": "PRDREF_BOLT",
            "instance_path": "RootProduct/Bolt.1",
            "depth": 1,
            "child_index": 2,
            "child_count": 0,
        },
    ]
    with (native_dir / "product_instances.jsonl").open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record) + "\n")
    repository.get_raw_revision = lambda _revision_id: _async_value(SimpleNamespace(
        id=revision_id,
        status="completed",
        status_message="ready",
        progress=100,
        source_file_ext=".catproduct",
        source_file_name="RootProduct.CATProduct",
        source_file_path=str(tmp_path / str(revision_id) / "source.CATProduct"),
        error_code=None,
        error_message=None,
        parse_manifest={
            "ingest": {"source_format": "CATPRODUCT", "processing_route": "catia_feature_center"},
            "viewer_asset": {
                "glb": "feature-center/lightweight/model.glb",
                "scene_manifest": "feature-center/manifest.json",
                "face_mesh_map": "feature-center/lightweight/face_mesh_map.json",
                "feature_mesh_map": "feature-center/lightweight/feature_mesh_map.json",
            },
            "native_semantics": {
                "available": True,
                "product_instances": "native-caa/product_instances.jsonl",
            },
        },
    ))
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    contract = await service.get_viewer_contract(build.id)

    assert contract["bom"]["assembly_mode"] == "assembly"
    assert contract["bom"]["part_count"] == 2
    root = contract["bom"]["nodes"][0]
    assert root["name"] == "RootProduct"
    assert [child["name"] for child in root["children"]] == ["Bracket.1", "Bolt.1"]
    assert root["children"][0]["assembly_path"] == "RootProduct/Bracket.1"


@pytest.mark.asyncio
async def test_viewer_contract_reports_empty_catproduct_geometry():
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="assy-empty-001",
        component_name="Empty Geometry Assy",
        component_type="assembly",
        cad_revision_id=revision_id,
    )
    repository.get_raw_revision = lambda _revision_id: _async_value(SimpleNamespace(
        id=revision_id,
        status="completed",
        status_message="ready",
        progress=100,
        source_file_ext=".catproduct",
        source_file_name="RootProduct.CATProduct",
        error_code=None,
        error_message=None,
        parse_manifest={
            "ingest": {"source_format": "CATPRODUCT", "processing_route": "catia_feature_center"},
            "viewer_asset": {
                "glb": "feature-center/lightweight/model.glb",
                "scene_manifest": "feature-center/manifest.json",
                "face_mesh_map": "feature-center/lightweight/face_mesh_map.json",
                "feature_mesh_map": "feature-center/lightweight/feature_mesh_map.json",
            },
            "viewer_summary": {"solid_count": 0},
            "feature_center_manifest": {
                "lightweight": {"primitive_count": 0, "triangle_count": 0},
            },
        },
    ))
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    contract = await service.get_viewer_contract(build.id)

    assert contract["viewer_geometry"]["displayable"] is False
    assert contract["viewer_geometry"]["triangle_count"] == 0
    assert contract["viewer_geometry"]["empty_reason"] == "catproduct_missing_loaded_representations"


async def _async_value(value):
    return value
    assert response["component_spec"]["custom_extension"] == {"curve_policy": "all"}
    assert stored.data["custom_extension"] == {"curve_policy": "all"}
    assert "custom_extension:" in stored.yaml


@pytest.mark.asyncio
async def test_fuse_component_spec_preserves_uploaded_schema_version():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="future-001",
        component_name="Future",
        component_type="shaft",
    )
    existing = component_spec_template.blank_data()
    existing["schema_version"] = "1.3"
    await repository.save_component_spec(
        build.id,
        pack_component_spec_document(existing, "schema_version: '1.3'\n", "future.yaml"),
    )
    service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=FakeFusionSourceReader(
            FusionSources(
                drawing_facts=[{
                    "fact_key": "product.component_type_raw",
                    "fact_type": "product_info",
                    "normalized_value": "shaft",
                    "confidence": 0.9,
                    "metadata": {},
                }],
                measurements=[],
                features=[],
            )
        ),
    )

    response = await service.fuse_component_spec(build.id)

    assert response["component_spec"]["schema_version"] == "1.3"


@pytest.mark.asyncio
async def test_fuse_component_spec_rejects_build_without_available_sources():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="flange-001",
        component_name="XMS06-DN80",
        component_type="flange",
    )
    service = ComponentBuildService(
        repository,
        source_status_reader=FakeSourceStatusReader(),
        fusion_source_reader=FakeFusionSourceReader(FusionSources([], [], [])),
    )

    with pytest.raises(FusionSourceUnavailable, match="no_sources_available"):
        await service.fuse_component_spec(build.id)


@pytest.mark.asyncio
async def test_status_projects_linked_source_states():
    model_id = uuid4()
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository(revision_models={revision_id: model_id})
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    status = await service.get_status(build.id)

    assert status["status"] == "parsing_sources"
    assert status["sources"]["reference_step"]["status"] == "processing"
    assert status["sources"]["drawing"]["status"] == "missing"


@pytest.mark.asyncio
async def test_status_prioritizes_failed_sources_and_manual_layout_review():
    model_id = uuid4()
    revision_id = uuid4()
    task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={task_id: revision_id},
    )
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=task_id)

    class FailedStepStatusReader:
        async def get_step_status(self, revision_id):
            return {"status": "failed"}

        async def get_drawing_status(self, task_id):
            return {"status": "needs_manual_layout"}

    status = await ComponentBuildService(repository, source_status_reader=FailedStepStatusReader()).get_status(build.id)

    assert status["status"] == "source_failed"


@pytest.mark.asyncio
async def test_tree_detail_and_status_share_the_same_projected_build_status():
    model_id = uuid4()
    revision_id = uuid4()
    task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={task_id: revision_id},
    )
    build = await repository.create_build(
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        status="parsing_sources",
    )
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=task_id)

    class ReadySourceStatusReader:
        async def get_step_status(self, _revision_id):
            return {"status": "completed", "progress": 100}

        async def get_drawing_status(self, _task_id):
            return {"status": "review_ready", "progress": 100}

    service = ComponentBuildService(repository, source_status_reader=ReadySourceStatusReader())

    tree = await service.get_tree()
    detail = await service.get_build(build.id)
    source_status = await service.get_status(build.id)

    assert find_build_node(tree, str(build.id))["status"] == "sources_ready"
    assert detail["status"] == "sources_ready"
    assert source_status["status"] == "sources_ready"


@pytest.mark.asyncio
async def test_unlinked_build_retains_persisted_upload_failure_status():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        status="source_failed",
    )
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    detail = await service.get_build(build.id)
    status = await service.get_status(build.id)

    assert detail["status"] == "source_failed"
    assert status["status"] == "source_failed"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("step_status", "drawing_status"),
    [("completed", "review_ready"), ("processing", "needs_manual_layout")],
)
async def test_persisted_source_failure_overrides_ready_and_manual_source_states(step_status, drawing_status):
    model_id = uuid4()
    revision_id = uuid4()
    task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={task_id: revision_id},
    )
    build = await repository.create_build(
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        status="source_failed",
    )
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=task_id)
    class SourceStatusReader:
        async def get_step_status(self, _revision_id):
            return {"status": step_status, "progress": 100}

        async def get_drawing_status(self, _task_id):
            return {"status": drawing_status, "progress": 100}

    service = ComponentBuildService(repository, source_status_reader=SourceStatusReader())

    tree = await service.get_tree()
    detail = await service.get_build(build.id)
    status = await service.get_status(build.id)

    assert find_build_node(tree, str(build.id))["status"] == "source_failed"
    assert detail["status"] == "source_failed"
    assert status["status"] == "source_failed"


@pytest.mark.asyncio
async def test_source_status_reader_projects_source_errors_when_available():
    revision = SimpleNamespace(
        status="failed",
        progress=100,
        status_message="parser failed",
        error_code="freecad_failed",
        error_message="FreeCAD exited",
    )
    task = SimpleNamespace(status="failed", progress=100)

    class Session:
        async def get(self, model, _identifier):
            return revision if model is CadModelRevision else task

    reader = SqlAlchemySourceStatusReader(Session())

    step = await reader.get_step_status(uuid4())
    drawing = await reader.get_drawing_status(uuid4())

    assert step["status_message"] == "parser failed"
    assert step["error_code"] == "freecad_failed"
    assert step["error_message"] == "FreeCAD exited"
    assert drawing["status_message"] is None
    assert drawing["error_code"] is None
    assert drawing["error_message"] is None


@pytest.mark.asyncio
async def test_tree_keeps_fusion_disabled_until_a_source_is_attached():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")

    tree = await ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader()).get_tree()

    version_node = find_build_node(tree, str(build.id))
    assert version_node["build_id"] == str(build.id)
    assert [child["node_type"] for child in version_node["children"]] == [
        "folder", "data_fusion", "component_spec", "publish_validation"
    ]
    fusion, component_spec, publish = version_node["children"][1:]
    assert fusion["disabled"] is True and fusion["status"] == "pending"
    assert component_spec["disabled"] is False and component_spec["status"] == "draft"
    assert publish["disabled"] is True and publish["status"] == "future"
    assert fusion["status_label"] == "待上传来源"
    assert component_spec["status_label"] == "待填写"
    assert publish["status_label"] == "后续能力"


@pytest.mark.asyncio
async def test_tree_enables_fusion_when_build_has_a_source_and_marks_saved_draft_completed():
    model_id = uuid4()
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository(revision_models={revision_id: model_id})
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    ready_tree = await service.get_tree()
    ready_fusion = find_build_node(ready_tree, str(build.id))["children"][1]

    assert ready_fusion["id"] == f"{build.id}:fusion"
    assert ready_fusion["build_id"] == str(build.id)
    assert ready_fusion["disabled"] is False
    assert ready_fusion["status"] == "ready"
    assert ready_fusion["status_label"] == "可开始"

    await repository.save_component_spec(build.id, component_spec_template.blank_data())
    completed_tree = await service.get_tree()
    completed_fusion = find_build_node(completed_tree, str(build.id))["children"][1]

    assert completed_fusion["disabled"] is False
    assert completed_fusion["status"] == "completed"
    assert completed_fusion["status_label"] == "已生成草稿"


@pytest.mark.asyncio
async def test_get_build_projects_identity_links_and_timestamps():
    model_id = uuid4()
    revision_id = uuid4()
    drawing_task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={drawing_task_id: revision_id},
    )
    build = await repository.create_build(
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        default_dn=80,
        default_pn=16,
        error_code="source_unavailable",
        error_message="STEP parser did not respond",
    )
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=drawing_task_id)
    service = ComponentBuildService(repository, source_status_reader=FakeSourceStatusReader())

    result = await service.get_build(build.id)

    assert result["component_id"] == "xms06"
    assert result["default_dn"] == 80
    assert result["cad_model_id"] == str(model_id)
    assert result["cad_revision_id"] == str(revision_id)
    assert result["drawing_task_id"] == str(drawing_task_id)
    assert result["error_code"] == "source_unavailable"
    assert result["error_message"] == "STEP parser did not respond"
    assert result["created_at"] is not None
    assert result["updated_at"] is not None


@pytest.mark.asyncio
async def test_memory_repository_accepts_explicit_component_version():
    repository = MemoryComponentBuildRepository()

    build = await repository.create_build(
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        version="2.1.0",
    )

    assert build.version == "2.1.0"


@pytest.mark.asyncio
async def test_memory_repository_defaults_component_version():
    repository = MemoryComponentBuildRepository()

    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")

    assert build.version == "1.0.0"


@pytest.mark.asyncio
async def test_unconfigured_memory_repository_accepts_trusted_source_ids():
    repository = MemoryComponentBuildRepository()
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    model_id = uuid4()
    revision_id = uuid4()
    task_id = uuid4()

    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=task_id)

    assert build.cad_model_id == model_id
    assert build.cad_revision_id == revision_id
    assert build.drawing_task_id == task_id


@pytest.mark.asyncio
async def test_memory_repository_clears_drawing_when_step_revision_is_replaced():
    model_id = uuid4()
    revision_a = uuid4()
    revision_b = uuid4()
    drawing_task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_a: model_id, revision_b: model_id},
        drawing_task_revisions={drawing_task_id: revision_a},
    )
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_a)
    await repository.attach_drawing(build.id, task_id=drawing_task_id)

    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_b)

    assert build.cad_revision_id == revision_b
    assert build.drawing_task_id is None


@pytest.mark.asyncio
async def test_memory_repository_preserves_drawing_when_step_revision_is_reattached():
    model_id = uuid4()
    revision_id = uuid4()
    drawing_task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={drawing_task_id: revision_id},
    )
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)
    await repository.attach_drawing(build.id, task_id=drawing_task_id)

    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)

    assert build.drawing_task_id == drawing_task_id


@pytest.mark.asyncio
async def test_memory_repository_lists_newest_builds_first_with_uuid_tie_breaker():
    repository = MemoryComponentBuildRepository()
    timestamp = datetime(2026, 7, 23, tzinfo=timezone.utc)
    oldest = await repository.create_build(
        id=UUID(int=1),
        component_id="old",
        component_name="Old",
        component_type="flange",
        created_at=timestamp - timedelta(seconds=1),
    )
    tied_lower = await repository.create_build(
        id=UUID(int=2),
        component_id="low",
        component_name="Low",
        component_type="flange",
        created_at=timestamp,
    )
    tied_higher = await repository.create_build(
        id=UUID(int=3),
        component_id="high",
        component_name="High",
        component_type="flange",
        created_at=timestamp,
    )

    builds = await repository.list_builds()

    assert [build.id for build in builds] == [tied_higher.id, tied_lower.id, oldest.id]


@pytest.mark.asyncio
async def test_memory_repository_rejects_step_revision_from_another_model():
    model_id = uuid4()
    revision_id = uuid4()
    repository = MemoryComponentBuildRepository(revision_models={revision_id: uuid4()})
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")

    with pytest.raises(ValueError, match="revision does not belong to model"):
        await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)


@pytest.mark.asyncio
async def test_memory_repository_rejects_drawing_task_from_another_revision():
    model_id = uuid4()
    revision_id = uuid4()
    task_id = uuid4()
    repository = MemoryComponentBuildRepository(
        revision_models={revision_id: model_id},
        drawing_task_revisions={task_id: uuid4()},
    )
    build = await repository.create_build(component_id="xms06", component_name="XMS06", component_type="flange")
    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)

    with pytest.raises(ValueError, match="drawing task does not belong to build revision"):
        await repository.attach_drawing(build.id, task_id=task_id)


class SourceLookupSession:
    def __init__(self, build, revision, task):
        self.build = build
        self.revision = revision
        self.task = task

    async def get(self, model, _identifier):
        if model is ComponentBuild:
            return self.build
        if model is CadModelRevision:
            return self.revision
        if model is CadSpecTask:
            return self.task
        return None

    async def commit(self):
        return None

    async def refresh(self, _build):
        return None


@pytest.mark.asyncio
async def test_sqlalchemy_repository_rejects_step_revision_from_another_model():
    build = ComponentBuild(id=uuid4(), component_id="xms06", component_name="XMS06", component_type="flange")
    session = SourceLookupSession(build, SimpleNamespace(model_id=uuid4()), None)
    repository = SqlAlchemyComponentBuildRepository(session)

    with pytest.raises(ValueError, match="revision does not belong to model"):
        await repository.attach_step(build.id, model_id=uuid4(), revision_id=uuid4())


@pytest.mark.asyncio
async def test_sqlalchemy_repository_rejects_drawing_task_from_another_revision():
    revision_id = uuid4()
    build = ComponentBuild(id=uuid4(), component_id="xms06", component_name="XMS06", component_type="flange", cad_revision_id=revision_id)
    session = SourceLookupSession(build, None, SimpleNamespace(revision_id=uuid4()))
    repository = SqlAlchemyComponentBuildRepository(session)

    with pytest.raises(ValueError, match="drawing task does not belong to build revision"):
        await repository.attach_drawing(build.id, task_id=uuid4())


@pytest.mark.asyncio
async def test_sqlalchemy_repository_clears_drawing_when_step_revision_is_replaced():
    model_id = uuid4()
    revision_a = uuid4()
    revision_b = uuid4()
    drawing_task_id = uuid4()
    build = ComponentBuild(
        id=uuid4(),
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        cad_model_id=model_id,
        cad_revision_id=revision_a,
        drawing_task_id=drawing_task_id,
    )
    session = SourceLookupSession(build, SimpleNamespace(model_id=model_id), None)
    repository = SqlAlchemyComponentBuildRepository(session)

    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_b)

    assert build.cad_revision_id == revision_b
    assert build.drawing_task_id is None


@pytest.mark.asyncio
async def test_sqlalchemy_repository_preserves_drawing_when_step_revision_is_reattached():
    model_id = uuid4()
    revision_id = uuid4()
    drawing_task_id = uuid4()
    build = ComponentBuild(
        id=uuid4(),
        component_id="xms06",
        component_name="XMS06",
        component_type="flange",
        cad_model_id=model_id,
        cad_revision_id=revision_id,
        drawing_task_id=drawing_task_id,
    )
    session = SourceLookupSession(build, SimpleNamespace(model_id=model_id), None)
    repository = SqlAlchemyComponentBuildRepository(session)

    await repository.attach_step(build.id, model_id=model_id, revision_id=revision_id)

    assert build.drawing_task_id == drawing_task_id
