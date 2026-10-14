/* ============================================================
 *  app.js —— 一卡通页面状态与业务交互
 *  结构：登录/注册 → 进入应用 → 按角色显示导航 → 切换视图
 *  视图由 nav 上的 data-view 与 section 上的 data-view 对应
 * ============================================================ */
(() => {
  const { $, $$, ok, err, prompt, money, esc, emptyRow, openModal } = UI;
  const state = {
    admin: false, card: null, timer: null, rate: 2,
    view: '', period: { ledger: 'day', consume: 'day', stats: 'day' }
  };

  /* ---------------- 登录 / 注册 ---------------- */
  let role = 'student';
  const HINTS = {
    student: '演示卡号 C001 / C002 / C003，密码 123456',
    admin: '演示管理员密码 admin123',
    register: '注册后交易密码默认取学号后 6 位'
  };
  $$('#loginTabs .tab').forEach((t) => {
    t.onclick = () => {
      $$('#loginTabs .tab').forEach((x) => x.classList.remove('active'));
      t.classList.add('active');
      role = t.dataset.role;
      $('#fId').classList.toggle('hidden', role === 'admin');
      $('#regFields').classList.toggle('hidden', role !== 'register');
      $('#btnLogin').textContent = role === 'register' ? '注 册' : '登 录';
      const h = $('#loginHint');
      h.classList.remove('error');
      h.textContent = HINTS[role];
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
      if (role === 'register') {
        const r = await API.register(id, $('#inName').value.trim(),
                                     $('#inNumber').value.trim(), pwd);
        hint.textContent = `注册成功：${r.card.id} ${r.card.name}，请用该密码登录`;
        $$('#loginTabs .tab')[0].click();          /* 切回学生登录 */
        $('#inId').value = r.card.id;
      } else {
        const r = await API.login(id, pwd, role === 'admin');
        API.setAuth(r.token, pwd);
        state.admin = !!r.admin;
        state.card = r.card;
        enterApp();
      }
    } catch (ex) {
      hint.textContent = ex.message;
      hint.classList.add('error');
    } finally {
      $('#btnLogin').disabled = false;
    }
  };
  $('#btnAdminTip').onclick = () => $$('#loginTabs .tab')[1].click();

  /* ---------------- 视图切换 ---------------- */
  function switchView(name) {
    state.view = name;
    $$('#mainnav .nav-btn').forEach((b) =>
      b.classList.toggle('on', b.dataset.view === name));
    $$('.view').forEach((v) =>
      v.classList.toggle('hidden', v.dataset.view !== name));
    refresh();
  }
  $$('#mainnav .nav-btn').forEach((b) => {
    b.onclick = () => switchView(b.dataset.view);
  });

  /* ---------------- 进入 / 退出 ---------------- */
  function enterApp() {
    $('#login').classList.add('hidden');
    $('#app').classList.remove('hidden');
    $('#installHint')?.classList.add('hidden');
    $('#topRole').textContent = state.admin ? '管理员' : '学生';
    $('#topSub').textContent = state.admin ? '一卡通管理端' : '一卡通学生端';
    $('#viewStudent').classList.toggle('hidden', state.admin);
    $('#viewAdmin').classList.toggle('hidden', !state.admin);
    /* 管理员与学生的默认视图不同 */
    $$('#mainnav .nav-btn').forEach((b) => {
      const isAdminBtn = b.classList.contains('admin-only');
      b.classList.toggle('hidden', state.admin ? !isAdminBtn : isAdminBtn);
    });
    switchView(state.admin ? 'admin-pos' : 'student-consume');
  }
  $('#btnLogout').onclick = () => {
    API.setAuth('', '');
    state.admin = false;
    state.card = null;
    stopTimer();
    $('#app').classList.add('hidden');
    $('#login').classList.remove('hidden');
    $('#inPwd').value = '';
    if (window.PWA && window.PWA.canInstall) $('#installHint')?.classList.remove('hidden');
  };
  $('#btnRefresh').onclick = () => refresh(true);

  /* ---------------- 统一刷新 ---------------- */
  async function refresh(manual) {
    try {
      /* 卡面在学生端总是刷新；管理端用下拉框替代 */
      if (!state.admin) await refreshStudent();
      else              await refreshAdminView();
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

  /* ---------------- 学生端 ---------------- */
  function paintCard(k) {
    state.card = k;
    $('#cvId').textContent = k.id;
    $('#cvName').textContent = k.name;
    $('#cvNumber').textContent = k.number;
    $('#cvRemain').textContent = money(k.remain);
    $('#cvRecharge').textContent = money(k.recharge);
    $('#cvStatus').textContent = k.stateText;
    /* 非正常状态禁用消费与上机 */
    const blocked = k.state !== 0;
    $('#machineIdle').classList.toggle('hidden', k.online === 1);
    $('#machineBusy').classList.toggle('hidden', k.online !== 1);
    $('#btnOnline').disabled = blocked;
    $('#btnOnline').textContent = blocked ? `当前状态：${k.stateText}` : '上机登记';
    $('#btnPay').disabled = blocked;
    $('#payTip').textContent = `单笔限额 ${k.limitOnce > 0 ? money(k.limitOnce) + ' 元' : '不限'}` +
      ` · 日累计 ${k.limitDay > 0 ? money(k.limitDay) + ' 元' : '不限'}` +
      ` · 今日已消费 ${money(k.usedDay)} 元`;
    if (k.online === 1) startTimer(k.startTime); else stopTimer();
  }

  async function refreshStudent() {
    const c = await API.card();
    state.rate = c.rate;
    paintCard(c.card);
    $('#rateTip').textContent = money(c.rate) + ' 元 / 小时';

    if (state.view === 'student-consume') {
      await loadMerchants();
    } else if (state.view === 'student-ledger') {
      await paintLedger($('#tblLedger tbody'), state.period.ledger, false);
    } else if (state.view === 'student-stats') {
      await paintConsumeStats();
      await paintMachineStats();
    } else if (state.view === 'student-access') {
      await paintAccess();
    }
  }

  /* 消费点下拉（学生端与管理端共用一次请求） */
  let merchantList = null;
  async function loadMerchants() {
    if (!merchantList) merchantList = (await API.merchants()).merchants;
    const fill = (sel, exclude) => {
      if (!sel || sel.options.length) return;
      sel.innerHTML = merchantList
        .filter((m) => !exclude || m !== exclude)
        .map((m) => `<option>${esc(m)}</option>`).join('');
    };
    fill($('#selMerchant'), '机房');            /* 机房走独立的上机面板 */
    fill($('#selPosMerchant'), null);
  }

  /* 交易流水渲染。admin 为真时显示卡号列 */
  async function paintLedger(tbody, period, admin) {
    if (!tbody) return;
    const r = admin ? await API.admLedger(period) : await API.ledger(period);
    const rows = (r.records || []).slice().reverse();
    tbody.innerHTML = rows.length
      ? rows.map((t) => `<tr>
          <td>${esc(t.time)}</td>
          ${admin ? `<td>${esc(t.id)}</td>` : ''}
          <td>${esc(t.typeText)}</td><td>${esc(t.merchant)}</td>
          <td class="num">${money(t.amount)}</td>
          <td class="num">${money(t.balance)}</td></tr>`).join('')
      : emptyRow(admin ? 6 : 5, '该时段暂无交易');
    return r.total;
  }

  async function paintConsumeStats() {
    const st = await API.consume(state.period.consume);
    $('#csCount').textContent = st.count;
    $('#csAmount').textContent = money(st.amount);
    const k = state.card || {};
    $('#csToday').textContent = money(k.usedDay);
    $('#csLimit').textContent = k.limitDay > 0 ? money(k.limitDay) : '不限';
  }

  async function paintMachineStats() {
    const st = await API.stats(state.period.stats);
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

  async function paintAccess() {
    const r = await API.accessLog();
    const rows = (r.records || []).slice().reverse();
    $('#tblAccess tbody').innerHTML = rows.length
      ? rows.map((a) => `<tr>
          <td>${esc(a.time)}</td><td>${esc(a.dirText)}</td>
          <td>${esc(a.kindText)}</td><td>${esc(a.place)}</td></tr>`).join('')
      : emptyRow(4, '暂无刷卡记录');
  }

  /* ---------------- 上机计时 ---------------- */
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
      const where = state.card ? state.card.address : '';
      $('#mWhere').innerHTML =
        `${esc(where)} · 预估 <b id="mCost">${money(est)}</b> 元`;
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

  /* ---------------- 时间段切换 ---------------- */
  const bindSeg = (sel, key) => {
    $$(sel + ' button').forEach((b) => {
      b.onclick = () => {
        $$(sel + ' button').forEach((x) => x.classList.remove('on'));
        b.classList.add('on');
        state.period[key] = b.dataset.p;
        refresh();
      };
    });
  };
  bindSeg('#segPeriod', 'stats');
  bindSeg('#segLedger', 'ledger');
  bindSeg('#segConsume', 'consume');

  /* ---------------- 学生操作 ---------------- */
  $('#btnPay').onclick = async () => {
    const merchant = $('#selMerchant').value;
    const amount = Number($('#inPayAmount').value);
    if (!(amount > 0)) return err('请输入消费金额');
    const v = await prompt({
      title: `消费确认 · ${merchant}`,
      fields: [{ name: 'payPwd', label: `交易密码（消费 ${money(amount)} 元）`,
                 type: 'password', placeholder: '学号后 6 位' }],
      confirmLabel: '确认扣款'
    });
    if (!v) return;
    try {
      const r = await API.pay(merchant, amount, v.payPwd);
      API.setPayPwd(v.payPwd);                   /* 记住，下次免输 */
      ok(`消费成功，余额 ${money(r.card.remain)} 元`);
      $('#inPayAmount').value = '';
      paintCard(r.card);
      refreshStudent();
    } catch (ex) { err(ex.message); }
  };

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

  $$('[data-access-dir]').forEach((btn) => {
    btn.onclick = async () => {
      const place = $('#selAccessPlace').value;
      const kind = Number($('#selAccessKind').value);
      const dir = Number(btn.dataset.accessDir);
      try {
        const r = await API.access(place, dir, kind);
        ok(r.message);
        paintAccess();
      } catch (ex) { err(ex.message); }
    };
  });

  /* ---------------- 管理员：商户终端 ---------------- */
  /* 模拟 POS：管理员选持卡人 + 消费点 + 金额，由持卡人输入交易密码确认 */
  $('#btnPosPay').onclick = async () => {
    const id = $('#selPosCard').value;
    const merchant = $('#selPosMerchant').value;
    const amount = Number($('#inPosAmount').value);
    const payPwd = $('#inPosPayPwd').value;
    if (!id) return err('请选择持卡人');
    if (!(amount > 0)) return err('请输入消费金额');
    if (!payPwd) return err('请输入交易密码');

    const btn = $('#btnPosPay');
    btn.disabled = true;
    try {
      const r = await API.adminOp('pay', { id, merchant, amount, payPwd });
      ok(`扣款成功，余额 ${money(r.card.remain)} 元`);
      $('#inPosAmount').value = '';
      $('#inPosPayPwd').value = '';
      refreshAdminView();
    } catch (ex) { err(ex.message); }
    finally { btn.disabled = false; }
  };

  /* ---------------- 管理员：视图刷新 ---------------- */
  async function refreshAdminView() {
    await loadMerchants();
    const c = await API.cards();
    /* 卡片下拉（卡务中心 + 商户终端） */
    const fillCards = (sel, keep) => {
      if (!sel) return;
      sel.innerHTML = c.cards.map((k) =>
        `<option value="${esc(k.id)}">${esc(k.id)} · ${esc(k.name)} · ${money(k.remain)} 元</option>`).join('');
      if (keep && [...sel.options].some((o) => o.value === keep)) sel.value = keep;
    };
    fillCards($('#selCard'), $('#selCard').value);
    fillCards($('#selPosCard'), $('#selPosCard').value);

    if (state.view === 'admin-card') {
      $('#cardSummary').textContent = `${c.total} 张卡 · 上机中 ${c.onlineCount} 人`;
      $('#tblCards tbody').innerHTML = c.cards.map((k) => `<tr>
          <td>${esc(k.id)}</td><td>${esc(k.name)}</td><td>${esc(k.number)}</td>
          <td class="num">${money(k.remain)}</td>
          <td class="num">${money(k.usedDay)}</td>
          <td>${esc(k.stateText)}</td>
          <td>${esc(k.expire || '长期')}</td></tr>`).join('');
    } else if (state.view === 'admin-report') {
      const r = await API.report();
      $('#incDay').textContent = r.day;
      $('#incConsume').textContent = money(r.consumeTotal);
      $('#incTimes').textContent = r.consumeTimes;
      $('#incRecharge').textContent = money(r.rechargeTotal);
      $('#incFee').textContent = money(r.feeIncome);
      $('#tblMerchantSum tbody').innerHTML = (r.merchants || []).map((m) => `<tr>
          <td>${esc(m.name)}</td><td class="num">${money(m.amount)}</td></tr>`).join('');

      const L = await API.admLedger('day');
      $('#ledgerCount').textContent = `共 ${L.total} 条`;
      $('#tblAdmLedgerAll tbody').innerHTML = (L.records || []).length
        ? L.records.slice().reverse().map((t) => `<tr>
            <td>${esc(t.time)}</td><td>${esc(t.id)}</td><td>${esc(t.typeText)}</td>
            <td>${esc(t.merchant)}</td>
            <td class="num">${money(t.amount)}</td>
            <td class="num">${money(t.balance)}</td></tr>`).join('')
        : emptyRow(6, '当天暂无交易');

      const A = await API.admAccess('');
      $('#accessCount').textContent = `共 ${A.total} 条`;
      $('#tblAccessAll tbody').innerHTML = (A.records || []).length
        ? A.records.slice().reverse().map((a) => `<tr>
            <td>${esc(a.id)}</td><td>${esc(a.time)}</td><td>${esc(a.dirText)}</td>
            <td>${esc(a.kindText)}</td><td>${esc(a.place)}</td></tr>`).join('')
        : emptyRow(5, '暂无门禁记录');
    }
  }

  /* ---------------- 管理员：卡务操作 ---------------- */
  $$('[data-op]').forEach((btn) => {
    btn.onclick = async () => {
      const op = btn.dataset.op;
      const id = $('#selCard').value;
      const amount = Number($('#opAmount').value);
      if (!id) return err('请先选择卡号');
      const body = { id };
      const needAmount = ['recharge', 'reissue', 'grant'];
      if (needAmount.includes(op)) {
        if (!(amount > 0)) return err('请在「金额」框填写金额');
        if (op === 'recharge') body.amount = amount;
        if (op === 'reissue')  body.fee = amount;
        if (op === 'grant')  { body.amount = amount; body.type = 4; body.memo = '补助发放'; }
      }
      if (op === 'limits') {
        const v = await prompt({
          title: '设置消费限额',
          fields: [
            { name: 'limitOnce', label: '单笔限额（元，0 表示不限）', placeholder: '20' },
            { name: 'limitDay',  label: '日累计限额（元，0 表示不限）', placeholder: '50' }
          ],
          confirmLabel: '保存'
        });
        if (!v) return;
        body.limitOnce = Number(v.limitOnce) || 0;
        body.limitDay  = Number(v.limitDay) || 0;
      }
      if (op === 'expire') {
        const v = await prompt({
          title: '卡片延期',
          fields: [{ name: 'date', label: '新的有效期（YYYY-MM-DD）', placeholder: '2030-06-30' }],
          confirmLabel: '保存'
        });
        if (!v) return;
        body.date = v.date;
      }
      if (op === 'unfreeze') { body.on = 0; }
      if (op === 'freeze')   { body.on = 1; }
      /* 服务端路径名与按钮 data-op 的映射 */
      const PATH = { unfreeze: 'freeze', reissue: 'reissue' };
      const apiOp = PATH[op] || op;

      btn.disabled = true;
      try {
        const r = await API.adminOp(apiOp, body);
        ok(`操作成功，余额 ${money(r.card.remain)} 元，状态 ${r.card.stateText}`);
        $('#opAmount').value = '';
        refreshAdminView();
      } catch (ex) { err(ex.message); }
      finally { btn.disabled = false; }
    };
  });

  $('#btnViewRec').onclick = async () => {
    const id = $('#selCard').value;
    if (!id) return err('请先选择卡号');
    try {
      const r = await API.admRecords(id);
      $('#tblAdmRecords tbody').innerHTML = (r.records || []).length
        ? r.records.slice().reverse().map((x) => `<tr>
            <td>${esc(x.id)}</td><td>${esc(x.date)}</td><td>${esc(x.startTime)}</td>
            <td class="num">${x.duration}</td><td class="num">${money(x.cost)}</td>
            <td>${esc(x.state)}</td><td>${esc(x.address)}</td></tr>`).join('')
        : emptyRow(7, '该卡暂无上机记录');
    } catch (ex) { err(ex.message); }
  };

  $('#btnViewLedger').onclick = async () => {
    const id = $('#selCard').value;
    if (!id) return err('请先选择卡号');
    try {
      const r = await API.admLedger('day');
      const rows = (r.records || []).filter((t) => t.id === id).reverse();
      $('#tblAdmLedger tbody').innerHTML = rows.length
        ? rows.map((t) => `<tr>
            <td>${esc(t.time)}</td><td>${esc(t.id)}</td><td>${esc(t.typeText)}</td>
            <td>${esc(t.merchant)}</td>
            <td class="num">${money(t.amount)}</td>
            <td class="num">${money(t.balance)}</td></tr>`).join('')
        : emptyRow(6, '该卡当天暂无交易');
    } catch (ex) { err(ex.message); }
  };

  /* ---------------- 启动：探活并恢复身份 ---------------- */
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
        const c = await API.card();
        state.card = c.card;
      }
      enterApp();
      handleShortcut();
    } catch (e) { API.setAuth('', ''); }
  })();

  /* PWA 快捷方式深链：/?action=online 或 /?action=stats */
  function handleShortcut() {
    const action = new URLSearchParams(location.search).get('action');
    if (!action || state.admin) return;
    history.replaceState(null, '', location.pathname);
    if (action === 'online') {
      if (state.card && state.card.online === 1) { ok('当前已在上机中'); return; }
      switchView('student-consume');
      $('#btnOnline')?.scrollIntoView({ behavior: 'smooth', block: 'center' });
    } else if (action === 'stats') {
      switchView('student-stats');
    }
  }

  /* 页面重新可见时自动刷新，保证多端数据一致 */
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !$('#app').classList.contains('hidden')) refresh();
  });
})();
