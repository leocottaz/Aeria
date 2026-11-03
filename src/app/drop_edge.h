// Bande de quelques pixels au bord haut de l'écran qui reconnaît un glisser de fichiers (OLE) :
// le dock rangé n'a aucune fenêtre visible à cet endroit, donc Poll ne voit qu'un bouton enfoncé.
// Windows seulement (Linux : bandes de linux/dock_linux.cpp).
#pragma once

#include <windows.h>

namespace drop_edge {

// Crée la bande. `message` est posté à `owner` quand des fichiers entrent dans la bande.
void Create(HINSTANCE instance, HWND owner, UINT message);

// Place la bande (pixels écran ; rectangle vide = cachée). `solid` : seulement pendant qu'un bouton de la
// souris est enfoncé, sinon la bande laisse passer les clics (ceux du bord haut de l'écran vont à l'appli dessous).
void Place(const RECT& r, bool solid);

} // namespace drop_edge
