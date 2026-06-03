#ifndef SPLASH_H_
#define SPLASH_H_
#include <NeoPixelBus.h>
#include "palette.h"   // palette di progetto condivisa (cyn = bordo, blk = sfondo)

// Splash-screen mostrato una volta sola all'avvio (full-display 32x8).
// La matrice e' indicizzata [riga][colonna] come le icone di meteo.h, ma copre
// tutte e 32 le colonne. Disegnala/esportala con tools/pixel-editor.html (griglia
// 32x8) e sostituisci il contenuto qui sotto.

// PLACEHOLDER: cornice attorno al display. Sostituisci con il tuo disegno.

RgbColor splash[8][32]{
  {bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls},
  {bls,rai,bls,bls,bls,rai,red,azr,azr,azr,red,bls,bls,bls,sno,sno,bls,bls,sno,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls},
  {bls,rai,bls,bls,bls,rai,bls,azr,bls,bls,bls,bls,bls,sno,bls,bls,sno,bls,sno,bls,bls,bls,bls,bls,bls,bls,bls,bls,sno,bls,sno,bls},
  {bls,rai,bls,bls,bls,rai,red,azr,bls,bls,red,bls,bls,sno,bls,bls,bls,bls,sno,bls,bls,sno,bls,bls,bls,sno,sno,bls,sno,bls,sno,bls},
  {bls,rai,bls,rai,bls,rai,red,azr,azr,azr,red,bls,bls,sno,bls,bls,bls,bls,sno,bls,sno,bls,sno,bls,sno,bls,bls,bls,sno,sno,bls,bls},
  {bls,rai,bls,rai,bls,rai,red,azr,bls,bls,red,bls,bls,sno,bls,bls,sno,bls,sno,bls,sno,bls,sno,bls,sno,bls,bls,bls,sno,bls,sno,bls},
  {bls,bls,rai,bls,rai,bls,red,azr,bls,bls,red,bls,bls,bls,sno,sno,bls,bls,sno,bls,bls,sno,bls,bls,bls,sno,sno,bls,sno,bls,sno,bls},
  {bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls,bls}
};

#endif
