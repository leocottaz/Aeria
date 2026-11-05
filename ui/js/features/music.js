// Lecteur : affiche la musique suivie par le natif (src/features/media.cpp, {"t":"media"}),
// la pilote (lecture/pause, piste suivante/précédente, position) et affiche les
// paroles, récupérées à la demande sur LRCLIB ou lrcmux et synchronisées quand c'est possible.
import { $, $$, esc, clamp, store } from '../core/util.js';
import { send } from '../core/bridge.js';
import { settings, eff, setSetting, onSettingChange } from '../core/settings.js';
import { S } from '../core/state.js';
import { setMiniLyric } from '../dock/mini.js';

// ------------------------------------------------------------------ état

const M = {
  active: false, app: '', title: '', artist: '', album: '', status: 'stopped',
  pos: 0, dur: 0, at: 0, canNext: false, canPrev: false, canSeek: false, art: '',
};
const lyrics = { key: '', run: null, dur: 0, state: 'idle', lines: [], synced: false, current: -1 };
const lyricsCache = new Map();
let seeking = null; // position (ms) pendant qu'on fait glisser la barre

const trackKey = (m) => (m.active ? `${m.title}\u001f${m.artist}\u001f${m.album}` : '');
const isPlaying = () => M.active && M.status === 'playing';

// Position courante, interpolée depuis la dernière mise à jour de Windows.
function currentPos() {
  if (seeking != null) return seeking;
  let p = M.pos;
  if (isPlaying()) p += Date.now() - M.at;
  return M.dur > 0 ? clamp(p, 0, M.dur) : Math.max(0, p);
}

const fmtDur = (ms) => {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

function sourceName(app) {
  const a = (app || '').toLowerCase();
  if (a.includes('deezer')) return 'Deezer';
  if (a.includes('spotify')) return 'Spotify';
  if (a.includes('msedge') || a.includes('edge')) return 'Microsoft Edge';
  if (a.includes('chrome')) return 'Google Chrome';
  if (a.includes('firefox')) return 'Firefox';
  return app ? app.split(/[\\!]/).pop().replace(/\.exe$/i, '') : '';
}

// ------------------------------------------------------------------ messages du natif

function onMedia(m) {
  const prevKey = trackKey(M);
  const prevArt = M.art;
  if (!m.active) {
    Object.assign(M, { active: false, status: 'stopped' });
  } else {
    // Windows date sa position ; si l'horodatage est absent ou incohérent, on prend l'heure de réception.
    const at = m.at && Math.abs(Date.now() - m.at) < 6 * 3600e3 ? m.at : Date.now();
    Object.assign(M, { ...m, at, art: 'art' in m ? m.art : M.art });
  }
  const changed = trackKey(M) !== prevKey;
  if (changed) {
    applyArt();
    if (M.active) loadLyrics();
  } else if (M.art !== prevArt) {
    applyArt(); // la pochette arrive parfois après le titre
  } else if (M.active && M.dur > 0 && Math.abs(M.dur - lyrics.dur) > 8000) {
    // Titre et durée ne changent pas au même instant : la recherche est partie avec une durée périmée.
    lyricsCache.delete(trackKey(M));
    loadLyrics();
  }
  render();
  // Nouveau morceau (ou musique qui démarre) : la carte musique descend un instant pour le montrer.
  if (changed && isPlaying() && eff('musicReveal')) { // un mode peut le couper (modes.js)
    const w = cardWidth();
    document.documentElement.style.setProperty('--mcard-w', `${w}px`);
    send('revealmusic', '3000', String(w));
  }
  ensureLoop();
}

// ------------------------------------------------------------------ rendu

function setText(el, text) {
  if (el.textContent !== text) el.textContent = text;
}

function applyArt() {
  for (const img of [$('#npArt'), $('#mArt'), $('#pillArt'), $('#mcardArt')]) {
    if (M.art) img.src = M.art;
    else img.removeAttribute('src');
    img.hidden = !M.art;
  }
  $('#musicBg').style.backgroundImage = M.art ? `url("${M.art}")` : '';
  tintFromArt();
}

// Accent tiré de la pochette : même teinte, mais saturation et luminosité bornées pour que le texte
// blanc des boutons et les icônes restent lisibles sur le verre (une pochette grise ou claire donnerait un bouton pâle).
function accentFrom(r, g, b, hueShift) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d) h = max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  const l = (max + min) / 2, sat = d ? d / (1 - Math.abs(2 * l - 1)) : 0;
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  return `hsl(${Math.round(h * 60 + hueShift) % 360}, ${Math.round(clamp(sat * 1.3, 0, 0.75) * 100)}%, ${Math.round(clamp(l + 0.05, 0.40, 0.47) * 100)}%)`;
}

// Couleur dominante de la pochette : teinte le lecteur et le mini-lecteur.
function tintFromArt() {
  const root = document.documentElement.style;
  if (!M.art) { for (const k of ['--m-tint', '--m-accent', '--m-accent2']) root.removeProperty(k); return; }
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = c.height = 12;
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0, 12, 12);
    const d = g.getImageData(0, 0, 12, 12).data;
    let r = 0, gr = 0, b = 0, w = 0;
    for (let i = 0; i < d.length; i += 4) {
      const max = Math.max(d[i], d[i + 1], d[i + 2]), min = Math.min(d[i], d[i + 1], d[i + 2]);
      const weight = 1 + (max - min) / 32; // privilégie les pixels colorés
      r += d[i] * weight; gr += d[i + 1] * weight; b += d[i + 2] * weight; w += weight;
    }
    root.setProperty('--m-tint', `rgb(${Math.round(r / w)}, ${Math.round(gr / w)}, ${Math.round(b / w)})`);
    root.setProperty('--m-accent', accentFrom(r / w, gr / w, b / w, 0));
    root.setProperty('--m-accent2', accentFrom(r / w, gr / w, b / w, 32));
  };
  img.src = M.art;
}

function render() {
  const active = M.active;
  document.body.classList.toggle('has-music', active);
  document.body.classList.toggle('music-playing', isPlaying());
  $('#np').hidden = !active;
  $('#askInput').placeholder = active ? 'Demander…' : 'Demander à Aeria…';

  setText($('#npTitle'), M.title);
  setText($('#npArtist'), M.artist);
  setText($('#mTitle'), M.title);
  setText($('#mArtist'), M.artist);
  setText($('#mAlbum'), M.album);
  setText($('#mcardTitle'), M.title);
  setText($('#mcardArtist'), M.artist);
  setText($('#mDur'), M.dur ? fmtDur(M.dur) : '–:––');
  setText($('#musicSource'), active ? `En lecture sur ${sourceName(M.app)}` : 'En attente de musique');
  $('#musicEmptyText').textContent = settings.musicSource === 'deezer'
    ? 'Lance un titre sur Deezer : le lecteur apparaîtra ici et dans la barre.'
    : 'Lance de la musique dans n’importe quelle appli : le lecteur apparaîtra ici et dans la barre.';

  $('#musicView').classList.toggle('is-empty', !active);
  $('#musicView').classList.toggle('with-lyrics', active && settings.lyricsOpen);
  $('#lyricsBtn').classList.toggle('on', settings.lyricsOpen);
  $('#lyricsBtn').hidden = !active;
  $('#mPrev').disabled = $('#mcardPrev').disabled = !M.canPrev;
  $('#mNext').disabled = $('#mcardNext').disabled = !M.canNext;
  $('#mBar').classList.toggle('disabled', !M.canSeek);
  for (const el of [$('#np'), $('#mPlay'), $('#mcardPlay')]) el.classList.toggle('playing', isPlaying());
  updateMarquee();
  tickPosition();
  updateMini();
}

// Largeur de la carte musique (DIP) : juste assez pour le titre, entre 280 et 380.
// Marges fixes (cf. .mcard dans css/music.css) : 10 + pochette 54 + 11 + 6 + boutons 108 + 12, et 6 de jeu.
let measure = null;
function cardWidth() {
  measure ??= document.createElement('canvas').getContext('2d');
  // (le canvas refuse la graisse 650 avec l'interligne : on compose la police à la main)
  const w = (el, weight, size, text) => {
    measure.font = `${weight} ${size}px ${getComputedStyle(el).fontFamily}`;
    return measure.measureText(text).width;
  };
  const text = Math.max(w($('#mcardTitle'), 650, 14, M.title), w($('#mcardArtist'), 400, 12, M.artist));
  return clamp(Math.ceil((207 + text) / 4) * 4, 280, 380);
}

// Mini-dock (js/dock/mini.js) : ligne en cours quand on écoute avec les paroles activées et synchronisées.
function updateMini() {
  const live = isPlaying() && settings.lyricsOpen && eff('miniLyrics') && lyrics.state === 'ready' && lyrics.synced;
  const line = live ? lyrics.lines[lyrics.current] : null;
  setMiniLyric(live ? { text: line?.text || '♪', gap: !line?.text } : null);
}

// Titre trop long pour le mini-lecteur : il défile doucement.
function updateMarquee() {
  const box = $('.np-title'), span = $('#npTitle');
  requestAnimationFrame(() => {
    const overflow = span.scrollWidth - box.clientWidth;
    box.classList.toggle('marquee', overflow > 4);
    box.style.setProperty('--dist', `${-Math.max(0, overflow)}px`);
    box.style.setProperty('--dur', `${Math.max(6, overflow / 18)}s`);
  });
}

function tickPosition() {
  const pos = currentPos();
  const frac = M.dur > 0 ? pos / M.dur : 0;
  $('#npFill').style.transform = `scaleX(${frac})`;
  $('#mcardFill').style.transform = `scaleX(${frac})`;
  $('#mFill').style.width = `${frac * 100}%`;
  setText($('#mPos'), fmtDur(pos));
  syncLyrics(pos);
}

// Boucle d'animation, seulement quand le dock est visible et qu'une musique est suivie.
// En mini-dock, seules les paroles comptent : 10 fois par seconde suffit.
let loopId = 0;
function ensureLoop() {
  if (loopId) return;
  const step = () => {
    loopId = 0;
    const visible = S.st === 'bar' || S.st === 'panel' || S.st === 'mini' || S.st === 'music';
    if (!M.active || !visible) return;
    tickPosition();
    loopId = S.st === 'mini' ? setTimeout(step, 100) : requestAnimationFrame(step);
  };
  loopId = requestAnimationFrame(step);
}

// ------------------------------------------------------------------ paroles (LRCLIB, puis lrcmux)

// Retire ce qui gêne la recherche : « (feat. …) », « - Remastered 2011 », « (Radio Edit) », « [Live] »…
const VARIANT = 'remaster|version|edit|live|mix|mono|stereo|deluxe|bonus|explicit|clean|acousti|extended|instrumental'
  + '|from|sped|slowed|reverb|soundtrack|bande originale|extrait'
  + '|official|officiel|clip|vid[ée]o|audio|lyric|paroles|visuali[sz]er';
function cleanTitle(t) {
  const out = t.replace(/\s*[([](feat|ft|featuring|with|avec)\b\.?[^)\]]*[)\]]/gi, '')
    .replace(/\s+(feat|ft|featuring)\.?\s.*$/i, '')
    .replace(new RegExp(`\\s*[([][^)\\]]*\\b(${VARIANT})[^)\\]]*[)\\]]`, 'gi'), '')
    .replace(new RegExp(`\\s+[-–]\\s+.*\\b(${VARIANT}).*$`, 'i'), '')
    .replace(/\s*\[[^\]]*\]/g, '')
    .trim();
  return out || t.trim();
}

// Premier artiste de « A, B », « A feat. B », « A & B »…
const mainArtist = (a) => a.split(/\s*(?:[,;/]|\s&\s|\sx\s|\s(?:feat|ft|featuring)\.?\s|\set\s|\swith\s)\s*/i)[0].trim() || a;

// Comparaison tolérante : sans accents, casse ni ponctuation.
const norm = (s) => (s || '').normalize('NFD').replace(/\p{M}/gu, '').toLowerCase()
  .replace(/[^\p{L}\p{N}]+/gu, ' ').trim();

// Format LRC : « [mm:ss.xx]texte », éventuellement plusieurs horodatages par ligne.
function parseLrc(text) {
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const stamps = [...raw.matchAll(/\[(\d+):(\d+(?:[.:]\d+)?)\]/g)];
    if (!stamps.length) continue;
    const line = raw.replace(/\[[^\]]*\]/g, '').trim();
    for (const s of stamps) out.push({ t: (+s[1] * 60 + parseFloat(s[2].replace(':', '.'))) * 1000, text: line });
  }
  return out.sort((a, b) => a.t - b.t);
}

const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));
const LYRICS_GAP = 350;                       // ms entre deux requêtes (conseil de la doc LRCLIB : 200 à 500)
const LYRICS_PATIENCE = [0, 5, 15, 40, 90];   // s avant chaque passe : les 503 durent jusqu'à une minute ou deux

// Deux sources, même API. `next` = heure de la prochaine requête permise.
// lrcmux agrège d'autres catalogues : aucun en-tête personnalisé, son CORS les refuse.
const LRCLIB = {
  base: 'https://lrclib.net/api/', timeout: 10000, next: 0,
  headers: { 'Lrclib-Client': 'Aeria (https://github.com/leocottaz/Aeria)' },
};
const LRCMUX = { base: 'https://api.lrcmux.dev/compat/lrclib/api/', timeout: 15000, next: 0, headers: {} };

// Une requête à la fois par source, en respectant Retry-After (429 et 503). null = introuvable (404).
// Pas de relance ici : fetchLyrics refait une passe plus tard sur tout ce qui a échoué.
async function askLyrics(source, path, params) {
  const at = Math.max(Date.now(), source.next);
  source.next = at + LYRICS_GAP;
  await sleep(at - Date.now());
  const url = `${source.base}${path}?${new URLSearchParams(params)}`;
  const r = await fetch(url, { headers: source.headers, signal: AbortSignal.timeout(source.timeout) });
  if (r.status === 404) return null;
  if (r.status === 429 || r.status === 503) {
    const wait = (parseFloat(r.headers.get('Retry-After')) || 1) * 1000 * (1 + Math.random() * 0.3);
    source.next = Math.max(source.next, Date.now() + wait);
  }
  if (!r.ok) throw new Error(`Paroles ${r.status}`);
  return r.json();
}

// Sans album, le morceau vient en général d'un site vidéo : l'artiste reçu est le nom de la chaîne
// (« Adele - Topic », « AdeleVEVO ») et le titre celui de la vidéo (« Adele - Hello (Clip officiel) »).
function guessTrack(track) {
  const artist = track.artist.replace(/\s+-\s+topic$/i, '').replace(/\s*vevo$/i, '')
    .replace(/\s+(official|officiel(le)?)$/i, '').trim() || track.artist;
  if (track.album) return { title: cleanTitle(track.title), artist, web: false };
  const title = cleanTitle(track.title.replace(/\s*[|｜].*$/, ''));
  const split = title.match(/^(.+?)\s+[-–—]\s+(.+)$/);
  return split ? { title: cleanTitle(split[2]), artist: split[1], web: true } : { title, artist, web: true };
}

// Plusieurs essais du plus précis au plus large ; on s'arrête dès qu'une version a la
// bonne durée (paroles synchronisées). Sinon, on garde la meilleure version trouvée
// mais sans synchronisation, plutôt que d'annoncer « introuvables ». Ce qui échoue
// (serveur surchargé) est rejoué aux passes suivantes tant que le morceau n'a pas changé.
// lrcmux passe après LRCLIB, ou dès que LRCLIB échoue pour ne pas subir toute sa patience.
async function fetchLyrics(track) {
  const secs = Math.round(track.dur / 1000);
  const { title, artist: shown, web } = guessTrack(track);
  const artist = mainArtist(shown);
  const hasText = (x) => x && (x.instrumental || x.syncedLyrics || x.plainLyrics);
  const diff = (x) => (secs > 0 ? Math.abs((x.duration || 0) - secs) : 0);
  const nt = norm(title), na = norm(artist);
  const sameTitle = (x) => { const c = norm(cleanTitle(x.trackName || '')); return c === nt || c.startsWith(`${nt} `) || nt.startsWith(`${c} `); };
  const sameArtist = (x) => { const c = norm(x.artistName); return c.includes(na) || (c && na.includes(c)); };

  // Même morceau, autre artiste affiché (ex. nom transcrit) : la durée exacte suffit.
  const sameTrack = (x) => sameTitle(x) && (sameArtist(x) || (secs > 0 && diff(x) <= 2));
  const plain = (s) => s.normalize('NFD').replace(/\p{M}/gu, '').replace(/['’]/g, '');

  let pending = [];
  if (secs > 0 && secs <= 3600 && artist) { // bornes de /api/get
    if (!web) pending.push([LRCLIB, 'get', { track_name: track.title, artist_name: track.artist, album_name: track.album, duration: secs }]);
    if (web || title !== track.title || artist !== track.artist) pending.push([LRCLIB, 'get', { track_name: title, artist_name: artist, duration: secs }]);
  }
  if (shown) pending.push([LRCLIB, 'search', { track_name: title, artist_name: shown }]);
  if (artist !== shown) pending.push([LRCLIB, 'search', { track_name: title, artist_name: artist }]);
  if (plain(title) !== title || plain(artist) !== artist) pending.push([LRCLIB, 'search', { track_name: plain(title), artist_name: plain(artist) }]);
  pending.push([LRCLIB, 'search', { q: `${title} ${artist}`.trim() }]);
  pending.push([LRCLIB, 'search', { track_name: title }]);
  if (artist) pending.push([LRCMUX, 'get', { track_name: title, artist_name: artist, ...(secs > 0 && { duration: secs }) }]);

  let fallback = null;
  for (let pass = 0; pass < LYRICS_PATIENCE.length && pending.length; pass++) {
    await sleep(LYRICS_PATIENCE[pass] * 1000);
    const failed = [];
    const queue = [...pending];
    while (queue.length) {
      if (track.stale()) throw new Error('piste changée'); // pas de mise en cache
      const attempt = queue.shift();
      const [source, path, params] = attempt;
      let res;
      try {
        res = await askLyrics(source, path, params);
      } catch {
        failed.push(attempt);
        const mux = queue.findIndex(([s]) => s === LRCMUX);
        if (mux > 0) queue.unshift(...queue.splice(mux, 1));
        continue;
      }
      const list = (Array.isArray(res) ? res : res ? [res] : []).filter(hasText);
      // Les recherches larges et l'agrégateur ramènent d'autres morceaux : on vérifie titre et artiste.
      const ok = (source === LRCLIB && (path === 'get' || params.artist_name) ? list : list.filter(sameTrack))
        .sort((a, b) => diff(a) - diff(b));
      const best = ok.find((x) => x.syncedLyrics || x.instrumental);
      if (best && diff(best) <= 8) return best;
      if (ok[0] && (!fallback || diff(ok[0]) < diff(fallback))) fallback = ok[0];
    }
    pending = failed;
    if (fallback && pass >= 1) break; // on a de quoi afficher : inutile de faire attendre plus
  }
  if (fallback) return { ...fallback, syncedLyrics: null, plainLyrics: fallback.plainLyrics || stripLrc(fallback.syncedLyrics) };
  if (pending.length) throw new Error('Sources de paroles injoignables'); // erreur : pas de « introuvable » mis en cache
  return null;
}

const stripLrc = (text) => (text || '').split(/\r?\n/).map((l) => l.replace(/\[[^\]]*\]/g, '').trim()).join('\n');

// Garde la trace des morceaux sans paroles synchronisées (métadonnées seulement, jamais les
// paroles) pour comprendre les échecs réels : store `lyricsMisses`, lisible avec cdp.ps1.
function noteMiss(track, state) {
  const { title, artist, album, dur } = track;
  const misses = store.get('lyricsMisses', []).filter((m) => m.title !== title || m.artist !== artist);
  store.set('lyricsMisses', [{ title, artist, album, dur, state, at: Date.now() }, ...misses].slice(0, 40));
}

async function loadLyrics() {
  const key = trackKey(M);
  const run = {}; // chaque chargement a son jeton : un ancien ne doit plus rien écrire
  const track = { title: M.title, artist: M.artist, album: M.album, dur: M.dur, stale: () => lyrics.run !== run };
  Object.assign(lyrics, { key, run, dur: M.dur, state: 'loading', lines: [], synced: false, current: -1 });
  renderLyrics();
  let result = lyricsCache.get(key);
  if (!result) {
    try {
      const data = await fetchLyrics(track);
      if (track.stale()) return;
      if (!data) result = { state: 'none' };
      else if (data.instrumental) result = { state: 'instrumental' };
      else if (data.syncedLyrics) result = { state: 'ready', synced: true, lines: parseLrc(data.syncedLyrics) };
      else if (data.plainLyrics) result = { state: 'ready', synced: false, lines: data.plainLyrics.split(/\r?\n/).map((text) => ({ t: 0, text })) };
      else result = { state: 'none' };
      lyricsCache.set(key, result);
    } catch {
      result = { state: 'error' };
    }
    if (track.stale()) return;
    if (!result.synced && result.state !== 'instrumental') noteMiss(track, result.state);
  }
  if (track.stale()) return; // la piste a changé entre-temps
  Object.assign(lyrics, { synced: false, lines: [], ...result, current: -1 });
  renderLyrics();
  syncLyrics(currentPos(), true);
}

function renderLyrics() {
  const box = $('#lyricsScroll');
  const msg = (title, text = '') => `<div class="lyrics-msg"><b>${title}</b>${text ? `<p>${text}</p>` : ''}</div>`;
  if (lyrics.state === 'loading') {
    box.innerHTML = '<div class="lyrics-loading">' + '<span></span>'.repeat(6) + '</div>';
  } else if (lyrics.state === 'none') {
    box.innerHTML = msg('Paroles introuvables', 'Aucune source ne connaît encore ce titre.');
  } else if (lyrics.state === 'instrumental') {
    box.innerHTML = msg('♪ Instrumental');
  } else if (lyrics.state === 'error') {
    box.innerHTML = msg('Paroles indisponibles', 'Impossible de joindre les sources de paroles pour le moment.');
  } else if (lyrics.state === 'ready') {
    box.classList.toggle('synced', lyrics.synced);
    box.innerHTML = '<div class="lyrics-pad"></div>'
      + lyrics.lines.map((l, i) => `<p data-i="${i}"${l.text ? '' : ' class="gap"'}>${esc(l.text) || '♪'}</p>`).join('')
      + '<div class="lyrics-pad"></div>';
  } else {
    box.innerHTML = '';
  }
  updateMini();
}

// Met en avant la ligne en cours et la garde au centre.
function syncLyrics(pos, instant = false) {
  if (lyrics.state !== 'ready' || !lyrics.synced) return;
  let i = -1;
  for (let k = 0; k < lyrics.lines.length && lyrics.lines[k].t <= pos + 150; k++) i = k;
  if (i === lyrics.current && !instant) return;
  const box = $('#lyricsScroll');
  box.querySelector('p.cur')?.classList.remove('cur');
  $$('p.past', box).forEach((p) => { if (+p.dataset.i >= i) p.classList.remove('past'); });
  for (let k = 0; k < i; k++) box.querySelector(`p[data-i="${k}"]`)?.classList.add('past');
  lyrics.current = i;
  updateMini();
  const el = i >= 0 ? box.querySelector(`p[data-i="${i}"]`) : null;
  if (!el) return;
  el.classList.add('cur');
  if (!box.matches(':hover') || instant) {
    const top = el.offsetTop - box.clientHeight / 2 + el.offsetHeight / 2;
    box.scrollTo({ top, behavior: instant || settings.reduceMotion ? 'auto' : 'smooth' });
  }
}

// ------------------------------------------------------------------ contrôles

function seekTo(ms) {
  if (!M.canSeek) return;
  ms = clamp(Math.round(ms), 0, M.dur || ms);
  Object.assign(M, { pos: ms, at: Date.now() });
  send('media', 'seek', ms);
  tickPosition();
}

function toggle() {
  // Bascule optimiste : l'état réel arrive dans la foulée.
  Object.assign(M, { pos: currentPos(), at: Date.now(), status: isPlaying() ? 'paused' : 'playing' });
  send('media', 'toggle');
  render();
  ensureLoop();
}

function setupSeekBar() {
  const bar = $('#mBar');
  const at = (e) => {
    const r = bar.getBoundingClientRect();
    return clamp((e.clientX - r.left) / r.width, 0, 1) * M.dur;
  };
  bar.addEventListener('pointerdown', (e) => {
    if (!M.canSeek || !M.dur) return;
    bar.setPointerCapture(e.pointerId);
    bar.classList.add('dragging');
    seeking = at(e);
    tickPosition();
  });
  bar.addEventListener('pointermove', (e) => {
    if (seeking == null) return;
    seeking = at(e);
    tickPosition();
  });
  const end = (e) => {
    if (seeking == null) return;
    const target = at(e);
    seeking = null;
    bar.classList.remove('dragging');
    seekTo(target);
  };
  bar.addEventListener('pointerup', end);
  bar.addEventListener('pointercancel', () => { seeking = null; bar.classList.remove('dragging'); });
}

function setupMusic() {
  $('#npPlay').addEventListener('click', toggle);
  $('#mPlay').addEventListener('click', toggle);
  $('#mcardPlay').addEventListener('click', toggle);
  for (const id of ['#mPrev', '#mcardPrev']) $(id).addEventListener('click', () => send('media', 'prev'));
  for (const id of ['#mNext', '#mcardNext']) $(id).addEventListener('click', () => send('media', 'next'));
  $('#lyricsBtn').addEventListener('click', () => { setSetting('lyricsOpen', !settings.lyricsOpen); render(); syncLyrics(currentPos(), true); });
  $('#openDeezer').addEventListener('click', () => send('openurl', 'https://www.deezer.com/'));
  document.querySelectorAll('.lyrics-credit a').forEach((a) => a.addEventListener('click', () => send('openurl', a.dataset.url)));
  $('#lyricsScroll').addEventListener('click', (e) => {
    const p = e.target.closest('p[data-i]');
    if (p && lyrics.synced) seekTo(lyrics.lines[+p.dataset.i].t);
  });
  setupSeekBar();
  onSettingChange((key, value) => {
    if (key === 'musicSource') { send('media', 'sources', value); render(); }
    if (key === 'miniLyrics') updateMini();
  });
  render();
}

// À appeler quand la page est prête : choix de la source et état complet (pochette comprise).
function connectMusic() {
  send('media', 'sources', settings.musicSource);
  send('media', 'refresh');
}

// Pour le bloc [Contexte] envoyé à Claude : le morceau suivi, ou null.
function nowPlaying() {
  if (!M.active || !M.title) return null;
  const pos = fmtDur(currentPos());
  return {
    title: M.title, artist: M.artist, album: M.album, app: sourceName(M.app), playing: isPlaying(),
    position: M.dur ? `${pos} / ${fmtDur(M.dur)}` : pos,
  };
}

// Aperçu navigateur uniquement : injecte des paroles factices pour tester l'affichage.
function previewLyrics(lines, synced = true) {
  Object.assign(lyrics, { key: trackKey(M), dur: M.dur, state: 'ready', synced, lines, current: -1 });
  lyricsCache.set(lyrics.key, { state: 'ready', synced, lines });
  renderLyrics();
  syncLyrics(currentPos(), true);
}

export { onMedia, setupMusic, connectMusic, previewLyrics, fetchLyrics, guessTrack, nowPlaying, ensureLoop as resumeMusicLoop };
