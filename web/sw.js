/* ============================================================
 *  sw.js —— Service Worker（PWA 核心）
 * ------------------------------------------------------------
 *  两条策略：
 *    · 应用外壳（HTML/CSS/JS/图标/清单）→ stale-while-revalidate
 *      先返回缓存（秒开），同时后台更新，下次进页面即为新版
 *    · 接口请求（以 /api/ 结尾的路径）→ network-only
 *      计费与余额必须实时，绝不使用缓存，离线时明确报错
 *
 *  子路径部署：本文件既可能被部署在站点根（本地 C 服务），
 *  也可能被部署在子路径（如 GitHub Pages 的 /project-1/）。
 *  因此所有资源路径都基于自身作用域动态推导，不写死开头的 /。
 * ============================================================ */
const VERSION = 'v2';

/* 作用域基路径：sw.js 位于 <base>/sw.js，去掉文件名即得基路径 */
const BASE = self.location.pathname.replace(/\/sw\.js$/, '') || '';
const SCOPE = BASE ? BASE + '/' : '/';
const SHELL_CACHE = `campus-shell-${VERSION}`;

/* 需要预缓存的资源（相对作用域） */
const SHELL_FILES = [
  'index.html',
  'manifest.webmanifest',
  'css/app.css',
  'js/api.js',
  'js/ui.js',
  'js/app.js',
  'js/pwa.js',
  'icons/icon-192.png',
  'icons/icon-512.png',
  'icons/maskable-512.png',
  'icons/apple-touch-icon.png'
];

/* 把相对路径转成完整路径 */
const full = (p) => SCOPE + p;

/* 判断是否为接口请求（部署在子路径时接口可能在 /project-1/api/ 下，
 * 但外置后端时又是完全不同的源，故只按路径尾部判断） */
function isApi(url) {
  return url.pathname.indexOf('/api/') !== -1;
}

/* 安装：预缓存外壳资源 */
self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(SHELL_CACHE);
    /* 逐个添加：任一资源失败不影响整体安装 */
    await Promise.all(SHELL_FILES.map(async (rel) => {
      const url = full(rel);
      try {
        const res = await fetch(new Request(url, { cache: 'reload' }));
        if (res && res.ok) await cache.put(url, res);
      } catch (e) {
        /* 离线或该资源暂不可用，跳过 */
      }
    }));
    await self.skipWaiting();
  })());
});

/* 激活：清理旧版本缓存并立即接管页面 */
self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.map((k) =>
      (k.startsWith('campus-shell-') && k !== SHELL_CACHE) ? caches.delete(k) : null));
    await self.clients.claim();
  })());
});

/* 拦截请求 */
self.addEventListener('fetch', (event) => {
  const req = event.request;
  const url = new URL(req.url);

  /* 只处理 GET；其他方法直接放行 */
  if (req.method !== 'GET') return;

  /* 接口请求：只走网络，绝不缓存 */
  if (isApi(url)) {
    event.respondWith((async () => {
      try {
        return await fetch(req);
      } catch (e) {
        return new Response(
          JSON.stringify({ ok: false, error: '离线状态：无法连接服务器' }),
          { status: 503, headers: { 'Content-Type': 'application/json; charset=utf-8' } }
        );
      }
    })());
    return;
  }

  /* 只缓存同源的应用外壳 */
  if (url.origin !== self.location.origin) return;

  event.respondWith((async () => {
    const cache = await caches.open(SHELL_CACHE);
    /* 导航请求统一按 index.html 命中缓存 */
    const key = (req.mode === 'navigate') ? full('index.html') : req;
    const cached = await cache.match(key, { ignoreSearch: true });

    const network = fetch(req).then((res) => {
      if (res && res.ok && res.type === 'basic') {
        cache.put(req, res.clone()).catch(() => {});
      }
      return res;
    }).catch(() => null);

    if (cached) return cached;
    const res = await network;
    if (res) return res;

    if (req.mode === 'navigate') {
      const fallback = await cache.match(full('index.html'));
      if (fallback) return fallback;
    }
    return new Response('离线且无缓存', {
      status: 504, headers: { 'Content-Type': 'text/plain; charset=utf-8' }
    });
  })());
});
