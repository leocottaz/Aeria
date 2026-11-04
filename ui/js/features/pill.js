// Pilule « Aeria » (état « listen » du dock rangé) : « Aeria » a été entendu, la pilule s'ouvre au sommet de l'écran,
// montre ce qu'on dit au fur et à mesure, envoie la phrase à l'assistante puis affiche sa réponse en direct.
// Le natif ne fait qu'ouvrir la fenêtre et la redimensionner (commande « pill », voir dock.cpp) : le contenu,
// sa taille et sa fermeture sont décidés ici. Cliquer dessus ouvre la conversation.
import { $ } from '../core/util.js';
import { send } from '../core/bridge.js';
import { S } from '../core/state.js';
import { md } from '../chat/markdown.js';
import { sendChat, onChatProgress, isBusy } from '../chat/chat.js';
import { openTab } from '../dock/dock.js';

const WIDTH = { compact: 190, text: 400 }; // DIP ; le natif borne (voir SetPill)
const MAX_H = 300; // = kPillMaxH dans src/app/dock.cpp (le CSS borne déjà la réponse à 170 px)
const HEAD = { listening: 'Je t’écoute…', working: 'Je transcris…', thinking: 'Aeria réfléchit…', answering: 'Aeria', done: 'Aeria' };

const p = { phase: 'listening', said: '', answer: '', turn: false, hover: false, timer: 0, sent: '' };

const active = () => S.st === 'listen';

// Taille voulue = celle du contenu (mesurée), envoyée au natif seulement quand elle change.
function measure() {
  if (p.frame) return;
  p.frame = requestAnimationFrame(() => {
    p.frame = 0;
    const w = p.said || p.answer ? WIDTH.text : WIDTH.compact;
    const h = Math.min(MAX_H, Math.ceil($('#listenIn').getBoundingClientRect().height));
    const root = document.documentElement.style;
    root.setProperty('--listen-w', `${w}px`);
    root.setProperty('--listen-h', `${h}px`);
    const keep = p.turn || p.phase === 'done' ? 1 : 0; // pendant la réponse, la pilule reste même si la voix s'arrête
    const key = `${w},${h},${keep}`;
    if (key !== p.sent && active()) {
      p.sent = key;
      send('pill', key);
    }
  });
}

function render() {
  const box = $('#listen');
  box.classList.toggle('has-text', !!(p.said || p.answer));
  box.classList.toggle('is-answer', p.phase === 'thinking' || p.phase === 'answering' || p.phase === 'done');
  $('#listenText').textContent = HEAD[p.phase];
  const said = $('#listenSaid'), answer = $('#listenAnswer');
  said.hidden = !p.said;
  said.textContent = p.said;
  answer.hidden = !p.answer;
  if (p.answer) {
    const stick = answer.scrollHeight - answer.scrollTop - answer.clientHeight < 40;
    answer.innerHTML = md(p.answer);
    if (stick) answer.scrollTop = answer.scrollHeight;
  }
  measure();
}

function cancelClose() {
  clearTimeout(p.timer);
  p.timer = 0;
}

// La réponse est lue : la pilule se range après un temps proportionnel à sa longueur (pas tant que la souris est dessus).
function scheduleClose(ms) {
  cancelClose();
  if (p.hover) return;
  p.timer = setTimeout(close, ms);
}

function close() {
  cancelClose();
  p.turn = false;
  p.sent = '';
  if (active()) send('pill', '0,0,0');
}

// Voix : « Aeria » entendu. Une réponse précédente encore affichée est effacée ; un tour en cours n'est pas touché.
function pillListening() {
  if (p.turn) return;
  if (p.phase === 'done' || p.phase === 'answering') Object.assign(p, { said: '', answer: '' });
  cancelClose();
  p.phase = 'listening';
  render();
}

function pillWorking() {
  if (p.turn) return;
  p.phase = 'working';
  render();
}

function pillPartial(text) {
  if (p.turn) return;
  p.said = text;
  render();
}

// La phrase est comprise : elle part à l'assistante, la réponse arrive dans la pilule.
function pillSend(text) {
  cancelClose();
  Object.assign(p, { said: text, answer: '', phase: 'thinking', turn: true });
  render();
  if (isBusy()) return fail('Aeria est encore en train de répondre.');
  sendChat(text);
  if (!isBusy()) fail('Je n’ai pas pu envoyer : ouvre le dock pour voir pourquoi.'); // pas de clé, Claude Code absent…
}

function fail(message) {
  Object.assign(p, { answer: message, phase: 'done', turn: false });
  render();
  scheduleClose(6000);
}

onChatProgress(({ text, done, error }) => {
  if (!p.turn) return;
  if (error) return fail(error);
  p.answer = text;
  if (text && p.phase !== 'answering') p.phase = 'answering';
  if (done) {
    p.phase = 'done';
    p.turn = false;
    render();
    scheduleClose(Math.min(25000, Math.max(6000, 4000 + text.length * 45)));
    return;
  }
  render();
});

function setupPill() {
  const box = $('#listen');
  box.addEventListener('pointerenter', () => { p.hover = true; cancelClose(); });
  box.addEventListener('pointerleave', () => {
    p.hover = false;
    if (p.phase === 'done') scheduleClose(3000);
  });
  // Cliquer sur la pilule ouvre la conversation ; la réponse (défilement, sélection) et la croix sont à part.
  box.addEventListener('click', (e) => {
    if (e.target.closest('#listenAnswer') || e.target.closest('#listenClose') || getSelection()?.toString()) return;
    openTab('claude');
  });
  $('#listenClose').addEventListener('click', close);
  // Le dock quitte l'état « listen » (panneau ouvert, souris en haut, Échap…) : la pilule s'efface, la réponse
  // continue dans la conversation.
  document.body.addEventListener('dockstate', (e) => {
    if (e.detail.prev === 'listen' && e.detail.st !== 'listen') {
      cancelClose();
      Object.assign(p, { turn: false, hover: false, sent: '' });
      if (p.phase === 'done' || p.phase === 'answering' || p.phase === 'thinking') Object.assign(p, { said: '', answer: '', phase: 'listening' });
      send('pill', '0,0,0');
      render();
    }
  });
}

export { setupPill, pillListening, pillWorking, pillPartial, pillSend, active as pillActive };
