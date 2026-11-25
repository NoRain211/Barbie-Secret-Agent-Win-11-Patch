//go:build !windows

package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"sync"
	"syscall"
	"time"
)

// The game is a 32-bit Windows program, so on Linux it runs under Wine. Wine
// prefers its builtin ddraw/dinput/d3dimm over DLLs in the game folder, which
// would bypass the patch and dgVoodoo2, so the launcher forces native first.
const wineDLLOverrides = "ddraw,dinput,d3dimm=n,b"

// Without a native webview, the UI opens in a browser. In the default-browser
// fallback the page pings the launcher, and the launcher exits once the pings
// stop (tab closed). Browsers throttle background tabs, so the timeout is long.
const (
	pingTimeout      = 90 * time.Second
	firstPingTimeout = 5 * time.Minute
)

var (
	uiDone    = make(chan struct{})
	closeOnce sync.Once
	pingMu    sync.Mutex
	lastPing  time.Time
)

func closeUI() { closeOnce.Do(func() { close(uiDone) }) }

func notePing() {
	pingMu.Lock()
	lastPing = time.Now()
	pingMu.Unlock()
}

func runUI(url string) error {
	browser, profile := openBrowser(url)
	if profile != "" {
		defer os.RemoveAll(profile)
	}
	fmt.Fprintln(os.Stderr, "Secret Agent Barbie launcher:", url)
	if browser == nil {
		fmt.Fprintln(os.Stderr, "Open the address above in a web browser if it did not open automatically.")
	} else {
		exited := make(chan struct{})
		go func() {
			_ = browser.Wait()
			close(exited)
			closeUI()
		}()
		defer func() {
			// After a launch the app window is ours to close, as on Windows.
			select {
			case <-exited:
			default:
				_ = browser.Process.Signal(syscall.SIGTERM)
				select {
				case <-exited:
				case <-time.After(3 * time.Second):
					_ = browser.Process.Kill()
				}
			}
		}()
	}

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(signals)

	started := time.Now()
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-uiDone:
			return nil
		case <-signals:
			return nil
		case <-ticker.C:
			if browser != nil {
				continue // The app window's process owns the lifetime.
			}
			pingMu.Lock()
			last := lastPing
			pingMu.Unlock()
			if (last.IsZero() && time.Since(started) > firstPingTimeout) ||
				(!last.IsZero() && time.Since(last) > pingTimeout) {
				return nil
			}
		}
	}
}

// openBrowser prefers a Chromium-based browser in app mode with a private
// profile: it opens a separate, launcher-sized window whose process exits when
// the window is closed. Otherwise it falls back to the desktop's default
// browser, and the ping timeout ends the launcher.
func openBrowser(url string) (*exec.Cmd, string) {
	candidates := []string{"chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
		"brave-browser", "brave", "microsoft-edge", "microsoft-edge-stable", "vivaldi"}
	if custom := os.Getenv("BARBIE_LAUNCHER_BROWSER"); custom != "" {
		candidates = []string{custom}
	}
	for _, name := range candidates {
		path, err := exec.LookPath(name)
		if err != nil {
			continue
		}
		profile, err := os.MkdirTemp("", "barbie-launcher-")
		if err != nil {
			break
		}
		cmd := exec.Command(path, "--app="+url, "--window-size=480,760",
			"--user-data-dir="+profile, "--no-first-run", "--no-default-browser-check")
		if cmd.Start() == nil {
			return cmd, profile
		}
		os.RemoveAll(profile)
	}
	for _, opener := range []string{"xdg-open", "open"} {
		if path, err := exec.LookPath(opener); err == nil {
			if exec.Command(path, url).Start() == nil {
				return nil, ""
			}
		}
	}
	return nil, ""
}

// xrandr reports "<index>: +[*]<name> <w>/<mm>x<h>/<mm>+<x>+<y> <name>", with
// "*" on the primary monitor.
var xrandrMonitor = regexp.MustCompile(`^\s*\d+:\s+\+(\*?)(\S+)\s+(\d+)/\d+x(\d+)/\d+`)

func parseXrandrMonitors(output []byte) []monitor {
	type entry struct {
		w, h    int
		primary bool
	}
	var found []entry
	scanner := bufio.NewScanner(bytes.NewReader(output))
	for scanner.Scan() {
		m := xrandrMonitor.FindStringSubmatch(scanner.Text())
		if m == nil {
			continue
		}
		w, _ := strconv.Atoi(m[3])
		h, _ := strconv.Atoi(m[4])
		e := entry{w: w, h: h, primary: m[1] == "*"}
		// Wine lists the primary monitor first, so dgVoodoo2's output ordinals do too.
		if e.primary {
			found = append([]entry{e}, found...)
		} else {
			found = append(found, e)
		}
	}
	monitors := []monitor{{Label: "Default", Value: "default"}}
	for i, e := range found {
		primary := ""
		if e.primary {
			primary = " (Primary)"
		}
		monitors = append(monitors, monitor{
			Label: fmt.Sprintf("Monitor %d%s - %dx%d", i+1, primary, e.w, e.h),
			Value: strconv.Itoa(i + 1),
			width: e.w, height: e.h,
		})
	}
	return monitors
}

func detectMonitors() []monitor {
	output, err := exec.Command("xrandr", "--listmonitors").Output()
	if err != nil {
		return []monitor{{Label: "Default", Value: "default"}}
	}
	return parseXrandrMonitors(output)
}

func checkWindowsExecutable(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	magic := make([]byte, 2)
	if _, err := io.ReadFull(f, magic); err != nil || string(magic) != "MZ" {
		return fmt.Errorf("%s is not a Windows executable", path)
	}
	return nil
}

func gameCommand(gamePath string) (*exec.Cmd, error) {
	if err := checkWindowsExecutable(gamePath); err != nil {
		return nil, err
	}
	wine := os.Getenv("WINE")
	if wine == "" {
		wine = "wine"
	}
	winePath, err := exec.LookPath(wine)
	if err != nil {
		return nil, fmt.Errorf("Wine was not found (%s); install Wine or set WINE to its path", wine)
	}
	cmd := exec.Command(winePath, gamePath)
	overrides := wineDLLOverrides
	if existing := os.Getenv("WINEDLLOVERRIDES"); existing != "" {
		overrides += ";" + existing
	}
	cmd.Env = append(os.Environ(), "WINEDLLOVERRIDES="+overrides)
	if os.Getenv("WINEDEBUG") == "" {
		cmd.Env = append(cmd.Env, "WINEDEBUG=-all")
	}
	// Keep the game running after the launcher exits or its terminal closes.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	return cmd, nil
}

// startGame records Wine's output in wine.log beside the game for bug reports.
func startGame(cmd *exec.Cmd) error {
	logFile, err := os.Create(filepath.Join(cmd.Dir, "wine.log"))
	if err != nil {
		return fmt.Errorf("could not create wine.log: %w", err)
	}
	defer logFile.Close()
	cmd.Stdout, cmd.Stderr = logFile, logFile
	if err := cmd.Start(); err != nil {
		return err
	}
	go func() { _ = cmd.Wait() }()
	return nil
}
