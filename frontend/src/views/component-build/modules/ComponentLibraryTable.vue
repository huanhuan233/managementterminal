<script setup lang="ts">
/**
 * ComponentLibraryTable.vue
 * =========================
 * Right-side table showing component list for the component library page.
 *
 * Displays build-level items from the flattened tree, with search, sort,
 * and row-level actions. Each row represents one build (one version of a
 * component).
 */

import { computed } from 'vue'
import { getDemoCallVolume } from '../mock/library-ui'

interface BuildRow {
  id: string                 // build_id
  componentId: string        // component_id
  componentName: string
  componentType: string
  family: string
  standardNumber: string
  version: string
  status: string
  categoryCode: string
  partTypeCode: string
  catalogPath: string
  cadRevisionId: string | null
  drawingTaskId: string | null
  hasStep: boolean
  sourceFormat: 'STEP' | 'CATPART' | 'CATPRODUCT' | null
  hasDrawing: boolean
  paramFields: { dn: string | null; pn: string | null }
}

const props = defineProps<{
  rows: BuildRow[]
  totalCount: number
  loading: boolean
  keyword: string
  selectedBuildId: string | null
}>()

const emit = defineEmits<{
  edit: [buildId: string]
  deleteBuild: [buildId: string]
  viewCadModel: [buildId: string, revisionId: string]
  startStepParsing: [buildId: string]
  viewComponentSpec: [buildId: string]
  viewYaml: [buildId: string]
  runFusion: [buildId: string]
  rowClick: [buildId: string]
  'update:keyword': [keyword: string]
}>()

function statusTagType(status: string): 'success' | 'info' | 'primary' | 'warning' | 'danger' {
  const map: Record<string, 'success' | 'info' | 'primary' | 'warning' | 'danger'> = {
    released: 'success',
    saved: 'success',
    completed: 'success',
    yaml_ready: 'success',
    parsing_sources: 'primary',
    uploading: 'primary',
    aligning: 'primary',
    sources_ready: 'success',
    review_required: 'warning',
    sources_partial: 'success',
    source_failed: 'danger',
    ready: 'success',
    failed: 'danger',
    draft: 'info',
    pending: 'info',
    missing: 'info'
  }
  return map[status] || 'info'
}

function simpleStatusLabel(status: string): string {
  const labels: Record<string, string> = {
    draft: '草稿',
    uploading: '上传中',
    parsing_sources: '解析中',
    source_failed: '失败',
    ready: '处理完成',
    sources_ready: '处理完成',
    sources_partial: '处理完成',
    aligning: '对齐中',
    review_required: '待审核',
    yaml_ready: 'YAML就绪',
    saved: '已保存',
    released: '已发布',
    completed: '处理完成',
    review_ready: '待审核',
    failed: '失败',
    waiting_for_step: '等待STEP',
  }
  return labels[status] || status
}

function handleRowClick(row: BuildRow) {
  emit('rowClick', row.id)
}

function handleEditClick(e: MouseEvent, row: BuildRow) {
  e.stopPropagation()
  emit('edit', row.id)
}

function handleMoreCommand(command: string, row: BuildRow) {
  switch (command) {
    case 'view-cad':
      if (row.cadRevisionId) emit('viewCadModel', row.id, row.cadRevisionId)
      break
    case 'start-step':
      emit('startStepParsing', row.id)
      break
    case 'view-spec':
      emit('viewComponentSpec', row.id)
      break
    case 'view-yaml':
      emit('viewYaml', row.id)
      break
    case 'run-fusion':
      emit('runFusion', row.id)
      break
  }
}

function handleDeleteClick(e: MouseEvent, row: BuildRow) {
  e.stopPropagation()
  emit('deleteBuild', row.id)
}
</script>

<template>
  <div class="table-wrapper">
    <!-- Search bar & count -->
    <div class="table-toolbar">
      <div class="search-area">
        <el-input
          :model-value="keyword"
          clearable
          class="table-search"
          placeholder="按名称 / 编码 / 国标号检索…"
          @update:model-value="$emit('update:keyword', $event)"
        >
          <template #prefix>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="11" cy="11" r="8" />
              <line x1="21" y1="21" x2="16.65" y2="16.65" />
            </svg>
          </template>
        </el-input>
      </div>
      <span class="total-count">共 {{ totalCount }} 条元数据</span>
    </div>

    <!-- Table -->
    <div v-loading="loading" class="table-body" element-loading-text="加载图元列表中…">
      <el-table
        v-if="rows.length"
        :data="rows"
        style="width: 100%"
        :highlight-current-row="true"
        :current-row-key="selectedBuildId || undefined"
        row-key="id"
        size="default"
        @row-click="handleRowClick"
      >
        <!-- 图元列 -->
        <el-table-column label="图元" min-width="180" show-overflow-tooltip>
          <template #default="{ row }">
            <div class="component-cell">
              <span class="component-icon">
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M21 16V8a2 2 0 00-1-1.73l-7-4a2 2 0 00-2 0l-7 4A2 2 0 002 8v8a2 2 0 001 1.73l7 4a2 2 0 002 0l7-4A2 2 0 0021 16z" />
                  <polyline points="3.29 7 12 12 20.71 7" />
                  <line x1="12" y1="22" x2="12" y2="12" />
                </svg>
              </span>
              <div class="component-info">
                <span class="component-name">{{ row.componentName }}</span>
              </div>
            </div>
          </template>
        </el-table-column>

        <!-- 编码列 -->
        <el-table-column label="编码" width="140" show-overflow-tooltip>
          <template #default="{ row }">
            <span class="code-primary">{{ row.componentId }}</span>
          </template>
        </el-table-column>

        <!-- 执行标准列 -->
        <el-table-column label="执行标准" min-width="130" show-overflow-tooltip>
          <template #default="{ row }">
            <span class="standard-text">{{ row.standardNumber || '-' }}</span>
          </template>
        </el-table-column>

        <!-- 参数化字段列 -->
        <el-table-column label="参数化字段" min-width="170" show-overflow-tooltip>
          <template #default="{ row }">
            <div class="params-cell">
              <template v-if="row.paramFields.dn">
                <el-tag size="small" effect="light" round>DN={{ row.paramFields.dn }}</el-tag>
              </template>
              <template v-if="row.paramFields.pn">
                <el-tag size="small" effect="light" round>PN={{ row.paramFields.pn }}</el-tag>
              </template>
              <el-tag v-if="row.hasStep" size="small" type="success" effect="light" round>{{ row.sourceFormat || '模型' }}</el-tag>
              <el-tag v-else size="small" type="info" effect="light" round>STEP</el-tag>
              <el-tag v-if="row.hasDrawing" size="small" type="success" effect="light" round>图纸</el-tag>
              <el-tag v-else size="small" type="info" effect="light" round>图纸</el-tag>
            </div>
          </template>
        </el-table-column>

        <!-- 版本列 -->
        <el-table-column label="版本" width="80">
          <template #default="{ row }">
            <span class="version-text">{{ row.version || '-' }}</span>
          </template>
        </el-table-column>

        <!-- 状态列 -->
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="statusTagType(row.status)" size="small" effect="light">
              {{ simpleStatusLabel(row.status) }}
            </el-tag>
          </template>
        </el-table-column>

        <!-- 调用量列 -->
        <el-table-column label="调用量" width="90" align="right">
          <template #default="{ row }">
            <span class="call-volume">{{ getDemoCallVolume(row.id) }}</span>
          </template>
        </el-table-column>

        <!-- 操作列 -->
        <el-table-column label="操作" width="130" fixed="right">
          <template #default="{ row }">
            <div class="action-cell" @click.stop>
              <el-tooltip content="编辑图元" placement="top">
                <el-button text circle size="small" @click="handleEditClick($event, row)">
                  <template #icon>
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M11 4H4a2 2 0 00-2 2v14a2 2 0 002 2h14a2 2 0 002-2v-7" />
                      <path d="M18.5 2.5a2.121 2.121 0 013 3L12 15l-4 1 1-4 9.5-9.5z" />
                    </svg>
                  </template>
                </el-button>
              </el-tooltip>

              <el-dropdown trigger="click" @command="(cmd: string) => handleMoreCommand(cmd, row)">
                <el-button text circle size="small">
                  <template #icon>
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
                      <circle cx="12" cy="5" r="2" />
                      <circle cx="12" cy="12" r="2" />
                      <circle cx="12" cy="19" r="2" />
                    </svg>
                  </template>
                </el-button>
                <template #dropdown>
                  <el-dropdown-menu>
                    <el-dropdown-item command="view-cad" :disabled="!row.cadRevisionId">
                      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px"><path d="M21 16V8a2 2 0 00-1-1.73l-7-4a2 2 0 00-2 0l-7 4A2 2 0 002 8v8a2 2 0 001 1.73l7 4a2 2 0 002 0l7-4A2 2 0 0021 16z"/></svg>
                      查看模型
                    </el-dropdown-item>
                    <el-dropdown-item divider />
                    <el-dropdown-item command="start-step" :disabled="!row.cadRevisionId">
                      启动 STEP 解析
                    </el-dropdown-item>
                    <el-dropdown-item divider />
                    <el-dropdown-item command="view-spec">
                      查看 ComponentSpec
                    </el-dropdown-item>
                    <el-dropdown-item command="view-yaml">
                      查看 YAML
                    </el-dropdown-item>
                    <el-dropdown-item command="run-fusion" :disabled="!row.cadRevisionId && !row.drawingTaskId">
                      数据融合
                    </el-dropdown-item>
                  </el-dropdown-menu>
                </template>
              </el-dropdown>

              <el-tooltip content="删除图元" placement="top">
                <el-button text circle size="small" @click="handleDeleteClick($event, row)">
                  <template #icon>
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <polyline points="3 6 5 6 21 6" />
                      <path d="M19 6v14a2 2 0 01-2 2H7a2 2 0 01-2-2V6m3 0V4a2 2 0 012-2h4a2 2 0 012 2v2" />
                      <line x1="10" y1="11" x2="10" y2="17" />
                      <line x1="14" y1="11" x2="14" y2="17" />
                    </svg>
                  </template>
                </el-button>
              </el-tooltip>
            </div>
          </template>
        </el-table-column>
      </el-table>

      <el-empty v-if="!loading && !rows.length" description="暂无图元" :image-size="58" />
    </div>
  </div>
</template>

<script lang="ts">
export default { name: 'ComponentLibraryTable' }
</script>

<style scoped>
.table-wrapper {
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: #fff;
  border: 1px solid #e5eaf2;
  border-radius: 12px;
}

.table-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 14px 16px 10px;
}

.search-area {
  flex: 1;
}

.table-search {
  height: 36px;
}

.table-search :deep(.el-input__wrapper) {
  border-radius: 9px;
  background: #fff;
  border: 1px solid #e5eaf2;
  box-shadow: none;
}

.table-search :deep(.el-input__inner) {
  font-size: 13px;
}

.total-count {
  flex-shrink: 0;
  font-size: 12px;
  color: #8e99aa;
}

.table-body {
  flex: 1;
  min-height: 300px;
  overflow-y: auto;
}

.table-body :deep(.el-table) {
  --el-table-border-color: transparent;
  --el-table-header-background-color: #f7f9fc;
  border-top: 1px solid #e5eaf2;
}

.table-body :deep(.el-table th) {
  font-size: 12px;
  font-weight: 600;
  color: #5a6a7e;
  background-color: #f7f9fc;
  padding: 8px 12px;
}

.table-body :deep(.el-table td) {
  padding: 10px 12px;
  font-size: 13px;
}

.table-body :deep(.el-table__row) {
  height: 58px;
  cursor: pointer;
}

.table-body :deep(.el-table__row:hover) {
  background-color: #f7f9fc;
}

.table-body :deep(.el-table__body tr.current-row > td) {
  background-color: #f0edff;
}

.component-cell {
  display: flex;
  align-items: center;
  gap: 10px;
}

.component-icon {
  display: inline-flex;
  flex-shrink: 0;
  color: #6c5ce7;
  background: #f0edff;
  width: 32px;
  height: 32px;
  align-items: center;
  justify-content: center;
  border-radius: 8px;
}

.component-info {
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.component-name {
  font-weight: 500;
  color: #1a2332;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.component-sub {
  font-size: 12px;
  color: #8e99aa;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.code-cell {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.code-primary {
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  font-size: 12px;
  color: #6c5ce7;
  font-weight: 500;
}

.code-secondary {
  font-size: 12px;
  color: #8e99aa;
}

.params-cell {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
  justify-content: center;
}

.params-cell .el-tag {
  margin: 0;
}

.status-cell {
  display: flex;
  flex-direction: column;
  gap: 4px;
  align-items: flex-start;
}

.version-text {
  font-size: 13px;
  font-weight: 500;
  color: #1a2332;
}

.call-volume {
  font-size: 13px;
  font-variant-numeric: tabular-nums;
  color: #5a6a7e;
}

.action-cell {
  display: flex;
  align-items: center;
  gap: 2px;
}

.action-cell .el-button {
  --el-button-size: 28px;
}

.action-cell .el-button:hover {
  color: #6c5ce7;
  background: #f0edff;
}
</style>
