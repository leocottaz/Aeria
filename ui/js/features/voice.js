// Commande vocale : le natif (src/features/voice.*) écoute le micro hors ligne ; ici on affiche son état
// (carte « Voix » des Réglages, champ « Demander ») et on envoie à Aeria la phrase qu'il a comprise.
import { $, $$, toast, autosize } from '../core/util.js';
import { send } from '../core/bridge.js';
import { settings, openSettingsPage } from '../core/settings.js';
import { openTab, switchTab } from '../dock/dock.js';
import { sendChat } from '../chat/chat.js';

const MODEL_MB = 190;
let state = { state: 'off' };
let placeholder = '';
let pending = false; // un bouton micro vient d'être pressé : on attend que le natif écoute (ou dise pourquoi non)
let dictate = null;  // champ du bouton micro pressé : il recevra la phrase au lieu de l'envoyer à Aeria
let armed = false;   // le natif écoute pour ce bouton (état « awake » vu)

const STATUS = {
  off: () => 'Désactivé.',
  nomodel: () => `Le modèle de reconnaissance (${MODEL_MB} Mo) n’est pas encore sur ce PC.`,
  downloading: (m) => `Téléchargement du modèle… ${m.pct ?? 0} %`,
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
  status.textContent = on || m.state === 'downloading' ? (STATUS[m.state] || STATUS.off)(m) : 'Désactivé.';
  status.classList.toggle('error', on && m.state === 'error');
  $('#voiceDownload').hidden = !(on && (m.state === 'nomodel' || (m.state === 'error' && m.msg?.includes('télécharge'))));
  $('#voiceBar').hidden = m.state !== 'downloading';
  $('#voiceFill').style.width = `${m.pct ?? 0}%`;

  // L'encoche du dock rangé (état natif « listen », css/dock.css) reprend le même texte.
  $('#listenText').textContent = m.state === 'working' ? 'Je transcris…' : 'Je t’écoute…';

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
  if (pending && m.state !== 'loading') {
    pending = false;
    if (m.state === 'error') toast(m.msg || 'Erreur du micro.');
    if (m.state === 'nomodel') { // le bouton ne peut rien sans le modèle : on montre où le télécharger
      toast('Télécharge d’abord le modèle vocal.');
      switchTab('settings');
      openSettingsPage('behavior');
    }
  }
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
  $('#voiceDownload').addEventListener('click', () => {
    state = { state: 'downloading', pct: 0 };
    render();
    send('voice', 'download');
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
