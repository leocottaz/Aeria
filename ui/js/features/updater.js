// Mises à jour : la page cherche une version plus récente sur GitHub et affiche la bannière ;
// le natif (src/features/updater.*) ne télécharge et n'installe qu'au clic, d'après un simple numéro.
import { $, store, toast } from '../core/util.js';
import { bridge, send } from '../core/bridge.js';
import { settings, setSetting } from '../core/settings.js';

const RELEASES_API = 'https://api.github.com/repos/leocottaz/ShadowAssistant/releases';
const RELEASES ='https://github.com/leocottaz/ShadowAssistant/releases';
const CHECK_EVERY = 6 * 3600 * 1000;

// version : numéro de l'exécutable ; canUpdate : copie installée par Setup.exe (sinon on renvoie vers GitHub).
const U = { version: '0.0.0', canUpdate: false, latest: null, dismissed: false, busy: false, busyText: '', error: '' };

// « v1.2.3 » → [1, 2, 3] ; null pour tout autre format (préversions comprises).
function parseVersion(text) {
  const m = /^v?(\d{1,5})\.(\d{1,5})\.(\d{1,5})$/.exec(String(text).trim());
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

function isNewer(candidate, current) {
  const a = parseVersion(candidate), b = parseVersion(current);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
  return false;
}

const isDev = () => U.version === '0.0.0';
const releaseUrl = () => (U.latest ? U.latest.url : RELEASES);

function setStatus(text) { $('#updateStatus').textContent = text; }

function render() {
  $('#updateVersion').textContent = isDev() ? 'développement' : U.version;
  const l = U.latest;
  const pending = !!l && !U.dismissed && l.tag !== settings.updateSkipped && isNewer(l.tag, U.version);
  const banner = $('#updateBanner');
  banner.hidden = !pending && !U.busy;
  if (pending || U.busy) {
    $('#updateTitle').textContent = l ? `Version ${l.version} disponible` : 'Mise à jour';
    $('#updateSub').textContent = U.error || (U.busy ? U.busyText : `Tu as la ${U.version}.`);
    $('#updateSub').title = U.error; // message complet quand la bannière le tronque
    $('#updateInstall').textContent = !U.canUpdate ? 'Télécharger' : U.error ? 'Réessayer' : 'Mettre à jour';
    $('#updateInstall').disabled = U.busy;
    $('#updateLater').hidden = U.busy;
    $('#updateSkip').hidden = U.busy;
    $('#updateBar').hidden = !U.busy;
    banner.classList.toggle('error', !!U.error);
  }
}

// --------------------------------------------------------------- vérification

async function check({ manual = false } = {}) {
  if (isDev()) {
    if (manual) setStatus('Version de développement : pas de vérification.');
    return;
  }
  if (manual) setStatus('Recherche en cours…');
  try {
    // La liste, pas `releases/latest` : GitHub y met la plus récente publiée, même un tag de test
    // (v0.3.1-dev) qu'on ignore ; on veut la plus haute version stable.
    const res = await fetch(`${RELEASES_API}?per_page=30`, { headers: { Accept: 'application/vnd.github+json' }, cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    store.set('updateLastCheck', Date.now());
    let r = null;
    for (const c of await res.json()) {
      if (c.draft || c.prerelease || !parseVersion(c.tag_name)) continue;
      if (!r || isNewer(c.tag_name, r.tag_name)) r = c;
    }
    if (!r) { if (manual) setStatus('Aucune version publiée pour le moment.'); return; }
    const version = r.tag_name.replace(/^v/, '');
    U.latest = { tag: r.tag_name, version, url: r.html_url || RELEASES };
    if (isNewer(r.tag_name, U.version)) {
      if (manual) { U.dismissed = false; setStatus(`La version ${version} est disponible.`); }
      // Une seule notification par version, même si le panneau reste fermé.
      if (store.get('updateNotified', '') !== r.tag_name && r.tag_name !== settings.updateSkipped) {
        store.set('updateNotified', r.tag_name);
        send('notify', 'Mise à jour disponible', `Shadow Assistant ${version} est prête (tu as la ${U.version}).`);
      }
    } else if (manual) {
      setStatus('Tu as la dernière version.');
    }
  } catch (e) {
    if (manual) setStatus('Impossible de joindre GitHub. Vérifie ta connexion.');
  }
  render();
}

function schedule() {
  const last = store.get('updateLastCheck', 0);
  if (settings.updateCheck && !isDev() && Date.now() - last > CHECK_EVERY) setTimeout(() => check(), 8000);
  setInterval(() => { if (settings.updateCheck) check(); }, CHECK_EVERY);
}

// --------------------------------------------------------------- installation (natif)

function install() {
  if (!U.latest) return;
  if (!U.canUpdate || !bridge) { send('openurl', releaseUrl()); return; }
  U.busy = true;
  U.error = '';
  U.busyText = 'Téléchargement…';
  $('#updateFill').style.width = '0%';
  render();
  send('update', U.latest.tag);
}

const STATE_TEXT = {
  downloading: 'Téléchargement…',
  verifying: 'Vérification du fichier…',
  installing: 'Installation…',
  restarting: 'Redémarrage de Shadow Assistant…',
};

// Messages {"t":"update"} du natif.
function onUpdateMessage(m) {
  if (m.state === 'error') {
    U.busy = false;
    U.error = m.msg || 'La mise à jour a échoué.';
    setStatus(U.error);
    render();
    return;
  }
  U.busy = true;
  U.busyText = STATE_TEXT[m.state] || '';
  if (typeof m.pct === 'number') {
    $('#updateFill').style.width = `${m.pct}%`;
    if (m.state === 'downloading') U.busyText = `Téléchargement… ${m.pct} %`;
  }
  render();
  // Le programme d'installation arrête et relance le dock ; on se ferme proprement juste avant.
  if (m.state === 'restarting') setTimeout(() => send('quit'), 700);
}

// Message « init » du natif : numéro de version de l'exécutable et type d'installation.
function onUpdateInit(m) {
  U.version = m.version || '0.0.0';
  U.canUpdate = !!m.canUpdate;
  render();
  schedule();
  announceChanges();
}

// --------------------------------------------------------------- nouveautés

// Les nouveautés sont le texte des Releases GitHub : une ligne par « - … » (ou « * … »).
// Renvoie les versions de `seen` (exclue) à la version actuelle (incluse), de la plus récente à la plus
// ancienne ; sans `seen`, seulement la version actuelle. null si GitHub est injoignable (on réessaiera
// au prochain lancement), [] s'il n'y a rien à montrer.
async function fetchNotes(seen) {
  try {
    const res = await fetch(`${RELEASES_API}?per_page=30`, { headers: { Accept: 'application/vnd.github+json' }, cache: 'no-store' });
    if (!res.ok) return null;
    const groups = [];
    for (const r of await res.json()) {
      const tag = String(r.tag_name);
      if (r.draft || r.prerelease || !parseVersion(tag) || isNewer(tag, U.version)) continue;
      if (seen ? !isNewer(tag, seen) : tag !== `v${U.version}`) continue;
      const items = String(r.body || '').split('\n').map((l) => l.trim()).filter((l) => /^[-*]\s+\S/.test(l)).map((l) => l.replace(/^[-*]\s+/, ''));
      if (items.length) groups.push({ version: tag.replace(/^v/, ''), tag, items });
    }
    return groups.sort((a, b) => (isNewer(a.tag, b.tag) ? -1 : 1));
  } catch (e) {
    return null;
  }
}

// groups : [{ version, items }] ; plusieurs versions → un intertitre par version.
function showChangelog(groups) {
  const many = groups.length > 1;
  $('#whatsNewTitle').textContent = many ? `Nouveautés depuis ta version précédente` : `Nouveautés de la version ${groups[0].version}`;
  const li = (text, cls) => Object.assign(document.createElement('li'), { textContent: text, className: cls || '' });
  $('#whatsNewList').replaceChildren(...groups.flatMap((g) => [...(many ? [li(`Version ${g.version}`, 'ver')] : []), ...g.items.map((t) => li(t))]));
  $('#whatsNew').hidden = false;
  send('open'); // le dock est rangé au démarrage : on le déroule pour montrer la carte
}

// Une seule fois par version : rien au premier lancement ni en version de développement.
async function announceChanges() {
  if (isDev()) return;
  const seen = store.get('lastSeenVersion', '');
  if (seen === U.version) return;
  // Pas de version enregistrée : installation neuve (welcomed absent, rien à montrer) ou mise à jour
  // depuis une version qui ne l'enregistrait pas encore (welcomed présent) : on montre les nouveautés.
  const upgraded = seen ? isNewer(U.version, seen) : store.get('welcomed', false);
  if (!upgraded) { store.set('lastSeenVersion', U.version); return; }
  const groups = await fetchNotes(seen);
  if (!groups) return; // hors ligne : on réessaie au prochain lancement
  store.set('lastSeenVersion', U.version);
  if (groups.length) showChangelog(groups);
}

function setupUpdater() {
  $('#updateInstall').addEventListener('click', install);
  $('#updateLater').addEventListener('click', () => { U.dismissed = true; render(); });
  $('#updateSkip').addEventListener('click', () => {
    if (U.latest) setSetting('updateSkipped', U.latest.tag);
    render();
    toast('Version ignorée');
  });
  $('#whatsNewClose').addEventListener('click', () => { $('#whatsNew').hidden = true; });
  $('#updateCheckNow').addEventListener('click', () => check({ manual: true }));
  $('#updateOpenReleases').addEventListener('click', () => send('openurl', releaseUrl()));
  render();
}

export { setupUpdater, onUpdateInit, showChangelog, onUpdateMessage, parseVersion, isNewer };
