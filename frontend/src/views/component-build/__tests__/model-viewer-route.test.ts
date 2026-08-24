import assert from 'node:assert/strict';
import test from 'node:test';
import { modelViewerLocation } from '../model-viewer-route';

// 用途：Feature Center 暂停注册期间，所有源模型统一进入 CAD 模型解析。
test('step opens cad model analysis route', () => {
  assert.deepEqual(modelViewerLocation('build-1', 'revision-1', 'STEP'), {
    path: '/cad-model',
    query: { build_id: 'build-1', revision_id: 'revision-1', source_format: 'STEP' }
  });
});

test('catpart opens cad model analysis route while feature center is disabled', () => {
  assert.deepEqual(modelViewerLocation('build-2', 'revision-2', 'CATPART'), {
    path: '/cad-model',
    query: { build_id: 'build-2', revision_id: 'revision-2', source_format: 'CATPART' }
  });
});

test('catproduct opens cad model analysis route while feature center is disabled', () => {
  assert.deepEqual(modelViewerLocation('build-3', 'revision-3', 'CATPRODUCT'), {
    path: '/cad-model',
    query: { build_id: 'build-3', revision_id: 'revision-3', source_format: 'CATPRODUCT' }
  });
});
