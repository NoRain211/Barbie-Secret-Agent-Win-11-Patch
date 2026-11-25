//go:build windows

package main

import (
	"errors"
	"fmt"
	"os/exec"
	"strconv"
	"sync"
	"syscall"
	"unsafe"

	"github.com/jchv/go-webview2"
)

var (
	user32                  = syscall.NewLazyDLL("user32.dll")
	procEnumDisplayMonitors = user32.NewProc("EnumDisplayMonitors")
	procGetMonitorInfoW     = user32.NewProc("GetMonitorInfoW")
	procEnumDisplaySettings = user32.NewProc("EnumDisplaySettingsW")
)

type monitorInfoEx struct {
	cbSize    uint32
	rcMonitor struct{ left, top, right, bottom int32 }
	rcWork    struct{ left, top, right, bottom int32 }
	dwFlags   uint32
	szDevice  [32]uint16
}

// devMode is DEVMODEW reduced to the fields the launcher reads.
type devMode struct {
	_          [68]byte
	size       uint16
	_          [102]byte
	pelsWidth  uint32
	pelsHeight uint32
	_          [40]byte
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
	// The launcher is not DPI-aware, so rcMonitor can be scaled; the display mode is not.
	mode := devMode{size: uint16(unsafe.Sizeof(devMode{}))}
	const enumCurrentSettings = 0xFFFFFFFF
	if result, _, _ := procEnumDisplaySettings.Call(uintptr(unsafe.Pointer(&info.szDevice[0])),
		enumCurrentSettings, uintptr(unsafe.Pointer(&mode))); result != 0 {
		w, h = int32(mode.pelsWidth), int32(mode.pelsHeight)
	}
	primary := ""
	if info.dwFlags&1 != 0 {
		primary = " (Primary)"
	}
	*scan.monitors = append(*scan.monitors, monitor{
		Label: fmt.Sprintf("Monitor %d%s - %dx%d", scan.index, primary, w, h),
		Value: strconv.Itoa(scan.index),
		width: int(w), height: int(h),
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

var wv webview2.WebView

func runUI(url string) error {
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
		return errors.New("failed to create webview2 window")
	}
	defer wv.Destroy()

	wv.Navigate(url)
	wv.Run()
	return nil
}

func closeUI() {
	wv.Dispatch(func() { wv.Destroy() })
}

// The WebView2 window owns the launcher's lifetime, so pings are unused.
func notePing() {}

func gameCommand(gamePath string) (*exec.Cmd, error) {
	return exec.Command(gamePath), nil
}

func startGame(cmd *exec.Cmd) error {
	return cmd.Start()
}
