from __future__ import annotations

from dataclasses import dataclass
from uuid import UUID, NAMESPACE_URL, uuid5


CATALOG_NAMESPACE = "https://component-builds.local/catalog/v1"


@dataclass(frozen=True)
class CatalogPart:
    code: str
    label: str
    label_en: str
    id_prefix: str
    sort_order: int
    catalog_node_id: UUID


@dataclass(frozen=True)
class CatalogCategory:
    code: str
    label: str
    label_en: str
    sort_order: int
    catalog_node_id: UUID
    parts: tuple[CatalogPart, ...]
    library_code: str = "MECHANICAL_COMPONENT_LIBRARY"


@dataclass(frozen=True)
class CatalogLibrary:
    code: str
    label: str
    label_en: str
    sort_order: int
    catalog_node_id: UUID
    categories: tuple[CatalogCategory, ...]


def _node_id(code: str) -> UUID:
    return uuid5(NAMESPACE_URL, f"{CATALOG_NAMESPACE}/{code}")


def _part(code: str, label: str, label_en: str, id_prefix: str, sort_order: int) -> CatalogPart:
    return CatalogPart(code, label, label_en, id_prefix, sort_order, _node_id(f"part/{code}"))


def _category(
    code: str,
    label: str,
    label_en: str,
    sort_order: int,
    parts: tuple[CatalogPart, ...],
    library_code: str = "MECHANICAL_COMPONENT_LIBRARY",
) -> CatalogCategory:
    return CatalogCategory(code, label, label_en, sort_order, _node_id(f"category/{code}"), parts, library_code)


CATEGORIES: tuple[CatalogCategory, ...] = (
    _category("support-frame", "支撑与框架类", "Frame / Housing / Support", 1, (
        _part("frame", "机架", "Frame", "frame", 1),
        _part("base", "底座", "Base", "base", 2),
        _part("housing", "壳体／箱体", "Housing", "housing", 3),
        _part("bracket", "支架／支座", "Bracket", "bracket", 4),
        _part("column", "立柱", "Column", "column", 5),
        _part("beam", "横梁", "Beam", "beam", 6),
        _part("support-ring", "支撑环", "Support Ring", "support-ring", 7),
        _part("support-plate", "支板", "Support Plate", "support-plate", 8),
    )),
    _category("shaft-transmission", "轴系与传动类", "Shaft / Gear / Bearing", 2, (
        _part("shaft", "传动轴", "Shaft", "shaft", 1),
        _part("bearing", "轴承", "Bearing", "bearing", 2),
        _part("bearing-housing", "轴承座", "Bearing Housing", "bearing-housing", 3),
        _part("gear", "齿轮", "Gear", "gear", 4),
        _part("pulley", "带轮", "Pulley", "pulley", 5),
        _part("sprocket", "链轮", "Sprocket", "sprocket", 6),
        _part("coupling", "联轴器", "Coupling", "coupling", 7),
        _part("lead-screw", "丝杠螺母副", "Lead Screw", "lead-screw", 8),
    )),
    _category("roller", "辊轮类", "Roller / Drum", 3, (
        _part("traction-roller", "牵引辊", "Traction Roller", "traction-roller", 1),
        _part("guide-roller", "导向辊", "Guide Roller", "guide-roller", 2),
        _part("press-roller", "压辊／压紧辊", "Press Roller", "press-roller", 3),
        _part("conveyor-roller", "输送滚筒", "Conveyor Roller", "conveyor-roller", 4),
        _part("idler-roller", "托辊", "Idler Roller", "idler-roller", 5),
        _part("paddle-roller", "拨料辊", "Paddle Roller", "paddle-roller", 6),
    )),
    _category("connection-fastening", "连接与紧固类", "Fastener / Flange / Hinge", 4, (
        _part("bolt-joint", "螺栓连接组", "Bolt Joint", "bolt-joint", 1),
        _part("flange", "法兰", "Flange", "flange", 2),
        _part("hinge", "铰接结构", "Hinge", "hinge", 3),
        _part("key", "键", "Key", "key", 4),
        _part("pin", "销", "Pin", "pin", 5),
        _part("clamp", "卡箍", "Clamp", "clamp", 6),
        _part("sleeve", "套筒", "Sleeve", "sleeve", 7),
        _part("retaining-ring", "挡圈 / 卡簧", "Retaining Ring / Circlip", "retaining-ring", 8),
    )),
    _category("drive-actuation", "驱动与执行类", "Motor / Cylinder / Spring", 5, (
        _part("motor", "电机", "Motor", "motor", 1),
        _part("reducer", "减速器", "Reducer", "reducer", 2),
        _part("cylinder", "气缸", "Cylinder", "cylinder", 3),
        _part("hydraulic-cylinder", "液压缸", "Hydraulic Cylinder", "hydraulic-cylinder", 4),
        _part("spring", "弹簧", "Spring", "spring", 5),
    )),
    _category("functional", "功能部件类", "Functional Components", 6, (
        _part("hopper", "料斗", "Hopper", "hopper", 1),
        _part("agitator", "搅拌器", "Agitator", "agitator", 2),
        _part("cutter", "切刀", "Cutter", "cutter", 3),
        _part("die", "模头", "Die", "die", 4),
        _part("screen", "筛网", "Screen", "screen", 5),
        _part("guide-rail", "导轨", "Guide Rail", "guide-rail", 6),
        _part("slider", "滑块", "Slider", "slider", 7),
        _part("air-ring", "风环", "Air Ring", "air-ring", 8),
        _part("cooling-unit", "冷却装置", "Cooling Unit", "cooling-unit", 9),
        _part("barrel-body", "桶体", "Barrel Body", "barrel-body", 10),
        _part("cone-body", "锥体", "Cone Body", "cone-body", 11),
    )),
)


LIBRARIES: tuple[CatalogLibrary, ...] = (
    CatalogLibrary(
        "MECHANICAL_COMPONENT_LIBRARY", "机械工程图元库", "Mechanical Component Library", 1,
        _node_id("library/mechanical-component-library"), CATEGORIES,
    ),
)

ALL_CATEGORIES: tuple[CatalogCategory, ...] = tuple(
    category for library in LIBRARIES for category in library.categories
)


class CatalogValidationError(ValueError):
    code = "invalid_catalog_selection"


def get_category(category_code: str) -> CatalogCategory | None:
    return next((category for category in ALL_CATEGORIES if category.code == category_code), None)


def resolve_part(category_code: str, part_type_code: str) -> tuple[CatalogCategory, CatalogPart]:
    category = get_category(category_code)
    if category is None:
        raise CatalogValidationError(f"unknown category_code: {category_code}")
    part = next((item for item in category.parts if item.code == part_type_code), None)
    if part is None:
        raise CatalogValidationError(f"part_type_code {part_type_code} does not belong to {category_code}")
    return category, part


def find_part_by_node_id(catalog_node_id: UUID | None) -> tuple[CatalogCategory, CatalogPart] | None:
    if catalog_node_id is None:
        return None
    for category in ALL_CATEGORIES:
        for part in category.parts:
            if part.catalog_node_id == catalog_node_id:
                return category, part
    return None


def find_part_by_legacy_type(component_type: str | None) -> tuple[CatalogCategory, CatalogPart] | None:
    normalized = _normalize_catalog_term(component_type)
    for category in ALL_CATEGORIES:
        for part in category.parts:
            aliases = {
                part.code,
                part.id_prefix,
                _normalize_catalog_term(part.label_en),
                _normalize_catalog_term(part.label),
                *(_normalize_catalog_term(alias) for alias in part.label.replace("／", "/").split("/")),
            }
            if normalized in aliases:
                return category, part
    return None


def _normalize_catalog_term(value: str | None) -> str:
    return (value or "").strip().lower().replace("_", "-").replace(" ", "-").replace("／", "/")


def catalog_payload() -> dict:
    categories = [
            {
                "catalog_node_id": str(category.catalog_node_id),
                "category_code": category.code,
                "label": category.label,
                "label_en": category.label_en,
                "sort_order": category.sort_order,
                "parts": [
                    {
                        "catalog_node_id": str(part.catalog_node_id),
                        "part_type_code": part.code,
                        "label": part.label,
                        "label_en": part.label_en,
                        "id_prefix": part.id_prefix,
                        "sort_order": part.sort_order,
                    }
                    for part in category.parts
                ],
            }
            for category in ALL_CATEGORIES
        ]
    return {
        "libraries": [
            {
                "catalog_node_id": str(library.catalog_node_id),
                "library_code": library.code,
                "label": library.label,
                "label_en": library.label_en,
                "sort_order": library.sort_order,
                "categories": [
                    next(item for item in categories if item["category_code"] == category.code)
                    for category in library.categories
                ],
            }
            for library in LIBRARIES
        ],
        # 用途：保留扁平分类字段，兼容尚未升级的调用方。
        "categories": categories,
    }
