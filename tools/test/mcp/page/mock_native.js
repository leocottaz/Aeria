// Faux hôte natif pour tester la page (ui/) dans un navigateur : injecté par mock_serve.js avant tout autre script.
// Il imite le pont (chrome.webview), le stockage de données, trois « modèles » scriptés (API, Pro, gratuit) et des serveurs MCP
// écrits en JS. Aucun vrai processus, aucun réseau. Pilotage depuis la page : window.__mock (voir checks.js).
(() => {
  const listeners = [];
  const post = (o) => setTimeout(() => listeners.forEach((fn) => fn({ data: o })), 0);
  const mock = (window.__mock = { log: [], requests: [], script: [], servers: new Map(), gen: 0, store: JSON.parse(localStorage.getItem('mockstore') || '{}') });
  const persist = () => localStorage.setItem('mockstore', JSON.stringify(mock.store));

  window.chrome = {
    webview: {
      postMessage: (s) => { try { handle(String(s)); } catch (e) { console.error('mock natif :', e); } },
      addEventListener: (t, fn) => { if (t === 'message') listeners.push(fn); },
    },
  };

  // ------------------------------------------------------------ serveurs MCP imités
  const SCHEMA_ECHO = { type: 'object', properties: { text: { type: 'string', description: 'Texte à répéter' } }, required: ['text'] };
  const LONG = 'x'.repeat(60);
  const TOOLS = [
    { name: 'echo', description: 'Répète le texte donné', inputSchema: SCHEMA_ECHO },
    { name: 'lecture', description: 'Outil en lecture seule', inputSchema: { type: 'object', properties: {} }, annotations: { readOnlyHint: true } },
    { name: 'boom', description: 'Échoue toujours', inputSchema: { type: 'object', properties: {} } },
    { name: 'bizarre', description: 'Schéma exotique', inputSchema: { $schema: 'http://json-schema.org/draft-07/schema#', type: 'object',
      properties: { a: { anyOf: [{ type: 'string' }, { type: 'null' }], default: 'x' }, b: { type: ['integer', 'null'], format: 'int32' }, c: { $ref: '#/definitions/c' }, d: { type: 'array', items: { enum: ['u', 'v'] } } }, required: ['a'] } },
    { name: LONG, description: 'Nom très long', inputSchema: { type: 'object', properties: {} } },
    { name: 'rouge/vert é', description: 'Nom avec caractères interdits', inputSchema: { type: 'object', properties: {} } },
  ];

  function serverReply(srv, msg) {
    const reply = (result) => ({ jsonrpc: '2.0', id: msg.id, result });
    if (msg.method === 'initialize') return reply({ protocolVersion: srv.protocol || msg.params.protocolVersion, capabilities: srv.noTools ? {} : { tools: { listChanged: true } }, serverInfo: { name: 'Faux serveur', version: '1.0' } });
    if (msg.method === 'tools/list') return reply({ tools: srv.tools });
    if (msg.method === 'ping') return reply({});
    if (msg.method === 'tools/call') {
      srv.calls.push({ name: msg.params.name, arguments: msg.params.arguments });
      const n = msg.params.name;
      if (n === 'echo') return reply({ content: [{ type: 'text', text: `écho : ${msg.params.arguments.text}` }, { type: 'image', data: 'AAAA', mimeType: 'image/png' }] });
      if (n === 'boom') return reply({ content: [{ type: 'text', text: 'kaboom' }], isError: true });
      if (n === 'slow') return null; // ne répond jamais (test du délai)
      if (n === 'huge') return reply({ content: [{ type: 'text', text: 'y'.repeat(50000) }] });
      return reply({ content: [{ type: 'text', text: `ok ${n}` }], structuredContent: undefined });
    }
    return null;
  }

  function startServer(arg) {
    const [id, command, args, env] = arg.split('\x1f');
    const old = mock.servers.get(id);
    const gen = ++mock.gen;
    if (old) { old.alive = false; post({ t: 'mcp', id, gen: old.gen, ev: 'exit', code: 0, stderr: '' }); }
    if (command === 'introuvable') { post({ t: 'mcp', id, gen, ev: 'fail', msg: `Commande introuvable dans le PATH : ${command}` }); return; }
    const srv = { id, gen, alive: true, command, args: args ? args.split('\x1e') : [], env: env ? env.split('\x1e') : [], tools: command === 'faux-vide' ? [] : TOOLS.concat(command === 'faux-lent' ? [{ name: 'slow', inputSchema: { type: 'object', properties: {} } }, { name: 'huge', inputSchema: { type: 'object', properties: {} } }] : []), calls: [], noTools: command === 'faux-sans-outils', protocol: command === 'faux-vieux' ? '1999-01-01' : undefined };
    mock.servers.set(id, srv);
    post({ t: 'mcp', id, gen, ev: 'started' });
  }

  function mcpSend(arg) {
    const nl = arg.indexOf('\n');
    const id = arg.slice(0, nl);
    const srv = mock.servers.get(id);
    if (!srv || !srv.alive) return;
    const msg = JSON.parse(arg.slice(nl + 1));
    srv.received = (srv.received || []).concat(msg);
    if (msg.method === 'tools/list' && srv.listDelay) return;
    if (!msg.method || msg.id === undefined) return;
    const r = serverReply(srv, msg);
    if (r) post({ t: 'mcp', id, gen: srv.gen, ev: 'line', data: JSON.stringify(r) });
  }

  // Les serveurs peuvent aussi faire une requête (ping) ou annoncer un changement d'outils.
  mock.serverSays = (id, obj) => { const s = mock.servers.get(id); post({ t: 'mcp', id, gen: s.gen, ev: 'line', data: JSON.stringify(obj) }); };
  mock.serverDies = (id, code = 1, stderr = 'plantage de test') => { const s = mock.servers.get(id); s.alive = false; post({ t: 'mcp', id, gen: s.gen, ev: 'exit', code, stderr }); };

  // ------------------------------------------------------------ modèles scriptés
  // mock.script : une entrée par requête du modèle : { text, tool: { name, input } , tools: [{ name, input }] }.
  const nextStep = () => mock.script.shift() || { text: 'Réponse par défaut' };
  const sse = (ev) => `event: ${ev.type}\ndata: ${JSON.stringify(ev)}\n\n`;

  function apiResponse(id, body) {
    mock.requests.push({ kind: 'api', body: JSON.parse(body) });
    const step = nextStep();
    const uses = step.tools || (step.tool ? [step.tool] : []);
    let out = sse({ type: 'message_start', message: { id: 'm', role: 'assistant', content: [] } });
    let idx = 0;
    if (step.text) {
      out += sse({ type: 'content_block_start', index: idx, content_block: { type: 'text', text: '' } });
      out += sse({ type: 'content_block_delta', index: idx, delta: { type: 'text_delta', text: step.text } });
      out += sse({ type: 'content_block_stop', index: idx++ });
    }
    uses.forEach((u, k) => {
      out += sse({ type: 'content_block_start', index: idx, content_block: { type: 'tool_use', id: `toolu_${mock.requests.length}_${k}`, name: u.name, input: {} } });
      out += sse({ type: 'content_block_delta', index: idx, delta: { type: 'input_json_delta', partial_json: JSON.stringify(u.input || {}) } });
      out += sse({ type: 'content_block_stop', index: idx++ });
    });
    out += sse({ type: 'message_delta', delta: { stop_reason: uses.length ? 'tool_use' : 'end_turn' } });
    out += sse({ type: 'message_stop' });
    post({ t: 'chunk', id, data: out });
    post({ t: 'done', id, cancelled: false, exit: 0, stderr: '' });
  }

  function proResponse(id, f) {
    mock.requests.push({ kind: 'pro', system: f[5], prompt: f[6], tools: f[4] });
    const step = nextStep();
    const lines = [{ type: 'assistant', message: { content: [{ type: 'text', text: step.text || '' }] } }, { type: 'result', is_error: false, result: step.text || '', session_id: 's1' }];
    post({ t: 'chunk', id, data: lines.map((l) => JSON.stringify(l)).join('\n') + '\n' });
    post({ t: 'done', id, cancelled: false, exit: 0, stderr: '' });
  }

  function freeResponse(id, body) {
    mock.requests.push({ kind: 'free', body: JSON.parse(body) });
    const step = nextStep();
    const uses = step.tools || (step.tool ? [step.tool] : []);
    let out = '';
    if (step.text) out += `data: ${JSON.stringify({ choices: [{ delta: { content: step.text } }] })}\n\n`;
    uses.forEach((u, k) => { out += `data: ${JSON.stringify({ choices: [{ delta: { tool_calls: [{ index: k, id: `call_${mock.requests.length}_${k}`, function: { name: u.name, arguments: JSON.stringify(u.input || {}) } }] } }] })}\n\n`; });
    out += `data: ${JSON.stringify({ choices: [{ delta: {}, finish_reason: uses.length ? 'tool_calls' : 'stop' }] })}\n\ndata: [DONE]\n\n`;
    post({ t: 'chunk', id, data: out });
    post({ t: 'done', id, cancelled: false, exit: 0, stderr: '' });
  }

  // ------------------------------------------------------------ routeur des commandes
  function handle(s) {
    const nl = s.indexOf('\n');
    const cmd = nl < 0 ? s : s.slice(0, nl);
    const arg = nl < 0 ? '' : s.slice(nl + 1);
    mock.log.push(cmd === 'mcpsend' ? `mcpsend ${arg.slice(0, 80)}` : cmd);
    if (cmd === 'loadstore') post({ t: 'store', data: mock.store });
    else if (cmd === 'savestore') { const i = arg.indexOf('\n'); mock.store[arg.slice(0, i)] = arg.slice(i + 1); persist(); }
    else if (cmd === 'ready') post({ t: 'init', hasKey: true, keyHint: 'sk-ant-…test', autostart: false, lowMemory: false, freeKeys: { gemini: { hasKey: true, keyHint: '…test' }, groq: { hasKey: true, keyHint: '…test' }, openai: { hasKey: true, keyHint: '…test' } }, version: '0.0.0', canUpdate: false });
    else if (cmd === 'ccstatus') post({ t: 'ccstatus', found: true, version: 'test', path: 'claude' });
    else if (cmd === 'mcpstart') { mock.log.push(`  ${arg.split('\x1f')[0]}`); startServer(arg); }
    else if (cmd === 'mcpsend') mcpSend(arg);
    else if (cmd === 'mcpstop') {
      const srv = mock.servers.get(arg);
      if (srv && srv.alive) { srv.alive = false; post({ t: 'mcp', id: arg, gen: srv.gen, ev: 'exit', code: 0, stderr: '' }); }
    } else if (cmd === 'http') { const f = arg.split('\n'); apiResponse(f[0], f.slice(4).join('\n')); }
    else if (cmd === 'cc') { const f = arg.split('\x1f'); proResponse(f[0], f); }
    else if (cmd === 'fhttp') {
      const f = arg.split('\n'); // id, service, méthode, chemin, corps
      if (f[2] === 'GET') { // liste des modèles : ne consomme pas le script
        post({ t: 'chunk', id: f[0], data: JSON.stringify({ data: [{ id: 'gemini-2.5-flash' }, { id: 'openai/gpt-oss-120b' }, { id: 'gpt-5-mini' }] }) });
        post({ t: 'done', id: f[0], cancelled: false, exit: 0, stderr: '' });
      } else freeResponse(f[0], f.slice(4).join('\n'));
    }
    else if (cmd === 'cancel') { /* rien à annuler : les réponses scriptées sont instantanées */ }
  }
})();
