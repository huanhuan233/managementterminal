// 用途：Feature Center 暂停注册期间，统一进入 CAD 模型解析，避免产生不可访问路由。
export function modelViewerLocation(
  buildId: string,
  revisionId: string,
  sourceFormat: 'STEP' | 'CATPART' | 'CATPRODUCT' | null | undefined
) {
  return {
    path: '/cad-model',
    query: { build_id: buildId, revision_id: revisionId, source_format: sourceFormat || '' }
  };
}
