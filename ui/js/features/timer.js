// Minuteur : compte à rebours (temps limité) ou chronomètre (illimité).
// Tout est calculé à partir d'horodatages : la page peut dormir quand le dock est rangé.
// La fin du compte à rebours est tenue par le natif (src/features/reminders.cpp), qui renvoie {"t":"timerdone"}.
import { $, $$, clamp, store, toast } from '../core/util.js';
import { bridge, send } from '../core/bridge.js';
import { fmtTime } from '../core/format.js';
import { S } from '../core/state.js';
import { settings, onSettingChange } from '../core/settings.js';
import { showAlert, dismissAlert } from './reminders.js';
import { tick } from '../dock/dock.js';
import { setMiniTimer } from '../dock/mini.js';

const PRESETS = [1, 3, 5, 10, 15, 25, 30, 45, 60]; // minutes
const MAX = 24 * 3600e3;

// mode : 'down' (temps limité) ou 'up' (illimité) ; duration : durée du compte à rebours (ms)
// startedAt : début de la course en cours (null = arrêté) ; elapsed : temps couru avant elle
// ringing : le compte à rebours est fini et attend « OK »
const T = { mode: 'down', duration: 5 * 60e3, startedAt: null, elapsed: 0, ringing: false, ...store.get('timer', {}) };

const running = () => T.startedAt != null;
const elapsedNow = (now = Date.now()) => T.elapsed + (running() ? now - T.startedAt : 0);
const remaining = (now) => Math.max(0, T.duration - elapsedNow(now));
const idle = () => !running() && !T.elapsed && !T.ringing;

// 1:05:09, 05:09 ; le compte à rebours arrondit au-dessus (on ne lit jamais 00:00 avant la fin)
function fmtClock(ms, up = false) {
  const s = up ? Math.floor(ms / 1000) : Math.ceil(ms / 1000);
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, sec = s % 60;
  const mmss = `${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
  return h ? `${h}:${mmss}` : mmss;
}

function fmtDuration(ms) {
  const s = Math.round(ms / 1000), h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, sec = s % 60;
  const parts = [];
  if (h) parts.push(`${h} h`);
  if (m) parts.push(`${m} min`);
  if (sec) parts.push(`${sec} s`);
  return parts.join(' ') || '0 s';
}

// « 7 » (minutes), « 1:30 », « 1:00:00 », « 1h20 », « 90 s », « 2 h 15 min »…
function parseDuration(input) {
  const s = input.trim().toLowerCase().replace(/,/g, '.');
  const clock = s.match(/^(\d+):(\d{1,2})(?::(\d{1,2}))?$/);
  if (clock) {
    const [a, b, c] = clock.slice(1).map((x) => (x == null ? null : +x));
    return (c == null ? a * 60 + b : (a * 60 + b) * 60 + c) * 1e3;
  }
  const UNITS = { h: 3600, m: 60, s: 1 };
  let total = 0, prev = null, rest = s;
  for (const [all, n, unit] of s.matchAll(/(\d+(?:\.\d+)?)\s*([a-zé]*)/g)) {
    rest = rest.replace(all, '');
    // sans unité : la suivante de la précédente (« 1h20 » = 1 h 20 min), sinon des minutes
    const u = unit ? unit[0] : prev === 'h' ? 'm' : prev === 'm' ? 's' : 'm';
    if (!UNITS[u]) return 0;
    total += parseFloat(n) * UNITS[u];
    prev = u;
  }
  if (rest.replace(/\bet\b/g, '').trim()) return 0;
  return Math.round(total) * 1e3;
}

function save() {
  store.set('timer', T);
  // Le natif sonne à la fin même si la page dort ; 0 = rien à surveiller.
  send('timer', T.mode === 'down' && running() ? String(T.startedAt + T.duration - T.elapsed) : '0');
  updatePill();
  tick();
}

function start() {
  if (T.mode === 'down' && remaining() <= 0) T.elapsed = 0; // fini : on recommence
  T.ringing = false;
  dismissAlert('timer');
  T.startedAt = Date.now();
  save();
  renderTimer();
}

function pause() {
  T.elapsed = elapsedNow();
  T.startedAt = null;
  save();
  renderTimer();
}

function resetTimer() {
  T.startedAt = null;
  T.elapsed = 0;
  T.ringing = false;
  dismissAlert('timer');
  save();
  renderTimer();
}

function setDuration(ms) {
  T.mode = 'down';
  T.duration = clamp(ms, 1000, MAX);
  resetTimer();
}

// +1 min : pendant la course on prolonge ; une fois fini, on relance pour une minute.
function addMinute() {
  if (T.ringing || remaining() <= 0) {
    T.duration = 60e3;
    T.elapsed = 0;
    start();
    return;
  }
  T.duration = Math.min(MAX, T.duration + 60e3);
  save();
  renderTimer();
}

function finishTimer() {
  if (T.mode !== 'down' || !running()) return;
  T.elapsed = T.duration;
  T.startedAt = null;
  T.ringing = true;
  save();
  renderTimer();
  showAlert({
    id: 'timer', text: `Minuteur terminé · ${fmtDuration(T.duration)}`,
    snoozeLabel: '+1 min', doneLabel: 'OK', onSnooze: addMinute, onDone: stopRinging,
  });
}

function stopRinging() {
  T.ringing = false;
  T.elapsed = 0; // prêt pour un nouveau tour de la même durée
  dismissAlert('timer');
  save();
  renderTimer();
}

// --- affichage

let raf = 0;

function frame() {
  raf = 0;
  const now = Date.now();
  const time = $('#timerTime');
  if (T.mode === 'down') {
    const left = remaining(now);
    time.textContent = fmtClock(left);
    $('#timerDial').style.setProperty('--p', (T.duration ? left / T.duration : 0).toFixed(4));
    if (!bridge && running() && left <= 0) { finishTimer(); return; } // aperçu : pas de natif pour sonner
  } else {
    const e = elapsedNow(now);
    time.innerHTML = `${fmtClock(e, true)}<small>,${Math.floor(e / 100) % 10}</small>`;
    $('#timerDial').style.setProperty('--p', ((e % 60e3) / 60e3).toFixed(4));
  }
  // On n'anime que si l'onglet est visible ; sinon tick() suffit pour la barre.
  if (running() && S.st === 'panel' && S.tab === 'timer') raf = requestAnimationFrame(frame);
}

function renderTimer() {
  const down = T.mode === 'down';
  const modes = $('#timerModes');
  $$('button', modes).forEach((b, i) => {
    b.classList.toggle('on', b.dataset.mode === T.mode);
    if (b.dataset.mode === T.mode) modes.style.setProperty('--i', i);
  });
  $('#timerView').classList.toggle('chrono', !down);
  $('#timerDial').classList.toggle('ringing', T.ringing);
  $('#timerDial').classList.toggle('paused', !running() && !idle() && !T.ringing);
  $('#timerPlay').classList.toggle('run', running());
  $('#timerPlay').title = running() ? 'Pause' : 'Démarrer';
  $('#timerPlus').hidden = !down || idle();
  $('#timerReset').disabled = idle();
  $('#timerSetup').hidden = !down || !idle();
  $$('#timerPresets button').forEach((b) => b.classList.toggle('on', +b.dataset.min * 60e3 === T.duration));

  let sub, summary;
  if (T.ringing) { sub = 'Temps écoulé'; summary = 'Terminé'; }
  else if (running()) {
    sub = down ? `Fin à ${fmtTime(new Date(T.startedAt + T.duration - T.elapsed))}` : 'En cours';
    summary = down ? `Compte à rebours de ${fmtDuration(T.duration)}` : 'Chronomètre en cours';
  } else if (!idle()) { sub = 'En pause'; summary = 'En pause'; }
  else { sub = down ? fmtDuration(T.duration) : 'Prêt'; summary = down ? 'Temps limité : choisis une durée' : 'Illimité : tourne jusqu’à l’arrêt'; }
  $('#timerSub').textContent = sub;
  $('#timerSummary').textContent = summary;

  cancelAnimationFrame(raf);
  frame();
}

// Texte de la barre (sous l'heure) quand le minuteur tourne, est en pause ou a sonné ; sinon null.
function timerLabel() {
  if (T.ringing) return '⏱ Minuteur terminé';
  if (idle()) return null;
  const t = T.mode === 'down' ? fmtClock(remaining()) : fmtClock(elapsedNow(), true);
  return `⏱ ${t}${running() ? '' : ' · en pause'}`;
}

const timerRinging = () => T.ringing;

// Temps dans le mini-dock (option de l'onglet). Fenêtre visible = page éveillée : la pilule peut
// compter elle-même. Calée sur le changement de seconde, pour ne jamais sauter un chiffre.
// Une fois terminé, c'est la cloche du mini-dock qui prend le relais.
let pillTimer = 0;

function updatePill() {
  clearTimeout(pillTimer);
  if (!settings.timerPill || idle() || T.ringing) { setMiniTimer(null); return; }
  const down = T.mode === 'down';
  setMiniTimer({ text: down ? fmtClock(remaining()) : fmtClock(elapsedNow(), true), paused: !running() });
  if (running()) pillTimer = setTimeout(updatePill, (down ? remaining() % 1000 || 1000 : 1000 - elapsedNow() % 1000) + 10);
}

// --- commandes de Aeria (chat/tools.js) : chacune renvoie une phrase pour le modèle

function startCountdown(ms) {
  if (!(ms >= 1000) || ms > MAX) throw new Error('Durée invalide : entre 1 seconde et 24 heures.');
  setDuration(ms);
  start();
  return `Minuteur lancé : ${fmtDuration(T.duration)}, fin à ${fmtTime(new Date(T.startedAt + T.duration))}.`;
}

function startStopwatch() {
  T.mode = 'up';
  T.startedAt = null;
  T.elapsed = 0;
  start();
  return 'Chronomètre lancé.';
}

function pauseTimer() {
  if (!running()) throw new Error("Le minuteur ne tourne pas.");
  pause();
  return `Minuteur en pause (${timerStatus()}).`;
}

function resumeTimer() {
  if (running()) throw new Error('Le minuteur tourne déjà.');
  if (idle() || T.ringing) throw new Error("Rien à reprendre : aucun minuteur en pause.");
  start();
  return `Minuteur repris (${timerStatus()}).`;
}

function stopTimer() {
  if (idle()) throw new Error('Aucun minuteur en cours.');
  const was = timerStatus();
  resetTimer();
  return `Minuteur arrêté et remis à zéro (il en était à : ${was}).`;
}

// État lisible par le modèle (bloc [Contexte]) ; null si rien ne tourne.
function timerStatus() {
  if (T.ringing) return `compte à rebours de ${fmtDuration(T.duration)} terminé, la sonnerie attend l'utilisateur`;
  if (idle()) return null;
  const state = running() ? 'en cours' : 'en pause';
  if (T.mode === 'up') return `chronomètre ${state}, ${fmtDuration(Math.floor(elapsedNow() / 1000) * 1000)} au compteur`;
  return `compte à rebours de ${fmtDuration(T.duration)} ${state}, reste ${fmtDuration(Math.ceil(remaining() / 1000) * 1000)}`;
}

// Au démarrage : redonne l'échéance au natif (il sonne aussitôt si elle est passée dock fermé).
const syncTimer = () => save();

function setupTimer() {
  $('#timerPresets').innerHTML = PRESETS.map((m) => `<button type="button" data-min="${m}">${m < 60 ? `${m} min` : `${m / 60} h`}</button>`).join('');
  $('#timerPresets').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-min]');
    if (!b) return;
    setDuration(+b.dataset.min * 60e3);
    start();
  });
  $('#timerForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const ms = parseDuration($('#timerInput').value);
    if (!ms) { toast('Durée non comprise : ex. 7, 1:30, 1h20'); return; }
    if (ms > MAX) { toast('24 h au maximum'); return; }
    $('#timerInput').value = '';
    setDuration(ms);
    start();
  });
  $('#timerModes').addEventListener('click', (e) => {
    const mode = e.target.closest('button[data-mode]')?.dataset.mode;
    if (!mode || mode === T.mode) return;
    if (!idle()) { toast('Réinitialise le minuteur pour changer de mode'); return; }
    T.mode = mode;
    save();
    renderTimer();
  });
  $('#timerPlay').addEventListener('click', () => (running() ? pause() : start()));
  $('#timerReset').addEventListener('click', resetTimer);
  $('#timerPlus').addEventListener('click', addMinute);
  onSettingChange((key) => { if (key === 'timerPill') updatePill(); });
  updatePill();
}

export {
  setupTimer, renderTimer, finishTimer, timerLabel, timerRinging, syncTimer, parseDuration,
  startCountdown, startStopwatch, pauseTimer, resumeTimer, stopTimer, timerStatus,
};
