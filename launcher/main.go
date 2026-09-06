package main

import (
	"bufio"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/jchv/go-webview2"
)

var (
	user32                  = syscall.NewLazyDLL("user32.dll")
	procEnumDisplayMonitors = user32.NewProc("EnumDisplayMonitors")
	procGetMonitorInfoW     = user32.NewProc("GetMonitorInfoW")
)

type monitorInfoEx struct {
	cbSize    uint32
	rcMonitor struct{ left, top, right, bottom int32 }
	rcWork    struct{ left, top, right, bottom int32 }
	dwFlags   uint32
	szDevice  [32]uint16
}

type monitorScan struct {
	monitors *[]monitor
	index    int
}

var (
	monitorScanMu     sync.Mutex
	activeMonitorScan *monitorScan
)

var monitorEnumCallback = syscall.NewCallback(func(hMonitor, _, _, lParam uintptr) uintptr {
	_ = lParam
	scan := activeMonitorScan
	if scan == nil {
		return 0
	}
	scan.index++

	var info monitorInfoEx
	info.cbSize = uint32(unsafe.Sizeof(info))
	if result, _, _ := procGetMonitorInfoW.Call(hMonitor, uintptr(unsafe.Pointer(&info))); result == 0 {
		return 1
	}

	w := info.rcMonitor.right - info.rcMonitor.left
	h := info.rcMonitor.bottom - info.rcMonitor.top
	primary := ""
	if info.dwFlags&1 != 0 {
		primary = " (Primary)"
	}
	*scan.monitors = append(*scan.monitors, monitor{
		Label: fmt.Sprintf("Monitor %d%s - %dx%d", scan.index, primary, w, h),
		Value: strconv.Itoa(scan.index),
	})
	return 1
})

func detectMonitors() []monitor {
	monitorScanMu.Lock()
	defer monitorScanMu.Unlock()

	monitors := []monitor{{Label: "Default", Value: "default"}}
	scan := monitorScan{monitors: &monitors}
	activeMonitorScan = &scan
	defer func() { activeMonitorScan = nil }()
	procEnumDisplayMonitors.Call(0, 0, monitorEnumCallback, 0)
	return monitors
}

type config struct {
	Resolution  string    `json:"resolution"`
	Scaling     string    `json:"scaling"`
	FOVFactor   float64   `json:"fovFactor"`
	Fullscreen  bool      `json:"fullscreen"`
	SkipIntro   bool      `json:"skipIntro"`
	BackupSaves bool      `json:"backupSaves"`
	Monitor     string    `json:"monitor"`
	Monitors    []monitor `json:"monitors"`
	Error       string    `json:"error,omitempty"`
}

type monitor struct {
	Label string `json:"label"`
	Value string `json:"value"`
}

var (
	gameDir        string
	confPath       string
	widescreenPath string
	token          string
	wv             webview2.WebView
)

func main() {
	exePath, err := os.Executable()
	if err != nil {
		fmt.Fprintln(os.Stderr, "Failed to find launcher path:", err)
		os.Exit(1)
	}
	gameDir = filepath.Dir(exePath)
	confPath = filepath.Join(gameDir, "dgVoodoo.conf")
	widescreenPath = filepath.Join(gameDir, "SecretAgentBarbieWidescreenFix.ini")
	token, err = newToken()
	if err != nil {
		fmt.Fprintln(os.Stderr, "Failed to initialize launcher:", err)
		os.Exit(1)
	}

	// Start local HTTP server for the UI
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintln(os.Stderr, "Failed to start HTTP server:", err)
		os.Exit(1)
	}
	port := listener.Addr().(*net.TCPAddr).Port

	mux := http.NewServeMux()
	mux.HandleFunc("/", handleIndex)
	mux.HandleFunc("/api/config", handleConfig)
	mux.HandleFunc("/api/launch", handleLaunch)
	server := &http.Server{Handler: mux}
	defer server.Close()
	go func() {
		if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
			fmt.Fprintln(os.Stderr, "Launcher HTTP server failed:", err)
		}
	}()

	wv = webview2.NewWithOptions(webview2.WebViewOptions{
		Debug:     false,
		AutoFocus: true,
		WindowOptions: webview2.WindowOptions{
			Title:  "Secret Agent Barbie",
			Width:  480,
			Height: 720,
			Center: true,
		},
	})
	if wv == nil {
		fmt.Fprintln(os.Stderr, "Failed to create webview2 window")
		os.Exit(1)
	}
	defer wv.Destroy()

	wv.Navigate(fmt.Sprintf("http://127.0.0.1:%d/?token=%s", port, token))
	wv.Run()
}

func newToken() (string, error) {
	data := make([]byte, 16)
	if _, err := rand.Read(data); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", data), nil
}

func authorize(w http.ResponseWriter, r *http.Request) bool {
	if r.URL.Query().Get("token") == token {
		return true
	}
	http.Error(w, "forbidden", http.StatusForbidden)
	return false
}

func handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	if !authorize(w, r) {
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = w.Write([]byte(launcherHTML))
}

func handleConfig(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	if !authorize(w, r) {
		return
	}
	cfg, err := loadConfig(confPath)
	if err != nil {
		cfg.Error = err.Error()
	}
	if wsErr := loadWidescreenConfig(widescreenPath, &cfg); wsErr != nil {
		cfg.Error = wsErr.Error()
	}
	cfg.Monitors = detectMonitors()
	writeJSON(w, http.StatusOK, cfg)
}

func handleLaunch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if !authorize(w, r) {
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	var cfg config
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": "Invalid settings: " + err.Error()})
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": "Invalid settings payload"})
		return
	}
	if err := validateConfig(cfg); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": err.Error()})
		return
	}
	if err := saveConfig(confPath, cfg); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": "Could not save settings: " + err.Error()})
		return
	}
	if err := saveWidescreenConfig(widescreenPath, cfg); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": "Could not save widescreen settings: " + err.Error()})
		return
	}
	if err := launchGame(gameDir, cfg.BackupSaves); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": "Could not launch game: " + err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"ok": true})

	// Close the window after a brief delay
	go func() {
		time.Sleep(250 * time.Millisecond)
		wv.Dispatch(func() { wv.Destroy() })
	}()
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func launchGame(dir string, backup bool) error {
	gamePath := filepath.Join(dir, "SecretAgent.exe")
	if info, err := os.Stat(gamePath); err != nil {
		return err
	} else if info.IsDir() {
		return fmt.Errorf("%s is not a file", gamePath)
	}
	if backup {
		if err := backupSaves(dir); err != nil {
			return fmt.Errorf("save backup failed (fix the error or turn off Automatic save backups): %w", err)
		}
	}
	cmd := exec.Command(gamePath)
	cmd.Dir = dir
	return cmd.Start()
}

const launcherHTML = `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Segoe UI', Tahoma, sans-serif;
    background: linear-gradient(135deg, #FFB6C1 0%, #FF69B4 30%, #FF1493 60%, #C71585 100%);
    color: #2D0A1F;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 20px;
    user-select: none;
  }
  .header {
    text-align: center;
    margin-bottom: 16px;
  }
  .header h1 {
    font-size: 28px;
    color: white;
    text-shadow: 2px 2px 4px rgba(0,0,0,0.3), 0 0 20px rgba(255,255,255,0.3);
    letter-spacing: 1px;
  }
  .header .subtitle {
    font-size: 12px;
    color: rgba(255,255,255,0.85);
    margin-top: 4px;
    letter-spacing: 2px;
    text-transform: uppercase;
  }
  .card {
    background: rgba(255,255,255,0.92);
    border-radius: 16px;
    padding: 24px;
    width: 100%;
    max-width: 400px;
    box-shadow: 0 8px 32px rgba(0,0,0,0.15), 0 0 0 1px rgba(255,255,255,0.2);
    backdrop-filter: blur(10px);
  }
  .field { margin-bottom: 16px; }
  .field label {
    display: block;
    font-size: 12px;
    font-weight: 700;
    color: #C71585;
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 6px;
  }
  select, input[type="number"] {
    width: 100%;
    padding: 10px 12px;
    border: 2px solid #FFB6C1;
    border-radius: 8px;
    font-size: 14px;
    background: white;
    color: #2D0A1F;
    outline: none;
    cursor: pointer;
    transition: border-color 0.2s;
  }
  select:focus { border-color: #FF1493; }
  input[type="number"]:focus { border-color: #FF1493; }
  .checks {
    display: flex;
    gap: 20px;
    margin: 16px 0;
  }
  .check-item {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    font-size: 14px;
    color: #8B008B;
    font-weight: 600;
  }
  .check-item input[type="checkbox"] {
    width: 18px;
    height: 18px;
    accent-color: #FF1493;
    cursor: pointer;
  }
  .launch-btn {
    width: 100%;
    padding: 14px;
    background: linear-gradient(135deg, #FF1493, #FF69B4);
    color: white;
    border: none;
    border-radius: 12px;
    font-size: 18px;
    font-weight: 700;
    letter-spacing: 1px;
    cursor: pointer;
    transition: all 0.2s;
    box-shadow: 0 4px 15px rgba(255,20,147,0.4);
    margin-top: 8px;
  }
  .launch-btn:hover {
    transform: translateY(-2px);
    box-shadow: 0 6px 20px rgba(255,20,147,0.6);
    background: linear-gradient(135deg, #FF69B4, #FF1493);
  }
  .launch-btn:active { transform: translateY(0); }
  .launch-btn:disabled { opacity: 0.6; cursor: wait; transform: none; }
  .status {
    min-height: 18px;
    margin-top: 12px;
    color: #A00055;
    font-size: 12px;
    text-align: center;
  }
  .footer {
    margin-top: 12px;
    font-size: 10px;
    color: rgba(255,255,255,0.7);
    text-align: center;
  }
</style>
</head>
<body>
  <div class="header">
    <h1>Secret Agent Barbie</h1>
    <div class="subtitle">Win11 Compatibility Launcher</div>
  </div>
  <div class="card">
    <div class="field">
      <label>Resolution</label>
      <select id="resolution">
        <option value="unforced">Original (game default)</option>
        <option value="h:1024, v:768">1024x768</option>
        <option value="h:1280, v:960">1280x960 (4:3)</option>
        <option value="h:1920, v:1080">1920x1080 (1080p)</option>
        <option value="h:2560, v:1440">2560x1440 (1440p)</option>
        <option value="max">Max (native)</option>
      </select>
    </div>
    <div class="field">
      <label>Aspect Ratio</label>
      <select id="scaling">
        <option value="stretched_ar">Native aspect ratio (recommended)</option>
        <option value="stretched_4_3">4:3 Pillarbox</option>
        <option value="stretched">Stretch to fill</option>
        <option value="centered">Centered (no scaling)</option>
      </select>
    </div>
    <div class="field">
      <label>Gameplay FOV multiplier</label>
      <input type="number" id="fovFactor" min="0.1" max="5" step="0.05" value="1">
    </div>
    <div class="field">
      <label>Monitor</label>
      <select id="monitor"></select>
    </div>
    <div class="checks">
      <label class="check-item">
        <input type="checkbox" id="fullscreen"> Fullscreen
      </label>
    </div>
    <div class="checks">
      <label class="check-item">
        <input type="checkbox" id="skipIntro"> Skip intro videos and logos
      </label>
    </div>
    <div class="checks">
      <label class="check-item" title="Keep the five latest pre-launch snapshots in SaveBackups. Live saves are not changed.">
        <input type="checkbox" id="backupSaves" checked> Automatic save backups (keep 5)
      </label>
    </div>
    <button id="launch" class="launch-btn" onclick="launchGame()">LAUNCH GAME</button>
    <div id="status" class="status" role="status"></div>
  </div>
  <div class="footer">Operation Pink — Win11 Compatibility Project</div>
<script>
  const token = new URLSearchParams(location.search).get('token');
  const api = path => path + '?token=' + encodeURIComponent(token || '');
  const statusEl = document.getElementById('status');

  fetch(api('/api/config')).then(r => r.json()).then(cfg => {
    document.getElementById('resolution').value = cfg.resolution || 'unforced';
    document.getElementById('scaling').value = cfg.scaling || 'stretched_ar';
    document.getElementById('fovFactor').value = cfg.fovFactor || 1;
    const monSel = document.getElementById('monitor');
    monSel.innerHTML = '';
    (cfg.monitors || [{label:'Default',value:'default'}]).forEach(m => {
      const opt = document.createElement('option');
      opt.value = m.value;
      opt.textContent = m.label;
      monSel.appendChild(opt);
    });
    monSel.value = cfg.monitor || 'default';
    document.getElementById('fullscreen').checked = cfg.fullscreen || false;
    document.getElementById('skipIntro').checked = cfg.skipIntro || false;
    document.getElementById('backupSaves').checked = cfg.backupSaves !== false;
    if (cfg.error) statusEl.textContent = cfg.error;
  }).catch(err => { statusEl.textContent = 'Could not load settings: ' + err.message; });

  async function launchGame() {
    const button = document.getElementById('launch');
    button.disabled = true;
    statusEl.textContent = 'Launching...';
    const cfg = {
      resolution: document.getElementById('resolution').value,
      scaling: document.getElementById('scaling').value,
      fovFactor: Number(document.getElementById('fovFactor').value),
      monitor: document.getElementById('monitor').value,
      fullscreen: document.getElementById('fullscreen').checked,
      skipIntro: document.getElementById('skipIntro').checked,
      backupSaves: document.getElementById('backupSaves').checked,
    };
    try {
      const response = await fetch(api('/api/launch'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(cfg),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || 'Launch failed');
      statusEl.textContent = 'Game launched.';
    } catch (err) {
      statusEl.textContent = err.message;
      button.disabled = false;
    }
  }
</script>
</body>
</html>`

// ── Config loading/saving ───────────────────────────────────────────

func defaultConfig() config {
	return config{
		Resolution:  "max",
		Scaling:     "stretched_ar",
		FOVFactor:   1.0,
		Fullscreen:  true,
		BackupSaves: true,
		Monitor:     "default",
	}
}

func loadConfig(path string) (config, error) {
	cfg := defaultConfig()

	f, err := os.Open(path)
	if err != nil {
		return cfg, err
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	section := ""
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = line
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}
		key := strings.TrimSpace(parts[0])
		val := strings.TrimSpace(parts[1])

		switch key {
		case "ScalingMode":
			if section == "[General]" {
				cfg.Scaling = val
			}
		case "FullScreenMode":
			if section == "[General]" {
				cfg.Fullscreen = val == "true"
			}
		case "Resolution":
			if section == "[DirectX]" {
				cfg.Resolution = val
			}
		case "FullScreenOutput":
			if section == "[General]" {
				cfg.Monitor = val
			}
		}
	}
	if err := scanner.Err(); err != nil {
		return cfg, err
	}
	return cfg, nil
}

func validateConfig(cfg config) error {
	validResolutions := map[string]bool{
		"unforced": true, "h:1024, v:768": true, "h:1280, v:960": true,
		"h:1920, v:1080": true, "h:2560, v:1440": true, "max": true,
	}
	if !validResolutions[cfg.Resolution] {
		return fmt.Errorf("unsupported resolution %q", cfg.Resolution)
	}
	validScaling := map[string]bool{
		"stretched_4_3": true, "stretched": true, "stretched_ar": true, "centered": true,
	}
	if !validScaling[cfg.Scaling] {
		return fmt.Errorf("unsupported aspect ratio %q", cfg.Scaling)
	}
	if math.IsNaN(cfg.FOVFactor) || math.IsInf(cfg.FOVFactor, 0) ||
		cfg.FOVFactor < 0.1 || cfg.FOVFactor > 5.0 {
		return fmt.Errorf("FOV multiplier must be between 0.1 and 5.0")
	}
	if cfg.Monitor != "default" {
		monitor, err := strconv.Atoi(cfg.Monitor)
		if err != nil || monitor < 1 || monitor > 64 {
			return fmt.Errorf("unsupported monitor %q", cfg.Monitor)
		}
	}
	return nil
}

func loadWidescreenConfig(path string, cfg *config) error {
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}

	enabled := true
	section := ""
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = line
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}
		key, value := strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1])
		if section == "[Fix]" && key == "Enabled" {
			enabled = strings.EqualFold(value, "true") || value == "1"
		}
		if section == "[Settings]" && key == "FOVFactor" {
			if parsed, parseErr := strconv.ParseFloat(value, 64); parseErr == nil {
				cfg.FOVFactor = parsed
			}
		}
		if section == "[Launcher]" {
			switch key {
			case "SkipIntro":
				cfg.SkipIntro = strings.EqualFold(value, "true") || value == "1"
			case "BackupSaves":
				cfg.BackupSaves = strings.EqualFold(value, "true") || value == "1"
			}
		}
	}
	if !enabled {
		cfg.Resolution = "unforced"
	}
	return nil
}

func widescreenDimensions(resolution string) (width, height int, enabled bool) {
	switch resolution {
	case "unforced":
		return 640, 480, false
	case "h:1024, v:768":
		return 1024, 768, true
	case "h:1280, v:960":
		return 1280, 960, true
	case "h:1920, v:1080":
		return 1920, 1080, true
	case "h:2560, v:1440":
		return 2560, 1440, true
	case "max":
		return 0, 0, true
	default:
		return 640, 480, false
	}
}

func saveWidescreenConfig(path string, cfg config) error {
	width, height, enabled := widescreenDimensions(cfg.Resolution)
	contents := fmt.Sprintf("[Fix]\nEnabled=%t\n\n[Settings]\nWidth=%d\nHeight=%d\nFOVFactor=%s\n",
		enabled, width, height, strconv.FormatFloat(cfg.FOVFactor, 'f', 3, 64))
	contents += fmt.Sprintf("\n[Launcher]\nSkipIntro=%t\nBackupSaves=%t\n", cfg.SkipIntro, cfg.BackupSaves)
	return os.WriteFile(path, []byte(contents), 0644)
}

func splitLineEnding(line string) (string, string) {
	switch {
	case strings.HasSuffix(line, "\r\n"):
		return strings.TrimSuffix(line, "\r\n"), "\r\n"
	case strings.HasSuffix(line, "\n"):
		return strings.TrimSuffix(line, "\n"), "\n"
	default:
		return line, ""
	}
}

func replaceConfigValue(line, value string) string {
	body, ending := splitLineEnding(line)
	eq := strings.IndexByte(body, '=')
	if eq < 0 {
		return line
	}
	after := body[eq+1:]
	spaces := after[:len(after)-len(strings.TrimLeft(after, " \t"))]
	return body[:eq+1] + spaces + value + ending
}

func saveConfig(path string, cfg config) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	lines := strings.SplitAfter(string(data), "\n")
	section := ""
	updated := map[string]bool{}

	fsVal := "false"
	if cfg.Fullscreen {
		fsVal = "true"
	}
	for i, line := range lines {
		body, _ := splitLineEnding(line)
		trimmed := strings.TrimSpace(body)
		if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
			section = trimmed
			continue
		}

		eqIdx := strings.Index(body, "=")
		if eqIdx < 0 {
			continue
		}
		key := strings.TrimSpace(body[:eqIdx])

		switch {
		case key == "ScalingMode" && section == "[General]":
			lines[i] = replaceConfigValue(line, cfg.Scaling)
			updated["ScalingMode"] = true
		case key == "FullScreenMode" && section == "[General]":
			lines[i] = replaceConfigValue(line, fsVal)
			updated["FullScreenMode"] = true
		case key == "FullScreenOutput" && section == "[General]":
			lines[i] = replaceConfigValue(line, cfg.Monitor)
			updated["FullScreenOutput"] = true
		case key == "AppControlledScreenMode" && section == "[DirectX]":
			lines[i] = replaceConfigValue(line, "false")
			updated["AppControlledScreenMode"] = true
		case key == "Resolution" && section == "[DirectX]" && !updated["Resolution"]:
			lines[i] = replaceConfigValue(line, cfg.Resolution)
			updated["Resolution"] = true
		}
	}

	for _, key := range []string{"ScalingMode", "FullScreenMode", "FullScreenOutput", "AppControlledScreenMode", "Resolution"} {
		if !updated[key] {
			return fmt.Errorf("dgVoodoo.conf is missing %s in its expected section", key)
		}
	}
	return os.WriteFile(path, []byte(strings.Join(lines, "")), 0644)
}
