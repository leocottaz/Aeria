// Rappels : liste, saisie en français courant, alertes. Le déclenchement à l'heure
// est fait par le natif (src/features/reminders.cpp), qui renvoie {"t":"fire"}.
import { $, $$, uid, esc, clamp, icon, store, toast, animateOut } from '../core/util.js';
import { send } from '../core/bridge.js';
import { fmtWhen, fmtIn } from '../core/format.js';
import { tick } from '../dock/dock.js';
import { showMiniNotice } from '../dock/mini.js';

let reminders = store.get('reminders', []);
const REPEAT_NAMES = { none: '', daily: 'chaque jour', weekdays: 'jours ouvrés', weekly: 'chaque semaine' };

const upcomingReminders = () => reminders.filter((r) => !r.done && !r.firedAt).sort((a, b) => a.due - b.due);

function saveReminders() {
  store.set('reminders', reminders);
  const lines = upcomingReminders().map((r) => [r.id, r.due, r.text.replace(/[\t\r\n]+/g, ' ')].join('\t'));
  send('reminders', lines.join('\n'));
  tick();
}

function addReminder(text, due, repeat = 'none') {
  const r = { id: uid(), text, due, repeat, created: Date.now(), firedAt: null, done: false };
  reminders.push(r);
  saveReminders();
  renderReminders();
  return r;
}

function nextOccurrence(r) {
  const d = new Date(r.due);
  const now = Date.now();
  do {
    if (r.repeat === 'weekly') d.setDate(d.getDate() + 7);
    else {
      d.setDate(d.getDate() + 1);
      if (r.repeat === 'weekdays') while (d.getDay() === 0 || d.getDay() === 6) d.setDate(d.getDate() + 1);
    }
  } while (d.getTime() <= now);
  return d.getTime();
}

let alertQueue = [];
let alertTimer;

function fireReminder(id) {
  const r = reminders.find((x) => x.id === id);
  if (!r) return;
  if (r.repeat && r.repeat !== 'none') {
    r.created = Date.now();
    r.due = nextOccurrence(r);
  } else {
    r.firedAt = Date.now();
  }
  saveReminders();
  renderReminders();
  showAlert(r);
  showMiniNotice(`Rappel · ${r.text}`, { icon: 'clock', tab: 'reminders' }); // le son est joué par le natif
}

// Alerte de la barre. Un rappel, ou une autre alerte (minuteur) qui apporte ses propres boutons :
// { id, text, snoozeLabel, doneLabel, onSnooze, onDone }.
function showAlert(r) {
  alertQueue = alertQueue.filter((x) => x.id !== r.id);
  alertQueue.push(r);
  renderAlert();
}

function dismissAlert(id) {
  if (!alertQueue.some((x) => x.id === id)) return;
  alertQueue = alertQueue.filter((x) => x.id !== id);
  renderAlert();
}

function renderAlert() {
  const a = alertQueue[alertQueue.length - 1];
  const el = $('#alert');
  clearTimeout(alertTimer);
  if (!a) {
    el.hidden = true;
    document.body.classList.remove('alerting');
    return;
  }
  $('#alertText').textContent = a.text;
  $('#alert [data-alert="snooze"]').textContent = a.snoozeLabel || '+10 min';
  $('#alert [data-alert="done"]').textContent = a.doneLabel || 'Fait';
  el.hidden = false;
  el.style.animation = 'none';
  void el.offsetWidth;
  el.style.animation = '';
  document.body.classList.add('alerting');
  alertTimer = setTimeout(() => { alertQueue = []; renderAlert(); }, 9000);
}

function completeReminder(id) {
  const r = reminders.find((x) => x.id === id);
  if (!r) return;
  r.done = true;
  r.doneAt = Date.now();
  saveReminders();
}

function snoozeReminder(id, minutes = 10) {
  const r = reminders.find((x) => x.id === id);
  if (!r) return;
  if (r.repeat && r.repeat !== 'none') {
    addReminder(r.text, Date.now() + minutes * 60e3);
    return;
  }
  r.firedAt = null;
  r.created = Date.now();
  r.due = Date.now() + minutes * 60e3;
  saveReminders();
}

function deleteReminder(id) {
  reminders = reminders.filter((x) => x.id !== id);
  saveReminders();
}

function remHTML(r, now) {
  const repeat = REPEAT_NAMES[r.repeat] ? ` · ${icon('repeat')} ${REPEAT_NAMES[r.repeat]}` : '';
  if (r.done) {
    return `<div class="rem glass done" data-id="${r.id}">
      <div class="rem-ring">${icon('check')}</div>
      <div class="rem-body"><div class="rem-text">${esc(r.text)}</div><div class="rem-meta">${fmtWhen(r.doneAt || r.due)}</div></div>
      <div class="rem-actions"><button type="button" class="icon-btn del" data-act="delete" title="Supprimer">${icon('trash')}</button></div></div>`;
  }
  if (r.firedAt) {
    return `<div class="rem glass pending" data-id="${r.id}">
      <div class="rem-ring" style="--p:1">${icon('bell')}</div>
      <div class="rem-body"><div class="rem-text">${esc(r.text)}</div><div class="rem-meta"><span class="in">C'est l'heure</span> · ${fmtWhen(r.due)}</div></div>
      <div class="rem-actions">
        <button type="button" class="icon-btn snooze" data-act="snooze" title="Reporter de 10 minutes">+10 min</button>
        <button type="button" class="icon-btn ok" data-act="done" title="Marquer comme fait">${icon('check')}</button>
      </div></div>`;
  }
  const total = Math.max(1, r.due - (r.created || now));
  const p = clamp(1 - (r.due - now) / total, 0, 1);
  return `<div class="rem glass" data-id="${r.id}">
    <div class="rem-ring" style="--p:${p.toFixed(3)}">${icon('clock')}</div>
    <div class="rem-body"><div class="rem-text">${esc(r.text)}</div><div class="rem-meta"><span class="in">${fmtIn(r.due - now)}</span> · ${fmtWhen(r.due)}${repeat}</div></div>
    <div class="rem-actions">
      <button type="button" class="icon-btn ok" data-act="done" title="Marquer comme fait">${icon('check')}</button>
      <button type="button" class="icon-btn del" data-act="delete" title="Supprimer">${icon('trash')}</button>
    </div></div>`;
}

let remRenderedKey = '';

function renderReminders(force = true) {
  const now = Date.now();
  const pending = reminders.filter((r) => !r.done && r.firedAt).sort((a, b) => b.firedAt - a.firedAt);
  const upcoming = upcomingReminders();
  const done = reminders.filter((r) => r.done).sort((a, b) => (b.doneAt || 0) - (a.doneAt || 0)).slice(0, 15);

  const key = JSON.stringify([pending, upcoming, done].map((l) => l.map((r) => r.id + r.due + r.firedAt)));
  if (!force && key === remRenderedKey) {
    // simple rafraîchissement des comptes à rebours, sans relancer les animations
    upcoming.forEach((r) => {
      const el = $(`.rem[data-id="${r.id}"]`);
      if (!el) return;
      const total = Math.max(1, r.due - (r.created || now));
      $('.rem-ring', el).style.setProperty('--p', clamp(1 - (r.due - now) / total, 0, 1).toFixed(3));
      $('.in', el).textContent = fmtIn(r.due - now);
    });
    return;
  }
  remRenderedKey = key;

  let html = '';
  if (pending.length) html += `<div class="sec-title">Maintenant</div>${pending.map((r) => remHTML(r, now)).join('')}`;
  if (upcoming.length) html += `<div class="sec-title">À venir</div>${upcoming.map((r) => remHTML(r, now)).join('')}`;
  if (done.length) html += `<div class="sec-title">Terminés <button type="button" data-act="clear-done">Tout effacer</button></div>${done.map((r) => remHTML(r, now)).join('')}`;
  if (!html) html = `<div class="empty">${icon('bell')}<b>Aucun rappel</b><p>Écris ce dont tu veux te souvenir, ou demande à Aeria : « rappelle-moi de… ».</p></div>`;
  $('#remList').innerHTML = html;

  const n = upcoming.length;
  $('#remSummary').textContent = pending.length
    ? `${pending.length} en attente · ${n} à venir`
    : n ? `${n} rappel${n > 1 ? 's' : ''} à venir` : 'Rien de prévu';
}

// Analyse simple du français : « dans 10 min », « demain à 9h », « à 14h30 », « ce soir »…
function parseWhen(input) {
  const now = new Date();
  const B = '(?<=^|[\\s,.;:!?(])';
  const E = '(?=$|[\\s,.;:!?)])';
  const rx = (s) => new RegExp(B + s + E, 'i');
  let text = input;
  const take = (re) => {
    const m = text.match(re);
    if (m) text = text.replace(m[0], ' ');
    return m;
  };

  const rel = take(rx('dans\\s+(\\d+(?:[.,]\\d+)?|une?)\\s*(minutes?|mins?|mn|m|heures?|h|jours?|j|secondes?|s)'));
  let due = null;
  if (rel) {
    const n = /^une?$/i.test(rel[1]) ? 1 : parseFloat(rel[1].replace(',', '.'));
    const u = rel[2].toLowerCase();
    const unit = u.startsWith('h') ? 3600e3 : u.startsWith('j') ? 864e5 : u.startsWith('s') ? 1e3 : 60e3;
    due = new Date(now.getTime() + n * unit);
  } else {
    let day = null;
    if (take(rx('après[- ]demain'))) day = 2;
    else if (take(rx('demain'))) day = 1;
    else if (take(rx("aujourd'hui"))) day = 0;
    let hh = null, mm = 0;
    const t = take(rx('(?:à|a|vers|pour)?\\s*(\\d{1,2})\\s*(?:h|:)\\s*(\\d{2})?'));
    if (t) { hh = +t[1]; mm = t[2] ? +t[2] : 0; }
    else if (take(rx('(?:ce|demain)\\s+soir')) || take(rx('ce soir'))) hh = 19;
    else if (take(rx('(?:ce|à|a)\\s+midi')) || take(rx('midi'))) hh = 12;
    else if (take(rx('ce matin')) || take(rx('le matin'))) hh = 9;
    else if (take(rx('cet après[- ]midi'))) hh = 14;
    if (hh === null && day === null) return null;
    if (hh === null) hh = 9;
    if (hh > 23 || mm > 59) return null;
    due = new Date(now);
    due.setDate(due.getDate() + (day || 0));
    due.setHours(hh, mm, 0, 0);
    if (day === null && due <= now) due.setDate(due.getDate() + 1);
  }
  let clean = text
    .replace(/^\s*(rappelle[- ]moi|me rappeler|rappel\s*:?|n'oublie pas)\s*/i, '')
    .replace(/^\s*(de\s+|d')/i, '')
    .replace(/\s{2,}/g, ' ')
    .trim()
    .replace(/[,.;:]+$/, '');
  if (clean) clean = clean.charAt(0).toUpperCase() + clean.slice(1);
  return { due: due.getTime(), text: clean };
}

const toLocalInput = (ts) => {
  const d = new Date(ts);
  d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
  return d.toISOString().slice(0, 16);
};

function setupReminders() {
  const text = $('#remText'), when = $('#remWhen'), detected = $('#remDetected');
  let manualWhen = false;

  const refreshDetected = () => {
    const parsed = parseWhen(text.value);
    if (parsed && !manualWhen) when.value = toLocalInput(parsed.due);
    const ts = when.value ? new Date(when.value).getTime() : null;
    detected.textContent = ts ? `→ ${fmtWhen(ts)} · ${fmtIn(ts - Date.now())}` : '';
  };

  text.addEventListener('input', () => { $$('#remChips button').forEach((b) => b.classList.remove('on')); refreshDetected(); });
  when.addEventListener('input', () => { manualWhen = !!when.value; refreshDetected(); });

  $('#remChips').addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    const d = new Date();
    if (b.dataset.in) d.setMinutes(d.getMinutes() + Number(b.dataset.in));
    else {
      const [h, m] = (b.dataset.at || b.dataset.tomorrow).split(':').map(Number);
      if (b.dataset.tomorrow) d.setDate(d.getDate() + 1);
      d.setHours(h, m, 0, 0);
      if (d <= new Date()) d.setDate(d.getDate() + 1);
    }
    $$('#remChips button').forEach((x) => x.classList.toggle('on', x === b));
    manualWhen = true;
    when.value = toLocalInput(d.getTime());
    refreshDetected();
    text.focus();
  });

  $('#remForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const parsed = parseWhen(text.value);
    const label = (parsed && parsed.text) || text.value.trim();
    const ts = when.value ? new Date(when.value).getTime() : parsed?.due;
    if (!label) { text.focus(); return; }
    if (!ts) { toast('Choisis une heure pour ce rappel'); when.focus(); return; }
    addReminder(label, ts, $('#remRepeat').value);
    toast(`Rappel ${fmtWhen(ts)}`);
    text.value = '';
    when.value = '';
    manualWhen = false;
    $('#remRepeat').value = 'none';
    $$('#remChips button').forEach((x) => x.classList.remove('on'));
    detected.textContent = '';
  });

  $('#remList').addEventListener('click', (e) => {
    const btn = e.target.closest('[data-act]');
    if (!btn) return;
    const act = btn.dataset.act;
    if (act === 'clear-done') {
      const els = $$('.rem.done');
      els.forEach((el) => el.classList.add('leaving'));
      setTimeout(() => { reminders = reminders.filter((r) => !r.done); saveReminders(); renderReminders(); }, document.body.classList.contains('reduce') ? 0 : 340);
      return;
    }
    const el = btn.closest('.rem');
    const id = el?.dataset.id;
    if (!id) return;
    animateOut(el, () => {
      if (act === 'done') completeReminder(id);
      if (act === 'delete') deleteReminder(id);
      if (act === 'snooze') snoozeReminder(id);
      renderReminders();
    });
  });

  $('#alert').addEventListener('click', (e) => {
    const act = e.target.closest('[data-alert]')?.dataset.alert;
    const a = alertQueue[alertQueue.length - 1];
    if (!act || !a) return;
    alertQueue.pop();
    renderAlert();
    if (a.onDone) { (act === 'done' ? a.onDone : a.onSnooze)(); return; }
    if (act === 'done') { if (!a.repeat || a.repeat === 'none') completeReminder(a.id); }
    if (act === 'snooze') { snoozeReminder(a.id); toast('Reporté de 10 minutes'); }
    renderReminders();
  });
}

// Un rappel a sonné et attend « Fait » ou un report.
const hasPendingReminders = () => alertQueue.some((a) => !a.onDone) || reminders.some((r) => !r.done && r.firedAt);

export {
  reminders, REPEAT_NAMES, upcomingReminders, saveReminders, addReminder, fireReminder,
  completeReminder, snoozeReminder, renderReminders, setupReminders, hasPendingReminders, showAlert, dismissAlert,
};
