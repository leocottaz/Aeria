// Réglages persistés (localStorage), appliqués à l'apparence et envoyés au natif.
import { $, $$, store } from './util.js';
import { send } from './bridge.js';

const ACCENTS = {
  violet: ['#8b7cff', '#5ac8fa'],
  bleu: ['#3d8bff', '#46e0d3'],
  rose: ['#ff5fa8', '#ffa45c'],
  vert: ['#2fcf8a', '#9be15d'],
  orange: ['#ff8a3d', '#ffd166'],
  graphite: ['#9aa3b5', '#d6dbe6'],
};

const DEFAULTS = {
  mode: 0, revealDelay: 550, hideDelay: 700, fullscreenReveal: false, width: 680, panelH: 540, flare: 18,
  theme: 'dark', accent: 'violet', reduceMotion: false, sound: true, autostart: false, lowMemory: true,
  backend: 'pro', ccModel: 'default',
  category: 'claude', claudeMode: 'pro', // catégorie de connexion (Claude, ChatGPT, Gratuit) ; claudeMode = dernier mode Claude
  freeService: 'gemini', freeModel: '', freeFallback: false, // modèle gratuit ('' = automatique)
  webSearch: true, // Claude (Pro et Clé API) peut chercher sur internet
  screenAccess: true, // Claude (Pro et Clé API) peut regarder l'écran quand la demande l'exige
  musicSource: 'deezer', musicReveal: true, lyricsOpen: true, miniLyrics: true, musicTint: true,
  timerPill: false, // temps du minuteur dans le mini-dock
  appNotifs: false, // notifications des autres applis dans la pastille du mini-dock (Windows : demande l'accès aux notifications)
  appNotifsText: true, // avec leur contenu ; sinon seulement le nom de l'appli
  voiceWake: false, // dire « Aeria » ouvre le dock (micro écouté hors ligne par le natif)
  voiceModel: 'small', // modèle de reconnaissance à utiliser (base, small, medium) : aucun n'est téléchargé tant qu'on ne le demande pas
  model: 'auto', effort: 'low', customInstr: '', lastTab: 'claude', settingsPage: 'aeria',
  updateCheck: true, updateSkipped: '', // mises à jour : vérification automatique, version ignorée (« v1.2.3 »)
};
const NUMERIC = new Set(['mode', 'revealDelay', 'hideDelay', 'width', 'panelH', 'flare']);
const NATIVE_KEYS = new Set(['mode', 'revealDelay', 'hideDelay', 'width', 'panelH', 'theme', 'sound', 'autostart', 'lyricsOpen', 'miniLyrics', 'musicReveal', 'fullscreenReveal', 'voiceWake', 'voiceModel']);
const MODEL_NAMES = { 'claude-opus-5-5': 'Opus 5.5', 'claude-sonnet-5-5': 'Sonnet 5.5', 'claude-haiku-5-5': 'Haiku 5.5' };
// « Auto » : le modèle suit le réglage Qualité.
const AUTO_MODELS = { low: 'claude-haiku-5-5', medium: 'claude-sonnet-5-5', high: 'claude-opus-5-5' };
const apiModel = () => (settings.model === 'auto' ? AUTO_MODELS[settings.effort] || AUTO_MODELS.medium : settings.model);
const CC_MODEL_NAMES = { default: 'modèle par défaut', opus: 'Opus', sonnet: 'Sonnet', haiku: 'Haiku' };
const EFFORT_NAMES = { low: 'rapide', medium: 'équilibrée', high: 'approfondie' };
const FREE_NAMES = { gemini: 'Gemini', groq: 'Groq', openai: 'OpenAI' };

let settings = { ...DEFAULTS, ...store.get('settings', {}) };
// La catégorie se déduit du mode enregistré (réglages d'avant les catégories).
settings.category = settings.backend === 'free' ? 'free' : settings.backend === 'chatgpt' ? 'chatgpt' : 'claude';
if (settings.category === 'claude') settings.claudeMode = settings.backend;
if (settings.settingsPage === 'routines') settings.settingsPage = 'modes'; // la page « Routines » est devenue « Modes »

// Réglages imposés par le mode en cours (js/features/modes.js) : ils passent avant ceux de l'utilisateur
// sans les modifier, donc arrêter le mode rend ses réglages tels quels. Lire `eff(clé)` là où un mode doit pouvoir agir.
let overrides = {};
const eff = (key) => (key in overrides ? overrides[key] : settings[key]);
// push = false au démarrage de la page : main.js envoie lui-même la config après « ready ».
function setOverrides(next, push = true) {
  if (JSON.stringify(next) === JSON.stringify(overrides)) return;
  const old = overrides;
  overrides = { ...next };
  applyAppearance(); // l'accent peut changer
  if (!push) return;
  sendConfig();
  // Comme un changement de réglage : les modules qui lisent `eff` se mettent à jour (paroles, voix…).
  for (const key of new Set([...Object.keys(old), ...Object.keys(overrides)])) {
    if (old[key] !== overrides[key]) listeners.forEach((fn) => fn(key, eff(key)));
  }
}

function sendConfig() {
  send('config', [
    `mode=${settings.mode}`, `revealDelay=${settings.revealDelay}`, `hideDelay=${settings.hideDelay}`,
    `width=${settings.width}`, `panelH=${settings.panelH}`, `theme=${settings.theme}`,
    `sound=${eff('sound') ? 1 : 0}`, `autostart=${settings.autostart ? 1 : 0}`,
    `fullscreenReveal=${settings.fullscreenReveal ? 1 : 0}`, `voiceWake=${eff('voiceWake') ? 1 : 0}`, `voiceModel=${settings.voiceModel}`,
    // Le natif garde la page éveillée pendant une lecture : elle seule demande le mini-dock et la carte musique.
    `miniLyrics=${(settings.lyricsOpen && eff('miniLyrics')) || eff('musicReveal') ? 1 : 0}`,
  ].join('\n'));
}

function applyAppearance() {
  const b = document.body;
  b.classList.toggle('theme-dark', settings.theme !== 'light');
  b.classList.toggle('theme-light', settings.theme === 'light');
  b.classList.toggle('reduce', !!settings.reduceMotion);
  b.classList.toggle('tint-music', !!settings.musicTint); // couleurs du dock = pochette (music.css)
  const [a, a2] = ACCENTS[eff('accent')] || ACCENTS.violet; // un mode peut imposer sa couleur
  const root = document.documentElement.style;
  root.setProperty('--accent', a);
  root.setProperty('--accent2', a2);
  root.setProperty('--panel-h', settings.panelH + 'px');
  root.setProperty('--dock-w', settings.width + 'px');
  root.setProperty('--flare', settings.flare + 'px');
  const effort = `réflexion ${EFFORT_NAMES[settings.effort] || settings.effort}`;
  $('#chatModel').textContent = settings.backend === 'pro'
    ? `Abonnement Pro · ${CC_MODEL_NAMES[settings.ccModel] || settings.ccModel} · ${effort}`
    : settings.backend === 'chatgpt'
      ? `ChatGPT${settings.freeModel ? ` · ${settings.freeModel}` : ''}`
    : settings.backend === 'free'
      ? `Gratuit · ${FREE_NAMES[settings.freeService] || settings.freeService}${settings.freeModel ? ` · ${settings.freeModel}` : ''}`
      : `API · ${MODEL_NAMES[apiModel()] || apiModel()} · ${effort}`;
}

function setSetting(key, value) {
  if (NUMERIC.has(key)) value = Number(value);
  if (settings[key] === value) return;
  settings[key] = value;
  store.set('settings', settings);
  applyAppearance();
  renderSettings();
  if (NATIVE_KEYS.has(key)) sendConfig();
  listeners.forEach((fn) => fn(key, value));
}

// Les autres modules réagissent aux changements sans que settings.js ne dépende d'eux.
const listeners = [];
const onSettingChange = (fn) => listeners.push(fn);

const RANGE_FORMAT = {
  revealDelay: (v) => (v === 0 ? 'instantané' : `${(v / 1000).toFixed(2).replace('.', ',')} s`),
  hideDelay: (v) => `${(v / 1000).toFixed(2).replace('.', ',')} s`,
  width: (v) => `${v} px`,
  panelH: (v) => `${v} px`,
  flare: (v) => (v === 0 ? 'aucun' : `${v} px`),
};

function paintRange(input) {
  const v = Number(input.value), min = Number(input.min), max = Number(input.max);
  input.style.setProperty('--v', `${((v - min) / (max - min)) * 100}%`);
  const out = $(`output[data-for="${input.dataset.setting}"]`);
  if (out) out.textContent = RANGE_FORMAT[input.dataset.setting]?.(v) ?? v;
}

function openSettingsPage(page) {
  setSetting('settingsPage', page);
  $('#settingsBody').scrollTop = 0;
}

function renderSettings() {
  $$('.set-page').forEach((p) => { p.hidden = p.dataset.page !== settings.settingsPage; });
  $$('#settingsNav button').forEach((b) => {
    const on = b.dataset.page === settings.settingsPage;
    b.classList.toggle('on', on);
    b.toggleAttribute('aria-current', on);
  });
  $$('.seg[data-setting], .mode-cards[data-setting]').forEach((seg) => {
    const buttons = $$('button[data-v]', seg);
    const idx = Math.max(0, buttons.findIndex((b) => String(settings[seg.dataset.setting]) === b.dataset.v));
    buttons.forEach((b, i) => b.classList.toggle('on', i === idx));
    const thumb = $('.seg-thumb', seg);
    if (thumb) { thumb.style.setProperty('--i', idx); thumb.style.setProperty('--n', buttons.length); }
  });
  $$('input[type=range][data-setting]').forEach((r) => {
    if (document.activeElement !== r) r.value = settings[r.dataset.setting];
    paintRange(r);
  });
  $$('input[type=checkbox][data-setting]').forEach((c) => { c.checked = !!settings[c.dataset.setting]; });
  $$('#swatches button').forEach((b) => b.classList.toggle('on', b.dataset.accent === settings.accent));
  $('#revealField').style.display = settings.mode === 0 ? '' : 'none';
  $('#proSection').hidden = settings.backend !== 'pro';
  $('#apiSection').hidden = settings.backend !== 'api';
  $('#proModelField').hidden = settings.backend !== 'pro';
  $('#apiModelField').hidden = settings.backend !== 'api';
  const claude = settings.category === 'claude';
  $('#claudeModeField').hidden = !claude;
  $('#freeFallbackRow').hidden = !claude; // le secours n'a de sens qu'avec Claude
  $('#webSearchRow').hidden = !claude || settings.backend === 'free'; // seulement Claude (Pro, Clé API) sait chercher
  $('#screenRow').hidden = settings.backend === 'free' ? settings.freeService === 'groq' : !(claude || settings.backend === 'chatgpt'); // regarder l'écran : pas avec Groq (pas d'images)
  $('#screenNote').hidden = $('#screenRow').hidden;
  $('#freeFields').hidden = claude && !settings.freeFallback;
  $('#freeServiceField').hidden = settings.category === 'chatgpt';
  $('#chatgptKeyLabel').hidden = settings.category !== 'chatgpt';
  $('#effortField').hidden = !claude;
  if (document.activeElement !== $('#customInstr')) $('#customInstr').value = settings.customInstr;
}

function setupSettings() {
  $('#settingsNav').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-page]');
    if (b) openSettingsPage(b.dataset.page);
  });
  $$('.seg[data-setting]').forEach((seg) => seg.insertAdjacentHTML('afterbegin', '<span class="seg-thumb"></span>'));
  $$('.seg[data-setting], .mode-cards[data-setting]').forEach((seg) => {
    seg.addEventListener('click', (e) => {
      const b = e.target.closest('button[data-v]');
      if (b) setSetting(seg.dataset.setting, b.dataset.v);
    });
  });
  $$('input[type=range][data-setting]').forEach((r) => {
    r.addEventListener('input', () => paintRange(r));
    r.addEventListener('change', () => setSetting(r.dataset.setting, r.value));
  });
  $$('input[type=checkbox][data-setting]').forEach((c) => {
    c.addEventListener('change', () => setSetting(c.dataset.setting, c.checked));
  });
  $('#swatches').innerHTML = Object.entries(ACCENTS)
    .map(([name, [a, b]]) => `<button type="button" data-accent="${name}" title="${name}" style="--a:${a};--b:${b}"></button>`)
    .join('');
  $('#swatches').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-accent]');
    if (b) setSetting('accent', b.dataset.accent);
  });
  let instrTimer;
  $('#customInstr').addEventListener('input', (e) => {
    clearTimeout(instrTimer);
    instrTimer = setTimeout(() => setSetting('customInstr', e.target.value), 400);
  });

  $('#quitBtn').addEventListener('click', () => send('quit'));
}

// Aeria peut-elle regarder l'écran avec ce mode de connexion ? La capture part en image : Claude (Pro, Clé API),
// Gemini et OpenAI la lisent, pas Groq. `backend` : 'pro', 'api', 'free' (Gemini ou Groq) ou 'chatgpt'.
const screenCapable = (backend = settings.backend) => settings.screenAccess
  && (backend === 'pro' || backend === 'api' || backend === 'chatgpt' || (backend === 'free' && settings.freeService !== 'groq'));

export { settings, ACCENTS, eff, setOverrides, apiModel, setSetting, sendConfig, applyAppearance, renderSettings, setupSettings, openSettingsPage, onSettingChange, screenCapable };
