// Carnet de notes : une grille de cartes ; un clic sur une carte ouvre la note en entier sur le dock
// (#noteEditor) avec un titre, de la mise en forme et un thème de fond. Le contenu est du HTML limité
// à quelques balises (`sanitize`) : seul ce que l'éditeur sait produire est gardé.
import { $, $$, uid, esc, icon, store, animateOut } from '../core/util.js';
import { fmtRel } from '../core/format.js';

// [id, nom] : les couleurs sont dans notes.css ([data-nt="…"]).
const THEMES = [['verre', 'Verre'], ['aurore', 'Aurore'], ['ocean', 'Océan'], ['crepuscule', 'Crépuscule'], ['rose', 'Rose'], ['ambre', 'Ambre'], ['foret', 'Forêt'], ['nuit', 'Nuit']];
const OLD_COLORS = { violet: 'aurore', cyan: 'ocean', rose: 'rose', ambre: 'ambre', vert: 'foret' }; // ancien format : une couleur par note

// ------------------------------------------------------------------ contenu

const TAGS = { B: 'b', STRONG: 'b', I: 'i', EM: 'i', U: 'u', S: 's', STRIKE: 's', DEL: 's', H1: 'h1', H2: 'h2', H3: 'h2', H4: 'h2', H5: 'h2', H6: 'h2',
  P: 'p', DIV: 'div', BR: 'br', UL: 'ul', OL: 'ol', LI: 'li', BLOCKQUOTE: 'blockquote', MARK: 'mark' };
const BLOCKS = new Set(['h1', 'h2', 'p', 'div', 'ul', 'ol', 'blockquote']);
const DROP = new Set(['SCRIPT', 'STYLE', 'TEMPLATE', 'HEAD', 'TITLE']);

// Surlignage posé par l'éditeur : un fond (background-color) sur un <span>, ou sur un <b>, <i>… déjà là.
const hasBg = (el) => { const bg = el.style?.backgroundColor; return !!bg && bg !== 'transparent' && !/rgba\(\s*0,\s*0,\s*0,\s*0\s*\)/.test(bg); };
const INLINE = new Set(['b', 'i', 'u', 's']);

function cleanInto(src, dst) {
  for (const n of src.childNodes) {
    if (n.nodeType === 3) { dst.append(n.textContent); continue; }
    if (n.nodeType !== 1 || DROP.has(n.tagName)) continue;
    let tag = TAGS[n.tagName];
    if (n.tagName === 'SPAN') tag = hasBg(n) ? 'mark' : undefined;
    if (INLINE.has(tag) && hasBg(n)) { // <b style="background…"> -> <mark><b>…</b></mark>
      const mark = document.createElement('mark');
      const el = document.createElement(tag);
      cleanInto(n, el);
      mark.append(el);
      dst.append(mark);
      continue;
    }
    // Balise inconnue : on garde le texte. Un paragraphe qui contient un bloc (<p><ul>…, que l'éditeur
    // produit parfois) est aplati : relu, il se scinderait en paragraphes vides à chaque enregistrement.
    if (!tag || ((tag === 'p' || tag === 'div') && n.querySelector(':scope > :is(ul, ol, blockquote, p, div, h1, h2, h3)'))) { cleanInto(n, dst); continue; }
    const el = document.createElement(tag);
    if (tag === 'ul' && n.hasAttribute('data-check')) el.setAttribute('data-check', '');
    if (tag === 'li' && n.classList.contains('done')) el.className = 'done';
    cleanInto(n, el);
    dst.append(el);
  }
}

// HTML quelconque -> HTML de note : balises connues seulement, texte et inlines isolés rangés dans des <p>.
function sanitize(html) {
  const src = document.createElement('div');
  src.innerHTML = html || '';
  const flat = document.createElement('div');
  cleanInto(src, flat);
  const out = document.createElement('div');
  let para = null;
  for (const n of [...flat.childNodes]) { // copie : append déplace les nœuds
    if (n.nodeType === 1 && BLOCKS.has(n.tagName.toLowerCase())) {
      para = null;
      if (!n.firstChild && /^(p|div)$/i.test(n.tagName)) continue; // paragraphe vide (sans même un <br>)
      out.append(n);
      continue;
    }
    if (!para) { para = document.createElement('p'); out.append(para); }
    para.append(n);
  }
  return out.innerHTML;
}

const textToHtml = (text) => String(text || '').split('\n').map((l) => `<p>${l.trim() ? esc(l) : '<br>'}</p>`).join('');

// Lignes de texte brut d'une note (aperçu des cartes) ; les listes gardent leurs puces.
function toLines(html) {
  const root = document.createElement('div');
  root.innerHTML = sanitize(html);
  root.querySelectorAll('br').forEach((b) => b.replaceWith('\n'));
  const lines = [];
  for (const b of root.children) {
    if (b.tagName === 'UL' || b.tagName === 'OL') {
      [...b.children].forEach((li, i) => {
        const mark = b.hasAttribute('data-check') ? (li.classList.contains('done') ? '☑ ' : '☐ ') : b.tagName === 'OL' ? `${i + 1}. ` : '• ';
        lines.push(mark + li.textContent.trim());
      });
    } else lines.push(...b.textContent.split('\n'));
  }
  return lines.map((l) => l.trim()).filter(Boolean);
}

// ------------------------------------------------------------------ données

function upgrade(n) { // ancien format { text, color } -> { title, html, theme }
  if (typeof n.html === 'string') return n;
  return { id: n.id || uid(), title: '', html: textToHtml(n.text), theme: OLD_COLORS[n.color] || 'verre', updated: n.updated || Date.now() };
}

let notes = store.get('notes', []).map(upgrade);
let cur = null;     // note ouverte dans l'éditeur
let dirty = false;  // l'éditeur a du contenu pas encore copié dans `cur`
let saveTimer;
const save = () => store.set('notes', notes);
const themeName = (id) => (THEMES.find((t) => t[0] === id) || THEMES[0])[1];

// Pour Claude (tools.js) : une note de texte brut, qui n'ouvre pas l'éditeur.
function addNote(text = '') {
  const n = { id: uid(), title: '', html: textToHtml(text), theme: THEMES[1 + notes.length % (THEMES.length - 1)][0], updated: Date.now() };
  notes.unshift(n);
  save();
  renderNotes();
  return n;
}

// Pour la revue des notes par Claude (notereview.js) : lecture dans l'ordre de la grille, puis
// remplacement du titre et/ou du contenu d'une note, accepté par l'utilisateur sur la carte du chat.
const notesByRecent = () => [...notes].sort((a, b) => b.updated - a.updated);
const getNote = (id) => notes.find((x) => x.id === id) || null;

function updateNote(id, { title, html }) {
  const n = getNote(id);
  if (!n) return null;
  if (cur === n) { clearTimeout(saveTimer); dirty = false; } // note ouverte : le remplacement l'emporte sur une frappe pas encore copiée
  if (typeof title === 'string') n.title = title;
  if (typeof html === 'string') n.html = sanitize(html);
  n.updated = Date.now();
  save();
  if (cur === n) {
    $('#neTitle').value = n.title;
    body().innerHTML = sanitize(n.html) || '<p><br></p>';
    syncEmpty();
    showTime();
  }
  renderNotes();
  return n;
}

// Copie dans la note ouverte ce qui est encore dans l'éditeur (sinon `updated` serait en retard).
const flushOpenNote = () => commit();

// ------------------------------------------------------------------ grille

// Titre affiché : celui de la note, sinon sa première ligne (notes écrites par Claude ou anciennes).
function cardParts(n) {
  const lines = toLines(n.html);
  return n.title ? { title: n.title, lines } : { title: lines[0] || '', lines: lines.slice(1) };
}

function renderNotes() {
  const grid = $('#notesGrid');
  if (!notes.length) {
    grid.innerHTML = `<div class="empty" style="grid-column:1/-1">${icon('note')}<b>Aucune note</b><p>Garde ici tes idées, listes et bouts de texte à portée de souris.</p></div>`;
  } else {
    grid.innerHTML = [...notes].sort((a, b) => b.updated - a.updated).map((n) => {
      const { title, lines } = cardParts(n);
      return `<div class="note glass" role="button" tabindex="0" data-id="${n.id}" data-nt="${n.theme}">
        <h3 class="note-title${title ? '' : ' none'}">${esc(title || 'Sans titre')}</h3>
        <p class="note-text">${esc(lines.join('\n'))}</p>
        <div class="note-foot"><span>${fmtRel(n.updated)}</span><button type="button" class="icon-btn del" title="Supprimer">${icon('trash')}</button></div>
      </div>`;
    }).join('');
  }
  $('#notesSummary').textContent = notes.length ? `${notes.length} note${notes.length > 1 ? 's' : ''}` : 'Ton carnet rapide';
}

// ------------------------------------------------------------------ éditeur

const ed = () => $('#noteEditor');
const body = () => $('#neBody');

function commit() {
  clearTimeout(saveTimer);
  if (!cur || !dirty) return;
  dirty = false;
  cur.title = $('#neTitle').value.trim();
  cur.html = sanitize(body().innerHTML);
  cur.updated = Date.now();
  save();
  showTime();
}

function touch() {
  dirty = true;
  syncEmpty();
  clearTimeout(saveTimer);
  saveTimer = setTimeout(commit, 400);
}

const showTime = () => { $('#neTime').textContent = cur ? `Modifiée ${fmtRel(cur.updated)}` : ''; };
const syncEmpty = () => body().classList.toggle('blank',!body().textContent.trim() && !body().querySelector('ul, ol, blockquote'));

function setTheme(id) {
  if (!cur) return;
  cur.theme = id;
  ed().dataset.nt = id;
  save();
  renderThemes();
}

function renderThemes() {
  $('#neThemes').innerHTML = THEMES.map(([id, name]) => `<button type="button" class="nt-dot${id === cur?.theme ? ' on' : ''}" data-nt="${id}" data-theme-id="${id}" title="${name}" aria-label="${name}"></button>`).join('')
    + `<span class="nt-name">${themeName(cur?.theme)}</span>`;
}

function placeCaretAtEnd(el) {
  el.focus();
  const r = document.createRange();
  r.selectNodeContents(el);
  r.collapse(false);
  const s = getSelection();
  s.removeAllRanges();
  s.addRange(r);
}

function openNote(id, isNew = false) {
  const n = notes.find((x) => x.id === id);
  if (!n) return;
  commit();
  cur = n;
  dirty = false;
  $('#neTitle').value = n.title;
  body().innerHTML = sanitize(n.html) || '<p><br></p>';
  body().scrollTop = 0;
  ed().dataset.nt = n.theme;
  $('#neThemes').classList.remove('open');
  rearmDelete();
  renderThemes();
  syncEmpty();
  showTime();
  ed().classList.add('open');
  ed().setAttribute('aria-hidden', 'false');
  setTimeout(() => (isNew ? $('#neTitle').focus() : placeCaretAtEnd(body())), document.body.classList.contains('reduce') ? 0 : 160);
}

function closeNote() {
  if (!cur) return;
  commit();
  const n = cur;
  cur = null;
  if (!n.title && !toLines(n.html).length) { notes = notes.filter((x) => x !== n); save(); } // note restée vide : on ne la garde pas
  ed().classList.remove('open');
  ed().setAttribute('aria-hidden', 'true');
  document.activeElement?.blur();
  renderNotes();
}

function newNote() {
  const n = { id: uid(), title: '', html: '', theme: 'verre', updated: Date.now() };
  notes.unshift(n);
  save();
  renderNotes();
  openNote(n.id, true);
}

// Suppression en deux temps (un clic arme le bouton, le suivant supprime).
let armTimer;
function rearmDelete() {
  clearTimeout(armTimer);
  $('#neDel').classList.remove('arm');
}
function deleteOpen() {
  const b = $('#neDel');
  if (!b.classList.contains('arm')) {
    b.classList.add('arm');
    armTimer = setTimeout(rearmDelete, 2500);
    return;
  }
  const n = cur;
  rearmDelete();
  clearTimeout(saveTimer);
  cur = null;
  dirty = false;
  notes = notes.filter((x) => x !== n);
  save();
  ed().classList.remove('open');
  ed().setAttribute('aria-hidden', 'true');
  renderNotes();
}

// ---- mise en forme (document.execCommand : seul moyen portable WebView2 / WebKitGTK d'éditer du texte riche)

const inBody = (node) => node && body().contains(node);
function closest(sel) {
  const s = getSelection();
  let n = s.anchorNode;
  if (!inBody(n)) return null;
  if (n.nodeType === 3) n = n.parentElement;
  const el = n.closest(sel);
  return el && body().contains(el) ? el : null;
}
function inMark() {
  const s = getSelection();
  for (let n = inBody(s.anchorNode) ? s.anchorNode : null; n && n !== body(); n = n.parentNode) {
    if (n.nodeType !== 1) continue;
    if (n.tagName === 'MARK') return true;
    if (hasBg(n)) return true;
  }
  return false;
}

const exec = (cmd, val) => document.execCommand(cmd, false, val);
const HILITE = 'rgba(255, 214, 90, 0.38)'; // = mark dans notes.css

function toggleBlock(tag) { // titre, sous-titre, citation : un second clic revient au paragraphe
  const el = closest(tag);
  if (el && tag === 'blockquote') { el.replaceWith(...el.childNodes); return; }
  exec('formatBlock', el ? '<p>' : `<${tag}>`);
}

function command(cmd) {
  if (document.activeElement !== body()) placeCaretAtEnd(body());
  switch (cmd) {
    case 'bold': exec('bold'); break;
    case 'italic': exec('italic'); break;
    case 'underline': exec('underline'); break;
    case 'strike': exec('strikeThrough'); break;
    case 'mark':
      exec('styleWithCSS', true);
      exec('hiliteColor', inMark() ? 'transparent' : HILITE);
      exec('styleWithCSS', false);
      break;
    case 'h1': case 'h2': toggleBlock(cmd); break;
    case 'quote': toggleBlock('blockquote'); break;
    case 'ul': exec('insertUnorderedList'); if (closest('ul')) closest('ul').removeAttribute('data-check'); break;
    case 'ol': exec('insertOrderedList'); break;
    case 'check': {
      const ul = closest('ul');
      if (ul?.hasAttribute('data-check')) exec('insertUnorderedList');
      else { if (!ul) exec('insertUnorderedList'); closest('ul')?.setAttribute('data-check', ''); }
      break;
    }
    case 'clear': exec('removeFormat'); exec('formatBlock', '<p>'); closest('mark')?.replaceWith(...closest('mark').childNodes); break;
    default: return;
  }
  touch();
  syncTools();
}

function syncTools() {
  const s = getSelection();
  if (!ed().classList.contains('open') || !inBody(s.anchorNode)) return;
  const on = {
    bold: document.queryCommandState('bold'), italic: document.queryCommandState('italic'),
    underline: document.queryCommandState('underline'), strike: document.queryCommandState('strikeThrough'),
    mark: inMark(), h1: !!closest('h1'), h2: !!closest('h2'), quote: !!closest('blockquote'),
    ul: !!closest('ul:not([data-check])'), ol: !!closest('ol'), check: !!closest('ul[data-check]'),
  };
  $$('#neTools [data-cmd]').forEach((b) => b.classList.toggle('on', !!on[b.dataset.cmd]));
}

// Raccourcis à la Markdown : « # », « ## », « - », « 1. », « > », « [] » puis espace, en début de ligne vide.
const MD = [[/^#$/, 'h1'], [/^##$/, 'h2'], [/^[-*]$/, 'ul'], [/^1\.$/, 'ol'], [/^>$/, 'quote'], [/^\[\]$/, 'check']];
function markdownShortcut() {
  const s = getSelection();
  const t = s.anchorNode;
  if (!s.isCollapsed || t?.nodeType !== 3 || !inBody(t)) return;
  const text = t.textContent.replace(/[\s ]+$/, '');
  const hit = MD.find(([re]) => re.test(text));
  const block = t.parentElement.closest('p, div, li, h1, h2, blockquote') || body();
  if (!hit || block.textContent !== t.textContent || block.closest('li')) return;
  const r = document.createRange();
  r.selectNodeContents(t);
  s.removeAllRanges();
  s.addRange(r);
  exec('delete');
  command(hit[1]);
}

function setupEditor() {
  const e = ed(), b = body();
  document.execCommand('defaultParagraphSeparator', false, 'p');

  $('#neBack').addEventListener('click', closeNote);
  $('#neDel').addEventListener('click', deleteOpen);
  $('#neThemeBtn').addEventListener('click', () => $('#neThemes').classList.toggle('open'));
  $('#neThemes').addEventListener('click', (ev) => {
    const d = ev.target.closest('[data-theme-id]');
    if (d) setTheme(d.dataset.themeId);
  });
  $('#neTitle').addEventListener('input', touch);
  $('#neTitle').addEventListener('keydown', (ev) => { if (ev.key === 'Enter') { ev.preventDefault(); placeCaretAtEnd(b); } });

  // Les boutons ne prennent pas le focus : la sélection reste dans le texte.
  $('#neTools').addEventListener('mousedown', (ev) => { if (ev.target.closest('[data-cmd]')) ev.preventDefault(); });
  $('#neTools').addEventListener('click', (ev) => {
    const c = ev.target.closest('[data-cmd]');
    if (c) command(c.dataset.cmd);
  });

  b.addEventListener('input', (ev) => {
    if (ev.inputType === 'insertParagraph') { // la nouvelle ligne d'une liste n'hérite pas du « fait »
      const li = closest('li.done');
      if (li && !li.textContent.trim()) li.classList.remove('done');
    }
    if (ev.inputType === 'insertText' && /^[\s ]$/.test(ev.data || '')) markdownShortcut();
    touch();
    syncTools();
  });
  b.addEventListener('paste', (ev) => { // texte brut seulement : pas de HTML venu d'ailleurs
    ev.preventDefault();
    const text = ev.clipboardData.getData('text/plain');
    if (text) { exec('insertText', text); touch(); }
  });
  b.addEventListener('drop', (ev) => ev.preventDefault());
  b.addEventListener('mousedown', (ev) => { // clic dans le vide sous le texte : curseur à la fin
    if (ev.target === b && ev.button === 0) { ev.preventDefault(); placeCaretAtEnd(b); }
  });
  b.addEventListener('click', (ev) => { // case d'une liste à cocher
    const li = ev.target.closest('ul[data-check] > li');
    if (li && ev.clientX - li.getBoundingClientRect().left < 26) { li.classList.toggle('done'); touch(); }
  });
  e.addEventListener('keydown', (ev) => {
    if (ev.key !== 'Escape') return;
    ev.stopPropagation(); // pas de repli du dock
    if ($('#neThemes').classList.contains('open')) $('#neThemes').classList.remove('open');
    else closeNote();
  });
  e.addEventListener('focusout', commit);
  document.addEventListener('selectionchange', syncTools);
  document.addEventListener('visibilitychange', commit);
}

function setupNotes() {
  $('#addNote').addEventListener('click', newNote);
  const grid = $('#notesGrid');
  grid.addEventListener('click', (e) => {
    const el = e.target.closest('.note');
    const n = notes.find((x) => x.id === el?.dataset.id);
    if (!n) return;
    if (e.target.closest('.del')) {
      animateOut(el, () => { notes = notes.filter((x) => x !== n); save(); renderNotes(); });
      return;
    }
    openNote(n.id);
  });
  grid.addEventListener('keydown', (e) => {
    if ((e.key === 'Enter' || e.key === ' ') && e.target.classList.contains('note')) { e.preventDefault(); openNote(e.target.dataset.id); }
  });
  setupEditor();
  save(); // enregistre la migration de l'ancien format
}

export { addNote, renderNotes, setupNotes, sanitize, notesByRecent, getNote, updateNote, flushOpenNote };
