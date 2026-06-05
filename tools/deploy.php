<?php
// deploy.php — backend locale multi-sketch per l'editor: elenca gli sketch in
// ~/Arduino, ne configura/compila/carica uno via ./deploy.sh (generico), con
// streaming dei log al browser.
//
// SICUREZZA: esegue comandi di shell -> accessibile SOLO DA LOCALHOST. Il
// progetto scelto è vincolato a un figlio diretto di ROOT (niente path
// traversal). Forziamo HOME/PATH così arduino-cli trova core e librerie.

$remote = $_SERVER['REMOTE_ADDR'] ?? '';
if (!in_array($remote, ['127.0.0.1', '::1'], true)) {
  http_response_code(403);
  exit("403 — deploy.php è accessibile solo da localhost.\n");
}

define('ROOT',   dirname(dirname(__DIR__)));      // tools/ -> progetto -> ~/Arduino
define('DEPLOY', dirname(__DIR__) . '/deploy.sh'); // deploy.sh generico del progetto
$HOME = '/home/giovanni';
$PATH = "$HOME/bin:/usr/local/bin:/usr/bin:/bin";

// Legge FQBN/HOST/PORT da un .deploy.conf (per mostrarli nell'elenco/form).
function parseConf($file) {
  $r = [];
  foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line)
    if (preg_match('/^\s*(FQBN|HOST|PORT)\s*=\s*"?([^"]*)"?\s*$/', $line, $m)) $r[$m[1]] = $m[2];
  return $r;
}
// Sketch in ROOT (cartelle con un .ino); $onlyReady => solo quelli con .deploy.conf.
function listSketches($onlyReady) {
  $out = [];
  foreach (glob(ROOT . '/*', GLOB_ONLYDIR) as $dir) {
    if (!glob("$dir/*.ino")) continue;
    $ready = is_file("$dir/.deploy.conf");
    if ($onlyReady && !$ready) continue;
    $row = ['name' => basename($dir), 'ready' => $ready];
    if ($ready) { $c = parseConf("$dir/.deploy.conf");
      $row += ['fqbn' => $c['FQBN'] ?? '', 'host' => $c['HOST'] ?? '', 'port' => $c['PORT'] ?? '']; }
    $out[] = $row;
  }
  return $out;
}
// Valida e risolve un progetto: nome semplice, figlio diretto di ROOT, con un .ino.
function projDir($name) {
  if (!preg_match('/^[A-Za-z0-9._-]+$/', (string)$name)) return null;
  $real = realpath(ROOT . '/' . $name);
  if ($real === false || dirname($real) !== realpath(ROOT)) return null;
  return glob("$real/*.ino") ? $real : null;
}

$action  = $_GET['action'] ?? '';
$project = $_REQUEST['project'] ?? '';

// --- elenchi (JSON) ---
if ($action === 'list' || $action === 'listall') {
  header('Content-Type: application/json');
  echo json_encode(listSketches($action === 'list'));
  exit;
}

// --- schede installate (FQBN) per il form di init ---
if ($action === 'boards') {
  header('Content-Type: application/json');
  $env = 'env -u LD_LIBRARY_PATH -u LD_PRELOAD HOME=' . escapeshellarg($HOME) . ' PATH=' . escapeshellarg($PATH);
  $d = json_decode((string)shell_exec("$env arduino-cli board listall --format json 2>/dev/null"), true);
  $out = [];
  foreach (($d['boards'] ?? []) as $b)
    if (!empty($b['fqbn'])) $out[] = ['fqbn' => $b['fqbn'], 'name' => $b['name'] ?? ''];
  echo json_encode($out);
  exit;
}

// --- init: crea il .deploy.conf di uno sketch ---
if ($action === 'init') {
  header('Content-Type: text/plain; charset=utf-8');
  $dir = projDir($project);
  if (!$dir) { http_response_code(400); exit("400 — sketch non valido (deve stare in ~/Arduino e avere un .ino).\n"); }
  $fqbn = trim($_POST['fqbn'] ?? '');
  $host = trim($_POST['host'] ?? '');
  $port = trim($_POST['port'] ?? '');
  if ($fqbn === '') { http_response_code(400); exit("400 — FQBN obbligatorio.\n"); }
  $conf = "# Configurazione deploy.sh (generata dall'editor).\n"
        . 'FQBN="' . addslashes($fqbn) . "\"\n"
        . 'HOST="' . addslashes($host) . "\"\n"
        . 'PORT="' . addslashes($port) . "\"\n";
  if (@file_put_contents("$dir/.deploy.conf", $conf) === false) {
    http_response_code(500); exit("500 — impossibile scrivere $dir/.deploy.conf\n");
  }
  exit("OK — .deploy.conf creato in $dir\n");
}

// --- build / ota / usb ---
$flags = ['build' => '-b', 'ota' => '', 'usb' => '-u'];
if (!array_key_exists($action, $flags)) {
  http_response_code(400);
  exit("400 — azione non valida (list|listall|init|build|ota|usb).\n");
}
$dir = projDir($project);
if (!$dir)                          { http_response_code(400); exit("400 — sketch non valido o senza .ino.\n"); }
if (!is_file("$dir/.deploy.conf"))  { http_response_code(400); exit("400 — manca .deploy.conf: configuralo prima (init).\n"); }
if (!is_file(DEPLOY))               { http_response_code(500); exit("500 — deploy.sh non trovato.\n"); }

header('Content-Type: text/plain; charset=utf-8');
header('X-Accel-Buffering: no');
set_time_limit(0);
while (ob_get_level()) ob_end_flush();   // streaming dei log in tempo reale
ob_implicit_flush(true);

// env -u LD_LIBRARY_PATH/LD_PRELOAD: XAMPP esporta /opt/lampp/lib e il curl di
// sistema (OTA) caricherebbe la libcurl vecchia di XAMPP. deploy.sh usa $PWD,
// quindi ci spostiamo nello sketch scelto e lanciamo il deploy.sh generico.
chdir($dir);
$env = 'env -u LD_LIBRARY_PATH -u LD_PRELOAD'
     . ' HOME=' . escapeshellarg($HOME)
     . ' PATH=' . escapeshellarg($PATH);
$cmd = "$env bash " . escapeshellarg(DEPLOY) . ' ' . $flags[$action] . ' 2>&1';
echo "\$ (" . basename($dir) . ") deploy.sh {$flags[$action]}\n\n";
passthru($cmd, $rc);
echo "\n__DEPLOY_RC__=$rc\n";          // sentinella letta dall'editor per lo stato
