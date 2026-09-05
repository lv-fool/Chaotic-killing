const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');

const cwd = process.cwd();
const gamePort = parseInt(process.env.SIM_GAME_PORT || '18200', 10);
const fakePort = parseInt(process.env.SIM_FAKE_PORT || '19100', 10);
const numGames = parseInt(process.env.SIM_GAMES || '10', 10);
const maxPolls = parseInt(process.env.SIM_MAX_POLLS || '120', 10);
const rescueSuccess = parseFloat(process.env.RESCUE_SUCCESS || '1.0');
const dangerAlways = process.env.DANGER_ALWAYS !== '0';

const configPath = path.join(cwd, 'luansha_ai.conf');
const configBackup = configPath + '.bak';
let gameLog = '';

function request(method, port, pathname, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = http.request({
      hostname: '127.0.0.1',
      port,
      path: pathname,
      method,
      agent: false,
      headers: data ? {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(data)
      } : {}
    }, res => {
      let d = '';
      res.on('data', c => d += c);
      res.on('end', () => {
        try { resolve(JSON.parse(d)); }
        catch (e) { reject(new Error('bad json: ' + d.slice(0, 200))); }
      });
    });
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

const get = (port, p) => request('GET', port, p, null);
const post = (port, p, b) => request('POST', port, p, b);

function parsePlayers(body) {
  const m = body.match(/在线玩家：([^\n]+)/);
  if (!m) return [];
  return m[1].split(/[,，]/).map(s => s.trim()).filter(Boolean);
}
function parseSelf(body) {
  const m = body.match(/你：([^\n]+)/);
  return m ? m[1].trim() : '';
}

let stats = {
  attackRequests: 0,
  rescueRequests: 0,
  narrationRequests: 0,
  totalGames: 0,
  deaths: [],
  warningsCreated: [],
  warningsResolved: [],
  deathNarratives: [],
  rounds: [],
  ended: 0,
  maxRounds: 0,
};

function startFakeServer() {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      let body = '';
      req.on('data', c => body += c);
      req.on('end', () => {
        let text;
        if (body.includes('danger')) {
          stats.attackRequests++;
          const players = parsePlayers(body);
          const self = parseSelf(body);
          const targets = players.filter(p => p !== self);
          const target = targets.length ? targets[Math.floor(Math.random() * targets.length)] : '小明';
          const danger = dangerAlways;
          text = JSON.stringify({
            target,
            content: `我对${target}发起致命攻击。`,
            danger,
            reason: danger ? '致命攻击' : '普通试探'
          });
        } else if (body.includes('rescued')) {
          stats.rescueRequests++;
          const ok = Math.random() < rescueSuccess;
          text = JSON.stringify({
            content: ok ? '我躲开了这次攻击。' : '我试图躲开但失败了。',
            rescued: ok,
            reason: ok ? '成功躲开' : '未能躲开'
          });
        } else {
          stats.narrationRequests++;
          text = '我观察四周。';
        }
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({ choices: [{ message: { content: text } }] }));
      });
    });
    server.listen(fakePort, '127.0.0.1', () => resolve(server));
  });
}

function startGameServer() {
  return new Promise((resolve, reject) => {
    const env = {
      ...process.env,
      LUANSHA_AI_URL: `http://127.0.0.1:${fakePort}/chat/completions`,
      LUANSHA_AI_KEY: 'sk-test',
      LUANSHA_AI_MODEL: 'fake-model',
      LUANSHA_AI_MIN_INTERVAL: '0',
      LUANSHA_RATE_LIMIT_MAX: '1000000',
      LUANSHA_RATE_LIMIT_WINDOW: '1'
    };
    const child = spawn(path.join(cwd, 'luansha.exe'), ['--no-open', String(gamePort)], {
      cwd,
      env,
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let out = '';
    child.stdout.on('data', d => out += d);
    child.stderr.on('data', d => out += d);
    const timer = setTimeout(() => {
      reject(new Error('game server start timeout\n' + out));
    }, 5000);
    // Wait until HTTP answers
    const tryConnect = () => {
      get(gamePort, '/api/room/list').then(() => {
        clearTimeout(timer);
        resolve(child);
      }).catch(() => setTimeout(tryConnect, 100));
    };
    tryConnect();
  });
}

async function playOneGame(gameIndex) {
  const c = await post(gamePort, '/api/room/create', { name: '测试', mode: 'auto', player_name: '小明' });
  if (!c.ok) throw new Error('create failed: ' + JSON.stringify(c));
  const roomId = c.room_id;
  const token = c.token;
  for (let i = 0; i < 3; i++) {
    await post(gamePort, '/api/room/add_ai', { room_id: roomId, token });
  }
  await post(gamePort, '/api/game/set_ready', { room_id: roomId, token, ready: 1 });
  await post(gamePort, '/api/game/start', { room_id: roomId, token, fill_ai: 1 });

  let humanSpoken = false;
  let lastState = null;
  for (let poll = 0; poll < maxPolls; poll++) {
    await new Promise(r => setTimeout(r, 50));
    const s = await get(gamePort, `/api/game/state?room_id=${roomId}&token=${token}`);
    if (!s.ok) break;
    lastState = s.state;
    if (s.state.status === 2) break;
    const me = s.state.players.find(p => p.is_me);
    if (me && me.alive && s.state.turn_player_id === me.id) {
      const content = humanSpoken ? '我观察四周。' : '我走出了家门。';
      const sp = await post(gamePort, '/api/game/speak', {
        room_id: roomId, token, operation: 'normal', content,
        limit_keyword: '', target_id: 0
      });
      if (sp.ok) humanSpoken = true;
    }
  }

  const st = lastState;
  const deadCount = st ? st.players.filter(p => !p.alive).length : 0;
  const warningCount = st ? st.warnings.length : 0;
  const resolvedWarnings = st ? st.warnings.filter(w => w.resolved).length : 0;
  const deathNarratives = st ? st.narratives.filter(n => n.operation === '死亡宣告').length : 0;

  stats.totalGames++;
  stats.deaths.push(deadCount);
  stats.warningsCreated.push(warningCount);
  stats.warningsResolved.push(resolvedWarnings);
  stats.deathNarratives.push(deathNarratives);
  stats.rounds.push(st ? st.round : 0);
  if (st && st.status === 2) stats.ended++;
  if (st && st.round > stats.maxRounds) stats.maxRounds = st.round;
}

(async () => {
  // 暂时移开真实配置文件，避免假 AI 测试打到真实服务商
  if (fs.existsSync(configPath)) {
    fs.renameSync(configPath, configBackup);
  }

  let fakeServer;
  let gameProc;
  try {
    fakeServer = await startFakeServer();
    gameProc = await startGameServer();
    for (let i = 0; i < numGames; i++) {
      await playOneGame(i);
      process.stdout.write(`game ${i + 1}/${numGames} done, deaths=${stats.deaths[i]}\n`);
    }
  } finally {
    if (gameProc) {
      try { execSync('taskkill //F //IM luansha.exe >/dev/null 2>&1'); } catch (e) {}
      gameProc.kill();
    }
    if (fakeServer) fakeServer.close();
    if (fs.existsSync(configBackup)) {
      fs.renameSync(configBackup, configPath);
    }
  }

  const avg = arr => arr.length ? arr.reduce((a, b) => a + b, 0) / arr.length : 0;
  const summary = {
    games: stats.totalGames,
    avgDeathsPerGame: avg(stats.deaths),
    deathRate: stats.totalGames ? stats.deaths.filter(d => d > 0).length / stats.totalGames : 0,
    totalDeaths: stats.deaths.reduce((a, b) => a + b, 0),
    avgWarningsCreated: avg(stats.warningsCreated),
    avgWarningsResolved: avg(stats.warningsResolved),
    avgDeathNarratives: avg(stats.deathNarratives),
    avgRounds: avg(stats.rounds),
    maxRounds: stats.maxRounds,
    endedGames: stats.ended,
    attackRequests: stats.attackRequests,
    rescueRequests: stats.rescueRequests,
    narrationRequests: stats.narrationRequests,
    perGame: stats
  };
  console.log('SUMMARY_JSON=' + JSON.stringify(summary, null, 2));
})().catch(e => {
  console.error('ERROR:', e);
  if (gameLog) console.error('GAME_LOG:\n' + gameLog);
  process.exit(1);
});
