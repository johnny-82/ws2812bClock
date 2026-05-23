#include "RTClib.h"
#include <TimeLib.h>
#include <Wire.h>
#include "myFont.h"
#include "meteo.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <SparkFun_APDS9960.h>
#include <Adafruit_PCF8574.h>

Adafruit_PCF8574 pcf;

#ifndef STASSID
#define STASSID "WIFI_SSID_PLACEHOLDER"
#define STAPSK "WIFI_PASS_PLACEHOLDER"
#endif

const char* host = "wificlock";
const char* update_path = "/firmware";
const char* ssid = STASSID;
const char* password = STAPSK;

volatile bool pcfInputs = false;
void ICACHE_RAM_ATTR pcfInputs_INT() {
  pcfInputs = true;
}
//OpenWeather API KEY
//API_KEY_PLACEHOLDER

//url
//http://api.openweathermap.org/data/2.5/forecast?q=dragoni,Campania,it&appid=API_KEY_PLACEHOLDER&lang=it&cnt=6&units=metric
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

RTC_DS3231 rtc;
unsigned long adesso = 0, prima = 0;
#include <NeoPixelBus.h>
#include <NeoPixelBusLg.h>
int cx = 0;
// I WS2812 sono pilotati via UART1 (DMA), uscita dati FISSA su GPIO2 = D4.
// Questo metodo e' immune agli NMI del WiFi -> scroll fluido anche con WiFi attivo.
#define PCF_INT_PIN D5
#define SDA_PIN D7
#define SCL_PIN D6
#define SX_btn_PCFpin 4
#define SU_btn_PCFpin 5
#define GIU_btn_PCFpin 6
#define DX_btn_PCFpin 7

#define NUM_LEDS 256
#define FONT_5x3 0
#define FONT_8x6 1

// Matrice WS2812 (GRB) su UART1/GPIO2 (D4). Variante "Lg" = luminanza globale
// (SetLuminance, equivalente a FastLED.setBrightness); NeoGammaNullMethod = niente
// correzione gamma, per mantenere i colori identici alla resa precedente.
NeoPixelBusLg<NeoGrbFeature, NeoEsp8266Uart1Ws2812xMethod, NeoGammaNullMethod> strip(NUM_LEDS);
uint8_t luminosita = 60;
//byte tonalita=0, saturazione=255, luminosita=128;

SparkFun_APDS9960 apds = SparkFun_APDS9960();
uint16_t ambient_light = 0;

// --- Meteo (OpenWeather) ---
const char* meteo_apikey = "API_KEY_PLACEHOLDER";
const char* meteo_localita = "dragoni,IT";
#define METEO_REFRESH 600000UL  // ricarica i dati al massimo ogni 10 minuti
float meteo_temp = 0;
int meteo_umidita = 0;
char meteo_icona[4] = "01d";  // codice icona OpenWeather (es. 01d, 04n)
bool meteo_ok = false;
unsigned long meteo_ultimoFetch = 0;

// --- Sincronizzazione tempo via Internet (NTP) ---
// Fuso orario Italia con ora legale automatica (CET/CEST)
#define TZ_ITALIA "CET-1CEST,M3.5.0,M10.5.0/3"

// --- WiFi sempre attivo ---
// Con i LED pilotati via UART1/DMA (immune agli NMI) il WiFi puo' restare
// sempre acceso senza disturbare lo scroll: OTA e gli endpoint /status, /version
// sono sempre raggiungibili, niente piu' finestre temporizzate.

int colore = 0xff0000;

time_t aggiornaDateTime() {
  DateTime now = rtc.now();
  tmElements_t datetime;
  datetime.Year = now.year() - 1970;
  datetime.Month = now.month();
  datetime.Day = now.day();
  datetime.Hour = now.hour();
  datetime.Minute = now.minute();
  datetime.Second = now.second();
  return makeTime(datetime);
}

// Allinea l'RTC all'ora di Internet (NTP). Con attendi=true aspetta la prima
// sincronizzazione (da chiamare nel setup); altrimenti usa l'ora di sistema
// gia' mantenuta in background dal client SNTP.
void aggiornaRTCdaNTP(bool attendi) {
  if (WiFi.status() != WL_CONNECTED) return;
  time_t ora = time(nullptr);
  if (attendi) {
    Serial.print("Sincronizzazione NTP");
    int tentativi = 0;
    while (ora < 1000000000 && tentativi < 40) {  // attende ora valida (post-2001)
      delay(250);
      Serial.print(".");
      ora = time(nullptr);
      tentativi++;
    }
    Serial.println();
  }
  if (ora < 1000000000) {
    Serial.println("NTP non disponibile, mantengo l'ora dell'RTC");
    return;
  }
  struct tm* tloc = localtime(&ora);
  rtc.adjust(DateTime(tloc->tm_year + 1900, tloc->tm_mon + 1, tloc->tm_mday,
                      tloc->tm_hour, tloc->tm_min, tloc->tm_sec));
  setTime(aggiornaDateTime());
  Serial.printf("RTC aggiornato da NTP: %02d/%02d/%04d %02d:%02d:%02d\n",
                tloc->tm_mday, tloc->tm_mon + 1, tloc->tm_year + 1900,
                tloc->tm_hour, tloc->tm_min, tloc->tm_sec);
}

// Disegna la pagina meteo (icona + temperatura) con i dati correnti.
void disegnaMeteo() {
  strip.ClearTo(RgbColor(0));
  bool notte = (meteo_icona[2] == 'n');
  bool sereno = (meteo_icona[0] == '0' && meteo_icona[1] == '1');
  if (sereno) disegna(notte ? notte_sereno : giorno_sereno, 0, 0);
  else disegna(notte ? notte_variabile : giorno_variabile, 0, 0);
  char buf[8];
  if (meteo_ok) sprintf(buf, "%d*", (int)lround(meteo_temp));
  else sprintf(buf, "--*");
  int x = 8 + (24 - larghezza(buf, FONT_5x3)) / 2;
  scrivi(buf, FONT_5x3, 2, x, colore);
}

// Scarica le condizioni meteo correnti da OpenWeather e aggiorna le variabili.
void scaricaMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=";
  url += meteo_localita;
  url += "&appid=";
  url += meteo_apikey;
  url += "&lang=it&units=metric";
  http.begin(client, url);
  int code = http.GET();
  Serial.print("Meteo HTTP ");
  Serial.println(code);
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      meteo_temp = doc["main"]["temp"] | 0.0f;
      meteo_umidita = doc["main"]["humidity"] | 0;
      const char* ic = doc["weather"][0]["icon"] | "01d";
      strncpy(meteo_icona, ic, 3);
      meteo_icona[3] = 0;
      meteo_ok = true;
      Serial.printf("Meteo: %.1f gradi, %d%%, icona %s\n", meteo_temp, meteo_umidita, meteo_icona);
    } else {
      Serial.print("Errore parsing JSON meteo: ");
      Serial.println(err.c_str());
    }
  }
  http.end();
  meteo_ultimoFetch = millis();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.flush();
  delay(2000);
  if (!pcf.begin(0x38, &Wire)) {
    Serial.println("Couldn't find PCF8574");
  }
  for (uint8_t p = 0; p < 8; p++) {
    pcf.pinMode(p, INPUT_PULLUP);
  }
  pinMode(PCF_INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PCF_INT_PIN), pcfInputs_INT, FALLING);
  // Initialize APDS-9960 (configure I2C and initial values)
  if (apds.init()) {
    Serial.println(F("APDS-9960 initialization complete"));
    // Start running the APDS-9960 light sensor (no interrupts)
    if (apds.enableLightSensor(false)) {
      Serial.println(F("Light sensor is now running"));
    } else {
      Serial.println(F("Something went wrong during light sensor init!"));
    }
  } else {
    Serial.println(F("Something went wrong during APDS-9960 init!"));
  }

  Serial.print("RTC");
  byte r = 0;
  while (!rtc.begin()) {
    Serial.print(".");
    r++;
    delay(10);
    if (r > 128) {
      Serial.println(" NON TROVATO!");
      break;
    }
  }
  if (r <= 128) Serial.println(" trovato");
  setSyncProvider(aggiornaDateTime);
  setSyncInterval(3600);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    Serial.println("WiFi failed, retrying.");
  }
  // Avvia il client NTP e allinea subito l'RTC all'ora di Internet
  configTime(TZ_ITALIA, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  aggiornaRTCdaNTP(true);
  scaricaMeteo();  // primo download cosi' la pagina meteo e' subito popolata
  MDNS.begin(host);

  httpUpdater.setup(&httpServer, update_path);
  httpServer.on("/version", []() {
    httpServer.send(200, "text/plain", "ws2812bClock build " __DATE__ " " __TIME__ "\n");
  });
  httpServer.on("/status", []() {
    time_t n = now();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "ora: %02d/%02d/%04d %02d:%02d:%02d\n"
             "meteo_ok: %s\n"
             "temp: %.1f C\n"
             "umidita: %d %%\n"
             "icona: %s\n"
             "wifi rssi: %ld dBm\n",
             day(n), month(n), year(n), hour(n), minute(n), second(n),
             meteo_ok ? "si" : "no", meteo_temp, meteo_umidita, meteo_icona, (long)WiFi.RSSI());
    httpServer.send(200, "text/plain", buf);
  });
  httpServer.begin();

  MDNS.addService("http", "tcp", 80);
  Serial.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser\n", host, update_path);
  Serial.println("per modificare data e ora digita 'dt=anno,mese,giorno,ora,minuti,secondi'");
  strip.Begin();
  strip.SetLuminance(luminosita);
  regolaLuminosita();
  strip.ClearTo(RgbColor(0));
  strip.Show();
  delay(1000);
}

#define LONG_PRESS_T 2000L
#define SHORT_PRESS_T 200L
#define BTN_REPEAT_T 200L
#define SX_ID 1
#define SU_ID 2
#define GIU_ID 3
#define DX_ID 4
#define PAGINA_ZERO 0
#define PAGINA_ORARIO 1
#define PAGINA_DATA 2
#define PAGINA_METEO 3
#define PAGINA_SVEGLIA1 4
#define PAGINA_SVEGLIA2 5
#define PAGINA_CONFIG 6
#define PAGINA_FINE 7

unsigned long t1, t_pagina, t_SX_btn, t_SU_btn, t_GIU_btn, t_DX_btn;
bool blink = true, AUTO_NEXT_PAGE = true, inizioPagina = true,
     SX_btn = false, SU_btn = false, GIU_btn = false, DX_btn = false,
     e_SX_btn = true, e_SU_btn = true, e_GIU_btn = true, e_DX_btn = true,
     r_SX_btn = false, r_SU_btn = true, r_GIU_btn = true, r_DX_btn = false,
     p_SX_btn = false, p_SU_btn = false, p_GIU_btn = false, p_DX_btn = false;
byte stato = 0, nuovo_stato = 0;
int pos_curs = 0;
int anno, mese, giorno, ore, minuti, secondi;
byte pagina = PAGINA_ZERO, nuova_pagina = PAGINA_ORARIO;
time_t t;

void BTN_ACTION(byte id, bool lp = false) {
  Serial.print("BTN_ACTION(");
  Serial.print(id);
  Serial.print(", ");
  Serial.print(lp ? "true" : "false");
  Serial.println(")");
  switch (id) {
    case SX_ID:
      if (lp) {
        //pressione lunga
        if (stato == 0) {
          AUTO_NEXT_PAGE = true;
        }
        if (stato == 1) {
          nuovo_stato = 0;
        }

      } else {
        //pressione corta
        if (stato == 1) {
          pos_curs--;
          if (pagina == PAGINA_ORARIO && pos_curs < 0) pos_curs = 1;
          if (pagina == PAGINA_DATA && pos_curs < 0) pos_curs = 2;
        }
      }
      break;
    case SU_ID:
      if (lp) {
        //pressione lunga: nessuna azione (WiFi sempre attivo, OTA sempre disponibile)
      } else {
        //pressione corta
        nuova_pagina--;
        if (nuova_pagina <= PAGINA_ZERO) nuova_pagina = PAGINA_FINE - 1;
        AUTO_NEXT_PAGE = false;
      }
      break;
    case GIU_ID:
      if (lp) {
        //pressione lunga
      } else {
        //pressione corta
        nuova_pagina++;
        if (nuova_pagina >= PAGINA_FINE) nuova_pagina = PAGINA_ZERO + 1;
        AUTO_NEXT_PAGE = false;
      }
      break;
    case DX_ID:
      if (lp) {
        //pressione lunga
        if (stato == 0) {
          if (pagina == 1) {
            //modifica ora
            stato = 1;
            nuovo_stato = true;
          }
        } else {
          //salva modifica ora
        }
      } else {
        //pressione corta
        if (stato == 1) {
          pos_curs++;
          if (pagina == PAGINA_ORARIO && pos_curs > 1) pos_curs = 0;
          if (pagina == PAGINA_DATA && pos_curs > 2) pos_curs = 0;
        }
      }
      break;
  }
}

void loop() {
  // WiFi sempre attivo: server OTA/HTTP e mDNS gestiti ad ogni giro
  httpServer.handleClient();
  MDNS.update();
  if (pcfInputs) {
    //Serial.println("Interrupt");
    pcfInputs = false;
    //leggi lo stato dei pulsanti
    SX_btn = !pcf.digitalRead(SX_btn_PCFpin), SU_btn = !pcf.digitalRead(SU_btn_PCFpin), GIU_btn = !pcf.digitalRead(GIU_btn_PCFpin), DX_btn = !pcf.digitalRead(DX_btn_PCFpin);

    if (SX_btn && !p_SX_btn) {
      Serial.println("SX_btn inizio pressione");
      BTN_ACTION(SX_ID);
      t_SX_btn = millis();
    }
    if (!SX_btn && p_SX_btn) {
      Serial.println("SX_btn rilasciato");
      e_SX_btn = true;
    }

    if (SU_btn && !p_SU_btn) {
      Serial.println("SU_btn inizio pressione");
      BTN_ACTION(SU_ID);
      t_SU_btn = millis();
    }
    if (!SU_btn && p_SU_btn) {
      Serial.println("SU_btn rilasciato");
      e_SU_btn = true;
    }

    if (GIU_btn && !p_GIU_btn) {
      Serial.println("GIU_btn inizio pressione");
      BTN_ACTION(GIU_ID);
      t_GIU_btn = millis();
    }
    if (!GIU_btn && p_GIU_btn) {
      Serial.println("GIU_btn rilasciato");
      e_GIU_btn = true;
    }

    if (DX_btn && !p_DX_btn) {
      Serial.println("DX_btn inizio pressione");
      BTN_ACTION(DX_ID);
      t_DX_btn = millis();
    }
    if (!DX_btn && p_DX_btn) {
      Serial.println("DX_btn rilasciato");
      e_DX_btn = true;
    }
  }
  if (SX_btn && p_SX_btn && e_SX_btn) {
    //Serial.println("SX_btn premuto");
    if (millis() - t_SX_btn >= LONG_PRESS_T) {
      BTN_ACTION(SX_ID, true);
      if (r_SX_btn) t_SX_btn += BTN_REPEAT_T;
      e_SX_btn = r_SX_btn;
    }
  }

  if (SU_btn && p_SU_btn && e_SU_btn) {
    //Serial.println("SU_btn premuto");
    if (millis() > t_SU_btn && millis() - t_SU_btn >= LONG_PRESS_T) {
      BTN_ACTION(SU_ID, true);
      if (r_SU_btn) t_SU_btn += BTN_REPEAT_T;
      e_SU_btn = r_SU_btn;
    }
  }

  if (GIU_btn && p_GIU_btn && e_GIU_btn) {
    //Serial.println("GIU_btn premuto");
    if (millis() - t_GIU_btn >= LONG_PRESS_T) {
      BTN_ACTION(GIU_ID, true);
      if (r_GIU_btn) t_GIU_btn += BTN_REPEAT_T;
      e_GIU_btn = r_GIU_btn;
    }
  }

  if (DX_btn && p_DX_btn && e_DX_btn) {
    //Serial.println("DX_btn premuto");
    if (millis() - t_DX_btn >= LONG_PRESS_T) {
      BTN_ACTION(DX_ID, true);
      if (r_DX_btn) t_DX_btn += BTN_REPEAT_T;
      e_DX_btn = r_DX_btn;
    }
  }

  p_SX_btn = SX_btn;
  p_DX_btn = DX_btn;
  p_SU_btn = SU_btn;
  p_GIU_btn = GIU_btn;

  if (Serial.available()) {
    String sr = "", s = "";
    sr = Serial.readString();
    sr.trim();
    Serial.println(sr);
    if (sr.startsWith("dt=")) {
      int i = 3, f = 4, c = 0;
      while (true) {
        f = sr.indexOf(",", i);
        if (f == -1) f = sr.length();
        s = sr.substring(i, f);
        Serial.print(c);
        Serial.print("=");
        Serial.print(s);
        Serial.print(" (");
        Serial.print(i);
        Serial.print(",");
        Serial.print(f);
        Serial.println(")");
        i = f + 1;
        if (c == 0) anno = s.toInt();
        if (c == 1) mese = s.toInt();
        if (c == 2) giorno = s.toInt();
        if (c == 3) ore = s.toInt();
        if (c == 4) minuti = s.toInt();
        if (c == 5) secondi = s.toInt();
        c++;
        if (c > 5) break;
      }
      Serial.println("Aggiornamento DateTime...");
      rtc.adjust(DateTime(anno, mese, giorno, ore, minuti, secondi));
      setTime(aggiornaDateTime());
    } else if (sr.startsWith("br=")) {
      luminosita = sr.substring(3, sr.length()).toInt();
      Serial.print("luminosita=");
      Serial.println(luminosita);
      strip.SetLuminance(luminosita);
    }
  }
  if (stato != nuovo_stato) {
    stato = nuovo_stato;
  }
  if (stato == 0) {
    if (pagina == PAGINA_ORARIO) {
      if (inizioPagina) {
        t_pagina = millis();
        inizioPagina = false;
      }
      if (millis() - t1 > 500) {
        t = now();
        t1 = millis();
        strip.ClearTo(RgbColor(0));
        char buf[6];
        sprintf(buf, "%02d%s%02d", hour(t), (blink ? ":" : " "), minute(t));
        scrivi(buf, FONT_8x6, 0, 1, colore);
        strip.Show();
        blink = !blink;
      }
      if (AUTO_NEXT_PAGE) {
        if (millis() - t_pagina >= 10000) nuova_pagina = PAGINA_DATA;
      }
    }
    if (pagina == PAGINA_DATA) {
      if (inizioPagina) {
        cx = 31;
        t = now();
        inizioPagina = false;
      }
      if (millis() - t1 > 75) {
        char buf[40];
        sprintf(buf, "%s, %d %s %d", ggSettStr(weekday(t)), day(t), meseStr(month(t)), year(t));
        t1 = millis();
        strip.ClearTo(RgbColor(0));
        scrivi(buf, FONT_5x3, 2, cx, colore);
        strip.Show();
        cx--;
        if (AUTO_NEXT_PAGE) {
          if (cx < 0 - larghezza(buf, FONT_5x3)) nuova_pagina = PAGINA_METEO;
        }else{
          inizioPagina=true;
        }
      }
    }
    if (pagina == PAGINA_METEO) {
      if (inizioPagina) {
        t_pagina = millis();
        inizioPagina = false;
        // mostra subito i dati correnti (anche se vecchi)
        disegnaMeteo();
        strip.Show();
        // WiFi sempre attivo: se i dati sono scaduti, aggiorna e ridisegna
        if (!meteo_ok || millis() - meteo_ultimoFetch >= METEO_REFRESH) {
          scaricaMeteo();
          disegnaMeteo();
          strip.Show();
        }
      }
      if (AUTO_NEXT_PAGE) {
        if (millis() - t_pagina >= 5000) nuova_pagina = PAGINA_ORARIO;
      }
    }
    //if (inizioPagina) nuova_pagina = PAGINA_ORARIO;
    if (nuova_pagina != pagina) {
      pagina = nuova_pagina;
      inizioPagina = true;
      Serial.print("pagina=");
      Serial.println(pagina);
      regolaLuminosita();
    }
  }
  if (stato == 1) {
    //modifica pagina
  }
}
int larghezza(String testo, byte font) {
  byte b;
  int totale = 0, lunghezza = 0, elemento, nc = 3;
  if (font == FONT_8x6) nc = 6;
  for (int i = 0; i < testo.length(); i++) {
    elemento = numEl(testo.charAt(i));
    for (int c = 0; c < nc; c++) {
      if (font == FONT_5x3) b = grafica5x3[elemento][c];
      if (font == FONT_8x6) b = grafica8x6[elemento][c];
      if (b > 0) lunghezza = c + 2;
      if (elemento == 11) lunghezza = floor(nc / 3) + 1;
    }
    totale += lunghezza;
  }
  return totale;
}
void scrivi(String testo, byte font, int startRiga, int startColonna, unsigned int colore) {
  byte b1 = 0, b2 = 0;
  int nr = 5, nc = 3, elemento, lunghezza = 0;
  if (font == FONT_8x6) {
    nr = 8;
    nc = 6;
  }
  for (int i = 0; i < testo.length(); i++) {
    startColonna += lunghezza;
    elemento = numEl(testo.charAt(i));
    for (int c = 0; c < nc; c++) {
      if (font == FONT_5x3) b1 = grafica5x3[elemento][c];
      if (font == FONT_8x6) b1 = grafica8x6[elemento][c];
      if (b1 > 0) lunghezza = c + 2;
      for (int r = 0; r < nr; r++) {
        int lp = ledPos(startRiga + r, startColonna + c);
        if (lp >= 0 && lp < NUM_LEDS) {
          b2 = bitRead(b1, 7 - r);
          strip.SetPixelColor(lp, b2 ? HtmlColor(colore) : HtmlColor(0));
        }
      }
      if (elemento == 11) lunghezza = floor(nc / 3) + 1;
    }
  }
}
void disegna(RgbColor img[8][8], int startRiga, int startColonna) {
  for(int c=0;c<8;c++){
    for(int r=0;r<8;r++){
      strip.SetPixelColor(ledPos(startRiga + r, startColonna + c), img[r][c]);
    }
  }
}
int numEl(const char cod) {
  if (isDigit(cod)) return (cod - '0') + 1;
  if (cod == ' ') return 11;
  if (cod == '.') return 12;
  if (cod == ',') return 13;
  if (cod == ':') return 14;
  if (cod == '-') return 15;
  if (cod == '/') return 16;
  if (cod == '*') return 17;  // °
  if (cod == '%') return 18;
  if (isAlpha(cod) && isLowerCase(cod)) return int(cod) - 78;
  if (isAlpha(cod) && isUpperCase(cod)) return int(cod) - 46;
  return -1;
}
int ledPos(int riga, int colonna) {
  if (colonna >= 0 && colonna < 8) return (riga * 8) + colonna;
  if (colonna >= 8 && colonna <= 15) return (riga * 8) + 64 + colonna - 8;
  if (colonna >= 16 && colonna <= 23) return (riga * 8) + 128 + colonna - 16;
  if (colonna >= 24 && colonna <= 31) return (riga * 8) + 192 + colonna - 24;
  return -1;
}
char* meseStr(int m) {
  switch (m) {
    case 1:
      return "GEN";
      break;
    case 2:
      return "FEB";
      break;
    case 3:
      return "MAR";
      break;
    case 4:
      return "APR";
      break;
    case 5:
      return "MAG";
      break;
    case 6:
      return "GIU";
      break;
    case 7:
      return "LUG";
      break;
    case 8:
      return "AGO";
      break;
    case 9:
      return "SET";
      break;
    case 10:
      return "OTT";
      break;
    case 11:
      return "NOV";
      break;
    case 12:
      return "DIC";
      break;
  }
  return "---";
}
char* ggSettStr(byte gs) {
  switch (gs) {
    case 1:
      return "DOM";
      break;
    case 2:
      return "LUN";
      break;
    case 3:
      return "MAR";
      break;
    case 4:
      return "MER";
      break;
    case 5:
      return "GIO";
      break;
    case 6:
      return "VEN";
      break;
    case 7:
      return "SAB";
      break;
  }
  return "---";
}

void regolaLuminosita() {
  if (apds.readAmbientLight(ambient_light)) {
    luminosita = _min(map(ambient_light, 0, 99, 1, 50),50);
    Serial.print("Regolazione automatica luminosita=");
    Serial.print(luminosita);
    Serial.print(" (");
    Serial.print(ambient_light);
    Serial.println(")");
    strip.SetLuminance(luminosita);
  }
}