const http = require('http');
const PORT = parseInt(process.env.DRIVE_PORT || '18300', 10);
const MAX_TICKS = parseInt(process.env.DRIVE_TICKS || '400', 10);

function request(method, pathname, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = http.request({
      hostname: '127.0.0.1', port: PORT, path: pathname, method, agent: false,
      headers: data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {}
    }, res => {
      let d = '';
      res.on('data', c => d += c);
      res.on('end', () => { try { resolve(JSON.parse(d)); } catch (e) { reject(new Error('bad json: ' + d.slice(0, 200))); } });
    });
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

(async () => {
  const c = await request('POST', '/api/room/create', { name: '真实测试', mode: 'auto', player_name: '小明' });
  if (!c.ok) throw new Error('create failed: ' + JSON.stringify(c));
  const roomId = c.room_id, token = c.token;
  for (let i = 0; i < 3; i++) await request('POST', '/api/room/add_ai', { room_id: roomId, token });
  await request('POST', '/api/game/set_ready', { room_id: roomId, token, ready: 1 });
  await request('POST', '/api/game/start', { room_id: roomId, token, fill_ai: 1 });

  let humanSpoken = false;
  let lastRound = 0;
  for (let tick = 0; tick < MAX_TICKS; tick++) {
    await new Promise(r => setTimeout(r, 500));
    const s = await request('GET', `/api/game/state?room_id=${roomId}&token=${token}`);
    if (!s.ok) break;
    if (s.state.status === 2) { console.log('game ended'); break; }
    if (s.state.round !== lastRound) {
      lastRound = s.state.round;
      console.log('round ' + lastRound + '  narratives=' + s.state.narratives.length);
    }
    const me = s.state.players.find(p => p.is_me);
    if (me && me.alive && s.state.turn_player_id === me.id) {
      const content = humanSpoken ? '我盯着他手里的东西，慢慢往门口挪。' : '我走出了家门。';
      await request('POST', '/api/game/speak', { room_id: roomId, token, operation: 'normal', content, limit_keyword: '', target_id: 0 });
      humanSpoken = true;
    }
  }
  console.log('DRIVE_DONE room=' + roomId);
})().catch(e => { console.error('ERROR:', e.message); process.exit(1); });
