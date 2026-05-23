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
#include <EEPROM.h>

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

// Matrice WS2812 (GRB) su UART1/GPIO2 (D4). NeoPixelBus base non ha una luminanza
// globale affidabile (SetLuminance di Lg non scalava nel nostro flusso): la
// luminosita' la applichiamo noi a mano ai colori, vedi applicaLum().
NeoPixelBus<NeoGrbFeature, NeoEsp8266Uart1Ws2812xMethod> strip(NUM_LEDS);
// Luminosita' (0-255), applicata a mano ai colori in applicaLum(). Regolata
// automaticamente da regolaLuminosita() in base al sensore APDS-9960, con tetto 20.
// Regolabile/forzabile dal vivo via /lum.
uint8_t luminosita = 20;

SparkFun_APDS9960 apds = SparkFun_APDS9960();
uint16_t ambient_light = 0;
uint16_t amb_max = 800;      // soglia ambient per luminosita' max (tarabile via /lum?max=NN)
bool apds_init_ok = false;   // esito init APDS al boot
bool apds_light_ok = false;  // esito enableLightSensor al boot

// --- Sveglia ---
// Una sveglia: orario + giorni della settimana attivi (bit0=Lun .. bit6=Dom).
// Suona quando il giorno corrente e' attivo all'ora impostata. Persistita in
// EEPROM (flash emulata) per sopravvivere a reboot/OTA.
#define SV_MAGIC 0x5B          // marcatore di validita' della config in EEPROM
#define SV_EEPROM_ADDR 0
uint8_t sv_ore = 7;
uint8_t sv_min = 0;
uint8_t sv_giorni = 0;         // giorni attivi (bitmask Lun..Dom)
bool sv_attiva = true;         // interruttore on/off (conserva i giorni da spenta)
bool allarme_attivo = false;   // true mentre la sveglia sta "suonando"
int sv_minuto_scattato = -1;   // minuto del giorno in cui e' gia' scattata (anti-ripetizione)
const char* GG_SIGLA[7] = { "LU", "MA", "ME", "GI", "VE", "SA", "DO" };
// Buzzer attivo su D3 (GPIO0). NB: GPIO0 e' pin di strapping (deve stare alto al
// boot); il buzzer va pilotato in modo da non tenere basso il pin all'accensione.
#define BUZZER_PIN D3
void buzzer(bool on) {
#ifdef BUZZER_PIN
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

// Indice giorno 0=Lun..6=Dom a partire da weekday() di TimeLib (1=Dom..7=Sab).
int giornoIndex(int wd) { return (wd == 1) ? 6 : (wd - 2); }

void caricaSveglia() {
  struct { uint8_t magic, ore, min, giorni, attiva; } c;
  EEPROM.get(SV_EEPROM_ADDR, c);
  if (c.magic == SV_MAGIC && c.ore < 24 && c.min < 60) {
    sv_ore = c.ore; sv_min = c.min; sv_giorni = c.giorni; sv_attiva = c.attiva;
    Serial.printf("Sveglia caricata: %02d:%02d giorni=0x%02X attiva=%d\n", sv_ore, sv_min, sv_giorni, sv_attiva);
  } else {
    Serial.println("Sveglia: nessuna config valida in EEPROM, uso default");
  }
}

void salvaSveglia() {
  struct { uint8_t magic, ore, min, giorni, attiva; } c = { SV_MAGIC, sv_ore, sv_min, sv_giorni, (uint8_t)sv_attiva };
  EEPROM.put(SV_EEPROM_ADDR, c);
  EEPROM.commit();
  Serial.printf("Sveglia salvata: %02d:%02d giorni=0x%02X attiva=%d\n", sv_ore, sv_min, sv_giorni, sv_attiva);
}

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
#ifdef BUZZER_PIN
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // buzzer spento subito (evita "tic" al boot)
#endif
  EEPROM.begin(64);
  caricaSveglia();

  Wire.begin(SDA_PIN, SCL_PIN);
  // L'APDS-9960 fa clock stretching aggressivo: clock basso + limite alto
  // rendono affidabili le scritture sul bus condiviso.
  Wire.setClock(25000);
  Wire.setClockStretchLimit(150000);
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
  // Inizializza l'APDS-9960 (sensore di luce). Qualche tentativo perche' al
  // primo accesso post power-up puo' fallire finche' il chip non e' pronto.
  for (byte i = 0; i < 10 && !apds_init_ok; i++) {
    apds_init_ok = apds.init();
    if (!apds_init_ok) delay(50);
  }
  if (apds_init_ok) {
    Serial.println(F("APDS-9960 init ok"));
    apds_light_ok = apds.enableLightSensor(false);
    // enableLightSensor imposta gain 4x: lo alziamo a 64x per piu' risoluzione
    // (sensore con poca luce -> conteggi bassi -> regolazione a gradini).
    apds.setAmbientLightGain(AGAIN_64X);
    Serial.println(apds_light_ok ? F("Light sensor running") : F("Light sensor init FAILED"));
  } else {
    Serial.println(F("APDS-9960 init FAILED"));
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
  // /lum?v=NN forza la luminosita' (override temporaneo, 0-255).
  // /lum?max=NN tara la soglia di luce per il massimo e rilegge il sensore.
  httpServer.on("/lum", []() {
    if (httpServer.hasArg("v")) {
      luminosita = constrain(httpServer.arg("v").toInt(), 0, 255);
    }
    if (httpServer.hasArg("max")) {
      amb_max = constrain(httpServer.arg("max").toInt(), 1, 37000);
      regolaLuminosita();  // applica subito la nuova soglia
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "luminosita: %d/255\nambient: %d\namb_max: %d\n",
             luminosita, ambient_light, amb_max);
    httpServer.send(200, "text/plain", buf);
  });
  // Pagina web per impostare la sveglia (alternativa ai pulsanti).
  httpServer.on("/sveglia", []() {
    if (httpServer.hasArg("save")) {
      sv_attiva = httpServer.hasArg("on");
      sv_ore = constrain(httpServer.arg("ore").toInt(), 0, 23);
      sv_min = constrain(httpServer.arg("min").toInt(), 0, 59);
      sv_giorni = 0;
      for (int i = 0; i < 7; i++)
        if (httpServer.hasArg(String("g") + i)) sv_giorni |= (1 << i);
      salvaSveglia();
    }
    String h = "<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Sveglia</title><style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 12px}"
               "label{display:inline-block;margin:6px 10px 6px 0}input[type=number]{width:3em}"
               "button{margin-top:14px;padding:8px 16px;font-size:1em}</style></head><body>";
    h += "<h2>Sveglia</h2><form method=get action=/sveglia>";
    h += "<label><input type=checkbox name=on" + String(sv_attiva ? " checked" : "") + "> <b>Attiva</b></label><br><br>";
    h += "Ora: <input type=number name=ore min=0 max=23 value=" + String(sv_ore) + ">";
    h += " : <input type=number name=min min=0 max=59 value=" + String(sv_min) + "><br>";
    const char* gg[7] = { "Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom" };
    for (int i = 0; i < 7; i++) {
      h += "<label><input type=checkbox name=g" + String(i);
      if (sv_giorni & (1 << i)) h += " checked";
      h += "> " + String(gg[i]) + "</label>";
    }
    h += "<input type=hidden name=save value=1><br><button type=submit>Salva</button></form>";
    h += "<p>Stato: " + String((sv_attiva && sv_giorni) ? "ATTIVA" : (!sv_attiva ? "spenta (off)" : "spenta (nessun giorno)")) + "</p>";
    h += "</body></html>";
    httpServer.send(200, "text/html", h);
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
             "wifi rssi: %ld dBm\n"
             "luminosita: %d/255\n"
             "ambient: %d\n",
             day(n), month(n), year(n), hour(n), minute(n), second(n),
             meteo_ok ? "si" : "no", meteo_temp, meteo_umidita, meteo_icona, (long)WiFi.RSSI(),
             luminosita, ambient_light);
    httpServer.send(200, "text/plain", buf);
  });
  httpServer.begin();

  MDNS.addService("http", "tcp", 80);
  Serial.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser\n", host, update_path);
  Serial.println("per modificare data e ora digita 'dt=anno,mese,giorno,ora,minuti,secondi'");
  strip.Begin();
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
#define PAGINA_SVEGLIA 4
#define PAGINA_CONFIG 5
#define PAGINA_FINE 6
// Ultima pagina navigabile con SU/GIU: Orario, Data, Meteo, Sveglia, Config.
#define PAGINA_ULTIMA PAGINA_CONFIG
#define SV_NCAMPI 10  // campi modifica: 0=on/off,1=ore,2=min,3..9=giorni Lun..Dom

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

// Modifica il campo della sveglia attualmente sotto il cursore (modalita' setting).
// 0=ore, 1=minuti (dir +1/-1), 2..8=giorni (toggle, dir ignorato).
void modificaCampoSveglia(int dir) {
  if (pos_curs == 0) sv_attiva = !sv_attiva;
  else if (pos_curs == 1) sv_ore = (sv_ore + 24 + dir) % 24;
  else if (pos_curs == 2) sv_min = (sv_min + 60 + dir) % 60;
  else sv_giorni ^= (1 << (pos_curs - 3));
}

void BTN_ACTION(byte id, bool lp = false) {
  Serial.print("BTN_ACTION(");
  Serial.print(id);
  Serial.print(", ");
  Serial.print(lp ? "true" : "false");
  Serial.println(")");
  switch (id) {
    case SX_ID:
      if (lp) {
        if (stato == 0) {
          AUTO_NEXT_PAGE = true;                     // riprende l'avanzamento auto
          // da Sveglia/Config (fuori dal ciclo auto) riparti dall'orario
          if (pagina > PAGINA_METEO) nuova_pagina = PAGINA_ORARIO;
        } else { salvaSveglia(); nuovo_stato = 0; }  // esce dalla modifica e salva
      } else {
        if (stato == 1) pos_curs = (pos_curs + SV_NCAMPI - 1) % SV_NCAMPI;  // cursore <-
      }
      break;
    case SU_ID:
      if (lp) {
        //pressione lunga: nessuna azione
      } else {
        if (stato == 0) {
          nuova_pagina--;
          if (nuova_pagina < PAGINA_ORARIO) nuova_pagina = PAGINA_ULTIMA;
          AUTO_NEXT_PAGE = false;
        } else {
          modificaCampoSveglia(+1);                  // in modifica: aumenta/toggle
        }
      }
      break;
    case GIU_ID:
      if (lp) {
        //pressione lunga: nessuna azione
      } else {
        if (stato == 0) {
          nuova_pagina++;
          if (nuova_pagina > PAGINA_ULTIMA) nuova_pagina = PAGINA_ORARIO;
          AUTO_NEXT_PAGE = false;
        } else {
          modificaCampoSveglia(-1);                  // in modifica: diminuisci/toggle
        }
      }
      break;
    case DX_ID:
      if (lp) {
        // entra in modifica sveglia (solo dalla pagina sveglia)
        if (stato == 0 && pagina == PAGINA_SVEGLIA) { nuovo_stato = 1; pos_curs = 0; }
      } else {
        if (stato == 1) pos_curs = (pos_curs + 1) % SV_NCAMPI;  // cursore ->
      }
      break;
  }
}

void loop() {
  // WiFi sempre attivo: server OTA/HTTP e mDNS gestiti ad ogni giro
  httpServer.handleClient();
  MDNS.update();
  controllaSveglia();
  if (pcfInputs) {
    //Serial.println("Interrupt");
    pcfInputs = false;
    //leggi lo stato dei pulsanti
    SX_btn = !pcf.digitalRead(SX_btn_PCFpin), SU_btn = !pcf.digitalRead(SU_btn_PCFpin), GIU_btn = !pcf.digitalRead(GIU_btn_PCFpin), DX_btn = !pcf.digitalRead(DX_btn_PCFpin);

    // Se la sveglia sta suonando, un tasto qualsiasi la silenzia (e non fa altro)
    if (allarme_attivo && (SX_btn || SU_btn || GIU_btn || DX_btn)) {
      allarme_attivo = false;
      buzzer(false);
      p_SX_btn = SX_btn; p_SU_btn = SU_btn; p_GIU_btn = GIU_btn; p_DX_btn = DX_btn;
      return;
    }

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
    }
  }
  if (stato != nuovo_stato) {
    stato = nuovo_stato;
  }
  // Mentre la sveglia suona, la visualizzazione e' presa dall'allarme
  if (allarme_attivo) {
    mostraAllarme();
    return;
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
        // quando la data e' uscita tutta a sinistra: in automatico avanza al
        // meteo, in manuale riparte da capo (scroll in loop sulla pagina).
        if (cx < 0 - larghezza(buf, FONT_5x3)) {
          if (AUTO_NEXT_PAGE) nuova_pagina = PAGINA_METEO;
          else inizioPagina = true;
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
    if (pagina == PAGINA_SVEGLIA) {
      disegnaSveglia(false);  // visualizzazione (non entra nell'avanzamento auto)
    }
    if (pagina == PAGINA_CONFIG) {
      mostraConfig();         // mostra l'indirizzo web per configurare da browser
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
    // modalita' setting: per ora solo la sveglia
    disegnaSveglia(true);
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
          strip.SetPixelColor(lp, b2 ? applicaLum(HtmlColor(colore)) : RgbColor(0));
        }
      }
      if (elemento == 11) lunghezza = floor(nc / 3) + 1;
    }
  }
}
void disegna(RgbColor img[8][8], int startRiga, int startColonna) {
  for(int c=0;c<8;c++){
    for(int r=0;r<8;r++){
      strip.SetPixelColor(ledPos(startRiga + r, startColonna + c), applicaLum(img[r][c]));
    }
  }
}

// Scala un colore per la luminosita' globale corrente (0-255). NeoPixelBus base
// non ha una luminanza globale, quindi la applichiamo qui prima di SetPixelColor.
RgbColor applicaLum(RgbColor c) {
  return RgbColor((uint16_t)c.R * luminosita / 255,
                  (uint16_t)c.G * luminosita / 255,
                  (uint16_t)c.B * luminosita / 255);
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

// Regolazione auto (gain ALS 64x: buio ~0, luce stanza ~18, sole centinaia/migliaia).
// La soglia di luce per il massimo e' la variabile amb_max (tarabile via /lum?max=NN).
#define AMBIENT_BUIO 0     // buio -> LUM_MIN
#define LUM_MIN 1
#define LUM_MAX 20

void regolaLuminosita() {
  if (apds.readAmbientLight(ambient_light)) {
    luminosita = constrain(map(ambient_light, AMBIENT_BUIO, amb_max, LUM_MIN, LUM_MAX), LUM_MIN, LUM_MAX);
    Serial.print("Luminosita auto=");
    Serial.print(luminosita);
    Serial.print(" (ambient=");
    Serial.print(ambient_light);
    Serial.println(")");
  }
}

// Verifica se la sveglia deve scattare: giorno corrente attivo + ora coincidente.
// Scatta una sola volta per minuto (sv_minuto_scattato evita ripetizioni se silenziata).
void controllaSveglia() {
  if (!sv_attiva || sv_giorni == 0 || allarme_attivo) return;
  time_t n = now();
  int idx = giornoIndex(weekday(n));
  int minutoGiorno = hour(n) * 60 + minute(n);
  if ((sv_giorni & (1 << idx)) && hour(n) == sv_ore && minute(n) == sv_min
      && sv_minuto_scattato != minutoGiorno) {
    allarme_attivo = true;
    sv_minuto_scattato = minutoGiorno;
    Serial.println("SVEGLIA!");
  }
}

// Pagina config: scorre l'indirizzo web per configurare la sveglia da browser.
void mostraConfig() {
  static unsigned long t = 0;
  static int ccx = 31;
  const char* msg = "WIFICLOCK.LOCAL/SVEGLIA";
  if (millis() - t < 75) return;
  t = millis();
  strip.ClearTo(RgbColor(0));
  scrivi(msg, FONT_5x3, 2, ccx, colore);
  strip.Show();
  ccx--;
  if (ccx < 0 - larghezza(msg, FONT_5x3)) ccx = 31;
}

// Icona interruttore a levetta 3x5 (cornice grigia). Leva in alto/verde = ON,
// in basso/rosso = OFF. sc = colonna iniziale; nascondi = non disegnare (lampeggio).
void disegnaInterruttore(int sc, bool on, bool nascondi) {
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 3; c++) {
      RgbColor px(0, 0, 0);
      bool bordo = (r == 0 || r == 4) || (c == 0 || c == 2);  // cornice
      if (bordo) px = RgbColor(150, 150, 150);                // grigio
      else if (on && (r == 1 || r == 2)) px = RgbColor(0, 255, 0);   // leva su = verde
      else if (!on && (r == 2 || r == 3)) px = RgbColor(255, 0, 0);  // leva giu = rosso
      int lp = ledPos(r, sc + c);
      if (lp >= 0 && lp < NUM_LEDS)
        strip.SetPixelColor(lp, nascondi ? RgbColor(0) : applicaLum(px));
    }
  }
}

// Pagina sveglia: HH:MM in alto + 7 segmenti da 3 LED in basso (uno per giorno,
// Lun..Dom), verde=attivo / rosso=non attivo. In modifica (mod=true) il campo
// sotto il cursore lampeggia; sui giorni mostra anche la sigla accanto all'ora.
void disegnaSveglia(bool mod) {
  static unsigned long tdraw = 0, tblink = 0;
  static bool bl = true;
  if (millis() - tdraw < 80) return;  // limita il refresh/Show
  tdraw = millis();
  if (millis() - tblink > 300) { bl = !bl; tblink = millis(); }
  strip.ClearTo(RgbColor(0));
  // Disposizione riga alta: [interruttore] [HH:MM] [sigla giorno].
  // Interruttore on/off a sinistra (lampeggia se selezionato in modifica).
  disegnaInterruttore(0, sv_attiva, mod && pos_curs == 0 && !bl);
  // ora HH:MM (bianca) a posizione fissa; in modifica il campo selezionato
  // lampeggia (coperto in nero, cosi' le cifre non si spostano).
  const int ORA_COL = 5;
  char hh[3], mm[3];
  sprintf(hh, "%02d", sv_ore);
  sprintf(mm, "%02d", sv_min);
  scrivi(String(hh) + ":" + mm, FONT_5x3, 0, ORA_COL, 0xffffff);
  if (mod && !bl) {
    if (pos_curs == 1) scrivi(hh, FONT_5x3, 0, ORA_COL, 0x000000);                                          // ore
    else if (pos_curs == 2) scrivi(mm, FONT_5x3, 0, ORA_COL + larghezza(String(hh) + ":", FONT_5x3), 0x000000);  // minuti
  }
  // sigla del giorno selezionato, a destra (solo in modifica su un giorno)
  if (mod && pos_curs >= 3) scrivi(GG_SIGLA[pos_curs - 3], FONT_5x3, 0, 25, 0xffffff);
  // 7 segmenti giorni sulla riga in basso
  for (int g = 0; g < 7; g++) {
    bool attivo = sv_giorni & (1 << g);
    bool sel = mod && (pos_curs == 3 + g);
    RgbColor c = attivo ? RgbColor(0, 255, 0) : RgbColor(255, 0, 0);
    for (int k = 0; k < 3; k++) {
      int lp = ledPos(7, 2 + g * 4 + k);
      if (lp >= 0 && lp < NUM_LEDS)
        strip.SetPixelColor(lp, (sel && !bl) ? RgbColor(0) : applicaLum(c));
    }
  }
  strip.Show();
}

// Suoneria visiva: "SVEGLIA" scorrevole e lampeggiante (rosso) finche' non si
// preme un tasto. buzzer() e' un hook per un futuro cicalino.
void mostraAllarme() {
  static unsigned long t_scroll = 0, t_blink = 0;
  static int acx = 31;
  static bool vis = true;
  if (millis() - t_blink > 350) { vis = !vis; t_blink = millis(); buzzer(vis); }
  if (millis() - t_scroll > 60) {
    t_scroll = millis();
    strip.ClearTo(RgbColor(0));
    // font 5x3: il font 8x6 contiene solo cifre/simboli, non le lettere
    if (vis) scrivi("SVEGLIA", FONT_5x3, 2, acx, 0xff0000);
    strip.Show();
    acx--;
    if (acx < 0 - larghezza("SVEGLIA", FONT_5x3)) acx = 31;
  }
}
