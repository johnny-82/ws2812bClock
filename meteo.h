#ifndef _METEO_H_
#define _METEO_H_
#include <NeoPixelBus.h>
#include "palette.h"   // palette di progetto condivisa (azr/ylw/blk/...)

// Puntatore a una icona 8x8 (come accettato da disegna()/disegnaScroll()).
typedef RgbColor (*Icona8)[8];

// Icone senza variante giorno/notte (sfondo nero): usate per la previsione.

RgbColor giorno_sereno[8][8]{
  {azr,azr,azr,azr,azr,azr,azr,azr},
  {azr,azr,ylw,ylw,ylw,ylw,azr,azr},
  {azr,ylw,ylw,ylw,ylw,ylw,ylw,azr},
  {azr,ylw,ylw,ylw,ylw,ylw,ylw,azr},
  {azr,ylw,ylw,ylw,ylw,ylw,ylw,azr},
  {azr,ylw,ylw,ylw,ylw,ylw,ylw,azr},
  {azr,azr,ylw,ylw,ylw,ylw,azr,azr},
  {azr,azr,azr,azr,azr,azr,azr,azr}
};
RgbColor giorno_variabile[8][8]{
  {azr,azr,azr,azr,azr,azr,azr,azr},
  {azr,azr,ylw,ylw,ylw,azr,azr,azr},
  {azr,ylw,ylw,ylw,ylw,ylw,azr,azr},
  {azr,ylw,ylw,ylw,ylw,wht,wht,azr},
  {azr,ylw,ylw,ylw,wht,wht,wht,wht},
  {azr,azr,ylw,wht,wht,wht,wht,wht},
  {wht,azr,azr,wht,wht,wht,wht,wht},
  {wht,wht,azr,azr,wht,wht,wht,azr}
};
RgbColor notte_sereno[8][8]{
  {bls,bls,bls,bls,bls,bls,bls,bls},
  {bls,bls,ylw,ylw,ylw,bls,bls,ylw},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,bls,ylw,ylw,ylw,bls,bls,bls},
  {bls,bls,bls,bls,bls,bls,bls,bls},
  {ylw,bls,bls,bls,bls,bls,bls,bls}
};
RgbColor notte_variabile[8][8]{
  {bls,bls,bls,bls,bls,bls,bls,bls},
  {bls,bls,ylw,ylw,ylw,bls,bls,ylw},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,ylw,ylw,ylw,wht,wht,wht,bls},
  {bls,bls,ylw,wht,wht,wht,wht,wht},
  {wht,bls,bls,wht,wht,wht,wht,wht},
  {wht,wht,bls,bls,wht,wht,wht,bls}
};
RgbColor nuvolo[8][8]{
  {rai,rai,rai,rai,rai,rai,rai,rai},
  {rai,rai,rai,rai,rai,rai,rai,rai},
  {rai,rai,rai,rai,rai,sno,sno,rai},
  {rai,rai,rai,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,sno},
  {rai,rai,sno,sno,sno,sno,sno,rai},
  {rai,rai,rai,rai,rai,rai,rai,rai}
};
RgbColor coperto[8][8]{
  {sno,sno,sno,sno,sno,rai,rai,rai},
  {sno,sno,sno,sno,sno,rai,rai,rai},
  {sno,sno,rai,rai,rai,rai,rai,rai},
  {rai,rai,rai,rai,rai,sno,sno,rai},
  {rai,rai,rai,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,sno},
  {rai,rai,sno,sno,sno,sno,sno,rai}
};
RgbColor pioggia[8][8]{
  {rai,rai,rai,sno,sno,rai,rai,rai},
  {rai,sno,sno,sno,sno,sno,sno,rai},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,rai},
  {rai,cyn,rai,rai,rai,rai,cyn,rai},
  {cyn,rai,rai,cyn,rai,cyn,rai,rai},
  {rai,rai,cyn,rai,rai,rai,rai,rai}
};
RgbColor temporale[8][8]{
  {rai,rai,rai,sno,sno,rai,rai,rai},
  {rai,sno,sno,sno,sno,sno,sno,rai},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {rai,sno,sno,sno,sno,sno,sno,rai},
  {rai,rai,rai,ylw,ylw,rai,rai,rai},
  {rai,rai,ylw,ylw,rai,rai,rai,rai},
  {rai,rai,rai,ylw,rai,rai,rai,rai}
};
RgbColor neve[8][8]{
  {sno,cyn,cyn,sno,sno,cyn,cyn,sno},
  {cyn,sno,cyn,sno,sno,cyn,sno,cyn},
  {cyn,cyn,sno,sno,sno,sno,cyn,cyn},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {sno,sno,sno,sno,sno,sno,sno,sno},
  {cyn,cyn,sno,sno,sno,sno,cyn,cyn},
  {cyn,sno,cyn,sno,sno,cyn,sno,cyn},
  {sno,cyn,cyn,sno,sno,cyn,cyn,sno}
};
RgbColor nebbia[8][8]{
  {sno,wht,sno,wht,sno,wht,sno,wht},
  {wht,sno,wht,sno,wht,sno,wht,sno},
  {sno,wht,sno,wht,sno,wht,sno,wht},
  {wht,sno,wht,sno,wht,sno,wht,sno},
  {sno,wht,sno,wht,sno,wht,sno,wht},
  {wht,sno,wht,sno,wht,sno,wht,sno},
  {sno,wht,sno,wht,sno,wht,sno,wht},
  {wht,sno,wht,sno,wht,sno,wht,sno}
};

#endif
