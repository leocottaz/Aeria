// Démarrage : récupère les données enregistrées par le natif (fichiers JSON, voir core/util.js `store`)
// AVANT de charger main.js, car les modules lisent leurs données dès leur import (rappels, notes, conversation…).
import { bridge, send } from './core/bridge.js';
import { loadFileStore } from './core/util.js';

// Sans hôte natif (aperçu navigateur), pas d'attente. Si le natif ne répond pas (ne devrait pas arriver), on démarre
// quand même sur le localStorage au bout de 8 s plutôt que de laisser un dock muet.
function fetchStore(timeoutMs = 8000) {
  return new Promise((resolve) => {
    if (!bridge) { resolve(null); return; }
    const done = (data) => { clearTimeout(timer); resolve(data); };
    const timer = setTimeout(() => done(null), timeoutMs);
    bridge.addEventListener('message', (e) => { if (e.data && e.data.t === 'store') done(e.data.data); });
    send('loadstore');
  });
}

(async () => {
  loadFileStore(await fetchStore());
  await import('./main.js');
})();
