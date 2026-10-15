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

  /* ---------------- 服务器地址设置 ---------------- */
  /* 前端可部署在静态托管（如 GitHub Pages），此时接口在另一台服务器上，
   * 需要让用户填写后端地址；若由 C 服务同时托管页面与接口则留空即可 */
  $('#btnSettings').onclick = async () => {
    const v = await prompt({
      title: '设置服务器地址',
      fields: [{
        name: 'apiBase',
        label: '后端地址（留空表示与页面同源）',
        placeholder: '例如 https://xxxx.lhr.life'
      }],
      confirmLabel: '保存并测试'
    });
    if (!v) return;
    API.setApiBase(v.apiBase);
    try {
      const h = await API.health();
      ok(`连接成功：${h.cards} 张卡，机房费率 ${money(h.rate)} 元/小时`);
      $('#loginHint').classList.remove('error');
      $('#loginHint').textContent = HINTS[role];
    } catch (ex) {
      const hint = $('#loginHint');
      hint.textContent = '连接失败：' + ex.message;
      hint.classList.add('error');
    }
  };

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
    /* 挂失按钮随卡片状态切换文案：正常时是「挂失」，挂失后是「解除挂失」 */
    const lost = k.state === 1;
    $('#tileLostText').textContent = lost ? '解除挂失' : '挂失';
    $('#tileLostTip').textContent = lost ? '恢复卡片正常使用' : '上机中会先结账';
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

  /* 交易流水渲染。admin 为真时显示卡号列。
   * 服务端分页返回（每页 300 条），这里只渲染当前页，并提示总数与翻页 */
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

    /* 分页信息：总数超过一页时给出提示 */
    const tip = $(admin ? '#ledgerCount' : '#recCount2');
    if (tip) {
      tip.textContent = r.total > r.count
        ? `共 ${r.total} 条，本页显示最近 ${r.count} 条`
        : `共 ${r.total} 条`;
    }
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

  /* 卡务：修改登录密码。
   * 后端 /api/pwd 需要新密码至少 6 位，这里先用弹窗收两遍确认 */
  $('#tilePwd').onclick = async () => {
    const v = await prompt({
      title: '修改登录密码',
      fields: [
        { name: 'p1', label: '新登录密码（至少 6 位）', type: 'password' },
        { name: 'p2', label: '再输入一次', type: 'password' }
      ],
      confirmLabel: '保存'
    });
    if (!v) return;
    if (v.p1.length < 6) return err('密码不得少于 6 位');
    if (v.p1 !== v.p2) return err('两次输入不一致');
    try {
      await API.changePwd(v.p1);
      API.setAuth(API.token, v.p1);            /* 同步本地凭据，避免下次登录失败 */
      ok('登录密码已修改，请牢记新密码');
    } catch (ex) { err(ex.message); }
  };

  /* 卡务：修改交易密码（消费时用） */
  $('#tilePayPwd').onclick = async () => {
    const v = await prompt({
      title: '修改交易密码',
      fields: [
        { name: 'p1', label: '新交易密码（6 位数字）', type: 'password' },
        { name: 'p2', label: '再输入一次', type: 'password' }
      ],
      confirmLabel: '保存'
    });
    if (!v) return;
    if (!/^\d{6}$/.test(v.p1)) return err('交易密码必须是 6 位数字');
    if (v.p1 !== v.p2) return err('两次输入不一致');
    try {
      await API.changePayPwd(v.p1);
      API.setPayPwd(v.p1);                     /* 同步本地，消费时直接可用 */
      ok('交易密码已修改');
    } catch (ex) { err(ex.message); }
  };

  /* 卡务：自助挂失 / 解挂。
   * 挂失后卡会被冻结不能消费；若正在上机，服务端会先结账 */
  $('#tileLost').onclick = async () => {
    if (state.card && state.card.state === 1) {
      try {
        const r = await API.selfUnlost();
        ok('已解除挂失');
        paintCard(r.card);
        refreshStudent();
      } catch (ex) { err(ex.message); }
      return;
    }
    const v = await prompt({
      title: '挂失校园卡',
      fields: [{ name: 'payPwd', label: '交易密码（确认身份）', type: 'password' }],
      confirmLabel: '确认挂失'
    });
    if (!v) return;
    try {
      /* 先用交易密码通过一次消费接口无法校验，这里直接调用挂失接口；
       * 服务端会核对当前登录身份 */
      API.setPayPwd(v.payPwd);
      const r = await API.selfLoss();
      ok('已挂失，如需恢复请再点一次「挂失」按钮解挂');
      paintCard(r.card);
      refreshStudent();
    } catch (ex) { err(ex.message); }
  };

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
      /* 载入时若因超过上限丢卡，明确告警，避免用户误以为数据丢失 */
      const warn = $('#cardWarn');
      if (warn) {
        if (c.cardTruncated) {
          warn.classList.remove('hidden');
          warn.innerHTML = `⚠ 卡片数已达上限 <b>${c.cardMax}</b> 张，`
            + `数据文件中还有卡片未被载入。数据未丢失，但当前不可见——`
            + `请提高 CARD_MAX 后重启服务。`;
        } else {
          warn.classList.add('hidden');
        }
      }
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
    } else if (action === 'pay') {
      switchView('student-consume');
      $('#inPayAmount')?.focus();
    } else if (action === 'ledger') {
      switchView('student-ledger');
    }
  }

  /* 心跳：告诉本地桌面启动器「应用窗口还开着」。
   * 只在页面可见时发送；窗口关闭后心跳自然停止，启动器据此退出并停止服务。
   * 直接 fetch 而不经 API 封装：心跳失败不应弹出任何错误提示。 */
  function heartbeat() {
    if (document.hidden) return;
    fetch('/api/beat', { cache: 'no-store' }).catch(() => {});
  }
  heartbeat();
  setInterval(heartbeat, 4000);

  /* 页面重新可见时自动刷新，保证多端数据一致 */
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !$('#app').classList.contains('hidden')) refresh();
  });
})();
