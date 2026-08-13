export type FeatureTreeKind =
  | 'catproduct'
  | 'product_assembly'
  | 'product_instance'
  | 'product_reference'
  | 'catpart'
  | 'part'
  | 'datum_group'
  | 'datum'
  | 'body'
  | 'geometry_set'
  | 'sketch'
  | 'pad'
  | 'pocket'
  | 'hole'
  | 'fillet'
  | 'chamfer'
  | 'solid_feature'
  | 'parameter'
  | 'system'
  | 'unknown';

export type FeatureTreeCategory = 'all' | 'mapped' | 'sketch' | 'solid' | 'datum';

export interface NativeFeatureRecord {
  feature_id: string;
  parent_id?: string;
  traversal_index?: number;
  native_enumeration_index?: number;
  container_enumeration_index?: number;
  display_name?: string;
  internal_name?: string;
  native_type?: string;
  startup_type?: string;
  container_kind?: string;
  tree_path?: string;
  decoder_id?: string;
  decode_level?: string;
  decode_status?: string;
  decoder_status?: string;
  payload_extraction_status?: string;
  native_feature_parameters?: Record<string, unknown>;
  canonical_native_type?: string;
  payload_type?: string;
  update_status?: string;
  attributes?: Record<string, unknown>;
  [key: string]: unknown;
}

export interface NativeTreeSelection {
  mesh_face_ids?: string[];
  topology_ids?: string[];
}

export interface NativeTreeRecord {
  id: string;
  parent_id?: string | null;
  label?: string;
  display_name?: string;
  display_text?: string;
  node_kind?: string;
  document_kind?: string;
  source_index?: number;
  instance_id?: string | null;
  reference_id?: string | null;
  feature_id?: string | null;
  topology_id?: string | null;
  startup_type?: string | null;
  internal_name?: string | null;
  tree_path?: string;
  has_geometry?: boolean;
  has_properties?: boolean;
  selection?: NativeTreeSelection;
  source_node_id?: string | null;
  children?: NativeTreeRecord[];
  [key: string]: unknown;
}

export interface FeatureTreeNode {
  id: string;
  parentId?: string;
  name: string;
  displayName: string;
  kind: FeatureTreeKind;
  nodeKind?: string;
  documentKind?: string;
  nativeType?: string;
  sourceRef?: string;
  sequence: number;
  children: FeatureTreeNode[];
  isSystem: boolean;
  isContainer: boolean;
  faceRefs: string[];
  topologyRefs: string[];
  hasGeometry: boolean;
  hasProperties: boolean;
  instanceId?: string;
  referenceId?: string;
  featureId?: string;
  topologyId?: string;
  sourceNodeId?: string;
  parameters?: Record<string, unknown>;
  raw?: NativeFeatureRecord | NativeTreeRecord;
}

export interface FeatureTreeProjection {
  nodes: FeatureTreeNode[];
  expandedKeys: string[];
}

const SYSTEM_TYPES = new Set([
  'catiprtcontainer',
  'catprtcontainer',
  'partspeccontainer',
  'gsminternal',
  'defaultvaluesbag',
  'catcatalogmanager'
]);

const KIND_BY_TYPE: Record<string, FeatureTreeKind> = {
  catdocument: 'catpart',
  mechanicalpart: 'part',
  gsmplane: 'datum',
  origin: 'datum',
  axis: 'datum',
  axis2placement3d: 'datum',
  partbody: 'body',
  body: 'body',
  solidbody: 'body',
  hybridbody: 'geometry_set',
  geometricalset: 'geometry_set',
  sketch: 'sketch',
  sketcher: 'sketch',
  pad: 'pad',
  pocket: 'pocket',
  hole: 'hole',
  fillet: 'fillet',
  edgefillet: 'fillet',
  chamfer: 'chamfer',
  shaft: 'solid_feature',
  groove: 'solid_feature',
  rib: 'solid_feature',
  stiffener: 'solid_feature',
  pattern: 'solid_feature',
  string: 'parameter',
  length: 'parameter',
  angle: 'parameter',
  real: 'parameter',
  integer: 'parameter',
  boolean: 'parameter'
};

const CONTAINER_KINDS = new Set<FeatureTreeKind>([
  'catproduct',
  'product_assembly',
  'product_instance',
  'catpart',
  'part',
  'datum_group',
  'body',
  'geometry_set',
  'system'
]);
const SOLID_KINDS = new Set<FeatureTreeKind>(['body', 'pad', 'pocket', 'hole', 'fillet', 'chamfer', 'solid_feature']);

// 用途：只截取文件名，防止 CAA 文档节点把本机绝对路径带入界面。
function baseName(path: string) {
  const parts = path.replace(/\\/g, '/').split('/');
  return parts.at(-1) || path;
}

// 用途：用经验证的 StartUp/原生类型映射展示语义；未知类型保持 unknown，不根据名称猜特征。
function featureKind(record: NativeFeatureRecord): FeatureTreeKind {
  const startup = String(record.startup_type || '').toLowerCase();
  const native = String(record.native_type || '').toLowerCase();
  if (SYSTEM_TYPES.has(startup) || SYSTEM_TYPES.has(native)) return 'system';
  return KIND_BY_TYPE[startup] || KIND_BY_TYPE[native] || 'unknown';
}

function nativeTreeKind(record: NativeTreeRecord): FeatureTreeKind {
  const nodeKind = String(record.node_kind || '').toLowerCase();
  const documentKind = String(record.document_kind || '').toLowerCase();
  if (nodeKind === 'document' && documentKind === 'catproduct') return 'catproduct';
  if (nodeKind === 'product_assembly') return 'product_assembly';
  if (nodeKind === 'product_instance') return 'product_instance';
  if (nodeKind === 'product_reference') return 'product_reference';
  if (nodeKind === 'document' && documentKind === 'catpart') return 'catpart';
  if (nodeKind === 'native_feature') {
    return featureKind({
      feature_id: String(record.feature_id || record.id),
      startup_type: String(record.startup_type || ''),
      native_type: String(record.startup_type || '')
    });
  }
  return 'unknown';
}

function sequenceOf(record: NativeFeatureRecord) {
  return Number(
    record.traversal_index ??
      record.container_enumeration_index ??
      record.native_enumeration_index ??
      Number.MAX_SAFE_INTEGER
  );
}

function sortNodes(nodes: FeatureTreeNode[]) {
  nodes.sort((left, right) => left.sequence - right.sequence || left.id.localeCompare(right.id));
  nodes.forEach(node => sortNodes(node.children));
}

// 用途：从扁平 CAA FeatureRecord 恢复原始父子树，并在 Part 下建立稳定的“基准元素”业务分组。
export function buildNativeFeatureTree(
  records: NativeFeatureRecord[],
  sourceFileName: string,
  faceRefsByFeatureId: Record<string, string[]> = {}
): FeatureTreeNode[] {
  const nodes = new Map<string, FeatureTreeNode>();
  const ordered = [...records].sort(
    (left, right) => sequenceOf(left) - sequenceOf(right) || left.feature_id.localeCompare(right.feature_id)
  );
  ordered.forEach(record => {
    const kind = featureKind(record);
    const rawName = String(record.display_name || record.internal_name || record.feature_id);
    const displayName = kind === 'catpart' ? baseName(sourceFileName || rawName) : baseName(rawName);
    nodes.set(record.feature_id, {
      id: record.feature_id,
      parentId: record.parent_id || undefined,
      name: rawName,
      displayName,
      kind,
      nativeType: String(record.startup_type || record.native_type || '') || undefined,
      sourceRef: String(record.tree_path || record.internal_name || '') || undefined,
      sequence: sequenceOf(record),
      children: [],
      isSystem: kind === 'system',
      isContainer: CONTAINER_KINDS.has(kind),
      faceRefs: [...(faceRefsByFeatureId[record.feature_id] || [])],
      topologyRefs: [],
      hasGeometry: Boolean(faceRefsByFeatureId[record.feature_id]?.length),
      hasProperties: false,
      featureId: record.feature_id,
      sourceNodeId: record.feature_id,
      parameters: record.attributes,
      raw: record
    });
  });

  const roots: FeatureTreeNode[] = [];
  ordered.forEach(record => {
    const node = nodes.get(record.feature_id)!;
    const parent = record.parent_id ? nodes.get(record.parent_id) : undefined;
    if (parent && parent.id !== node.id) parent.children.push(node);
    else roots.push(node);
  });
  sortNodes(roots);

  for (const part of nodes.values()) {
    if (part.kind === 'part') {
      const datums = part.children.filter(child => child.kind === 'datum');
      if (datums.length) {
        const datumIds = new Set(datums.map(node => node.id));
        const group: FeatureTreeNode = {
          id: `datum-group:${part.id}`,
          parentId: part.id,
          name: '鍩哄噯鍏冪礌',
          displayName: '鍩哄噯鍏冪礌',
          kind: 'datum_group',
          sequence: Math.min(...datums.map(node => node.sequence)),
          children: datums,
          isSystem: false,
          isContainer: true,
          faceRefs: [],
          topologyRefs: [],
          hasGeometry: false,
          hasProperties: false
        };
        datums.forEach(node => {
          node.parentId = group.id;
        });
        part.children = [...part.children.filter(child => !datumIds.has(child.id)), group];
        sortNodes(part.children);
      }
    }
  }
  if (!roots.some(node => node.kind === 'catpart')) {
    return [
      {
        id: `source:${sourceFileName}`,
        name: sourceFileName,
        displayName: baseName(sourceFileName),
        kind: 'catpart',
        sequence: 0,
        children: roots,
        isSystem: false,
        isContainer: true,
        faceRefs: [],
        topologyRefs: [],
        hasGeometry: false,
        hasProperties: false
      }
    ];
  }
  return roots;
}

function categoryMatches(node: FeatureTreeNode, category: FeatureTreeCategory) {
  if (category === 'all') return true;
  if (category === 'mapped') return node.faceRefs.length > 0;
  if (category === 'sketch') return node.kind === 'sketch';
  if (category === 'solid') return SOLID_KINDS.has(node.kind);
  return node.kind === 'datum' || node.kind === 'datum_group';
}

function textMatches(node: FeatureTreeNode, query: string) {
  if (!query) return true;
  const haystack = [node.displayName, node.nativeType, node.id, node.sourceRef]
    .filter(Boolean)
    .join(' ')
    .toLocaleLowerCase();
  return haystack.includes(query.toLocaleLowerCase());
}

// 用途：隐藏系统节点时提升其业务后代；搜索和分类只裁剪展示副本，不改变原始树或选中 ID。
export function projectFeatureTree(
  source: FeatureTreeNode[],
  options: { showSystem: boolean; query?: string; category?: FeatureTreeCategory }
): FeatureTreeProjection {
  const query = options.query?.trim() || '';
  const category = options.category || 'all';

  function hideSystem(nodes: FeatureTreeNode[]): FeatureTreeNode[] {
    return nodes.flatMap(node => {
      const children = hideSystem(node.children);
      if (node.isSystem && !options.showSystem) return children;
      return [{ ...node, children }];
    });
  }

  function filterNodes(nodes: FeatureTreeNode[]): FeatureTreeNode[] {
    return nodes.flatMap(node => {
      const children = filterNodes(node.children);
      const ownMatch = textMatches(node, query) && categoryMatches(node, category);
      if (!ownMatch && !children.length) return [];
      return [{ ...node, children: ownMatch && query ? hideSystem(node.children) : children }];
    });
  }

  const nodes = filterNodes(hideSystem(source));
  const expandedKeys = flattenFeatureTree(nodes)
    .filter(node => node.children.length > 0)
    .map(node => node.id);
  return { nodes, expandedKeys };
}

export function flattenFeatureTree(nodes: FeatureTreeNode[]): FeatureTreeNode[] {
  const flattened: FeatureTreeNode[] = [];
  const visit = (node: FeatureTreeNode) => {
    flattened.push(node);
    node.children.forEach(visit);
  };
  nodes.forEach(visit);
  return flattened;
}

function nativeTreeName(record: NativeTreeRecord) {
  return String(record.display_text || record.display_name || record.label || record.internal_name || record.id);
}

function nativeTreeSelection(record: NativeTreeRecord) {
  const selection = record.selection || {};
  return {
    faceRefs: [...new Set((selection.mesh_face_ids || []).map(String))].sort(),
    topologyRefs: [...new Set((selection.topology_ids || []).map(String))].sort()
  };
}

function optionalRecordString(value: unknown) {
  return value ? String(value) : undefined;
}

export function buildUnifiedNativeTree(records: NativeTreeRecord[]): FeatureTreeNode[] {
  function convert(record: NativeTreeRecord, parentId?: string): FeatureTreeNode {
    const kind = nativeTreeKind(record);
    const { faceRefs, topologyRefs } = nativeTreeSelection(record);
    const rawName = nativeTreeName(record);
    const node: FeatureTreeNode = {
      id: String(record.id),
      parentId: record.parent_id ? String(record.parent_id) : parentId,
      name: rawName,
      displayName: baseName(rawName),
      kind,
      nodeKind: String(record.node_kind || ''),
      documentKind: String(record.document_kind || ''),
      nativeType: optionalRecordString(record.startup_type),
      sourceRef: optionalRecordString(record.tree_path || record.source_node_id),
      sequence: Number(record.source_index ?? Number.MAX_SAFE_INTEGER),
      children: (record.children || []).map(child => convert(child, String(record.id))),
      isSystem: kind === 'system',
      isContainer: CONTAINER_KINDS.has(kind) || Boolean(record.children?.length),
      faceRefs,
      topologyRefs,
      hasGeometry: Boolean(record.has_geometry || faceRefs.length || topologyRefs.length),
      hasProperties: Boolean(record.has_properties),
      instanceId: optionalRecordString(record.instance_id),
      referenceId: optionalRecordString(record.reference_id),
      featureId: optionalRecordString(record.feature_id),
      topologyId: optionalRecordString(record.topology_id),
      sourceNodeId: optionalRecordString(record.source_node_id),
      raw: record
    };
    sortNodes(node.children);
    return node;
  }
  const roots = records.map(record => convert(record));
  sortNodes(roots);
  return roots;
}

// 用途：返回纯文本片段供模板使用 mark 渲染，避免 v-html 和转义风险。
export function splitHighlight(text: string, query: string): Array<{ text: string; matched: boolean }> {
  const needle = query.trim();
  if (!needle) return [{ text, matched: false }];
  const index = text.toLocaleLowerCase().indexOf(needle.toLocaleLowerCase());
  if (index < 0) return [{ text, matched: false }];
  return [
    { text: text.slice(0, index), matched: false },
    { text: text.slice(index, index + needle.length), matched: true },
    { text: text.slice(index + needle.length), matched: false }
  ].filter(part => part.text.length > 0);
}
