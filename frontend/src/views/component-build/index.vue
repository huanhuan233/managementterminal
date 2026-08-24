<script setup lang="ts">
/**
 * index.vue——图元库统一管理页面
 * =================================
 * 用途：编排左侧目录、右侧零件表、编辑弹窗和 YAML 预览。
 *
 * 保留原文件树版本中的加载、增删改、解析、融合和规范编辑业务逻辑。
 */

import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { ElMessageBox } from 'element-plus';
import { useRoute, useRouter } from 'vue-router';
import {
  createComponentBuild,
  deleteComponentBuild,
  fetchComponentBuild,
  fetchComponentBuildCatalog,
  fetchComponentBuildStatus,
  fetchComponentBuildTree,
  fetchComponentSpec,
  fuseComponentBuild,
  retryComponentBuild,
  saveComponentSpec,
  updateComponentBuild
} from '@/service/api';
import { isOfflineRequestError, loadComponentSpecWithFallback } from './component-spec-loader';
import { buildCatalogNavigation, type CatalogNavigationItem } from './catalog-navigation';
import { modelViewerLocation } from './model-viewer-route';
import ComponentLibraryCatalog from './modules/ComponentLibraryCatalog.vue';
import ComponentLibraryTable from './modules/ComponentLibraryTable.vue';
import ComponentLibraryDialog from './modules/ComponentLibraryDialog.vue';

defineOptions({ name: 'ComponentBuild' });

type ComponentTreeNode = Api.ComponentBuild.TreeNode;
type RawTreeNode = Api.ComponentBuild.RawTreeNode;

const route = useRoute();
const router = useRouter();
const requestController = new AbortController();

// 用途：保存对子组件公开方法的引用。
const libraryDialogRef = ref<InstanceType<typeof ComponentLibraryDialog> | null>(null);

// 用途：保存页面核心状态。
const treeLoading = ref(false);
const catalogLoading = ref(false);
const refreshing = ref(false);
const submitting = ref(false);
const parsingRole = ref<Api.ComponentBuild.RetryRole | null>(null);
const searchKeyword = ref('');
const treeData = ref<ComponentTreeNode[]>([]);
const catalog = ref<Api.ComponentBuild.CatalogCategory[]>([]);
const selectedCatalogId = ref('__root__');
const selectedBuildId = ref('');
const selectedBuild = ref<Api.ComponentBuild.BuildDetail | null>(null);
const cachedBuildDetails = ref<Record<string, Api.ComponentBuild.BuildDetail>>({});
const buildStatuses = ref<Record<string, Api.ComponentBuild.BuildStatus>>({});
const pollTimer = ref<number | null>(null);
const polling = ref(false);
const statusUnavailable = ref(false);
const componentSpec = ref<Api.ComponentBuild.ComponentSpecDocument | null>(null);
const componentSpecLoading = ref(false);
const componentSpecSaving = ref(false);
let componentSpecRequestSequence = 0;

// 用途：保存弹窗继续使用的融合状态。
const fusionReport = ref<Api.ComponentBuild.FusionResponse | null>(null);
const fusionLoading = ref(false);

// 用途：集中声明页面派生状态。
const catalogItems = computed<CatalogNavigationItem[]>(() => buildCatalogNavigation(treeData.value));

const flatBuildRows = computed(() => {
  return flattenBuilds(treeData.value).map(build => {
    const detail = buildDetailMap.value[build.id]
    // 用途：依次采用后端详情缓存、当前构建和目录节点中已有的真实字段。
    const cached = cachedBuildDetails.value[build.id]
    const merged = cached || (selectedBuild.value?.id === build.id ? selectedBuild.value : null) || detail
    return {
      id: build.id,
      componentId: merged?.component_id || '',
      componentName: merged?.component_name || '',
      componentType: merged?.component_type || '',
      family: merged?.family || findCatalogLabel(build.category_code),
      standardNumber: merged?.standard_number || '',
      version: merged?.version || '',
      status: build.status,
      statusLabel: statusLabel(build.status),
      categoryCode: build.category_code || '',
      partTypeCode: build.part_type_code || '',
      catalogPath: merged?.catalog_path || '',
      cadRevisionId: merged?.cad_revision_id || null,
      drawingTaskId: merged?.drawing_task_id || null,
      hasStep: Boolean(merged?.cad_revision_id),
      sourceFormat: merged?.source_format || build.source_format || null,
      hasDrawing: Boolean(merged?.drawing_task_id),
      paramFields: {
        dn: merged?.default_dn != null ? String(merged.default_dn) : null,
        pn: merged?.default_pn != null ? String(merged.default_pn) : null,
      }
    }
  });
});

const filteredRows = computed(() => {
  const keyword = searchKeyword.value.trim().toLowerCase()
  if (!keyword) return flatBuildRows.value
  return flatBuildRows.value.filter(row => {
    return [
      row.componentName, row.componentId, row.componentType, row.family,
      row.standardNumber, row.version, row.statusLabel
    ].some(val => val.toLowerCase().includes(keyword))
  })
});

const displayedRows = computed(() => {
  if (selectedCatalogId.value === '__root__') return filteredRows.value
  const selectedItem = catalogItems.value.find(c => c.id === selectedCatalogId.value)
  if (selectedItem?.nodeType === 'type') {
    return filteredRows.value.filter(row => row.partTypeCode === selectedItem.partTypeCode && row.categoryCode === selectedItem.categoryCode)
  }
  if (selectedItem) {
    const categories = new Set(selectedItem.descendantCategoryCodes)
    return filteredRows.value.filter(row => categories.has(row.categoryCode))
  }
  return filteredRows.value.filter(row => row.categoryCode === selectedCatalogId.value)
});

const buildDetailMap = computed(() => {
  const map: Record<string, Api.ComponentBuild.BuildDetail> = {}
  for (const build of allBuildNodes(treeData.value)) {
    if (build.component_name && build.component_id) {
      map[build.id] = {
        id: build.id,
        catalog_node_id: null,
        catalog_path: null,
        component_id: build.component_id || '',
        component_name: build.component_name || '',
        component_type: '',
        component_subtype: null,
        family: findCatalogLabel(build.category_code),
        standard_number: '',
        version: '',
        default_dn: null,
        default_pn: null,
        cad_model_id: null,
        cad_revision_id: null,
        drawing_task_id: null,
        status: build.status,
        status_message: null,
        error_code: null,
        error_message: null,
        created_at: '',
        updated_at: ''
      }
    }
  }
  // 用途：合并当前构建已经从后端取得的详情。
  if (selectedBuild.value?.id && map[selectedBuild.value.id]) {
    map[selectedBuild.value.id] = {
      ...map[selectedBuild.value.id],
      ...selectedBuild.value
    }
  }
  return map
});

// 用途：集中放置页面辅助函数。
function findCatalogLabel(code: string | null | undefined): string {
  if (!code) return ''
  const cat = catalog.value.find(c => c.category_code === code)
  if (!cat) return ''
  return `${cat.label} · ${cat.label_en || ''}`
}

function flattenBuilds(nodes: ComponentTreeNode[], parentCategoryCode?: string | null, parentPartTypeCode?: string | null): ComponentTreeNode[] {
  const result: ComponentTreeNode[] = []
  for (const node of nodes) {
    // 用途：节点缺少分类或零件类型编码时，沿真实父目录继承。
    if (!node.category_code && parentCategoryCode) {
      node.category_code = parentCategoryCode
    }
    if (!node.part_type_code && parentPartTypeCode) {
      node.part_type_code = parentPartTypeCode
    }
    if (node.node_type === 'build') {
      result.push(node)
    }
    result.push(...flattenBuilds(
      node.children,
      node.category_code || parentCategoryCode,
      node.part_type_code || parentPartTypeCode
    ))
  }
  return result
}

function allBuildNodes(nodes: ComponentTreeNode[], parentCategoryCode?: string | null, parentPartTypeCode?: string | null): ComponentTreeNode[] {
  const result: ComponentTreeNode[] = []
  for (const node of nodes) {
    // 用途：节点缺少分类或零件类型编码时，沿真实父目录继承。
    if (!node.category_code && parentCategoryCode) {
      node.category_code = parentCategoryCode
    }
    if (!node.part_type_code && parentPartTypeCode) {
      node.part_type_code = parentPartTypeCode
    }
    if (node.node_type === 'build') result.push(node)
    result.push(...allBuildNodes(
      node.children,
      node.category_code || parentCategoryCode,
      node.part_type_code || parentPartTypeCode
    ))
  }
  return result
}

function countBuildsInCategory(code: string, parentCode?: string): number {
  return allBuildNodes(treeData.value).filter(node => {
    if (parentCode) {
      // 用途：同时按零件类型编码和分类编码筛选。
      return node.part_type_code === code && node.category_code === parentCode
    }
    return node.category_code === code
  }).length
}

function statusLabel(status: string) {
  const labels: Record<string, string> = {
    draft: '草稿',
    uploading: '上传中',
    parsing_sources: '解析中',
    source_failed: '来源失败',
    sources_ready: '处理完成',
    sources_partial: '处理完成',
    aligning: '字段对齐中',
    review_required: '待人工处理',
    yaml_ready: 'YAML 就绪',
    saved: '已保存',
    released: '已发布',
    completed: '处理完成',
    ready: '处理完成',
    review_ready: '待审核',
    failed: '解析失败',
    waiting_for_step: '等待 STEP',
    missing: '未上传',
    pending: '等待处理',
    future: '后续能力'
  }
  return labels[status] || status
}

function isFailure(status: string) {
  return status === 'failed' || status === 'source_failed'
}

function futureLabel(nodeType: Api.ComponentBuild.NodeType) {
  if (nodeType === 'fusion') return '数据融合'
  if (nodeType === 'yaml' || nodeType === 'component_spec') return 'ComponentSpec'
  return '后续能力'
}

function normalizeNodeType(nodeType?: string): Api.ComponentBuild.NodeType {
  const aliases: Record<string, Api.ComponentBuild.NodeType> = {
    data_fusion: 'fusion',
    publish_validation: 'future'
  }
  const supported = new Set<Api.ComponentBuild.NodeType>([
    'root', 'library', 'family', 'type', 'subtype', 'component', 'build', 'folder',
    'reference_step', 'drawing', 'component_spec', 'fusion', 'yaml', 'future'
  ])
  if (nodeType && supported.has(nodeType as Api.ComponentBuild.NodeType)) return nodeType as Api.ComponentBuild.NodeType
  return aliases[nodeType || ''] || 'future'
}

function normalizeTree(nodes: RawTreeNode[], parentBuildId: string | null = null, parentId = 'tree'): ComponentTreeNode[] {
  return nodes.map((node, index) => {
    const nodeType = normalizeNodeType(node.node_type)
    const buildId = node.build_id || (nodeType === 'build' ? node.id || null : parentBuildId)
    const id = node.id || `${parentId}:${nodeType}:${index}`
    const children = normalizeTree((node.children || []) as RawTreeNode[], buildId, id)
    return {
      id,
      label: node.label || node.name || futureLabel(nodeType),
      label_en: node.label_en || null,
      node_type: nodeType,
      status: node.status || (nodeType === 'future' || nodeType === 'fusion' || nodeType === 'yaml' ? 'future' : 'pending'),
      progress: typeof node.progress === 'number' ? node.progress : null,
      disabled: Boolean(node.disabled) || nodeType === 'future' || nodeType === 'yaml',
      build_id: buildId,
      library_code: node.library_code || null,
      category_code: node.category_code || null,
      part_type_code: node.part_type_code || null,
      component_id: node.component_id || null,
      component_name: node.component_name || null,
      target: node.target || null,
      status_label: node.status_label || null,
      status_message: node.status_message || null,
      error_code: node.error_code || null,
      error_message: node.error_message || null,
      source_format: node.source_format || null,
      processing_route: node.processing_route || null,
      current_stage: node.current_stage || null,
      children
    }
  })
}

function findNodeById(nodes: ComponentTreeNode[], id: string): ComponentTreeNode | null {
  for (const node of nodes) {
    if (node.id === id) return node;
    const found = findNodeById(node.children, id)
    if (found) return found
  }
  return null
}

function hasPendingBuilds() {
  return allBuildNodes(treeData.value).some(node => node.status === 'uploading' || node.status === 'parsing_sources')
}

function formatProgress(progress: number | null | undefined) {
  return typeof progress === 'number' ? `${Math.round(progress)}%` : ''
}

function formatError(error: unknown, fallback: string) {
  if (typeof error === 'string') return error
  if (error && typeof error === 'object') {
    const data = error as { message?: string; response?: { data?: { detail?: string | { message?: string } } } }
    const detail = data.response?.data?.detail
    if (typeof detail === 'string') return detail
    if (detail?.message) return detail.message
    if (data.message) return data.message
  }
  return fallback
}

// 用途：集中放置页面数据加载函数。

async function loadSelectedBuild(buildId: string, options: { silent?: boolean } = {}): Promise<boolean> {
  if (!buildId) {
    selectedBuild.value = null
    return true
  }
  const queryOptions = { signal: requestController.signal, silent: options.silent }
  const [detailResult, statusResult] = await Promise.all([
    fetchComponentBuild(buildId, queryOptions),
    fetchComponentBuildStatus(buildId, queryOptions)
  ])
  if (requestController.signal.aborted) return false
  if (!detailResult.error && detailResult.data) selectedBuild.value = detailResult.data
  if (!statusResult.error && statusResult.data) {
    buildStatuses.value = { ...buildStatuses.value, [buildId]: statusResult.data }
  }
  return !detailResult.error && Boolean(detailResult.data) && !statusResult.error && Boolean(statusResult.data)
}

async function ensureBuildDetailLoaded(buildId: string) {
  if (buildDetailMap.value[buildId]?.component_id) return
  const queryOptions = { signal: requestController.signal, silent: true }
  const result = await fetchComponentBuild(buildId, queryOptions)
  if (!result.error && result.data) {
    // 用途：保存后由 buildDetailMap 在下一次渲染中读取。
  }
}

function componentSpecStorageKey(buildId: string) {
  return `component-spec-v1.2:${buildId}`
}

function localComponentSpec(buildId: string): Api.ComponentBuild.ComponentSpecDocument {
  const document: Api.ComponentBuild.ComponentSpecDocument = {
    build_id: buildId,
    schema: { schema_version: 'dynamic', sections: [] },
    data: {},
    yaml: null,
    source_filename: null,
    saved: false,
    updated_at: null
  }
  const saved = localStorage.getItem(componentSpecStorageKey(buildId))
  if (!saved) return document
  try {
    const cached = JSON.parse(saved) as {
      data: Record<string, any>
      yaml?: string | null
      source_filename?: string | null
      updated_at: string
    }
    document.data = cached.data
    document.yaml = cached.yaml || null
    document.source_filename = cached.source_filename || null
    document.saved = true
    document.updated_at = cached.updated_at
  } catch {
    localStorage.removeItem(componentSpecStorageKey(buildId))
  }
  return document
}

async function loadComponentSpecForDialog(buildId: string) {
  const sequence = ++componentSpecRequestSequence
  componentSpecLoading.value = true
  libraryDialogRef.value?.setSpecLoading(buildId, true)
  try {
    const result = await loadComponentSpecWithFallback(
      buildId,
      id => fetchComponentSpec(id),
      localComponentSpec
    )
    if (sequence !== componentSpecRequestSequence) return
    componentSpec.value = result.document
    libraryDialogRef.value?.setComponentSpec(buildId, result.document, result.offline)
  } finally {
    if (sequence === componentSpecRequestSequence) {
      componentSpecLoading.value = false
      libraryDialogRef.value?.setSpecLoading(buildId, false)
    }
  }
}

async function loadAllBuildDetails(silent = true) {
  const ids = allBuildNodes(treeData.value).map(b => b.id)
  if (!ids.length) return
  const results = await Promise.all(
    ids.map(id => fetchComponentBuild(id, { signal: requestController.signal, silent }))
  )
  const updates: Record<string, Api.ComponentBuild.BuildDetail> = {}
  for (let i = 0; i < results.length; i++) {
    const result = results[i]
    if (!result.error && result.data) {
      updates[ids[i]] = result.data
    }
  }
  cachedBuildDetails.value = { ...cachedBuildDetails.value, ...updates }
}

async function loadTree(options: { preserveSelection?: boolean; silent?: boolean } = {}): Promise<boolean> {
  const showLoading = !options.silent
  if (showLoading) treeLoading.value = true
  try {
    const result = await fetchComponentBuildTree({
      signal: requestController.signal,
      silent: options.silent
    })
    if (requestController.signal.aborted) return false
    if (result.error || !result.data) {
      if (!options.silent) window.$message?.error('图元建库树暂时不可用')
      return false
    }
    treeData.value = normalizeTree(result.data)
    // 用途：首次进入页面时选中第一个系统库，避免把两个库的数据混成一个无归属列表。
    if (!options.preserveSelection && selectedCatalogId.value === '__root__' && treeData.value.length) {
      selectedCatalogId.value = treeData.value[0].id
    }
    // 用途：读取全部构建详情，填充标准号等真实表格字段。
    void loadAllBuildDetails(options.silent)
    return true
  } finally {
    if (showLoading) treeLoading.value = false
  }
}

async function loadCatalog(options: { silent?: boolean } = {}): Promise<boolean> {
  catalogLoading.value = true
  try {
    const result = await fetchComponentBuildCatalog({
      signal: requestController.signal,
      silent: options.silent
    })
    if (requestController.signal.aborted) return false
    if (result.error || !result.data) {
      if (!options.silent) window.$message?.error('图元分类目录暂时不可用')
      return false
    }
    catalog.value = result.data.categories
    return true
  } finally {
    catalogLoading.value = false
  }
}

async function refresh() {
  refreshing.value = true
  try {
    const [treeOk, catalogOk] = await Promise.all([
      loadTree({ preserveSelection: true }),
      loadCatalog()
    ])
    const buildOk = selectedBuildId.value ? await loadSelectedBuild(selectedBuildId.value) : true
    statusUnavailable.value = !(treeOk && catalogOk && buildOk)
  } finally {
    refreshing.value = false
  }
}

async function pollBuilds() {
  if (polling.value) return
  const pending = allBuildNodes(treeData.value).filter(node => node.status === 'uploading' || node.status === 'parsing_sources')
  if (!pending.length) return
  polling.value = true
  try {
    const results = await Promise.all(
      pending.map(node =>
        fetchComponentBuildStatus(node.id, {
          signal: requestController.signal,
          silent: true
        })
      )
    )
    if (requestController.signal.aborted) return
    const next = { ...buildStatuses.value }
    results.forEach((result, index) => {
      if (!result.error && result.data) next[pending[index].id] = result.data
    })
    buildStatuses.value = next
    const treeOk = await loadTree({ preserveSelection: true, silent: true })
    const buildOk = selectedBuildId.value ? await loadSelectedBuild(selectedBuildId.value, { silent: true }) : true
    statusUnavailable.value = !(treeOk && buildOk)
  } finally {
    polling.value = false
  }
}

function syncPolling() {
  if (hasPendingBuilds() && !pollTimer.value) {
    pollTimer.value = window.setInterval(() => {
      void pollBuilds()
    }, 2000)
  }
  if (!hasPendingBuilds() && pollTimer.value) {
    window.clearInterval(pollTimer.value)
    pollTimer.value = null
  }
}

// 用途：集中放置页面事件处理函数。

function handleCatalogSelect(catalogId: string) {
  selectedCatalogId.value = catalogId
}

function handleRowClick(buildId: string) {
  selectedBuildId.value = buildId
  void loadSelectedBuild(buildId)
  openDialogForBuild(buildId)
}

// 用途：根据真实源格式进入对应工作台，防止 STEP 被错误送入 CATPart 专属 Feature Center。
function openModelViewer(buildId: string, revisionId: string) {
  const row = displayedRows.value.find(item => item.id === buildId);
  router.push(modelViewerLocation(buildId, revisionId, row?.sourceFormat));
}

async function openDialogForBuild(buildId: string) {
  const build = selectedBuild.value?.id === buildId ? selectedBuild.value : await fetchComponentBuild(buildId, { silent: true }).then(r => r.data || null)
  if (!build) return
  const opened = await libraryDialogRef.value?.open(build, buildStatuses.value, null)
  if (!opened) return
  componentSpec.value = null
  await loadComponentSpecForDialog(buildId)
}

function handleDialogSubmit(payload: {
  form: Omit<Api.ComponentBuild.CreatePayload, 'source_file' | 'step_file' | 'drawing_file'>
  editingBuild: Api.ComponentBuild.BuildDetail | null
  sourceFile: File | null
  drawingFile: File | null
}) {
  submitting.value = true
  const doSubmit = async () => {
    const data = {
      ...payload.form,
      ...(payload.sourceFile ? { source_file: payload.sourceFile } : {}),
      ...(payload.drawingFile ? { drawing_file: payload.drawingFile } : {})
    }
    const result = payload.editingBuild
      ? await updateComponentBuild({ ...data, build_id: payload.editingBuild.id })
      : await createComponentBuild(data)
    if (result.error || !result.data) throw result.error
    libraryDialogRef.value?.close()
    void router.replace({ path: '/component-build', query: { build_id: result.data.id } })
    // 用途：用保存后的构建数据更新缓存，让表格立即显示新值。
    cachedBuildDetails.value = { ...cachedBuildDetails.value, [result.data.id]: result.data }
    void loadTree()
    selectedBuild.value = result.data
    selectedBuildId.value = result.data.id
    window.$message?.success(payload.editingBuild ? '图元修改已保存' : '图元已创建')
  }
  doSubmit().catch(error => {
    window.$message?.error(formatError(error, payload.editingBuild ? '图元修改保存失败' : '图元创建失败'))
  }).finally(() => {
    submitting.value = false
  })
}

function handleDeleteBuild(buildId: string) {
  ElMessageBox.confirm(
    '此操作将永久删除该图元及其关联的所有数据（STEP模型、图纸解析、ComponentSpec等），不可恢复。',
    '确认删除图元',
    {
      confirmButtonText: '确认删除',
      cancelButtonText: '取消',
      type: 'warning',
      confirmButtonClass: 'el-button--danger'
    }
  ).then(async () => {
    try {
      const result = await deleteComponentBuild(buildId)
      if (result.error) throw result.error
      // 用途：清除已删除构建的详情缓存。
      const next = { ...cachedBuildDetails.value }
      delete next[buildId]
      cachedBuildDetails.value = next
      // 用途：从当前目录树移除已删除节点。
      treeData.value = treeData.value.filter(node => node.id !== buildId).map(node => ({
        ...node,
        children: removeNodeFromChildren(node.children, buildId)
      }))
      if (selectedBuildId.value === buildId) {
        selectedBuild.value = null
        selectedBuildId.value = ''
      }
      window.$message?.success('图元已删除')
      // 用途：删除后从服务端重新加载权威目录数据。
      void refresh()
    } catch (error) {
      window.$message?.error(formatError(error, '删除图元失败'))
    }
  }).catch(() => { /* user cancelled */ })
}

function removeNodeFromChildren(children: ComponentTreeNode[], targetId: string): ComponentTreeNode[] {
  return children.filter(child => child.id !== targetId).map(child => ({
    ...child,
    children: removeNodeFromChildren(child.children, targetId)
  }))
}

function handleStartParsing(buildId: string, role: Api.ComponentBuild.RetryRole) {
  const run = async () => {
    parsingRole.value = role
    try {
      const result = await retryComponentBuild(buildId, role)
      if (result.error) throw result.error
      await refresh()
      window.$message?.success(role === 'reference_step' ? 'STEP 已进入解析队列' : '二维图纸已进入解析队列')
    } catch (error) {
      window.$message?.error(formatError(error, role === 'reference_step' ? 'STEP 解析启动失败' : '二维图纸解析启动失败'))
    } finally {
      parsingRole.value = null
      libraryDialogRef.value?.setSourceParsing(false)
    }
  }
  run()
}

function handleFusion(
  buildId: string,
  workingPayload: Api.ComponentBuild.ComponentSpecSavePayload | null = null
) {
  const run = async () => {
    fusionLoading.value = true
    try {
      if (workingPayload) {
        componentSpecSaving.value = true
        libraryDialogRef.value?.setSpecSaving(buildId, true)
        const saved = await saveComponentSpec(buildId, workingPayload)
        if (saved.error || !saved.data) {
          throw saved.error || new Error('ComponentSpec save returned no document')
        }
        localStorage.removeItem(componentSpecStorageKey(buildId))
        componentSpec.value = saved.data
        libraryDialogRef.value?.setComponentSpec(buildId, saved.data, false)
      }
      const result = await fuseComponentBuild(buildId, false)
      if (result.error || !result.data) throw result.error
      fusionReport.value = result.data
      await loadComponentSpecForDialog(buildId)
      window.$message?.success('数据融合完成')
    } catch (error) {
      window.$message?.error(formatError(error, '数据融合失败'))
    } finally {
      componentSpecSaving.value = false
      libraryDialogRef.value?.setSpecSaving(buildId, false)
      fusionLoading.value = false
    }
  }
  run()
}

function handleSaveSpec(
  buildId: string,
  payload: Api.ComponentBuild.ComponentSpecSavePayload
) {
  const run = async () => {
    componentSpecSaving.value = true
    libraryDialogRef.value?.setSpecSaving(buildId, true)
    const baseSpec = componentSpec.value?.build_id === buildId
      ? componentSpec.value
      : localComponentSpec(buildId)
    try {
      const result = await saveComponentSpec(buildId, payload)
      if (result.error || !result.data) {
        if (!isOfflineRequestError(result.error)) {
          throw result.error || new Error('ComponentSpec save returned no document')
        }
        const updatedAt = new Date().toISOString()
        localStorage.setItem(
          componentSpecStorageKey(buildId),
          JSON.stringify({ ...payload, updated_at: updatedAt })
        )
        const localSaved = {
          ...baseSpec,
          data: payload.data,
          yaml: payload.yaml,
          source_filename: payload.source_filename,
          saved: true,
          updated_at: updatedAt
        }
        componentSpec.value = localSaved
        libraryDialogRef.value?.setComponentSpec(buildId, localSaved, true)
        window.$message?.warning('后端模板接口尚未启用，草稿已暂存在当前浏览器')
      } else {
        localStorage.removeItem(componentSpecStorageKey(buildId))
        componentSpec.value = result.data
        libraryDialogRef.value?.setComponentSpec(buildId, result.data, false)
        window.$message?.success('ComponentSpec 草稿已保存')
      }
    } catch (error) {
      window.$message?.error(formatError(error, 'ComponentSpec 保存失败'))
    } finally {
      componentSpecSaving.value = false
      libraryDialogRef.value?.setSpecSaving(buildId, false)
    }
  }
  run()
}

function handleOpenCreateDialog() {
  libraryDialogRef.value?.open(null, buildStatuses.value, null)
}

// 用途：监听目录和路由变化。
watch(treeData, syncPolling, { deep: true })
watch(buildStatuses, statuses => {
  libraryDialogRef.value?.updateBuildStatuses(statuses)
}, { deep: true })

// 用途：执行页面初始化。
onMounted(async () => {
  const [treeOk, catalogOk] = await Promise.all([loadTree(), loadCatalog()])
  statusUnavailable.value = !(treeOk && catalogOk)
  syncPolling()
})

onBeforeUnmount(() => {
  if (pollTimer.value) window.clearInterval(pollTimer.value)
  requestController.abort()
})
</script>

<template>
  <div class="component-library-page">
    <!-- Header -->
    <header class="library-header">
      <div class="header-title-group">
        <h3 class="header-title">图元库存储管理</h3>
        <span class="header-subtitle">目录层级建设 · 元数据管理 · 支持增删改查</span>
      </div>
      <div class="header-actions">
        <ElButton
          ref="addBtnRef"
          type="primary"
          @click="handleOpenCreateDialog"
        >
          <template #icon>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <line x1="12" y1="5" x2="12" y2="19" />
              <line x1="5" y1="12" x2="19" y2="12" />
            </svg>
          </template>
          + 新增零件
        </ElButton>
        <ElTooltip content="刷新目录和列表" placement="bottom">
          <ElButton :loading="refreshing" circle @click="refresh">
            <template #icon>
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <polyline points="1 4 1 10 7 10" />
                <path d="M3.51 15a9 9 0 102.13-9.36L1 10" />
              </svg>
            </template>
          </ElButton>
        </ElTooltip>
      </div>
    </header>

    <!-- Main layout: catalog left + table right -->
    <main class="library-main">
      <aside class="library-catalog">
        <ComponentLibraryCatalog
          :catalog-items="catalogItems"
          :selected-catalog-id="selectedCatalogId"
          :loading="treeLoading"
          @select="handleCatalogSelect"
        />
      </aside>

      <section class="library-content">
        <ComponentLibraryTable
          :rows="displayedRows"
          :total-count="displayedRows.length"
          :loading="treeLoading"
          :keyword="searchKeyword"
          :selected-build-id="selectedBuildId"
          @update:keyword="searchKeyword = $event"
          @edit="openDialogForBuild"
          @delete-build="handleDeleteBuild"
          @view-cad-model="openModelViewer"
          @view-drawing="(bid, taskId) => router.push({ path: '/cad-spec', query: { revision_id: '', task_id: taskId, build_id: bid } })"
          @start-step-parsing="(bid) => handleStartParsing(bid, 'reference_step')"
          @start-drawing-parsing="(bid) => handleStartParsing(bid, 'drawing')"
          @view-component-spec="(bid) => openDialogForBuild(bid)"
          @view-yaml="(bid) => openDialogForBuild(bid)"
          @run-fusion="handleFusion"
          @row-click="handleRowClick"
        />

        <!-- Status alert -->
        <ElAlert
          v-if="statusUnavailable"
          class="status-alert"
          title="解析状态暂时不可用，页面保留上次成功结果并继续重试。"
          type="warning"
          :closable="false"
          show-icon
        />
      </section>
    </main>

    <!-- Dialog -->
    <ComponentLibraryDialog
      ref="libraryDialogRef"
      :catalog="catalog"
      :catalog-loading="catalogLoading"
      :submitting="submitting"
      @submit="handleDialogSubmit"
      @refresh="refresh"
      @fusion="handleFusion"
      @save-spec="handleSaveSpec"
      @start-parsing="handleStartParsing"
    />
  </div>
</template>

<script lang="ts">
export default { name: 'ComponentBuild' }
</script>

<style scoped>
.component-library-page {
  display: flex;
  flex-direction: column;
  min-height: 100%;
  padding: 20px 24px;
  background: #f5f7fb;
  gap: 16px;
}

/* 页面标题栏样式 */
.library-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.header-title-group {
  display: flex;
  align-items: baseline;
  gap: 10px;
}

.header-title {
  margin: 0;
  font-size: 18px;
  font-weight: 600;
  color: #1a2332;
  line-height: 1.3;
}

.header-subtitle {
  font-size: 12px;
  color: #8e99aa;
}

.header-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

/* 页面主布局样式 */
.library-main {
  display: grid;
  grid-template-columns: 320px minmax(0, 1fr);
  gap: 16px;
  min-height: 0;
  flex: 1;
}

.library-catalog {
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.library-content {
  display: flex;
  min-height: 0;
  flex-direction: column;
}

.status-alert {
  margin-top: 12px;
}

/* 响应式布局样式 */
@media (max-width: 860px) {
  .component-library-page {
    padding: 12px;
  }

  .library-main {
    grid-template-columns: 1fr;
  }

  .library-catalog {
    max-height: 240px;
  }

  .header-title-group {
    flex-direction: column;
    gap: 2px;
  }

  .header-subtitle {
    font-size: 11px;
  }
}
</style>
