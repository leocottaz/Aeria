// Notifications des autres applications (Discord, mails…) : le natif (src/features/appnotifs.cpp) les lit
// et les envoie une à une ({"t":"appnotif"}) ; on les montre dans la pastille du mini-dock, comme les
// paroles (dock.js ouvert : un toast). Réglage désactivé par défaut ; « contenu » coupé = seulement le nom
// de l'appli (utile pour partager son écran).
import { $, toast } from '../core/util.js';
import { send, bridge } from '../core/bridge.js';
import { settings, setSetting, onSettingChange } from '../core/settings.js';
import { S } from '../core/state.js';
import { showMiniNotice } from '../dock/mini.js';

const STATUS_TEXT = {
  allowed: 'Actif : les notifications des autres applis s’affichent dans la pastille.',
  denied: 'Windows refuse l’accès. Autorise-le dans Paramètres › Confidentialité et sécurité › Notifications.',
  unsupported: 'Pas encore pris en charge sur ce système.',
  error: 'Impossible de lire les notifications.',
};

function setStatus(text) {
  const el = $('#appNotifsStatus');
  if (el) el.textContent = text;
}

// Le natif annonce le résultat à chaque démarrage de l'écoute.
function onAppNotifsStatus(m) {
  setStatus(STATUS_TEXT[m.status] || '');
  if (m.status !== 'allowed' && settings.appNotifs) {
    toast(STATUS_TEXT[m.status] || 'Notifications des applis indisponibles.');
    setSetting('appNotifs', false);
  }
}

function onAppNotif(m) {
  const app = (m.app || '').trim() || 'Application';
  // Contenu masqué : seulement le nom de l'appli.
  const card = settings.appNotifsText
    ? { app, title: m.title || '', text: m.text || '', icon: m.icon || '' }
    : { app, title: app, text: 'Nouvelle notification', icon: m.icon || '' };
  const line = [card.title, card.text].filter(Boolean).join(' — ');
  if (!line) return;
  if (S.st === 'bar' || S.st === 'panel') toast(`${app} · ${line}`); // dock ouvert : pas de pastille
  else showMiniNotice(line, { card }); // le son est celui de l'appli elle-même
}

function syncAppNotifs() {
  if (!bridge) return;
  send('appnotifs', settings.appNotifs ? '1' : '0');
  if (!settings.appNotifs) setStatus('');
}

function setupAppNotifs() {
  onSettingChange((key) => { if (key === 'appNotifs') syncAppNotifs(); });
}

export { onAppNotif, onAppNotifsStatus, setupAppNotifs, syncAppNotifs };
