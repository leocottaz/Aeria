// Aujourd'hui : vue d'ensemble (rappels, tâches, minuteur ; la musique s'ouvre par la pilule de la barre). Aucun état propre : tout vient
// des autres modules, et chaque action passe par eux. Le HTML n'est refait que quand il change ;
// les valeurs qui bougent chaque seconde (compte à rebours, « dans 5 min ») sont posées à part
// (`data-live`), pour ne pas détruire un champ ou un clic en cours.
import { $, $$, esc, icon, toast } from '../core/util.js';
import { S } from '../core/state.js';
import { fmtTime, fmtIn, fmtWhen, startOfDay } from '../core/format.js';
import { reminders, upcomingReminders, renderReminders, completeReminder, snoozeReminder, dismissAlert } from './reminders.js';
import { openTodos, addTodo, setTodoDone } from './todo.js';
import { timerCard, toggleTimer, stopRinging, resetTimer } from './timer.js';
import { openTab } from '../dock/dock.js';

const MAX_ROWS = 4;
let renderedKey = null;

const greeting = (h) => (h < 5 ? 'Bonne nuit' : h < 12 ? 'Bonjour' : h < 18 ? 'Bon après-midi' : 'Bonsoir');

function head(tab, ico, title, count = '') {
  return `<div class="td-head"><button type="button" data-open="${tab}">${icon(ico)}<span>${title}</span>${icon('chevron')}</button>${count !== '' ? `<span class="td-count">${count}</span>` : ''}</div>`;
}

// Rappels : ceux qui ont sonné d'abord, puis ceux d'aujourd'hui (à défaut, le prochain).
function remindersCard() {
  const pending = reminders.filter((r) => !r.done && r.firedAt).sort((a, b) => b.firedAt - a.firedAt);
  const upcoming = upcomingReminders();
  const endOfDay = startOfDay(new Date()).getTime() + 864e5;
  const today = upcoming.filter((r) => r.due < endOfDay);
  const rows = [...pending, ...(today.length ? today : upcoming.slice(0, 1))];
  const body = rows.slice(0, MAX_ROWS).map((r) => {
    if (r.firedAt) {
      return `<div class="td-row pending" data-id="${r.id}"><span class="td-ico">${icon('bell')}</span>
        <div class="td-body"><b>${esc(r.text)}</b><small><span class="in">C'est l'heure</span> · ${fmtTime(new Date(r.due))}</small></div>
        <div class="td-actions"><button type="button" class="icon-btn snooze" data-act="rem-snooze" title="Reporter de 10 minutes">+10 min</button>
        <button type="button" class="icon-btn ok" data-act="rem-done" title="Marquer comme fait">${icon('check')}</button></div></div>`;
    }
    const when = r.due < endOfDay ? fmtTime(new Date(r.due)) : fmtWhen(r.due);
    return `<div class="td-row" data-id="${r.id}"><span class="td-ico">${icon('clock')}</span>
      <div class="td-body"><b>${esc(r.text)}</b><small><span class="in" data-live="in" data-due="${r.due}"></span> · ${when}</small></div>
      <div class="td-actions"><button type="button" class="icon-btn ok" data-act="rem-done" title="Marquer comme fait">${icon('check')}</button></div></div>`;
  }).join('');
  const more = rows.length - MAX_ROWS;
  return `<section class="glass td-card">${head('reminders', 'bell', 'Rappels', pending.length + today.length || '')}
    ${body || '<div class="td-empty">Rien de prévu aujourd’hui</div>'}
    ${more > 0 ? `<button type="button" class="td-more" data-open="reminders">+ ${more} autre${more > 1 ? 's' : ''}</button>` : ''}</section>`;
}

// Tâches : les importantes d'abord (openTodos les trie ainsi), avec un champ pour en ajouter une.
function todosCard() {
  const open = openTodos();
  const body = open.slice(0, MAX_ROWS).map((t) => `<div class="td-row" data-id="${t.id}">
      <button type="button" class="todo-check" data-act="todo-done" title="Fait">${icon('check')}</button>
      <div class="td-body"><b class="td-title">${esc(t.text)}</b></div>${t.star ? icon('star', 'td-star') : ''}</div>`).join('');
  const more = open.length - MAX_ROWS;
  return `<section class="glass td-card">${head('todo', 'todo', 'À faire', open.length || '')}
    ${body || '<div class="td-empty">Tout est fait ✨</div>'}
    ${more > 0 ? `<button type="button" class="td-more" data-open="todo">+ ${more} autre${more > 1 ? 's' : ''}</button>` : ''}
    <form class="td-add" id="tdAdd" autocomplete="off">${icon('plus')}<input id="tdAddInput" placeholder="Ajouter une tâche…" spellcheck="false" maxlength="200"></form></section>`;
}

function timerHTML() {
  const t = timerCard();
  const sub = { idle: 'Prêt', running: 'En cours', paused: 'En pause', ringing: 'Temps écoulé' }[t.state];
  const main = t.state === 'ringing'
    ? '<button type="button" class="td-round" data-act="timer-ok" title="Arrêter la sonnerie">OK</button>'
    : `<button type="button" class="td-round" data-act="timer-toggle" title="${t.state === 'running' ? 'Pause' : 'Démarrer'}">${icon(t.state === 'running' ? 'pause' : 'play')}</button>`;
  const reset = t.state === 'paused' ? `<button type="button" class="icon-btn" data-act="timer-reset" title="Réinitialiser">${icon('reset')}</button>` : '';
  return `<section class="glass td-card${t.state === 'ringing' ? ' ringing' : ''}">${head('timer', 'timer', 'Minuteur')}
    <div class="td-timer"><div class="td-clock"><div class="td-time" data-live="timer"></div><div class="td-sub">${sub}</div></div>${reset}${main}</div>
    <div class="td-bar"><span data-live="bar"></span></div></section>`;
}

// Valeurs qui changent seule, sans refaire le HTML.
function updateLive() {
  const now = Date.now();
  $$('[data-live="in"]').forEach((el) => { el.textContent = fmtIn(+el.dataset.due - now); });
  const time = $('[data-live="timer"]');
  if (time) {
    const t = timerCard();
    time.textContent = t.text;
    $('[data-live="bar"]').style.setProperty('--p', t.p.toFixed(4));
  }
}

function renderToday() {
  const grid = $('#todayGrid');
  const now = new Date();
  const date = now.toLocaleDateString('fr-FR', { weekday: 'long', day: 'numeric', month: 'long' });
  $('#todaySummary').textContent = `${greeting(now.getHours())} · ${date}`;

  const html = `<div class="td-col">${remindersCard()}${todosCard()}</div><div class="td-col">${timerHTML()}</div>`;
  if (html !== renderedKey) {
    renderedKey = html;
    const input = $('#tdAddInput');
    const keep = input && { value: input.value, focus: document.activeElement === input };
    grid.innerHTML = html;
    if (keep?.value || keep?.focus) {
      const next = $('#tdAddInput');
      next.value = keep.value;
      if (keep.focus) next.focus();
    }
  }
  updateLive();
}

const visible = () => S.st === 'panel' && S.tab === 'today';

function setupToday() {
  $('#panel').addEventListener('click', (e) => { if (e.target.closest('[data-back]')) openTab('today'); });
  const grid = $('#todayGrid');
  grid.addEventListener('click', (e) => {
    const open = e.target.closest('[data-open]');
    if (open) { openTab(open.dataset.open); return; }
    const btn = e.target.closest('[data-act]');
    if (!btn) return;
    const id = btn.closest('.td-row')?.dataset.id;
    switch (btn.dataset.act) {
      case 'rem-done': completeReminder(id); dismissAlert(id); renderReminders(); break;
      case 'rem-snooze': snoozeReminder(id); dismissAlert(id); renderReminders(); toast('Reporté de 10 minutes'); break;
      case 'timer-toggle': toggleTimer(); break;
      case 'timer-ok': stopRinging(); break;
      case 'timer-reset': resetTimer(); break;
      case 'todo-done': {
        // La coche se dessine, puis la tâche quitte la liste (comme dans l'onglet To-do).
        const row = btn.closest('.td-row');
        row.classList.add('checking');
        setTimeout(() => { setTodoDone(id, true); if (visible()) renderToday(); }, document.body.classList.contains('reduce') ? 0 : 380);
        return;
      }
      default: return;
    }
    renderToday();
  });
  grid.addEventListener('submit', (e) => {
    e.preventDefault();
    const input = $('#tdAddInput');
    if (!addTodo(input.value)) return;
    input.value = '';
    renderToday();
    $('#tdAddInput').focus();
  });
}

export { setupToday, renderToday };
