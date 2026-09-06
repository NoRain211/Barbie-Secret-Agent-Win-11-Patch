package main

import (
	"archive/zip"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestSaveBackups(t *testing.T) {
	dir := t.TempDir()
	if err := backupSaves(dir); err != nil {
		t.Fatalf("first launch: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "SaveBackups")); !os.IsNotExist(err) {
		t.Fatal("missing saves created a backup directory")
	}
	profile := filepath.Join(dir, "Saves", "Barbie")
	if err := os.MkdirAll(profile, 0755); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 7; i++ {
		if err := os.WriteFile(filepath.Join(profile, "SaveData.0"), []byte(strconv.Itoa(i)), 0644); err != nil {
			t.Fatal(err)
		}
		if err := backupSaves(dir); err != nil {
			t.Fatal(err)
		}
	}
	backups := filepath.Join(dir, "SaveBackups")
	entries, err := os.ReadDir(backups)
	if err != nil || len(entries) != 5 {
		t.Fatalf("backups = %v, %v; want five", entries, err)
	}
	for i, entry := range entries {
		archive, err := zip.OpenReader(filepath.Join(backups, entry.Name()))
		if err != nil {
			t.Fatal(err)
		}
		file, err := archive.Open("Saves/Barbie/SaveData.0")
		if err != nil {
			archive.Close()
			t.Fatal(err)
		}
		data, err := io.ReadAll(file)
		file.Close()
		archive.Close()
		if err != nil || string(data) != strconv.Itoa(i+2) {
			t.Fatalf("snapshot %d = %q, %v; want %d", i, data, err, i+2)
		}
	}
	data, err := os.ReadFile(filepath.Join(profile, "SaveData.0"))
	if err != nil || string(data) != "6" {
		t.Fatalf("live save changed: %q, %v", data, err)
	}
	// A failed snapshot must preserve existing backups and clean up its temp file.
	if err := os.Symlink(filepath.Join(profile, "SaveData.0"), filepath.Join(profile, "linked-save")); err != nil {
		t.Logf("symlink failure check unavailable: %v", err)
		return
	}
	if err := backupSaves(dir); err == nil {
		t.Fatal("linked save accepted")
	}
	after, err := os.ReadDir(backups)
	if err != nil || len(after) != len(entries) {
		t.Fatalf("failed backup changed snapshots: %v, %v", after, err)
	}
	for i := range entries {
		if after[i].Name() != entries[i].Name() {
			t.Fatal("failed backup replaced a snapshot")
		}
	}
}

func TestBackupFailurePreventsLaunch(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{"SecretAgent.exe", "SaveBackups"} {
		if err := os.WriteFile(filepath.Join(dir, name), nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "Saves"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := launchGame(dir, true); err == nil || !strings.Contains(err.Error(), "save backup failed") {
		t.Fatalf("backup failure did not block launch: %v", err)
	}
	// With backups disabled, reach the process launch (this test EXE is invalid).
	if err := launchGame(dir, false); err == nil || strings.Contains(err.Error(), "save backup failed") {
		t.Fatalf("disabled backup still ran: %v", err)
	}
}
