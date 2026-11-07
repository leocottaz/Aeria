// Mini-dock : quand le dock est rangé, le natif réduit la fenêtre à une pastille (état « mini »).
// Elle montre les rappels qui ont sonné (cloche + nombre), le temps du minuteur et/ou une ligne de
// texte : une notification (rappel, minuteur, mode, mise à jour…) qui prend la place des paroles
// quelques secondes, avec la même animation, ou la ligne de paroles en cours. On envoie au natif la
// largeur voulue en DIP ; 0 = rien à montrer, le dock se range comme avant. Les autres modules ne font
// que lui donner leur contenu (setMiniAlerts, setMiniTimer, setMiniLyric, showMiniNotice, notify).
import { $, icon } from '../core/util.js';
import { send } from '../core/bridge.js';
import { S } from '../core/state.js';

const MAX_W = 560;
// timer : { text, paused } ; lyric : { text, gap } ou null ; notice : { text, icon, tab, left, born } ou null
const state = { alerts: 0, timer: null, lyric: null, notice: null };
let sent = '';
let ctx = null;

function textWidth(el, text, weight) {
  ctx ??= document.createElement('canvas').getContext('2d');
  const cs = getComputedStyle(el);
  ctx.font = `${weight} ${cs.fontSize} ${cs.fontFamily}`; // le canvas refuse la graisse 650 et l'interligne
  return ctx.measureText(text).width;
}

// La ligne affichée : la notification passe devant les paroles.
const currentLine = () => state.notice || state.lyric;

// Carte de notification (logo, titre, texte) : largeur et hauteur, d'après les marges de .pill.has-card et
// .pc-text dans css/dock.css (14 + logo 36 + 12 + texte + 18 ; hauteur : 12 + titre 17 + 2 + lignes de 16 + 12).
function cardSize(card) {
  const titleW = card.title ? textWidth($('#pcTitle'), card.title, 650) : 0;
  const bodyW = card.text ? textWidth($('#pcBody'), card.text, 500) * 1.06 : 0; // 6 % de marge : les retours à la ligne mangent de la place
  const textW = Math.min(300, Math.max(160, titleW, bodyW));
  const width = Math.min(440, Math.max(264, Math.ceil((14 + 36 + 12 + textW + 18 + 4) / 8) * 8));
  const avail = width - 14 - 36 - 12 - 18;
  const lines = bodyW ? Math.min(2, Math.ceil(bodyW / avail)) : 0;
  return { width, height: Math.max(56, 24 + 17 + (lines ? 2 + 16 * lines : 0)) };
}

const currentCard = () => state.notice?.card || null;

function wantedHeight() {
  const card = currentCard();
  return card ? cardSize(card).height : 40; // 40 = kMiniH
}

// Doit suivre les marges de .pill dans css/dock.css.
function wantedWidth() {
  const { alerts, timer } = state;
  const lyric = currentLine();
  const card = currentCard();
  if (card) return cardSize(card).width;
  if (!alerts && !timer && !lyric) return 0;
  const chip = alerts ? 8 + 15 + 4 + textWidth($('#pillCount'), String(alerts), 700) + 8 : 0;
  // Chiffres mesurés comme des 0 : la largeur ne doit pas bouger à chaque seconde.
  const time = timer ? 4 + 15 + 5 + textWidth($('#pillTime'), timer.text.replace(/\d/g, '0'), 600) + 4 : 0;
  if (!lyric) return Math.ceil(7 + chip + (chip && time ? 8 : 0) + time + 7); // la petite pastille : cloche et/ou minuteur
  const w = 7 + (chip ? chip + 8 : 0) + (time ? time + 10 : 0) + 26 + 10 + textWidth($('#pillLine'), lyric.text, 600) + 16 + 8;
  return Math.min(MAX_W, Math.max(180, Math.ceil(w / 8) * 8)); // par pas de 8 : moins de redimensionnements
}

// Affiche la ligne courante : l'ancienne monte et s'efface pendant que la nouvelle arrive.
function paintLine() {
  const box = $('#pillLine');
  const cur = currentLine();
  const key = cur ? `${state.notice ? 'n' : 'l'}:${cur.text}` : '';
  if (cur && box.dataset.text !== key) {
    box.dataset.text = key;
    for (const old of box.querySelectorAll('span:not(.out)')) {
      old.classList.add('out');
      setTimeout(() => old.remove(), 400);
    }
    const span = document.createElement('span');
    span.textContent = cur.text;
    span.classList.toggle('gap', !!cur.gap);
    box.append(span);
  }
  if (state.notice) $('#pillIc').innerHTML = icon(state.notice.icon);
  paintCard();
}

// Carte de notification : logo (sinon l'initiale de l'appli), titre, texte. Elle réapparaît (animation) à chaque nouvelle carte.
let paintedCard = null;
function paintCard() {
  const card = currentCard();
  if (card === paintedCard) return;
  paintedCard = card;
  if (!card) return;
  const img = $('#pcImg');
  img.hidden = !card.icon;
  if (card.icon) img.src = card.icon; else img.removeAttribute('src');
  $('#pcIcon').classList.toggle('has-img', !!card.icon);
  $('#pcInitial').textContent = card.icon ? '' : ([...(card.app || '?')][0] || '?').toUpperCase();
  $('#pcTitle').textContent = card.title || card.text || '';
  $('#pcBody').textContent = card.title ? card.text || '' : '';
  $('#pillCard').title = card.app || '';
  const el = $('#pillCard');
  el.style.animation = 'none';
  void el.offsetWidth;
  el.style.animation = '';
}

function update() {
  const pill = $('#pill');
  pill.classList.toggle('has-alert', state.alerts > 0);
  pill.classList.toggle('has-timer', !!state.timer);
  pill.classList.toggle('has-lyric', !!currentLine());
  pill.classList.toggle('has-notice', !!state.notice && !state.notice.card);
  pill.classList.toggle('has-card', !!currentCard());
  paintLine();
  const width = wantedWidth();
  const height = width ? wantedHeight() : 40;
  // largeur, 1 = notification (le natif affiche la pastille même devant une appli plein écran), hauteur.
  const msg = width ? `${width},${state.notice ? 1 : 0},${height}` : '0';
  if (msg === sent) return;
  sent = msg;
  if (width) {
    const root = document.documentElement.style;
    root.setProperty('--mini-w', `${width}px`);
    root.setProperty('--mini-h', `${height}px`);
  }
  send('mini', msg);
}

// Nombre de rappels qui ont sonné et attendent « Fait » ou un report.
function setMiniAlerts(count) {
  if (count === state.alerts) return;
  const more = count > state.alerts;
  state.alerts = count;
  $('#pillCount').textContent = count;
  if (more) { // la cloche sonne à chaque nouveau rappel
    const bell = $('#pill .pill-alert');
    bell.classList.remove('ring');
    void bell.offsetWidth;
    bell.classList.add('ring');
  }
  update();
}

// Temps du minuteur ({ text, paused }), ou null quand il n'y a rien à montrer.
function setMiniTimer(timer) {
  if (timer) {
    $('#pillTime').textContent = timer.text;
    $('#pillTimer').classList.toggle('paused', timer.paused);
  }
  state.timer = timer;
  update();
}

// Ligne de paroles en cours ({ text, gap }), ou null quand il n'y en a pas à montrer.
function setMiniLyric(lyric) {
  state.lyric = lyric;
  update();
}

// --- notifications
// Une notification reste affichée ~5 à 10 s, décomptées seulement tant que la pastille est visible
// (dock en « mini ») : si le dock est ouvert elle disparaît (l'alerte y est déjà), s'il est occupé
// (carte musique, barre à peine sortie…) elle attend. Garde-fou : 60 s au plus.
const TICK = 250;
let clock = 0;

function clearNotice() {
  clearInterval(clock);
  clock = 0;
  if (!state.notice) return;
  state.notice = null;
  update();
}

function tickNotice() {
  const n = state.notice;
  if (!n || Date.now() - n.born > 60000) return clearNotice();
  if (!document.body.classList.contains('st-mini')) return;
  n.left -= TICK;
  if (n.left <= 0) clearNotice();
}

// Texte d'une notification dans la pastille. opts : { icon, tab (onglet ouvert au clic), ms,
// card : { app, title, text, icon (data URL) } pour une carte avec le logo de l'appli au lieu d'une ligne }.
function showMiniNotice(text, opts = {}) {
  if (!text || S.st === 'bar' || S.st === 'panel') return; // dock ouvert : on voit déjà l'information
  const ms = opts.ms ?? Math.min(10000, Math.max(5000, 4500 + text.length * 45));
  state.notice = { text, icon: opts.icon || 'bell', tab: opts.tab || '', card: opts.card || null, left: ms, born: Date.now() };
  clearInterval(clock);
  clock = setInterval(tickNotice, TICK);
  update();
}

// Notification de l'application (mode démarré, mise à jour…) : texte dans la pastille + son (le natif
// vérifie le réglage). Les rappels et le minuteur, eux, sonnent côté natif et appellent showMiniNotice.
function notify(title, text, opts) {
  showMiniNotice(text ? `${title} · ${text}` : title, opts);
  send('chime');
}

// Onglet à ouvrir quand on clique sur la pastille.
const miniTab = () => (state.alerts ? 'reminders' : state.notice ? state.notice.tab || 'today' : state.lyric ? 'music' : 'timer');

// Le dock s'ouvre : la notification a fait son temps.
document.body.addEventListener('dockstate', (e) => {
  if (e.detail.st === 'bar' || e.detail.st === 'panel') clearNotice();
});

export { setMiniAlerts, setMiniTimer, setMiniLyric, showMiniNotice, notify, miniTab };
