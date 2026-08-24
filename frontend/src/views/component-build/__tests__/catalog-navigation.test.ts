import assert from 'node:assert/strict'
import test from 'node:test'
import { buildCatalogNavigation } from '../catalog-navigation'
import { isSupportedPartSourceFile } from '../source-file'

function makeLibrary(id: string, label: string, category: string, buildCount: number) {
  return {
    id,
    label,
    node_type: 'library',
    library_code: id,
    children: [{
      id: `${id}-family`,
      label: `${label}分类`,
      label_en: 'Family',
      node_type: 'family',
      category_code: category,
      children: [{
        id: `${id}-type`,
        label: '中文零件类型',
        label_en: 'English Part Type',
        node_type: 'type',
        category_code: category,
        part_type_code: `${category}-type`,
        children: Array.from({ length: buildCount }, (_, index) => ({
          id: `${id}-build-${index}`,
          label: '版本',
          node_type: 'build',
          children: []
        }))
      }]
    }]
  }
}

test('机械库作为唯一系统根显示', () => {
  const items = buildCatalogNavigation([
    makeLibrary('MECHANICAL_COMPONENT_LIBRARY', '机械工程图元库', 'mechanical', 2)
  ])
  const roots = items.filter(item => item.nodeType === 'library')
  assert.deepEqual(roots.map(item => item.parentId), [undefined])
  assert.deepEqual(roots.map(item => item.count), [2])
  assert.deepEqual(roots.map(item => item.label), ['机械工程图元库'])
  assert.equal(items.some(item => item.label === '中文零件类型'), true)
  assert.equal(items.some(item => item.label === 'English Part Type'), false)
})

test('前端隐藏航空航天零件库及其子分类', () => {
  const items = buildCatalogNavigation([
    makeLibrary('MECHANICAL_COMPONENT_LIBRARY', '机械工程图元库', 'mechanical', 2),
    makeLibrary('AEROSPACE_PART_LIBRARY', '航空航天零件库', 'aero-general', 1)
  ])
  assert.equal(items.some(item => item.label.includes('航空航天')), false)
  assert.equal(items.some(item => item.code === 'AEROSPACE_PART_LIBRARY'), false)
  assert.equal(items.some(item => item.categoryCode?.startsWith('aero-')), false)
})

test('上传提示接受 STEP、STP、CATPart、CATProduct 和依赖 ZIP，且大小写不敏感', () => {
  assert.equal(isSupportedPartSourceFile('零件 (1).STEP'), true)
  assert.equal(isSupportedPartSourceFile('零件.stp'), true)
  assert.equal(isSupportedPartSourceFile('框体.CATPart'), true)
  assert.equal(isSupportedPartSourceFile('装配.CATProduct'), true)
  assert.equal(isSupportedPartSourceFile('装配依赖.ZIP'), true)
  assert.equal(isSupportedPartSourceFile('错误.cart'), false)
})
