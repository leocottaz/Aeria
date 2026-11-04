// Conversation : état, envoi, évènements de streaming communs, rendu et saisie.
// Le transport dépend du mode : chat/api.js (clé API) ou chat/pro.js (Claude Code).
import { $, uid, esc, icon, store, toast, autosize } from '../core/util.js';
import { bridge, send } from '../core/bridge.js';
import { settings, openSettingsPage } from '../core/settings.js';
import { S } from '../core/state.js';
import { fmtWhen } from '../core/format.js';
import { renderKey, updateNoKey } from './connection.js';
import { switchTab, openTab } from '../dock/dock.js';
import { contextBlock, remindersContext, todosContext, stripActions } from './prompts.js';
import { foundFor, foundContext, openPath, folderOf, fmtDay } from '../features/files.js';
import { md } from './markdown.js';
import { streamStep } from './api.js';
import { ccStep } from './pro.js';
import { freeStep, freeReady, freeService } from './free.js';
import { setupAttach, attachments, clearAttachments, hasAttachments, restoreAttachments, unsupported, attachmentBlocks, forStorage, sentChipsHTML } from './attach.js';

// Le prompt système n'est pas gardé dans la conversation : il est recalculé à chaque message
// (buildSystem), pour qu'une ancienne conversation connaisse les capacités ajoutées depuis.
const newConvo = () => ({ id: uid(), backend: settings.backend, messages: [], created: Date.now() });

let convo = store.get('convo', null) || newConvo();
// Images et PDF ne sont pas gardés dans le stockage local (voir forStorage dans attach.js).
// Les résultats de recherche de l'API (contenu chiffré, volumineux) et les citations ne sont pas gardés non plus.
const slim = (m) => (m.role === 'assistant' && m.content.some((c) => c.type === 'server_tool_use' || c.type === 'web_search_tool_result' || c.citations)
  ? { ...m, content: m.content.filter((c) => c.type !== 'server_tool_use' && c.type !== 'web_search_tool_result').map(({ citations, ...c }) => c) }
  : m);
const saveConvo = () => store.set('convo', { ...convo, messages: convo.messages.map((m) => forStorage(slim(m))) });
let turn = null;      // tour en cours : { reqId, blocks, stop, startLen, userText, loops, ... }
let chatNotice = null;

// La pilule de la commande vocale (js/features/pill.js) suit la réponse : texte de l'assistant depuis le dernier
// message de l'utilisateur (blocs déjà reçus + tour en cours), sans les balises d'actions.
let progressHook = null;
const onChatProgress = (fn) => { progressHook = fn; };
const isBusy = () => !!turn;
function answerText() {
  let start = 0;
  convo.messages.forEach((m, i) => { if (m.role === 'user' && m.content.some((c) => c.type === 'text')) start = i + 1; });
  const blocks = convo.messages.slice(start).filter((m) => m.role === 'assistant').flatMap((m) => m.content);
  if (turn) blocks.push(...turn.blocks);
  return stripActions(blocks.filter((b) => b.type === 'text').map((b) => b.text || '').join('\n\n'), !!turn).trim();
}

// Secours gratuit possible : activé dans les réglages, clé du service enregistrée, et Claude choisi.
const canFallback = () => settings.category === 'claude' && settings.freeFallback && freeReady();

function sendChat(typed) {
  const files = attachments();
  const text = typed.trim() || (files.length ? `Regarde ${files.length > 1 ? 'ces fichiers' : 'ce fichier'}.` : '');
  if (!text || turn) return;
  const backend = settings.backend;
  const pro = backend === 'pro';
  let fallback = ''; // raison du passage au modèle gratuit avant même d'essayer Claude
  const direct = backend === 'free' || backend === 'chatgpt'; // service compatible OpenAI choisi comme connexion
  if (direct && !freeReady()) { updateNoKey(); toast('Ajoute d’abord une clé'); return; }
  if (bridge && pro && S.cc.checked && !S.cc.found) {
    if (!canFallback()) { updateNoKey(); toast('Installe d’abord Claude Code'); return; }
    fallback = 'Claude Code n’est pas installé';
  }
  if (bridge && backend === 'api' && !S.hasKey) {
    if (!canFallback()) { renderKey(); toast('Ajoute d’abord ta clé API'); return; }
    fallback = 'aucune clé API';
  }
  if ((convo.backend || 'api') !== backend) {
    if (convo.messages.length) toast('Nouvelle conversation (mode de connexion changé)');
    convo = newConvo();
  }
  const route = fallback || direct ? freeService() : pro ? 'pro' : 'api';
  const blocked = unsupported(files, route);
  if (blocked) { toast(blocked); return; }
  chatNotice = null;
  clearAttachments();
  const startLen = convo.messages.length;
  const ctx = contextBlock();
  const fileBlocks = attachmentBlocks(files);
  const msg = { role: 'user', content: [{ type: 'text', text: ctx }, ...fileBlocks, { type: 'text', text }] }; // le texte de l'utilisateur reste en dernier
  if (files.length) msg.files = files.map(({ name, kind, size }) => ({ name, kind, size }));
  convo.messages.push(msg);
  turn = { startLen, userText: typed.trim(), files, loops: 0, blocks: [], via: fallback ? { reason: fallback } : null };
  renderChat();
  scrollChat(true);
  if (fallback || direct) freeStep();
  else if (pro) {
    const attached = fileBlocks.map((b) => b.text).join('\n\n'); // en mode Pro, seulement du texte (voir unsupported)
    ccStep([ctx, remindersContext(), todosContext(), foundContext()].filter(Boolean).join('\n') + `\n\n${attached ? `${attached}\n\n` : ''}${text}`);
  } else streamStep();
}

// Claude a échoué (quota, connexion, clé…) : on rejoue le message avec le modèle gratuit.
function tryFallback(reason) {
  if (!turn || turn.free || !canFallback()) return false;
  convo.messages.length = turn.startLen + 1; // garde seulement le message de l'utilisateur
  chatNotice = null;
  Object.assign(turn, { loops: 0, via: { reason: reason || 'erreur' } });
  renderChat();
  freeStep();
  return true;
}

function handleEvent(ev) {
  const blocks = turn.blocks;
  const idx = (turn.base || 0) + (ev.index ?? 0);
  switch (ev.type) {
    case 'message_start':
      turn.base = blocks.length;
      break;
    case 'content_block_start': {
      const b = structuredClone(ev.content_block);
      if (b.type === 'tool_use' || b.type === 'server_tool_use') b._json = '';
      blocks[idx] = b;
      break;
    }
    case 'content_block_delta': {
      const b = blocks[idx];
      const d = ev.delta;
      if (!b) break;
      if (d.type === 'text_delta') b.text = (b.text || '') + d.text;
      else if (d.type === 'thinking_delta') b.thinking = (b.thinking || '') + d.thinking;
      else if (d.type === 'signature_delta') b.signature = (b.signature || '') + d.signature;
      else if (d.type === 'input_json_delta') b._json += d.partial_json;
      else if (d.type === 'citations_delta') (b.citations ||= []).push(d.citation);
      break;
    }
    case 'content_block_stop': {
      const b = blocks[idx];
      if (b?.type === 'tool_use' || b?.type === 'server_tool_use') {
        try { b.input = b._json.trim() ? JSON.parse(b._json) : {}; } catch { b.input = {}; b._invalid = true; }
      }
      if (b) b._complete = true;
      break;
    }
    case 'message_delta':
      turn.stop = ev.delta?.stop_reason ?? turn.stop;
      turn.stopDetails = ev.delta?.stop_details ?? turn.stopDetails;
      break;
    case 'error':
      turn.error = ev.error;
      break;
    default:
      break;
  }
}

// Annule le tour : l'historique revient à son état d'avant, le texte retourne dans la saisie.
// fallback : l'échec vient de Claude lui-même, le modèle gratuit peut prendre le relais.
function rollback(message, fallback = false) {
  if (fallback && tryFallback(message)) return;
  convo.messages.length = turn.startLen;
  const input = $('#chatInput');
  if (!input.value.trim()) { input.value = turn.userText; autosize(input, 140); }
  restoreAttachments(turn.files);
  chatNotice = message;
  endTurn();
}

function endTurn() {
  turn = null;
  setStreaming(false);
  saveConvo();
  renderChat();
  scrollChat(false);
  progressHook?.({ text: answerText(), done: true, error: chatNotice });
}

function setStreaming(on) {
  $('#chatForm').classList.toggle('streaming', on);
  $('#orb').classList.toggle('thinking', on);
  $('#sendBtn').title = on ? 'Arrêter' : 'Envoyer (Entrée)';
}

function thinkingHTML(b, active) {
  const txt = (b.thinking || '').trim();
  if (active) {
    const last = txt.split('\n').filter(Boolean).pop() || '';
    return `<div class="think active"><span class="think-head"><span class="think-dot"></span><span class="shimmer">Réflexion…</span></span>${last ? `<span class="think-live">${esc(last)}</span>` : ''}</div>`;
  }
  if (!txt) return '';
  return `<details class="think"><summary>${icon('sparkles')}Raisonnement${icon('chevron')}</summary><div class="think-body md">${md(txt)}</div></details>`;
}

function toolHTML(b, res, live) {
  const inp = b.input || {};
  const err = res?.is_error;
  const state = (okLabel, pendingLabel, errLabel) => (res ? (err ? errLabel : okLabel) : pendingLabel);
  let ic = 'bell', title, detail = '';
  if (b.name === 'create_reminder') {
    title = state('Rappel créé', 'Création du rappel…', 'Rappel non créé');
    const due = inp.when ? new Date(inp.when).getTime() : NaN;
    detail = [inp.text, Number.isFinite(due) ? fmtWhen(due) : ''].filter(Boolean).join(' · ');
  } else if (b.name === 'create_note') {
    ic = 'note';
    title = state('Note ajoutée', 'Ajout de la note…', 'Note non ajoutée');
    detail = inp.text || '';
  } else if (b.name === 'create_todo') {
    ic = 'todo';
    title = state('Tâche ajoutée', 'Ajout de la tâche…', 'Tâche non ajoutée');
    detail = (inp.important ? '★ ' : '') + (inp.text || '');
  } else if (b.name === 'complete_todo') {
    ic = 'check';
    title = state('Tâche cochée', 'Coche de la tâche…', 'Tâche non cochée');
    detail = inp.text || '';
  } else if (b.name === 'list_todos') {
    ic = 'todo';
    title = state('To-do consultée', 'Consultation de la to-do…', 'Consultation impossible');
  } else if (b.name === 'search_files') {
    ic = 'search';
    const files = res && !err ? foundFor(b.id) : null;
    title = state(files?.length ? `${files.length} fichier${files.length > 1 ? 's' : ''} trouvé${files.length > 1 ? 's' : ''}` : 'Aucun fichier trouvé',
      'Recherche de fichiers…', 'Recherche impossible');
    detail = [inp.words, inp.extension && `.${inp.extension}`, inp.from && `du ${inp.from}`, inp.to && `au ${inp.to}`].filter(Boolean).join(' · ');
    if (files?.length) return foundHTML(title, detail, files);
  } else if (b.name === 'open_file') {
    ic = 'file';
    title = state('Fichier ouvert', 'Ouverture…', 'Fichier non ouvert');
    detail = inp.number ? `n° ${inp.number}` : '';
  } else if (b.name === 'open_url') {
    ic = 'globe';
    title = state('Page ouverte', 'Ouverture de la page…', 'Page non ouverte');
    detail = inp.url || '';
  } else if (b.name === 'run_routine') {
    ic = 'routine';
    title = state('Routine lancée', 'Lancement de la routine…', 'Routine non lancée');
    detail = inp.name || '';
  } else {
    ic = 'clock';
    title = state('Rappels consultés', 'Consultation des rappels…', 'Consultation impossible');
  }
  if (err) detail = typeof res.content === 'string' ? res.content : detail;
  if (live && !res && b.type === 'tool_use' && !b._complete) detail = '';
  return `<div class="tool ${err ? 'err' : ''}">${icon(ic)}<div><b>${title}</b>${detail ? `<span>${esc(detail)}</span>` : ''}</div>${res && !err ? icon('check', 'ok') : ''}</div>`;
}

// Résultats d'une recherche de fichiers : liste numérotée, un clic ouvre le fichier.
function foundHTML(title, detail, files) {
  const rows = files.map((f, i) => `<button type="button" class="found-row" data-open="${esc(f.path)}" title="${esc(f.path)}">
      <span class="found-n">${i + 1}</span>
      <span class="found-name">${esc(f.name)}</span>
      <span class="found-meta">${esc(folderOf(f.path))} · ${esc(fmtDay(f.modified))}</span>
    </button>`).join('');
  return `<div class="tool found">${icon('search')}<div><b>${esc(title)}</b>${detail ? `<span>${esc(detail)}</span>` : ''}
    <div class="found-list">${rows}</div></div></div>`;
}

// Recherche sur internet : outil de Claude Code (tool_use « WebSearch », mode Pro) ou outil serveur de l'API
// (server_tool_use + web_search_tool_result). Ni l'un ni l'autre ne passe par execTool.
function searchHTML(b, blocks, live) {
  const result = blocks.find((x) => x.type === 'web_search_tool_result' && x.tool_use_id === b.id);
  const failed = result && !Array.isArray(result.content);
  const done = !live || !!result || (b.type === 'tool_use' && blocks.indexOf(b) < blocks.length - 1); // Claude Code n'envoie pas le résultat : la suite le prouve
  const n = Array.isArray(result?.content) ? result.content.length : 0;
  const title = failed ? 'Recherche impossible' : done ? (n ? `Recherche sur internet · ${n} résultat${n > 1 ? 's' : ''}` : 'Recherche sur internet') : 'Recherche sur internet…';
  const query = b.input?.query || '';
  return `<div class="tool ${failed ? 'err' : ''}">${icon('globe')}<div><b>${title}</b>${query ? `<span>${esc(query)}</span>` : ''}</div>${done && !failed ? icon('check', 'ok') : ''}</div>`;
}

function groupBlocksHTML(blocks, results, live) {
  let html = '';
  const visible = blocks.filter(Boolean);
  const last = visible[visible.length - 1];
  let hasText = false;
  for (const b of visible) {
    if (b.type === 'thinking') html += thinkingHTML(b, live && b === last && !b._complete);
    else if (b.type === 'text') {
      const shown = stripActions(b.text || '', live && b === last);
      if (!shown.trim()) continue;
      hasText = true;
      html += `<div class="md${live && b === last ? ' streaming' : ''}">${md(shown)}</div>`;
    } else if (b.type === 'server_tool_use' || (b.type === 'tool_use' && b.name === 'WebSearch')) {
      html += searchHTML(b, visible, live);
    } else if (b.type === 'tool_use') html += toolHTML(b, results[b.id], live);
    else if (b.type === 'fallback') html += `<div class="note-fallback">Réponse poursuivie par ${esc(b.to?.model || 'un autre modèle')}</div>`;
  }
  const busy = live && (!last || (last._complete && last.type !== 'tool_use') || (last.type === 'tool_use' && results[last.id]));
  if (live && (!visible.length || busy) && !(last && last.type === 'thinking' && !last._complete)) {
    html += '<div class="typing"><span></span><span></span><span></span></div>';
  }
  if (!live && hasText) html += `<div class="msg-actions"><button type="button" data-act="copy">${icon('copy')}Copier</button></div>`;
  return html;
}

// Réponse de secours : on dit pourquoi Claude n'a pas répondu et quel modèle l'a fait.
function viaHTML(via) {
  if (!via?.reason) return '';
  const who = via.name ? `${via.name}${via.model ? ` (${via.model})` : ''}` : 'le modèle gratuit';
  const reason = String(via.reason).trim().replace(/[.\s]+$/, '');
  return `<div class="via-note">${icon('sparkles')}<span>Claude indisponible (${esc(reason)}) : réponse de ${esc(who)}</span></div>`;
}

let liveCommitted = [];
let liveResults = {};

function renderChat() {
  const chat = $('#chat');
  const msgs = convo.messages;
  const results = {};
  msgs.forEach((m) => Array.isArray(m.content) && m.content.forEach((c) => { if (c.type === 'tool_result') results[c.tool_use_id] = c; }));

  const groups = [];
  let cur = null;
  for (const m of msgs) {
    if (m.role === 'user') {
      const texts = m.content.filter((c) => c.type === 'text');
      if (texts.length) { groups.push({ type: 'user', text: texts[texts.length - 1].text, files: m.files }); cur = null; }
      continue;
    }
    if (!cur) { cur = { type: 'assistant', blocks: [] }; groups.push(cur); }
    cur.blocks.push(...m.content);
    if (m.via) cur.via = m.via;
  }
  if (turn && groups[groups.length - 1]?.type !== 'assistant') groups.push({ type: 'assistant', blocks: [] });
  if (turn) {
    const g = groups[groups.length - 1];
    g.live = true;
    if (turn.via) g.via = turn.via;
  }

  if (!groups.length) {
    chat.innerHTML = `<div class="empty-chat">
      <div class="orb"><span class="orb-core"><span class="orb-spin"></span></span></div>
      <h3>Que puis-je faire pour toi ?</h3>
      <p>Pose une question, ou demande-moi de te rappeler quelque chose.</p>
      <div class="suggest">
        <button type="button" data-suggest>Rappelle-moi de faire une pause dans 45 min</button>
        <button type="button" data-suggest>Explique-moi un concept simplement</button>
        <button type="button" data-suggest>Aide-moi à rédiger un message</button>
      </div></div>`;
  } else {
    chat.innerHTML = groups.map((g) => {
      if (g.type === 'user') return `<div class="msg user"><div class="bubble">${g.files?.length ? sentChipsHTML(g.files) : ''}${esc(g.text)}</div></div>`;
      const live = !!g.live;
      if (live) { liveCommitted = g.blocks; liveResults = results; }
      const blocks = live ? [...g.blocks, ...turn.blocks] : g.blocks;
      return `<div class="msg assistant"${live ? ' id="live"' : ''}>
        <div class="orb"><span class="orb-core"><span class="orb-spin"></span></span></div>
        <div class="content">${viaHTML(g.via)}${groupBlocksHTML(blocks, results, live)}</div></div>`;
    }).join('');
  }
  if (chatNotice) chat.insertAdjacentHTML('beforeend', `<div class="notice"><b>Oups.</b> ${esc(chatNotice)}</div>`);
}

let liveFrame = 0;
function scheduleLive() {
  if (liveFrame) return;
  liveFrame = requestAnimationFrame(() => {
    liveFrame = 0;
    const el = $('#live .content');
    if (!el || !turn) return;
    const stick = isNearBottom();
    el.innerHTML = viaHTML(turn.via) + groupBlocksHTML([...liveCommitted, ...turn.blocks], liveResults, true);
    if (stick) scrollChat(false);
    progressHook?.({ text: answerText(), done: false });
  });
}

function isNearBottom() {
  const c = $('#chat');
  return c.scrollHeight - c.scrollTop - c.clientHeight < 60;
}

function scrollChat(instant) {
  const c = $('#chat');
  requestAnimationFrame(() => c.scrollTo({ top: c.scrollHeight, behavior: instant || settings.reduceMotion ? 'auto' : 'smooth' }));
}

function focusComposer() {
  setTimeout(() => $('#chatInput').focus(), 80);
}

function setupChat() {
  const input = $('#chatInput');
  input.addEventListener('input', () => autosize(input, 140));
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      $('#chatForm').requestSubmit();
    }
  });
  $('#chatForm').addEventListener('submit', (e) => {
    e.preventDefault();
    if (turn) { send('cancel', turn.reqId); return; }
    const text = input.value;
    if (!text.trim() && !hasAttachments()) return;
    input.value = '';
    autosize(input, 140);
    sendChat(text);
  });
  $('#newChat').addEventListener('click', () => {
    if (turn) return;
    convo = newConvo();
    chatNotice = null;
    saveConvo();
    renderChat();
    focusComposer();
  });
  $('#chat').addEventListener('click', (e) => {
    const sug = e.target.closest('[data-suggest]');
    if (sug) { sendChat(sug.textContent); return; }
    const file = e.target.closest('[data-open]');
    if (file) { openPath(file.dataset.open); return; }
    const link = e.target.closest('a[href]');
    if (link) { e.preventDefault(); send('openurl', link.getAttribute('href').replace(/&amp;/g, '&')); return; }
    if (e.target.closest('[data-act="copy"]')) {
      const text = [...e.target.closest('.content').querySelectorAll(':scope > .md')].map((m) => m.innerText).join('\n\n');
      navigator.clipboard?.writeText(text).then(() => toast('Copié'), () => toast('Copie impossible'));
    }
  });
  setupAttach(() => { openTab('claude'); focusComposer(); });
  $('[data-goto="settings"]').addEventListener('click', () => { switchTab('settings'); openSettingsPage('aeria'); });
}

export {
  convo, turn, sendChat, onChatProgress, isBusy, saveConvo, handleEvent, rollback, endTurn, setStreaming,
  renderChat, scheduleLive, scrollChat, focusComposer, setupChat,
};
