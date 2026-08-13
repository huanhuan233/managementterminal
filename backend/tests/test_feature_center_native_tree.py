import json
from pathlib import Path
from typing import List

import pytest

from app.feature_center.native_tree import (
    NativeTreeJsonError,
    build_native_tree_from_bundle,
    write_jsonl,
)


def _write(path: Path, name: str, records: List[dict]) -> None:
    write_jsonl(path / name, records)


def test_catpart_tree_preserves_chinese_unknown_nodes_and_source_order(tmp_path: Path) -> None:
    _write(tmp_path, "native_features.jsonl", [
        {"native_feature_id": "F2", "parent_feature_id": "F1", "source_index": 2, "name": "未知节点"},
        {"native_feature_id": "F1", "source_index": 1, "name": "零件", "startup_type": "MechanicalPart"},
    ])

    result = build_native_tree_from_bundle(tmp_path, "零件.CATPart")

    assert result.nodes[0]["document_kind"] == "catpart"
    assert result.nodes[0]["children"][0]["feature_id"] == "F1"
    assert result.nodes[0]["children"][0]["children"][0]["label"] == "未知节点"
    assert result.nodes[0]["children"][0]["children"][0]["node_kind"] == "unknown"


def test_catproduct_repeated_instances_are_not_deduplicated(tmp_path: Path) -> None:
    _write(tmp_path, "product_references.jsonl", [{"reference_id": "R1", "part_number": "P1"}])
    _write(tmp_path, "product_instances.jsonl", [
        {"instance_id": "I0", "reference_id": "ASM", "instance_name": "Root", "child_count": 2, "depth": 0},
        {"instance_id": "I1", "parent_instance_id": "I0", "reference_id": "R1", "instance_name": "Part.1", "depth": 1},
        {"instance_id": "I2", "parent_instance_id": "I0", "reference_id": "R1", "instance_name": "Part.2", "depth": 1},
    ])
    _write(tmp_path, "native_features.jsonl", [
        {"native_feature_id": "F10", "source_index": 1, "name": "Hole.1", "startup_type": "Hole"}
    ])

    result = build_native_tree_from_bundle(tmp_path, "Asm.CATProduct")
    flat = _flatten(result.nodes)

    assert "instance:I1" in {node["id"] for node in flat}
    assert "instance:I2" in {node["id"] for node in flat}
    assert "instance:I1/feature:F10" in {node["id"] for node in flat}
    assert "instance:I2/feature:F10" in {node["id"] for node in flat}


def test_multiple_references_without_mapping_show_bom_and_diagnostic(tmp_path: Path) -> None:
    _write(tmp_path, "product_references.jsonl", [{"reference_id": "R1"}, {"reference_id": "R2"}])
    _write(tmp_path, "product_instances.jsonl", [
        {"instance_id": "I1", "reference_id": "R1", "instance_name": "A"},
        {"instance_id": "I2", "reference_id": "R2", "instance_name": "B"},
    ])
    _write(tmp_path, "native_features.jsonl", [{"native_feature_id": "F10", "name": "Hole.1", "startup_type": "Hole"}])

    result = build_native_tree_from_bundle(tmp_path, "Asm.CATProduct")

    assert [node["id"] for node in _flatten(result.nodes) if "/feature:" in node["id"]] == []
    assert result.diagnostics[0]["diagnostic_id"] == "NATIVE_TREE_REFERENCE_MAPPING_UNRESOLVED"


def test_missing_optional_jsonl_still_returns_tree(tmp_path: Path) -> None:
    result = build_native_tree_from_bundle(tmp_path, "empty.CATPart")

    assert result.nodes[0]["label"] == "empty.CATPart"
    assert result.nodes[0]["children"] == []


def test_feature_topology_and_mesh_selection_are_attached(tmp_path: Path) -> None:
    _write(tmp_path, "native_features.jsonl", [{"native_feature_id": "F1", "name": "Hole.1", "startup_type": "Hole"}])
    _write(tmp_path, "native_feature_topology_links.jsonl", [{"native_feature_id": "F1", "cell_id": "TopoFace_12"}])
    _write(tmp_path, "native_mesh_face_map.jsonl", [{"cell_id": "TopoFace_12", "mesh_face_id": "Face_12"}])

    node = build_native_tree_from_bundle(tmp_path).nodes[0]["children"][0]

    assert node["has_geometry"] is True
    assert node["selection"] == {"mesh_face_ids": ["Face_12"], "topology_ids": ["TopoFace_12"]}


def test_node_properties_normalize_tabs_and_mark_nodes(tmp_path: Path) -> None:
    _write(tmp_path, "native_features.jsonl", [{"native_feature_id": "F1", "name": "PartName", "startup_type": "String"}])
    _write(tmp_path, "node_properties.jsonl", [{
        "node_id": "feature:F1",
        "tab_id": "product",
        "tab_label": "产品",
        "group_id": "identity",
        "group_label": "常规",
        "field_key": "PartName",
        "field_label": "PartName",
        "value": "泵体",
    }])

    result = build_native_tree_from_bundle(tmp_path)

    assert result.nodes[0]["children"][0]["has_properties"] is True
    assert result.properties[0]["tab_label"] == "产品"
    assert result.properties[0]["read_only"] is True


def test_empty_properties_do_not_mark_node(tmp_path: Path) -> None:
    _write(tmp_path, "native_features.jsonl", [{"native_feature_id": "F1", "name": "Hole.1"}])

    result = build_native_tree_from_bundle(tmp_path)

    assert result.properties == []
    assert result.nodes[0]["children"][0]["has_properties"] is False


def test_invalid_jsonl_reports_file_and_line(tmp_path: Path) -> None:
    (tmp_path / "native_features.jsonl").write_text('{"native_feature_id":"F1"}\n{bad\n', encoding="utf-8")

    with pytest.raises(NativeTreeJsonError, match="native_features.jsonl:2"):
        build_native_tree_from_bundle(tmp_path)


def test_preferred_native_tree_nodes_are_nested_and_use_source_feature_id(tmp_path: Path) -> None:
    _write(tmp_path, "native_tree_nodes.jsonl", [
        {"node_id": "feature:F1", "display_text": "PartBody", "source_index": 1},
        {
            "node_id": "feature:F2",
            "parent_id": "feature:F1",
            "display_text": "Hole.1",
            "source_feature_id": "F2",
            "properties_available": True,
            "selection": {"topology_ids": ["TopoFace_1"], "mesh_face_ids": ["MeshFace_1"]},
            "source_index": 2,
        },
    ])

    result = build_native_tree_from_bundle(tmp_path)

    child = result.nodes[0]["children"][0]
    assert child["feature_id"] == "F2"
    assert child["has_properties"] is True
    assert child["selection"]["mesh_face_ids"] == ["MeshFace_1"]


def test_preferred_native_tree_missing_parent_reports_diagnostic(tmp_path: Path) -> None:
    _write(tmp_path, "native_tree_nodes.jsonl", [
        {"node_id": "feature:F2", "parent_id": "feature:missing", "display_text": "孤立节点"},
    ])

    result = build_native_tree_from_bundle(tmp_path)

    assert result.nodes[0]["id"] == "feature:F2"
    assert result.diagnostics[0]["diagnostic_id"] == "NATIVE_TREE_PARENT_MISSING"


def test_preferred_native_tree_parent_cycle_reports_diagnostic(tmp_path: Path) -> None:
    _write(tmp_path, "native_tree_nodes.jsonl", [
        {"node_id": "feature:F1", "parent_id": "feature:F2", "display_text": "A"},
        {"node_id": "feature:F2", "parent_id": "feature:F1", "display_text": "B"},
    ])

    result = build_native_tree_from_bundle(tmp_path)

    assert {node["id"] for node in result.nodes} == {"feature:F1", "feature:F2"}
    assert any(item["diagnostic_id"] == "NATIVE_TREE_PARENT_CYCLE" for item in result.diagnostics)


def _flatten(nodes: List[dict]) -> List[dict]:
    result = []
    for node in nodes:
        result.append(node)
        result.extend(_flatten(node.get("children") or []))
    return result
