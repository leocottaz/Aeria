// To-do : liste de tâches à cocher. Les importantes (étoile) passent en tête ;
// les tâches faites descendent dans « Faites » jusqu'à ce qu'on les efface.
import { $, uid, esc, icon, store, animateOut } from '../core/util.js';

let todos = store.get('todos', []);
let fresh = ''; // tâche qui vient d'apparaître ou de changer de section : seule elle s'anime

const save = () => store.set('todos', todos);
const openTodos = () => todos.filter((t) => !t.done).sort((a, b) => (b.star - a.star) || (b.created - a.created));
const doneTodos = () => todos.filter((t) => t.done).sort((a, b) => b.doneAt - a.doneAt);

function addTodo(text, star = false) {
  text = text.trim().replace(/\s+/g, ' ');
  if (!text) return null;
  const t = { id: uid(), text, done: false, star: !!star, created: Date.now(), doneAt: 0 };
  todos.push(t);
  fresh = t.id;
  save();
  renderTodos();
  return t;
}

// Pour Claude : coche la tâche à faire qui correspond au texte, sans tenir compte de la casse
// ni des accents : texte identique, sinon la seule tâche qui le contient (ou qu'il contient).
// Retourne la tâche, ou null si aucune ou plusieurs correspondent.
function completeTodo(text) {
  const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
  const q = norm(text);
  if (!q) return null;
  const open = openTodos();
  const near = open.filter((x) => norm(x.text).includes(q) || q.includes(norm(x.text)));
  const t = open.find((x) => norm(x.text) === q) || (near.length === 1 ? near[0] : null);
  if (!t) return null;
  t.done = true;
  t.doneAt = Date.now();
  fresh = t.id;
  save();
  renderTodos();
  return t;
}

// Pour la vue Aujourd'hui : coche (ou décoche) une tâche par son id.
function setTodoDone(id, done) {
  const t = todos.find((x) => x.id === id);
  if (!t) return null;
  t.done = done;
  t.doneAt = done ? Date.now() : 0;
  save();
  renderTodos();
  return t;
}

function todoHTML(t) {
  return `<div class="todo glass${t.done ? ' done' : ''}${t.star ? ' star' : ''}${t.id === fresh ? ' fresh' : ''}" data-id="${t.id}">
    <button type="button" class="todo-check" data-act="toggle" title="${t.done ? 'Remettre à faire' : 'Fait'}">${icon('check')}</button>
    <span class="todo-text" title="Double-clic pour modifier">${esc(t.text)}</span>
    <div class="todo-actions">
      ${t.done ? '' : `<button type="button" class="icon-btn todo-star" data-act="star" title="Important">${icon('star')}</button>`}
      <button type="button" class="icon-btn del" data-act="delete" title="Supprimer">${icon('trash')}</button>
    </div>
  </div>`;
}

function renderTodos() {
  const open = openTodos(), done = doneTodos();
  const list = $('#todoList');
  if (!todos.length) {
    list.innerHTML = `<div class="empty">${icon('todo')}<b>Aucune tâche</b><p>Ajoute ce que tu as à faire : coche-le quand c'est fait, mets une étoile à ce qui compte.</p></div>`;
  } else {
    list.innerHTML = open.map(todoHTML).join('')
      + (open.length ? '' : '<div class="todo-clear">Tout est fait ✨</div>')
      + (done.length ? `<div class="sec-title"><span>Faites · ${done.length}</span><button type="button" data-act="clear-done">Effacer</button></div>` + done.map(todoHTML).join('') : '');
  }
  fresh = '';
  const total = todos.length;
  $('#todoSummary').textContent = !total ? 'Ta liste de tâches'
    : `${open.length} à faire${done.length ? ` · ${done.length} faite${done.length > 1 ? 's' : ''}` : ''}`;
  $('#todoProgress').style.setProperty('--p', total ? done.length / total : 0);
  $('#todoProgress').hidden = !total;
}

// Modification sur place : Entrée valide, Échap annule, vide = supprimer.
function editTodo(el, t) {
  const span = $('.todo-text', el);
  span.contentEditable = 'plaintext-only';
  span.focus();
  getSelection().selectAllChildren(span);
  getSelection().collapseToEnd();
  const finish = (keep) => {
    span.removeEventListener('blur', onBlur);
    span.removeEventListener('keydown', onKey);
    const text = span.textContent.trim().replace(/\s+/g, ' ');
    if (keep && !text) { todos = todos.filter((x) => x !== t); save(); }
    else if (keep) { t.text = text; save(); }
    renderTodos();
  };
  const onBlur = () => finish(true);
  const onKey = (e) => {
    if (e.key === 'Enter') { e.preventDefault(); finish(true); }
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); finish(false); } // pas de repli du dock
  };
  span.addEventListener('blur', onBlur);
  span.addEventListener('keydown', onKey);
}

function setupTodo() {
  $('#todoForm').addEventListener('submit', (e) => {
    e.preventDefault();
    if (addTodo($('#todoInput').value)) $('#todoInput').value = '';
  });
  const list = $('#todoList');
  list.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-act]');
    if (!btn) return;
    if (btn.dataset.act === 'clear-done') {
      todos = todos.filter((t) => !t.done);
      save();
      renderTodos();
      return;
    }
    const el = btn.closest('.todo');
    const t = todos.find((x) => x.id === el?.dataset.id);
    if (!t) return;
    if (btn.dataset.act === 'toggle') {
      // La coche s'anime, puis la tâche change de section.
      el.classList.toggle('checking', !t.done);
      t.done = !t.done;
      t.doneAt = t.done ? Date.now() : 0;
      save();
      setTimeout(() => { fresh = t.id; renderTodos(); }, document.body.classList.contains('reduce') ? 0 : 380);
    } else if (btn.dataset.act === 'star') {
      t.star = !t.star;
      fresh = t.id;
      save();
      renderTodos();
    } else if (btn.dataset.act === 'delete') {
      animateOut(el, () => { todos = todos.filter((x) => x !== t); save(); renderTodos(); });
    }
  });
  list.addEventListener('dblclick', (e) => {
    const el = e.target.closest('.todo');
    const t = todos.find((x) => x.id === el?.dataset.id);
    if (t && e.target.closest('.todo-text')) editTodo(el, t);
  });
}

export { addTodo, completeTodo, setTodoDone, openTodos, doneTodos, renderTodos, setupTodo };
