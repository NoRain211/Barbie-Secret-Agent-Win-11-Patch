package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"

	"github.com/jchv/go-webview2"
)

var (
	user32              = syscall.NewLazyDLL("user32.dll")
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

func detectMonitors() []monitor {
	monitors := []monitor{{Label: "Default", Value: "default"}}
	idx := 0

	cb := syscall.NewCallback(func(hMonitor uintptr, hdc uintptr, lprc uintptr, lParam uintptr) uintptr {
		idx++
		var info monitorInfoEx
		info.cbSize = uint32(unsafe.Sizeof(info))
		procGetMonitorInfoW.Call(hMonitor, uintptr(unsafe.Pointer(&info)))

		w := info.rcMonitor.right - info.rcMonitor.left
		h := info.rcMonitor.bottom - info.rcMonitor.top
		primary := ""
		if info.dwFlags&1 != 0 {
			primary = " (Primary)"
		}
		monitors = append(monitors, monitor{
			Label: fmt.Sprintf("Monitor %d%s - %dx%d", idx, primary, w, h),
			Value: fmt.Sprintf("%d", idx),
		})
		return 1 // TRUE = continue
	})

	procEnumDisplayMonitors.Call(0, 0, cb, 0)
	return monitors
}

type config struct {
	Resolution string    `json:"resolution"`
	Scaling    string    `json:"scaling"`
	Fullscreen bool      `json:"fullscreen"`
	Watermark  bool      `json:"watermark"`
	Monitor    string    `json:"monitor"`
	Monitors   []monitor `json:"monitors"`
}

type monitor struct {
	Label string `json:"label"`
	Value string `json:"value"`
}

var (
	gameDir  string
	confPath string
	cfg      config
	wv       webview2.WebView
)

func main() {
	exePath, _ := os.Executable()
	gameDir = filepath.Dir(exePath)
	confPath = filepath.Join(gameDir, "dgVoodoo.conf")
	cfg = loadConfig(confPath)

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
	go http.Serve(listener, mux)

	wv = webview2.NewWithOptions(webview2.WebViewOptions{
		Debug:     false,
		AutoFocus: true,
		WindowOptions: webview2.WindowOptions{
			Title:  "Secret Agent Barbie",
			Width:  480,
			Height: 620,
			Center: true,
		},
	})
	if wv == nil {
		fmt.Fprintln(os.Stderr, "Failed to create webview2 window")
		os.Exit(1)
	}
	defer wv.Destroy()

	wv.Navigate(fmt.Sprintf("http://127.0.0.1:%d/", port))
	wv.Run()
}

func handleIndex(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(launcherHTML))
}

func handleConfig(w http.ResponseWriter, r *http.Request) {
	cfg.Monitors = detectMonitors()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(cfg)
}

func handleLaunch(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "POST only", 405)
		return
	}
	json.NewDecoder(r.Body).Decode(&cfg)
	saveConfig(confPath, cfg)

	gamePath := filepath.Join(gameDir, "SecretAgent.exe")
	cmd := exec.Command(gamePath)
	cmd.Dir = gameDir
	_ = cmd.Start()

	w.Write([]byte(`{"ok":true}`))

	// Close the window after a brief delay
	go func() {
		wv.Dispatch(func() { wv.Destroy() })
	}()
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
  select {
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
        <option value="stretched_4_3">4:3 Pillarbox (recommended)</option>
        <option value="stretched">Stretch to fill</option>
        <option value="stretched_ar">Auto aspect ratio</option>
        <option value="centered">Centered (no scaling)</option>
      </select>
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
    <button class="launch-btn" onclick="launch()">LAUNCH GAME</button>
  </div>
  <div class="footer">Operation Pink — Win11 Compatibility Project</div>
<script>
  fetch('/api/config').then(r => r.json()).then(cfg => {
    document.getElementById('resolution').value = cfg.resolution || 'unforced';
    document.getElementById('scaling').value = cfg.scaling || 'stretched_4_3';
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
  });

  function launch() {
    const cfg = {
      resolution: document.getElementById('resolution').value,
      scaling: document.getElementById('scaling').value,
      monitor: document.getElementById('monitor').value,
      fullscreen: document.getElementById('fullscreen').checked,
      watermark: false,
    };
    fetch('/api/launch', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(cfg),
    });
  }
</script>
</body>
</html>`

// ── Config loading/saving ───────────────────────────────────────────

func loadConfig(path string) config {
	cfg := config{
		Resolution: "unforced",
		Scaling:    "stretched_4_3",
		Fullscreen: true,
		Watermark:  false,
		Monitor:    "default",
	}

	f, err := os.Open(path)
	if err != nil {
		return cfg
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	inDDraw := false
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "[DirectDraw]") {
			inDDraw = true
			continue
		}
		if strings.HasPrefix(line, "[") {
			inDDraw = false
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}
		key := strings.TrimSpace(parts[0])
		val := strings.TrimSpace(parts[1])

		switch key {
		case "ScalingMode":
			if !inDDraw {
				cfg.Scaling = val
			}
		case "FullScreenMode":
			if !inDDraw {
				cfg.Fullscreen = val == "true"
			}
		case "dgVoodooWatermark":
			cfg.Watermark = val == "true"
		case "Resolution":
			if inDDraw {
				cfg.Resolution = val
			}
		case "FullScreenOutput":
			if !inDDraw {
				cfg.Monitor = val
			}
		}
	}
	return cfg
}

func saveConfig(path string, cfg config) {
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}

	lines := strings.Split(string(data), "\n")
	inDDraw := false
	resSet := false

	fsVal := "false"
	if cfg.Fullscreen {
		fsVal = "true"
	}
	wmVal := "false"
	if cfg.Watermark {
		wmVal = "true"
	}

	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "[DirectDraw]") {
			inDDraw = true
			continue
		}
		if strings.HasPrefix(trimmed, "[") && inDDraw {
			inDDraw = false
		}

		eqIdx := strings.Index(line, "=")
		if eqIdx < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eqIdx])
		prefix := line[:eqIdx+2]

		switch {
		case key == "ScalingMode" && !inDDraw:
			lines[i] = prefix + cfg.Scaling
		case key == "FullScreenMode" && !inDDraw:
			lines[i] = prefix + fsVal
		case key == "FullScreenOutput" && !inDDraw:
			lines[i] = prefix + cfg.Monitor
		case key == "dgVoodooWatermark":
			lines[i] = prefix + wmVal
		case key == "AppControlledScreenMode" && inDDraw:
			appVal := "false"
			if cfg.Fullscreen {
				appVal = "true"
			}
			lines[i] = prefix + appVal
		case key == "Resolution" && inDDraw && !resSet:
			lines[i] = prefix + cfg.Resolution
			resSet = true
		}
	}

	os.WriteFile(path, []byte(strings.Join(lines, "\n")), 0644)
}
