#ifndef _METEO_H_
#define _METEO_H_
#include <NeoPixelBus.h>
#define azr RgbColor(0,0x7f,0xff)
#define bls RgbColor(0,0,100)
#define ylw RgbColor(255,150,80)
#define wht RgbColor(200,200,255)
#define gry RgbColor(110,110,130)  // nuvola
#define rai RgbColor(40,110,255)   // pioggia
#define sno RgbColor(210,210,255)  // neve
#define blk RgbColor(0,0,0)        // sfondo trasparente (nero)

// Puntatore a una icona 8x8 (come accettato da disegna()/disegnaScroll()).
typedef RgbColor (*Icona8)[8];

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
// Icone senza variante giorno/notte (sfondo nero): usate per la previsione.
RgbColor nuvolo[8][8]{
  {blk,blk,blk,blk,blk,blk,blk,blk},
  {blk,blk,blk,blk,blk,blk,blk,blk},
  {blk,blk,blk,blk,blk,gry,gry,blk},
  {blk,blk,blk,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,gry},
  {blk,blk,gry,gry,gry,gry,gry,blk},
  {blk,blk,blk,blk,blk,blk,blk,blk}
};
RgbColor coperto[8][8]{
  {gry,gry,gry,gry,gry,blk,blk,blk},
  {gry,gry,gry,gry,gry,blk,blk,blk},
  {gry,gry,blk,blk,blk,blk,blk,blk},
  {blk,blk,blk,blk,blk,gry,gry,blk},
  {blk,blk,blk,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,gry},
  {blk,blk,gry,gry,gry,gry,gry,blk}
};
RgbColor pioggia[8][8]{
  {blk,blk,blk,gry,gry,blk,blk,blk},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {blk,rai,blk,blk,blk,blk,rai,blk},
  {rai,blk,blk,rai,blk,rai,blk,blk},
  {blk,blk,rai,blk,blk,blk,blk,blk}
};
RgbColor temporale[8][8]{
  {blk,blk,blk,gry,gry,blk,blk,blk},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {blk,blk,blk,ylw,ylw,blk,blk,blk},
  {blk,blk,ylw,ylw,blk,blk,blk,blk},
  {blk,blk,blk,ylw,blk,blk,blk,blk}
};
RgbColor neve[8][8]{
  {blk,blk,blk,gry,gry,blk,blk,blk},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {gry,gry,gry,gry,gry,gry,gry,gry},
  {blk,gry,gry,gry,gry,gry,gry,blk},
  {blk,sno,blk,blk,blk,sno,blk,blk},
  {sno,sno,sno,blk,sno,sno,sno,blk},
  {blk,sno,blk,blk,blk,sno,blk,blk}
};
RgbColor nebbia[8][8]{
  {gry,wht,gry,wht,gry,wht,gry,wht},
  {wht,gry,wht,gry,wht,gry,wht,gry},
  {gry,wht,gry,wht,gry,wht,gry,wht},
  {wht,gry,wht,gry,wht,gry,wht,gry},
  {gry,wht,gry,wht,gry,wht,gry,wht},
  {wht,gry,wht,gry,wht,gry,wht,gry},
  {gry,wht,gry,wht,gry,wht,gry,wht},
  {wht,gry,wht,gry,wht,gry,wht,gry}
};
#endif
