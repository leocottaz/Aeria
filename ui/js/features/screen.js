// Capture d'écran demandée par Aeria (outil `screenshot`) : le natif prend l'image, le dock n'y figure pas
// (src/features/screen.cpp). Les miniatures restent en mémoire pour la carte du chat, jamais enregistrées.
import { ask } from '../core/bridge.js';

const shots = new Map(); // id de l'appel d'outil → data URL

// file : le natif écrit aussi l'image dans le dossier de travail de Claude Code (mode Pro, qui la lit avec Read).
async function takeScreenshot(key, file) {
  const r = await ask('screenshot', [file ? 'file' : 'data'], 20000);
  if (!r.ok || !r.data) throw new Error(r.error || 'La capture a échoué.');
  shots.set(key, `data:${r.media};base64,${r.data}`);
  return r;
}

const shotFor = (key) => shots.get(key);

export { takeScreenshot, shotFor };
