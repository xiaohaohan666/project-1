/* ============================================================
 *  ui.js —— 通用界面部件：提示条、弹窗、输入弹窗、格式化
 * ============================================================ */
const UI = (() => {
  const $  = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  let timer = null;
  /* 底部提示条。msg 文本；kind 为空/ok/err */
  function toast(msg, kind = '') {
    const el = $('#toast');
    el.textContent = msg;
    el.className = 'toast ' + kind;
    clearTimeout(timer);
    timer = setTimeout(() => el.classList.add('hidden'), 2600);
  }
  const ok  = (m) => toast(m, 'ok');
  const err = (m) => toast(m, 'err');

  const modal = $('#modal');
  /* 打开弹窗。title 标题；bodyHTML 正文 HTML；actions 按钮数组 */
  function openModal({ title, bodyHTML, actions = [] }) {
    $('#mdTitle').textContent = title;
    $('#mdBody').innerHTML = bodyHTML;
    const foot = $('#mdFoot');
    foot.innerHTML = '';
    actions.forEach((a) => {
      const b = document.createElement('button');
      b.className = 'btn ' + (a.kind || '');
      b.textContent = a.label;
      b.onclick = () => { if (a.onClick) a.onClick(); if (a.close !== false) closeModal(); };
      foot.appendChild(b);
    });
    modal.classList.remove('hidden');
  }
  function closeModal() { modal.classList.add('hidden'); }
  $('#mdClose').onclick = closeModal;
  modal.addEventListener('click', (e) => { if (e.target === modal) closeModal(); });

  /* 输入弹窗：返回 Promise<值对象|null>。fields 为 [{name,label,type,placeholder}] */
  function prompt({ title, fields, confirmLabel = '确定' }) {
    return new Promise((resolve) => {
      const html = fields.map((f) => `
        <div class="field-row">
          <label>${f.label}</label>
          <input class="input" id="pf_${f.name}" placeholder="${f.placeholder || ''}"
                 ${f.type ? `type="${f.type}"` : ''}>
        </div>`).join('');
      openModal({
        title,
        bodyHTML: html,
        actions: [
          { label: '取消', onClick: () => resolve(null) },
          {
            label: confirmLabel, kind: 'primary',
            onClick: () => {
              const vals = {};
              fields.forEach((f) => { vals[f.name] = ($('#pf_' + f.name).value || '').trim(); });
              resolve(vals);
            }
          }
        ]
      });
      const first = fields[0] && $('#pf_' + fields[0].name);
      if (first) setTimeout(() => first.focus(), 30);
    });
  }

  const money = (v) => Number(v || 0).toFixed(2);
  const esc = (s) => String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  /* 空表格占位行。cols 列数；text 提示文字 */
  const emptyRow = (cols, text) =>
    `<tr><td colspan="${cols}" class="empty">${text}</td></tr>`;

  return { $, $$, toast, ok, err, openModal, closeModal, prompt, money, esc, emptyRow };
})();
