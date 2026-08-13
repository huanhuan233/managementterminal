from __future__ import annotations

import json
import shutil as _shutil
from pathlib import Path
from uuid import UUID

from sqlalchemy.ext.asyncio import AsyncSession

from app.component_builds.catalog import (
    CATEGORIES,
    LIBRARIES,
    CatalogCategory,
    CatalogLibrary,
    CatalogPart,
    find_part_by_legacy_type,
    find_part_by_node_id,
    resolve_part,
)
from app.component_builds.component_spec import component_spec_template
from app.component_builds.component_spec_document import (
    pack_component_spec_document,
    unpack_component_spec_document,
    validate_component_spec_yaml,
)
from app.component_builds.fusion import FusionSourceUnavailable, fuse_component_spec
from app.core.config import Settings
from app.component_builds.viewer_bom import build_bom_contract
from app.db.models import CadModel, CadModelRevision, CadSpecTask, CadSpecSource, CadDrawingRegion, CadDrawingFact, ComponentBuild


INPUTS_LABEL = "\u8f93\u5165\u8d44\u6599"
DATA_FUSION_LABEL = "\u6570\u636e\u878d\u5408"
PUBLISH_VALIDATION_LABEL = "\u53d1\u5e03\u6821\u9a8c"
FUTURE_STATUS_LABEL = "\u540e\u7eed\u80fd\u529b"


class SqlAlchemySourceStatusReader:
    def __init__(self, session: AsyncSession):
        self.session = session

    async def get_step_status(self, revision_id: UUID) -> dict:
        revision = await self.session.get(CadModelRevision, revision_id)
        return self._source_status(revision) if revision else {"status": "missing"}

    async def get_drawing_status(self, task_id: UUID) -> dict:
        task = await self.session.get(CadSpecTask, task_id)
        return self._source_status(task) if task else {"status": "missing"}

    @staticmethod
    def _source_status(source) -> dict:
        payload = {
            "status": source.status,
            "progress": source.progress,
            "status_message": getattr(source, "status_message", None),
            "error_code": getattr(source, "error_code", None),
            "error_message": getattr(source, "error_message", None),
        }
        manifest = getattr(source, "parse_manifest", None) or {}
        payload.update(manifest.get("ingest", {}))
        payload["viewer_asset"] = manifest.get("viewer_asset")
        payload["feature_center"] = manifest.get("feature_center")
        return payload


class ComponentBuildService:
    def __init__(self, repository, *, source_status_reader, fusion_source_reader=None):
        self.repository = repository
        self.source_status_reader = source_status_reader
        self.fusion_source_reader = fusion_source_reader

    async def get_tree(self) -> list[dict]:
        builds = await self.repository.list_builds()
        grouped: dict[str, dict[str, list[ComponentBuild]]] = {}
        uncategorized: list[ComponentBuild] = []
        for build in builds:
            catalog = self._catalog_for_build(build)
            if catalog is None:
                uncategorized.append(build)
                continue
            category, part = catalog
            grouped.setdefault(category.code, {}).setdefault(part.code, []).append(build)

        tree = [await self._library_node(library, grouped) for library in LIBRARIES]
        if uncategorized:
            tree[0]["children"].append(await self._uncategorized_node(uncategorized))
            tree[0]["count"] = self._count_build_nodes(tree[0]["children"])
        return tree

    async def create_build(self, **fields) -> dict:
        build = await self.repository.create_build(**fields)
        return self._build_payload(build)

    async def get_component_spec(self, build_id: UUID) -> dict:
        draft = await self.repository.get_component_spec(build_id)
        document = (
            unpack_component_spec_document(draft.data)
            if draft
            else None
        )
        data = (
            document.data
            if document and document.is_envelope
            else component_spec_template.normalize(document.data)
            if document
            else component_spec_template.blank_data()
        )
        yaml_text = document.yaml if document and document.yaml is not None else component_spec_template.render_yaml(data)
        return {
            "build_id": str(build_id),
            "schema": component_spec_template.schema,
            "data": data,
            "yaml": yaml_text,
            "source_filename": document.source_filename if document else None,
            "saved": draft is not None,
            "updated_at": draft.updated_at.isoformat() if draft else None,
        }

    async def save_component_spec(
        self,
        build_id: UUID,
        data: dict,
        *,
        yaml_text: str | None = None,
        source_filename: str | None = None,
    ) -> dict:
        if yaml_text is None:
            saved_data = component_spec_template.normalize(data)
            saved_yaml = component_spec_template.render_yaml(saved_data)
        else:
            saved_data = validate_component_spec_yaml(yaml_text, data)
            saved_yaml = yaml_text
        draft = await self.repository.save_component_spec(
            build_id,
            pack_component_spec_document(saved_data, saved_yaml, source_filename),
        )
        return {
            "build_id": str(build_id),
            "schema": component_spec_template.schema,
            "data": saved_data,
            "yaml": saved_yaml,
            "source_filename": source_filename,
            "saved": True,
            "updated_at": draft.updated_at.isoformat(),
        }

    async def preview_component_spec(self, build_id: UUID, data: dict, *, yaml_text: str | None = None) -> str:
        await self.repository.get_build(build_id) or self._raise_missing_build(build_id)
        if yaml_text is not None:
            validate_component_spec_yaml(yaml_text, data)
            return yaml_text
        return component_spec_template.render_yaml(data)

    async def fuse_component_spec(self, build_id: UUID, *, overwrite: bool = False) -> dict:
        build = await self._require_build(build_id)
        if self.fusion_source_reader is None:
            raise FusionSourceUnavailable("no_sources_available")
        sources = await self.fusion_source_reader.read(build)
        if not sources.available:
            raise FusionSourceUnavailable("no_sources_available")
        current_draft = await self.repository.get_component_spec(build_id)
        current = (
            unpack_component_spec_document(current_draft.data).data
            if current_draft
            else component_spec_template.blank_data()
        )
        result = fuse_component_spec(
            build=self._build_payload(build),
            current=current,
            sources=sources,
            overwrite=overwrite,
        )
        normalized = component_spec_template.normalize(result.data)
        rendered = component_spec_template.render_yaml(normalized)
        await self.repository.save_component_spec(
            build_id,
            pack_component_spec_document(normalized, rendered),
        )
        return {
            "build_id": str(build_id),
            "status": "completed",
            "summary": result.summary,
            "fields": result.fields,
            "warnings": result.warnings,
            "component_spec": normalized,
        }

    async def create_catalog_build(
        self,
        *,
        category_code: str,
        part_type_code: str,
        component_name: str,
        standard_number: str | None = None,
        version: str = "1.0.0",
        status: str = "draft",
    ) -> dict:
        category, part = resolve_part(category_code, part_type_code)
        component_id = await self.repository.next_component_id(part.id_prefix)
        return await self.create_build(
            catalog_node_id=part.catalog_node_id,
            component_id=component_id,
            component_name=component_name,
            component_type=part.code,
            component_subtype=None,
            family=category.code,
            standard_number=standard_number,
            version=version,
            default_dn=None,
            default_pn=None,
            status=status,
        )

    async def attach_step(self, build_id: UUID, *, model_id: UUID, revision_id: UUID) -> dict:
        build = await self.repository.attach_step(build_id, model_id=model_id, revision_id=revision_id)
        return self._build_payload(build)

    async def update_catalog_build(
        self,
        build_id: UUID,
        *,
        category_code: str,
        part_type_code: str,
        component_name: str,
        standard_number: str | None = None,
        version: str = "1.0.0",
    ) -> dict:
        category, part = resolve_part(category_code, part_type_code)
        build = await self.repository.update_build(
            build_id,
            catalog_node_id=part.catalog_node_id,
            component_name=component_name,
            component_type=part.code,
            component_subtype=None,
            family=category.code,
            standard_number=standard_number,
            version=version,
        )
        return self._build_payload(build)

    async def attach_drawing(self, build_id: UUID, *, task_id: UUID) -> dict:
        build = await self.repository.attach_drawing(build_id, task_id=task_id)
        return self._build_payload(build)

    async def set_status(
        self,
        build_id: UUID,
        *,
        status: str,
        message: str | None = None,
        error_code: str | None = None,
        error_message: str | None = None,
    ) -> dict:
        await self.repository.set_status(
            build_id,
            status=status,
            message=message,
            error_code=error_code,
            error_message=error_message,
        )
        return await self.get_build(build_id)

    async def delete_build(self, build_id: UUID, *, settings: Settings) -> dict:
        """Delete a component build and all associated resources.

        Cascading cleanup:
        1. ComponentSpecDraft — DB cascade (FK ondelete=CASCADE)
        2. CadSpecTask / drawing regions / sources / facts — manual cleanup + disk files
        3. CadModel + CadModelRevision + disk files
        4. ComponentBuild record
        """
        build = await self._require_build(build_id)

        # 用途：存在关联图纸任务时清理其受控资源。
        if build.drawing_task_id:
            task_id = build.drawing_task_id
            # 用途：删除图纸区域裁剪文件。
            region_rows = await self.repository.list_drawing_regions(task_id)
            for region in region_rows:
                crop_path = region.get("crop_file_path")
                if crop_path:
                    p = Path(crop_path)
                    if p.exists():
                        p.unlink()
            # 用途：删除图纸源文件。
            source_rows = await self.repository.list_drawing_sources(task_id)
            for src in source_rows:
                fp = src.get("file_path")
                if fp:
                    p = Path(fp)
                    if p.exists():
                        p.unlink()
            # 用途：删除图纸任务工作目录。
            task_dir = Path(settings.cad_spec_work_dir) / str(task_id)
            if task_dir.exists():
                _shutil.rmtree(task_dir)
            # 用途：删除图纸任务记录，并由数据库级联清理来源、区域和事实。
            drawing_task = await self.repository.session.get(CadSpecTask, task_id)
            if drawing_task:
                await self.repository.session.delete(drawing_task)

        # 用途：清理 CAD 模型文件和数据库记录。
        if build.cad_revision_id:
            revision_id = build.cad_revision_id
            # 用途：删除 Revision 的源模型文件。
            revision = await self.repository.get_raw_revision(revision_id)
            if revision and revision.source_file_path and revision.source_file_path != "pending":
                src_path = Path(revision.source_file_path)
                if src_path.exists():
                    src_path.unlink()
            # 用途：删除 Revision 工作目录。
            rev_dir = Path(settings.cad_work_dir) / str(revision_id)
            if rev_dir.exists():
                _shutil.rmtree(rev_dir)

        # 用途：仅当模型未被其他构建共享时删除 CAD 模型。
        if build.cad_model_id:
            other = await self.repository.list_builds_by_model(build.cad_model_id, exclude_build_id=build_id)
            if not other:
                model_dir = Path(settings.cad_work_dir) / str(build.cad_model_id)
                if model_dir.exists():
                    _shutil.rmtree(model_dir)
                # 用途：删除 CadModel 记录，并由数据库级联清理 Revision 和实体等数据。
                cad_model = await self.repository.session.get(CadModel, build.cad_model_id)
                if cad_model:
                    await self.repository.session.delete(cad_model)

        # 用途：删除构建记录，ComponentSpecDraft 交由数据库级联清理。
        deleted = await self.repository.delete_build(build_id)
        return {"id": str(deleted.id), "deleted": True}

    async def get_build(self, build_id: UUID) -> dict:
        build = await self._require_build(build_id)
        step = await self._step_source(build)
        drawing = await self._drawing_source(build)
        return self._projected_build_payload(build, step, drawing)

    async def get_status(self, build_id: UUID) -> dict:
        build = await self._require_build(build_id)
        step = await self._step_source(build)
        drawing = await self._drawing_source(build)
        projected = self._projected_build_payload(build, step, drawing)
        return {
            "build_id": str(build.id),
            "status": projected["status"],
            "source_format": projected.get("source_format"),
            "processing_route": projected.get("processing_route"),
            "current_stage": projected.get("current_stage"),
            "progress": projected.get("progress"),
            "status_message": projected.get("status_message"),
            "error_code": projected.get("error_code"),
            "error_message": projected.get("error_message"),
            "sources": {"reference_step": step, "drawing": drawing},
        }

    async def get_viewer_contract(self, build_id: UUID) -> dict:
        """用途：把两条处理路线统一投影为浏览器可消费的 Viewer URL 契约。"""
        build = await self._require_build(build_id)
        if build.cad_revision_id is None:
            raise ValueError("viewer source is not attached")
        revision = await self.repository.get_raw_revision(build.cad_revision_id)
        if revision is None:
            raise ValueError("viewer revision is missing")
        manifest = revision.parse_manifest or {}
        ingest = manifest.get("ingest", {})
        viewer = manifest.get("viewer_asset") or {}
        required = ("glb", "scene_manifest", "face_mesh_map", "feature_mesh_map")
        source_format = ingest.get("source_format") or self._source_format_from_extension(revision.source_file_ext)
        bom = await self._viewer_bom(revision, source_format, manifest)
        viewer_summary = manifest.get("viewer_summary") or {}
        viewer_geometry = self._viewer_geometry_contract(source_format, manifest, viewer_summary)
        summary = {
            "model_name": build.component_name,
            "source_file_name": getattr(revision, "source_file_name", None),
            "part_number": build.standard_number or "",
            "part_name": build.component_name,
            "version": build.version or "",
            "material": str(viewer_summary.get("material") or ""),
            "solid_count": int(viewer_summary.get("solid_count") or 0),
            "native_feature_count": int(viewer_summary.get("native_feature_count") or 0),
            "recognized_feature_count": int(viewer_summary.get("recognized_feature_count") or 0),
            # 用途：只有 Bundle 中实际存在 Feature–Face 链接时才向前端声明映射可用，不能用 Bundle 是否完成替代。
            "feature_face_mapping_available": bool((manifest.get("feature_center") or {}).get("mapping_available")),
        }
        base_payload = {
            "part_id": str(build.id),
            "task_id": str(revision.id),
            "source_format": source_format,
            "processing_route": ingest.get("processing_route") or "step_cad_parse",
            "summary": summary,
            "bom": bom,
            "viewer_geometry": viewer_geometry,
            "worker": manifest.get("worker") or {"mode": "not_applicable"},
            "progress": int(getattr(revision, "progress", 0) or 0),
        }
        if revision.status != "completed" or revision.status_message != "ready" or any(not viewer.get(key) for key in required):
            return {
                **base_payload,
                "status": revision.status,
                "current_stage": revision.status_message,
                "viewer_asset": None,
                "feature_center": {"available": False},
                "error_code": revision.error_code,
                "error_message": revision.error_message,
            }
        asset_base = f"/api/component-builds/{build.id}/viewer/assets/"
        feature_center = manifest.get("feature_center") or {}
        return {
            **base_payload,
            "status": "ready",
            "current_stage": "ready",
            "viewer_asset": {
                "glb_url": asset_base + viewer["glb"],
                "scene_manifest_url": asset_base + viewer["scene_manifest"],
                "face_mesh_map_url": asset_base + viewer["face_mesh_map"],
                "feature_mesh_map_url": asset_base + viewer["feature_mesh_map"],
                "selection_index_url": asset_base + viewer["selection_index"]
                if viewer.get("selection_index") else None,
            },
            "feature_center": {
                "available": bool(feature_center.get("available")),
                "mapping_available": bool(feature_center.get("mapping_available")),
                "feature_face_mapping_count": int(feature_center.get("feature_face_mapping_count") or 0),
                "canonical_features_url": asset_base + feature_center["canonical_features"]
                if feature_center.get("canonical_features") else None,
                "feature_geometry_links_url": asset_base + feature_center["feature_geometry_links"]
                if feature_center.get("feature_geometry_links") else None,
                "measurements_url": asset_base + feature_center["measurements"]
                if feature_center.get("measurements") else None,
                "topology_faces_url": asset_base + feature_center["topology_faces"]
                if feature_center.get("topology_faces") else None,
                "topology_edges_url": asset_base + feature_center["topology_edges"]
                if feature_center.get("topology_edges") else None,
            },
            "native_semantics": self._native_semantics_contract(asset_base, manifest.get("native_semantics") or {}),
            "error_code": None,
            "error_message": None,
        }

    @staticmethod
    def _native_semantics_contract(asset_base: str, native: dict) -> dict:
        payload = {"available": bool(native.get("available"))}
        for key in (
            "features",
            "native_features",
            "topology_bodies",
            "topology_cells",
            "topology_wires",
            "topology_coedges",
            "mesh_face_map",
            "mesh_triangles",
            "feature_results",
            "feature_result_cells",
            "feature_topology_links",
            "product_references",
            "product_instances",
            "native_tree_nodes",
            "node_properties",
            "native_tree_diagnostics",
            "capabilities",
        ):
            payload[f"{key}_url"] = asset_base + native[key] if native.get(key) else None
        return payload

    async def _viewer_bom(self, revision, source_format: str, manifest: dict | None = None) -> dict:
        """用途：把数据库中的真实结构实体投影为统一 BOM；无装配数据时保持单零件/空契约。"""
        entities = await self.repository.list_structure_entities(revision.id)
        nodes: dict[str, dict] = {}
        roots: list[dict] = []
        for entity in entities:
            node = {
                "id": str(entity.id),
                "parent_entity_id": str(entity.parent_entity_id) if entity.parent_entity_id else "",
                "entity_type": entity.entity_type,
                "label": entity.label or entity.name or entity.source_ref or entity.entity_type,
                "source_ref": entity.source_ref,
                "placement": entity.placement,
                "volume": entity.volume,
                "bounding_box": entity.bounding_box,
                "metadata": entity.metadata_json,
                "children": [],
            }
            nodes[str(entity.id)] = node
        for node in nodes.values():
            parent = nodes.get(node["parent_entity_id"])
            if parent is None:
                roots.append(node)
            else:
                parent["children"].append(node)
        if not roots and source_format.upper() == "CATPRODUCT":
            roots = self._native_product_instances_bom_roots(revision, manifest or {})
        if not roots:
            roots = [{
                "id": str(revision.id),
                "parent_entity_id": "",
                "entity_type": "part",
                "label": getattr(revision, "source_file_name", None) or "单零件",
                "source_ref": getattr(revision, "source_file_name", None) or "",
                "metadata": {"quantity": 1},
                "children": [],
            }]
        return build_bom_contract(roots, source_format)

    @staticmethod
    def _viewer_geometry_contract(source_format: str, manifest: dict, viewer_summary: dict) -> dict:
        feature_manifest = manifest.get("feature_center_manifest") or {}
        lightweight = feature_manifest.get("lightweight") or {}
        performance = feature_manifest.get("performance") or {}
        primitive_count = int(lightweight.get("primitive_count") or performance.get("mesh_primitive_count") or 0)
        triangle_count = int(lightweight.get("triangle_count") or performance.get("mesh_triangle_count") or 0)
        solid_count = int(viewer_summary.get("solid_count") or 0)
        displayable = primitive_count > 0 and triangle_count > 0
        empty_reason = None
        if not displayable:
            if source_format.upper() == "CATPRODUCT":
                empty_reason = "catproduct_missing_loaded_representations"
            else:
                empty_reason = "empty_lightweight_geometry"
        return {
            "displayable": displayable,
            "primitive_count": primitive_count,
            "triangle_count": triangle_count,
            "solid_count": solid_count,
            "empty_reason": empty_reason,
        }

    @staticmethod
    def _native_product_instances_bom_roots(revision, manifest: dict) -> list[dict]:
        native = manifest.get("native_semantics") or {}
        product_instances = native.get("product_instances")
        if not product_instances:
            return []
        base_dir = Path(getattr(revision, "source_file_path", "") or "").parent
        if not str(base_dir):
            return []
        relative_path = Path(product_instances)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            return []
        path = base_dir / relative_path
        if not path.is_file():
            return []

        records: list[dict] = []
        with path.open("r", encoding="utf-8") as stream:
            for line in stream:
                if line.strip():
                    records.append(json.loads(line))

        nodes: dict[str, dict] = {}
        ordered_records = sorted(
            records,
            key=lambda item: (int(item.get("depth") or 0), int(item.get("child_index") or 0)),
        )
        for record in ordered_records:
            instance_id = str(record.get("instance_id") or "")
            if not instance_id:
                continue
            parent_id = str(record.get("parent_instance_id") or "")
            child_count = int(record.get("child_count") or 0)
            node_type = "subassembly" if parent_id and child_count > 0 else ("assembly" if child_count > 0 else "part")
            instance_name = str(record.get("instance_name") or record.get("instance_path") or instance_id)
            reference_id = str(record.get("reference_id") or "")
            nodes[instance_id] = {
                "id": instance_id,
                "parent_entity_id": parent_id,
                "entity_type": node_type,
                "label": instance_name,
                "source_ref": reference_id,
                "placement": record.get("transform_4x4"),
                "metadata": {
                    "instance_name": instance_name,
                    "part_number": reference_id,
                    "assembly_path": str(record.get("instance_path") or record.get("tree_path") or ""),
                    "load_status": record.get("load_status"),
                    "read_status": record.get("read_status"),
                    "transform_status": record.get("transform_status"),
                    "quantity": 1,
                },
                "children": [],
            }

        roots: list[dict] = []
        for record in ordered_records:
            node = nodes.get(str(record.get("instance_id") or ""))
            if node is None:
                continue
            parent = nodes.get(node["parent_entity_id"])
            if parent is None:
                roots.append(node)
            else:
                parent["children"].append(node)
        return roots

    @staticmethod
    def _source_format_from_extension(extension: str) -> str:
        normalized = (extension or "").lower()
        if normalized == ".catpart":
            return "CATPART"
        if normalized == ".catproduct":
            return "CATPRODUCT"
        return "STEP"

    async def _require_build(self, build_id: UUID) -> ComponentBuild:
        build = await self.repository.get_build(build_id)
        if build is None:
            raise ValueError(f"component build not found: {build_id}")
        return build

    async def _step_source(self, build: ComponentBuild) -> dict:
        if build.cad_revision_id is None:
            return {"id": None, "status": "waiting_for_step"}
        source = await self.source_status_reader.get_step_status(build.cad_revision_id)
        return {"id": str(build.cad_revision_id), **source}

    async def _drawing_source(self, build: ComponentBuild) -> dict:
        if build.drawing_task_id is None:
            return {"id": None, "status": "missing"}
        source = await self.source_status_reader.get_drawing_status(build.drawing_task_id)
        return {"id": str(build.drawing_task_id), **source}

    @staticmethod
    def _project_status(build: ComponentBuild, step_status: str, drawing_status: str) -> str:
        if build.status == "source_failed":
            return "source_failed"
        if step_status == "failed" or drawing_status == "failed":
            return "source_failed"
        if drawing_status == "needs_manual_layout":
            return "review_required"
        if step_status == "completed" and drawing_status == "review_ready":
            return "sources_ready"
        if step_status == "completed" and drawing_status in {"waiting_for_step", "missing"}:
            return "sources_partial"
        if build.status == "uploading" and not (build.cad_revision_id and build.drawing_task_id):
            return build.status
        if build.cad_revision_id or build.drawing_task_id:
            return "parsing_sources"
        return build.status

    def _build_payload(self, build: ComponentBuild) -> dict:
        catalog = self._catalog_for_build(build)
        return {
            "id": str(build.id),
            "catalog_node_id": str(build.catalog_node_id) if build.catalog_node_id else None,
            "catalog_path": self._catalog_path(catalog),
            "component_id": build.component_id,
            "component_name": build.component_name,
            "component_type": build.component_type,
            "component_subtype": build.component_subtype,
            "family": build.family,
            "standard_number": build.standard_number,
            "version": build.version,
            "default_dn": build.default_dn,
            "default_pn": build.default_pn,
            "cad_model_id": str(build.cad_model_id) if build.cad_model_id else None,
            "cad_revision_id": str(build.cad_revision_id) if build.cad_revision_id else None,
            "drawing_task_id": str(build.drawing_task_id) if build.drawing_task_id else None,
            "status": build.status,
            "status_message": build.status_message,
            "error_code": build.error_code,
            "error_message": build.error_message,
            "created_at": build.created_at,
            "updated_at": build.updated_at,
        }

    def _projected_build_payload(self, build: ComponentBuild, step: dict, drawing: dict) -> dict:
        payload = self._build_payload(build)
        step_ready = step.get("status") == "completed" and step.get("status_message") == "ready"
        payload["status"] = (
            "ready"
            if step_ready and drawing.get("status") == "missing"
            else self._project_status(build, step["status"], drawing["status"])
        )
        payload["task_id"] = step.get("id")
        payload["source_format"] = step.get("source_format")
        payload["processing_route"] = step.get("processing_route")
        payload["current_stage"] = step.get("status_message")
        payload["progress"] = step.get("progress")
        # 用途：图纸失败优先投影为整体错误，避免已就绪三维模型掩盖另一来源的真实失败。
        if drawing.get("status") == "failed":
            payload["current_stage"] = drawing.get("status_message") or "drawing_failed"
            payload["status_message"] = payload["current_stage"]
            payload["error_code"] = drawing.get("error_code")
            payload["error_message"] = drawing.get("error_message")
        # 用途：统一上传任务存在时，以持久化 Revision 的真实阶段和错误覆盖旧的 Build 排队文案。
        elif step.get("processing_route"):
            payload["status_message"] = step.get("status_message")
            payload["error_code"] = step.get("error_code")
            payload["error_message"] = step.get("error_message")
        return payload

    async def _category_node(self, category: CatalogCategory, part_builds: dict[str, list[ComponentBuild]]) -> dict:
        children = [
            await self._part_node(category, part, part_builds.get(part.code, []))
            for part in category.parts
        ]
        return {
            "id": str(category.catalog_node_id),
            "name": category.label,
            "label": category.label,
            "label_en": category.label_en,
            "node_type": "family",
            "category_code": category.code,
            "part_type_code": None,
            "sort_order": category.sort_order,
            "count": self._count_build_nodes(children),
            "children": children,
        }

    async def _library_node(self, library: CatalogLibrary, grouped: dict[str, dict[str, list[ComponentBuild]]]) -> dict:
        """用途：把真实目录数据组织成系统根节点，并汇总所有后代零件数量。"""
        children = [
            await self._category_node(category, grouped.get(category.code, {}))
            for category in library.categories
        ]
        return {
            "id": str(library.catalog_node_id),
            "catalog_node_id": str(library.catalog_node_id),
            "name": library.label,
            "label": library.label,
            "label_en": library.label_en,
            "node_type": "library",
            "library_code": library.code,
            "sort_order": library.sort_order,
            "count": self._count_build_nodes(children),
            "children": children,
        }

    @classmethod
    def _count_build_nodes(cls, nodes: list[dict]) -> int:
        """用途：按现有构建记录口径统计任意目录节点的全部后代数量。"""
        return sum(
            1 if node.get("node_type") == "build" else cls._count_build_nodes(node.get("children", []))
            for node in nodes
        )

    async def _part_node(self, category: CatalogCategory, part: CatalogPart, builds: list[ComponentBuild]) -> dict:
        children = await self._component_nodes(builds)
        return {
            "id": str(part.catalog_node_id),
            "catalog_node_id": str(part.catalog_node_id),
            "name": part.label,
            "label": part.label,
            "label_en": part.label_en,
            "node_type": "type",
            "category_code": category.code,
            "part_type_code": part.code,
            "sort_order": part.sort_order,
            "count": self._count_build_nodes(children),
            "children": children,
        }

    async def _uncategorized_node(self, builds: list[ComponentBuild]) -> dict:
        component_children = await self._component_nodes(builds)
        type_node = {
            "id": "catalog:uncategorized:type",
            "name": "未分类",
            "label": "未分类",
            "label_en": "Uncategorized",
            "node_type": "type",
            "category_code": "uncategorized",
            "part_type_code": "uncategorized",
            "sort_order": 1,
            "count": self._count_build_nodes(component_children),
            "children": component_children,
        }
        return {
            "id": "catalog:uncategorized",
            "name": "未分类",
            "label": "未分类",
            "label_en": "Uncategorized",
            "node_type": "family",
            "category_code": "uncategorized",
            "part_type_code": None,
            "sort_order": len(CATEGORIES) + 1,
            "count": type_node["count"],
            "children": [type_node],
        }

    async def _component_nodes(self, builds: list[ComponentBuild]) -> list[dict]:
        grouped: dict[str, list[ComponentBuild]] = {}
        for build in builds:
            grouped.setdefault(build.component_id, []).append(build)
        return [
            {
                "id": f"component:{component_id}",
                "name": component_builds[0].component_name,
                "label": component_builds[0].component_name,
                "node_type": "component",
                "component_id": component_id,
                "component_name": component_builds[0].component_name,
                "children": [await self._tree_node(build) for build in component_builds],
            }
            for component_id, component_builds in sorted(grouped.items())
        ]

    async def _tree_node(self, build: ComponentBuild) -> dict:
        step = await self._step_source(build)
        drawing = await self._drawing_source(build)
        component_spec = await self.repository.get_component_spec(build.id)
        fusion_ready = bool(build.cad_revision_id or build.drawing_task_id)
        fusion_status = "completed" if component_spec else "ready" if fusion_ready else "pending"
        fusion_status_label = "已生成草稿" if component_spec else "可开始" if fusion_ready else "待上传来源"
        projected = self._projected_build_payload(build, step, drawing)
        return {
            "id": str(build.id),
            "build_id": str(build.id),
            "name": build.version,
            "label": f"{build.component_name} {build.version}",
            "node_type": "build",
            "catalog_node_id": projected["catalog_node_id"],
            "catalog_path": projected["catalog_path"],
            "component_id": build.component_id,
            "component_name": build.component_name,
            "status": projected["status"],
            "status_message": projected.get("status_message"),
            "error_code": projected.get("error_code"),
            "error_message": projected.get("error_message"),
            "children": [
                {
                    "id": f"{build.id}:inputs",
                    "build_id": str(build.id),
                    "name": INPUTS_LABEL,
                    "label": INPUTS_LABEL,
                    "node_type": "folder",
                    "status": "pending",
                    "disabled": False,
                    "children": [
                        self._source_node(build, "reference_step", step),
                        self._source_node(build, "drawing", drawing),
                    ],
                },
                {
                    "id": f"{build.id}:fusion",
                    "build_id": str(build.id),
                    "name": DATA_FUSION_LABEL,
                    "label": DATA_FUSION_LABEL,
                    "node_type": "data_fusion",
                    "status": fusion_status,
                    "status_label": fusion_status_label,
                    "disabled": not fusion_ready,
                },
                {
                    "id": f"{build.id}:component_spec",
                    "build_id": str(build.id),
                    "name": "ComponentSpec",
                    "label": "ComponentSpec",
                    "node_type": "component_spec",
                    "status": "saved" if component_spec else "draft",
                    "status_label": "已保存" if component_spec else "待填写",
                    "disabled": False,
                },
                {"name": PUBLISH_VALIDATION_LABEL, "node_type": "publish_validation", "status": "future", "status_label": FUTURE_STATUS_LABEL, "disabled": True},
            ],
        }

    @staticmethod
    def _catalog_path(catalog: tuple[CatalogCategory, CatalogPart] | None) -> str:
        if catalog is None:
            return "/未分类"
        category, part = catalog
        return f"/{category.label}/{part.label}"

    @staticmethod
    def _catalog_for_build(build: ComponentBuild) -> tuple[CatalogCategory, CatalogPart] | None:
        return find_part_by_node_id(build.catalog_node_id) or find_part_by_legacy_type(build.component_type)

    @staticmethod
    def _raise_missing_build(build_id: UUID):
        raise ValueError(f"component build not found: {build_id}")

    @staticmethod
    def _source_node(build: ComponentBuild, role: str, source: dict) -> dict:
        labels = {"reference_step": "\u53c2\u8003 STEP", "drawing": "\u4e8c\u7ef4\u56fe\u7eb8"}
        target = None
        if source["id"]:
            target = (
                {"revision_id": str(build.cad_revision_id)}
                if role == "reference_step"
                else {"revision_id": str(build.cad_revision_id), "task_id": str(build.drawing_task_id)}
            )
        return {
            "id": f"{build.id}:{role}",
            "build_id": str(build.id),
            "name": labels[role],
            "label": labels[role],
            "node_type": role,
            "status": source["status"],
            "progress": source.get("progress"),
            "status_message": source.get("status_message"),
            "error_code": source.get("error_code"),
            "error_message": source.get("error_message"),
            "disabled": target is None,
            "target": target,
        }
