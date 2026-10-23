//go:build windows

package main

// Starts programs on the desktop of the user signed in at the console: the screen the SPICE display route shows,
// where RDP can't be used (an RDP connection would take that session over). This service runs as LocalSystem, so it
// starts them with the console user's token, environment and desktop.

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"

	"github.com/gorilla/mux"
	"golang.org/x/sys/windows"
)

type DesktopLaunchRequest struct {
	Path     string `json:"path"`     // an executable, possibly with environment variables or already quoted
	Args     string `json:"args"`     // its command line arguments
	Maximize bool   `json:"maximize"` // start its window maximized (the SPICE app window mode)
	Seamless bool   `json:"seamless"` // report its windows to the viewer (windowtracker.go)
	Taskbar  string `json:"taskbar"`  // "hide" or "show" Windows' taskbar first, or leave it ("")
}

var procExpandEnvironmentStringsForUserW = windows.NewLazySystemDLL("userenv.dll").NewProc("ExpandEnvironmentStringsForUserW")

func expandForUser(token windows.Token, value string) string {
	src, err := windows.UTF16PtrFromString(value)
	if err != nil {
		return value
	}
	buf := make([]uint16, 32768)
	ok, _, _ := procExpandEnvironmentStringsForUserW.Call(uintptr(token), uintptr(unsafe.Pointer(src)),
		uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if ok == 0 {
		return value
	}
	return windows.UTF16ToString(buf)
}

var errNoConsoleUser = errors.New("nobody is signed in to the Windows desktop")

type startOptions struct {
	Maximize bool // the window starts maximized
	Hidden   bool // no window at all (this server's own helpers)
	Wait     bool // until it exits (at most a few seconds)
	// With Wait: its exit code
	ExitCode *uint32
	// Nobody signed in at the console: the user's remote (RDP) session instead
	AnySession bool
}

// activeRemoteSession is the session of a user connected over RDP, if any
func activeRemoteSession() (uint32, bool) {
	var sessions *windows.WTS_SESSION_INFO
	var count uint32
	if err := windows.WTSEnumerateSessions(0, 0, 1, &sessions, &count); err != nil {
		return 0, false
	}
	defer windows.WTSFreeMemory(uintptr(unsafe.Pointer(sessions)))
	for _, session := range unsafe.Slice(sessions, count) {
		if session.State == windows.WTSActive && session.SessionID != 0 {
			return session.SessionID, true
		}
	}
	return 0, false
}

// startOnConsoleDesktop starts a program as the console user and returns its process id
func startOnConsoleDesktop(path string, args string, options startOptions) (uint32, error) {
	var token windows.Token
	err := windows.WTSQueryUserToken(windows.WTSGetActiveConsoleSessionId(), &token)
	if err != nil && options.AnySession {
		if session, ok := activeRemoteSession(); ok {
			err = windows.WTSQueryUserToken(session, &token)
		}
	}
	if err != nil {
		if errors.Is(err, windows.ERROR_NO_TOKEN) {
			return 0, errNoConsoleUser
		}
		return 0, err
	}
	defer token.Close()

	var env *uint16
	if err := windows.CreateEnvironmentBlock(&env, token, false); err != nil {
		return 0, err
	}
	defer windows.DestroyEnvironmentBlock(env)

	executable := strings.Trim(expandForUser(token, strings.TrimSpace(path)), `"`)
	commandLine := `"` + executable + `"`
	if args = strings.TrimSpace(args); args != "" {
		commandLine += " " + expandForUser(token, args)
	}
	cmd, err := windows.UTF16PtrFromString(commandLine)
	if err != nil {
		return 0, err
	}
	var dir *uint16
	if filepath.IsAbs(executable) {
		dir, _ = windows.UTF16PtrFromString(filepath.Dir(executable))
	}
	desktop, _ := windows.UTF16PtrFromString(`winsta0\default`)
	startup := windows.StartupInfo{Desktop: desktop}
	flags := uint32(windows.CREATE_UNICODE_ENVIRONMENT | windows.CREATE_NEW_PROCESS_GROUP)
	switch {
	case options.Hidden:
		startup.Flags = windows.STARTF_USESHOWWINDOW
		startup.ShowWindow = windows.SW_HIDE
		flags |= windows.CREATE_NO_WINDOW
	case options.Maximize:
		// The initial show state: most programs use it for their first window
		startup.Flags = windows.STARTF_USESHOWWINDOW
		startup.ShowWindow = windows.SW_SHOWMAXIMIZED
	}
	startup.Cb = uint32(unsafe.Sizeof(startup))
	var process windows.ProcessInformation
	if err := windows.CreateProcessAsUser(token, nil, cmd, nil, nil, false, flags, env, dir, &startup, &process); err != nil {
		return 0, err
	}
	if options.Wait {
		windows.WaitForSingleObject(process.Process, 5000)
		if options.ExitCode != nil {
			windows.GetExitCodeProcess(process.Process, options.ExitCode)
		}
	}
	windows.CloseHandle(process.Thread)
	windows.CloseHandle(process.Process)
	return process.ProcessId, nil
}

// processTreeRunning tells whether a process, or one it started (also after it exited), still runs
func processTreeRunning(pid uint32) bool {
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return true // can't tell: don't close anything
	}
	defer windows.CloseHandle(snapshot)

	children := map[uint32][]uint32{}
	running := map[uint32]bool{}
	var entry windows.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		running[entry.ProcessID] = true
		children[entry.ParentProcessID] = append(children[entry.ParentProcessID], entry.ProcessID)
	}
	if running[pid] {
		return true
	}
	// Descendants keep the id of a parent that exited
	queue := children[pid]
	for len(queue) > 0 {
		next := queue[0]
		queue = queue[1:]
		if running[next] {
			return true
		}
		queue = append(queue, children[next]...)
	}
	return false
}

func writeDesktopError(w http.ResponseWriter, what string, err error) {
	status := http.StatusInternalServerError
	if errors.Is(err, errNoConsoleUser) {
		status = http.StatusConflict
	}
	http.Error(w, what+" failed: "+err.Error(), status)
}

func launchOnDesktop(w http.ResponseWriter, r *http.Request) {
	var req DesktopLaunchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if strings.TrimSpace(req.Path) == "" {
		http.Error(w, "No program to start", http.StatusBadRequest)
		return
	}

	if req.Taskbar == "hide" || req.Taskbar == "show" {
		if err := setConsoleTaskbar(req.Taskbar == "show"); err != nil && !errors.Is(err, errNoConsoleUser) {
			log.Println("Setting the taskbar failed:", err)
		}
	}
	if req.Seamless {
		if err := ensureWindowTracker(); err != nil && !errors.Is(err, errNoConsoleUser) {
			log.Println("Starting the window tracker failed:", err)
		}
	}
	pid, err := startOnConsoleDesktop(req.Path, req.Args, startOptions{Maximize: req.Maximize})
	if err != nil {
		writeDesktopError(w, "Starting the program", err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]uint32{"pid": pid})
}

type DesktopTaskbarRequest struct {
	Visible bool `json:"visible"`
}

// setDesktopTaskbar shows or hides Windows' taskbar on the console desktop
func setDesktopTaskbar(w http.ResponseWriter, r *http.Request) {
	var req DesktopTaskbarRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if err := setConsoleTaskbar(req.Visible); err != nil {
		writeDesktopError(w, "Setting the taskbar", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// getDesktopProcess tells whether a program /desktop/launch started still runs (WinBoat closes its window after)
func getDesktopProcess(w http.ResponseWriter, r *http.Request) {
	pid, err := strconv.ParseUint(mux.Vars(r)["pid"], 10, 32)
	if err != nil {
		http.Error(w, "Invalid process id", http.StatusBadRequest)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]bool{"running": processTreeRunning(uint32(pid))})
}
