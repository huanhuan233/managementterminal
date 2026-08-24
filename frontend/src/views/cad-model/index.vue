<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import type { UploadRequestOptions } from 'element-plus';
import {
  fetchCadEdgeTopology,
  fetchCadEntities,
  fetchCadEntity,
  fetchCadFaceTopology,
  fetchCadFeatures,
  fetchCadMeasurements,
  fetchCadMeshes,
  fetchCadModels,
  fetchCadRevisionStatus,
  fetchCadStructureTree,
  recomputeCadMeasurements,
  uploadCadModel
} from '@/service/api';
import CadViewer from './modules/CadViewer.vue';

type GeometryTab = 'face' | 'edge' | 'vertex' | 'measurement' | 'feature';
type LeftNavigationTab = 'bom' | 'feature' | 'geometry';

const EMPTY_TEXT = '—';
const route = useRoute();
const router = useRouter();

const requestedRevisionId = computed(() => String(route.query.revision_id || ''));
const buildId = computed(() => String(route.query.build_id || ''));

const loadingModels = ref(false);
const loadingTree = ref(false);
const loadingMeshes = ref(false);
const loadingEntities = ref(false);
const loadingMeasurements = ref(false);
const loadingFeatures = ref(false);
const recomputingMeasurements = ref(false);
const uploading = ref(false);

const models = ref<Api.Cad.ModelSummary[]>([]);
const selectedModelId = ref('');
const selectedRevisionId = ref('');
const status = ref<Api.Cad.ParseStatus | null>(null);
const treeData = ref<Api.Cad.TreeNode[]>([]);
const viewerMeshes = ref<Api.Cad.Mesh[]>([]);
const selectedNode = ref<Api.Cad.TreeNode | null>(null);
const selectedEntity = ref<Api.Cad.Entity | null>(null);
const selectedMeasurement = ref<Api.Cad.Measurement | null>(null);
const selectedFeature = ref<Api.Cad.FeatureCandidate | null>(null);
const selectedFaceId = ref('');
const selectedSolidId = ref('');
const solidFaceIds = ref<string[]>([]);
const measurementHighlightFaceIds = ref<string[]>([]);
const selectedPatternEvidence = ref<Api.Cad.PatternEvidence | null>(null);
const faceTopology = ref<Api.Cad.FaceTopology | null>(null);
const edgeTopology = ref<Api.Cad.EdgeTopology | null>(null);
const pollTimer = ref<number | null>(null);
const entityCache = ref(new Map<string, Api.Cad.Entity>());
const geometryListRef = ref<HTMLElement | null>(null);

const activeGeometryTab = ref<GeometryTab>('face');
const geometryKeyword = ref('');
const geometryTypeFilter = ref('');
const geometryPage = ref(1);
const geometryPageSize = ref(20);
const geometryRows = ref<Api.Cad.Entity[]>([]);
const geometryTotal = ref(0);
const measurementRows = ref<Api.Cad.Measurement[]>([]);
const measurementTotal = ref(0);
const featureRows = ref<Api.Cad.FeatureCandidate[]>([]);
const featureTotal = ref(0);

const isLeftCollapsed = ref(false);
const isRightCollapsed = ref(false);
const activeLeftNavigation = ref<LeftNavigationTab>('bom');

const selectedModel = computed(() => models.value.find(item => item.id === selectedModelId.value) ?? null);
const isProcessing = computed(() => status.value?.status === 'queued' || status.value?.status === 'processing');
const progress = computed(() => status.value?.progress ?? selectedModel.value?.progress ?? 0);
const statusText = computed(() => {
  if (status.value?.status === 'completed' || selectedModel.value?.status === 'completed') return '解析完成';
  if (status.value?.status === 'failed') return '解析失败';
  if (status.value?.status === 'processing') return '解析中';
  if (status.value?.status === 'queued') return '排队中';
  return '未选择';
});

const faceCount = computed(() => selectedModel.value?.face_count ?? 0);
const edgeCount = computed(() => selectedModel.value?.edge_count ?? 0);
const vertexCount = computed(() => selectedModel.value?.vertex_count ?? 0);

const selectedGeometryTitle = computed(() => {
  if (activeGeometryTab.value === 'face') return '面 Face';
  if (activeGeometryTab.value === 'edge') return '边 Edge';
  if (activeGeometryTab.value === 'measurement') return '尺寸';
  if (activeGeometryTab.value === 'feature') return '特征';
  return '顶点 Vertex';
});

const activeListTotal = computed(() => {
  if (activeGeometryTab.value === 'measurement') return measurementTotal.value;
  if (activeGeometryTab.value === 'feature') return featureTotal.value;
  return geometryTotal.value;
});

const geometryTypeOptions = computed(() => {
  const values = new Set<string>();
  geometryRows.value.forEach(item => {
    if (item.geometry_type) values.add(item.geometry_type);
  });
  return Array.from(values).sort();
});

// 用途：从折叠栏或页签进入对应导航区，并同步几何列表的数据类型。
function openLeftNavigation(tab: LeftNavigationTab) {
  activeLeftNavigation.value = tab;
  if (tab === 'feature') activeGeometryTab.value = 'feature';
  if (tab === 'geometry' && activeGeometryTab.value === 'feature') activeGeometryTab.value = 'face';
  isLeftCollapsed.value = false;
}

function stopPolling() {
  if (pollTimer.value) {
    window.clearInterval(pollTimer.value);
    pollTimer.value = null;
  }
}

function cacheEntities(items: Api.Cad.Entity[]) {
  const next = new Map(entityCache.value);
  items.forEach(item => next.set(item.id, item));
  entityCache.value = next;
}

function findRequestedModel(items: Api.Cad.ModelSummary[]) {
  if (!requestedRevisionId.value) return null;
  return items.find(item => item.current_revision_id === requestedRevisionId.value) ?? null;
}

async function loadModels() {
  loadingModels.value = true;
  try {
    const result = await fetchCadModels({ page: 1, page_size: 50, has_build: true });
    if (result.error || !result.data) return;
    models.value = result.data.items;
    if (!selectedModelId.value && result.data.items.length > 0) {
      await selectModel(findRequestedModel(result.data.items) ?? result.data.items[0]);
    }
  } finally {
    loadingModels.value = false;
  }
}

async function loadStatus(revisionId = selectedRevisionId.value) {
  if (!revisionId) return;
  const result = await fetchCadRevisionStatus(revisionId);
  if (result.error || !result.data) return;
  status.value = result.data;
  if (result.data.status === 'completed') {
    stopPolling();
    await Promise.all([loadStructureTree(revisionId), loadViewerMeshes(revisionId), loadGeometryObjects()]);
    await loadModels();
  }
  if (result.data.status === 'failed') stopPolling();
}

async function loadStructureTree(revisionId = selectedRevisionId.value) {
  if (!revisionId) return;
  loadingTree.value = true;
  try {
    const result = await fetchCadStructureTree(revisionId);
    if (result.error || !result.data) return;
    treeData.value = result.data;
  } finally {
    loadingTree.value = false;
  }
}

async function loadViewerMeshes(revisionId = selectedRevisionId.value) {
  if (!revisionId) return;
  loadingMeshes.value = true;
  try {
    const result = await fetchCadMeshes(revisionId, { page: 1, page_size: 5000 });
    if (result.error || !result.data) return;
    viewerMeshes.value = result.data.items;
  } finally {
    loadingMeshes.value = false;
  }
}

async function loadGeometryObjects() {
  if (!selectedRevisionId.value) return;
  if (activeGeometryTab.value === 'measurement') {
    await loadMeasurements();
    return;
  }
  if (activeGeometryTab.value === 'feature') {
    await loadFeatures();
    return;
  }
  loadingEntities.value = true;
  try {
    const result = await fetchCadEntities(selectedRevisionId.value, {
      parent_entity_id: activeGeometryTab.value === 'face' ? selectedSolidId.value || undefined : undefined,
      entity_type: activeGeometryTab.value,
      geometry_type: geometryTypeFilter.value || undefined,
      keyword: geometryKeyword.value || undefined,
      page: geometryPage.value,
      page_size: geometryPageSize.value
    });
    if (result.error || !result.data) return;
    geometryRows.value = result.data.items;
    geometryTotal.value = result.data.total;
    cacheEntities(result.data.items);
    await nextTick();
    scrollSelectedIntoView();
  } finally {
    loadingEntities.value = false;
  }
}

async function loadMeasurements() {
  if (!selectedRevisionId.value) return;
  loadingMeasurements.value = true;
  try {
    const result = await fetchCadMeasurements(selectedRevisionId.value, {
      measurement_type: geometryKeyword.value || undefined,
      confidence_min: geometryTypeFilter.value ? Number(geometryTypeFilter.value) : undefined,
      page: geometryPage.value,
      page_size: geometryPageSize.value
    });
    if (result.error || !result.data) return;
    measurementRows.value = result.data.items;
    measurementTotal.value = result.data.total;
  } finally {
    loadingMeasurements.value = false;
  }
}

async function loadFeatures() {
  if (!selectedRevisionId.value) return;
  loadingFeatures.value = true;
  try {
    const result = await fetchCadFeatures(selectedRevisionId.value, {
      feature_type: geometryKeyword.value || undefined,
      confidence_min: geometryTypeFilter.value ? Number(geometryTypeFilter.value) : undefined,
      page: geometryPage.value,
      page_size: geometryPageSize.value
    });
    if (result.error || !result.data) return;
    featureRows.value = result.data.items;
    featureTotal.value = result.data.total;
  } finally {
    loadingFeatures.value = false;
  }
}

function startPolling() {
  stopPolling();
  pollTimer.value = window.setInterval(() => {
    runAsync(loadStatus());
  }, 2000);
}

function resetSelection() {
  selectedNode.value = null;
  selectedEntity.value = null;
  selectedMeasurement.value = null;
  selectedFeature.value = null;
  selectedFaceId.value = '';
  selectedSolidId.value = '';
  solidFaceIds.value = [];
  measurementHighlightFaceIds.value = [];
  selectedPatternEvidence.value = null;
  faceTopology.value = null;
  edgeTopology.value = null;
}

async function selectModel(model: Api.Cad.ModelSummary) {
  stopPolling();
  selectedModelId.value = model.id;
  selectedRevisionId.value = model.current_revision_id ?? '';
  status.value = null;
  treeData.value = [];
  viewerMeshes.value = [];
  geometryRows.value = [];
  geometryTotal.value = 0;
  measurementRows.value = [];
  measurementTotal.value = 0;
  featureRows.value = [];
  featureTotal.value = 0;
  geometryPage.value = 1;
  geometryKeyword.value = '';
  geometryTypeFilter.value = '';
  entityCache.value = new Map();
  resetSelection();
  if (!selectedRevisionId.value) return;
  await loadStatus(selectedRevisionId.value);
  const currentStatus = status.value as Api.Cad.ParseStatus | null;
  if (currentStatus?.status === 'completed') {
    await Promise.all([
      loadStructureTree(selectedRevisionId.value),
      loadViewerMeshes(selectedRevisionId.value),
      loadGeometryObjects()
    ]);
  } else if (isProcessing.value) {
    startPolling();
  }
}

async function handleUpload(options: UploadRequestOptions) {
  const file = options.file;
  uploading.value = true;
  try {
    const result = await uploadCadModel(file, file.name.replace(/\.(step|stp)$/i, ''));
    if (result.error || !result.data) throw new Error('upload failed');
    selectedModelId.value = result.data.model_id;
    selectedRevisionId.value = result.data.revision_id;
    status.value = {
      status: result.data.status,
      progress: 0,
      status_message: 'queued',
      error_code: null,
      error_message: null
    };
    treeData.value = [];
    viewerMeshes.value = [];
    geometryRows.value = [];
    geometryTotal.value = 0;
    measurementRows.value = [];
    measurementTotal.value = 0;
    featureRows.value = [];
    featureTotal.value = 0;
    resetSelection();
    await loadModels();
    startPolling();
    options.onSuccess(result.data);
  } catch (error) {
    options.onError(error as Parameters<typeof options.onError>[0]);
  } finally {
    uploading.value = false;
  }
}

function beforeUpload(file: File) {
  const ok = /\.(step|stp)$/i.test(file.name);
  if (!ok) window.$message?.error('只支持 STEP/STP 文件');
  return ok;
}

async function handleTreeClick(node: Api.Cad.TreeNode) {
  selectedNode.value = node;
  selectedEntity.value = null;
  selectedMeasurement.value = null;
  selectedFeature.value = null;
  selectedFaceId.value = '';
  measurementHighlightFaceIds.value = [];
  selectedPatternEvidence.value = null;
  faceTopology.value = null;
  edgeTopology.value = null;
  solidFaceIds.value = [];

  if (node.entity_type === 'solid' && selectedRevisionId.value) {
    selectedSolidId.value = node.id;
    activeGeometryTab.value = 'face';
    geometryKeyword.value = '';
    geometryTypeFilter.value = '';
    geometryPage.value = 1;
    const result = await fetchCadMeshes(selectedRevisionId.value, {
      parent_entity_id: node.id,
      page: 1,
      page_size: 5000
    });
    if (!result.error && result.data) {
      solidFaceIds.value = result.data.items.map(item => item.entity_id);
    }
    await loadGeometryObjects();
  }
}

async function selectEntity(entity: Api.Cad.Entity) {
  selectedEntity.value = entity;
  selectedMeasurement.value = null;
  selectedFeature.value = null;
  selectedNode.value = null;
  faceTopology.value = null;
  edgeTopology.value = null;
  solidFaceIds.value = [];
  measurementHighlightFaceIds.value = [];
  selectedPatternEvidence.value = null;
  selectedFaceId.value = entity.entity_type === 'face' ? entity.id : '';
  cacheEntities([entity]);

  if (!selectedRevisionId.value) return;
  if (entity.entity_type === 'face') {
    const result = await fetchCadFaceTopology(selectedRevisionId.value, entity.id);
    if (!result.error && result.data) {
      faceTopology.value = result.data;
      cacheEntities([...(result.data.edges ?? []), ...(result.data.adjacent_faces ?? [])]);
    }
  } else if (entity.entity_type === 'edge') {
    const result = await fetchCadEdgeTopology(selectedRevisionId.value, entity.id);
    if (!result.error && result.data) {
      edgeTopology.value = result.data;
      cacheEntities([...(result.data.vertices ?? []), ...(result.data.faces ?? [])]);
    }
  }
  await nextTick();
  scrollSelectedIntoView();
}

async function selectMeasurement(measurement: Api.Cad.Measurement) {
  selectedMeasurement.value = measurement;
  selectedFeature.value = null;
  selectedEntity.value = null;
  selectedNode.value = null;
  selectedFaceId.value = '';
  selectedPatternEvidence.value = null;
  measurementHighlightFaceIds.value = await resolveSourceFaceIds(measurement.source_entity_ids);
}

async function selectFeature(feature: Api.Cad.FeatureCandidate) {
  selectedFeature.value = feature;
  selectedMeasurement.value = null;
  selectedEntity.value = null;
  selectedNode.value = null;
  selectedFaceId.value = '';
  measurementHighlightFaceIds.value = await resolveSourceFaceIds(feature.source_entity_ids);
  selectedPatternEvidence.value = feature.feature_type === 'circular_pattern' ? patternEvidence(feature) : null;
}

async function handleViewerFaceClick(entityId: string) {
  let entity = entityCache.value.get(entityId);
  if (!entity && selectedRevisionId.value) {
    const result = await fetchCadEntity(selectedRevisionId.value, entityId);
    if (!result.error && result.data) entity = result.data;
  }
  if (!entity) return;

  activeGeometryTab.value = 'face';
  if (!geometryRows.value.some(item => item.id === entityId)) {
    geometryKeyword.value = entity.source_ref ?? '';
    geometryPage.value = 1;
    await loadGeometryObjects();
  }
  await selectEntity(entity);
}

async function resolveSourceFaceIds(sourceEntityIds: string[]) {
  const faceIds = new Set<string>();
  const entities = await Promise.all(
    sourceEntityIds.map(async sourceId => {
      const cached = entityCache.value.get(sourceId);
      if (cached || !selectedRevisionId.value) return cached ?? null;
      const result = await fetchCadEntity(selectedRevisionId.value, sourceId);
      if (!result.error && result.data) {
        cacheEntities([result.data]);
        return result.data;
      }
      return null;
    })
  );
  entities
    .filter((entity): entity is Api.Cad.Entity => Boolean(entity))
    .forEach(entity => {
      if (entity.entity_type === 'face') {
        faceIds.add(entity.id);
      }
    });
  const edgeEntities = entities.filter(
    (entity): entity is Api.Cad.Entity => entity !== null && entity.entity_type === 'edge'
  );
  const revisionId = selectedRevisionId.value;
  if (revisionId) {
    const topologyResults = await Promise.all(edgeEntities.map(entity => fetchCadEdgeTopology(revisionId, entity.id)));
    topologyResults.forEach(result => {
      if (!result.error && result.data?.faces?.length) {
        result.data.faces.forEach(face => faceIds.add(face.id));
        cacheEntities(result.data.faces);
      }
    });
  }
  return Array.from(faceIds);
}

async function recomputeMeasurements() {
  if (!selectedRevisionId.value) return;
  recomputingMeasurements.value = true;
  try {
    const result = await recomputeCadMeasurements(selectedRevisionId.value);
    if (!result.error) await Promise.all([loadMeasurements(), loadFeatures()]);
  } finally {
    recomputingMeasurements.value = false;
  }
}

function goBackToComponentBuild() {
  if (!buildId.value) return;
  router.push({ path: '/component-build', query: { build_id: buildId.value } });
}

function applyGeometrySearch() {
  geometryPage.value = 1;
  runAsync(loadGeometryObjects());
}

function clearGeometryFilter() {
  geometryKeyword.value = '';
  geometryTypeFilter.value = '';
  applyGeometrySearch();
}

function nodeLabel(data: Record<string, unknown>) {
  const node = data as unknown as Api.Cad.TreeNode;
  return node.label || node.source_ref || node.entity_type;
}

function displayName(entity: Api.Cad.Entity | Api.Cad.TreeNode | null) {
  if (!entity) return EMPTY_TEXT;
  if ('name' in entity) return entity.label || entity.name || entity.source_ref || entity.entity_type;
  return entity.label || entity.source_ref || entity.entity_type;
}

function empty(value: unknown) {
  return value === null || value === undefined || value === '' ? EMPTY_TEXT : value;
}

function formatNumber(value: number | null | undefined) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return EMPTY_TEXT;
  return Number(value)
    .toFixed(4)
    .replace(/\.?0+$/, '');
}

function formatValue(value: unknown): string {
  if (value === null || value === undefined || value === '') return EMPTY_TEXT;
  if (typeof value === 'number') return formatNumber(value);
  if (typeof value === 'string') return value;
  if (Array.isArray(value)) return value.map(item => formatValue(item)).join(', ');
  if (typeof value === 'object')
    return Object.entries(value as Record<string, unknown>)
      .map(([key, item]) => `${key} ${formatValue(item)}`)
      .join(' · ');
  return String(value);
}

function normalizedValueText(measurement: Api.Cad.Measurement) {
  return formatValue(measurement.normalized_value);
}

function confidenceText(value: number | null | undefined) {
  if (value === null || value === undefined) return EMPTY_TEXT;
  return `${formatNumber(value * 100)}%`;
}

function patternEvidence(feature: Api.Cad.FeatureCandidate): Api.Cad.PatternEvidence | null {
  const center = feature.parameters.center ?? feature.center;
  const axis = feature.parameters.axis ?? feature.axis;
  const pitch = feature.parameters.pitch_circle_diameter;
  if (!Array.isArray(center) || !Array.isArray(axis) || typeof pitch !== 'number') return null;
  return {
    center: center.map(item => Number(item)),
    axis: axis.map(item => Number(item)),
    pitch_circle_diameter: pitch
  };
}

function formatPoint(value: unknown) {
  if (Array.isArray(value)) return value.map(item => formatNumber(Number(item))).join(', ');
  if (value && typeof value === 'object') {
    const point = value as Record<string, unknown>;
    const values = [point.x, point.y, point.z].filter(item => item !== undefined);
    if (values.length) return values.map(item => formatNumber(Number(item))).join(', ');
  }
  return EMPTY_TEXT;
}

function vertexCoordinate(entity: Api.Cad.Entity) {
  return formatPoint(entity.geometry?.point ?? entity.center);
}

function vertexCoordinateShort(entity: Api.Cad.Entity) {
  const raw = entity.geometry?.point ?? entity.center;
  if (Array.isArray(raw)) {
    return ['X', 'Y', 'Z'].map((axis, index) => `${axis} ${formatNumber(Number(raw[index]))}`).join(' · ');
  }
  if (raw && typeof raw === 'object') {
    const point = raw as Record<string, unknown>;
    return ['x', 'y', 'z'].map(axis => `${axis.toUpperCase()} ${formatNumber(Number(point[axis]))}`).join(' · ');
  }
  return EMPTY_TEXT;
}

function entityTypeLabel(type: string) {
  const labels: Record<string, string> = {
    root: 'Root',
    imported_object: '导入对象',
    solid: 'Solid',
    face: '面',
    edge: '边',
    vertex: '顶点'
  };
  return labels[type] ?? type;
}

function getBoundingBoxValue(entity: Api.Cad.Entity | null, key: 'min' | 'max') {
  const box = entity?.bounding_box as Record<string, unknown> | null;
  return formatPoint(box?.[key]);
}

function geometryParamEntries(entity: Api.Cad.Entity | null): { key: string; value: string }[] {
  if (!entity?.geometry || entity.entity_type === 'vertex') return [];
  return Object.entries(entity.geometry)
    .filter(([, value]) => value !== null && value !== undefined && value !== '')
    .map(([key, value]) => ({ key, value: formatValue(value) }));
}

function compactGeometrySummary(entity: Api.Cad.Entity): string {
  if (entity.entity_type === 'vertex') return vertexCoordinateShort(entity);

  const parts = [entity.geometry_type || EMPTY_TEXT];
  if (entity.entity_type === 'face') parts.push(`A ${formatNumber(entity.area)}`);
  if (entity.entity_type === 'edge') parts.push(`L ${formatNumber(entity.length)}`);

  const radius = entity.geometry?.radius ?? entity.geometry?.major_radius ?? entity.geometry?.minor_radius;
  if (radius !== null && radius !== undefined) parts.push(`R ${formatNumber(Number(radius))}`);

  return parts.filter(item => item && item !== EMPTY_TEXT).join(' · ') || EMPTY_TEXT;
}

function scrollSelectedIntoView() {
  const selectedId = selectedEntity.value?.id;
  if (!selectedId || !geometryListRef.value) return;
  const item = geometryListRef.value.querySelector(`[data-entity-id="${selectedId}"]`);
  item?.scrollIntoView({ block: 'nearest' });
}

function runAsync(task: Promise<unknown>) {
  task.catch(() => undefined);
}

function handleVisibilityChange() {
  if (document.visibilityState === 'visible' && route.name === 'cad-model') {
    runAsync(loadModels());
  }
}

watch([activeGeometryTab, geometryPage, geometryPageSize], () => {
  runAsync(loadGeometryObjects());
});

// 每次路由参数变化时重新加载模型列表
watch(() => route.name, () => {
  if (route.name === 'cad-model') {
    runAsync(loadModels());
  }
});

onMounted(() => {
  runAsync(loadModels());
  // 页面切换回前台时自动刷新
  document.addEventListener('visibilitychange', handleVisibilityChange);
});

onActivated(() => {
  runAsync(loadModels());
});

onBeforeUnmount(() => {
  stopPolling();
  document.removeEventListener('visibilitychange', handleVisibilityChange);
});
</script>

<template>
  <div class="cad-page">
    <div class="cad-toolbar">
      <ElUpload
        :show-file-list="false"
        :http-request="handleUpload"
        :before-upload="beforeUpload"
        :disabled="uploading"
        accept=".step,.stp"
      >
        <ElButton type="primary" :loading="uploading">
          <template #icon>
            <icon-carbon-upload />
          </template>
          上传 STEP
        </ElButton>
      </ElUpload>

      <ElButton :loading="loadingModels" @click="loadModels">
        <template #icon>
          <icon-ic-round-refresh />
        </template>
        刷新
      </ElButton>

      <ElButton v-if="buildId" @click="goBackToComponentBuild">返回图元建库</ElButton>

      <div class="status-area">
        <ElTag :type="statusText === '解析完成' ? 'success' : statusText === '解析失败' ? 'danger' : 'info'">
          {{ statusText }}
        </ElTag>
        <ElProgress v-if="isProcessing" :percentage="progress" :stroke-width="8" class="status-progress" />
        <div v-else-if="selectedModel" class="status-counts">
          <span>Face {{ faceCount }}</span>
          <span>Edge {{ edgeCount }}</span>
          <span>Vertex {{ vertexCount }}</span>
        </div>
      </div>

      <ElButton text @click="isRightCollapsed = !isRightCollapsed">
        {{ isRightCollapsed ? '展开属性' : '折叠属性' }}
      </ElButton>
    </div>

    <div class="cad-shell" :class="{ 'left-collapsed': isLeftCollapsed, 'right-collapsed': isRightCollapsed }">
      <aside v-show="!isLeftCollapsed" class="left-panel">
        <header class="navigation-heading">
          <strong>装配导航</strong>
          <button type="button" title="隐藏装配导航" aria-label="隐藏装配导航" @click="isLeftCollapsed = true">‹</button>
        </header>
        <nav class="navigation-tabs" aria-label="模型导航类型">
          <button type="button" :class="{ active: activeLeftNavigation === 'bom' }" @click="openLeftNavigation('bom')">BOM 树</button>
          <button type="button" :class="{ active: activeLeftNavigation === 'feature' }" @click="openLeftNavigation('feature')">特征</button>
          <button type="button" :class="{ active: activeLeftNavigation === 'geometry' }" @click="openLeftNavigation('geometry')">几何拓扑</button>
        </nav>

        <section v-show="activeLeftNavigation === 'bom'" class="panel-section tree-section">
          <div class="panel-title">装配结构</div>
          <ElSkeleton v-if="loadingTree" :rows="5" animated />
          <ElEmpty v-else-if="!treeData.length" description="解析完成后显示结构" :image-size="42" />
          <ElScrollbar v-else class="tree-scroll">
            <ElTree
              :data="treeData"
              node-key="id"
              default-expand-all
              highlight-current
              :props="{ children: 'children', label: nodeLabel }"
              @node-click="handleTreeClick"
            />
          </ElScrollbar>
        </section>

        <section v-show="activeLeftNavigation !== 'bom'" class="panel-section geometry-section">
          <div class="panel-title geometry-title">
            <span>{{ activeLeftNavigation === 'feature' ? '识别特征' : '几何拓扑' }}</span>
            <span class="geometry-total">{{ activeListTotal }}</span>
          </div>

          <ElTabs v-if="activeLeftNavigation === 'geometry'" v-model="activeGeometryTab" class="compact-tabs">
            <ElTabPane label="面 Face" name="face" />
            <ElTabPane label="边 Edge" name="edge" />
            <ElTabPane label="顶点 Vertex" name="vertex" />
            <ElTabPane label="尺寸" name="measurement" />
          </ElTabs>

          <div class="geometry-tools">
            <ElInput
              v-model="geometryKeyword"
              clearable
              size="small"
              placeholder="source_ref"
              @clear="applyGeometrySearch"
              @keyup.enter="applyGeometrySearch"
            />
            <ElSelect
              v-model="geometryTypeFilter"
              clearable
              filterable
              allow-create
              size="small"
              placeholder="type"
              @change="applyGeometrySearch"
              @clear="applyGeometrySearch"
            >
              <ElOption v-for="item in geometryTypeOptions" :key="item" :label="item" :value="item" />
            </ElSelect>
            <ElButton size="small" @click="applyGeometrySearch">
              <template #icon>
                <icon-ic-round-search />
              </template>
            </ElButton>
            <ElButton size="small" text @click="clearGeometryFilter">清空</ElButton>
          </div>

          <div v-if="activeGeometryTab === 'measurement'" class="measurement-action-row">
            <ElButton size="small" :loading="recomputingMeasurements" @click="recomputeMeasurements">重新计算</ElButton>
          </div>

          <div
            ref="geometryListRef"
            v-loading="loadingEntities || loadingMeasurements || loadingFeatures"
            class="geometry-list"
          >
            <ElEmpty
              v-if="
                activeGeometryTab !== 'measurement' &&
                activeGeometryTab !== 'feature' &&
                !geometryRows.length &&
                !loadingEntities
              "
              :description="`${selectedGeometryTitle} 暂无数据`"
            />
            <template v-if="activeGeometryTab !== 'measurement' && activeGeometryTab !== 'feature'">
              <button
                v-for="entity in geometryRows"
                :key="entity.id"
                class="geometry-item"
                :class="{ selected: selectedEntity?.id === entity.id }"
                :data-entity-id="entity.id"
                type="button"
                @click="selectEntity(entity)"
              >
                <span class="geometry-item-title">{{ displayName(entity) }}</span>
                <span class="geometry-item-summary">{{ compactGeometrySummary(entity) }}</span>
              </button>
            </template>

            <ElEmpty
              v-if="activeGeometryTab === 'measurement' && !measurementRows.length && !loadingMeasurements"
              description="暂无尺寸候选"
              :image-size="42"
            />
            <button
              v-for="measurement in activeGeometryTab === 'measurement' ? measurementRows : []"
              :key="measurement.id"
              class="geometry-item"
              :class="{ selected: selectedMeasurement?.id === measurement.id }"
              type="button"
              @click="selectMeasurement(measurement)"
            >
              <span class="geometry-item-title">{{ measurement.measurement_type }}</span>
              <span class="geometry-item-summary">
                {{ normalizedValueText(measurement) }} {{ measurement.unit ?? '' }} ·
                {{ confidenceText(measurement.confidence) }} · {{ measurement.algorithm_version }}
              </span>
            </button>

            <ElEmpty
              v-if="activeGeometryTab === 'feature' && !featureRows.length && !loadingFeatures"
              description="暂无特征候选"
              :image-size="42"
            />
            <button
              v-for="feature in activeGeometryTab === 'feature' ? featureRows : []"
              :key="feature.id"
              class="geometry-item"
              :class="{ selected: selectedFeature?.id === feature.id }"
              type="button"
              @click="selectFeature(feature)"
            >
              <span class="geometry-item-title">{{ feature.feature_type }}</span>
              <span class="geometry-item-summary">
                {{ confidenceText(feature.confidence) }} · {{ feature.algorithm }} · {{ feature.algorithm_version }}
              </span>
            </button>
          </div>

          <div class="compact-pagination">
            <ElPagination
              v-model:current-page="geometryPage"
              v-model:page-size="geometryPageSize"
              size="small"
              :pager-count="5"
              :total="activeListTotal"
              :page-sizes="[20, 50, 100]"
              layout="prev, pager, next"
            />
          </div>
        </section>
      </aside>

      <aside v-if="isLeftCollapsed" class="collapse-rail left-rail" aria-label="折叠的模型导航">
        <button type="button" class="rail-action active" title="装配结构 / BOM 树" aria-label="装配结构 / BOM 树" @click="openLeftNavigation('bom')">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m12 2 8 4.5v10L12 21l-8-4.5v-10L12 2Z"/><path d="m4.4 6.7 7.6 4.2 7.6-4.2M12 11v10"/></svg>
        </button>
        <button type="button" class="rail-action" title="特征" aria-label="特征" @click="openLeftNavigation('feature')">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m12 3 7 4v9l-7 4-7-4V7l7-4Z"/><path d="m5 7 7 4 7-4M12 11v9"/><circle cx="12" cy="11" r="2.2"/></svg>
        </button>
        <button type="button" class="rail-action" title="几何拓扑" aria-label="几何拓扑" @click="openLeftNavigation('geometry')">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m12 2.8 7.2 4.1v8.3L12 19.3l-7.2-4.1V6.9L12 2.8Z"/><path d="m4.8 6.9 7.2 4.2 7.2-4.2M12 11.1v8.2"/><path d="M8 21h8M12 19.3V21"/></svg>
        </button>
        <button type="button" class="rail-action expand" title="展开装配导航" aria-label="展开装配导航" @click="openLeftNavigation(activeLeftNavigation)">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 5 7 7-7 7"/></svg>
        </button>
      </aside>

      <main v-loading="loadingMeshes" class="viewer-panel">
        <CadViewer
          :meshes="viewerMeshes"
          :selected-face-id="selectedFaceId"
          :highlight-face-ids="[...solidFaceIds, ...measurementHighlightFaceIds]"
          :pattern-evidence="selectedPatternEvidence"
          @face-click="handleViewerFaceClick"
        />
      </main>

      <aside v-show="!isRightCollapsed" class="right-panel">
        <div class="panel-title">属性</div>

        <template v-if="selectedEntity">
          <section class="property-group">
            <div class="property-title">基本信息</div>
            <dl class="property-grid">
              <dt>名称</dt>
              <dd>{{ displayName(selectedEntity) }}</dd>
              <dt>UUID</dt>
              <dd>{{ selectedEntity.id }}</dd>
              <dt>source_ref</dt>
              <dd>{{ empty(selectedEntity.source_ref) }}</dd>
              <dt>geometry_type</dt>
              <dd>{{ empty(selectedEntity.geometry_type) }}</dd>
            </dl>
          </section>

          <section class="property-group">
            <div class="property-title">几何属性</div>
            <dl class="property-grid">
              <dt v-if="selectedEntity.entity_type === 'face'">area</dt>
              <dd v-if="selectedEntity.entity_type === 'face'">{{ formatNumber(selectedEntity.area) }}</dd>
              <dt v-if="selectedEntity.entity_type === 'edge'">length</dt>
              <dd v-if="selectedEntity.entity_type === 'edge'">{{ formatNumber(selectedEntity.length) }}</dd>
              <dt v-if="selectedEntity.entity_type === 'vertex'">坐标</dt>
              <dd v-if="selectedEntity.entity_type === 'vertex'">{{ vertexCoordinate(selectedEntity) }}</dd>
              <dt v-if="selectedEntity.entity_type !== 'vertex'">center</dt>
              <dd v-if="selectedEntity.entity_type !== 'vertex'">{{ formatPoint(selectedEntity.center) }}</dd>
              <dt v-if="selectedEntity.entity_type !== 'vertex'">bounding_box.min</dt>
              <dd v-if="selectedEntity.entity_type !== 'vertex'">{{ getBoundingBoxValue(selectedEntity, 'min') }}</dd>
              <dt v-if="selectedEntity.entity_type !== 'vertex'">bounding_box.max</dt>
              <dd v-if="selectedEntity.entity_type !== 'vertex'">{{ getBoundingBoxValue(selectedEntity, 'max') }}</dd>
            </dl>
          </section>

          <section v-if="selectedEntity.entity_type !== 'vertex'" class="property-group">
            <div class="property-title">专用几何参数</div>
            <dl class="property-grid">
              <template v-if="geometryParamEntries(selectedEntity).length">
                <template v-for="item in geometryParamEntries(selectedEntity)" :key="item.key">
                  <dt>{{ item.key }}</dt>
                  <dd>{{ item.value }}</dd>
                </template>
              </template>
              <template v-else>
                <dt>参数</dt>
                <dd>{{ EMPTY_TEXT }}</dd>
              </template>
            </dl>
          </section>

          <section class="property-group">
            <div class="property-title">拓扑关系</div>
            <template v-if="selectedEntity.entity_type === 'face'">
              <div class="sub-title">关联 Edge</div>
              <div v-if="faceTopology?.edges?.length" class="relation-list">
                <ElButton
                  v-for="edge in faceTopology.edges"
                  :key="edge.id"
                  link
                  type="primary"
                  @click="selectEntity(edge)"
                >
                  {{ displayName(edge) }} · {{ empty(edge.geometry_type) }}
                </ElButton>
              </div>
              <div v-else class="empty-inline">{{ EMPTY_TEXT }}</div>

              <div class="sub-title">相邻 Face</div>
              <div v-if="faceTopology?.adjacent_faces?.length" class="relation-list">
                <ElButton
                  v-for="face in faceTopology.adjacent_faces"
                  :key="face.id"
                  link
                  type="primary"
                  @click="selectEntity(face)"
                >
                  {{ displayName(face) }}
                </ElButton>
              </div>
              <div v-else class="empty-inline">{{ EMPTY_TEXT }}</div>
            </template>

            <template v-else-if="selectedEntity.entity_type === 'edge'">
              <div class="sub-title">关联 Vertex</div>
              <div v-if="edgeTopology?.vertices?.length" class="relation-list">
                <ElButton
                  v-for="vertex in edgeTopology.vertices"
                  :key="vertex.id"
                  link
                  type="primary"
                  @click="selectEntity(vertex)"
                >
                  {{ displayName(vertex) }} · {{ vertexCoordinateShort(vertex) }}
                </ElButton>
              </div>
              <div v-else class="empty-inline">{{ EMPTY_TEXT }}</div>

              <div class="sub-title">所属 Face</div>
              <div v-if="edgeTopology?.faces?.length" class="relation-list">
                <ElButton
                  v-for="face in edgeTopology.faces"
                  :key="face.id"
                  link
                  type="primary"
                  @click="selectEntity(face)"
                >
                  {{ displayName(face) }}
                </ElButton>
              </div>
              <div v-else class="empty-inline">{{ EMPTY_TEXT }}</div>
            </template>

            <div v-else class="empty-inline">{{ EMPTY_TEXT }}</div>
          </section>
        </template>

        <template v-else-if="selectedMeasurement">
          <section class="property-group">
            <div class="property-title">测量详情</div>
            <dl class="property-grid">
              <dt>类型</dt>
              <dd>{{ selectedMeasurement.measurement_type }}</dd>
              <dt>归一化值</dt>
              <dd>{{ normalizedValueText(selectedMeasurement) }}</dd>
              <dt>单位</dt>
              <dd>{{ empty(selectedMeasurement.unit) }}</dd>
              <dt>置信度</dt>
              <dd>{{ confidenceText(selectedMeasurement.confidence) }}</dd>
              <dt>算法</dt>
              <dd>{{ selectedMeasurement.algorithm_version }}</dd>
              <dt>method</dt>
              <dd>{{ selectedMeasurement.method }}</dd>
              <dt>source</dt>
              <dd>{{ selectedMeasurement.source_entity_ids.join(', ') }}</dd>
            </dl>
          </section>

          <section class="property-group">
            <div class="property-title">原始/归一化</div>
            <dl class="property-grid">
              <dt>raw_value</dt>
              <dd>{{ formatValue(selectedMeasurement.raw_value) }}</dd>
              <dt>normalized</dt>
              <dd>{{ formatValue(selectedMeasurement.normalized_value) }}</dd>
            </dl>
          </section>
        </template>

        <template v-else-if="selectedFeature">
          <section class="property-group">
            <div class="property-title">特征详情</div>
            <dl class="property-grid">
              <dt>类型</dt>
              <dd>{{ selectedFeature.feature_type }}</dd>
              <dt>置信度</dt>
              <dd>{{ confidenceText(selectedFeature.confidence) }}</dd>
              <dt>算法</dt>
              <dd>{{ selectedFeature.algorithm }}</dd>
              <dt>版本</dt>
              <dd>{{ selectedFeature.algorithm_version }}</dd>
              <dt>状态</dt>
              <dd>{{ selectedFeature.status }}</dd>
              <dt>source</dt>
              <dd>{{ selectedFeature.source_entity_ids.join(', ') }}</dd>
            </dl>
          </section>

          <section v-if="selectedFeature.feature_type === 'circular_pattern'" class="property-group">
            <div class="property-title">圆周阵列</div>
            <dl class="property-grid">
              <dt>count</dt>
              <dd>{{ formatValue(selectedFeature.parameters.count) }}</dd>
              <dt>孔径</dt>
              <dd>{{ formatValue(selectedFeature.parameters.member_diameter) }}</dd>
              <dt>PCD</dt>
              <dd>{{ formatValue(selectedFeature.parameters.pitch_circle_diameter) }}</dd>
              <dt>角间隔</dt>
              <dd>{{ formatValue(selectedFeature.parameters.angular_spacing) }}</dd>
              <dt>fit_residual</dt>
              <dd>{{ formatValue(selectedFeature.parameters.fit_residual) }}</dd>
            </dl>
          </section>

          <section class="property-group">
            <div class="property-title">参数</div>
            <dl class="property-grid">
              <template v-for="[key, value] in Object.entries(selectedFeature.parameters)" :key="key">
                <dt>{{ key }}</dt>
                <dd>{{ formatValue(value) }}</dd>
              </template>
            </dl>
          </section>
        </template>

        <ElDescriptions v-else-if="selectedNode" :column="1" border size="small">
          <ElDescriptionsItem label="名称">{{ displayName(selectedNode) }}</ElDescriptionsItem>
          <ElDescriptionsItem label="UUID">{{ selectedNode.id }}</ElDescriptionsItem>
          <ElDescriptionsItem label="类型">{{ entityTypeLabel(selectedNode.entity_type) }}</ElDescriptionsItem>
          <ElDescriptionsItem label="source_ref">{{ empty(selectedNode.source_ref) }}</ElDescriptionsItem>
          <ElDescriptionsItem v-if="selectedNode.entity_type === 'solid'" label="Face">
            {{ solidFaceIds.length || EMPTY_TEXT }}
          </ElDescriptionsItem>
        </ElDescriptions>

        <ElEmpty v-else description="点击结构、面、边或顶点查看属性" />
      </aside>

      <button v-if="isRightCollapsed" class="collapse-rail right-rail" type="button" @click="isRightCollapsed = false">
        属性
      </button>
    </div>
  </div>
</template>

<style scoped>
.cad-page {
  display: flex;
  height: calc(100vh - 118px);
  min-height: 620px;
  flex-direction: column;
  gap: 10px;
  overflow: hidden;
}

.cad-toolbar {
  display: flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 10px;
  border-bottom: 1px solid var(--el-border-color-light);
  padding: 8px 0 10px;
}

.status-area {
  display: flex;
  min-width: 280px;
  flex: 1;
  align-items: center;
  gap: 12px;
}

.status-progress {
  max-width: 360px;
  flex: 1;
}

.status-counts {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  color: var(--el-text-color-secondary);
  font-size: 13px;
}

.cad-shell {
  display: grid;
  min-height: 0;
  flex: 1;
  grid-template-columns: 360px minmax(420px, 1fr) 340px;
  gap: 10px;
}

.cad-shell.left-collapsed {
  grid-template-columns: 56px minmax(420px, 1fr) 340px;
}

.cad-shell.right-collapsed {
  grid-template-columns: 360px minmax(420px, 1fr) 36px;
}

.cad-shell.left-collapsed.right-collapsed {
  grid-template-columns: 56px minmax(420px, 1fr) 36px;
}

.left-panel,
.right-panel {
  min-width: 0;
  min-height: 0;
  border: 1px solid var(--el-border-color-light);
  border-radius: 8px;
  background: var(--el-bg-color);
}

.left-panel {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.navigation-heading {
  display: flex;
  min-height: 48px;
  align-items: center;
  justify-content: space-between;
  border-bottom: 1px solid var(--el-border-color-light);
  padding: 0 14px;
}

.navigation-heading button {
  border: 0;
  background: transparent;
  color: var(--el-text-color-secondary);
  cursor: pointer;
  font-size: 22px;
}

.navigation-tabs {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  border-bottom: 1px solid var(--el-border-color-light);
}

.navigation-tabs button {
  border: 0;
  border-bottom: 2px solid transparent;
  background: transparent;
  color: var(--el-text-color-secondary);
  cursor: pointer;
  padding: 11px 4px;
}

.navigation-tabs button.active {
  border-bottom-color: var(--el-color-primary);
  color: var(--el-color-primary);
}

.tree-section,
.geometry-section {
  flex: 1;
}

.right-panel {
  overflow: auto;
  padding: 12px;
}

.panel-section {
  min-height: 0;
  border-bottom: 1px solid var(--el-border-color-light);
  padding: 10px 12px;
}

.panel-section:last-child {
  border-bottom: 0;
}

.panel-title {
  display: flex;
  min-height: 22px;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
  color: var(--el-text-color-primary);
  font-size: 14px;
  font-weight: 600;
}

.models-scroll {
  height: 68px;
}

.model-row,
.geometry-item {
  display: flex;
  width: 100%;
  flex-direction: column;
  align-items: flex-start;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: inherit;
  cursor: pointer;
  text-align: left;
}

.model-row {
  gap: 3px;
  margin-bottom: 4px;
  padding: 6px 8px;
}

.model-row:hover,
.model-row.active,
.geometry-item:hover,
.geometry-item.selected {
  background: var(--el-color-primary-light-9);
}

.model-name,
.geometry-item-title {
  width: 100%;
  overflow: hidden;
  color: var(--el-text-color-primary);
  font-size: 13px;
  font-weight: 600;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.model-meta,
.geometry-item-summary,
.geometry-total {
  color: var(--el-text-color-secondary);
  font-size: 12px;
}

.tree-section {
  overflow: hidden;
}

.tree-scroll {
  height: 132px;
}

.tree-section :deep(.el-tree-node__label) {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.geometry-section {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.geometry-title {
  margin-bottom: 2px;
}

.compact-tabs {
  flex: 0 0 auto;
}

.compact-tabs :deep(.el-tabs__header) {
  margin-bottom: 8px;
}

.compact-tabs :deep(.el-tabs__item) {
  height: 30px;
  padding: 0 10px;
  font-size: 12px;
}

.geometry-tools {
  display: grid;
  flex: 0 0 auto;
  grid-template-columns: minmax(110px, 1fr) 92px 32px 42px;
  gap: 6px;
  margin-bottom: 8px;
}

.measurement-action-row {
  display: flex;
  flex: 0 0 auto;
  justify-content: flex-end;
  margin: -2px 0 8px;
}

.geometry-list {
  min-height: 0;
  flex: 1;
  overflow: auto;
  padding-right: 2px;
}

.geometry-item {
  gap: 4px;
  margin-bottom: 6px;
  padding: 8px;
}

.geometry-item.selected {
  outline: 1px solid var(--el-color-primary);
}

.compact-pagination {
  display: flex;
  flex: 0 0 auto;
  justify-content: center;
  border-top: 1px solid var(--el-border-color-lighter);
  margin-top: 8px;
  padding-top: 8px;
}

.viewer-panel {
  min-width: 0;
  min-height: 0;
}

.collapse-rail {
  display: flex;
  min-width: 0;
  min-height: 0;
  align-items: center;
  flex-direction: column;
  gap: 10px;
  border: 1px solid var(--el-border-color-light);
  border-radius: 8px;
  background: var(--el-bg-color);
  padding: 10px 6px;
}

.rail-action {
  display: grid;
  width: 42px;
  height: 42px;
  place-items: center;
  border: 1px solid var(--el-border-color-light);
  border-radius: 7px;
  background: var(--el-bg-color);
  color: var(--el-text-color-primary);
  cursor: pointer;
  padding: 0;
}

.rail-action svg {
  width: 22px;
  height: 22px;
  fill: none;
  stroke: currentcolor;
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: 1.7;
}

.rail-action:hover,
.rail-action.active {
  border-color: var(--el-color-primary-light-5);
  background: var(--el-color-primary-light-9);
  color: var(--el-color-primary);
}

.rail-action.expand {
  margin-top: 2px;
}

.property-group {
  border-bottom: 1px solid var(--el-border-color-light);
  padding: 0 0 12px;
  margin-bottom: 12px;
}

.property-group:last-child {
  border-bottom: 0;
  margin-bottom: 0;
}

.property-title {
  margin-bottom: 8px;
  color: var(--el-text-color-primary);
  font-size: 13px;
  font-weight: 600;
}

.property-grid {
  display: grid;
  grid-template-columns: 118px minmax(0, 1fr);
  gap: 6px 10px;
  margin: 0;
  font-size: 12px;
}

.property-grid dt {
  color: var(--el-text-color-secondary);
}

.property-grid dd {
  min-width: 0;
  margin: 0;
  overflow-wrap: anywhere;
  color: var(--el-text-color-primary);
}

.sub-title {
  margin: 10px 0 6px;
  color: var(--el-text-color-regular);
  font-size: 12px;
  font-weight: 600;
}

.relation-list {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 4px;
}

.empty-inline {
  color: var(--el-text-color-placeholder);
  font-size: 12px;
}

@media (max-width: 1180px) {
  .cad-shell {
    grid-template-columns: 330px minmax(360px, 1fr);
  }

  .right-panel,
  .right-rail {
    display: none;
  }
}

@media (max-width: 820px) {
  .cad-page {
    height: auto;
    min-height: calc(100vh - 118px);
    overflow: visible;
  }

  .cad-toolbar {
    flex-wrap: wrap;
  }

  .cad-shell,
  .cad-shell.left-collapsed,
  .cad-shell.right-collapsed,
  .cad-shell.left-collapsed.right-collapsed {
    display: flex;
    flex-direction: column;
  }

  .left-panel {
    min-height: 560px;
  }

  .viewer-panel {
    min-height: 460px;
  }

  .right-panel {
    display: block;
    min-height: 360px;
  }

  .collapse-rail {
    padding: 8px;
  }
}
</style>
