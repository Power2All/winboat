//go:build windows
// +build windows

package main

// Windows' light or dark mode, which WinBoat sets (its "Windows color mode" setting: the host's own, or a fixed
// one). Both of Windows' switches are the user's settings (apps, and Windows itself: taskbar, Start), so this server
// runs itself in the user's session (--theme light|dark) to set them and tell the running apps.

import (
	"encoding/json"
	"net/http"
	"os"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const personalizeKey = `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`

var procSendMessageTimeoutTheme = windows.NewLazySystemDLL("user32.dll").NewProc("SendMessageTimeoutW")

// setTheme runs in the user's session: Windows' apps and Windows itself in light or dark mode
func setTheme(light bool) error {
	key, _, err := registry.CreateKey(registry.CURRENT_USER, personalizeKey, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer key.Close()
	value := uint32(0)
	if light {
		value = 1
	}
	if err := key.SetDWordValue("AppsUseLightTheme", value); err != nil {
		return err
	}
	if err := key.SetDWordValue("SystemUsesLightTheme", value); err != nil {
		return err
	}
	// Running apps (and the taskbar) switch when told
	const hwndBroadcast, wmSettingChange, smtoAbortIfHung = 0xFFFF, 0x001A, 0x0002
	area, _ := windows.UTF16PtrFromString("ImmersiveColorSet")
	var result uintptr
	procSendMessageTimeoutTheme.Call(hwndBroadcast, wmSettingChange, 0, uintptr(unsafe.Pointer(area)),
		smtoAbortIfHung, 1000, uintptr(unsafe.Pointer(&result)))
	return nil
}

// handleThemeArgs runs `--theme light|dark` (setUserTheme starts it in the user's session) and exits
func handleThemeArgs() {
	if len(os.Args) == 3 && os.Args[1] == "--theme" {
		if err := setTheme(os.Args[2] == "light"); err != nil {
			os.Exit(1)
		}
		os.Exit(0)
	}
}

type DesktopThemeRequest struct {
	Mode string `json:"mode"` // "light" or "dark"
}

// setDesktopTheme sets Windows' light or dark mode
func setDesktopTheme(w http.ResponseWriter, r *http.Request) {
	var req DesktopThemeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.Mode != "light" && req.Mode != "dark" {
		http.Error(w, `mode must be "light" or "dark"`, http.StatusBadRequest)
		return
	}
	self, err := os.Executable()
	if err != nil {
		writeDesktopError(w, "Setting the theme", err)
		return
	}
	var code uint32
	if _, err := startOnConsoleDesktop(self, "--theme "+req.Mode,
		startOptions{Hidden: true, Wait: true, ExitCode: &code, AnySession: true}); err != nil {
		writeDesktopError(w, "Setting the theme", err)
		return
	}
	if code != 0 {
		http.Error(w, "Setting the theme failed", http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
