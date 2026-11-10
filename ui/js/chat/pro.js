// Mode « Abonnement Pro » : passe par Claude Code (`claude -p`, voir src/ai/claude_code.cpp).
import { uid } from '../core/util.js';
import { bridge, send, http } from '../core/bridge.js';
import { settings } from '../core/settings.js';
import { convo, turn, handleEvent, rollback, endTurn, setStreaming, renderChat, scheduleLive } from './chat.js';
import { ACTION_RE, stripActions, buildSystem } from './prompts.js';
import { execTool } from './tools.js';
import { parseDuration } from '../features/timer.js';
import { onStreamError } from './api.js';

// ---------------------------------------------------------- Abonnement Pro

function ccStep(prompt) {
  Object.assign(turn, { blocks: [], stop: null, error: null, buf: '', ccResult: null, ccSession: null, sawStream: false });
  const id = `c${++http.seq}_${Date.now()}`;
  const clean = (s) => String(s).replace(/\x1f/g, ' ');
  http.pending.set(id, { onChunk: onCcChunk, onDone: onCcDone, onError: onStreamError });
  turn.reqId = id;
  if (!bridge) {
    setTimeout(() => http.onError(id, -2, "Aperçu navigateur : Claude Code n'est disponible que dans l'application."), 300);
  } else {
    // Champ « outils » : recherche web et lecture des captures d'écran seulement (le natif ne connaît que « web » et « screen »).
    const tools = [settings.webSearch && 'web', settings.screenAccess && 'screen'].filter(Boolean).join(',');
    send('cc', [id, settings.ccModel, settings.effort, convo.ccSession || '', tools, buildSystem('pro'), prompt].map(clean).join('\x1f'));
  }
  setStreaming(true);
}

function onCcChunk(data) {
  if (!turn) return;
  turn.buf += data.replace(/\r\n/g, '\n');
  let i;
  while ((i = turn.buf.indexOf('\n')) >= 0) {
    const line = turn.buf.slice(0, i).trim();
    turn.buf = turn.buf.slice(i + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    if (msg.session_id) turn.ccSession = msg.session_id;
    if (msg.type === 'stream_event' && msg.event) {
      turn.sawStream = true;
      handleEvent(msg.event);
    } else if (msg.type === 'assistant' && !turn.sawStream && Array.isArray(msg.message?.content)) {
      msg.message.content.forEach((b) => turn.blocks.push({ ...b, _complete: true }));
    } else if (msg.type === 'result') {
      turn.ccResult = msg;
    }
  }
  scheduleLive();
}

function ccErrorText(res, info) {
  let text = (res && (res.result || (Array.isArray(res.errors) && res.errors.join(' ')))) || '';
  if (!text && info) {
    const lines = String(info.stderr || '').trim().split('\n').filter(Boolean);
    text = lines.slice(-3).join(' ') || `Claude Code s'est arrêté (code ${info.exit}).`;
  }
  if (/log ?in|auth|credential|token|connect/i.test(text)) {
    text = text.trim().replace(/([^.!?])$/, '$1.') + ' Clique sur « Se connecter » dans Réglages.';
  }
  return text;
}

async function onCcDone(cancelled, info) {
  if (!turn) return;
  const res = turn.ccResult;
  if (turn.ccSession) convo.ccSession = turn.ccSession;
  const blocks = turn.blocks.filter(Boolean)
    .filter((b) => (b.type === 'text' && b.text) || (b.type === 'thinking' && b.thinking))
    .map((b) => (b.type === 'text' ? { type: 'text', text: b.text } : { type: 'thinking', thinking: b.thinking, signature: '' }));
  if (cancelled) {
    if (blocks.some((b) => b.type === 'text' && b.text.trim())) await commitCcAnswer(blocks);
    else { rollback(null); return; }
    endTurn();
    return;
  }
  if (!res || res.is_error) { rollback(ccErrorText(res, info), true); return; } // quota, connexion… : secours gratuit
  if (!blocks.some((b) => b.type === 'text') && res.result) blocks.push({ type: 'text', text: res.result });
  if (!blocks.length) { rollback('Réponse vide, réessaie.'); return; }
  const next = await commitCcAnswer(blocks);
  if (next && turn.loops < 2) { // une seule capture ou lecture de notes par demande, plus une de reprise au plus
    turn.loops++;
    renderChat();
    ccStep(next);
    return;
  }
  endTurn();
}

// Une ligne d'action → appel d'outil équivalent au mode Clé API.
function actionToTool(kind, body) {
  const fields = body.split('|').map((s) => s.trim());
  if (kind === 'rappel') {
    const [when = '', repeat = 'none', ...rest] = fields;
    return ['create_reminder', { text: rest.join(' | '), when, repeat }];
  }
  if (kind.startsWith('t')) return ['create_todo', { text: body.trim(), important: kind.endsWith('!') }]; // tache, tâche, tache!
  if (kind === 'fait') return ['complete_todo', { text: body.trim() }];
  if (kind === 'chercher') {
    const [words = '', extension = '', from = '', to = ''] = fields;
    return ['search_files', { words, extension, from, to }];
  }
  if (kind === 'ouvrir') return ['open_file', { number: parseInt(body, 10) }];
  if (kind === 'site') return ['open_url', { url: body.trim() }];
  if (kind === 'ecran') return ['screenshot', {}];
  if (kind === 'revue') return ['read_notes', {}];
  if (kind === 'modif') { // numéro | raison | titre | contenu (qui peut contenir « | » ; « \n » = retour à la ligne)
    const [number = '', reason = '', title = '', ...text] = fields;
    return ['propose_note_edit', { note: parseInt(number, 10), reason, title, text: text.join(' | ').replace(/(\\)?\\n/g, (m, before) => (before ? m : '\n')) }]; // sans lookbehind (WebKitGTK ancien)
  }
  if (kind === 'mode' || kind === 'routine') { // routine : ancien nom de la ligne
    return /^(stop|arr[eêè]t(er)?|fin)$/i.test(body.trim()) ? ['stop_mode', {}] : ['start_mode', { name: body.trim() }];
  }
  if (kind === 'minuteur') {
    const what = body.trim().toLowerCase();
    const action = { chrono: 'stopwatch', pause: 'pause', reprendre: 'resume', stop: 'stop' }[what];
    return ['timer', action ? { action, seconds: 0 } : { action: 'countdown', seconds: parseDuration(what) / 1000 }];
  }
  return ['create_note', { text: body.trim() }];
}

// Enregistre la réponse et exécute ses lignes d'action (voir PRO_ACTIONS dans prompts.js).
async function commitCcAnswer(blocks) {
  const actions = [];
  for (const b of blocks) {
    if (b.type !== 'text') continue;
    for (const m of b.text.matchAll(ACTION_RE)) actions.push([m[1].toLowerCase(), m[2]]);
    b.text = stripActions(b.text, false).trim();
  }
  const content = blocks.filter((b) => b.type !== 'text' || b.text);
  const results = [];
  for (const [kind, body] of actions) {
    const id = 'cc_' + uid();
    const [name, input] = actionToTool(kind, body);
    content.push({ type: 'tool_use', id, name, input });
    const r = { type: 'tool_result', tool_use_id: id };
    try { r.content = await execTool(name, input, id); } catch (err) { r.content = String(err.message || err); r.is_error = true; }
    results.push(r);
  }
  convo.messages.push({ role: 'assistant', content });
  if (results.length) convo.messages.push({ role: 'user', content: results });
  // Capture prise : Claude Code ne voit l'image qu'en la lisant (Read), donc au tour suivant.
  const shot = results.map((r) => (r.is_error ? '' : String(r.content).match(/screens\/screen-\d+\.jpg/)?.[0] || '')).find(Boolean);
  if (shot) return `[Capture d'écran prise : ${shot}. Lis ce fichier avec l'outil Read, puis réponds à la demande de l'utilisateur. Ne prends pas d'autre capture.]`;
  // Notes lues : Claude Code ne les voit qu'au tour suivant, dans le message (voir [[revue|…]] dans prompts.js).
  const read = results.find((r) => !r.is_error && content.find((c) => c.id === r.tool_use_id)?.name === 'read_notes');
  if (read) return `[Notes de l'utilisateur lues par le dock :\n${read.content}\n]\nRéponds maintenant à la demande de l'utilisateur d'après ces notes. Pour proposer une modification, utilise [[modif|…]]. N'utilise plus [[revue|…]].`;
  return '';
}

export { ccStep };
