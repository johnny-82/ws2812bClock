# ws2812bClock

Orologio a matrice LED 32×8 (256× WS2812B) basato su ESP8266 (NodeMCU v2).
Mostra ora (sincronizzata via NTP), data, meteo (Open-Meteo) e qualità
dell'aria, con luminosità automatica tramite sensore APDS-9960, sveglia
configurabile e interfaccia web. L'output LED usa NeoPixelBus (UART1 su D4).

Il dispositivo è raggiungibile in rete come `wificlock.local` (mDNS) e
aggiornabile via OTA, quindi senza cavo USB.

## Build & deploy

Lo script `deploy.sh` compila lo sketch con `arduino-cli` e lo carica sul
dispositivo. È **generico e riutilizzabile fra progetti**: la configurazione
(scheda, host OTA, porta USB) vive in un file `.deploy.conf` nella cartella
del progetto — non è hardcoded nello script. Puoi quindi tenere `deploy.sh`
in `~/bin` o nel PATH e usarlo da qualsiasi sketch.

### Prima configurazione

Dalla cartella del progetto, crea il file di configurazione:

```bash
./deploy.sh init
```

Verranno chiesti FQBN (scheda), host OTA e porta USB, con i valori attuali
proposti come default. Il risultato è un `.deploy.conf` (ignorato da git):

```bash
FQBN="esp8266:esp8266:nodemcuv2"
HOST="wificlock.local"
PORT="/dev/ttyUSB0"
```

### Uso quotidiano

| Comando | Cosa fa |
|---|---|
| `./deploy.sh` | compila e carica via **OTA** |
| `./deploy.sh -b` | solo **build** (niente upload) |
| `./deploy.sh -u` | compila e carica via **USB** (usa `PORT`) |
| `./deploy.sh -t <IP>` | override dell'host OTA per questa esecuzione |
| `./deploy.sh init` | crea/aggiorna `.deploy.conf` |
| `./deploy.sh -h` | mostra l'aiuto |

Note:

- Per gli host `*.local` lo script risolve l'IP via mDNS (`getent hosts`).
  L'IP del dispositivo **non è fisso** (DHCP): se mDNS non risponde, usa
  `./deploy.sh -t <IP>`.
- L'OTA usa il web updater (`POST /firmware`, campo `firmware`, nessuna auth).
- Dopo l'OTA il device riavvia; lo script attende e verifica l'esito
  interrogando `http://<host>/version`.

## Segreti

Le credenziali WiFi di default stanno in `secrets.h` (ignorato da git); usa
`secrets.h.example` come template. In assenza, le credenziali si configurano
runtime dal portale `WificlockSetup` o dalla pagina web `/wifi`.
