/* ============================================================
 *  app.js —— 页面状态与业务交互
 * ============================================================ */
(() => {
  const { $, $$, ok, err, prompt, money, esc, emptyRow } = UI;
  const state = { admin: false, card: null, period: 'day', timer: null, rate: 2 };

  /* ---------------- 登录 ---------------- */
  let role = 'student';
  $$('#loginTabs .tab').forEach((t) => {
    t.onclick = () => {
      $$('#loginTabs .tab').forEach((x) => x.classList.remove('active'));
      t.classList.add('active');
      role = t.dataset.role;
      $('#fId').classList.toggle('hidden', role === 'admin');
      const h = $('#loginHint');
      h.classList.remove('error');
      h.textContent = role === 'admin'
        ? '演示管理员密码 admin123'
        : '演示卡号 C001 / C002 / C003，密码 123456';
    };
  });

  $('#loginForm').onsubmit = async (e) => {
    e.preventDefault();
    const id = $('#inId').value.trim();
    const pwd = $('#inPwd').value;
    const hint = $('#loginHint');
    hint.classList.remove('error');
    $('#btnLogin').disabled = true;
    try {
      const r = await API.login(id, pwd, role === 'admin');
      API.setAuth(r.token, pwd);           /* 保存身份，后续请求自动携带 */
      state.admin = !!r.admin;
      state.card = r.card;
      enterApp();
    } catch (ex) {
      hint.textContent = ex.message;
      hint.classList.add('error');
    } finally {
      $('#btnLogin').disabled = false;
    }
  };
  $('#btnAdminTip').onclick = () => $$('#loginTabs .tab')[1].click();

  /* ---------------- 进入 / 退出 ---------------- */
  function enterApp() {
    $('#login').classList.add('hidden');
    $('#app').classList.remove('hidden');
    $('#topRole').textContent = state.admin ? '管理员' : '学生';
    $('#topSub').textContent = state.admin ? '机房管理端' : '机房上机计费';
    $('#viewStudent').classList.toggle('hidden', state.admin);
    $('#viewAdmin').classList.toggle('hidden', !state.admin);
    refresh();
  }
  $('#btnLogout').onclick = () => {
    API.setAuth('', '');
    state.admin = false;
    state.card = null;
    stopTimer();
    $('#app').classList.add('hidden');
    $('#login').classList.remove('hidden');
    $('#inPwd').value = '';
  };
  $('#btnRefresh').onclick = () => refresh(true);

  /* 统一刷新入口。manual 为真时给出提示 */
  async function refresh(manual) {
    try {
      if (state.admin) await refreshAdmin();
      else             await refreshStudent();
      if (manual) ok('已刷新');
    } catch (ex) {
      if (/卡号或密码错误|缺少身份/.test(ex.message)) {
        API.setAuth('', '');
        $('#app').classList.add('hidden');
        $('#login').classList.remove('hidden');
        $('#loginHint').textContent = '登录状态已失效，请重新登录';
        $('#loginHint').classList.add('error');
      } else err(ex.message);
    }
  }

  /* ---------------- 学生视图 ---------------- */
  function paintCard(k) {
    state.card = k;
    $('#cvId').textContent = k.id;
    $('#cvName').textContent = k.name;
    $('#cvNumber').textContent = k.number;
    $('#cvRemain').textContent = money(k.remain);
    $('#cvRecharge').textContent = money(k.recharge);
    $('#cvStatus').textContent = k.stateText;
    /* 冻结或挂失时禁用上机按钮，并给出原因 */
    const blocked = k.state !== 0;
    $('#machineIdle').classList.toggle('hidden', k.online === 1);
    $('#machineBusy').classList.toggle('hidden', k.online !== 1);
    $('#btnOnline').disabled = blocked;
    $('#btnOnline').textContent = blocked ? `当前状态：${k.stateText}` : '上机登记';
    if (k.online === 1) startTimer(k.startTime); else stopTimer();
  }

  async function refreshStudent() {
    const c = await API.card();
    state.rate = c.rate;
    paintCard(c.card);
    $('#rateTip').textContent = money(c.rate) + ' 元 / 小时';

    const st = await API.stats(state.period);
    $('#stCount').textContent = st.count;
    $('#stMinutes').textContent = st.minutes;
    $('#stCost').textContent = money(st.cost);
    $('#stAvg').textContent = st.count ? Math.round(st.minutes / st.count) : 0;

    const r = await API.records();
    $('#recCount').textContent = `共 ${r.total} 条`;
    $('#tblRecords tbody').innerHTML = r.records.length
      ? r.records.slice().reverse().map((x) => `<tr>
          <td>${esc(x.date)}</td><td>${esc(x.startTime)}</td>
          <td class="num">${x.duration}</td><td class="num">${money(x.cost)}</td>
          <td>${esc(x.state)}</td><td>${esc(x.address)}</td></tr>`).join('')
      : emptyRow(6, '暂无上机记录');
  }

  /* 上机计时：以服务端开始时间为准本地走字，实时预估费用 */
  function startTimer(startTime) {
    stopTimer();
    const t0 = new Date(startTime.replace(/-/g, '/')).getTime();
    const tick = () => {
      const sec = Math.max(0, Math.floor((Date.now() - t0) / 1000));
      const hh = String(Math.floor(sec / 3600)).padStart(2, '0');
      const mm = String(Math.floor(sec % 3600 / 60)).padStart(2, '0');
      const ss = String(sec % 60).padStart(2, '0');
      const est = sec / 3600 * state.rate;
      $('#mTime').textContent = `${hh}:${mm}:${ss}`;
      $('#mWhere').innerHTML =
        `${esc(state.card ? state.card.address : '')} · 预估 <b id="mCost">${money(est)}</b> 元`;
    };
    tick();
    state.timer = setInterval(tick, 1000);
  }
  function stopTimer() {
    if (state.timer) clearInterval(state.timer);
    state.timer = null;
    $('#mTime').textContent = '00:00:00';
    const box = $('#mCost');
    if (box) box.textContent = '0.00';
  }

  /* 统计时间段切换 */
  $$('#segPeriod button').forEach((b) => {
    b.onclick = () => {
      $$('#segPeriod button').forEach((x) => x.classList.remove('on'));
      b.classList.add('on');
      state.period = b.dataset.p;
      refreshStudent();
    };
  });

  $('#btnOnline').onclick = async () => {
    const place = $('#selPlace').value;
    try {
      const r = await API.online(place);
      ok(`${r.message}（${r.card.address}）`);
      paintCard(r.card);
      refreshStudent();
    } catch (ex) { err(ex.message); }
  };
  $('#btnOffline').onclick = async () => {
    try {
      const r = await API.offline();
      ok(`下机成功，本次费用 ${money(r.cost)} 元`);
      paintCard(r.card);
      refreshStudent();
    } catch (ex) { err(ex.message); }
  };

  $('#tilePwd').onclick = async () => {
    const v = await prompt({
      title: '修改登录密码',
      fields: [{ name: 'newPwd', label: '新密码', type: 'password', placeholder: '至少 6 位' }],
      confirmLabel: '修改'
    });
    if (!v) return;
    try {
      ok((await API.changePwd(v.newPwd)).message);
      API.setAuth(API.token, v.newPwd);     /* 同步本地保存的密码 */
    } catch (ex) { err(ex.message); }
  };

  $('#tileLost').onclick = () => {
    const k = state.card || {};
    UI.openModal({
      title: '挂失确认',
      bodyHTML: `<p>挂失后该卡不能上机，需管理员重置密码才能解除冻结。</p>
        ${k.online === 1 ? '<p class="muted">该卡正在上机，挂失将先按下机结账。</p>' : ''}`,
      actions: [
        { label: '取消' },
        {
          label: '确认挂失', kind: 'danger',
          onClick: async () => {
            try {
              const r = await API.selfLoss();
              ok(r.message);
              paintCard(r.card);
              refreshStudent();
            } catch (ex) { err(ex.message); }
          }
        }
      ]
    });
  };

  /* ---------------- 管理员视图 ---------------- */
  async function refreshAdmin() {
    const inc = await API.income();
    $('#incDay').textContent = inc.day;
    $('#incFee').textContent = money(inc.feeIncome);
    $('#incTimes').textContent = inc.times;

    const c = await API.cards();
    $('#incRemain').textContent = money(c.remainTotal);
    $('#incRecharge').textContent = money(c.rechargeTotal);
    $('#cardSummary').textContent = `${c.total} 张卡 · 上机中 ${c.onlineCount} 人`;

    $('#tblCards tbody').innerHTML = c.cards.map((k) => `<tr>
        <td>${esc(k.id)}</td><td>${esc(k.name)}</td><td>${esc(k.number)}</td>
        <td class="num">${money(k.recharge)}</td>
        <td class="num">${money(k.remain)}</td>
        <td>${esc(k.stateText)}</td>
        <td>${k.online === 1 ? esc(k.address) : '未上机'}</td></tr>`).join('');

    const sel = $('#selCard');
    const keep = sel.value;
    sel.innerHTML = c.cards.map((k) =>
      `<option value="${esc(k.id)}">${esc(k.id)} · ${esc(k.name)} · ${money(k.remain)} 元</option>`).join('');
    if (keep && [...sel.options].some((o) => o.value === keep)) sel.value = keep;
  }

  $$('[data-op]').forEach((btn) => {
    btn.onclick = async () => {
      const op = btn.dataset.op;
      const id = $('#selCard').value;
      const body = { id };
      if (!id) return err('请先选择卡号');
      if (op === 'recharge') {
        const amount = Number($('#opAmount').value);
        if (!(amount > 0)) return err('请输入充值金额');
        body.amount = amount;
      } else if (op === 'reset') {
        if (!confirm(`确认把 ${id} 的密码重置为 123456 并解除冻结？`)) return;
      }
      btn.disabled = true;
      try {
        const r = await API.adminOp(op, body);
        ok(op === 'recharge'
          ? `充值成功，余额 ${money(r.card.remain)} 元`
          : `操作成功，当前状态：${r.card.stateText}`);
        $('#opAmount').value = '';
        refreshAdmin();
      } catch (ex) { err(ex.message); }
      finally { btn.disabled = false; }
    };
  });

  $('#btnViewRec').onclick = async () => {
    const id = $('#selCard').value;
    if (!id) return err('请先选择卡号');
    try {
      const r = await API.admRecords(id);
      $('#admRecTitle').textContent = `${id} 共 ${r.total} 条`;
      $('#tblAdmRecords tbody').innerHTML = r.records.length
        ? r.records.slice().reverse().map((x) => `<tr>
            <td>${esc(x.id)}</td><td>${esc(x.date)}</td><td>${esc(x.startTime)}</td>
            <td class="num">${x.duration}</td><td class="num">${money(x.cost)}</td>
            <td>${esc(x.state)}</td><td>${esc(x.address)}</td></tr>`).join('')
        : emptyRow(7, '该卡暂无上机记录');
    } catch (ex) { err(ex.message); }
  };

  /* ---------------- 启动：校验服务并恢复身份 ---------------- */
  (async () => {
    try { await API.health(); }
    catch (e) {
      $('#loginHint').textContent = e.message;
      $('#loginHint').classList.add('error');
      return;
    }
    if (!API.token) return;
    try {
      const admin = API.token === 'ADMIN';
      if (admin) { state.admin = true; state.card = null; }
      else {
        const c = await API.card();       /* 用本地身份试读一次卡片 */
        state.card = c.card;
      }
      enterApp();
    } catch (e) { API.setAuth('', ''); }
  })();

  /* 页面重新可见时自动刷新，保证多端数据一致 */
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !$('#app').classList.contains('hidden')) refresh();
  });
})();
