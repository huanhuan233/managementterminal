"""Build a unified CATProduct/CATPart native tree from CAA sidecar JSONL files."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


class NativeTreeJsonError(ValueError):
    def __init__(self, file_name: str, line_number: int, message: str) -> None:
        super().__init__(f"NATIVE_TREE_JSON_INVALID:{file_name}:{line_number}:{message}")
        self.file_name = file_name
        self.line_number = line_number


@dataclass
class NativeTreeBuildResult:
    nodes: list[dict[str, Any]]
    properties: list[dict[str, Any]] = field(default_factory=list)
    diagnostics: list[dict[str, Any]] = field(default_factory=list)


def read_optional_jsonl(bundle_dir: Path, file_name: str) -> list[dict[str, Any]]:
    path = bundle_dir / file_name
    if not path.is_file():
        return []
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise NativeTreeJsonError(file_name, line_number, exc.msg) from exc
            if isinstance(record, dict):
                records.append(record)
    return records


def write_jsonl(path: Path, records: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
            stream.write("\n")


def build_native_tree_from_bundle(bundle_dir: Path, source_file_name: str = "") -> NativeTreeBuildResult:
    preferred = read_optional_jsonl(bundle_dir, "native_tree_nodes.jsonl")
    properties = _normalize_properties(read_optional_jsonl(bundle_dir, "node_properties.jsonl"))
    if preferred:
        diagnostics: list[dict[str, Any]] = []
        roots = _nest_flat_nodes([_normalize_preferred_node(item) for item in preferred], diagnostics)
        _mark_property_state(roots, properties)
        return NativeTreeBuildResult(nodes=roots, properties=properties, diagnostics=diagnostics)

    product_instances = read_optional_jsonl(bundle_dir, "product_instances.jsonl")
    product_references = read_optional_jsonl(bundle_dir, "product_references.jsonl")
    native_features = read_optional_jsonl(bundle_dir, "native_features.jsonl") or read_optional_jsonl(bundle_dir, "features.jsonl")
    feature_topology_links = read_optional_jsonl(bundle_dir, "native_feature_topology_links.jsonl")
    mesh_face_map = read_optional_jsonl(bundle_dir, "native_mesh_face_map.jsonl")
    parameters = read_optional_jsonl(bundle_dir, "parameters.jsonl")
    business_features = read_optional_jsonl(bundle_dir, "business_features.jsonl")

    topology_by_feature = _topology_ids_by_feature(feature_topology_links)
    mesh_faces_by_topology = _mesh_faces_by_topology(mesh_face_map)
    fallback_properties = _fallback_properties(parameters, business_features)
    properties = properties or fallback_properties

    diagnostics: list[dict[str, Any]] = []
    feature_roots = _build_feature_nodes(native_features, topology_by_feature, mesh_faces_by_topology)
    if product_instances:
        roots = _build_product_tree(
            product_instances,
            product_references,
            feature_roots,
            diagnostics,
            source_file_name,
        )
    else:
        root_id = "document:catpart"
        roots = [_node(
            id=root_id,
            parent_id=None,
            label=Path(source_file_name).name or "CATPart",
            node_kind="document",
            document_kind="catpart",
            source_index=0,
            tree_path=f"/{Path(source_file_name).name}" if source_file_name else "/CATPart",
            children=_clone_feature_tree(feature_roots, root_id, "", ""),
        )]
    _mark_property_state(roots, properties)
    return NativeTreeBuildResult(nodes=roots, properties=properties, diagnostics=diagnostics)


def _node(**values: Any) -> dict[str, Any]:
    selection = values.pop("selection", None) or {"mesh_face_ids": [], "topology_ids": []}
    return {
        "id": values.get("id"),
        "parent_id": values.get("parent_id"),
        "label": values.get("label") or values.get("internal_name") or values.get("id"),
        "node_kind": values.get("node_kind", "unknown"),
        "document_kind": values.get("document_kind", "unknown"),
        "source_index": int(values.get("source_index") or 0),
        "instance_id": values.get("instance_id"),
        "reference_id": values.get("reference_id"),
        "feature_id": values.get("feature_id"),
        "topology_id": values.get("topology_id"),
        "startup_type": values.get("startup_type"),
        "internal_name": values.get("internal_name"),
        "tree_path": values.get("tree_path") or "",
        "has_geometry": bool(selection.get("mesh_face_ids") or selection.get("topology_ids")),
        "has_properties": bool(values.get("has_properties", False)),
        "selection": selection,
        "source_node_id": values.get("source_node_id") or values.get("feature_id") or values.get("instance_id"),
        "children": values.get("children") or [],
    }


def _normalize_preferred_node(record: dict[str, Any]) -> dict[str, Any]:
    has_properties = record.get("has_properties", record.get("properties_available", False))
    return _node(
        id=str(record.get("id") or record.get("node_id") or ""),
        parent_id=record.get("parent_id"),
        label=record.get("label") or record.get("display_text") or record.get("display_name"),
        node_kind=record.get("node_kind") or "unknown",
        document_kind=record.get("document_kind") or "unknown",
        source_index=record.get("source_index"),
        instance_id=record.get("instance_id"),
        reference_id=record.get("reference_id"),
        feature_id=record.get("feature_id") or record.get("source_feature_id"),
        topology_id=record.get("topology_id"),
        startup_type=record.get("startup_type"),
        internal_name=record.get("internal_name"),
        tree_path=record.get("tree_path"),
        has_properties=has_properties,
        selection=record.get("selection") or {},
        source_node_id=record.get("source_node_id") or record.get("source_feature_id") or record.get("node_id"),
    )


def _feature_id(record: dict[str, Any]) -> str:
    return str(record.get("feature_id") or record.get("native_feature_id") or record.get("source_object_id") or record.get("source_feature_id") or "")


def _feature_parent_id(record: dict[str, Any]) -> str:
    value = record.get("parent_id", record.get("parent_feature_id"))
    return "" if value is None else str(value)


def _source_index(record: dict[str, Any], fallback: int) -> int:
    for key in ("source_index", "traversal_index", "native_enumeration_index", "container_enumeration_index"):
        if record.get(key) is not None:
            return int(record[key])
    return fallback


def _build_feature_nodes(
    records: list[dict[str, Any]],
    topology_by_feature: dict[str, list[str]],
    mesh_faces_by_topology: dict[str, list[str]],
) -> list[dict[str, Any]]:
    nodes: dict[str, dict[str, Any]] = {}
    ordered = sorted(enumerate(records, 1), key=lambda item: (_source_index(item[1], item[0]), _feature_id(item[1])))
    for fallback, record in ordered:
        feature_id = _feature_id(record)
        if not feature_id:
            continue
        topology_ids = topology_by_feature.get(feature_id, [])
        mesh_face_ids = sorted({face for topo in topology_ids for face in mesh_faces_by_topology.get(topo, [])})
        nodes[feature_id] = _node(
            id=f"feature:{feature_id}",
            parent_id=None,
            label=record.get("display_text") or record.get("display_name") or record.get("name") or feature_id,
            node_kind="native_feature" if (record.get("startup_type") or record.get("canonical_native_type")) else "unknown",
            document_kind="catpart",
            source_index=_source_index(record, fallback),
            feature_id=feature_id,
            startup_type=record.get("startup_type") or record.get("native_type") or record.get("canonical_native_type"),
            internal_name=record.get("internal_name") or record.get("name"),
            tree_path=record.get("tree_path") or "",
            selection={"mesh_face_ids": mesh_face_ids, "topology_ids": topology_ids},
            source_node_id=feature_id,
        )
    roots: list[dict[str, Any]] = []
    for _, record in ordered:
        feature_id = _feature_id(record)
        node = nodes.get(feature_id)
        if not node:
            continue
        parent = nodes.get(_feature_parent_id(record))
        if parent and parent["id"] != node["id"]:
            node["parent_id"] = parent["id"]
            parent["children"].append(node)
        else:
            roots.append(node)
    _sort_tree(roots)
    return roots


def _build_product_tree(
    instances: list[dict[str, Any]],
    references: list[dict[str, Any]],
    feature_roots: list[dict[str, Any]],
    diagnostics: list[dict[str, Any]],
    source_file_name: str,
) -> list[dict[str, Any]]:
    reference_ids = {str(item.get("reference_id") or "") for item in references if item.get("reference_id")}
    instance_reference_ids = {str(item.get("reference_id") or "") for item in instances if item.get("reference_id")}
    attach_reference_ids = sorted(reference_ids or instance_reference_ids)
    can_attach = bool(feature_roots) and len(attach_reference_ids) <= 1
    if feature_roots and not can_attach:
        diagnostics.append({
            "diagnostic_id": "NATIVE_TREE_REFERENCE_MAPPING_UNRESOLVED",
            "severity": "warning",
            "message": "multiple product references exist but native features do not identify their owning reference",
        })

    nodes: dict[str, dict[str, Any]] = {}
    ordered = sorted(
        enumerate(instances, 1),
        key=lambda item: (int(item[1].get("depth") or 0), int(item[1].get("child_index") or item[0])),
    )
    for fallback, record in ordered:
        instance_id = str(record.get("instance_id") or f"I{fallback:06d}")
        parent_id = str(record.get("parent_instance_id") or "")
        reference_id = str(record.get("reference_id") or "")
        child_count = int(record.get("child_count") or 0)
        nodes[instance_id] = _node(
            id=f"instance:{instance_id}",
            parent_id=f"instance:{parent_id}" if parent_id else None,
            label=record.get("display_name") or record.get("instance_name") or instance_id,
            node_kind="product_instance" if child_count == 0 else "product_assembly",
            document_kind="catproduct",
            source_index=_source_index(record, fallback),
            instance_id=instance_id,
            reference_id=reference_id,
            internal_name=record.get("instance_name"),
            tree_path=record.get("tree_path") or record.get("instance_path") or "",
        )
    roots: list[dict[str, Any]] = []
    for _, record in ordered:
        instance_id = str(record.get("instance_id") or "")
        node = nodes.get(instance_id)
        if not node:
            continue
        parent_instance_id = str(record.get("parent_instance_id") or "")
        parent = nodes.get(parent_instance_id)
        if parent and parent["id"] != node["id"]:
            parent["children"].append(node)
        else:
            roots.append(node)
    if can_attach:
        attach_id = attach_reference_ids[0] if attach_reference_ids else ""
        for node in nodes.values():
            if node["children"]:
                continue
            if attach_id and node["reference_id"] != attach_id:
                continue
            node["children"].extend(_clone_feature_tree(feature_roots, node["id"], node["instance_id"], node["reference_id"]))
    if len(roots) != 1:
        roots = [_node(
            id="document:catproduct",
            parent_id=None,
            label=Path(source_file_name).name or "CATProduct",
            node_kind="document",
            document_kind="catproduct",
            source_index=0,
            tree_path=f"/{Path(source_file_name).name}" if source_file_name else "/CATProduct",
            children=roots,
        )]
    _sort_tree(roots)
    return roots


def _clone_feature_tree(nodes: list[dict[str, Any]], parent_id: str, instance_id: str, reference_id: str) -> list[dict[str, Any]]:
    cloned: list[dict[str, Any]] = []
    for node in nodes:
        feature_id = node.get("feature_id")
        clone_id = f"{parent_id}/feature:{feature_id}"
        clone = {**node, "id": clone_id, "parent_id": parent_id, "instance_id": instance_id or None,
                 "reference_id": reference_id or None, "source_node_id": node.get("id")}
        clone["children"] = _clone_feature_tree(node.get("children") or [], clone_id, instance_id, reference_id)
        cloned.append(clone)
    return cloned


def _topology_ids_by_feature(records: list[dict[str, Any]]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for record in records:
        feature_id = str(record.get("feature_id") or record.get("native_feature_id") or record.get("source_feature_id") or "")
        topology_id = str(record.get("topology_id") or record.get("cell_id") or record.get("result_cell_id") or "")
        if feature_id and topology_id:
            result.setdefault(feature_id, []).append(topology_id)
    return {key: sorted(set(value)) for key, value in result.items()}


def _mesh_faces_by_topology(records: list[dict[str, Any]]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for record in records:
        topology_id = str(record.get("topology_id") or record.get("cell_id") or record.get("native_face_id") or "")
        mesh_face_id = str(record.get("mesh_face_id") or record.get("mesh_map_id") or record.get("render_face_id") or record.get("face_id") or "")
        if topology_id and mesh_face_id:
            result.setdefault(topology_id, []).append(mesh_face_id)
    return {key: sorted(set(value)) for key, value in result.items()}


def _nest_flat_nodes(nodes: list[dict[str, Any]], diagnostics: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_id = {str(node.get("id") or ""): node for node in nodes if node.get("id")}
    roots: list[dict[str, Any]] = []
    visiting: set[str] = set()
    visited: set[str] = set()
    cyclic: set[str] = set()
    attached: set[str] = set()

    for node in nodes:
        node["children"] = []

    def has_cycle(node_id: str) -> bool:
        if node_id in visited:
            return False
        if node_id in visiting:
            diagnostics.append({
                "diagnostic_id": "NATIVE_TREE_PARENT_CYCLE",
                "severity": "warning",
                "message": f"native_tree_nodes parent cycle includes {node_id}",
            })
            cyclic.update(visiting)
            cyclic.add(node_id)
            return True
        visiting.add(node_id)
        parent_id = str(by_id[node_id].get("parent_id") or "")
        result = bool(parent_id and parent_id in by_id and has_cycle(parent_id))
        visiting.remove(node_id)
        visited.add(node_id)
        return result

    for node in nodes:
        node_id = str(node.get("id") or "")
        if not node_id:
            continue
        has_cycle(node_id)
        parent_id = str(node.get("parent_id") or "")
        parent = by_id.get(parent_id)
        if node_id in cyclic:
            roots.append(node)
            attached.add(node_id)
        elif parent and parent is not node:
            parent["children"].append(node)
            attached.add(node_id)
        else:
            if parent_id and parent_id not in by_id:
                diagnostics.append({
                    "diagnostic_id": "NATIVE_TREE_PARENT_MISSING",
                    "severity": "warning",
                    "message": f"native_tree_nodes parent_id {parent_id} does not exist",
                })
            roots.append(node)
            attached.add(node_id)

    for node in nodes:
        node_id = str(node.get("id") or "")
        if node_id and node_id not in attached:
            roots.append(node)
    _sort_tree(roots)
    return roots


def _normalize_properties(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        result.append({
            "node_id": str(record.get("node_id") or ""),
            "tab_id": str(record.get("tab_id") or "properties"),
            "tab_label": str(record.get("tab_label") or "属性"),
            "group_id": str(record.get("group_id") or "general"),
            "group_label": str(record.get("group_label") or "常规"),
            "field_key": str(record.get("field_key") or record.get("name") or f"field_{index}"),
            "field_label": str(record.get("field_label") or record.get("parameter_name") or record.get("field_key") or ""),
            "value": record.get("value", record.get("value_text")),
            "unit": record.get("unit") or record.get("normalized_unit") or "",
            "value_type": str(record.get("value_type") or "string"),
            "source": str(record.get("source") or record.get("value_source") or "native_caa"),
            "display_order": int(record.get("display_order") or index),
            "read_only": bool(record.get("read_only", True)),
        })
    return result


def _fallback_properties(parameters: list[dict[str, Any]], business_features: list[dict[str, Any]]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for index, parameter in enumerate(parameters):
        node_id = f"feature:{parameter.get('parameter_id')}"
        records.append({
            "node_id": node_id,
            "tab_id": "properties",
            "tab_label": "属性",
            "group_id": "parameters",
            "group_label": "参数",
            "field_key": str(parameter.get("parameter_name") or parameter.get("parameter_id") or f"parameter_{index}"),
            "field_label": str(parameter.get("parameter_name") or parameter.get("parameter_id") or ""),
            "value": parameter.get("value_text"),
            "unit": parameter.get("normalized_unit") or "",
            "value_type": "number" if parameter.get("has_normalized_numeric_value") else "string",
            "source": parameter.get("value_source") or "parameters.jsonl",
            "display_order": index,
            "read_only": True,
        })
    offset = len(records)
    for index, feature in enumerate(business_features):
        node_id = f"feature:{feature.get('source_feature_id')}"
        records.append({
            "node_id": node_id,
            "tab_id": "product",
            "tab_label": "产品",
            "group_id": "business",
            "group_label": "业务特征",
            "field_key": "feature_kind",
            "field_label": "特征类型",
            "value": feature.get("feature_kind"),
            "unit": "",
            "value_type": "string",
            "source": "business_features.jsonl",
            "display_order": offset + index,
            "read_only": True,
        })
    return records


def _mark_property_state(nodes: list[dict[str, Any]], properties: list[dict[str, Any]]) -> None:
    property_node_ids = {item["node_id"] for item in properties}

    def visit(node: dict[str, Any]) -> None:
        ids = {node["id"], f"feature:{node.get('feature_id')}" if node.get("feature_id") else ""}
        node["has_properties"] = bool(node.get("has_properties") or property_node_ids.intersection(ids))
        for child in node.get("children") or []:
            visit(child)

    for root in nodes:
        visit(root)


def _sort_tree(nodes: list[dict[str, Any]]) -> None:
    nodes.sort(key=lambda item: (int(item.get("source_index") or 0), str(item.get("id") or "")))
    for node in nodes:
        _sort_tree(node.get("children") or [])
