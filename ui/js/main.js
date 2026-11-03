// Point d'entrée : branche les modules et route les messages du natif.
import { $, store, toast, hydrateIcons } from './core/util.js';
import { bridge, send, http, onReply } from './core/bridge.js';
import { settings, sendConfig, applyAppearance, renderSettings, setupSettings, openSettingsPage, onSettingChange } from './core/settings.js';
import { S } from './core/state.js';
import { checkClaudeCode, renderCc, renderKey, renderFree, testFree, onFreeKey, setupConnection } from './chat/connection.js';
import { knownModels } from './chat/free.js';
import { setState, switchTab, tick, setupBar, setupGlobal, onWheel } from './dock/dock.js';
import { fireReminder, renderReminders, saveReminders, setupReminders } from './features/reminders.js';
import { renderNotes, setupNotes } from './features/notes.js';
import { renderTodos, setupTodo } from './features/todo.js';
import { setupToday, renderToday } from './features/today.js';
import { setupRoutines } from './features/routines.js';
import { setupTimer, renderTimer, finishTimer, syncTimer } from './features/timer.js';
import { renderChat, focusComposer, setupChat } from './chat/chat.js';
import { onMedia, setupMusic, connectMusic, previewLyrics, fetchLyrics, guessTrack } from './features/music.js';
import { setupUpdater, onUpdateInit, onUpdateMessage, showChangelog } from './features/updater.js';
import { onVoice, setupVoice, syncVoice } from './features/voice.js';

function onNative(m) {
  switch (m.t) {
    case 'init':
      S.hasKey = m.hasKey;
      S.keyHint = m.keyHint;
      // Réglages gardés côté natif : c'est lui qui fait foi. Sous Linux, lowMemory ne vit que dans la page.
      if (settings.autostart !== m.autostart || (m.lowMemory !== undefined && settings.lowMemory !== m.lowMemory)) {
        settings.autostart = m.autostart;
        settings.lowMemory = m.lowMemory ?? settings.lowMemory;
        store.set('settings', settings);
      }
      S.freeKeys = m.freeKeys || {};
      renderKey();
      renderFree();
      if (!knownModels().length) testFree(true); // liste des modèles gratuits, si une clé est là
      renderSettings();
      checkClaudeCode();
      onUpdateInit(m);
      if (!store.get('welcomed', false)) {
        store.set('welcomed', true);
        setTimeout(() => { switchTab('settings'); openSettingsPage('aeria'); send('open'); setTimeout(() => $('#keyInput').focus(), 400); }, 1400);
      }
      break;
    case 'state': setState(m.s, m); break;
    case 'wheel': onWheel(m); break;
    case 'key':
      S.hasKey = m.hasKey;
      S.keyHint = m.keyHint;
      S.keyTest = null;
      renderKey();
      if (m.ok) toast(m.hasKey ? 'Clé enregistrée 🔒' : 'Clé supprimée');
      else toast("Impossible d'enregistrer la clé");
      break;
    case 'freekey': onFreeKey(m); break;
    case 'chunk': http.onChunk(m.id, m.data); break;
    case 'done': http.onDone(m.id, !!m.cancelled, m); break;
    case 'ccstatus':
      S.cc = { checked: true, found: m.found, version: m.version, path: m.path };
      renderCc();
      break;
    case 'error': http.onError(m.id, m.status, m.body); break;
    case 'fire': fireReminder(m.id); break;
    case 'timerdone': finishTimer(); break;
    case 'media': onMedia(m); break;
    case 'update': onUpdateMessage(m); break;
    case 'voice': onVoice(m); break;
    case 'reply': onReply(m); break;
    case 'hotkey': switchTab('claude'); focusComposer(); break;
    case 'show': switchTab(m.tab); if (m.tab === 'claude') focusComposer(); break;
    default: break;
  }
}

function init() {
  hydrateIcons();
  applyAppearance();
  setupSettings();
  setupConnection();
  setupBar();
  setupReminders();
  setupNotes();
  setupTodo();
  setupToday();
  setupRoutines();
  setupTimer();
  setupChat();
  setupMusic();
  setupGlobal();
  setupUpdater();
  setupVoice();
  renderSettings();
  renderKey();
  renderCc();
  renderFree();
  renderReminders();
  renderNotes();
  renderTodos();
  renderTimer();
  renderChat();
  switchTab(S.tab);
  tick();
  setInterval(() => {
    tick();
    if (S.st === 'panel' && S.tab === 'reminders') renderReminders(false);
    if (S.st === 'panel' && S.tab === 'today') renderToday();
  }, 1000);

  if (bridge) {
    bridge.addEventListener('message', (e) => onNative(e.data));
    send('ready');
    sendConfig();
    syncVoice();
    // Le natif recrée la vue avec les nouvelles options : on laisse l'interrupteur finir.
    onSettingChange((key, value) => {
      if (key === 'voiceWake') setTimeout(syncVoice, 50); // le natif a reçu la config : il annonce son état
      const linux = document.documentElement.classList.contains('linux'); // là-bas, rien à recharger
      if (key === 'lowMemory' && !linux) { toast('Rechargement du dock…'); setTimeout(() => send('lowmem', value ? '1' : '0'), 450); }
    });
    saveReminders();
    syncTimer();
    connectMusic();
  } else {
    // Aperçu dans un navigateur : on affiche directement le panneau. window.__preview permet
    // de simuler des messages du natif depuis la console (ex. un morceau en cours).
    document.body.classList.add('preview');
    window.__preview = { onNative, previewLyrics, fetchLyrics, guessTrack, updater: { onUpdateInit, onUpdateMessage, showChangelog } };
    setTimeout(() => setState('panel', {}), 60);
  }
}

init();
