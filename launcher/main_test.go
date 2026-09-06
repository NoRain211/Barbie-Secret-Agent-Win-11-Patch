package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testConfig = "[General]\r\n" +
	"FullScreenOutput                     = 1\r\n" +
	"FullScreenMode                       = false\r\n" +
	"ScalingMode                          = stretched_4_3\r\n" +
	"\r\n" +
	"[Glide]\r\n" +
	"Resolution                          = 640x480\r\n" +
	"\r\n" +
	"[DirectX]\r\n" +
	"Resolution                          = max\r\n" +
	"AppControlledScreenMode             = false\r\n"

func TestConfigRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "dgVoodoo.conf")
	if err := os.WriteFile(path, []byte(testConfig), 0644); err != nil {
		t.Fatal(err)
	}

	cfg, err := loadConfig(path)
	if err != nil {
		t.Fatalf("loadConfig: %v", err)
	}
	if cfg.Resolution != "max" {
		t.Fatalf("Resolution = %q; want max from [DirectX]", cfg.Resolution)
	}

	cfg.Resolution = "h:1920, v:1080"
	cfg.Scaling = "stretched"
	cfg.Fullscreen = true
	if err := saveConfig(path, cfg); err != nil {
		t.Fatalf("saveConfig: %v", err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got := string(data)
	for _, want := range []string{
		"FullScreenMode                       = true\r\n",
		"ScalingMode                          = stretched\r\n",
		"Resolution                          = 640x480\r\n",
		"Resolution                          = h:1920, v:1080\r\n",
		"AppControlledScreenMode             = false\r\n",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("saved config missing %q", want)
		}
	}
	if strings.Count(got, "\r\n") != strings.Count(testConfig, "\r\n") {
		t.Error("saveConfig changed CRLF line endings")
	}
}

func TestConfigErrors(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "missing.conf")
	if _, err := loadConfig(missing); err == nil {
		t.Error("loadConfig missing file returned nil error")
	}
	if err := saveConfig(missing, defaultConfig()); err == nil {
		t.Error("saveConfig missing file returned nil error")
	}
}

func TestValidateConfig(t *testing.T) {
	valid := defaultConfig()
	if err := validateConfig(valid); err != nil {
		t.Fatalf("default config rejected: %v", err)
	}

	invalid := valid
	invalid.Resolution = "bad\nResolution = max"
	if err := validateConfig(invalid); err == nil {
		t.Error("invalid resolution accepted")
	}

	invalid = valid
	invalid.FOVFactor = 0
	if err := validateConfig(invalid); err == nil {
		t.Error("invalid FOV multiplier accepted")
	}
}

func TestWidescreenConfig(t *testing.T) {
	path := filepath.Join(t.TempDir(), "SecretAgentBarbieWidescreenFix.ini")
	cfg := defaultConfig()
	cfg.Resolution = "h:1920, v:1080"
	cfg.FOVFactor = 1.15
	if err := saveWidescreenConfig(path, cfg); err != nil {
		t.Fatal(err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"Enabled=true", "Width=1920", "Height=1080", "FOVFactor=1.150"} {
		if !strings.Contains(string(data), want) {
			t.Errorf("saved widescreen config missing %q", want)
		}
	}
	fovFactor, enabled, err := loadWidescreenConfig(path)
	if err != nil || !enabled || fovFactor != 1.15 {
		t.Fatalf("loadWidescreenConfig = (%v, %v, %v)", fovFactor, enabled, err)
	}
}

func TestLaunchGameMissingExecutable(t *testing.T) {
	if err := launchGame(t.TempDir()); err == nil {
		t.Error("launchGame missing executable returned nil error")
	}
}

func TestIndexRequiresToken(t *testing.T) {
	oldToken := token
	token = "test-token"
	t.Cleanup(func() { token = oldToken })

	for _, tc := range []struct {
		name   string
		method string
		url    string
		status int
	}{
		{"missing token", http.MethodGet, "/", http.StatusForbidden},
		{"valid token", http.MethodGet, "/?token=test-token", http.StatusOK},
		{"wrong method", http.MethodPost, "/?token=test-token", http.StatusMethodNotAllowed},
	} {
		t.Run(tc.name, func(t *testing.T) {
			recorder := httptest.NewRecorder()
			handleIndex(recorder, httptest.NewRequest(tc.method, tc.url, nil))
			if recorder.Code != tc.status {
				t.Fatalf("status = %d; want %d", recorder.Code, tc.status)
			}
		})
	}
}
