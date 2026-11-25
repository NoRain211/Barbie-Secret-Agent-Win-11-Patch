//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseXrandrMonitors(t *testing.T) {
	output := []byte("Monitors: 2\n" +
		" 0: +HDMI-1 1920/527x1080/296+0+0  HDMI-1\n" +
		" 1: +*DP-1 2560/597x1440/336+1920+0  DP-1\n")
	monitors := parseXrandrMonitors(output)
	if len(monitors) != 3 || monitors[0].Value != "default" {
		t.Fatalf("monitors = %+v", monitors)
	}
	if m := monitors[1]; m.Value != "1" || m.width != 2560 || m.height != 1440 ||
		m.Label != "Monitor 1 (Primary) - 2560x1440" {
		t.Errorf("primary monitor not first: %+v", m)
	}
	if m := monitors[2]; m.Value != "2" || m.width != 1920 || m.height != 1080 {
		t.Errorf("second monitor = %+v", m)
	}
	if got := parseXrandrMonitors(nil); len(got) != 1 {
		t.Errorf("empty output = %+v", got)
	}
}

func TestGameCommandUsesWineOverrides(t *testing.T) {
	dir := t.TempDir()
	game := filepath.Join(dir, "SecretAgent.exe")
	if err := os.WriteFile(game, []byte("MZ\x90\x00"), 0644); err != nil {
		t.Fatal(err)
	}
	fakeWine := filepath.Join(dir, "fake-wine")
	if err := os.WriteFile(fakeWine, []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("WINE", fakeWine)
	t.Setenv("WINEDLLOVERRIDES", "mscoree=")
	t.Setenv("WINEDEBUG", "")

	cmd, err := gameCommand(game)
	if err != nil {
		t.Fatal(err)
	}
	if cmd.Path != fakeWine || len(cmd.Args) != 2 || cmd.Args[1] != game {
		t.Errorf("command = %q %q", cmd.Path, cmd.Args)
	}
	env := strings.Join(cmd.Env, "\n")
	for _, want := range []string{"WINEDLLOVERRIDES=" + wineDLLOverrides + ";mscoree=", "WINEDEBUG=-all"} {
		if !strings.Contains(env, want) {
			t.Errorf("environment missing %q", want)
		}
	}

	t.Setenv("WINE", filepath.Join(dir, "missing-wine"))
	if _, err := gameCommand(game); err == nil || !strings.Contains(err.Error(), "Wine was not found") {
		t.Errorf("missing Wine error = %v", err)
	}
	if err := os.WriteFile(game, []byte("#!/bin/sh\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := gameCommand(game); err == nil || !strings.Contains(err.Error(), "not a Windows executable") {
		t.Errorf("non-PE error = %v", err)
	}
}
