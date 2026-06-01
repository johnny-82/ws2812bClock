#ifndef SPLASH_H_
#define SPLASH_H_
#include <NeoPixelBus.h>

// Splash-screen mostrato una volta sola all'avvio (full-display 32x8).
// La matrice e' indicizzata [riga][colonna] come le icone di meteo.h, ma copre
// tutte e 32 le colonne. Disegnala/esportala con tools/pixel-editor.html (griglia
// 32x8) e sostituisci il contenuto qui sotto.
//
// Palette locale (nomi brevi solo per leggibilita' della matrice).
#define spO RgbColor(0, 0, 0)        // off / sfondo
#define spB RgbColor(0, 60, 120)     // bordo (azzurro tenue)

// PLACEHOLDER: cornice attorno al display. Sostituisci con il tuo disegno.
RgbColor splash[8][32]{
  {spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spO,spB},
  {spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB,spB}
};

#endif
