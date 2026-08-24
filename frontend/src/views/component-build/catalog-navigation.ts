/**
 * 用途：把后端目录树转换为左侧可递归展开的导航项，并统一计算后代零件数量。
 */

export interface CatalogTreeNodeLike {
  id: string
  label: string
  label_en?: string | null
  node_type: string
  library_code?: string | null
  category_code?: string | null
  part_type_code?: string | null
  children: CatalogTreeNodeLike[]
}

export interface CatalogNavigationItem {
  id: string
  label: string
  code: string
  count: number
  parentId?: string
  depth: number
  nodeType: 'library' | 'family' | 'type'
  categoryCode?: string
  partTypeCode?: string
  descendantCategoryCodes: string[]
}

const hiddenCatalogLibraryCodes = new Set(['AEROSPACE_PART_LIBRARY'])

/**
 * 用途：递归生成导航项；系统库、分类和零件类型均来自后端，不维护前端分类常量。
 */
export function buildCatalogNavigation(
  nodes: CatalogTreeNodeLike[],
  parentId?: string,
  depth = 0
): CatalogNavigationItem[] {
  const result: CatalogNavigationItem[] = []
  for (const node of nodes) {
    if (!['library', 'family', 'type'].includes(node.node_type)) continue
    if (isHiddenCatalogNode(node)) continue
    result.push({
      id: node.id,
      label: catalogNodeLabel(node),
      code: node.library_code || node.category_code || node.part_type_code || node.id,
      count: countBuildNodes(node),
      parentId,
      depth,
      nodeType: node.node_type as 'library' | 'family' | 'type',
      categoryCode: node.category_code || undefined,
      partTypeCode: node.part_type_code || undefined,
      descendantCategoryCodes: collectCategoryCodes(node)
    })
    result.push(...buildCatalogNavigation(node.children, node.id, depth + 1))
  }
  return result
}

function catalogNodeLabel(node: CatalogTreeNodeLike): string {
  if (node.node_type === 'library') return node.label
  return node.label
}

function isHiddenCatalogNode(node: CatalogTreeNodeLike): boolean {
  if (node.library_code && hiddenCatalogLibraryCodes.has(node.library_code)) return true
  if (node.label.includes('航空航天零件库')) return true
  if (node.category_code?.startsWith('aero-')) return true
  if (node.part_type_code?.startsWith('aero-')) return true
  return false
}

/**
 * 用途：收集某目录全部后代分类编码，供选择系统库根时过滤其下所有零件。
 */
export function collectCategoryCodes(node: CatalogTreeNodeLike): string[] {
  const values = node.category_code ? [node.category_code] : []
  for (const child of node.children) values.push(...collectCategoryCodes(child))
  return [...new Set(values)]
}

/**
 * 用途：只统计真实 build 叶子，使系统库根和分类数量采用同一口径。
 */
export function countBuildNodes(node: CatalogTreeNodeLike): number {
  if (node.node_type === 'build') return 1
  return node.children.reduce((total, child) => total + countBuildNodes(child), 0)
}
