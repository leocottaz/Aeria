// Pièces jointes du chat : images, PDF et fichiers texte. La page lit les fichiers elle-même
// (File API : sélecteur, glisser-déposer, collage) ; rien ne passe par le natif.
import { $, uid, esc, icon, toast } from '../core/util.js';

const MAX_FILES = 5;
const MAX_TEXT = 100 * 1024;           // octets de texte gardés par fichier (le reste est coupé)
const MAX_PDF = 8 * 1024 * 1024;
const MAX_IMG_SIDE = 1568;             // au-delà, l'image est réduite (l'API la réduit de toute façon)
const MAX_IMG_BYTES = 3 * 1024 * 1024; // image plus lourde : ré-encodée en JPEG
const IMAGE_TYPES = ['image/png', 'image/jpeg', 'image/gif', 'image/webp'];
const TEXT_EXT = /\.(txt|md|markdown|csv|tsv|json|jsonl|xml|html?|css|scss|js|mjs|cjs|ts|tsx|jsx|py|rb|go|rs|c|h|cpp|hpp|cc|cs|java|kt|swift|php|sh|bat|ps1|sql|yml|yaml|toml|ini|cfg|conf|log|tex|srt|vtt|lrc|ics|svg)$/i;

let pending = []; // fichiers prêts à partir avec le prochain message

const dataUrl = (blob) => new Promise((resolve, reject) => {
  const r = new FileReader();
  r.onload = () => resolve(r.result);
  r.onerror = () => reject(r.error);
  r.readAsDataURL(blob);
});

function kindOf(file) {
  if (IMAGE_TYPES.includes(file.type)) return 'image';
  if (file.type === 'application/pdf' || /\.pdf$/i.test(file.name)) return 'pdf';
  if (file.type.startsWith('text/') || /json|xml|javascript/.test(file.type) || TEXT_EXT.test(file.name)) return 'text';
  return file.type.startsWith('image/') || file.type.startsWith('audio/') || file.type.startsWith('video/') ? null : 'text?'; // à sonder
}

async function readText(file, sniff) {
  const head = file.slice(0, MAX_TEXT);
  const text = new TextDecoder('utf-8').decode(await head.arrayBuffer());
  if (sniff && (text.includes('\0') || (text.match(/�/g) || []).length > text.length / 100)) return null; // binaire
  return { text, truncated: file.size > MAX_TEXT };
}

async function prepareImage(file) {
  const url = URL.createObjectURL(file);
  try {
    const img = await new Promise((resolve, reject) => {
      const i = new Image();
      i.onload = () => resolve(i);
      i.onerror = () => reject(new Error('image illisible'));
      i.src = url;
    });
    const scale = Math.min(1, MAX_IMG_SIDE / Math.max(img.naturalWidth, img.naturalHeight));
    if (scale === 1 && file.size <= MAX_IMG_BYTES) return { media: file.type, data: (await dataUrl(file)).split(',')[1] };
    const c = document.createElement('canvas');
    c.width = Math.max(1, Math.round(img.naturalWidth * scale));
    c.height = Math.max(1, Math.round(img.naturalHeight * scale));
    const ctx = c.getContext('2d');
    ctx.fillStyle = '#fff'; // JPEG n'a pas de transparence
    ctx.fillRect(0, 0, c.width, c.height);
    ctx.drawImage(img, 0, 0, c.width, c.height);
    return { media: 'image/jpeg', data: c.toDataURL('image/jpeg', 0.85).split(',')[1] };
  } finally { URL.revokeObjectURL(url); }
}

// Lit un fichier : renvoie la pièce jointe, ou lève une erreur dont le message dit pourquoi.
async function prepare(file) {
  const kind = kindOf(file);
  const att = { id: uid(), name: file.name || 'image', size: file.size };
  if (kind === 'image') return { ...att, kind, ...(await prepareImage(file)) };
  if (kind === 'pdf') {
    if (file.size > MAX_PDF) throw new Error('PDF trop lourd (8 Mo au plus)');
    return { ...att, kind, media: 'application/pdf', data: (await dataUrl(file)).split(',')[1] };
  }
  const t = kind ? await readText(file, kind === 'text?') : null;
  if (!t) throw new Error('type de fichier non pris en charge');
  if (!t.text.trim()) throw new Error('fichier vide');
  return { ...att, kind: 'text', ...t };
}

async function addFiles(files) {
  const list = [...files];
  if (!list.length) return;
  for (const file of list) {
    if (pending.length >= MAX_FILES) { toast(`${MAX_FILES} fichiers au plus par message`); break; }
    try {
      const att = await prepare(file);
      pending.push(att);
      renderBar();
    } catch (err) { toast(`${file.name || 'Fichier'} : ${err.message || 'lecture impossible'}`); }
  }
}

const attachments = () => pending;
const clearAttachments = () => { pending = []; renderBar(); };
const hasAttachments = () => pending.length > 0;
function restoreAttachments(files) { // le tour a échoué : les fichiers reviennent dans la saisie
  if (files?.length && !pending.length) { pending = files; renderBar(); }
}

// Raison pour laquelle ce mode de connexion ne peut pas lire ces fichiers ('' = tout passe).
// route : 'pro' | 'api' | 'gemini' | 'groq' | 'openai'.
function unsupported(files, route) {
  const kinds = new Set(files.map((f) => f.kind));
  if (route === 'pro' && (kinds.has('image') || kinds.has('pdf'))) {
    return 'Claude Code (abonnement Pro) ne lit que les fichiers texte : pour une image ou un PDF, passe en mode Clé API.';
  }
  if (route === 'groq' && kinds.has('image')) return 'Groq ne lit pas les images : choisis Gemini, ChatGPT ou le mode Clé API.';
  if ((route === 'gemini' || route === 'groq' || route === 'openai') && kinds.has('pdf')) return 'Ce modèle ne lit pas les PDF : passe en mode Clé API.';
  return '';
}

// Blocs de message au format de l'API Claude. Le texte est recopié tel quel ; image et PDF
// sont précédés d'une ligne qui donne leur nom.
function attachmentBlocks(files) {
  const out = [];
  for (const f of files) {
    if (f.kind === 'text') {
      out.push({ type: 'text', text: `<fichier nom="${f.name.replace(/"/g, "'")}"${f.truncated ? ' tronque="oui"' : ''}>\n${f.text}\n</fichier>` });
      continue;
    }
    out.push({ type: 'text', text: `[Fichier joint : ${f.name}]` });
    out.push(f.kind === 'image'
      ? { type: 'image', source: { type: 'base64', media_type: f.media, data: f.data } }
      : { type: 'document', source: { type: 'base64', media_type: 'application/pdf', data: f.data }, title: f.name });
  }
  return out;
}

// Ce qu'on garde dans la conversation enregistrée : le contenu binaire (images, PDF) est trop lourd
// pour le stockage local, on ne laisse qu'une trace.
const STORED_TEXT = 20000;
const forStorage = (m) => (m.files
  ? {
    ...m,
    content: m.content.map((c) => {
      if (c.type === 'image' || c.type === 'document') return { type: 'text', text: '[contenu non conservé]' };
      if (c.type === 'text' && c.text.startsWith('<fichier ') && c.text.length > STORED_TEXT) {
        return { type: 'text', text: `${c.text.slice(0, STORED_TEXT)}\n[…suite non conservée]\n</fichier>` };
      }
      return c;
    }),
  }
  : m);

const chipMeta = (f) => ({ image: 'Image', pdf: 'PDF', text: 'Texte' }[f.kind]);

// Pastilles des fichiers prêts à partir (au-dessus de la saisie).
function renderBar() {
  const bar = $('#attachBar');
  if (!bar) return;
  bar.hidden = !pending.length;
  bar.innerHTML = pending.map((f) => `<span class="att-chip" title="${esc(f.name)}">
      ${f.kind === 'image' ? `<img src="data:${f.media};base64,${f.data}" alt="">` : icon('file')}
      <span class="att-name">${esc(f.name)}</span><small>${chipMeta(f)}</small>
      <button type="button" data-att-remove="${f.id}" title="Retirer">${icon('x')}</button></span>`).join('');
}

// Pastilles d'un message envoyé (sans le contenu, qui n'est gardé que pour la session).
const sentChipsHTML = (files) => `<div class="att-sent">${files.map((f) => `<span class="att-chip">${icon('file')}<span class="att-name">${esc(f.name)}</span></span>`).join('')}</div>`;

const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');

// onDrop : appelé avec les fichiers déposés (le chat s'ouvre puis les reçoit).
function setupAttach(onDrop) {
  const input = $('#attachInput');
  $('#attachBtn').addEventListener('click', () => input.click());
  input.addEventListener('change', () => { addFiles(input.files); input.value = ''; });
  $('#attachBar').addEventListener('click', (e) => {
    const b = e.target.closest('[data-att-remove]');
    if (!b) return;
    pending = pending.filter((f) => f.id !== b.dataset.attRemove);
    renderBar();
  });
  $('#chatInput').addEventListener('paste', (e) => {
    const files = [...(e.clipboardData?.files || [])];
    if (!files.length) return;
    e.preventDefault();
    addFiles(files);
  });

  // Glisser-déposer sur tout le dock. Sans preventDefault, la vue naviguerait vers le fichier.
  let depth = 0;
  const hint = (on) => document.body.classList.toggle('dropping', on);
  addEventListener('dragenter', (e) => { if (hasFiles(e)) { depth++; hint(true); } });
  addEventListener('dragleave', (e) => { if (hasFiles(e) && --depth <= 0) { depth = 0; hint(false); } });
  addEventListener('dragover', (e) => { if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; } });
  addEventListener('drop', (e) => {
    e.preventDefault(); // aussi pour autre chose que des fichiers (texte, liens) : jamais de navigation
    depth = 0;
    hint(false);
    if (hasFiles(e)) { onDrop(); addFiles(e.dataTransfer.files); }
  });
}

export { setupAttach, addFiles, attachments, clearAttachments, hasAttachments, restoreAttachments, unsupported, attachmentBlocks, forStorage, sentChipsHTML };
