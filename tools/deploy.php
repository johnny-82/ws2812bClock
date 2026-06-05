<?php
// deploy.php — backend locale per l'editor: lancia ./deploy.sh sul PC e fa lo
// streaming dei log al browser. Pensato per girare sotto XAMPP servendo la
// cartella tools/ (es. symlink /opt/lampp/htdocs/wsclock -> .../ws2812bClock/tools).
//
// Sicurezza: esegue comandi di shell, quindi è ACCESSIBILE SOLO DA LOCALHOST.
// Apache deve girare come l'utente che possiede il toolchain (vedi README/chat):
// qui forziamo comunque HOME e PATH così arduino-cli trova core e librerie.

$remote = $_SERVER['REMOTE_ADDR'] ?? '';
if (!in_array($remote, ['127.0.0.1', '::1'], true)) {
  http_response_code(403);
  exit("403 — deploy.php è accessibile solo da localhost.\n");
}

header('Content-Type: text/plain; charset=utf-8');
header('X-Accel-Buffering: no');          // niente buffering (innocuo fuori da nginx)
set_time_limit(0);                         // il compile può superare i 30s
while (ob_get_level()) ob_end_flush();     // streaming dei log in tempo reale
ob_implicit_flush(true);

// Mappa azione -> flag di deploy.sh:
//   build = solo compilazione (-b);  ota = compila e carica via WiFi (default);
//   usb   = compila e carica via cavo (-u).
$flags = ['build' => '-b', 'ota' => '', 'usb' => '-u'];
$action = $_GET['action'] ?? 'build';
if (!array_key_exists($action, $flags)) {
  http_response_code(400);
  exit("400 — azione non valida (build|ota|usb).\n");
}

$proj = realpath(__DIR__ . '/..');         // tools/ -> radice del progetto
if ($proj === false || !is_file("$proj/deploy.sh")) {
  http_response_code(500);
  exit("500 — deploy.sh non trovato nella radice del progetto.\n");
}

// HOME/PATH espliciti: l'ambiente di Apache non li imposta, ma arduino-cli ne ha
// bisogno per trovare ~/.arduino15 (core/librerie) e il binario in ~/bin.
$home = '/home/giovanni';
$path = "$home/bin:/usr/local/bin:/usr/bin:/bin";
// `env -u LD_LIBRARY_PATH -u LD_PRELOAD`: Apache/XAMPP esporta LD_LIBRARY_PATH=
// /opt/lampp/lib, e così il curl di sistema (usato da deploy.sh per l'OTA)
// caricherebbe la libcurl vecchia di XAMPP -> "undefined symbol curl_global_trace".
// Ripulendo l'ambiente curl usa di nuovo le librerie di sistema.
$env  = 'env -u LD_LIBRARY_PATH -u LD_PRELOAD'
      . ' HOME=' . escapeshellarg($home)
      . ' PATH=' . escapeshellarg($path);

chdir($proj);
$cmd = "$env ./deploy.sh {$flags[$action]} 2>&1";
echo "\$ ./deploy.sh {$flags[$action]}\n\n";
passthru($cmd, $rc);
echo "\n__DEPLOY_RC__=$rc\n";             // sentinella letta dall'editor per lo stato
