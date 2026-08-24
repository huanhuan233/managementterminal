from uuid import uuid4

import pytest

from app.component_builds.catalog import CATEGORIES, LIBRARIES, catalog_payload
from app.component_builds.repository import MemoryComponentBuildRepository, SqlAlchemyComponentBuildRepository
from app.component_builds.service import ComponentBuildService


class SourceStatusReader:
    async def get_step_status(self, _revision_id):
        return {"status": "completed", "progress": 100}

    async def get_drawing_status(self, _task_id):
        return {"status": "review_ready", "progress": 100}


def _find(nodes: list[dict], *, node_type: str, code: str) -> dict:
    for node in nodes:
        if node["node_type"] == node_type and (node.get("category_code") == code or node.get("part_type_code") == code):
            return node
        found = _find(node.get("children", []), node_type=node_type, code=code)
        if found:
            return found
    return {}


def test_catalog_defines_all_six_categories_and_required_parts():
    assert [category.code for category in CATEGORIES] == [
        "support-frame",
        "shaft-transmission",
        "roller",
        "connection-fastening",
        "drive-actuation",
        "functional",
    ]
    assert {part.code for category in CATEGORIES for part in category.parts} == {
        "frame", "base", "housing", "bracket", "column", "beam", "support-ring", "support-plate",
        "shaft", "bearing", "bearing-housing", "gear", "pulley", "sprocket", "coupling", "lead-screw",
        "traction-roller", "guide-roller", "press-roller", "conveyor-roller", "idler-roller", "paddle-roller",
        "bolt-joint", "flange", "hinge", "key", "pin", "clamp", "sleeve", "retaining-ring",
        "motor", "reducer", "cylinder", "hydraulic-cylinder", "spring",
        "hopper", "agitator", "cutter", "die", "screen", "guide-rail", "slider", "air-ring", "cooling-unit",
        "barrel-body", "cone-body",
    }


def test_catalog_defines_single_mechanical_library_root():
    assert [library.code for library in LIBRARIES] == [
        "MECHANICAL_COMPONENT_LIBRARY",
    ]
    assert [library.label for library in LIBRARIES] == ["机械工程图元库"]
    assert LIBRARIES[0].categories == CATEGORIES
    assert str(CATEGORIES[0].catalog_node_id) == "5f3b58f8-6b36-5372-ad97-7612b368bbce"
    assert str(CATEGORIES[0].parts[0].catalog_node_id) == "68652018-674f-548e-aa1c-62e5d56c8501"


def test_catalog_payload_exposes_library_roots_without_duplicating_categories():
    payload = catalog_payload()

    assert len(payload["libraries"]) == 1
    assert payload["libraries"][0]["library_code"] == "MECHANICAL_COMPONENT_LIBRARY"
    assert len(payload["libraries"][0]["categories"]) == len(CATEGORIES)
    assert len({item["category_code"] for item in payload["categories"]}) == len(payload["categories"])


@pytest.mark.asyncio
async def test_service_generates_part_prefixed_ids_and_catalog_payloads():
    service = ComponentBuildService(MemoryComponentBuildRepository(), source_status_reader=SourceStatusReader())

    first = await service.create_catalog_build(
        category_code="connection-fastening",
        part_type_code="flange",
        component_name="带颈对焊法兰",
    )
    second = await service.create_catalog_build(
        category_code="connection-fastening",
        part_type_code="flange",
        component_name="平焊法兰",
    )

    assert first["component_id"] == "flange-001"
    assert second["component_id"] == "flange-002"
    assert first["family"] == "connection-fastening"
    assert first["component_type"] == "flange"
    assert first["component_subtype"] is None
    assert first["default_dn"] is None
    assert first["default_pn"] is None
    assert first["catalog_node_id"]
    assert first["catalog_path"] == "/连接与紧固类/法兰"


@pytest.mark.asyncio
async def test_memory_and_sql_repositories_use_the_same_next_id_rule():
    memory = MemoryComponentBuildRepository()
    await memory.create_build(component_id="flange-001", component_name="旧件", component_type="flange")
    await memory.create_build(component_id="flange-009", component_name="旧件", component_type="flange")

    class Scalars:
        def all(self):
            return ["flange-001", "flange-009", "frame-002"]

    class Result:
        def scalars(self):
            return Scalars()

    class Session:
        async def execute(self, _statement):
            return Result()

    sql = SqlAlchemyComponentBuildRepository(Session())

    assert await memory.next_component_id("flange") == "flange-010"
    assert await sql.next_component_id("flange") == "flange-010"


@pytest.mark.asyncio
async def test_tree_keeps_empty_catalog_groups_and_preserves_legacy_builds():
    repository = MemoryComponentBuildRepository()
    flange = await repository.create_build(component_id="legacy-flange", component_name="旧法兰", component_type="flange")
    chinese_flange = await repository.create_build(
        component_id="legacy-flange-cn",
        component_name="中文旧法兰",
        component_type="法兰",
    )
    unknown = await repository.create_build(component_id="legacy-other", component_name="旧未知件", component_type="legacy-thing")
    tree = await ComponentBuildService(repository, source_status_reader=SourceStatusReader()).get_tree()

    assert [node["library_code"] for node in tree[:1]] == ["MECHANICAL_COMPONENT_LIBRARY"]
    mechanical_categories = tree[0]["children"]
    assert [node["category_code"] for node in mechanical_categories[:6]] == [
        "support-frame", "shaft-transmission", "roller", "connection-fastening", "drive-actuation", "functional",
    ]
    flange_part = _find(tree, node_type="type", code="flange")
    assert flange_part["label_en"] == "Flange"
    assert {node["component_id"] for node in flange_part["children"]} == {
        flange.component_id,
        chinese_flange.component_id,
    }
    assert all(node["node_type"] == "component" for node in flange_part["children"])
    assert all(node["children"][0]["node_type"] == "build" for node in flange_part["children"])
    uncategorized = _find(tree, node_type="family", code="uncategorized")
    assert uncategorized["children"][0]["children"][0]["component_id"] == unknown.component_id
    assert tree[0]["count"] == 3
    assert _find(tree, node_type="family", code="connection-fastening")["count"] == 2
    assert flange_part["count"] == 2
    assert uncategorized["count"] == 1
    assert _find(tree, node_type="type", code="motor")["children"] == []
