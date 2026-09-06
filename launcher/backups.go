package main

import (
	"archive/zip"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func backupSaves(dir string) error {
	saves := filepath.Join(dir, "Saves")
	if info, err := os.Lstat(saves); errors.Is(err, os.ErrNotExist) {
		return nil // First launch has no saves yet.
	} else if err != nil {
		return err
	} else if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s must be a directory, not a link or file", saves)
	}
	backups := filepath.Join(dir, "SaveBackups")
	if err := os.MkdirAll(backups, 0755); err != nil {
		return err
	}
	if info, err := os.Lstat(backups); err != nil {
		return err
	} else if info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s must not be a link", backups)
	}
	f, err := os.CreateTemp(backups, "saves-"+time.Now().UTC().Format("20060102T150405.000000000Z")+"-*.zip.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	archive := zip.NewWriter(f)
	err = filepath.WalkDir(saves, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("cannot back up linked save path %s", path)
		}
		if !entry.IsDir() && !entry.Type().IsRegular() {
			return fmt.Errorf("cannot back up non-regular save file %s", path)
		}
		name, err := filepath.Rel(dir, path)
		if err != nil {
			return err
		}
		if entry.IsDir() {
			_, err = archive.Create(filepath.ToSlash(name) + "/")
			return err
		}
		dst, err := archive.Create(filepath.ToSlash(name))
		if err != nil {
			return err
		}
		src, err := os.Open(path)
		if err != nil {
			return err
		}
		_, err = io.Copy(dst, src)
		return errors.Join(err, src.Close())
	})
	err = errors.Join(err, archive.Close(), f.Sync(), f.Close())
	if err != nil {
		return err
	}
	if err := os.Rename(f.Name(), strings.TrimSuffix(f.Name(), ".tmp")); err != nil {
		return err
	}
	// Only prune our completed snapshots, after the new archive is safely closed.
	entries, err := os.ReadDir(backups) // Sorted by timestamp-prefixed filename.
	if err != nil {
		return err
	}
	var snapshots []string
	for _, entry := range entries {
		match, _ := filepath.Match("saves-????????T??????.?????????Z-*.zip", entry.Name())
		if match && entry.Type().IsRegular() {
			snapshots = append(snapshots, entry.Name())
		}
	}
	for len(snapshots) > 5 {
		if err := os.Remove(filepath.Join(backups, snapshots[0])); err != nil {
			return fmt.Errorf("remove old save backup: %w", err)
		}
		snapshots = snapshots[1:]
	}
	return nil
}
