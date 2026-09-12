/* 乱杀跑团前端 */

const LS_ROOM = 'luansha_room_id';
const LS_TOKEN = 'luansha_token';

const state = {
  room_id: localStorage.getItem(LS_ROOM) || '',
  token: localStorage.getItem(LS_TOKEN) || '',
  current: null,
  pollTimer: null
};

/* Keep the user's in-progress input across automatic re-renders. */
const pendingControls = {
  content: '',
  operation: 'create',
  keyword: '',
  target: '0',
  killTarget: '0'
};

let lastStateKey = '';
let speaking = false;

function $(id) {
  return document.getElementById(id);
}

function escaped(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function toast(msg) {
  const el = $('toast');
  el.textContent = msg;
  el.classList.remove('hidden');
  clearTimeout(el._timer);
  el._timer = setTimeout(() => el.classList.add('hidden'), 2500);
}

function stateKey(room) {
  if (!room) return '';
  const lastNarration = room.narratives.length
    ? room.narratives[room.narratives.length - 1].id
    : 0;
  return [
    room.room_id,
    room.status,
    room.round,
    room.turn_index,
    room.turn_player_id,
    room.players.map(p => `${p.id}:${p.alive}:${p.ready}:${p.hp}:${p.calamity_tier}:${p.calamity}`).join('|'),
    room.narratives.length,
    lastNarration,
    room.warnings.filter(w => !w.resolved).length,
    (room.limit && room.limit.remaining) || 0,
    (room.limit && room.limit.keyword) || '',
    room.winner_id || 0
  ].join('|');
}

async function api(path, body) {
  const opts = body
    ? {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      }
    : {};
  const res = await fetch(path, opts);
  if (!res.ok) {
    throw new Error('HTTP ' + res.status);
  }
  return res.json();
}

function setSession(roomId, token) {
  state.room_id = String(roomId);
  state.token = token;
  localStorage.setItem(LS_ROOM, state.room_id);
  localStorage.setItem(LS_TOKEN, state.token);
  startPolling();
  render();
}

function leaveRoom() {
  localStorage.removeItem(LS_ROOM);
  localStorage.removeItem(LS_TOKEN);
  state.room_id = '';
  state.token = '';
  state.current = null;
  lastStateKey = '';
  if (state.pollTimer) {
    clearInterval(state.pollTimer);
    state.pollTimer = null;
  }
  $('game-view').classList.add('hidden');
  $('lobby-view').classList.remove('hidden');
  $('player-list').innerHTML = '';
  $('room-header').innerHTML = '';
  toast('已退出房间');
}
function quitGame() {
  if (!confirm('确定退出游戏吗？')) return;
  localStorage.removeItem(LS_ROOM);
  localStorage.removeItem(LS_TOKEN);
  state.room_id = '';
  state.token = '';
  state.current = null;
  lastStateKey = '';
  if (state.pollTimer) {
    clearInterval(state.pollTimer);
    state.pollTimer = null;
  }
  if (window.pywebview && window.pywebview.api && window.pywebview.api.quit_app) {
    window.pywebview.api.quit_app();
    return;
  }
  try { window.close(); } catch (e) {}
  location.reload();
}

async function startPublicTunnel() {
  const el = $('public-tunnel-result');
  if (!el) return;
  if (!window.pywebview || !window.pywebview.api || !window.pywebview.api.start_tunnel) {
    el.textContent = '请使用桌面客户端才能开启公网联机';
    return;
  }
  el.textContent = '正在开启隧道…';
  try {
    const r = await window.pywebview.api.start_tunnel();
    if (r && r.ok) {
      el.innerHTML = '公网地址：<a href="' + r.url + '" target="_blank">' + r.url + '</a>';
    } else {
      el.textContent = (r && r.error) || '开启失败';
    }
  } catch (e) {
    el.textContent = '开启失败：' + e.message;
  }
}

/* ---------------- 历史战绩 / 回放 ---------------- */

let recordsVisible = false;
let recordsCache = [];

async function toggleRecords() {
  const panel = $('records-view');
  const status = $('records-status');
  if (!panel) return;
  if (recordsVisible) {
    panel.classList.add('hidden');
    recordsVisible = false;
    return;
  }
  recordsVisible = true;
  panel.classList.remove('hidden');
  if (status) status.textContent = '加载中…';
  try {
    const data = await api('/api/records');
    recordsCache = data.records || [];
    if (status) status.textContent = '';
    renderRecordsList(recordsCache);
  } catch (e) {
    if (status) status.textContent = '加载失败：' + e.message;
  }
}

function renderRecordsList(records) {
  const el = $('records-list');
  if (!el) return;
  if (!records.length) {
    el.innerHTML = '<p class="hint">暂无历史对局。</p>';
    return;
  }
  const diffNames = ['休闲', '普通', '困难', '噩梦', '地狱'];
  el.innerHTML = records.map(r => `
    <div class="record-card">
      <div><strong>${escaped(r.name)}</strong> · ${diffNames[r.difficulty] || r.difficulty}</div>
      <div class="hint">房间 ${r.room_id} · ${escaped(r.created_at || '')} · 玩家 ${r.player_count} · 叙事 ${r.narrative_count}</div>
      <div>胜者：${escaped(r.winner_name || ('玩家' + r.winner_id))}</div>
      <button onclick="showRecordDetail(${r.record_id})">查看回放</button>
    </div>
  `).join('');
}

async function showRecordDetail(recordId) {
  const el = $('records-list');
  if (!el) return;
  el.innerHTML = '<p class="hint">加载回放…</p>';
  try {
    const res = await fetch('/api/records?id=' + encodeURIComponent(recordId));
    const data = await res.json();
    renderRecordDetail(data);
  } catch (e) {
    el.innerHTML = '<p class="hint">加载失败：' + escaped(e.message) + '</p>';
  }
}

function renderRecordDetail(record) {
  const el = $('records-list');
  if (!el) return;
  if (!record || !record.narratives) {
    el.innerHTML = '<p class="hint">回放不存在。</p>';
    return;
  }
  const diffNames = ['休闲', '普通', '困难', '噩梦', '地狱'];
  const players = (record.players || []).map(p => `
    <span class="${p.alive ? '' : 'record-dead'}">${escaped(p.name)}${p.is_ai ? '(AI)' : ''}${p.alive ? '' : ' ☠'}</span>
  `).join(' ');
  const nars = (record.narratives || []).map(n => `
    <div class="record-narrative ${n.calamity ? 'calamity' : ''} ${n.notice ? 'notice' : ''} ${n.green ? 'green' : ''}">
      <span class="record-round">R${n.round}</span>
      <strong>${escaped(n.player_name)}</strong>
      <span class="record-op">[${escaped(n.operation)}]</span>
      ${escaped(n.content)}
    </div>
  `).join('');
  el.innerHTML = `
    <div class="record-detail">
      <div><button onclick="renderRecordsList(recordsCache)">← 返回列表</button></div>
      <h3>${escaped(record.name)}</h3>
      <div class="hint">难度 ${diffNames[record.difficulty] || record.difficulty} · 胜者 ${escaped(record.winner_name || '')} · ${escaped(record.created_at || '')}</div>
      <div class="record-players">${players}</div>
      <div class="record-narratives">${nars}</div>
    </div>
  `;
}


/* ---------------- 大厅操作 ---------------- */

async function createRoom() {
  const name = $('player-name').value.trim();
  if (!name) { toast('请输入昵称'); return; }
  try {
    const data = await api('/api/room/create', {
      name: $('room-name').value.trim(),
      mode: $('room-mode').value,
      difficulty: parseInt($('room-difficulty').value || '0', 10),
      player_name: name
    });
    if (!data.ok) throw new Error(data.error || '创建失败');
    setSession(data.room_id, data.token);
  } catch (e) {
    toast(e.message || '创建失败');
  }
}

async function joinRoom() {
  const roomId = parseInt($('join-room-id').value, 10);
  const name = $('player-name').value.trim();
  if (!roomId || !name) { toast('请输入房间ID和昵称'); return; }
  try {
    const data = await api('/api/room/join', {
      room_id: roomId,
      player_name: name
    });
    if (!data.ok) throw new Error(data.error || '加入失败');
    setSession(data.room_id, data.token);
  } catch (e) {
    toast(e.message || '加入失败');
  }
}

async function openAIConfig() {
  const modal = $('ai-config-modal');
  if (!modal) return;
  const resultEl = $('ai-config-result');
  if (resultEl) resultEl.textContent = '';
  try {
    const data = await api('/api/ai/config');
    if (data && data.ok) {
      $('ai-url').value = data.url || '';
      $('ai-model').value = data.model || '';
      $('ai-key').value = '';
      $('ai-key').placeholder = data.key_set
        ? '已保存（留空表示不修改）'
        : 'sk-xxx（本地 Ollama 可留空）';
    }
  } catch (e) {
    $('ai-url').value = '';
    $('ai-model').value = '';
    $('ai-key').value = '';
    $('ai-key').placeholder = 'sk-xxx（本地 Ollama 可留空）';
  }
  modal.classList.remove('hidden');
}

function closeAIConfig() {
  const modal = $('ai-config-modal');
  if (modal) modal.classList.add('hidden');
}

async function saveAIConfig() {
  const resultEl = $('ai-config-result');
  if (resultEl) resultEl.textContent = '保存中…';
  try {
    const data = await api('/api/ai/config', {
      url: $('ai-url').value.trim(),
      model: $('ai-model').value.trim(),
      key: $('ai-key').value.trim()
    });
    if (!data || !data.ok) throw new Error((data && data.error) || '保存失败');
    if (resultEl) resultEl.textContent = '✅ 已保存，正在测试…';
    await testAIConnection();
  } catch (e) {
    if (resultEl) resultEl.textContent = '❌ ' + e.message;
    toast('AI 配置保存失败');
  }
}

async function testAIConnection() {
  const lobbyEl = $('ai-test-result');
  const modalEl = $('ai-config-result');
  if (lobbyEl) lobbyEl.textContent = '测试中…';
  try {
    const data = await api('/api/ai/test');
    if (data && data.ok) {
      const msg = '✅ ' + (data.reply || 'AI连接正常');
      if (lobbyEl) lobbyEl.textContent = msg;
      if (modalEl) modalEl.textContent = msg;
      toast('AI 接口正常');
    } else {
      const msg = (data && data.error) || 'AI连接失败';
      if (lobbyEl) lobbyEl.textContent = '❌ ' + msg;
      if (modalEl) modalEl.textContent = '❌ ' + msg;
      toast('AI 接口连接失败');
    }
  } catch (e) {
    if (lobbyEl) lobbyEl.textContent = '❌ ' + e.message;
    if (modalEl) modalEl.textContent = '❌ ' + e.message;
    toast('AI 接口连接失败');
  }
}

async function addAI() {
  try {
    const data = await api('/api/room/add_ai', {
      room_id: Number(state.room_id),
      token: state.token
    });
    if (!data.ok) throw new Error(data.error || '添加AI失败');
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || '添加AI失败');
  }
}

async function setReady(ready) {
  try {
    const data = await api('/api/game/set_ready', {
      room_id: Number(state.room_id),
      token: state.token,
      ready: ready ? 1 : 0
    });
    if (!data.ok) throw new Error(data.error || '设置失败');
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || '设置失败');
  }
}

async function startGame(fillAI) {
  try {
    /* 开始前如果自己还没准备，自动先准备，避免误点“开始”后被“未准备”拦截。 */
    const cur = state.current;
    const me = cur && cur.players.find(p => p.is_me);
    if (me && !me.ready) {
      const rdy = await api('/api/game/set_ready', {
        room_id: Number(state.room_id),
        token: state.token,
        ready: 1
      });
      if (!rdy.ok) throw new Error(rdy.error || '准备失败');
    }
    const data = await api('/api/game/start', {
      room_id: Number(state.room_id),
      token: state.token,
      fill_ai: fillAI ? 1 : 0
    });
    if (!data.ok) throw new Error(data.error || '开始失败');
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || '开始失败');
  }
}

/* ---------------- 游戏操作 ---------------- */

async function speak() {
  const btn = $('speak-btn');
  if (speaking) return;
  const contentEl = $('speak-content');
  const content = contentEl ? contentEl.value.trim() : '';
  if (!content) { toast('请输入发言内容'); return; }
  speaking = true;
  if (btn) { btn.disabled = true; btn.textContent = '审核中...'; }
  const opEl = $('speak-operation');
  const keywordEl = $('speak-keyword');
  const targetEl = $('speak-target');
  if (contentEl) contentEl.disabled = true;
  if (opEl) opEl.disabled = true;
  if (keywordEl) keywordEl.disabled = true;
  if (targetEl) targetEl.disabled = true;
  const op = $('speak-operation').value;
  const keyword = $('speak-keyword').value.trim();
  const targetId = parseInt($('speak-target').value || '0', 10);

  try {
    const data = await api('/api/game/speak', {
      room_id: Number(state.room_id),
      token: state.token,
      operation: op,
      content: content,
      limit_keyword: keyword,
      target_id: targetId || 0
    });
    if (!data.ok) throw new Error(data.error || '发言未通过');
    $('speak-content').value = '';
    pendingControls.content = '';
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || '发言未通过规则校验');
  } finally {
    speaking = false;
    if (btn) { btn.disabled = false; btn.textContent = '发言'; }
    if (contentEl) contentEl.disabled = false;
    if (opEl) opEl.disabled = false;
    if (keywordEl) keywordEl.disabled = false;
    if (targetEl) targetEl.disabled = false;
  }
}

async function declareDeath(targetId) {
  try {
    const room = state.current;
    const me = room && room.players ? room.players.find(p => p.is_me) : null;
    if (me && targetId === me.id) {
      if (!confirm('确定要自认死亡吗？')) return;
    }
    const data = await api('/api/game/declare_death', {
      room_id: Number(state.room_id),
      token: state.token,
      target_id: targetId
    });
    if (!data.ok) throw new Error(data.error || '宣告失败');
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || '宣告失败');
  }
}

async function lethalAttack() {
  const op = $('speak-operation');
  const content = $('speak-content');
  const targetSel = $('speak-target');
  if (!op || !content || !targetSel) return;
  const targetId = parseInt(targetSel.value, 10);
  const room = state.current;
  if (!room || !targetId) { toast('请先选择攻击目标'); return; }
  const me = room.players.find(p => p.is_me);
  if (me && targetId === me.id) { toast('不能选择自己'); return; }
  const target = room.players.find(p => p.id === targetId);
  if (!target) { toast('目标不存在'); return; }
  try {
    const data = await api('/api/ai/lethal', {
      room_id: Number(state.room_id),
      token: state.token,
      target_id: targetId
    });
    if (!data || !data.ok) throw new Error((data && data.error) || '生成失败');
    op.value = 'twist';
    content.value = data.reply || ('我给了' + target.name + '致命一击');
    await speak();
  } catch (e) {
    toast(e.message || '致命攻击生成失败');
  }
}
async function gmCommand(action) {
  const targetId = parseInt($('gm-target').value || '0', 10);
  if (!targetId) { toast('请先选择GM操作目标'); return; }
  try {
    const data = await api('/api/game/gm', {
      room_id: Number(state.room_id),
      token: state.token,
      action: action,
      target_id: targetId
    });
    if (!data.ok) throw new Error(data.error || 'GM指令失败');
    renderFromResponse(data);
  } catch (e) {
    toast(e.message || 'GM指令失败');
  }
}

/* ---------------- 渲染 ---------------- */

function renderFromResponse(data) {
  if (data && data.state) {
    render(data.state);
  } else {
    poll();
  }
}

function captureControls() {
  const content = $('speak-content');
  const op = $('speak-operation');
  const keyword = $('speak-keyword');
  const target = $('speak-target');
  const killTarget = $('kill-target');
  if (content) pendingControls.content = content.value;
  if (op) pendingControls.operation = op.value;
  if (keyword) pendingControls.keyword = keyword.value;
  if (target) pendingControls.target = target.value;
  if (killTarget) pendingControls.killTarget = killTarget.value;
}

function applyControls() {
  const content = $('speak-content');
  const op = $('speak-operation');
  const keyword = $('speak-keyword');
  const target = $('speak-target');
  const killTarget = $('kill-target');
  if (content) content.value = pendingControls.content;
  if (op) op.value = pendingControls.operation;
  if (keyword) keyword.value = pendingControls.keyword;
  if (target) target.value = pendingControls.target;
  if (killTarget) killTarget.value = pendingControls.killTarget;
}

function bindControlListeners() {
  const content = $('speak-content');
  const op = $('speak-operation');
  const keyword = $('speak-keyword');
  const target = $('speak-target');
  const killTarget = $('kill-target');
  if (content) {
    content.oninput = () => { pendingControls.content = content.value; };
    content.onkeydown = (e) => {
      /* 按回车直接发言；Shift+回车或输入法选词回车时不触发。 */
      if (e.key !== 'Enter') return;
      if (e.shiftKey || e.isComposing || e.keyCode === 229) return;
      e.preventDefault();
      speak();
    };
  }
  if (op) op.onchange = () => { pendingControls.operation = op.value; };
  if (keyword) keyword.oninput = () => { pendingControls.keyword = keyword.value; };
  if (target) target.onchange = () => { pendingControls.target = target.value; };
  if (killTarget) killTarget.onchange = () => { pendingControls.killTarget = killTarget.value; };
}

const statusNames = ['等待中', '游戏中', '已结束'];

function render(room) {
  if (!room) return;
  state.current = room;
  if (room.room_id) {
    state.room_id = String(room.room_id);
    localStorage.setItem(LS_ROOM, state.room_id);
  }
  $('lobby-view').classList.add('hidden');
  $('game-view').classList.remove('hidden');
  renderHeader(room);
  renderPlayers(room);
  renderLimit(room);
  renderNarratives(room);
  renderWarnings(room);
  renderActions(room);
  lastStateKey = stateKey(room);
}

function renderHeader(room) {
  const me = room.players.find(p => p.is_me);
  const ownerText = me && room.owner_id === me.id ? '你是房主' : '';
  $('room-header').innerHTML = `
    <div class="room-title">
      <div>
        <h2>${escaped(room.name)}</h2>
        <span>房间ID：${room.room_id}</span>
        <span class="status-badge">${statusNames[room.status] || room.status}</span>
        <span class="status-badge">${room.mode === 1 ? 'GM模式' : '自动判定'}</span>
        <span class="status-badge">${['休闲','普通','困难','噩梦','地狱'][room.difficulty] || '普通'}</span>
        <span class="status-badge">第 ${room.round || 0} 轮</span>
        ${ownerText ? `<span class="status-badge">${escaped(ownerText)}</span>` : ''}
      </div>
      <div class="status-badge">当前：${playerName(room, room.turn_player_id)}</div>
<button onclick="leaveRoom()" title="清除本机保存的房间信息">退出房间</button>
    </div>`;
}

function playerName(room, id) {
  const p = room.players.find(x => x.id === id);
  return p ? p.name + (p.is_ai ? ' (AI)' : '') : '—';
}

function playerState(p) {
  if (!p.alive) return '死亡';
  if (p.hp === 1) return '濒死';
  if (p.hp <= Math.floor((p.max_hp || 6) / 2)) return '受伤';
  return '健康';
}

/* 灾祸状态：对外只显示档位，自己的确切数值才可见。
   数值可逆——言之有物的叙述会把它压下去。 */
function calamityInfo(p) {
  if (!p.alive) return '';
  const tier = p.calamity_tier || 0;
  const mine = p.is_me && p.calamity >= 0;
  if (tier <= 0 && !mine) return '';
  const label = tier >= 2 ? '危险' : (tier >= 1 ? '躁动' : '平静');
  const cls = tier >= 2 ? 'cal-danger' : (tier >= 1 ? 'cal-uneasy' : 'cal-calm');
  const exact = mine ? ` ${p.calamity}` : '';
  return `<span class="calamity-tag ${cls}">灾祸·${label}${exact}</span>`;
}

function renderPlayers(room) {
  const list = room.players.map(p => {
    const classes = ['player-card'];
    if (!p.alive) classes.push('dead');
    if (room.turn_player_id === p.id && room.status === 1) classes.push('current');
    const state = playerState(p);
    return `<div class="${classes.join(' ')}">
      ${escaped(p.name)}${p.is_ai ? '<span class="ai-tag">AI</span>' : ''}
      ${p.is_me ? '（我）' : ''}
      ${!p.alive ? '💀' : ''}
      <span class="player-state state-${state}">${state}</span>
      ${calamityInfo(p)}
    </div>`;
  }).join('');
  $('player-list').innerHTML = `<div>${list}</div>`;

  if (room.status === 0) {
    $('player-list').innerHTML += `
      <div class="toolbar">
        <button onclick="addAI()">添加 AI</button>
      </div>`;
  }
}

function renderLimit(room) {
  const l = room.limit || {};
  if (l.remaining > 0) {
    $('limit-banner').innerHTML = `
      <div class="limit-banner">
        ⚠️ 当前限定：<b>${escaped(l.keyword)}</b>，还需 <b>${l.remaining}</b> 句接受限定。
        下一句如果可以合理摆脱，即可解除。
      </div>`;
  } else {
    $('limit-banner').innerHTML = '';
  }
}

function renderWarnings(room) {
  const warns = room.warnings.filter(w => !w.resolved);
  if (!warns.length) {
    $('warning-list').innerHTML = '';
    return;
  }
  $('warning-list').innerHTML = `<div class="warning-box">
    <b>💀 濒死警告</b>
    <ul>
      ${warns.map(w => `<li>${escaped(playerName(room, w.victim_id))} 受到 ${escaped(playerName(room, w.source_id))} 的致命威胁：${escaped(w.reason)}</li>`).join('')}
    </ul>
  </div>`;
}

function renderNarratives(room) {
  if (!room.narratives.length) {
    $('narrative-list').innerHTML = '<div class="hint">还没有发言，等待第一个玩家开始叙事……</div>';
    return;
  }
  $('narrative-list').innerHTML = room.narratives.map(n => `
    <div class="narrative-item${n.calamity ? ' calamity' : ''}${n.notice ? ' notice' : ''}${n.green ? ' notice-green' : ''}${n.roll_used ? (n.roll_success ? ' roll-success' : ' roll-fail') : ''}">
      <div class="meta">第${n.round}轮 · ${escaped(n.player_name)} · ${escaped(n.operation)}</div>
      <div>${escaped(n.content)}</div>
      ${n.roll_used ? `<div class="meta roll-meta">🎲 ${escaped(n.roll_note)}</div>` : ''}
      ${n.limit_keyword ? `<div class="meta">限定词：${escaped(n.limit_keyword)}</div>` : ''}
    </div>
  `).join('');
  const list = $('narrative-list');
  list.scrollTop = list.scrollHeight;
}

function renderActions(room) {
  const box = $('game-actions');
  const me = room.players.find(p => p.is_me);
  if (!me) {
    box.innerHTML = '';
    return;
  }

  captureControls();

  if (room.status === 0) {
    const mePlayer = room.players.find(p => p.is_me);
    const isReady = !!mePlayer.ready;
    const isOwner = mePlayer.id === room.owner_id;
    const startButtons = isOwner
      ? `<button class="primary" onclick="startGame(true)">人不够的话，用 AI 补位并开始</button>
          <button onclick="startGame(false)">直接开始（需满4人）</button>`
      : `<span class="hint">等待房主开始…</span>`;
    box.innerHTML = `
      <div class="action-box">
        <div class="toolbar">
          <button onclick="setReady(${isReady ? 0 : 1})">${isReady ? '取消准备' : '准备'}</button>
          ${startButtons}
        </div>
      </div>`;
    return;
  }

  if (room.status === 2) {
    box.innerHTML = `
      <div class="action-box">
        <h3>🏆 游戏结束</h3>
        ${room.winner_id ? `<p>胜利者：${escaped(playerName(room, room.winner_id))}</p>` : '<p>无人胜利</p>'}
      </div>`;
    return;
  }

if (!me.alive) {
      box.innerHTML = `
        <div class="action-box">
          <h3>💀 你已死亡，无法发言</h3>
        </div>`;
      return;
    }

  const isMyTurn = room.turn_player_id === me.id;
  const targetOptions = room.players
    .filter(p => p.id !== me.id)
    .map(p => `<option value="${p.id}">${escaped(p.name)}${p.alive ? '' : '（已死亡）'}</option>`).join('');

  box.innerHTML = `
    <div class="action-box">
      <h3>${isMyTurn ? '🎲 轮到你发言' : `⏳ 等待 ${escaped(playerName(room, room.turn_player_id))} 发言`}</h3>
      <textarea id="speak-content" ${isMyTurn ? '' : 'disabled'} placeholder="一句话，描述你的行动…"></textarea>
      <div class="toolbar">
        <select id="speak-operation" ${isMyTurn ? '' : 'disabled'}>
          <option value="create">创造</option>
          <option value="twist">扭曲</option>
          <option value="explain">解释/补充</option>
          <option value="limit">限定</option>
          <option value="normal">普通陈述</option>
        </select>
        <input id="speak-keyword" ${isMyTurn ? '' : 'disabled'} placeholder="限定关键词（选择限定时填）">
        <select id="speak-target" ${isMyTurn ? '' : 'disabled'}>
          <option value="0">不指定目标</option>
          ${targetOptions}
        </select>
        <button id="speak-btn" class="primary" onclick="speak()" ${isMyTurn ? '' : 'disabled'}>发言</button>
          <button class="danger-btn" onclick="lethalAttack()" ${isMyTurn ? '' : 'disabled'}>💀 致命</button>
      </div>
      <div class="toolbar">
        <button onclick="declareDeath(${me.id})" ${me.alive ? '' : 'disabled'}>自认死亡</button>
        <span class="hint">伤害来源可在自己回合宣告目标死亡：选择目标后点击</span>
        <select id="kill-target" ${isMyTurn ? '' : 'disabled'}>
          <option value="0">选择宣告死亡目标</option>
          ${targetOptions}
        </select>
        <button onclick="declareDeath(parseInt($('kill-target').value || '0', 10))" ${isMyTurn ? '' : 'disabled'}>宣告目标死亡</button>
      </div>
      <div class="toolbar">
        <button class="danger-btn" onclick="if(confirm('确定退出游戏吗？')) leaveRoom()">退出游戏</button>
      </div>
    </div>`;
if (room.mode === 1 && me.id === room.owner_id) {
      const gmTargetOptions = room.players
        .map(p => `<option value="${p.id}">${escaped(p.name)}${p.alive ? '' : '（已死亡）'}${p.id === me.id ? '（我）' : ''}</option>`).join('');
      box.innerHTML += `
        <div class="action-box gm-box">
          <h3>🛡️ GM 控制台</h3>
          <div class="toolbar">
            <select id="gm-target">
              <option value="0">选择GM操作目标</option>
              ${gmTargetOptions}
            </select>
            <button onclick="gmCommand('force_death')">强制击杀</button>
            <button onclick="gmCommand('revive')">复活</button>
            <button onclick="gmCommand('resolve_warnings')">清除濒死警告</button>
            <button onclick="gmCommand('set_turn')">切换到该玩家回合</button>
          </div>
        </div>`;
    }
applyControls();
  bindControlListeners();
}

/* ---------------- 轮询 ---------------- */

function startPolling() {
  if (state.pollTimer) clearInterval(state.pollTimer);
  state.pollTimer = setInterval(poll, 1000);
}

async function poll() {
  if (!state.room_id) return;
  try {
    const url = `/api/game/state?room_id=${encodeURIComponent(state.room_id)}&token=${encodeURIComponent(state.token)}`;
    const data = await api(url);
    if (data && data.state) {
        const key = stateKey(data.state);
        if (key !== lastStateKey) render(data.state);
      }
  } catch (e) {
    /* 网络暂时不可用，不打断玩家 */
  }
}

/* ---------------- 初始化 ---------------- */

(function init() {
  if (state.room_id && state.token) {
    startPolling();
    poll();
  }
  $('player-name').value = localStorage.getItem('luansha_nickname') || '';
  $('player-name').addEventListener('change', e => {
    localStorage.setItem('luansha_nickname', e.target.value.trim());
  });
})();