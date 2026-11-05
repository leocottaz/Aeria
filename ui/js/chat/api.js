// Mode « Clé API » : requêtes streaming à l'API Messages, avec outils.
import { toast } from '../core/util.js';
import { http, apiErrorText } from '../core/bridge.js';
import { settings, apiModel } from '../core/settings.js';
import { convo, turn, saveConvo, handleEvent, rollback, endTurn, setStreaming, renderChat, scheduleLive } from './chat.js';
import { TOOLS, execTool } from './tools.js';
import { buildSystem } from './prompts.js';

// Outil serveur : l'API lance elle-même les recherches (facturées à part) et renvoie les résultats dans le flux.
const WEB_SEARCH_TOOL = { type: 'web_search_20250305', name: 'web_search', max_uses: 3 };

// Si l'API refuse la recherche (non activée pour l'organisation), on dit comment s'en passer.
const webHint = (message, raw) => (settings.webSearch && /web.?search/i.test(String(raw)) ? `${message} Désactive « Chercher sur internet » dans Réglages › Aeria.` : message);

function streamStep() {
  Object.assign(turn, { blocks: [], stop: null, stopDetails: null, error: null, buf: '', base: 0 });
  const model = apiModel();
  const body = {
    model,
    max_tokens: 64000,
    stream: true,
    thinking: { type: 'adaptive', display: 'summarized' },
    output_config: { effort: settings.effort },
    cache_control: { type: 'ephemeral' },
    system: buildSystem('api'),
    tools: [...TOOLS.filter((t) => t.name !== 'screenshot' || settings.screenAccess), ...(settings.webSearch ? [WEB_SEARCH_TOOL] : [])],
    messages: convo.messages.map(({ role, content }) => ({ role, content })), // sans nos champs (via)
  };
  // Repli automatique côté serveur si le modèle décline (pas disponible pour Haiku).
  let betas = '';
  if (!model.includes('haiku')) {
    body.fallbacks = 'default';
    betas = 'server-side-fallback-2026-07-01';
  }
  turn.reqId = http.request('POST', '/v1/messages', betas, body, {
    onChunk: onStreamChunk,
    onDone: onStreamDone,
    onError: onStreamError,
  });
  setStreaming(true);
}

function onStreamChunk(data) {
  if (!turn) return;
  turn.buf += data.replace(/\r\n/g, '\n');
  let i;
  while ((i = turn.buf.indexOf('\n\n')) >= 0) {
    const raw = turn.buf.slice(0, i);
    turn.buf = turn.buf.slice(i + 2);
    const payload = raw.split('\n').filter((l) => l.startsWith('data:')).map((l) => l.slice(5).trimStart()).join('\n');
    if (!payload) continue;
    let ev;
    try { ev = JSON.parse(payload); } catch { continue; }
    handleEvent(ev);
  }
  scheduleLive();
}

// Blocs renvoyés tels quels à l'API : on retire seulement nos champs internes.
function finalBlocks(partial) {
  const out = [];
  for (const b of turn.blocks) {
    if (!b) continue;
    if (partial && !b._complete) {
      if (b.type === 'text' && b.text) out.push({ type: 'text', text: b.text });
      continue;
    }
    if (partial && (b.type === 'tool_use' || b.type === 'server_tool_use' || b.type === 'web_search_tool_result')) continue;
    if (b.type === 'thinking' && !b.signature) continue;
    const { _json, _invalid, _complete, ...clean } = b;
    out.push(clean);
  }
  return out;
}

async function runTools(blocks) {
  const raws = turn.blocks.slice(); // lu avant le premier await : le tour peut changer ensuite
  const results = [];
  for (const b of blocks) {
    if (b.type !== 'tool_use') continue;
    const raw = raws.find((x) => x && x.id === b.id);
    const res = { type: 'tool_result', tool_use_id: b.id };
    try {
      if (raw?._invalid) throw new Error('INVALID_JSON : arguments illisibles, renvoie l’appel.');
      res.content = await execTool(b.name, b.input || {}, b.id);
    } catch (err) {
      res.content = String(err.message || err);
      res.is_error = true;
    }
    results.push(res);
  }
  return results;
}

function onStreamDone(cancelled) {
  if (!turn) return;
  if (turn.error) {
    const overloaded = turn.error.type === 'overloaded_error';
    rollback(overloaded ? "L'API Claude est surchargée, réessaie." : webHint(turn.error.message || 'Erreur pendant la réponse.', turn.error.message), overloaded);
    return;
  }
  if (cancelled) {
    const blocks = finalBlocks(true);
    if (blocks.some((b) => b.type === 'text' && b.text.trim())) convo.messages.push({ role: 'assistant', content: blocks });
    else { rollback(null); return; }
    endTurn();
    return;
  }
  if (turn.stop === 'refusal') {
    const why = turn.stopDetails?.explanation;
    rollback(`Claude a décliné cette demande.${why ? ' ' + why : ''}`);
    return;
  }
  const blocks = finalBlocks(false);
  if (!blocks.length) { rollback('Réponse vide, réessaie.'); return; }
  convo.messages.push({ role: 'assistant', content: blocks });
  if (turn.stop === 'pause_turn' && turn.loops < 8) { // recherche longue : l'API demande de reprendre là où elle s'est arrêtée
    turn.loops++;
    renderChat();
    streamStep();
    return;
  }
  if (turn.stop === 'tool_use' && turn.loops < 8) {
    // Certains outils attendent le natif (recherche de fichiers) : la carte reste « en cours ».
    const t = turn;
    const pending = runTools(blocks);
    t.blocks = []; // déjà dans convo.messages : ne pas les afficher deux fois
    renderChat();
    pending.then((results) => {
      if (turn !== t) return; // annulé entre-temps
      convo.messages.push({ role: 'user', content: results });
      t.loops++;
      saveConvo();
      renderChat();
      streamStep();
    });
    return;
  }
  if (turn.stop === 'max_tokens') toast('Réponse tronquée (longueur maximale)');
  endTurn();
}

// Pas de clé, clé refusée, quota, surcharge, panne : le modèle gratuit peut prendre le relais.
// (Pas pour une erreur réseau : il serait lui aussi injoignable.)
function onStreamError(status, body) {
  if (!turn) return;
  const claudeDown = status === -1 || status === -3 /* Claude Code introuvable */ || status === 401 || status === 403 || status === 429 || status >= 500;
  rollback(webHint(apiErrorText(status, body), body), claudeDown);
}

export { streamStep, onStreamError };
