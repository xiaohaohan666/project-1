/* ============================================================
 *  api.js —— 一卡通 REST 客户端
 *  身份通过请求头 X-Token（卡号或 ADMIN）与 X-Pwd（密码）携带，
 *  消费类请求额外携带交易密码 X-PayPwd。
 *  服务端每次校验，因此刷新页面、多端同时用都不需要服务端会话。
 *
 *  后端地址可配置：前端既可被同一个 C 服务托管（默认同源），
 *  也可部署在静态托管（如 GitHub Pages）而把接口指向另一台服务器。
 *  地址保存在 localStorage，用户可在「设置」中修改。
 * ============================================================ */
const API = (() => {
  const K_TOK = 'campus.token';
  const K_PWD = 'campus.pwd';
  const K_PAY = 'campus.paypwd';
  const K_API = 'campus.apiBase';
  let token = localStorage.getItem(K_TOK) || '';
  let pwd   = localStorage.getItem(K_PWD) || '';
  let payPwd = localStorage.getItem(K_PAY) || '';
  /* 默认同源：由 C 服务同时托管页面与接口时无需配置 */
  let base = localStorage.getItem(K_API) || '';

  /* 拼接完整地址。path 以 / 开头 */
  function url(path) {
    if (!base) return path;
    return base.replace(/\/+$/, '') + path;
  }

  /* 统一请求。path 路径；method 方法；body 请求体对象 */
  async function req(path, method = 'GET', body = null) {
    const headers = {};
    if (token)  headers['X-Token'] = token;      /* 身份令牌 */
    if (pwd)    headers['X-Pwd'] = pwd;          /* 登录密码 */
    if (payPwd) headers['X-PayPwd'] = payPwd;    /* 交易密码 */
    if (body)   headers['Content-Type'] = 'application/json';

    let res;
    try {
      res = await fetch(url(path), {
        method, headers,
        body: body ? JSON.stringify(body) : undefined
      });
    } catch (e) {
      throw new Error(navigator.onLine
        ? '无法连接服务，请检查「设置」中的服务器地址'
        : '当前离线，请联网后重试');
    }
    let data;
    try { data = await res.json(); } catch (e) { throw new Error('服务返回异常'); }
    if (!data || data.ok !== true) throw new Error((data && data.error) || '操作失败');
    return data;
  }
  const get = (p) => req(p);
  const post = (p, b) => req(p, 'POST', b || {});

  return {
    get token() { return token; },
    get payPwd() { return payPwd; },
    /* 后端地址（空表示同源） */
    get apiBase() { return base; },
    setApiBase(v) {
      base = (v || '').trim().replace(/\/+$/, '');
      if (base) localStorage.setItem(K_API, base);
      else localStorage.removeItem(K_API);
    },
    /* 登录成功后保存身份 */
    setAuth(t, p) {
      token = t || ''; pwd = p || '';
      if (token) localStorage.setItem(K_TOK, token); else localStorage.removeItem(K_TOK);
      if (pwd)   localStorage.setItem(K_PWD, pwd);   else localStorage.removeItem(K_PWD);
    },
    /* 交易密码单独保存（消费时服务端校验） */
    setPayPwd(p) {
      payPwd = p || '';
      if (payPwd) localStorage.setItem(K_PAY, payPwd); else localStorage.removeItem(K_PAY);
    },

    /* ---- 通用 ---- */
    health:      ()                => get('/api/health'),
    login:       (id, p, admin)    => post('/api/login', { id, pwd: p, admin: !!admin }),
    register:    (id, name, number, p) =>
                   post('/api/register', { id, name, number, pwd: p }),
    merchants:   ()                => get('/api/merchants'),

    /* ---- 学生端 ---- */
    card:        ()                => get('/api/card'),
    records:     ()                => get('/api/records'),
    stats:       (period)          => get('/api/stats?period=' + period),
    ledger:      (period, offset)  => get('/api/ledger?period=' + period +
                                          '&offset=' + (offset || 0)),
    consume:     (period)          => get('/api/consume?period=' + period),
    /* 消费：交易密码可显式传入，否则用本地保存的 */
    pay:         (merchant, amount, p) =>
                   post('/api/pay', { merchant, amount, payPwd: p || payPwd }),
    online:      (place)           => post('/api/machine', { action: 'online', place }),
    offline:     ()                => post('/api/machine', { action: 'offline' }),
    selfLoss:    ()                => post('/api/lost'),
    selfUnlost:  ()                => post('/api/unlost'),
    changePwd:   (newPwd)          => post('/api/pwd', { newPwd }),
    changePayPwd:(newPwd)          => post('/api/paypwd', { newPwd }),
    /* 门禁 / 考勤：dir 0 进入 1 离开；kind 0 门禁 1 考勤 */
    access:      (place, dir, kind) =>
                   post('/api/access', { place, dir, kind }),
    accessLog:   ()                => get('/api/access'),

    /* ---- 管理员端 ---- */
    cards:       ()                => get('/api/admin/cards'),
    income:      ()                => get('/api/admin/income'),
    report:      ()                => get('/api/admin/report'),
    admLedger:   (period, offset)  => get('/api/admin/ledger?period=' + period +
                                          '&offset=' + (offset || 0)),
    admRecords:  (id)              => get('/api/admin/records?id=' + encodeURIComponent(id)),
    admAccess:   (id)              => get('/api/admin/access?id=' + encodeURIComponent(id || '')),
    /* 卡务操作：op 为 recharge/lost/unlost/freeze/reset/resetpay/reissue/limits/expire/grant/pay */
    adminOp:     (op, body)        => post('/api/admin/' + op, body)
  };
})();
