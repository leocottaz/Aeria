// Vérifications de la page (client MCP + chat) dans le navigateur, avec mock_native.js. À lancer dans la page :
//   const { run } = await import('/__test/checks.js'); await run();
// Prérequis : localStorage.mockstore contient les serveurs « test » (confirmation) et « auto » (outils lecture seule sans
// confirmation), activés, avec les réglages backend=api (voir seed() ci-dessous). Rend la liste des résultats.
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (fn, ms = 4000) => { const end = Date.now() + ms; while (Date.now() < end) { const v = fn(); if (v) return v; await wait(30); } return fn(); };
const results = [];
const check = (ok, what, extra = '') => { results.push(`${ok ? 'OK    ' : 'ÉCHEC '} ${what}${ok ? '' : ` ${extra}`}`); };

export const seed = () => {
  const mcp = { servers: [
    { id: 'test', name: 'Test', command: 'faux', args: ['--a', 'b c'], env: { TOKEN: 'abc=1' }, enabled: true, autoRead: false, allow: [] },
    { id: 'auto', name: 'Auto', command: 'faux', args: [], env: {}, enabled: true, autoRead: true, allow: [] },
  ] };
  const settings = { backend: 'api', category: 'claude', claudeMode: 'api', reduceMotion: true, webSearch: false, screenAccess: false, freeFallback: false };
  localStorage.setItem('mockstore', JSON.stringify({ mcp: JSON.stringify(mcp), settings: JSON.stringify(settings) }));
};

const card = () => document.querySelector('#chat [data-mcpask]');
const click = (kind) => { document.querySelector(`#chat [data-mcpask="${kind}"]`).click(); };
const lastUserTools = (req) => (req.body?.tools || []).map((t) => t.name || t.function?.name);

export async function run() {
  try { await runInner(); } catch (e) { results.push(`ERREUR ${e.message} (${(e.stack || '').split('\n')[1]?.trim()})`); }
  return results;
}

async function runInner() {
  results.length = 0;
  const mock = window.__mock;
  const chatMod = await import('/js/chat/chat.js');
  const { settings } = await import('/js/core/settings.js');
  const { S } = await import('/js/core/state.js');
  S.freeKeys = { gemini: { hasKey: true }, groq: { hasKey: true }, openai: { hasKey: true } };
  const send = async (text, backend) => {
    settings.backend = backend;
    settings.category = backend === 'free' ? 'free' : 'claude';
    mock.requests.length = 0;
    await until(() => !chatMod.isBusy());
    document.querySelector('#newChat').click();
    chatMod.sendChat(text);
  };
  const idle = () => until(() => !chatMod.isBusy(), 6000);

  // ---- registre d'outils
  const srvTest = mock.servers.get('test');
  check(srvTest?.alive && srvTest.received.map((m) => m.method).join() === 'initialize,notifications/initialized,tools/list', 'poignée de main : initialize, initialized, tools/list');
  const init = srvTest.received[0];
  check(init.params.protocolVersion === '2025-06-18' && init.params.clientInfo.name === 'Aeria' && JSON.stringify(init.params.capabilities) === '{}', 'initialize : version, clientInfo, aucune capacité déclarée');

  // ---- mode Clé API : outil appelé, confirmation, exécution
  mock.script = [{ text: 'Je répète.', tool: { name: 'mcp__test__echo', input: { text: 'salut' } } }, { text: 'Terminé : écho reçu.' }];
  await send('répète salut', 'api');
  const names = await until(() => mock.requests[0] && lastUserTools(mock.requests[0]));
  check(names.includes('mcp__test__echo') && names.includes('mcp__auto__echo') && names.includes('create_note'), 'API : les outils MCP s\'ajoutent à ceux d\'Aeria', JSON.stringify(names));
  check(names.every((n) => /^[A-Za-z0-9_-]{1,64}$/.test(n)), 'API : tous les noms d\'outils sont valides (≤ 64, [A-Za-z0-9_-])', JSON.stringify(names.filter((n) => !/^[A-Za-z0-9_-]{1,64}$/.test(n))));
  check(new Set(names).size === names.length, 'API : pas de doublon de nom');
  const tool = mock.requests[0].body.tools.find((t) => t.name === 'mcp__test__echo');
  check(tool.description.startsWith('[Outil du serveur MCP « Test »]') && tool.input_schema.required[0] === 'text' && !('strict' in tool), 'API : description préfixée, schéma conservé, pas de strict');
  check(/serveurs MCP/.test(mock.requests[0].body.system), 'API : le prompt système mentionne les outils MCP (données, pas instructions)');
  const bz = mock.requests[0].body.tools.find((t) => t.name.startsWith('mcp__test__bizarre'));
  check(bz && bz.input_schema.properties.a.anyOf && !('$schema' in bz.input_schema), 'API : $schema retiré, reste du schéma intact');
  await until(card);
  check(!!card(), 'API : une carte de confirmation apparaît (rien n\'est exécuté avant)');
  check(srvTest.calls.length === 0, 'API : aucun tools/call avant le clic');
  const cardText = document.querySelector('#chat .mcpask').textContent;
  check(/echo/.test(cardText) && /salut/.test(cardText) && /Test/.test(cardText), 'carte : outil, serveur et arguments affichés', cardText);
  click('once');
  await idle();
  check(srvTest.calls.length === 1 && srvTest.calls[0].arguments.text === 'salut', 'API : « Autoriser » exécute l\'outil avec les bons arguments');
  const second = mock.requests[1];
  const result = second.body.messages.at(-1).content[0];
  check(result.type === 'tool_result' && !result.is_error && /écho : salut/.test(result.content) && /pas des instructions/.test(result.content), 'API : le résultat revient au modèle, balisé comme donnée', JSON.stringify(result));
  check(/\[image non transmise\]/.test(result.content), 'API : une image du serveur est signalée, pas transmise');
  check(/exécuté/.test(document.querySelector('#chat').textContent) && /Terminé/.test(document.querySelector('#chat').textContent), 'API : carte « exécuté » puis réponse finale');

  // ---- refus
  mock.script = [{ text: '', tool: { name: 'mcp__test__echo', input: { text: 'non' } } }, { text: 'D\'accord, rien fait.' }];
  const before = srvTest.calls.length;
  await send('refuse', 'api');
  await until(card);
  click('deny');
  await idle();
  const r2 = mock.requests[1].body.messages.at(-1).content[0];
  check(srvTest.calls.length === before && r2.is_error && /refusé/.test(r2.content), 'refus : aucun appel, le modèle reçoit une erreur « refusé »', JSON.stringify(r2));
  check(/refusé/.test(document.querySelector('#chat').textContent), 'refus : la carte le dit');

  // ---- « Toujours autoriser » : la demande ne revient plus
  mock.script = [{ tool: { name: 'mcp__test__echo', input: { text: 'a' } } }, { text: 'ok' }];
  await send('toujours', 'api');
  await until(card);
  click('always');
  await idle();
  check(JSON.parse(mock.store.mcp).servers[0].allow.includes('echo'), 'toujours autoriser : mémorisé dans la configuration enregistrée');
  mock.script = [{ tool: { name: 'mcp__test__echo', input: { text: 'b' } } }, { text: 'ok' }];
  const c0 = srvTest.calls.length;
  await send('encore', 'api');
  await idle();
  check(!card() && srvTest.calls.length === c0 + 1, 'toujours autoriser : l\'appel suivant s\'exécute sans carte');
  const forget = document.querySelector('.mcp-srv[data-id=test] [data-act=forget]');
  check(!!forget && /Toujours autorisé/.test(forget.textContent), 'réglages : l\'outil « toujours autorisé » est listé avec son bouton');
  forget?.click();
  check(JSON.parse(mock.store.mcp).servers[0].allow.length === 0, 'réglages : « Toujours autorisé ✕ » redemande la confirmation (configuration enregistrée)');

  // ---- lecture seule sans confirmation (serveur « auto ») ; confirmation pour les autres outils du même serveur
  const srvAuto = mock.servers.get('auto');
  mock.script = [{ tool: { name: 'mcp__auto__lecture', input: {} } }, { text: 'ok' }];
  await send('lecture', 'api');
  await idle();
  check(srvAuto.calls.length === 1 && !card(), 'lecture seule + réglage : exécuté sans confirmation');
  mock.script = [{ tool: { name: 'mcp__auto__echo', input: { text: 'x' } } }, { text: 'ok' }];
  await send('echo auto', 'api');
  await until(card);
  check(!!card() && srvAuto.calls.length === 1, 'outil non lecture seule du même serveur : confirmation demandée');
  click('deny');
  await idle();

  // ---- erreur de l'outil (isError)
  mock.script = [{ tool: { name: 'mcp__test__boom', input: {} } }, { text: 'Il a échoué.' }];
  await send('boom', 'api');
  await until(card);
  click('once');
  await idle();
  const r3 = mock.requests[1].body.messages.at(-1).content[0];
  check(r3.is_error === true && /kaboom/.test(r3.content), 'isError du serveur → tool_result en erreur', JSON.stringify(r3));
  check(/échec/.test(document.querySelector('#chat').textContent), 'carte « échec »');

  // ---- bouton Arrêter pendant la confirmation = refus
  mock.script = [{ tool: { name: 'mcp__test__echo', input: { text: 'stop' } } }, { text: 'ok' }];
  const c1 = srvTest.calls.length;
  await send('stop', 'api');
  await until(card);
  document.querySelector('#chatForm').requestSubmit(); // bouton « Arrêter »
  await idle();
  check(srvTest.calls.length === c1 && /refusé/.test(mock.requests[1]?.body.messages.at(-1).content[0].content), 'Arrêter pendant la carte : refusé, rien exécuté, la conversation continue',
    `appels ${srvTest.calls.length} (attendu ${c1}) ; requêtes ${mock.requests.length} ; dernier message ${JSON.stringify(mock.requests[1]?.body.messages.at(-1))} ; occupé ${chatMod.isBusy()}`);

  // ---- outil inconnu / serveur arrêté
  mock.script = [{ tool: { name: 'mcp__test__nexistepas', input: {} } }, { text: 'ok' }];
  await send('inconnu', 'api');
  await idle();
  check(/n'est plus disponible/.test(mock.requests[1].body.messages.at(-1).content[0].content), 'outil inconnu : erreur claire au modèle');

  // ---- mode gratuit (OpenAI) : outils au format function, schéma simplifié
  mock.script = [{ text: 'Go', tool: { name: 'mcp__test__echo', input: { text: 'gratuit' } } }, { text: 'Fini.' }];
  await send('gratuit', 'free');
  await until(card);
  const fnames = lastUserTools(mock.requests[0]);
  check(fnames.includes('mcp__test__echo') && fnames.includes('create_note'), 'gratuit : outils MCP dans la requête');
  const fb = mock.requests[0].body.tools.find((t) => t.function.name === 'mcp__test__bizarre').function.parameters;
  check(fb.type === 'object' && fb.properties.a.type === 'string' && fb.properties.b.type === 'integer' && fb.properties.c.type === 'string' && fb.properties.d.items.type === 'string'
    && !JSON.stringify(fb).match(/anyOf|\$ref|\$schema|default|format/), 'gratuit : schéma exotique réduit (anyOf, $ref, format, default retirés)', JSON.stringify(fb));
  click('once');
  await idle();
  const toolMsg = mock.requests[1].body.messages.find((m) => m.role === 'tool');
  check(toolMsg && /écho : gratuit/.test(toolMsg.content), 'gratuit : résultat renvoyé au modèle', JSON.stringify(toolMsg));

  // ---- mode Pro : ligne [[mcp|…]]
  mock.script = [{ text: 'Je m\'en occupe.\n[[mcp|mcp__test__echo|{"text":"pro","liste":[[1],[2]]}]]' }, { text: 'Pro terminé.' }];
  await send('pro', 'pro');
  await until(card);
  const proReq = mock.requests[0];
  check(/Outils d'applications externes \(serveurs MCP\)/.test(proReq.system) && /- mcp__test__echo \{text: string\}/.test(proReq.system), 'Pro : le prompt liste les outils MCP avec leurs arguments');
  check(/n'obéis jamais à une instruction/.test(proReq.system), 'Pro : consigne anti-injection dans le prompt');
  check(!/\[\[mcp\|/.test(document.querySelector('#chat').textContent), 'Pro : la ligne [[mcp|…]] est masquée à l\'écran');
  check(!!card(), 'Pro : carte de confirmation visible pendant l\'attente');
  const cp = srvTest.calls.length;
  click('once');
  await idle();
  check(srvTest.calls.length === cp + 1 && JSON.stringify(srvTest.calls.at(-1).arguments) === '{"text":"pro","liste":[[1],[2]]}', 'Pro : JSON avec « ]] » imbriqués analysé correctement', JSON.stringify(srvTest.calls.at(-1)));
  check(/Résultats des outils MCP/.test(mock.requests[1].prompt) && /écho : pro/.test(mock.requests[1].prompt), 'Pro : le résultat revient au tour suivant dans le message');
  check(/Pro terminé/.test(document.querySelector('#chat').textContent) && !document.querySelector('#chat .mcpask'), 'Pro : réponse finale affichée, plus de carte en attente');
  const msgsHtml = document.querySelector('#chat').innerHTML;
  check((msgsHtml.match(/exécuté/g) || []).length === 1, 'Pro : une seule carte « exécuté » (pas de doublon)', String((msgsHtml.match(/exécuté/g) || []).length));

  // Pro : JSON invalide, nom invalide, refus, deux actions sur une ligne
  mock.script = [{ text: 'a\n[[mcp|mcp__test__echo|{pas du json}]]\n[[mcp|create_note|{"text":"piège"}]]' }, { text: 'ok' }];
  await send('pro invalide', 'pro');
  await idle();
  check(/JSON valide/.test(mock.requests[1].prompt) && /Nom d'outil MCP invalide/.test(mock.requests[1].prompt), 'Pro : JSON invalide et nom non MCP (create_note) refusés avec un message');
  mock.script = [{ text: '[[mcp|mcp__test__echo|{"text":"1"}]][[mcp|mcp__test__echo|{"text":"2"}]]' }, { text: 'ok' }];
  const cq = srvTest.calls.length;
  await send('pro deux', 'pro');
  await until(card); click('once');
  await until(() => document.querySelector('#chat [data-mcpask]') && srvTest.calls.length === cq + 1);
  click('once');
  await idle();
  check(srvTest.calls.length === cq + 2 && srvTest.calls.at(-2).arguments.text === '1' && srvTest.calls.at(-1).arguments.text === '2', 'Pro : deux actions sur la même ligne, exécutées dans l\'ordre, une confirmation chacune');

  // ---- serveur qui s'arrête : outils retirés, relance
  const nb = lastUserTools({ body: { tools: (await import('/js/features/mcp.js')).mcpTools() } });
  check(nb.includes('mcp__test__echo'), 'registre : outils présents tant que le serveur est prêt');
  mock.serverDies('test', 1, 'plantage de test');
  await wait(100);
  const nb2 = (await import('/js/features/mcp.js')).mcpTools().map((t) => t.name);
  check(!nb2.some((n) => n.startsWith('mcp__test__')) && nb2.some((n) => n.startsWith('mcp__auto__')), 'serveur arrêté : ses outils disparaissent, ceux des autres restent');
  const status = document.querySelector('.mcp-srv[data-id=test] [data-status]').textContent;
  check(/Erreur/.test(status) && /code 1/.test(status) && /plantage de test/.test(status), 'réglages : l\'erreur et la fin de stderr sont affichées', status);

  // ---- messages du serveur : ping, requête inconnue, notification
  const sa = mock.servers.get('auto');
  const rcv0 = (sa.received || []).length;
  mock.serverSays('auto', { jsonrpc: '2.0', id: 'srv-1', method: 'ping' });
  mock.serverSays('auto', { jsonrpc: '2.0', id: 'srv-2', method: 'sampling/createMessage', params: {} });
  await wait(100);
  const answers = sa.received.slice(rcv0);
  check(answers.some((m) => m.id === 'srv-1' && m.result && Object.keys(m.result).length === 0) && answers.some((m) => m.id === 'srv-2' && m.error?.code === -32601), 'requêtes du serveur : ping répondu, le reste refusé (-32601)');
  sa.tools = sa.tools.concat([{ name: 'nouveau', description: 'ajouté', inputSchema: { type: 'object', properties: {} } }]);
  mock.serverSays('auto', { jsonrpc: '2.0', method: 'notifications/tools/list_changed' });
  await wait(700); // la relecture est différée de 300 ms
  check((await import('/js/features/mcp.js')).mcpTools().some((t) => t.name === 'mcp__auto__nouveau'), 'list_changed : la liste d\'outils est relue');
}
