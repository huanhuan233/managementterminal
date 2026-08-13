<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import {
  buildNativeFeatureTree,
  buildUnifiedNativeTree,
  flattenFeatureTree,
  projectFeatureTree,
  splitHighlight
} from './native-feature-tree';
import type {
  FeatureTreeCategory,
  FeatureTreeKind,
  FeatureTreeNode,
  NativeFeatureRecord,
  NativeTreeRecord
} from './native-feature-tree';

defineOptions({ name: 'NativeFeatureTree' });

const props = defineProps<{
  records: NativeFeatureRecord[];
  nativeTreeRecords?: NativeTreeRecord[];
  sourceFileName: string;
  selectedId: string;
  faceRefsByFeatureId?: Record<string, string[]>;
}>();

const emit = defineEmits<{
  select: [node: FeatureTreeNode];
  openProperties: [node: FeatureTreeNode];
  missingProperties: [node: FeatureTreeNode];
}>();

interface TreeNodeState {
  expanded: boolean;
}

interface TreeExposed {
  getNode: (key: string) => TreeNodeState | undefined;
  setCurrentKey: (key?: string) => void;
}

const treeRef = ref<TreeExposed | null>(null);
const query = ref('');
const showSystem = ref(false);
const category = ref<FeatureTreeCategory>('all');
const userExpandedKeys = ref<string[]>([]);
const savedExpandedKeys = ref<string[]>([]);

const sourceTree = computed(() =>
  props.nativeTreeRecords?.length
    ? buildUnifiedNativeTree(props.nativeTreeRecords)
    : buildNativeFeatureTree(props.records, props.sourceFileName, props.faceRefsByFeatureId || {})
);
const projection = computed(() =>
  projectFeatureTree(sourceTree.value, {
    showSystem: showSystem.value,
    query: query.value,
    category: category.value
  })
);
const visibleNodes = computed(() => flattenFeatureTree(projection.value.nodes));

const KIND_ICONS: Record<FeatureTreeKind, string> = {
  catproduct: 'lucide:boxes',
  product_assembly: 'lucide:package',
  product_instance: 'lucide:box',
  product_reference: 'lucide:box-open',
  catpart: 'lucide:file-box',
  part: 'lucide:box',
  datum_group: 'lucide:layers-3',
  datum: 'lucide:diamond',
  body: 'lucide:package-open',
  geometry_set: 'lucide:folder-cog',
  sketch: 'lucide:pencil-ruler',
  pad: 'lucide:package-plus',
  pocket: 'lucide:package-minus',
  hole: 'lucide:circle-dot-dashed',
  fillet: 'lucide:blend',
  chamfer: 'lucide:badge-minus',
  solid_feature: 'lucide:blocks',
  parameter: 'lucide:braces',
  system: 'lucide:cpu',
  unknown: 'lucide:component'
};

const KIND_LABELS: Partial<Record<FeatureTreeKind, string>> = {
  sketch: 'Sketch',
  pad: 'Pad',
  pocket: 'Pocket',
  hole: 'Hole',
  fillet: 'Fillet',
  chamfer: 'Chamfer'
};

// 用途：只默认展开业务骨架，避免 941 个节点首次渲染时全部展开。
function defaultExpandedKeys() {
  return flattenFeatureTree(sourceTree.value)
    .filter(node => node.children.length && ['catpart', 'part', 'datum_group', 'body'].includes(node.kind))
    .map(node => node.id);
}

function expansionKeys() {
  if (query.value || category.value !== 'all') return projection.value.expandedKeys;
  return userExpandedKeys.value.length ? userExpandedKeys.value : defaultExpandedKeys();
}

// 用途：搜索期间临时展开命中祖先，清空后恢复用户搜索前的展开集合和当前项。
async function syncTreeState() {
  await nextTick();
  const expanded = new Set(expansionKeys());
  visibleNodes.value.forEach(node => {
    const state = treeRef.value?.getNode(node.id);
    if (state) state.expanded = expanded.has(node.id);
  });
  treeRef.value?.setCurrentKey(props.selectedId || undefined);
  await nextTick();
  document
    .querySelector('.native-feature-tree .el-tree-node.is-current > .el-tree-node__content')
    ?.scrollIntoView({ block: 'nearest' });
}

function handleExpand(node: FeatureTreeNode) {
  if (query.value || category.value !== 'all') return;
  userExpandedKeys.value = [...new Set([...userExpandedKeys.value, node.id])];
}

function handleCollapse(node: FeatureTreeNode) {
  if (query.value || category.value !== 'all') return;
  userExpandedKeys.value = userExpandedKeys.value.filter(key => key !== node.id);
}

function handleSelect(node: FeatureTreeNode) {
  emit('select', node);
}

function handleOpenProperties(node: FeatureTreeNode) {
  emit('openProperties', node);
}

function handleNodeContextMenu(event: Event, node: FeatureTreeNode) {
  event.preventDefault();
  event.stopPropagation();
  emit('select', node);
  handleOpenProperties(node);
}

function iconFor(kind: FeatureTreeKind) {
  return KIND_ICONS[kind];
}

function kindLabel(node: FeatureTreeNode) {
  return KIND_LABELS[node.kind] || (node.isSystem ? node.nativeType : '');
}

function highlightParts(text: string) {
  return splitHighlight(text, query.value);
}

watch(
  sourceTree,
  () => {
    userExpandedKeys.value = defaultExpandedKeys();
  },
  { immediate: true }
);

watch(query, (value, previous) => {
  if (value && !previous) savedExpandedKeys.value = [...userExpandedKeys.value];
  if (!value && previous) userExpandedKeys.value = [...savedExpandedKeys.value];
  syncTreeState();
});
watch(
  [projection, () => props.selectedId],
  () => {
    syncTreeState();
  },
  { flush: 'post' }
);
</script>

<template>
  <section class="native-tree-browser">
    <div class="tree-toolbar">
      <ElInput v-model="query" clearable size="small" placeholder="搜索名称、类型或编号">
        <template #prefix><SvgIcon icon="lucide:search" /></template>
      </ElInput>
      <ElPopover trigger="click" placement="bottom-end" :width="190">
        <template #reference>
          <button type="button" class="filter-trigger" :class="{ active: category !== 'all' }" title="筛选原生特征">
            <SvgIcon icon="lucide:list-filter" />
          </button>
        </template>
        <div class="filter-options">
          <label
            v-for="option in [
              ['all', '全部节点'],
              ['mapped', '仅看有关联面'],
              ['sketch', '仅看草图'],
              ['solid', '仅看实体特征'],
              ['datum', '仅看基准元素']
            ]"
            :key="option[0]"
          >
            <input v-model="category" type="radio" :value="option[0]" />
            <span>{{ option[1] }}</span>
          </label>
        </div>
      </ElPopover>
    </div>

    <div class="native-tree-scroll">
      <ElTree
        v-if="projection.nodes.length"
        ref="treeRef"
        class="native-feature-tree"
        :data="projection.nodes"
        node-key="id"
        :props="{ label: 'displayName', children: 'children' }"
        :default-expanded-keys="defaultExpandedKeys()"
        :current-node-key="selectedId"
        :expand-on-click-node="false"
        :indent="20"
        highlight-current
        @node-click="handleSelect"
        @node-contextmenu="handleNodeContextMenu"
        @node-expand="handleExpand"
        @node-collapse="handleCollapse"
      >
        <template #default="{ data }">
          <ElTooltip
            :content="`${data.displayName}${data.nativeType ? ` · ${data.nativeType}` : ''}`"
            placement="right"
            :show-after="500"
          >
            <span
              class="feature-tree-row"
              :class="[`kind-${data.kind}`, { system: data.isSystem }]"
              @dblclick.stop="handleOpenProperties(data)"
            >
              <span class="node-icon"><SvgIcon :icon="iconFor(data.kind)" /></span>
              <span class="node-title">
                <template v-for="(part, index) in highlightParts(data.displayName)" :key="`${data.id}-${index}`">
                  <mark v-if="part.matched">{{ part.text }}</mark>
                  <span v-else>{{ part.text }}</span>
                </template>
              </span>
              <span v-if="data.faceRefs.length" class="mapping-dot" title="已建立 Feature–Face 映射" />
              <span v-if="data.hasProperties" class="property-dot" title="CATIA 属性可用" />
              <small v-if="kindLabel(data)" class="node-kind">{{ kindLabel(data) }}</small>
            </span>
          </ElTooltip>
        </template>
      </ElTree>
      <ElEmpty
        v-else
        :description="query || category !== 'all' ? '没有匹配的原生特征' : '没有可用的 CAA 原生特征索引'"
      />
    </div>

    <footer class="system-node-switch">
      <div>
        <strong>显示系统节点</strong>
        <span>CATPrtContainer、GSMInternal 等技术节点</span>
      </div>
      <ElSwitch v-model="showSystem" size="small" @change="syncTreeState" />
    </footer>
  </section>
</template>

<style scoped>
.native-tree-browser {
  display: flex;
  height: 100%;
  min-height: 0;
  flex-direction: column;
}
.tree-toolbar {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 34px;
  gap: 7px;
  padding: 9px 10px;
}
.filter-trigger {
  display: grid;
  width: 32px;
  height: 32px;
  place-items: center;
  border: 1px solid var(--el-border-color);
  border-radius: 6px;
  background: var(--el-bg-color);
  padding: 0;
}
.filter-trigger:hover,
.filter-trigger.active {
  border-color: var(--el-color-primary-light-5);
  color: var(--el-color-primary);
  background: var(--el-color-primary-light-9);
}
.filter-options {
  display: flex;
  flex-direction: column;
  gap: 9px;
}
.filter-options label {
  display: flex;
  cursor: pointer;
  align-items: center;
  gap: 8px;
  font-size: 13px;
}
.native-tree-scroll {
  min-height: 0;
  flex: 1;
  overflow: auto;
  padding: 1px 9px 8px 7px;
}
.native-feature-tree {
  --el-tree-node-hover-bg-color: var(--el-fill-color-light);
  width: max-content;
  min-width: 100%;
  background: transparent;
}
.native-feature-tree :deep(.el-tree-node) {
  position: relative;
}
.native-feature-tree :deep(.el-tree-node__children) {
  border-left: 1px solid #d7dce7;
  margin-left: 12px;
}
.native-feature-tree :deep(.el-tree-node__content) {
  position: relative;
  height: 28px;
  border-radius: 3px;
  margin: 0;
  padding-right: 8px;
}
.native-feature-tree :deep(.el-tree-node__content::before) {
  position: absolute;
  top: 14px;
  left: -12px;
  width: 12px;
  border-top: 1px solid #d7dce7;
  content: '';
}
.native-feature-tree :deep(.el-tree-node:first-child > .el-tree-node__content::before) {
  left: -8px;
  width: 8px;
}
.native-feature-tree :deep(.el-tree-node.is-current > .el-tree-node__content) {
  color: var(--el-color-primary);
  background: var(--el-color-primary-light-9);
  box-shadow: inset 2px 0 0 var(--el-color-primary);
}
.native-feature-tree :deep(.el-tree-node__expand-icon) {
  color: var(--el-text-color-regular);
}
.feature-tree-row {
  display: flex;
  width: max-content;
  min-width: 100%;
  align-items: center;
  gap: 6px;
  padding-right: 8px;
}
.feature-tree-row.system {
  color: var(--el-text-color-secondary);
}
.node-icon {
  display: grid;
  flex: 0 0 18px;
  color: var(--el-text-color-regular);
  font-size: 17px;
  place-items: center;
}
.kind-catproduct .node-icon,
.kind-product_assembly .node-icon,
.kind-product_instance .node-icon,
.kind-product_reference .node-icon {
  color: #5f6673;
}
.kind-datum .node-icon {
  color: var(--el-text-color-secondary);
}
.kind-body .node-icon {
  color: var(--el-color-success);
}
.kind-geometry_set .node-icon {
  color: var(--el-color-warning);
}
.kind-pocket .node-icon,
.kind-hole .node-icon,
.kind-fillet .node-icon {
  color: var(--el-color-warning);
}
.node-title {
  flex: 0 0 auto;
  min-width: 0;
  max-width: 360px;
  overflow: visible;
  white-space: nowrap;
}
.node-title mark {
  border-radius: 2px;
  background: var(--el-color-warning-light-7);
  color: inherit;
  padding: 0 1px;
}
.node-kind {
  max-width: 72px;
  flex: 0 0 auto;
  color: var(--el-text-color-secondary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.mapping-dot {
  width: 6px;
  height: 6px;
  flex: 0 0 6px;
  border-radius: 50%;
  background: var(--el-color-success);
}
.property-dot {
  width: 6px;
  height: 6px;
  flex: 0 0 6px;
  border-radius: 50%;
  background: var(--el-color-warning);
}
.system-node-switch {
  display: flex;
  min-height: 68px;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  border-top: 1px solid var(--el-border-color-lighter);
  padding: 9px 12px;
}
.system-node-switch div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}
.system-node-switch strong {
  font-size: 13px;
}
.system-node-switch span {
  color: var(--el-text-color-secondary);
  font-size: 11px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
