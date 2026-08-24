<script setup lang="ts">
/**
 * ComponentLibraryCatalog.vue
 * ===========================
 * 用途：递归展示后端返回的系统库根、分类和零件类型，支持任意层级展开。
 */

import { computed, ref, watch } from 'vue'

interface CatalogItem {
  id: string
  label: string
  code: string
  count: number
  parentId?: string
  depth: number
  nodeType: 'library' | 'family' | 'type'
}

const props = defineProps<{
  catalogItems: CatalogItem[]
  selectedCatalogId: string
  loading: boolean
}>()

const emit = defineEmits<{
  select: [catalogId: string]
}>()

const expandedNodeIds = ref<Set<string>>(new Set())

// 用途：首次取得目录时只展开两个系统库根，分类节点由用户按需继续展开。
watch(() => props.catalogItems, (items) => {
  if (expandedNodeIds.value.size === 0) {
    const libraryIds = items
      .filter(item => item.nodeType === 'library' && isParent(item.id))
      .map(item => item.id)
    expandedNodeIds.value = new Set(libraryIds)
  }
}, { immediate: true })

const visibleItems = computed(() => {
  const byId = new Map(props.catalogItems.map(item => [item.id, item]))
  return props.catalogItems.filter(item => {
    let parentId = item.parentId
    while (parentId) {
      if (!expandedNodeIds.value.has(parentId)) return false
      parentId = byId.get(parentId)?.parentId
    }
    return true
  })
})

function isParent(id: string): boolean {
  return props.catalogItems.some(item => item.parentId === id)
}

function isExpanded(id: string): boolean {
  return expandedNodeIds.value.has(id)
}

function handleItemClick(item: CatalogItem) {
  if (isParent(item.id)) {
    const next = new Set(expandedNodeIds.value)
    if (next.has(item.id)) {
      next.delete(item.id)
    } else {
      next.add(item.id)
    }
    expandedNodeIds.value = next
  }
  emit('select', item.id)
}

function expandAll() {
  const next = new Set<string>()
  for (const item of props.catalogItems) {
    if (isParent(item.id)) next.add(item.id)
  }
  expandedNodeIds.value = next
}

function toggleAll() {
  const parentCount = props.catalogItems.filter(item => isParent(item.id)).length
  if (expandedNodeIds.value.size === parentCount) {
    expandedNodeIds.value = new Set()
  } else {
    expandAll()
  }
}

function isSelected(id: string) {
  return props.selectedCatalogId === id
}
</script>

<template>
  <div class="catalog-panel">
    <div class="catalog-header">
      <span class="catalog-title">目录层级</span>
      <button class="catalog-expand-button" type="button" @click="toggleAll">展开/收起</button>
    </div>

    <div v-loading="loading" class="catalog-body" element-loading-text="加载目录中…">
      <template v-for="item in visibleItems" :key="item.id">
        <div
          class="catalog-item"
          :class="{
            active: isSelected(item.id),
            expanded: isExpanded(item.id),
            'root-item': item.nodeType === 'library',
            'family-item': item.nodeType === 'family',
            'type-item': item.nodeType === 'type'
          }"
          :style="{ paddingLeft: `${10 + item.depth * 16}px` }"
          :title="item.label"
          @click="handleItemClick(item)"
        >
          <span class="catalog-item-icon">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M22 19a2 2 0 01-2 2H4a2 2 0 01-2-2V5a2 2 0 012-2h5l2 3h9a2 2 0 012 2z" />
            </svg>
          </span>
          <span class="catalog-item-label">{{ item.label }}</span>
          <span class="catalog-item-count">{{ item.count }}</span>
          <span v-if="isParent(item.id)" class="expand-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor" :class="{ rotated: isExpanded(item.id) }">
              <path d="M12 16l-6-6h12z" />
            </svg>
          </span>
        </div>

      </template>

      <ElEmpty v-if="!loading && !catalogItems.length" description="" :image-size="40" />
    </div>

    <!-- Info footer -->
    <div class="catalog-footer">
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="12" cy="12" r="10" />
        <line x1="12" y1="16" x2="12" y2="12" />
        <line x1="12" y1="8" x2="12.01" y2="8" />
      </svg>
      <span>支持多级目录扩展。新增目录将同步至用户端图元选择器与检索命名空间。</span>
    </div>
  </div>
</template>

<style scoped>
.catalog-panel {
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: #fff;
  border: 1px solid #e5eaf2;
  border-radius: 12px;
}

.catalog-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 16px 10px;
}

.catalog-title {
  font-size: 14px;
  font-weight: 600;
  color: #1a2332;
}

.catalog-expand-button {
  border: 0;
  background: transparent;
  color: #7d8796;
  cursor: pointer;
  font-size: 11px;
}

.catalog-body {
  flex: 1;
  overflow-y: auto;
  padding: 2px 8px 8px;
}

.catalog-item {
  display: flex;
  align-items: center;
  gap: 8px;
  height: 38px;
  padding: 0 10px;
  margin-bottom: 3px;
  border-radius: 8px;
  cursor: pointer;
  font-size: 13.5px;
  line-height: 1.35;
  color: #5a6a7e;
  transition: background-color 0.15s;
}

.catalog-item:hover {
  background-color: #f0f2f6;
}

.catalog-item.active {
  background-color: #f0edff;
  color: #6c5ce7;
  font-weight: 500;
}

.catalog-item.active .catalog-item-icon {
  color: #6c5ce7;
}

.catalog-item-icon.child-icon {
  color: #9aa6b5;
}

.catalog-item.active .catalog-item-icon.child-icon {
  color: #6c5ce7;
}

.catalog-item.root-item {
  margin-top: 4px;
  font-weight: 600;
  color: #1a2332;
  border-top: 1px solid #edf0f5;
}

.catalog-item.family-item {
  font-weight: 500;
  color: #3f4d62;
}

.catalog-item.type-item {
  height: 36px;
  margin-bottom: 3px;
  color: #6a778a;
}

.catalog-item-icon {
  display: inline-flex;
  flex-shrink: 0;
  color: #8e99aa;
}

.catalog-item.active .catalog-item-icon {
  color: #6c5ce7;
}

.catalog-item-label {
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.catalog-item-count {
  flex-shrink: 0;
  min-width: 20px;
  text-align: right;
  font-size: 12px;
  color: #8e99aa;
  font-variant-numeric: tabular-nums;
}

.catalog-divider {
  height: 1px;
  margin: 4px 10px;
  background: #e5eaf2;
}

.catalog-footer {
  display: flex;
  align-items: flex-start;
  gap: 6px;
  padding: 10px 14px;
  font-size: 11px;
  line-height: 1.5;
  color: #8e99aa;
  border-top: 1px solid #e5eaf2;
}

.catalog-footer svg {
  margin-top: 1px;
  flex-shrink: 0;
}

.expand-icon {
  display: inline-flex;
  flex-shrink: 0;
  color: #9aa6b5;
  transition: transform 0.2s;
}

.expand-icon svg {
  transition: transform 0.2s;
}

.expand-icon svg.rotated {
  transform: rotate(180deg);
}
</style>
