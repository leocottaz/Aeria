// Pont avec l'hôte natif (src/web/bridge.cpp) : send() envoie "commande\nargument",
// http garde les requêtes en cours (mode Clé API et Claude Code) jusqu'à "done"/"error".

// Windows : WebView2 (chrome.webview). Linux : WebKitGTK, qui expose le gestionnaire « aeria » ;
// le natif répond en appelant window.__fromNative(json).
function linuxBridge() {
  const handler = window.webkit?.messageHandlers?.aeria;
  if (!handler) return null;
  document.documentElement.classList.add('linux'); // pas d'acrylique : la page dessine son fond
  let listener = null;
  window.__fromNative = (m) => listener?.({ data: m });
  return {
    postMessage: (s) => handler.postMessage(s),
    addEventListener: (type, fn) => { if (type === 'message') listener = fn; },
  };
}

const bridge = (window.chrome && window.chrome.webview) || linuxBridge();
const send = (...parts) => bridge && bridge.postMessage(parts.join('\n'));

const http = {
  pending: new Map(),
  seq: 0,
  request(method, path, betas, body, handlers) {
    const id = `r${++this.seq}_${Date.now()}`;
    this.pending.set(id, handlers);
    if (!bridge) {
      setTimeout(() => this.onError(id, -2, "Aperçu navigateur : l'API n'est disponible que dans l'application."), 300);
    } else {
      send('http', id, method, path, betas || '', body ? JSON.stringify(body) : '');
    }
    return id;
  },
  // Mode Gratuit : même principe, vers Groq ou Gemini (la clé est ajoutée par le natif).
  requestFree(service, method, path, body, handlers) {
    const id = `f${++this.seq}_${Date.now()}`;
    this.pending.set(id, handlers);
    if (!bridge) {
      setTimeout(() => this.onError(id, -2, "Aperçu navigateur : le modèle gratuit n'est disponible que dans l'application."), 300);
    } else {
      send('fhttp', id, service, method, path, body ? JSON.stringify(body) : '');
    }
    return id;
  },
  onChunk(id, data) { this.pending.get(id)?.onChunk?.(data); },
  onDone(id, cancelled, info) { const h = this.pending.get(id); this.pending.delete(id); h?.onDone?.(cancelled, info); },
  onError(id, status, body) { const h = this.pending.get(id); this.pending.delete(id); h?.onError?.(status, body); },
};

// Commandes qui attendent une réponse du natif ({"t":"reply","id":…}) : ask() rend une promesse.
const replies = new Map();
let replySeq = 0;

function ask(cmd, args = [], timeoutMs = 30000) {
  if (!bridge) return Promise.reject(new Error("Aperçu navigateur : disponible seulement dans l'application."));
  const id = `q${++replySeq}_${Date.now()}`;
  return new Promise((resolve, reject) => {
    replies.set(id, { resolve, reject });
    send(cmd, id, ...args);
    setTimeout(() => { if (replies.delete(id)) reject(new Error('Le dock ne répond pas.')); }, timeoutMs);
  });
}

function onReply(m) {
  const r = replies.get(m.id);
  if (!r) return;
  replies.delete(m.id);
  if (m.ok === false) r.reject(new Error(m.error || 'Échec.'));
  else r.resolve(m);
}

function apiErrorText(status, body) {
  let detail = '';
  try { detail = JSON.parse(body)?.error?.message || ''; } catch { detail = typeof body === 'string' ? body : ''; }
  if (status === -1) return 'Aucune clé API enregistrée.';
  if (status === -2 || status === 0) return detail || 'Erreur réseau.';
  if (status === 401) return 'Clé API invalide ou révoquée.';
  if (status === 403) return "Cette clé n'a pas accès à cette ressource.";
  if (status === 404) return detail || 'Modèle ou ressource introuvable.';
  if (status === 429) return 'Limite de débit atteinte, réessaie dans un instant.';
  if (status === 529 || status >= 500) return "L'API Claude est surchargée ou indisponible, réessaie.";
  return detail || `Erreur ${status}.`;
}

export { bridge, send, http, ask, onReply, apiErrorText };
