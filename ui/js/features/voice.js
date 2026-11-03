// Commande vocale : le natif (src/features/voice.*) écoute le micro hors ligne ; ici on affiche son état
// (carte « Voix » des Réglages, champ « Demander ») et on envoie à Aeria la phrase qu'il a comprise.
import { $, toast } from '../core/util.js';
import { send } from '../core/bridge.js';
import { settings } from '../core/settings.js';
import { openTab } from '../dock/dock.js';
import { sendChat } from '../chat/chat.js';

const MODEL_MB = 190;
let state = { state: 'off' };
let placeholder = '';

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

  // Le champ « Demander » montre que l'assistante écoute (sans animation continue : voir Pièges).
  const ask = $('#askForm'), input = $('#askInput');
  const listening = settings.voiceWake && (m.state === 'awake' || m.state === 'working');
  if (listening && !ask.classList.contains('listening')) placeholder = input.placeholder;
  ask.classList.toggle('listening', listening);
  if (listening) input.placeholder = m.state === 'working' ? 'Je transcris…' : 'Je t’écoute…';
  else if (placeholder) { input.placeholder = placeholder; placeholder = ''; }
}

function onVoice(m) {
  if (m.state === 'heard') {
    if (!m.text) return;
    $('#askInput').value = '';
    openTab('claude');
    sendChat(m.text);
    return;
  }
  state = m;
  render();
}

function setupVoice() {
  $('#voiceDownload').addEventListener('click', () => {
    state = { state: 'downloading', pct: 0 };
    render();
    send('voice', 'download');
  });
  render();
}

// Le réglage a changé (interrupteur) ou la page vient de démarrer : on redemande l'état au natif.
function syncVoice() {
  if (!settings.voiceWake) state = { state: 'off' };
  render();
  send('voice', 'status');
}

export { onVoice, setupVoice, syncVoice };
