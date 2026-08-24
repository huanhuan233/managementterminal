import type { CustomRoute, ElegantConstRoute, ElegantRoute } from '@elegant-router/types';
import { layouts, views } from '../elegant/imports';
import { transformElegantRoutesToVueRoutes } from '../elegant/transform';

const componentBuildRoute = {
  name: 'component-build',
  path: '/component-build',
  component: 'layout.base$view.component-build',
  meta: {
    title: '图元建库',
    icon: 'carbon:tree-view-alt',
    order: 1
  }
} as unknown as CustomRoute;

const cadModelRoute = {
  name: 'cad-model',
  path: '/cad-model',
  component: 'layout.base$view.cad-model',
  meta: {
    title: 'CAD 模型解析',
    icon: 'carbon:assembly-cluster',
    order: 2
  }
} as unknown as CustomRoute;

const customRoutes: CustomRoute[] = [componentBuildRoute, cadModelRoute];

/** create routes when the auth route mode is static */
export function createStaticRoutes() {
  const constantRoutes: ElegantRoute[] = [];
  const authRoutes: ElegantRoute[] = [];

  customRoutes.forEach(item => {
    if (item.meta?.constant) {
      constantRoutes.push(item);
    } else {
      authRoutes.push(item);
    }
  });

  return {
    constantRoutes,
    authRoutes
  };
}

/**
 * Get auth vue routes
 *
 * @param routes Elegant routes
 */
export function getAuthVueRoutes(routes: ElegantConstRoute[]) {
  return transformElegantRoutesToVueRoutes(routes, layouts, views);
}
