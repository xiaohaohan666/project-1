/* ============================================================
 *  api.js —— REST 客户端
 *  身份通过请求头 X-Token（卡号或 ADMIN）与 X-Pwd（密码）携带，
 *  服务端每次校验，因此刷新页面、多端同时用都不需要服务端会话。
 * ============================================================ */
const API = (() => {
  const K_TOK = 'campus.token';
  const K_PWD = 'campus.pwd';
  let token = localStorage.getItem(K_TOK) || '';
  let pwd   = localStorage.getItem(K_PWD) || '';

  /* 统一请求。path 路径；method 方法；body 请求体对象 */
  async function req(path, method = 'GET', body = null) {
    const headers = {};
    if (token) headers['X-Token'] = token;      /* 身份令牌 */
    if (pwd)   headers['X-Pwd'] = pwd;          /* 对应密码 */
    if (body)  headers['Content-Type'] = 'application/json';

    let res;
    try {
      res = await fetch(path, {
        method, headers,
        body: body ? JSON.stringify(body) : undefined
      });
    } catch (e) {
      throw new Error('无法连接服务，请确认后端已启动');
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
    /* 登录成功后保存身份 */
    setAuth(t, p) {
      token = t || ''; pwd = p || '';
      if (token) localStorage.setItem(K_TOK, token); else localStorage.removeItem(K_TOK);
      if (pwd)   localStorage.setItem(K_PWD, pwd);   else localStorage.removeItem(K_PWD);
    },
    health:   ()                    => get('/api/health'),
    login:    (id, p, admin)        => post('/api/login', { id, pwd: p, admin: !!admin }),
    card:     ()                    => get('/api/card'),
    records:  ()                    => get('/api/records'),
    stats:    (period)              => get('/api/stats?period=' + period),
    online:   (place)               => post('/api/machine', { action: 'online', place }),
    offline:  ()                    => post('/api/machine', { action: 'offline' }),
    selfLoss: ()                    => post('/api/lost'),
    changePwd:(newPwd)              => post('/api/pwd', { newPwd }),
    cards:    ()                    => get('/api/admin/cards'),
    income:   ()                    => get('/api/admin/income'),
    admRecords: (id)                => get('/api/admin/records?id=' + encodeURIComponent(id)),
    adminOp:  (op, body)            => post('/api/admin/' + op, body)
  };
})();
