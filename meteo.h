#ifndef _METEO_H_
#define _METEO_H_
#include <NeoPixelBus.h>
#define azr RgbColor(0,0x7f,0xff)
#define bls RgbColor(0,0,100)
#define ylw RgbColor(255,150,80)
#define wht RgbColor(200,200,255)

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
  {azr,azr,azr,wht,wht,wht,wht,wht},
  {azr,azr,azr,azr,wht,wht,wht,azr}
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
  {bls,bls,ylw,ylw,ylw,ylw,bls,ylw},
  {bls,ylw,ylw,ylw,ylw,bls,bls,bls},
  {bls,ylw,ylw,ylw,bls,bls,bls,bls},
  {bls,ylw,ylw,ylw,wht,wht,wht,bls},
  {bls,bls,ylw,wht,wht,wht,wht,wht},
  {bls,bls,bls,wht,wht,wht,wht,wht},
  {ylw,bls,bls,bls,bls,wht,wht,bls}
};
#endif