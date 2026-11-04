// Modes : un mode se démarre (à la main ou par Claude : « lance le mode travail »), exécute ses étapes de démarrage
// (ouvrir, volume, son, musique, attendre, verrouiller) puis reste « en cours » jusqu'à ce qu'on l'arrête.
// Tant qu'il est en cours, une pilule s'affiche dans la barre du dock et ses comportements (BEHAVIORS) s'appliquent.
// Les étapes tournent côté natif (pc::RunRoutine, commande « routine ») : elles continuent quand le dock se range.
// Les comportements sont gérés par la page, qui les impose aux réglages (setOverrides de settings.js).
import { $, $$, uid, esc, icon, store, toast } from '../core/util.js';
import { ask, send } from '../core/bridge.js';
import { ACCENTS, setOverrides } from '../core/settings.js';

const STEP_TYPES = { open: 'Ouvrir', volume: 'Volume', mute: 'Son', music: 'Musique', wait: 'Attendre', lock: 'Verrouiller' };
const MUSIC = { play: 'Lecture', pause: 'Pause', next: 'Morceau suivant', previous: 'Morceau précédent' };
const MUTE = { on: 'Couper le son', off: 'Rétablir le son', toggle: 'Basculer' };
const DEFAULTS = { open: '', volume: 20, mute: 'on', music: 'play', wait: 3, lock: 'now' };
const CHOICES = { music: MUSIC, mute: MUTE }; // étapes à menu déroulant

// Comportements pendant un mode. Valeur choisie dans le mode : `m.on[clé]` (vide ou false = non touché).
//   - interrupteur : `m.on[clé]` est un booléen ;  - `choices` : menu (valeur = clé de `choices`) ;  - `accent` : pastilles de couleur.
// `resolve(valeur)` renvoie les réglages imposés tant que le mode est en cours (ou null) ; `name(valeur)` le décrit pour Claude.
// Pour en ajouter un : une entrée ici, et lire le réglage via `eff` de settings.js (jamais `settings`) là où il agit.
const BEHAVIORS = {
  silent: {
    label: 'Notifications sans son', hint: 'rappels et minuteur s’affichent sans sonner',
    resolve: (v) => (v ? { sound: false } : null), name: () => 'notifications sans son',
  },
  noTrackCard: {
    label: 'Ne pas montrer le morceau à chaque changement', hint: 'la carte musique reste rangée',
    resolve: (v) => (v ? { musicReveal: false } : null), name: () => 'carte musique rangée',
  },
  noLyrics: {
    label: 'Paroles du mini-dock coupées', hint: 'pas de paroles quand le dock est rangé',
    resolve: (v) => (v ? { miniLyrics: false } : null), name: () => 'paroles du mini-dock coupées',
  },
  voice: {
    label: 'Voix', hint: 'réveil par « Aeria »', choices: { '': 'Inchangée', off: 'Coupée', on: 'Forcée' },
    resolve: (v) => (v === 'off' ? { voiceWake: false } : v === 'on' ? { voiceWake: true } : null),
    name: (v) => (v === 'on' ? 'voix forcée' : 'voix coupée'),
  },
  accent: {
    label: 'Couleur d’accent', hint: 'propre à ce mode', accent: true,
    resolve: (v) => (ACCENTS[v] ? { accent: v } : null), name: (v) => `accent ${v}`,
  },
};

const normalize = (m) => ({
  id: m.id || uid(), name: String(m.name ?? ''), steps: Array.isArray(m.steps) ? m.steps : [],
  on: m.on && typeof m.on === 'object' ? m.on : {},
  apps: Array.isArray(m.apps) ? m.apps : [], // applis dont l'ouverture démarre le mode (noms d'exécutable)
});

// Les « routines » d'avant sont devenues des modes (leurs étapes sont les étapes de démarrage).
let modes = (store.get('modes', null) ?? store.get('routines', [])).map(normalize);
let active = store.get('activeMode', null); // { id, since } : le mode en cours survit à un redémarrage du dock

const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/^(le |la |l'|mode |routine )+/g, '').trim();
let saveTimer;
const saveNow = () => { clearTimeout(saveTimer); store.set('modes', modes); };
const save = () => { clearTimeout(saveTimer); saveTimer = setTimeout(() => store.set('modes', modes), 250); };

// Par nom, sans casse ni accents (« mode travail », « travail ») : identique, sinon le seul qui contient.
function findMode(name) {
  const q = norm(name || '');
  if (!q) return null;
  const near = modes.filter((r) => norm(r.name).includes(q) || q.includes(norm(r.name)));
  return modes.find((r) => norm(r.name) === q) || (near.length === 1 ? near[0] : null);
}

const modeNames = () => modes.map((r) => r.name.trim()).filter(Boolean);
const activeMode = () => modes.find((m) => m.id === active?.id) || null;
const forcedBy = (m) => Object.fromEntries(Object.entries(BEHAVIORS).map(([k, b]) => [k, b.resolve(m.on[k])]).filter(([, r]) => r));
const behaviorNames = (m) => Object.entries(forcedBy(m)).map(([k]) => BEHAVIORS[k].name(m.on[k]));

// ------------------------------------------------------------------ démarrage / arrêt

// Impose les réglages du mode en cours (ou rend la main à l'utilisateur) et met à jour la pilule et l'éditeur.
function apply(push = true) {
  const m = activeMode();
  if (active && !m) { active = null; store.set('activeMode', null); } // mode supprimé ailleurs
  setOverrides(m ? Object.assign({}, ...Object.values(forcedBy(m))) : {}, push);
  renderPill();
  syncEditor();
}

// Étapes de démarrage au natif, puis le mode est « en cours ». Retourne le nombre d'étapes lancées.
function startMode(m) {
  const lines = m.steps
    .filter((s) => s.type !== 'open' || String(s.value).trim())
    .map((s) => `${s.type}\t${String(s.value).replace(/[\t\r\n]+/g, ' ').trim()}`);
  if (lines.length) send('routine', lines.join('\n'));
  active = { id: m.id, since: Date.now() };
  store.set('activeMode', active);
  apply();
  return lines.length;
}

// Arrête le mode en cours (ses comportements cessent). Retourne le mode arrêté, ou null s'il n'y en avait pas.
function stopMode() {
  const m = activeMode();
  active = null;
  store.set('activeMode', null);
  apply();
  return m;
}

// ------------------------------------------------------------------ déclencheur : ouverture d'une appli

// « C:\…\Teams.exe », « Teams.exe » et « teams » désignent la même appli.
const appId = (s) => String(s).trim().split(/[\\/]/).pop().toLowerCase().replace(/\.exe$/, '');
const appFile = (s) => String(s).trim().split(/[\\/]/).pop();

// Le natif surveille les processus de tous les modes (noms seulement) et prévient la page à chaque ouverture (`appstart`).
function syncWatch() {
  const names = new Set(modes.flatMap((m) => m.apps.map((a) => appFile(a).toLowerCase())).filter(Boolean));
  send('watch', [...names].join('\n'));
}

// Une appli surveillée vient de s'ouvrir : le premier mode qui l'attend démarre (sauf s'il est déjà en cours).
function onAppStart(app) {
  const id = appId(app || '');
  const m = id && modes.find((x) => x.apps.some((a) => appId(a) === id));
  if (!m || m.id === active?.id) return;
  startMode(m);
  const name = m.name.trim() || 'Mode';
  send('notify', `Mode « ${name} » démarré`, `${appFile(app)} s’est ouverte.`);
  toast(`Mode « ${name} » démarré (${appFile(app)})`);
}

// ------------------------------------------------------------------ pilule (barre du dock)

function renderPill() {
  const m = activeMode();
  const pill = $('#modePill');
  const was = !pill.hidden;
  pill.hidden = !m;
  document.body.classList.toggle('has-mode', !!m);
  if (!m) return;
  const name = m.name.trim() || 'Mode';
  $('#modeName').textContent = name;
  $('#modeOpen').title = `Mode « ${name} » en cours`;
  if (!was) { // petit rebond à l'apparition
    pill.classList.remove('pop-in');
    void pill.offsetWidth;
    pill.classList.add('pop-in');
  }
}

// ------------------------------------------------------------------ éditeur (Réglages)

function stepHTML(s, i) {
  const opts = (map, cur) => Object.entries(map).map(([k, v]) => `<option value="${k}"${k === cur ? ' selected' : ''}>${v}</option>`).join('');
  let control = '';
  if (s.type === 'open') {
    control = `<input class="step-value" value="${esc(s.value)}" placeholder="Appli, dossier, fichier ou https://…" spellcheck="false">
      <button type="button" class="ghost step-pick" data-act="pick" title="Choisir un fichier">Parcourir…</button>`;
  } else if (s.type === 'volume') {
    control = `<input class="step-value" type="range" min="0" max="100" step="5" value="${+s.value || 0}" style="--v:${+s.value || 0}%"><output>${+s.value || 0} %</output>`;
  } else if (CHOICES[s.type]) {
    control = `<select class="step-value">${opts(CHOICES[s.type], s.value)}</select>`;
  } else if (s.type === 'lock') {
    control = '<span class="hint">Verrouille la session (à mettre en dernier)</span>';
  } else {
    control = `<input class="step-value step-secs" type="number" min="0" max="60" value="${+s.value || 0}"><span class="hint">secondes</span>`;
  }
  return `<div class="step" data-i="${i}">
    <select class="step-type">${opts(STEP_TYPES, s.type)}</select>
    <div class="step-control">${control}</div>
    <button type="button" class="icon-btn" data-act="del-step" title="Retirer l'étape">${icon('x')}</button>
  </div>`;
}

const appsHTML = (m) => m.apps.map((a) => `<span class="amode-app">${esc(a)}<button type="button" data-act="del-app" data-app="${esc(a)}" title="Retirer">${icon('x')}</button></span>`).join('')
  || '<span class="hint">Aucune appli : le mode ne démarre qu’à la main ou par Aeria.</span>';

const runBtnHTML = (on) => (on ? `${icon('stop')}Arrêter` : `${icon('play')}Démarrer`);

function behaviorHTML(m) {
  return Object.entries(BEHAVIORS).map(([k, b]) => {
    const label = `${b.label} <span class="hint">${b.hint}</span>`;
    if (b.choices) {
      const opts = Object.entries(b.choices).map(([v, t]) => `<option value="${v}"${(m.on[k] || '') === v ? ' selected' : ''}>${t}</option>`).join('');
      return `<div class="toggle-row amode-opt"><span>${label}</span><select class="amode-sel" data-b="${k}">${opts}</select></div>`;
    }
    if (b.accent) {
      const dots = Object.entries(ACCENTS).map(([n, [a, c]]) => `<button type="button" data-act="accent" data-v="${n}" title="${n}" class="${m.on[k] === n ? 'on' : ''}" style="--a:${a};--b:${c}"></button>`).join('');
      return `<div class="toggle-row amode-opt"><span>${label}</span>
        <span class="swatches amode-swatches"><button type="button" data-act="accent" data-v="" title="Couleur inchangée" class="swatch-none${m.on[k] ? '' : ' on'}"></button>${dots}</span></div>`;
    }
    return `<label class="toggle-row amode-opt"><span>${label}</span>
      <span class="toggle"><input type="checkbox" data-b="${k}"${m.on[k] ? ' checked' : ''}><span></span></span></label>`;
  }).join('');
}

function renderModes() {
  const box = $('#modes');
  box.innerHTML = modes.map((m) => `
    <div class="amode${m.id === active?.id ? ' is-on' : ''}" data-id="${m.id}">
      <div class="amode-head">
        <input class="amode-name" value="${esc(m.name)}" placeholder="Nom du mode" spellcheck="false">
        <button type="button" class="${m.id === active?.id ? 'ghost' : 'primary'} amode-run" data-act="run">${runBtnHTML(m.id === active?.id)}</button>
        <button type="button" class="icon-btn del" data-act="del" title="Supprimer le mode">${icon('trash')}</button>
      </div>
      <div class="amode-sec">Démarre quand une de ces applis s’ouvre</div>
      <div class="amode-apps">${appsHTML(m)}</div>
      <div class="amode-app-add">
        <input class="amode-app-in" placeholder="Nom de l’appli, ex. teams.exe" spellcheck="false">
        <button type="button" class="ghost" data-act="add-app">Ajouter</button>
        <button type="button" class="ghost" data-act="pick-app">Parcourir…</button>
      </div>
      <div class="amode-sec">Au démarrage</div>
      <div class="steps">${m.steps.map(stepHTML).join('')}</div>
      <button type="button" class="ghost add-step" data-act="add-step">${icon('plus')}Étape</button>
      <div class="amode-sec">Tant que le mode est en cours</div>
      ${behaviorHTML(m)}
    </div>`).join('')
    || '<p class="hint modes-empty">Aucun mode pour l’instant.</p>';
}

// Démarrer / arrêter change juste l'état des blocs (sans les redessiner : pas de nouvelle animation).
function syncEditor() {
  for (const el of $$('#modes .amode')) {
    const on = el.dataset.id === active?.id;
    el.classList.toggle('is-on', on);
    const b = $('.amode-run', el);
    b.innerHTML = runBtnHTML(on);
    b.classList.toggle('primary', !on);
    b.classList.toggle('ghost', on);
  }
}

function setupModes() {
  const box = $('#modes');
  const at = (e) => {
    const el = e.target.closest('.amode');
    const m = modes.find((x) => x.id === el?.dataset.id);
    const stepEl = e.target.closest('.step');
    return { el, m, i: stepEl ? +stepEl.dataset.i : -1, stepEl };
  };

  $('#addMode').addEventListener('click', () => {
    modes.push(normalize({ name: modes.length ? 'Nouveau mode' : 'Travail', steps: [{ type: 'open', value: '' }, { type: 'volume', value: 20 }] }));
    save();
    renderModes();
    $$('.amode-name', box).pop()?.select();
  });

  box.addEventListener('input', (e) => {
    const { m, i, stepEl } = at(e);
    if (!m) return;
    if (e.target.classList.contains('amode-name')) {
      m.name = e.target.value;
      if (m.id === active?.id) renderPill();
    } else if (e.target.classList.contains('step-value') && i >= 0) {
      const s = m.steps[i];
      s.value = s.type === 'open' || CHOICES[s.type] ? e.target.value : +e.target.value;
      if (s.type === 'volume') {
        e.target.style.setProperty('--v', `${s.value}%`);
        $('output', stepEl).textContent = `${s.value} %`;
      }
    }
    save();
  });

  box.addEventListener('change', (e) => {
    const { m, i } = at(e);
    if (!m) return;
    if (e.target.dataset.b) { // comportement du mode : prend effet tout de suite si le mode est en cours
      m.on[e.target.dataset.b] = e.target.type === 'checkbox' ? e.target.checked : e.target.value;
      save();
      if (m.id === active?.id) apply();
      return;
    }
    if (i < 0) return;
    if (e.target.classList.contains('step-type')) {
      m.steps[i] = { type: e.target.value, value: DEFAULTS[e.target.value] };
      save();
      renderModes();
    } else if (e.target.tagName === 'SELECT') { // menus « Musique » et « Son »
      m.steps[i].value = e.target.value;
      save();
    }
  });

  box.addEventListener('click', async (e) => {
    const btn = e.target.closest('[data-act]');
    const { el, m, i } = at(e);
    if (!btn || !m) return;
    const act = btn.dataset.act;
    if (act === 'run') {
      if (m.id === active?.id) {
        stopMode();
        toast(`Mode « ${m.name} » arrêté`);
      } else {
        const n = startMode(m);
        toast(`Mode « ${m.name} » démarré${n ? '' : ' (aucune étape)'}`);
      }
    } else if (act === 'add-app' || act === 'del-app' || act === 'pick-app') {
      let file = '';
      if (act === 'del-app') {
        m.apps = m.apps.filter((a) => a !== btn.dataset.app);
      } else if (act === 'add-app') {
        const input = $('.amode-app-in', el);
        file = appFile(input.value);
        input.value = '';
      } else {
        try {
          const { path } = await ask('pickfile', [], 600000);
          file = path ? appFile(path) : '';
        } catch (err) {
          toast(err.message);
          return;
        }
      }
      if (file) {
        if (/\.(lnk|url)$/i.test(file)) { toast('Choisis l’exécutable de l’appli (.exe), pas un raccourci.'); return; }
        if (!m.apps.some((a) => appId(a) === appId(file))) m.apps.push(file);
      }
      $('.amode-apps', el).innerHTML = appsHTML(m);
      saveNow();
      syncWatch();
    } else if (act === 'accent') {
      m.on.accent = btn.dataset.v;
      $$('[data-act=accent]', el).forEach((b) => b.classList.toggle('on', b === btn));
      save();
      if (m.id === active?.id) apply();
    } else if (act === 'del') {
      if (m.id === active?.id) stopMode();
      modes = modes.filter((x) => x !== m);
      saveNow();
      syncWatch();
      el.classList.add('leaving');
      setTimeout(renderModes, document.body.classList.contains('reduce') ? 0 : 300);
    } else if (act === 'add-step') {
      m.steps.push({ type: 'open', value: '' });
      save();
      renderModes();
    } else if (act === 'del-step') {
      m.steps.splice(i, 1);
      save();
      renderModes();
    } else if (act === 'pick') {
      try {
        const { path } = await ask('pickfile', [], 600000);
        if (!path) return;
        m.steps[i].value = path;
        save();
        renderModes();
      } catch (err) {
        toast(err.message);
      }
    }
  });

  box.addEventListener('keydown', (e) => { // Entrée dans le champ d'une appli = Ajouter
    if (e.key !== 'Enter' || !e.target.classList.contains('amode-app-in')) return;
    e.preventDefault();
    $('[data-act=add-app]', e.target.closest('.amode')).click();
  });

  $('#modeStop').addEventListener('click', () => {
    const m = stopMode();
    if (m) toast(`Mode « ${m.name} » arrêté`);
  });
  if (store.get('modes', null) === null && modes.length) saveNow(); // routines d'avant, reprises une fois
  renderModes();
  apply(false); // main.js envoie la config après « ready »
}

export { findMode, startMode, stopMode, activeMode, modeNames, behaviorNames, setupModes, renderModes, syncWatch, onAppStart };
