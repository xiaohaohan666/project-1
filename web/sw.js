/* ============================================================
 *  sw.js —— Service Worker（PWA 核心）
 * ------------------------------------------------------------
 *  两条策略：
 *    · 应用外壳（HTML/CSS/JS/图标/清单）→ stale-while-revalidate
 *      先返回缓存（秒开），同时后台更新，下次进页面即为新版
 *    · 接口请求（/api/）→ network-only
 *      计费与余额必须实时，绝不使用缓存，离线时明确报错
 *
 *  改版本号即可让所有旧缓存失效（客户端下次访问时清理）
 * ============================================================ */
const VERSION = 'v1';
const SHELL_CACHE = `campus-shell-${VERSION}`;

/* 需要预缓存的资源：首次安装后即可离线打开界面 */
const SHELL_FILES = [
  '/',
  '/index.html',
  '/manifest.webmanifest',
  '/css/app.css',
  '/js/api.js',
  '/js/ui.js',
  '/js/app.js',
  '/icons/icon-192.png',
  '/icons/icon-512.png',
  '/icons/maskable-512.png',
  '/icons/apple-touch-icon.png'
];

/* 安装：预缓存外壳资源 */
self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(SHELL_CACHE);
    /* 逐个添加：任一资源失败不影响整体安装 */
    await Promise.all(SHELL_FILES.map(async (url) => {
      try {
        const res = await fetch(new Request(url, { cache: 'reload' }));
        if (res && res.ok) await cache.put(url, res);
      } catch (e) {
        /* 离线或该资源暂不可用，跳过 */
      }
    }));
    await self.skipWaiting();            /* 新 SW 立即进入 waiting → activate */
  })());
});

/* 激活：清理旧版本缓存 */
self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.map((k) => {
      if (k.startsWith('campus-shell-') && k !== SHELL_CACHE) return caches.delete(k);
      return null;
    }));
    if (self.registration.navigationPreload) {
      try { await self.registration.navigationPreload.disable(); } catch (e) { /* 忽略 */ }
    }
    await self.clients.claim();          /* 立即接管已打开的页面 */
  })());
});

/* 拦截请求 */
self.addEventListener('fetch', (event) => {
  const req = event.request;
  const url = new URL(req.url);

  /* 只处理同源请求；跨域交给浏览器默认行为 */
  if (url.origin !== self.location.origin) return;

  /* 接口请求：只走网络，绝不缓存（余额与计费必须实时） */
  if (url.pathname.startsWith('/api/')) {
    event.respondWith((async () => {
      try {
        return await fetch(req);
      } catch (e) {
        /* 离线时给出结构化错误，前端能按统一格式提示 */
        return new Response(
          JSON.stringify({ ok: false, error: '离线状态：无法连接服务器' }),
          { status: 503, headers: { 'Content-Type': 'application/json; charset=utf-8' } }
        );
      }
    })());
    return;
  }

  /* 只缓存 GET；其他方法直接放行 */
  if (req.method !== 'GET') return;

  /* 应用外壳：先缓存后网络（stale-while-revalidate） */
  event.respondWith((async () => {
    const cache = await caches.open(SHELL_CACHE);
    const cached = await cache.match(url.pathname === '/' ? '/index.html' : req, { ignoreSearch: true });

    const network = fetch(req).then((res) => {
      if (res && res.ok && res.type === 'basic') {
        cache.put(req, res.clone()).catch(() => {});     /* 后台更新缓存 */
      }
      return res;
    }).catch(() => null);

    if (cached) return cached;                           /* 命中缓存：秒开 */

    const res = await network;
    if (res) return res;

    /* 离线且未缓存：导航请求回退到首页（前端自身处理登录态） */
    if (req.mode === 'navigate') {
      const fallback = await cache.match('/index.html');
      if (fallback) return fallback;
    }
    return new Response('离线且无缓存', {
      status: 504, headers: { 'Content-Type': 'text/plain; charset=utf-8' }
    });
  })());
});
