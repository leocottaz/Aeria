// Commande vocale : le natif (src/features/voice.*) écoute le micro hors ligne ; ici on affiche son état
// (page « Voix » des Réglages, champ « Demander ») et on envoie à Aeria la phrase qu'il a comprise.
import { $, $$, toast, autosize } from '../core/util.js';
import { send } from '../core/bridge.js';
import { settings, openSettingsPage, onSettingChange } from '../core/settings.js';
import { openTab, switchTab } from '../dock/dock.js';
import { sendChat } from '../chat/chat.js';
import { setupPill, pillActive, pillListening, pillWorking, pillPartial, pillSend } from './pill.js';

// Modèles proposés (ids et tailles = kModels dans src/features/voice.cpp). Aucun n'est livré : on choisit, puis on télécharge.
const MODELS = {
  base: { label: 'Base', mb: 60, info: 'Très rapide (environ 0,5 s par phrase), mais il se trompe souvent, même sur « Aeria ».' },
  small: { label: 'Small', mb: 190, info: 'Le bon compromis : environ 2 s par phrase, assez précis.' },
  medium: { label: 'Medium', mb: 540, info: 'Le plus lourd : environ 6 s par phrase sur un PC courant (le double pour « Aeria, … » dit d’une traite).' },
};
let state = { state: 'off' };
let models = []; // modèles déjà téléchargés (annoncés par le natif)
let placeholder = '';
let pending = false; // un bouton micro vient d'être pressé : on attend que le natif écoute (ou dise pourquoi non)
let dictate = null;  // champ du bouton micro pressé : il recevra la phrase au lieu de l'envoyer à Aeria
let armed = false;   // le natif écoute pour ce bouton (état « awake » vu)
let confirmTimer = 0; // « Supprimer » demande une seconde pression

const STATUS = {
  off: () => 'Désactivé.',
  downloading: (m) => `Téléchargement de ${MODELS[m.model]?.label ?? 'du modèle'}… ${m.pct ?? 0} %`,
  loading: () => 'Chargement du modèle…',
  idle: () => 'À l’écoute : dis « Aeria ».',
  awake: () => 'Je t’écoute…',
  working: () => 'Je transcris…',
  error: (m) => m.msg || 'Erreur du micro.',
};

function render() {
  const status = $('#voiceStatus');
  if (!status) return;
  const m = state;
  const on = settings.voiceWake;
  const model = MODELS[settings.voiceModel] ? settings.voiceModel : 'small';
  const info = MODELS[model];
  const have = models.includes(model);
  const downloading = m.state === 'downloading';
  $('#voiceModelInfo').textContent = `${info.label} · ${info.mb} Mo — ${info.info}`;
  if (downloading) status.textContent = STATUS.downloading(m);
  else if (!have) status.textContent = `Modèle non téléchargé (${info.mb} Mo).`;
  else if (on) status.textContent = (STATUS[m.state] || STATUS.off)(m);
  else status.textContent = 'Téléchargé. Active l’interrupteur pour l’utiliser.';
  status.classList.toggle('error', on && have && m.state === 'error');
  $('#voiceDownload').hidden = have || downloading;
  $('#voiceDownload').textContent = `Télécharger (${info.mb} Mo)`;
  $('#voiceDelete').hidden = !have || downloading;
  $('#voiceBar').hidden = !downloading;
  $('#voiceFill').style.width = `${m.pct ?? 0}%`;

  // Le champ « Demander » montre que l'assistante écoute (sans animation continue : voir Pièges).
  const ask = $('#askForm'), input = $('#askInput');
  const listening = m.state === 'awake' || m.state === 'working'; // aussi avec le bouton micro, écoute permanente éteinte
  for (const b of $$('[data-mic]')) {
    b.classList.toggle('on', listening);
    b.classList.toggle('busy', pending && m.state === 'loading');
    b.title = listening ? 'Arrêter l’écoute' : 'Parler à Aeria';
  }
  const chat = $('#chatInput'); // le champ de la conversation prend le même texte quand il reçoit la dictée
  if (listening && dictate === chat) {
    chat.dataset.ph ??= chat.placeholder;
    chat.placeholder = m.state === 'working' ? 'Je transcris…' : 'Je t’écoute…';
  } else if (chat.dataset.ph !== undefined) {
    chat.placeholder = chat.dataset.ph;
    delete chat.dataset.ph;
  }
  if (listening && !ask.classList.contains('listening')) placeholder = input.placeholder;
  ask.classList.toggle('listening', listening);
  if (listening) input.placeholder = m.state === 'working' ? 'Je transcris…' : 'Je t’écoute…';
  else if (placeholder) { input.placeholder = placeholder; placeholder = ''; }
}

function onVoice(m) {
  if (m.models) models = m.models;
  if (pending && m.state !== 'loading') {
    pending = false;
    if (m.state === 'error') toast(m.msg || 'Erreur du micro.');
    if (m.state === 'nomodel') { // le bouton ne peut rien sans le modèle : on montre où le télécharger
      toast('Choisis et télécharge d’abord un modèle vocal.');
      switchTab('settings');
      openSettingsPage('voice');
    }
  }
  // Dock rangé : la pilule (pill.js) montre ce qu'on dit en direct, puis la réponse. Barre ou panneau ouverts, c'est
  // leur champ « Demander » qui s'éclaire et la conversation qui répond.
  const inPill = !dictate && pillActive();
  if (m.state === 'partial') { // texte en direct pendant l'écoute : il ne change pas l'état
    if (inPill) pillPartial(m.text);
    return;
  }
  if (inPill && m.state === 'awake') pillListening();
  if (inPill && m.state === 'working') pillWorking();
  if (dictate && m.state === 'awake') armed = true;
  if (dictate && armed && ['idle', 'off', 'error'].includes(m.state)) dictate = null; // l'écoute s'est arrêtée sans phrase
  if (m.state === 'heard') {
    if (!m.text) return;
    if (dictate) { // bouton micro d'un champ : la phrase s'écrit dans ce champ, à relire avant d'envoyer
      write(dictate, m.text);
      dictate = null;
      state = { state: 'idle' };
      render();
      return;
    }
    if (inPill) {
      pillSend(m.text);
      state = { state: 'idle' };
      render();
      return;
    }
    $('#askInput').value = '';
    openTab('claude');
    sendChat(m.text);
    return;
  }
  state = m;
  render();
}

// Ajoute le texte dicté au champ (à la suite de ce qui y est déjà) et lui rend le focus.
function write(el, text) {
  el.value = el.value.trim() ? `${el.value.trimEnd()} ${text}` : text;
  el.dispatchEvent(new Event('input', { bubbles: true }));
  if (el.tagName === 'TEXTAREA') autosize(el);
  el.focus();
  el.setSelectionRange(el.value.length, el.value.length);
}

function setupVoice() {
  setupPill();
  onSettingChange((key) => { if (key === 'voiceModel' || key === 'voiceWake') render(); });
  $('#voiceDownload').addEventListener('click', () => {
    const model = settings.voiceModel;
    state = { state: 'downloading', pct: 0, model };
    render();
    send('voice', `download:${model}`);
  });
  // Supprimer libère la place du modèle (on peut le retélécharger) : une seconde pression confirme.
  const del = $('#voiceDelete');
  del.addEventListener('click', () => {
    if (!confirmTimer) {
      del.textContent = 'Confirmer ?';
      confirmTimer = setTimeout(() => { confirmTimer = 0; del.textContent = 'Supprimer'; }, 3000);
      return;
    }
    clearTimeout(confirmTimer);
    confirmTimer = 0;
    del.textContent = 'Supprimer';
    send('voice', `delete:${settings.voiceModel}`);
  });
  for (const b of $$('[data-mic]')) {
    b.addEventListener('click', () => {
      if (state.state === 'awake' || state.state === 'working') { // déjà à l'écoute : le bouton l'arrête
        dictate = null;
        send('voice', 'cancel');
        return;
      }
      if (state.state === 'downloading') {
        toast('Le modèle vocal est en cours de téléchargement.');
        return;
      }
      pending = true;
      armed = false;
      dictate = b.closest('form').querySelector('input, textarea');
      send('voice', 'listen');
      if (state.state !== 'idle') state = { state: 'loading' }; // micro fermé : il faut le temps de charger le modèle
      render();
    });
  }
  render();
}

// Le réglage a changé (interrupteur) ou la page vient de démarrer : on redemande l'état au natif.
function syncVoice() {
  if (!settings.voiceWake) state = { state: 'off' };
  render();
  send('voice', 'status');
}

export { onVoice, setupVoice, syncVoice };
