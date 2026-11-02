// États du dock (masqué, languette, barre, panneau), onglets, horloge, barre et raccourcis.
import { $, $$, store } from '../core/util.js';
import { send } from '../core/bridge.js';
import { settings } from '../core/settings.js';
import { S } from '../core/state.js';
import { fmtTime } from '../core/format.js';
import { reminders, upcomingReminders, renderReminders, hasPendingReminders } from '../features/reminders.js';
import { scrollChat, focusComposer, sendChat } from '../chat/chat.js';
import { resumeMusicLoop } from '../features/music.js';
import { setMiniAlerts, miniTab } from './mini.js';
import { renderTimer, timerLabel, timerRinging } from '../features/timer.js';

function setState(st, info = {}) {
  const prev = S.st;
  S.st = st;
  S.pinned = !!info.pinned;
  const b = document.body;
  b.classList.remove('st-hidden', 'st-progress', 'st-peek', 'st-mini', 'st-music', 'st-bar', 'st-panel');
  b.classList.add('st-' + st);
  $('#pinBtn').classList.toggle('on', S.pinned);

  if (st === 'progress') startStrip(info.delay || 500);
  if (st === 'mini' || st === 'music') resumeMusicLoop(); // paroles du mini-dock, progression de la carte
  const open = (s) => s === 'bar' || s === 'panel';
  if (open(st) && !open(prev)) {
    tick();
    resumeMusicLoop();
    b.classList.remove('enter');
    void b.offsetWidth;
    b.classList.add('enter');
  }
  if (st === 'panel') {
    moveNav(false);
    if (S.tab === 'claude') scrollChat(true);
    if (S.tab === 'reminders') renderReminders();
    if (S.tab === 'timer') renderTimer();
  }
}

function startStrip(delay) {
  const fill = $('#stripFill');
  fill.style.transition = 'none';
  fill.style.width = '0';
  void fill.offsetWidth;
  fill.style.transition = `width ${delay}ms cubic-bezier(.4, 0, .6, 1)`;
  fill.style.width = '100%';
}

// Onglets de la barre, dans l'ordre (Ctrl+1…6). Claude n'y a pas de bouton : on y accède par
// l'orbe, le champ « Demander » et Ctrl+Alt+Espace.
const NAV_TABS = ['reminders', 'todo', 'music', 'notes', 'timer', 'settings'];
const TABS = [...NAV_TABS, 'claude'];

function switchTab(tab) {
  if (!TABS.includes(tab)) return;
  const changed = tab !== S.tab;
  S.tab = tab;
  settings.lastTab = tab;
  store.set('settings', settings);
  $$('.view').forEach((v) => v.classList.toggle('active', v.dataset.view === tab));
  $$('#nav button').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
  moveNav(changed);
  if (tab === 'claude') scrollChat(true);
  if (tab === 'reminders') renderReminders();
  if (tab === 'timer') renderTimer();
}

function moveNav(stretch) {
  const ind = $('#navIndicator');
  const i = NAV_TABS.indexOf(S.tab);
  ind.classList.toggle('off', i < 0); // onglet sans bouton (Claude)
  if (i < 0) return;
  ind.style.setProperty('--i', i);
  if (stretch && !settings.reduceMotion) {
    ind.classList.remove('stretch');
    void ind.offsetWidth;
    ind.classList.add('stretch');
  }
}

function openTab(tab) {
  switchTab(tab);
  if (S.st !== 'panel') send('open');
}

function tick() {
  const now = new Date();
  $('#clockTime').textContent = fmtTime(now);
  const sub = $('#clockSub');
  const pending = reminders.filter((r) => !r.done && r.firedAt);
  const next = upcomingReminders()[0];
  const timer = timerLabel();
  if (pending.length) {
    sub.textContent = pending.length === 1 ? `⏰ ${pending[0].text}` : `⏰ ${pending.length} rappels en attente`;
    sub.classList.add('hot');
  } else if (timer) {
    sub.textContent = timer;
    sub.classList.toggle('hot', timerRinging());
  } else if (next && next.due - now < 24 * 3600e3) {
    sub.textContent = `⏰ ${fmtTime(new Date(next.due))} · ${next.text}`;
    sub.classList.remove('hot');
  } else {
    const date = now.toLocaleDateString('fr-FR', { weekday: 'long', day: 'numeric', month: 'long' });
    sub.textContent = date.charAt(0).toUpperCase() + date.slice(1);
    sub.classList.remove('hot');
  }
  const badge = $('#orbBadge');
  badge.hidden = !pending.length;
  badge.textContent = pending.length;
  $('#remDot').hidden = !pending.length;
  $('#timerDot').hidden = !timerRinging();
  setMiniAlerts(pending.length + (timerRinging() ? 1 : 0)); // cloche du mini-dock quand le dock est rangé
}

// Onglet qui attend l'utilisateur : rappels qui ont sonné d'abord, puis minuteur terminé.
const alertTab = () => (hasPendingReminders() ? 'reminders' : timerRinging() ? 'timer' : null);

function setupBar() {
  $('#nav').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-tab]');
    if (!b) return;
    if (S.st === 'panel' && S.tab === b.dataset.tab) { send('bar'); return; }
    openTab(b.dataset.tab);
  });
  $('#pinBtn').addEventListener('click', () => send('pin', S.pinned ? '0' : '1'));
  $('#askForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = $('#askInput').value.trim();
    if (!text) { openTab('claude'); focusComposer(); return; }
    $('#askInput').value = '';
    openTab('claude');
    sendChat(text);
  });
  $('#orb').addEventListener('click', () => openTab(alertTab() || 'claude'));
  $('#npOpen').addEventListener('click', () => openTab('music'));
  $('#mcardOpen').addEventListener('click', () => openTab('music'));
  $('#pill').addEventListener('click', (e) => openTab(e.target.closest('#pillTimer') ? 'timer' : alertTab() || miniTab()));
}

// Linux : la molette arrive du natif ({t:"wheel"}, dock_linux.cpp), en crans. On déplace la zone
// défilante la plus proche de la souris qui a vraiment quelque chose à faire défiler.
function onWheel({ x, y, dy }) {
  for (let el = document.elementFromPoint(x, y); el && el !== document.body; el = el.parentElement) {
    const overflow = getComputedStyle(el).overflowY;
    if ((overflow === 'auto' || overflow === 'scroll') && el.scrollHeight > el.clientHeight) {
      el.scrollTop += dy * 40;
      return;
    }
  }
}

function setupGlobal() {
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') { send('hide'); return; }
    if (e.ctrlKey && !e.altKey && /^[1-6]$/.test(e.key)) { e.preventDefault(); openTab(NAV_TABS[+e.key - 1]); }
  });
  // reflet spéculaire qui suit la souris sur les surfaces de verre
  document.addEventListener('pointermove', (e) => {
    const g = e.target.closest?.('.glass');
    if (!g) return;
    const r = g.getBoundingClientRect();
    g.style.setProperty('--mx', `${e.clientX - r.left}px`);
    g.style.setProperty('--my', `${e.clientY - r.top}px`);
  }, { passive: true });
  document.addEventListener('contextmenu', (e) => {
    if (!e.target.closest('input, textarea, .md, .bubble')) e.preventDefault();
  });
}

export { setState, switchTab, openTab, TABS, tick, setupBar, setupGlobal, onWheel };
