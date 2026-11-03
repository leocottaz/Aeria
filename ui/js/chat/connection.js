// Connexion : catégorie Claude (abonnement Pro ou clé API), ChatGPT (clé OpenAI) ou Gratuit (Gemini, Groq).
import { $, esc, icon, toast } from '../core/util.js';
import { bridge, send, http, apiErrorText } from '../core/bridge.js';
import { settings, setSetting, onSettingChange } from '../core/settings.js';
import { S } from '../core/state.js';
import { SERVICES, freeReady, freeService, listModels, knownModels, autoModel } from './free.js';

const INSTALL_CMD = 'irm https://claude.ai/install.ps1 | iex';

function checkClaudeCode() {
  if (!bridge) return;
  S.cc = { ...S.cc, checking: true };
  renderCc();
  send('ccstatus');
}

function renderCc() {
  const el = $('#ccStatus');
  const cc = S.cc;
  if (cc.checking && !cc.checked) {
    el.innerHTML = '<span>Recherche de Claude Code…</span>';
  } else if (cc.found) {
    el.innerHTML = `<span class="ok">✓ Installé</span> <span>${esc(cc.version || '')}</span>
      <button type="button" data-act="login">${icon('key')}Se connecter</button>
      <button type="button" data-act="check">Vérifier</button>`;
  } else {
    el.innerHTML = `<span class="bad">Claude Code n'est pas installé.</span>
      <span>Dans PowerShell :</span> <code>${esc(INSTALL_CMD)}</code>
      <button type="button" data-act="copy">${icon('copy')}Copier</button>
      <button type="button" data-act="check">Vérifier</button>
      <button type="button" data-act="docs">Aide</button>`;
  }
  updateNoKey();
}

function updateNoKey() {
  let title = '', text = '';
  if (settings.backend === 'free' || settings.backend === 'chatgpt') {
    if (!freeReady()) {
      title = `Ajoute une clé ${SERVICES[freeService()].name}`;
      text = settings.backend === 'chatgpt'
        ? "Elle est chiffrée par Windows et ne quitte jamais ton PC, sauf vers l'API d'OpenAI. L'usage est facturé à part de l'abonnement ChatGPT."
        : 'Elle se crée gratuitement en quelques secondes (lien dans Réglages) et reste chiffrée par Windows.';
    }
  } else if (settings.backend === 'pro') {
    if (S.cc.checked && !S.cc.found) {
      title = 'Installe Claude Code pour utiliser ton abonnement';
      text = 'Le mode Abonnement Pro passe par Claude Code, inclus dans Pro. La commande d’installation est dans Réglages.';
    }
  } else if (!S.hasKey) {
    title = 'Ajoute ta clé API Anthropic';
    text = "Elle est chiffrée par Windows et ne quitte jamais ton PC, sauf vers l'API de Claude. L'usage est facturé à part.";
  }
  $('#noKeyTitle').textContent = title;
  $('#noKeyText').textContent = text;
  $('#noKey').hidden = !title;
}

function saveKey() {
  const v = $('#keyInput').value.trim();
  if (!v) return;
  if (!bridge) { toast('Disponible dans l’application'); return; }
  send('key', v);
  $('#keyInput').value = '';
}

function testKey() {
  S.keyTest = { state: 'pending' };
  renderKey();
  http.request('GET', '/v1/models?limit=1', '', null, {
    onDone: () => { S.keyTest = { state: 'ok' }; renderKey(); },
    onError: (status, body) => { S.keyTest = { state: 'bad', text: apiErrorText(status, body) }; renderKey(); },
  });
}

function renderKey() {
  const el = $('#keyStatus');
  let html;
  if (S.hasKey) {
    html = `${icon('key')} Clé enregistrée <b>••••${esc(S.keyHint)}</b>, chiffrée par Windows.`;
    if (S.keyTest?.state === 'pending') html += ' <span>Test…</span>';
    else if (S.keyTest?.state === 'ok') html += ' <span class="ok">✓ Connexion réussie</span>';
    else if (S.keyTest?.state === 'bad') html += ` <span class="bad">${esc(S.keyTest.text)}</span>`;
    html += ' <button type="button" data-act="test">Tester</button><button type="button" data-act="del">Supprimer</button>';
  } else {
    html = 'Aucune clé. <a data-act="console">Créer une clé sur console.anthropic.com</a>';
  }
  el.innerHTML = html;
  updateNoKey();
}

// ------------------------------------------------------------------ modèle gratuit

let freeTest = null; // { state: 'pending' | 'ok' | 'bad', text }

function renderFree() {
  const svc = freeService();
  const s = SERVICES[svc];
  const k = S.freeKeys?.[svc];
  let html;
  if (k?.hasKey) {
    html = `${icon('key')} Clé ${s.name} <b>••••${esc(k.keyHint)}</b>, chiffrée par Windows.`;
    if (freeTest?.state === 'pending') html += ' <span>Test…</span>';
    else if (freeTest?.state === 'ok') html += ' <span class="ok">✓ Connexion réussie</span>';
    else if (freeTest?.state === 'bad') html += ` <span class="bad">${esc(freeTest.text)}</span>`;
    html += ' <button type="button" data-act="test">Tester</button><button type="button" data-act="del">Supprimer</button>';
  } else {
    html = `Aucune clé. <a data-act="get">Créer une clé gratuite ${s.name}</a>`;
  }
  $('#freeKeyStatus').innerHTML = html;
  $('#freeKeyInput').placeholder = `Colle ta clé ${s.name}…`;

  // Liste des modèles : celle du service (récupérée avec la clé), « Automatique » en tête.
  const ids = k?.hasKey ? knownModels(svc) : [];
  const sel = $('#freeModel');
  const cur = settings.freeModel;
  const options = [`<option value="">Automatique (${esc(autoModel(svc))})</option>`]
    .concat([...new Set([...ids, ...(cur ? [cur] : [])])].sort().map((id) => `<option value="${esc(id)}"${id === cur ? ' selected' : ''}>${esc(id)}</option>`));
  sel.innerHTML = options.join('');
  sel.value = cur;
  $('#freeModelField').hidden = !k?.hasKey;
  updateNoKey();
}

function testFree(quiet = false) {
  if (!freeReady()) return;
  if (!quiet) { freeTest = { state: 'pending' }; renderFree(); }
  listModels().then(
    () => { if (!quiet) freeTest = { state: 'ok' }; renderFree(); },
    (err) => { if (!quiet) freeTest = { state: 'bad', text: err.message }; renderFree(); },
  );
}

function saveFreeKey() {
  const v = $('#freeKeyInput').value.trim();
  if (!v) return;
  if (!bridge) { toast('Disponible dans l’application'); return; }
  send('freekey', freeService(), v);
  $('#freeKeyInput').value = '';
}

// Réponse du natif à freekey / delfreekey.
function onFreeKey(m) {
  S.freeKeys = m.freeKeys;
  freeTest = null;
  if (!m.ok) toast("Impossible d'enregistrer la clé");
  else if (S.freeKeys?.[m.service]?.hasKey) { toast('Clé enregistrée 🔒'); testFree(); }
  else toast('Clé supprimée');
  renderFree();
}

function setupConnection() {
  $('#freeKeySave').addEventListener('click', saveFreeKey);
  $('#freeKeyInput').addEventListener('keydown', (e) => { if (e.key === 'Enter') saveFreeKey(); });
  $('#freeKeyStatus').addEventListener('click', (e) => {
    const act = e.target.closest('[data-act]')?.dataset.act;
    if (act === 'test') testFree();
    if (act === 'del') send('delfreekey', freeService());
    if (act === 'get') send('openurl', SERVICES[freeService()].keyUrl);
  });
  $('#freeModel').addEventListener('change', (e) => setSetting('freeModel', e.target.value));
  $('#keySave').addEventListener('click', saveKey);
  $('#keyInput').addEventListener('keydown', (e) => { if (e.key === 'Enter') saveKey(); });
  $('#keyStatus').addEventListener('click', (e) => {
    const act = e.target.closest('[data-act]')?.dataset.act;
    if (act === 'test') testKey();
    if (act === 'del') send('delkey');
    if (act === 'console') send('openurl', 'https://console.anthropic.com/settings/keys');
  });
  $('#ccStatus').addEventListener('click', (e) => {
    const act = e.target.closest('[data-act]')?.dataset.act;
    if (act === 'check') checkClaudeCode();
    if (act === 'login') { send('cclogin'); toast('Connecte-toi dans la console qui s’ouvre'); }
    if (act === 'copy') navigator.clipboard?.writeText(INSTALL_CMD).then(() => toast('Commande copiée'), () => toast('Copie impossible'));
    if (act === 'docs') send('openurl', 'https://code.claude.com/docs/en/setup');
  });
  let lastService = freeService();
  onSettingChange((key, value) => {
    // Catégorie (Claude, ChatGPT, Gratuit) → mode de connexion ; Claude reprend son dernier mode.
    if (key === 'category') setSetting('backend', value === 'claude' ? settings.claudeMode : value);
    if (key === 'backend' && (value === 'pro' || value === 'api')) setSetting('claudeMode', value);
    if (key === 'backend') { renderKey(); renderCc(); renderFree(); }
    if (key === 'backend' || key === 'freeService') {
      if (freeService() === lastService) return;
      lastService = freeService();
      freeTest = null;
      if (settings.freeModel) setSetting('freeModel', ''); // un modèle Gemini n'existe pas chez Groq ni chez OpenAI
      renderFree();
      testFree(true); // récupère la liste des modèles du service
    }
  });
}

export { checkClaudeCode, renderCc, renderKey, renderFree, testFree, onFreeKey, updateNoKey, setupConnection };
