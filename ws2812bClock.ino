#include "RTClib.h"
#include <TimeLib.h>
#include <Wire.h>
#include "myFont.h"
#include "meteo.h"
#include "splash.h"
#include "secrets.h"  // STASSID / STAPSK (non versionato, vedi secrets.h.example)
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
#include <DNSServer.h>

Adafruit_PCF8574 pcf;

#ifndef STASSID
#error "Manca secrets.h: copia secrets.h.example in secrets.h e inserisci le credenziali WiFi"
#endif

const char* host = "wificlock";
const char* update_path = "/firmware";

// --- Credenziali WiFi (persistite in EEPROM, con fallback ai default sopra) ---
#define WIFI_MAGIC 0x7C
#define WIFI_EEPROM_ADDR 16    // sveglia sta a 0 (pochi byte), wifi da 16
#define AP_SSID "WificlockSetup"  // access point del portale di configurazione
char wifi_ssid[33] = STASSID;
char wifi_pass[65] = STAPSK;
bool modo_portale = false;       // true mentre il portale di config WiFi e' attivo
bool richiesta_portale = false;  // settata da SU lungo per aprire il portale a richiesta
unsigned long portale_t0 = 0;    // istante di avvio del portale (per il timeout)
#define PORTALE_TIMEOUT 300000UL  // 5 min: se nessuno configura, riavvia e ritenta
DNSServer dnsServer;             // per il captive portal (redirect a 192.168.4.1)

volatile bool pcfInputs = false;
void ICACHE_RAM_ATTR pcfInputs_INT() {
  pcfInputs = true;
}
//url meteo (Open-Meteo, gratuito senza API key):
//http://api.open-meteo.com/v1/forecast?latitude=41.21&longitude=14.27&hourly=temperature_2m,relative_humidity_2m,weather_code,is_day&forecast_hours=7&timezone=auto
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
// Parametri della luminosita' automatica, persistiti in EEPROM (vedi caricaLum/salvaLum).
uint8_t lum_min = 1;         // luminosita' al buio (0-255)
uint8_t lum_max = 20;        // luminosita' a piena luce (0-255)
uint16_t amb_max = 800;      // soglia ambient per cui si raggiunge lum_max
bool apds_init_ok = false;   // esito init APDS al boot
bool apds_light_ok = false;  // esito enableLightSensor al boot

// EEPROM: sveglia a 0 (5 byte), luminosita' a 8 (6 byte), wifi a 16.
#define LUM_MAGIC 0x4C
#define LUM_EEPROM_ADDR 8
void caricaLum() {
  struct { uint8_t magic, lmin, lmax; uint16_t amax; } c;
  EEPROM.get(LUM_EEPROM_ADDR, c);
  if (c.magic == LUM_MAGIC && c.lmin <= c.lmax && c.amax > 0) {
    lum_min = c.lmin; lum_max = c.lmax; amb_max = c.amax;
    Serial.printf("Lum caricata: min=%d max=%d amb_max=%d\n", lum_min, lum_max, amb_max);
  } else {
    Serial.println("Lum: nessuna config valida in EEPROM, uso default");
  }
}
void salvaLum() {
  struct { uint8_t magic, lmin, lmax; uint16_t amax; } c = { LUM_MAGIC, lum_min, lum_max, amb_max };
  EEPROM.put(LUM_EEPROM_ADDR, c);
  EEPROM.commit();
  Serial.printf("Lum salvata: min=%d max=%d amb_max=%d\n", lum_min, lum_max, amb_max);
}

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
// Buzzer passivo su D3 (GPIO0). Pilotato con tone()/noTone() perche' un piezo
// passivo richiede onda quadra in banda audio (no DC). Frequenza tarabile a
// orecchio via /buzz?f=NN. NB: GPIO0 e' pin di strapping, ma noTone() lo lascia
// a LOW (lo stato impostato in setup), quindi nessun problema al boot.
#define BUZZER_PIN D3
uint16_t buzz_freq = 3000;  // tarata a orecchio sul nostro piezo
void buzzer(bool on) {
#ifdef BUZZER_PIN
  if (on) tone(BUZZER_PIN, buzz_freq);
  else    noTone(BUZZER_PIN);
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

struct WifiCfg { uint8_t magic; char ssid[33]; char pass[65]; };

void caricaWifi() {
  WifiCfg c;
  EEPROM.get(WIFI_EEPROM_ADDR, c);
  if (c.magic == WIFI_MAGIC && c.ssid[0]) {
    c.ssid[32] = 0; c.pass[64] = 0;
    strncpy(wifi_ssid, c.ssid, sizeof(wifi_ssid));
    strncpy(wifi_pass, c.pass, sizeof(wifi_pass));
    Serial.printf("WiFi: credenziali da EEPROM, ssid='%s'\n", wifi_ssid);
  } else {
    Serial.printf("WiFi: nessuna config in EEPROM, uso i default ('%s')\n", wifi_ssid);
  }
}

void salvaWifi(const char* s, const char* p) {
  WifiCfg c = { WIFI_MAGIC };
  strncpy(c.ssid, s, sizeof(c.ssid)); c.ssid[32] = 0;
  strncpy(c.pass, p, sizeof(c.pass)); c.pass[64] = 0;
  EEPROM.put(WIFI_EEPROM_ADDR, c);
  EEPROM.commit();
  Serial.printf("WiFi: credenziali salvate, ssid='%s'\n", c.ssid);
}

// Prova a connettersi in STA con le credenziali correnti, con timeout. true se connesso.
bool connettiWifi(unsigned long timeoutMs) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// Avvia il portale di configurazione WiFi: access point aperto + captive DNS.
void avviaPortale() {
  modo_portale = true;
  portale_t0 = millis();
  WiFi.mode(WIFI_AP_STA);          // AP per il portale, STA per lo scan reti
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();  // tipicamente 192.168.4.1
  dnsServer.start(53, "*", ip);    // qualsiasi dominio -> IP del portale
  Serial.printf("Portale WiFi attivo: rete '%s' -> http://%s\n", AP_SSID, ip.toString().c_str());
}

// --- Meteo (Open-Meteo, gratuito e senza API key) ---
// Coordinate di default (Dragoni CE), sovrascrivibili da /coord (geocoding) e
// persistite in EEPROM. Open-Meteo vuole lat/lon, non il nome localita'.
#define METEO_LAT_DEF 41.21
#define METEO_LON_DEF 14.27
#define METEO_NOME_DEF "Dragoni"
#define COORD_EEPROM_ADDR 120   // dopo WIFI (16..~114); EEPROM.begin(512)
#define COORD_MAGIC 0x3D
double meteo_lat = METEO_LAT_DEF;
double meteo_lon = METEO_LON_DEF;
char meteo_nome[24] = METEO_NOME_DEF;
#define METEO_REFRESH 600000UL  // ricarica i dati al massimo ogni 10 minuti
// Previsione a 3 slot da una sola chiamata: con forecast_hours l'array hourly
// parte dall'ora corrente, quindi gli indici 0/3/6 sono ora / +3h / +6h.
#define METEO_NSLOT 3
struct MeteoSlot {
  float temp;
  int umidita;
  int wmo;       // codice meteo WMO di Open-Meteo (0=sereno, 3=coperto, 61=pioggia...)
  bool giorno;   // is_day: true=icona diurna, false=notturna (per sereno/variabile)
};
MeteoSlot meteo_slot[METEO_NSLOT] = {
  { 0, 0, 0, true }, { 0, 0, 0, true }, { 0, 0, 0, true }
};
bool meteo_ok = false;
unsigned long meteo_ultimoFetch = 0;
int meteo_http_code = 0;          // ultimo codice HTTP (per diagnostica /meteo)
const char* meteo_last_err = "";  // ultimo errore di parsing/fetch (diagnostica)

// Qualita' dell'aria (European AQI da Open-Meteo air-quality), appesa alla scena meteo.
int aqi = -1;
bool aqi_ok = false;

// Messaggio scorrevole one-shot (impostato da /msg, non persistito in EEPROM).
#define MSG_REPEAT_DEF 3
String msgText = "";
int msgRepeatLeft = 0;

// Coordinate persistite in EEPROM (stesso modello di caricaSveglia/caricaWifi).
struct CoordCfg { uint8_t magic; double lat, lon; char nome[24]; };
void caricaCoord() {
  CoordCfg c;
  EEPROM.get(COORD_EEPROM_ADDR, c);
  if (c.magic == COORD_MAGIC && c.lat >= -90 && c.lat <= 90 && c.lon >= -180 && c.lon <= 180) {
    c.nome[sizeof(c.nome) - 1] = 0;
    meteo_lat = c.lat; meteo_lon = c.lon;
    strncpy(meteo_nome, c.nome, sizeof(meteo_nome)); meteo_nome[sizeof(meteo_nome) - 1] = 0;
    Serial.printf("Coord da EEPROM: %s (%.4f, %.4f)\n", meteo_nome, meteo_lat, meteo_lon);
  } else {
    Serial.printf("Coord: nessuna config valida, uso default %s\n", meteo_nome);
  }
}
void salvaCoord() {
  CoordCfg c = { COORD_MAGIC, meteo_lat, meteo_lon };
  strncpy(c.nome, meteo_nome, sizeof(c.nome)); c.nome[sizeof(c.nome) - 1] = 0;
  EEPROM.put(COORD_EEPROM_ADDR, c);
  EEPROM.commit();
  Serial.printf("Coord salvate: %s (%.4f, %.4f)\n", meteo_nome, meteo_lat, meteo_lon);
}

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

// Mappa il codice meteo WMO di Open-Meteo sull'icona 8x8 da mostrare.
// Tabella WMO: 0 sereno, 1-2 poco nuvoloso, 3 coperto, 45/48 nebbia,
// 51-67 pioviggine/pioggia, 71-77 neve, 80-82 rovesci, 85-86 rovesci di neve,
// 95-99 temporale. Per sereno/variabile c'e' la variante giorno/notte (is_day).
Icona8 iconaPer(int wmo, bool giorno) {
  switch (wmo) {
    case 0:  return giorno ? giorno_sereno : notte_sereno;        // cielo sereno
    case 1:
    case 2:  return giorno ? giorno_variabile : notte_variabile;  // poco nuvoloso
    case 3:  return coperto;                                      // coperto
    case 45:
    case 48: return nebbia;                                       // nebbia
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86: return neve;                                         // neve
    case 95:
    case 96:
    case 99: return temporale;                                    // temporale
    default: return pioggia;  // 51-67 pioviggine/pioggia, 80-82 rovesci
  }
}

// Descrizione breve del codice WMO (per la pagina /status, solo diagnostica).
const char* meteoDesc(int wmo) {
  switch (wmo) {
    case 0:  return "sereno";
    case 1:  return "quasi sereno";
    case 2:  return "poco nuvoloso";
    case 3:  return "coperto";
    case 45:
    case 48: return "nebbia";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86: return "neve";
    case 95:
    case 96:
    case 99: return "temporale";
    default: return "pioggia";
  }
}

// Imposta un pixel applicando la luminosita', con clipping fuori matrice.
void setPx(int riga, int colonna, RgbColor c) {
  int lp = ledPos(riga, colonna);
  if (lp >= 0 && lp < NUM_LEDS) strip.SetPixelColor(lp, applicaLum(c));
}

// Disegna un'icona 8x8 a una colonna qualsiasi (anche negativa/>31): le colonne
// fuori matrice vengono ignorate, quindi va bene per lo scrolling orizzontale.
void disegnaScroll(Icona8 img, int startColonna) {
  for (int c = 0; c < 8; c++)
    for (int r = 0; r < 8; r++)
      setPx(r, startColonna + c, img[r][c]);
}

// Splash-screen full-display 32x8 (vedi splash.h), mostrato una volta al boot.
void disegnaSplash() {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 32; c++)
      setPx(r, c, splash[r][c]);
  strip.Show();
}

// Ridisegna lo splash scalandone l'intensita' (scala 0..255), per il fade-out.
void disegnaSplashDim(uint8_t scala) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 32; c++) {
      RgbColor px = splash[r][c];
      setPx(r, c, RgbColor((px.R * scala) / 255, (px.G * scala) / 255, (px.B * scala) / 255));
    }
  strip.Show();
}

// Transizione splash -> ora: un paio di blink rapidi seguiti da un fade-out.
void animazioneSplash() {
  for (int i = 0; i < 2; i++) {
    strip.ClearTo(RgbColor(0)); strip.Show(); delay(110);
    disegnaSplash();                           delay(110);
  }
  for (int s = 255; s >= 0; s -= 15) {
    disegnaSplashDim((uint8_t)s);
    delay(28);
  }
  strip.ClearTo(RgbColor(0)); strip.Show();
}

// Freccetta ">" (chevron 3x5) usata come separatore tra i blocchi della scena.
void disegnaFreccia(int col, RgbColor c) {
  setPx(2, col, c);
  setPx(3, col + 1, c);
  setPx(4, col + 2, c);
  setPx(5, col + 1, c);
  setPx(6, col, c);
}

// --- Scena meteo da scorrere: [icona][temp] > [icona][temp] > [icona][temp] ---
#define METEO_GAP_ICO_TXT 1  // spazio tra icona e temperatura
#define METEO_SEP_W 5        // larghezza riservata al separatore tra blocchi
char meteo_txt[METEO_NSLOT][8];
Icona8 meteo_ico[METEO_NSLOT];
int meteo_blkX[METEO_NSLOT];  // colonna iniziale (relativa) dell'icona di ogni blocco
int meteo_sceneW = 0;         // larghezza totale della scena
int aq_blkX = -1;             // colonna iniziale del blocco AQI (-1 = assente)
String aq_txt;                // testo del blocco AQI ("AQ" + indice)

// Colore (0xRRGGBB) per categoria European AQI: piu' alto = aria peggiore.
unsigned int coloreAQI(int v) {
  if (v < 0)    return 0x787878;  // sconosciuto (grigio)
  if (v <= 20)  return 0x00C800;  // buona (verde)
  if (v <= 40)  return 0x96C800;  // discreta (verde-giallo)
  if (v <= 60)  return 0xDCC800;  // moderata (giallo)
  if (v <= 80)  return 0xFF7800;  // scarsa (arancio)
  if (v <= 100) return 0xFF0000;  // molto scarsa (rosso)
  return 0xB40078;                // estrema (viola)
}

// Categoria testuale per l'AQI europeo (per le pagine diagnostiche).
const char* categoriaAQI(int v) {
  if (v < 0)    return "n/d";
  if (v <= 20)  return "buona";
  if (v <= 40)  return "discreta";
  if (v <= 60)  return "moderata";
  if (v <= 80)  return "scarsa";
  if (v <= 100) return "molto scarsa";
  return "estrema";
}

// Prepara testi, icone e posizioni dei blocchi in base ai dati correnti.
void preparaScenaMeteo() {
  int x = 0;
  for (int i = 0; i < METEO_NSLOT; i++) {
    if (meteo_ok) sprintf(meteo_txt[i], "%d*", (int)lround(meteo_slot[i].temp));
    else sprintf(meteo_txt[i], "--*");
    meteo_ico[i] = iconaPer(meteo_slot[i].wmo, meteo_slot[i].giorno);
    meteo_blkX[i] = x;
    x += 8 + METEO_GAP_ICO_TXT + larghezza(meteo_txt[i], FONT_5x3);
    if (i < METEO_NSLOT - 1) x += METEO_SEP_W;
  }
  // Blocco qualita' dell'aria appeso in coda (solo se disponibile).
  if (aqi_ok) {
    x += METEO_SEP_W;
    aq_blkX = x;
    aq_txt = "AQ" + String(aqi);
    x += larghezza(aq_txt, FONT_5x3);
  } else {
    aq_blkX = -1;
  }
  meteo_sceneW = x;
}

// Disegna la scena meteo con l'icona+temperatura di ogni slot e le frecce di
// separazione, traslata di 'off' colonne (per lo scrolling).
void disegnaScenaMeteo(int off) {
  for (int i = 0; i < METEO_NSLOT; i++) {
    disegnaScroll(meteo_ico[i], off + meteo_blkX[i]);
    scrivi(meteo_txt[i], FONT_5x3, 2, off + meteo_blkX[i] + 8 + METEO_GAP_ICO_TXT, colore);
    if (i < METEO_NSLOT - 1)
      disegnaFreccia(off + meteo_blkX[i + 1] - METEO_SEP_W + 1, RgbColor(120, 120, 255));
  }
  // Blocco AQI in coda: freccia separatore + "AQ<indice>" colorato per categoria.
  if (aqi_ok && aq_blkX >= 0) {
    disegnaFreccia(off + aq_blkX - METEO_SEP_W + 1, RgbColor(120, 120, 255));
    scrivi(aq_txt, FONT_5x3, 2, off + aq_blkX, coloreAQI(aqi));
  }
}

// Scarica la previsione da OpenWeather (una sola chiamata /forecast, step di 3h)
// e popola i 3 slot: 0=ora, 1=+3h, 2=+6h.
void scaricaMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  // forecast_hours=7: l'array hourly parte dall'ora corrente, cosi' gli indici
  // 0/3/6 danno ora / +3h / +6h. Open-Meteo accetta HTTP semplice (no TLS).
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(meteo_lat, 4)
               + "&longitude=" + String(meteo_lon, 4)
               + "&hourly=temperature_2m,relative_humidity_2m,weather_code,is_day"
               + "&forecast_hours=7&timezone=auto";
  http.begin(client, url);
  int code = http.GET();
  meteo_http_code = code;
  meteo_last_err = "";
  Serial.print("Meteo HTTP ");
  Serial.println(code);
  if (code == HTTP_CODE_OK) {
    // IMPORTANTE: Open-Meteo risponde in Transfer-Encoding chunked. getStream()
    // espone i marker di chunk grezzi (es. "260\r\n{...}") e il parser fallisce
    // con InvalidInput; getString() invece DECODIFICA i chunk. Il payload e'
    // piccolo (~600 byte con forecast_hours=7), quindi niente filtro: si parsa
    // l'intero documento (gli array sono PARALLELI: hourly.temperature_2m[], ...).
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonObject h = doc["hourly"];
      JsonArray temp = h["temperature_2m"];
      JsonArray hum = h["relative_humidity_2m"];
      JsonArray wmo = h["weather_code"];
      JsonArray isday = h["is_day"];
      const int idx[METEO_NSLOT] = { 0, 3, 6 };  // ora, +3h, +6h
      int n = 0;
      for (int i = 0; i < METEO_NSLOT; i++) {
        int k = idx[i];
        if (k >= (int)temp.size()) break;
        meteo_slot[i].temp = temp[k] | 0.0f;
        meteo_slot[i].umidita = hum[k] | 0;
        meteo_slot[i].wmo = wmo[k] | 0;
        meteo_slot[i].giorno = (isday[k] | 1) != 0;
        n++;
      }
      if (n > 0) {
        meteo_ok = true;
        Serial.printf("Meteo: ora %.1f/wmo%d, +3h %.1f/wmo%d, +6h %.1f/wmo%d\n",
                      meteo_slot[0].temp, meteo_slot[0].wmo,
                      meteo_slot[1].temp, meteo_slot[1].wmo,
                      meteo_slot[2].temp, meteo_slot[2].wmo);
      }
    } else {
      meteo_last_err = err.c_str();
      Serial.print("Errore parsing JSON meteo: ");
      Serial.println(err.c_str());
    }
  } else {
    meteo_last_err = "HTTP != 200";
  }
  http.end();
  scaricaAQ();   // aggiorna l'AQI insieme al meteo (stesso refresh)
  meteo_ultimoFetch = millis();
}

// Scarica l'AQI europeo corrente da Open-Meteo air-quality (valore istantaneo).
void scaricaAQ() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + String(meteo_lat, 4)
               + "&longitude=" + String(meteo_lon, 4) + "&current=european_aqi";
  http.begin(client, url);
  int code = http.GET();
  Serial.print("AQ HTTP ");
  Serial.println(code);
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();   // chunked: getString() decodifica (come meteo)
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonVariant v = doc["current"]["european_aqi"];
      if (!v.isNull()) { aqi = v.as<int>(); aqi_ok = true; }
      else aqi_ok = false;
    }
  }
  http.end();
}

// Percent-encoding minimale per i nomi citta' (gestisce spazi e accenti).
String urlEncode(const String& s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) out += c;
    else { sprintf(buf, "%%%02X", (uint8_t)c); out += buf; }
  }
  return out;
}

// Cerca una citta' col geocoding di Open-Meteo, aggiorna+salva le coordinate e
// rinfresca subito il meteo. Ritorna true se la citta' e' stata trovata.
bool cercaCitta(const String& nome) {
  if (WiFi.status() != WL_CONNECTED || nome.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(nome)
               + "&count=1&language=it&format=json";
  http.begin(client, url);
  int code = http.GET();
  Serial.print("Geocoding HTTP ");
  Serial.println(code);
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonArray res = doc["results"];
      if (!res.isNull() && res.size() > 0) {
        meteo_lat = res[0]["latitude"] | meteo_lat;
        meteo_lon = res[0]["longitude"] | meteo_lon;
        const char* nm = res[0]["name"] | nome.c_str();
        strncpy(meteo_nome, nm, sizeof(meteo_nome)); meteo_nome[sizeof(meteo_nome) - 1] = 0;
        salvaCoord();
        scaricaMeteo();
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

// CSS condiviso da tutte le pagine web. Tema chiaro/scuro automatico via
// prefers-color-scheme (le variabili --* cambiano in base al SO/browser).
// In PROGMEM per non occupare RAM.
static const char PAGE_CSS[] PROGMEM =
  ":root{--bg:#fafafa;--fg:#222;--card:#ececef;--accent:#0a84ff;--muted:#666;--bd:#ccc}"
  "@media(prefers-color-scheme:dark){:root{--bg:#1b1b1f;--fg:#e8e8ea;--card:#2a2a30;--muted:#a0a0a8;--bd:#3a3a42}}"
  "*{box-sizing:border-box}"
  "body{font-family:system-ui,sans-serif;max-width:440px;margin:20px auto;padding:0 14px;background:var(--bg);color:var(--fg)}"
  "h2{margin:.2em 0 .6em}"
  "a.card{display:block;padding:14px;margin:9px 0;background:var(--card);border-radius:10px;text-decoration:none;color:var(--fg);font-size:1.1em}"
  "a.card:active{background:var(--accent);color:#fff}"
  "a.back{display:inline-block;margin:0 0 6px;color:var(--accent);text-decoration:none}"
  "label{display:block;margin:10px 0 4px}"
  "label.in{display:inline-block;margin:6px 12px 6px 0}"
  "input,select{font-size:1em;padding:7px;border:1px solid var(--bd);border-radius:7px;background:var(--bg);color:var(--fg)}"
  "input[type=number]{width:5em}input[type=checkbox]{width:auto;vertical-align:middle}"
  "button{margin-top:16px;padding:9px 18px;font-size:1em;background:var(--accent);color:#fff;border:0;border-radius:8px}"
  "p.s{color:var(--muted);font-size:.9em;margin:18px 0 4px}"
  "pre{background:var(--card);padding:12px;border-radius:8px;overflow:auto;font-size:.95em;line-height:1.5}";

// Restituisce l'intestazione HTML (doctype/head/style + apertura body, link Home
// e titolo h2). Con home=true salta il link "Home" e usa il titolo come testata.
String pageHead(const char* titolo, bool home = false) {
  String h = F("<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
  h += titolo;
  h += F("</title><style>");
  h += FPSTR(PAGE_CSS);
  h += F("</style></head><body>");
  if (!home) h += F("<a class=back href=/>&#8592; Home</a>");
  h += F("<h2>");
  h += titolo;
  h += F("</h2>");
  return h;
}

String pageFoot() {
  return F("</body></html>");
}

void setup() {
  // Pulizia LED il prima possibile: la ROM di boot emette byte su GPIO2/D4
  // (= U1TXD, la linea dati WS2812) prima che parta il firmware, accendendo
  // i primi pixel della riga 0. Spegnerli subito riduce il lampo bianco
  // visibile sulla schermata "congelata" dopo un OTA.
  strip.Begin();
  strip.ClearTo(RgbColor(0));
  strip.Show();

  Serial.begin(115200);
#ifdef BUZZER_PIN
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // buzzer spento subito (evita "tic" al boot)
#endif
  EEPROM.begin(512);
  caricaSveglia();
  caricaLum();
  caricaWifi();
  caricaCoord();

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
  // Inizializza l'APDS-9960 (sensore di luce). Lascia decantare ~200ms dopo il
  // power-up del chip, poi prova fino a 20 volte (la SparkFun lib e' a volte
  // intermittente al boot).
  delay(200);
  for (byte i = 0; i < 20 && !apds_init_ok; i++) {
    apds_init_ok = apds.init();
    if (!apds_init_ok) delay(100);
  }
  if (apds_init_ok) {
    Serial.println(F("APDS-9960 init ok"));
    apds_light_ok = apds.enableLightSensor(false);
    if (apds_light_ok) apds.setAmbientLightGain(AGAIN_64X);
  } else {
    Serial.println(F("APDS-9960 init FAILED (provo fallback raw)"));
  }
  // Rete di sicurezza: la lib SparkFun rifiuta talvolta init() e/o
  // enableLightSensor() anche quando il chip e' raggiungibile e funzionante.
  // Il fallback scrive direttamente CONTROL+ENABLE e verifica con rilettura,
  // cosi' readAmbientLight() funziona comunque (vedi apdsForceLightOn).
  if (!apds_light_ok) apds_light_ok = apdsForceLightOn();
  Serial.println(apds_light_ok ? F("Light sensor running") : F("Light sensor init FAILED"));

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

  // LED gia' inizializzati in cima al setup (strip.Begin()); qui regoliamo
  // solo la luminosita' col sensore prima di disegnare lo splash.
  regolaLuminosita();
  // Splash-screen di benvenuto: resta acceso per tutto il resto del setup
  // (connessione WiFi, NTP, meteo) e viene tolto solo dall'animazione finale,
  // cosi' non c'e' piu' lo schermo nero in attesa dell'ora.
  disegnaSplash();

  httpUpdater.setup(&httpServer, update_path);
  // Schermata fissa "UPDATING" mostrata appena parte il flash OTA, prima che
  // il device si "congeli" durante la scrittura della flash.
  Update.onStart([]() { mostraUpdating(); });
  // Barra di avanzamento azzurra sulla riga 7 (ultima). Ridisegnata solo
  // quando cambia il numero di pixel pieni, per non rallentare l'OTA.
  Update.onProgress([](size_t prog, size_t total) {
    if (total == 0) return;
    static int lastN = -1;
    int n = (int)((uint64_t)prog * 32 / total);  // pixel pieni 0..32
    if (n == lastN) return;
    lastN = n;
    for (int c = 0; c < 32; c++)
      strip.SetPixelColor(ledPos(7, c), c < n ? applicaLum(HtmlColor(0x0080ff)) : RgbColor(0));
    strip.Show();
  });
  httpServer.on("/version", []() {
    httpServer.send(200, "text/plain", "ws2812bClock build " __DATE__ " " __TIME__ "\n");
  });
  // Home: link rapidi (comodo da telefono digitando solo l'IP).
  httpServer.on("/", []() {
    String h = pageHead("Wificlock", true);
    h += F("<a class=card href=/sveglia>&#9200; Sveglia</a>"
           "<a class=card href=/msg>&#128172; Messaggio</a>"
           "<a class=card href=/coord>&#127757; Localita meteo</a>"
           "<a class=card href=/status>&#128202; Stato</a>"
           "<a class=card href=/lum>&#128161; Luminosita</a>"
           "<a class=card href=/wifi>&#128246; WiFi setup</a>");
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // Pagina luminosita': form per min/max e soglia ambient, persistiti in EEPROM.
  // Query param ?v=NN forza la luminosita' come override temporaneo (non salvato).
  // ?reinit riprova l'init dell'APDS senza dover riavviare la scheda.
  httpServer.on("/lum", []() {
    if (httpServer.hasArg("v")) {
      luminosita = constrain(httpServer.arg("v").toInt(), 0, 255);
    }
    if (httpServer.hasArg("reinit")) {
      // ATTENZIONE: apds.init() e' intermittente E termina con setMode(ALL,OFF),
      // cioe' SPEGNE il sensore (ENABLE=0x00). Quindi va SEMPRE ri-abilitato dopo,
      // altrimenti CDATA resta 0. apds_init_ok qui e' solo informativo.
      apds_init_ok = apds.init();
      // Riparti da false e ri-abilita SEMPRE: se ci affidassimo al valore
      // precedente (gia' true dal boot) salteremmo la riaccensione lasciando
      // il sensore spento dopo l'init().
      apds_light_ok = false;
      if (apds_init_ok && apds.enableLightSensor(false)) {
        apds.setAmbientLightGain(AGAIN_64X);
        apds_light_ok = true;
      }
      // Fallback raw affidabile: scrive CONTROL (gain) ed ENABLE=0x03 direttamente
      // e verifica con una lettura reale. E' questo il vero motore della riaccensione.
      if (!apds_light_ok) apds_light_ok = apdsForceLightOn();
      // Dopo l'enable i dati di colore (CDATA) sono validi solo dopo >103ms di
      // integrazione: aspetta, altrimenti la prima lettura della pagina e' 0.
      delay(120);
    }
    if (httpServer.hasArg("save")) {
      uint8_t nmin = constrain(httpServer.arg("lmin").toInt(), 0, 255);
      uint8_t nmax = constrain(httpServer.arg("lmax").toInt(), 0, 255);
      if (nmin > nmax) { uint8_t t = nmin; nmin = nmax; nmax = t; }
      lum_min = nmin; lum_max = nmax;
      amb_max = constrain(httpServer.arg("amb").toInt(), 1, 37000);
      salvaLum();
      regolaLuminosita();  // applica subito i nuovi parametri
    }
    // Lettura "live" del sensore ad ogni GET, cosi' la pagina e' sempre fresca.
    bool read_ok = apds.readAmbientLight(ambient_light);
    String h = pageHead("Luminosita");
    h += "<form method=get action=/lum>";
    h += "<label>Min (al buio, 0-255): <input type=number name=lmin min=0 max=255 value=" + String(lum_min) + "></label>";
    h += "<label>Max (a piena luce, 0-255): <input type=number name=lmax min=0 max=255 value=" + String(lum_max) + "></label>";
    h += "<label>Soglia ambient per il max: <input type=number name=amb min=1 max=37000 value=" + String(amb_max) + "></label>";
    h += "<input type=hidden name=save value=1><button type=submit>Salva</button></form>";
    h += "<p class=s>Stato attuale (la pagina rilegge il sensore ad ogni apertura)</p>";
    // L'indicatore di salute e' la PROVA REALE (la lettura I2C e' riuscita?), non
    // apds_init_ok: init() della lib SparkFun e' intermittente e fuorviante.
    h += "<p>Sensore: " + String(read_ok ? "<b style=color:#27c93f>OK</b>" : "<b style=color:#ff5f57>NON LEGGE</b>") + "<br>";
    h += "luminosita: " + String(luminosita) + "/255<br>";
    h += "ambient: " + String(ambient_light) + "</p>";
    h += "<p class=s>diagnostica: init=" + String(apds_init_ok ? "ok" : "fail") + " (intermittente, innocuo) &middot; light=" + String(apds_light_ok ? "ok" : "fail") + "</p>";
    h += "<p><a class=back href=/lum?reinit=1>&#8635; Ri-inizializza sensore</a></p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // /buzz?f=NN imposta la frequenza del tono (Hz) e suona un test.
  // /buzz?t=MS regola la durata del test (default 500 ms). Senza argomenti suona
  // solo un beep alla frequenza corrente. Frequenza in RAM: dopo reboot torna al default.
  httpServer.on("/buzz", []() {
    if (httpServer.hasArg("f")) {
      buzz_freq = constrain(httpServer.arg("f").toInt(), 50, 20000);
    }
    uint16_t dur = httpServer.hasArg("t") ? constrain(httpServer.arg("t").toInt(), 50, 5000) : 500;
#ifdef BUZZER_PIN
    tone(BUZZER_PIN, buzz_freq, dur);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "buzz freq: %u Hz, test %u ms\n", buzz_freq, dur);
    httpServer.send(200, "text/plain", buf);
  });
  // /i2c?addr=0x39 -> probe raw del bus I2C senza coinvolgere la libreria SparkFun.
  // /i2c?addr=0x39&reg=0x92 -> legge 1 byte dal registro indicato.
  // /i2c?addr=0x39&reg=0x80&val=0x03 -> scrive val nel registro, poi rilegge per
  // verifica. Accetta esadecimale (0x..) o decimale. Default 0x39 (APDS-9960).
  httpServer.on("/i2c", []() {
    auto parseNum = [](const String& s) -> uint8_t {
      String t = s; t.trim();
      return (t.startsWith("0x") || t.startsWith("0X"))
               ? (uint8_t)strtol(t.c_str(), NULL, 16)
               : (uint8_t)t.toInt();
    };
    uint8_t addr = httpServer.hasArg("addr") ? parseNum(httpServer.arg("addr")) : 0x39;
    char buf[128];
    if (httpServer.hasArg("reg")) {
      uint8_t reg = parseNum(httpServer.arg("reg"));
      uint8_t err_w = 0;
      if (httpServer.hasArg("val")) {
        uint8_t val = parseNum(httpServer.arg("val"));
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.write(val);
        err_w = Wire.endTransmission();
      }
      Wire.beginTransmission(addr);
      Wire.write(reg);
      uint8_t err_r = Wire.endTransmission();
      uint8_t got = (err_r == 0) ? Wire.requestFrom((int)addr, 1) : 0;
      if (got == 1) {
        uint8_t v = Wire.read();
        snprintf(buf, sizeof(buf),
                 "addr 0x%02X reg 0x%02X = 0x%02X (%u)%s\n",
                 addr, reg, v, v,
                 httpServer.hasArg("val") ? (err_w == 0 ? " [write ok]" : " [write FAIL]") : "");
      } else {
        snprintf(buf, sizeof(buf),
                 "addr 0x%02X reg 0x%02X: read FAIL (endTx=%u, got=%u)\n",
                 addr, reg, err_r, got);
      }
    } else {
      Wire.beginTransmission(addr);
      uint8_t err = Wire.endTransmission();
      // codici: 0=ok, 1=data too long, 2=NACK su addr, 3=NACK su data, 4=other, 5=timeout
      snprintf(buf, sizeof(buf), "addr 0x%02X: %s (endTransmission=%u)\n",
               addr, err == 0 ? "ACK" : "NO ACK", err);
    }
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
    String h = pageHead("Sveglia");
    h += "<form method=get action=/sveglia>";
    h += "<label class=in><input type=checkbox name=on" + String(sv_attiva ? " checked" : "") + "> <b>Attiva</b></label><br><br>";
    h += "Ora: <input type=number name=ore min=0 max=23 value=" + String(sv_ore) + ">";
    h += " : <input type=number name=min min=0 max=59 value=" + String(sv_min) + "><br>";
    const char* gg[7] = { "Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom" };
    for (int i = 0; i < 7; i++) {
      h += "<label class=in><input type=checkbox name=g" + String(i);
      if (sv_giorni & (1 << i)) h += " checked";
      h += "> " + String(gg[i]) + "</label>";
    }
    h += "<input type=hidden name=save value=1><br><button type=submit>Salva</button></form>";
    h += "<p>Stato: " + String((sv_attiva && sv_giorni) ? "ATTIVA" : (!sv_attiva ? "spenta (off)" : "spenta (nessun giorno)")) + "</p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // Localita' meteo: cerca una citta' col geocoding Open-Meteo e salva lat/lon.
  httpServer.on("/coord", []() {
    String esito;
    if (httpServer.hasArg("citta")) {
      String q = httpServer.arg("citta"); q.trim();
      esito = cercaCitta(q) ? ("Trovata: " + String(meteo_nome)) : "Citta' non trovata o errore di rete.";
    }
    String h = pageHead("Localita meteo");
    h += "<form method=get action=/coord>";
    h += "Citta: <input name=citta style=width:60% value='" + String(meteo_nome) + "'> ";
    h += "<button type=submit>Cerca e salva</button></form>";
    if (esito.length()) h += "<p>" + esito + "</p>";
    h += "<p class=s>Coordinate attuali: " + String(meteo_lat, 4) + ", " + String(meteo_lon, 4) + "</p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // Messaggio scorrevole one-shot: il testo scorre subito N volte sul display.
  httpServer.on("/msg", []() {
    if (httpServer.hasArg("testo")) {
      msgText = httpServer.arg("testo");
      if (msgText.length() > 64) msgText = msgText.substring(0, 64);
      int rip = httpServer.hasArg("rip") ? httpServer.arg("rip").toInt() : MSG_REPEAT_DEF;
      msgRepeatLeft = constrain(rip, 1, 20);
    }
    String h = pageHead("Messaggio");
    h += "<form method=get action=/msg>";
    h += "Testo: <input name=testo maxlength=64 style=width:60% value='" + msgText + "'><br>";
    h += "Ripetizioni: <input type=number name=rip min=1 max=20 value=" + String(MSG_REPEAT_DEF) + "><br><br>";
    h += "<button type=submit>Mostra sul display</button></form>";
    h += "<p class=s>Il messaggio scorre subito e poi torna all'orologio.</p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // Forza un refresh meteo immediato e mostra l'esito (comodo per debug e per
  // non dover aspettare il refresh automatico o la pagina meteo sui LED).
  httpServer.on("/meteo", []() {
    scaricaMeteo();
    char buf[400];
    snprintf(buf, sizeof(buf),
             "loc:  %s (%.4f, %.4f)\n"
             "meteo_ok: %s (http %d %s)\n"
             "aria: AQI %d (%s)\n"
             "ora:  %.1f C %d%% %s%s\n"
             "+3h:  %.1f C %d%% %s\n"
             "+6h:  %.1f C %d%% %s\n",
             meteo_nome, meteo_lat, meteo_lon,
             meteo_ok ? "si" : "no", meteo_http_code, meteo_last_err,
             aqi_ok ? aqi : -1, categoriaAQI(aqi_ok ? aqi : -1),
             meteo_slot[0].temp, meteo_slot[0].umidita, meteoDesc(meteo_slot[0].wmo),
             meteo_slot[0].giorno ? "" : " (notte)",
             meteo_slot[1].temp, meteo_slot[1].umidita, meteoDesc(meteo_slot[1].wmo),
             meteo_slot[2].temp, meteo_slot[2].umidita, meteoDesc(meteo_slot[2].wmo));
    httpServer.send(200, "text/plain", buf);
  });
  httpServer.on("/status", []() {
    time_t n = now();
    char buf[420];
    snprintf(buf, sizeof(buf),
             "ora: %02d/%02d/%04d %02d:%02d:%02d\n"
             "loc: %s (%.4f, %.4f)\n"
             "meteo_ok: %s\n"
             "aria: AQI %d (%s)\n"
             "ora:  %.1f C %d%% %s\n"
             "+3h:  %.1f C %d%% %s\n"
             "+6h:  %.1f C %d%% %s\n"
             "wifi rssi: %ld dBm\n"
             "luminosita: %d/255\n"
             "ambient: %d\n",
             day(n), month(n), year(n), hour(n), minute(n), second(n),
             meteo_nome, meteo_lat, meteo_lon,
             meteo_ok ? "si" : "no",
             aqi_ok ? aqi : -1, categoriaAQI(aqi_ok ? aqi : -1),
             meteo_slot[0].temp, meteo_slot[0].umidita, meteoDesc(meteo_slot[0].wmo),
             meteo_slot[1].temp, meteo_slot[1].umidita, meteoDesc(meteo_slot[1].wmo),
             meteo_slot[2].temp, meteo_slot[2].umidita, meteoDesc(meteo_slot[2].wmo),
             (long)WiFi.RSSI(), luminosita, ambient_light);
    String h = pageHead("Stato");
    h += "<pre>";
    h += buf;
    h += "</pre><p><a class=back href=/status>&#8635; Aggiorna</a></p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  // Portale WiFi: form per inserire SSID/password della nuova rete.
  httpServer.on("/wifi", []() {
    String h = pageHead("Configura WiFi");
    h += "<form method=get action=/wifisave>";
    h += "<label>Rete (SSID):<br><input name=ssid list=reti style=width:100% value='" + String(wifi_ssid) + "'></label><datalist id=reti>";
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) h += "<option value='" + WiFi.SSID(i) + "'>";
    h += "</datalist><label>Password:<br><input name=pass type=password style=width:100%></label><br>";
    h += "<button type=submit>Salva e riavvia</button></form>";
    h += "<p class=s>Reti trovate: " + String(n) + "</p>";
    h += pageFoot();
    httpServer.send(200, "text/html", h);
  });
  httpServer.on("/wifisave", []() {
    salvaWifi(httpServer.arg("ssid").c_str(), httpServer.arg("pass").c_str());
    String h = pageHead("Salvato", true);
    h += F("<p>Riavvio in corso&hellip;</p><p class=s>Ricollegati alla tua rete WiFi.</p>");
    h += pageFoot();
    httpServer.send(200, "text/html", h);
    delay(1500);
    ESP.restart();
  });
  // Captive portal: in modalita' portale ogni URL sconosciuto rimanda al form.
  httpServer.onNotFound([]() {
    if (modo_portale) {
      httpServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/wifi", true);
      httpServer.send(302, "text/plain", "");
    } else {
      httpServer.send(404, "text/plain", "Not found");
    }
  });
  httpServer.begin();

  // Connessione WiFi: se fallisce entro il timeout, apri il portale di config.
  if (connettiWifi(20000)) {
    Serial.printf("WiFi connesso, IP %s\n", WiFi.localIP().toString().c_str());
    configTime(TZ_ITALIA, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    aggiornaRTCdaNTP(true);
    scaricaMeteo();
    MDNS.begin(host);
    MDNS.addService("http", "tcp", 80);
    // Ora e' tutto pronto: chiudi lo splash con il fade-blink e passa all'orario.
    animazioneSplash();
  } else {
    Serial.println("WiFi non connesso -> portale di configurazione");
    strip.ClearTo(RgbColor(0)); strip.Show();  // togli lo splash prima del portale
    avviaPortale();
  }
  Serial.println("per modificare data e ora digita 'dt=anno,mese,giorno,ora,minuti,secondi'");
  delay(500);
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
// Pagina speciale FUORI dal range navigabile SU/GIU: si attiva solo dal flag
// one-shot del messaggio (msgRepeatLeft>0) e al termine torna all'orario.
#define PAGINA_MSG 7
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
        if (stato == 0) richiesta_portale = true;  // apri il portale di config WiFi
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
  // Modalita' portale WiFi: solo DNS captive + web server + istruzioni sui LED
  if (modo_portale) {
    if (millis() - portale_t0 > PORTALE_TIMEOUT) ESP.restart();  // safety: ritenta
    dnsServer.processNextRequest();
    httpServer.handleClient();
    mostraPortale();
    return;
  }
  // Apertura portale su richiesta (SU lungo)
  if (richiesta_portale) {
    richiesta_portale = false;
    avviaPortale();
    return;
  }
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
    // Messaggio one-shot: ha priorita', dirotta su PAGINA_MSG finche' ha ripetizioni.
    if (msgRepeatLeft > 0 && pagina != PAGINA_MSG) nuova_pagina = PAGINA_MSG;
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
        inizioPagina = false;
        // WiFi sempre attivo: se i dati sono scaduti, aggiorna prima di scorrere
        if (!meteo_ok || millis() - meteo_ultimoFetch >= METEO_REFRESH) scaricaMeteo();
        preparaScenaMeteo();
        cx = 31;  // la scena entra da destra
      }
      if (millis() - t1 > 75) {
        t1 = millis();
        strip.ClearTo(RgbColor(0));
        disegnaScenaMeteo(cx);
        strip.Show();
        cx--;
        // scena uscita tutta a sinistra: in auto avanza all'orario, in manuale
        // riparte da capo (scroll in loop sulla pagina), come la pagina data.
        if (cx < 0 - meteo_sceneW) {
          if (AUTO_NEXT_PAGE) nuova_pagina = PAGINA_ORARIO;
          else inizioPagina = true;
        }
      }
    }
    if (pagina == PAGINA_MSG) {
      if (inizioPagina) {
        cx = 31;  // il messaggio entra da destra
        inizioPagina = false;
      }
      if (millis() - t1 > 75) {
        t1 = millis();
        strip.ClearTo(RgbColor(0));
        scrivi(msgText, FONT_5x3, 2, cx, colore);
        strip.Show();
        cx--;
        if (cx < 0 - larghezza(msgText, FONT_5x3)) {  // passata completata
          msgRepeatLeft--;
          if (msgRepeatLeft > 0) inizioPagina = true;        // ripeti
          else nuova_pagina = PAGINA_ORARIO;                 // finito: torna all'orario
        }
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
// Schermata fissa (non scorrevole) mostrata all'inizio dell'OTA. Usa
// "UPDATING" se ci sta nei 32 px, altrimenti ripiega su "UPDATE". Centrata.
void mostraUpdating() {
  String testo = "UPDATING";
  if (larghezza(testo, FONT_5x3) > 32) testo = "UPDATE";
  int w = larghezza(testo, FONT_5x3) - 1;   // -1: l'ultimo char include lo spazio finale
  int startColonna = (32 - w) / 2;
  if (startColonna < 0) startColonna = 0;
  strip.ClearTo(RgbColor(0));
  scrivi(testo, FONT_5x3, 2, startColonna, 0xff8000);  // arancione
  strip.Show();
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
  return 11;  // carattere non gestito (accenti, simboli) -> spazio, evita indici negativi
}
int ledPos(int riga, int colonna) {
  if (colonna >= 0 && colonna < 8) return (riga * 8) + colonna;
  if (colonna >= 8 && colonna <= 15) return (riga * 8) + 64 + colonna - 8;
  if (colonna >= 16 && colonna <= 23) return (riga * 8) + 128 + colonna - 16;
  if (colonna >= 24 && colonna <= 31) return (riga * 8) + 192 + colonna - 24;
  return -1;
}
const char* meseStr(int m) {
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
const char* ggSettStr(byte gs) {
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

// Fallback per attivare l'ALS quando enableLightSensor() della SparkFun lib fallisce
// (verifica interna del registro ENABLE va in errore anche se il chip e' sano):
// scriviamo direttamente CONTROL = 0x0B (AGAIN_64X | PGAIN_4X) e ENABLE = 0x03 (PON|AEN),
// poi proviamo readAmbientLight() come prova di funzionamento (e' il test che
// davvero ci interessa: il chip e' OK se restituisce dati validi).
// Verificato 2026-05-29.
bool apdsForceLightOn() {
  delay(10);  // lascia decantare eventuali transitori del bus dopo enableLightSensor
  Wire.beginTransmission(0x39);
  Wire.write(0x8F); Wire.write(0x0B);
  Wire.endTransmission();
  delay(2);
  Wire.beginTransmission(0x39);
  Wire.write(0x80); Wire.write(0x03);
  Wire.endTransmission();
  delay(150);  // un'integrazione ATIME piena prima di leggere
  uint16_t v;
  return apds.readAmbientLight(v);
}

// Regolazione auto (gain ALS 64x: buio ~0, luce stanza ~18, sole centinaia/migliaia).
// Min/max della luminosita' e soglia ambient sono in EEPROM (vedi caricaLum/salvaLum).
#define AMBIENT_BUIO 0     // buio -> lum_min

void regolaLuminosita() {
  if (apds.readAmbientLight(ambient_light)) {
    luminosita = constrain(map(ambient_light, AMBIENT_BUIO, amb_max, lum_min, lum_max), lum_min, lum_max);
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
  } else if (hour(n) != sv_ore || minute(n) != sv_min) {
    sv_minuto_scattato = -1;  // usciti dal minuto-target: ri-arma per le prossime volte
  }
}

// Modalita' portale WiFi: scorre le istruzioni per connettersi e configurare.
void mostraPortale() {
  static unsigned long t = 0;
  static int pcx = 31;
  const char* msg = "WIFI SETUP: COLLEGATI ALLA RETE WIFICLOCKSETUP E APRI 192.168.4.1";
  if (millis() - t < 75) return;
  t = millis();
  strip.ClearTo(RgbColor(0));
  scrivi(msg, FONT_5x3, 2, pcx, 0x0080ff);  // azzurro per distinguere la modalita' setup
  strip.Show();
  pcx--;
  if (pcx < 0 - larghezza(msg, FONT_5x3)) pcx = 31;
}

// Pagina config: scorre l'indirizzo web per configurare la sveglia da browser.
void mostraConfig() {
  static unsigned long t = 0;
  static int ccx = 31;
  if (millis() - t < 75) return;
  t = millis();
  // mostra l'IP reale: Android non risolve gli indirizzi .local nel browser
  String msg = "SVEGLIA: " + WiFi.localIP().toString() + " - SETUP WIFI: TIENI PREMUTO SU";
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
// preme un tasto. Buzzer: 4 beep corti in 1s + 1s di pausa, ciclico.
void mostraAllarme() {
  static unsigned long t_scroll = 0, t_blink = 0;
  static int acx = 31;
  static bool vis = true;
  // Buzzer: periodo 1s. Nei primi 500 ms quattro beep da ~63 ms ON / ~62 ms OFF;
  // nei successivi 500 ms silenzio. Aggiorna il pin solo sui fronti per non
  // riavviare tone() ad ogni iterazione.
  unsigned long phase = millis() % 1000;
  bool buzz_on = (phase < 500) && ((phase % 125) < 63);
  static bool buzz_prev = false;
  if (buzz_on != buzz_prev) { buzzer(buzz_on); buzz_prev = buzz_on; }
  // Visivo: scroll + lampeggio "SVEGLIA" indipendenti dal buzzer.
  if (millis() - t_blink > 350) { vis = !vis; t_blink = millis(); }
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
