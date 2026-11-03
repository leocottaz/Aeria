// Mode « Gratuit » : un modèle gratuit en ligne (Google Gemini ou Groq), par leurs API
// compatibles OpenAI. Choisi dans les réglages, ou en secours quand Claude est indisponible.
// La conversation reste au format de Claude (blocs text / tool_use / tool_result) : on la
// convertit à chaque requête, ce qui permet de mélanger les deux dans une même conversation.
import { store, toast } from '../core/util.js';
import { http } from '../core/bridge.js';
import { settings } from '../core/settings.js';
import { S } from '../core/state.js';
import { convo, turn, rollback, endTurn, setStreaming, renderChat, scheduleLive } from './chat.js';
import { TOOLS, execTool } from './tools.js';
import { buildSystem, stripActions } from './prompts.js';

const SERVICES = {
  gemini: { name: 'Gemini', base: '/v1beta/openai/', keyUrl: 'https://aistudio.google.com/apikey', fallbackModel: 'gemini-3.8-flash' },
  groq: { name: 'Groq', base: '/openai/v1/', keyUrl: 'https://console.groq.com/keys', fallbackModel: 'openai/gpt-oss-120b' },
  openai: { name: 'OpenAI', base: '/v1/', keyUrl: 'https://platform.openai.com/api-keys', fallbackModel: 'gpt-5-mini' },
};

// Catégorie ChatGPT = OpenAI ; catégorie Gratuit (et secours de Claude) = Gemini ou Groq.
const service = () => (settings.backend === 'chatgpt' ? 'openai'
  : settings.freeService === 'groq' ? 'groq' : 'gemini');
const freeReady = () => !!S.freeKeys?.[service()]?.hasKey;

// ------------------------------------------------------------------ modèles

let models = store.get('freeModels', {}); // { service: [ids] }

function listModels(svc = service()) {
  return new Promise((resolve, reject) => {
    let buf = '';
    http.requestFree(svc, 'GET', SERVICES[svc].base + 'models', null, {
      onChunk: (d) => { buf += d; },
      onDone: () => {
        try {
          const ids = (JSON.parse(buf).data || []).map((m) => String(m.id).replace(/^models\//, ''));
          models[svc] = ids;
          store.set('freeModels', models);
          resolve(ids);
        } catch { reject(new Error('Liste des modèles illisible.')); }
      },
      onError: (status, body) => reject(new Error(freeErrorText(status, body))),
    });
  });
}

// Modèle « automatique » : le meilleur modèle de discussion de la liste du service.
function autoModel(svc = service()) {
  const ids = models[svc] || [];
  const version = (id) => (id.match(/\d+(\.\d+)?/g) || ['0']).map(Number);
  const newest = (a, b) => {
    const va = version(a), vb = version(b);
    for (let i = 0; i < Math.max(va.length, vb.length); i++) if ((vb[i] || 0) !== (va[i] || 0)) return (vb[i] || 0) - (va[i] || 0);
    return 0;
  };
  if (svc === 'gemini') {
    const chat = ids.filter((id) => /^gemini/.test(id) && /flash/.test(id) && !/lite|image|tts|audio|live|embed|exp|preview/.test(id));
    if (chat.length) return chat.sort(newest)[0];
  } else if (svc === 'openai') {
    // Modèle de discussion « mini » le plus récent (sans variantes audio, image, recherche, versions datées).
    const chat = ids.filter((id) => /^gpt-\d/.test(id) && !/audio|realtime|image|tts|transcribe|search|codex|instruct|chat-latest|\d{4}-\d{2}-\d{2}/.test(id));
    const mini = chat.filter((id) => /-mini$/.test(id));
    if (mini.length) return mini.sort(newest)[0];
    if (chat.length) return chat.sort(newest)[0];
  } else {
    if (ids.includes('openai/gpt-oss-120b')) return 'openai/gpt-oss-120b';
    const chat = ids.filter((id) => !/whisper|tts|guard|playai|compound|distil|orpheus/.test(id));
    if (chat.length) return chat.sort(newest)[0];
  }
  return SERVICES[svc].fallbackModel;
}

const currentModel = () => settings.freeModel || autoModel();
const knownModels = (svc = service()) => models[svc] || [];

// ------------------------------------------------------------------ conversion

const toolsForOpenAI = () => TOOLS.map((t) => ({
  type: 'function',
  function: { name: t.name, description: t.description, parameters: stripSchema(t.input_schema) },
}));

// Gemini refuse additionalProperties dans les schémas : on le retire partout.
function stripSchema(s) {
  if (Array.isArray(s)) return s.map(stripSchema);
  if (!s || typeof s !== 'object') return s;
  const out = {};
  for (const [k, v] of Object.entries(s)) if (k !== 'additionalProperties') out[k] = stripSchema(v);
  return out;
}

function toOpenAI(messages) {
  const out = [{ role: 'system', content: buildSystem('free') }];
  for (const m of messages) {
    const content = Array.isArray(m.content) ? m.content : [];
    if (m.role === 'user') {
      for (const r of content.filter((c) => c.type === 'tool_result')) {
        out.push({ role: 'tool', tool_call_id: r.tool_use_id, content: typeof r.content === 'string' ? r.content : JSON.stringify(r.content) });
      }
      const text = content.filter((c) => c.type === 'text').map((c) => c.text).join('\n');
      if (text) out.push({ role: 'user', content: text });
    } else {
      const text = content.filter((c) => c.type === 'text').map((c) => stripActions(c.text, false).trim()).filter(Boolean).join('\n\n');
      const calls = content.filter((c) => c.type === 'tool_use')
        .map((c) => ({ id: c.id, type: 'function', function: { name: c.name, arguments: JSON.stringify(c.input || {}) } }));
      if (!text && !calls.length) continue;
      const msg = { role: 'assistant', content: text };
      if (calls.length) msg.tool_calls = calls;
      out.push(msg);
    }
  }
  return out;
}

// ------------------------------------------------------------------ requête

function freeStep() {
  Object.assign(turn, { blocks: [], buf: '', finish: null, calls: [], free: true });
  const svc = service();
  turn.via = { ...(turn.via || {}), name: SERVICES[svc].name, model: currentModel() };
  turn.reqId = http.requestFree(svc, 'POST', SERVICES[svc].base + 'chat/completions', {
    model: turn.via.model,
    stream: true,
    messages: toOpenAI(convo.messages),
    tools: toolsForOpenAI(),
  }, { onChunk: onFreeChunk, onDone: onFreeDone, onError: onFreeError });
  setStreaming(true);
}

function onFreeChunk(data) {
  if (!turn) return;
  turn.buf += data.replace(/\r\n/g, '\n');
  let i;
  while ((i = turn.buf.indexOf('\n')) >= 0) {
    const line = turn.buf.slice(0, i).trim();
    turn.buf = turn.buf.slice(i + 1);
    if (!line.startsWith('data:')) continue;
    const payload = line.slice(5).trim();
    if (!payload || payload === '[DONE]') continue;
    let ev;
    try { ev = JSON.parse(payload); } catch { continue; }
    const choice = ev.choices?.[0];
    if (!choice) continue;
    const d = choice.delta || {};
    if (d.content) {
      let t = turn.blocks.find((b) => b.type === 'text');
      if (!t) { t = { type: 'text', text: '' }; turn.blocks.unshift(t); }
      t.text += d.content;
    }
    for (const tc of d.tool_calls || []) {
      const k = tc.index ?? turn.calls.length;
      const call = (turn.calls[k] ||= { id: '', name: '', args: '' });
      if (tc.id) call.id = tc.id;
      if (tc.function?.name) call.name += tc.function.name;
      if (tc.function?.arguments) call.args += tc.function.arguments;
    }
    if (choice.finish_reason) turn.finish = choice.finish_reason;
  }
  scheduleLive();
}

// Blocs finaux au format de Claude (ids d'outils compatibles avec l'API Anthropic).
function finalBlocks() {
  const out = [];
  const text = turn.blocks.find((b) => b.type === 'text')?.text?.trim();
  if (text) out.push({ type: 'text', text });
  turn.calls.filter(Boolean).forEach((c, k) => {
    let input = {};
    try { input = c.args.trim() ? JSON.parse(c.args) : {}; } catch { /* arguments illisibles : outil appelé sans */ }
    const id = /^[A-Za-z0-9_-]+$/.test(c.id) ? c.id : `toolu_free_${Date.now().toString(36)}_${k}`;
    out.push({ type: 'tool_use', id, name: c.name, input });
  });
  return out;
}

async function onFreeDone(cancelled) {
  if (!turn) return;
  const t = turn;
  const blocks = finalBlocks();
  if (cancelled) {
    if (blocks.some((b) => b.type === 'text')) { pushAssistant(blocks.filter((b) => b.type === 'text')); endTurn(); }
    else rollback(null);
    return;
  }
  if (!blocks.length) { rollback('Le modèle gratuit a renvoyé une réponse vide, réessaie.'); return; }
  pushAssistant(blocks);
  const uses = blocks.filter((b) => b.type === 'tool_use');
  if (uses.length && t.loops < 8) {
    t.blocks = [];
    renderChat();
    const results = [];
    for (const b of uses) {
      const r = { type: 'tool_result', tool_use_id: b.id };
      try { r.content = await execTool(b.name, b.input, b.id); } catch (err) { r.content = String(err.message || err); r.is_error = true; }
      results.push(r);
    }
    if (turn !== t) return; // annulé entre-temps
    convo.messages.push({ role: 'user', content: results });
    t.loops++;
    store.set('convo', convo);
    renderChat();
    freeStep();
    return;
  }
  if (t.finish === 'length') toast('Réponse tronquée (longueur maximale)');
  endTurn();
}

function pushAssistant(content) {
  const msg = { role: 'assistant', content };
  if (turn.via) msg.via = turn.via;
  convo.messages.push(msg);
}

function freeErrorText(status, body) {
  let detail = '';
  try {
    const j = JSON.parse(body);
    detail = (Array.isArray(j) ? j[0] : j)?.error?.message || '';
  } catch { detail = typeof body === 'string' ? body : ''; }
  const name = SERVICES[service()].name;
  if (status === -1) return `Ajoute une clé gratuite ${name} dans Réglages.`;
  if (status === -2 || status === 0) return detail || 'Erreur réseau.';
  if (status === 401 || status === 403) return `Clé ${name} invalide ou refusée.`;
  if (status === 429) return `Quota de ${name} atteint (ou crédit épuisé), réessaie un peu plus tard.`;
  if (status === 404) return `Modèle introuvable chez ${name} : choisis-en un autre dans Réglages.`;
  if (status >= 500) return `${name} est indisponible pour le moment, réessaie.`;
  return detail || `Erreur ${status}.`;
}

function onFreeError(status, body) {
  if (!turn) return;
  rollback(freeErrorText(status, body));
}

export { SERVICES, freeReady, freeStep, listModels, knownModels, autoModel, currentModel, freeErrorText, service as freeService };
