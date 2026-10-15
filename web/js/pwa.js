/* ============================================================
 *  pwa.js —— PWA 装配：Service Worker 注册、安装引导、离线提示
 * ------------------------------------------------------------
 *  职责边界：只管"应用化"能力，不碰业务逻辑。
 *  对外暴露 window.PWA，供 app.js 调用（安装按钮、在线状态）。
 * ============================================================ */
window.PWA = (() => {
  const { $ } = UI;

  /* 运行形态判定 */
  const standalone = window.matchMedia('(display-mode: standalone)').matches
    || window.navigator.standalone === true;      /* iOS Safari 专有属性 */
  const isIOS = /iP(hone|ad|od)/.test(navigator.userAgent)
    || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);

  let deferredPrompt = null;                       /* 暂存安装事件，待用户点按钮再用 */
  let installable = false;

  /* ---------------- Service Worker ---------------- */
  async function registerSW() {
    if (!('serviceWorker' in navigator)) return;
    /* 非安全上下文（http 且非 localhost）无法注册 SW，直接跳过 */
    if (!window.isSecureContext) {
      console.info('[PWA] 非安全上下文，跳过 Service Worker 注册');
      return;
    }
    try {
      /* 用相对路径注册，兼容子路径部署（如 GitHub Pages 的 /project-1/） */
      const reg = await navigator.serviceWorker.register('sw.js');
      /* 有新版本时静默更新，下次进入即为新版 */
      reg.addEventListener('updatefound', () => {
        const sw = reg.installing;
        if (!sw) return;
        sw.addEventListener('statechange', () => {
          if (sw.state === 'installed' && navigator.serviceWorker.controller) {
            console.info('[PWA] 新版本已就绪，下次打开生效');
          }
        });
      });
      console.info('[PWA] Service Worker 已注册，作用域:', reg.scope);
    } catch (e) {
      console.warn('[PWA] Service Worker 注册失败：', e.message);
    }
  }

  /* ---------------- 安装引导 ---------------- */
  /* 浏览器判定可安装时触发（Chrome/Edge/Android） */
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();                            /* 阻止浏览器默认迷你提示条 */
    deferredPrompt = e;
    installable = true;
    showInstallUI();
  });

  /* 已安装完成 */
  window.addEventListener('appinstalled', () => {
    deferredPrompt = null;
    installable = false;
    hideInstallUI();
    UI.ok('已安装到本机');
  });

  function showInstallUI() {
    $('#btnInstall')?.classList.remove('hidden');
    const hint = $('#installHint');
    if (hint && !$('#login').classList.contains('hidden')) hint.classList.remove('hidden');
  }
  function hideInstallUI() {
    $('#btnInstall')?.classList.add('hidden');
    $('#installHint')?.classList.add('hidden');
  }

  /* 触发安装：优先用浏览器事件；不支持时给出各平台手动步骤 */
  async function install() {
    if (standalone) { UI.ok('已经以应用方式运行'); return; }
    if (deferredPrompt) {
      deferredPrompt.prompt();
      const { outcome } = await deferredPrompt.userChoice;
      deferredPrompt = null;
      installable = false;
      if (outcome === 'accepted') hideInstallUI();
      return;
    }
    /* 手动引导 */
    const text = isIOS
      ? '在 Safari 中点「分享」按钮 → 选择「添加到主屏幕」→ 点「添加」'
      : '在浏览器菜单中（右上角 ⋮ 或 ⋯）选择「安装应用」或「添加到主屏幕」';
    UI.openModal({
      title: '安装到本机',
      bodyHTML: `<p>${text}</p>
        <p class="muted">安装后可全屏运行、有独立图标，断网也能打开界面。</p>`,
      actions: [{ label: '知道了', kind: 'primary' }]
    });
  }

  /* ---------------- 在线 / 离线状态 ---------------- */
  function applyOnlineState() {
    const off = !navigator.onLine;
    document.body.classList.toggle('is-offline', off);
    let bar = $('#offlineBar');
    if (off && !bar) {
      bar = document.createElement('div');
      bar.id = 'offlineBar';
      bar.className = 'offline-bar';
      bar.textContent = '离线状态：界面可浏览，但余额与计费需要联网';
      document.body.appendChild(bar);
    } else if (!off && bar) {
      bar.remove();
    }
  }
  window.addEventListener('online', () => { applyOnlineState(); UI.ok('网络已恢复'); });
  window.addEventListener('offline', applyOnlineState);

  /* ---------------- 初始化 ---------------- */
  function init() {
    /* 已是独立窗口形态：隐藏一切安装引导 */
    if (standalone) hideInstallUI();
    else if (isIOS) setTimeout(showInstallUI, 800);   /* iOS 无法自动判定，稍后显示引导 */

    $('#btnInstall') && ($('#btnInstall').onclick = install);
    $('#installHint') && ($('#installHint').onclick = install);

    applyOnlineState();
    registerSW();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  return {
    get isStandalone() { return standalone; },
    get canInstall() { return installable; },
    install
  };
})();
