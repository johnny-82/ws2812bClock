#!/usr/bin/env bash
#
# deploy.sh — compila uno sketch Arduino e lo carica via OTA o USB.
#
# Script GENERICO e riutilizzabile fra progetti: la configurazione (scheda,
# host OTA, porta USB) sta in un file ".deploy.conf" nella cartella del
# progetto, NON dentro lo script. Tienilo pure in ~/bin o nel PATH.
#
# Uso (da dentro la cartella del progetto):
#   ./deploy.sh              compila e carica via OTA
#   ./deploy.sh init         crea/aggiorna .deploy.conf (interattivo)
#   ./deploy.sh -b           solo build (niente upload)
#   ./deploy.sh -u           compila e carica via USB (usa PORT)
#   ./deploy.sh -t HOST      override dell'host OTA per questa esecuzione
#   ./deploy.sh -h           mostra questo aiuto
#
# Note:
#   - L'OTA usa il web updater (campo "firmware", nessuna auth).
#   - Per host *.local lo script risolve l'IP via mDNS (getent hosts).
#   - Dopo l'OTA il device riavvia: /version torna raggiungibile dopo ~15-18s.

set -euo pipefail

# --- dove cercare la configurazione ----------------------------------------
PROJECT_DIR="$PWD"
CONF="$PROJECT_DIR/.deploy.conf"

# Valori di default (sovrascritti dal .conf se presente)
FQBN=""
HOST=""
PORT=""

# --- colori (solo se su terminale) -----------------------------------------
if [[ -t 1 ]]; then
  B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'
else
  B=""; G=""; Y=""; R=""; N=""
fi
info() { echo "${B}${G}==>${N}${B} $*${N}"; }
warn() { echo "${Y}!! $*${N}" >&2; }
die()  { echo "${R}xx $*${N}" >&2; exit 1; }

usage() { sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0; }

# --- comando init: crea/aggiorna .deploy.conf in modo interattivo ----------
do_init() {
  # Carica eventuali valori esistenti per usarli come default
  [[ -f "$CONF" ]] && source "$CONF"
  local def_fqbn="${FQBN:-esp8266:esp8266:nodemcuv2}"
  local def_host="${HOST:-wificlock.local}"
  local def_port="${PORT:-/dev/ttyUSB0}"

  info "Configuro il deploy per: $PROJECT_DIR"
  read -rp "  FQBN (scheda)       [$def_fqbn]: " in_fqbn
  read -rp "  Host OTA            [$def_host]: " in_host
  read -rp "  Porta USB           [$def_port]: " in_port

  cat > "$CONF" <<EOF
# Configurazione deploy.sh per questo progetto.
# Generato da "deploy.sh init" — modificabile a mano.
FQBN="${in_fqbn:-$def_fqbn}"
HOST="${in_host:-$def_host}"
PORT="${in_port:-$def_port}"
EOF
  info "Salvato in $CONF"
  exit 0
}

# --- parsing comando/opzioni -----------------------------------------------
[[ "${1:-}" == "init" ]] && do_init

BUILD_ONLY=0
USE_USB=0
HOST_OVERRIDE=""
while getopts ":but:h" opt; do
  case "$opt" in
    b) BUILD_ONLY=1 ;;
    u) USE_USB=1 ;;
    t) HOST_OVERRIDE="$OPTARG" ;;
    h) usage ;;
    \?) die "Opzione sconosciuta: -$OPTARG" ;;
    :)  die "L'opzione -$OPTARG richiede un argomento" ;;
  esac
done

# --- carica la configurazione ----------------------------------------------
if [[ ! -f "$CONF" ]]; then
  warn "Nessuna configurazione trovata ($CONF)."
  warn "Crea la configurazione con:  $(basename "$0") init"
  exit 1
fi
source "$CONF"
[[ -n "$HOST_OVERRIDE" ]] && HOST="$HOST_OVERRIDE"
[[ -n "$FQBN" ]] || die "FQBN non impostato nel .conf — rilancia: $(basename "$0") init"

# --- percorsi derivati ------------------------------------------------------
OUTPUT_DIR="$PROJECT_DIR/build/${FQBN//:/.}"
BIN="$OUTPUT_DIR/$(basename "$PROJECT_DIR").ino.bin"

# --- 1. compilazione -------------------------------------------------------
info "Compilo lo sketch (${FQBN})"
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUTPUT_DIR" "$PROJECT_DIR"
[[ -f "$BIN" ]] || die "Binario non trovato: $BIN"
info "Build OK — $(du -h "$BIN" | cut -f1) ($BIN)"

if [[ "$BUILD_ONLY" == "1" ]]; then
  info "Modalita' solo-build: salto l'upload."
  exit 0
fi

# --- 2a. upload via USB -----------------------------------------------------
if [[ "$USE_USB" == "1" ]]; then
  [[ -n "$PORT" ]] || die "PORT non impostata nel .conf — rilancia: $(basename "$0") init"
  info "Flash via USB su $PORT"
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$OUTPUT_DIR" "$PROJECT_DIR"
  info "Flash USB completato."
  exit 0
fi

# --- 2b. upload via OTA -----------------------------------------------------
[[ -n "$HOST" ]] || die "HOST non impostato nel .conf — rilancia: $(basename "$0") init"
TARGET="$HOST"
if [[ "$HOST" == *.local ]]; then
  if IP=$(getent hosts "$HOST" 2>/dev/null | awk '{print $1; exit}') && [[ -n "${IP:-}" ]]; then
    info "$HOST risolto in $IP"
    TARGET="$IP"
  else
    warn "Non riesco a risolvere $HOST via mDNS; provo lo stesso con il nome."
  fi
fi

info "Carico via OTA su http://$TARGET/firmware"
if ! curl -fsS -H "Expect:" -F "firmware=@$BIN" "http://$TARGET/firmware" >/dev/null; then
  die "OTA fallito. Se mDNS non risolve, riprova con: $(basename "$0") -t <IP-del-device>"
fi
info "Upload completato — il device sta riavviando."

# --- 3. verifica /version ---------------------------------------------------
info "Attendo il reboot e verifico /version..."
for _ in $(seq 1 20); do
  sleep 2
  if VER=$(curl -fsS --max-time 3 "http://$TARGET/version" 2>/dev/null); then
    info "Firmware attivo: ${B}$VER${N}"
    exit 0
  fi
  printf '.'
done
echo
warn "Nessuna risposta da /version, ma l'upload era andato a buon fine."
warn "Controlla a mano: curl http://$TARGET/version"
