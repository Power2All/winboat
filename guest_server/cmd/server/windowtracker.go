//go:build windows
// +build windows

package main

// The window tracker of the SPICE display route's seamless mode: it runs in the user's session
// (--window-tracker, started by /desktop/launch) and reports the windows of the apps WinBoat starts over the SPICE
// port "org.winboat.windows" (a virtio-serial port). WinBoat's viewer shows each of them as a Linux window of its own,
// like RDP's RemoteApp; spice/viewer-build/winboat-spice-viewer.c describes the protocol. Only one tracker runs: the
// port opens once.
//
// Dragging a window's title bar or edge starts Windows' own move loop; the tracker tells the viewer, which hands the
// drag to the Linux window manager (like RemoteApp's local move/size) and reports where the window goes ("move").
// Apps started in Windows while apps are shown (a link that opens the browser, a program another one starts) show
// too: a window that shows up from then on is adopted, its process tracked from then on (only that window for
// Explorer's shell, whose other windows are the taskbar and the desktop), and it comes to the front. That includes
// apps that hand their start to an instance running already (Edge, Explorer's folders, single-instance apps): the
// started process exits, and one WinBoat didn't start opens the window, maybe before the viewer tracks the app.
// Windows' screen is one picture: where two app windows overlap in Windows, each Linux window shows the one on top
// there. The tracker stacks them as the Linux window manager does ("order", from the viewer).
// Other apps' windows (from using the desktop) are kept under the tracked ones: they share Windows' screen. The
// desktop behind them gets the color the viewer asks for ("background": black, or one it leaves out to show the
// Linux desktop) while a viewer is connected, for the session only: the wallpaper comes back when it disconnects.

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

const windowsPortPath = `\\.\Global\org.winboat.windows`

const (
	// How long before its "track" an app may have shown its window (WinBoat starts it, then the viewer, which tracks
	// it): windows shown since are adopted
	launchLead = 15 * time.Second
	// How long after its "track" an app handed to a running instance may take to show its window (the viewer stays)
	launchGrace = 20 * time.Second
	// How long a window started in Windows shows (not minimized) before it is adopted: consoles of programs started
	// hidden flash up in Windows Terminal for a moment, then minimize or go
	adoptAfter = 500 * time.Millisecond
)

// The virtio-serial driver's IOCTL_GET_INFORMATION (CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT,
// FILE_ANY_ACCESS)): a VIRTIO_PORT_INFO (UINT Id, BOOLEAN OutVqFull, BOOLEAN HostConnected, ...)
const ioctlPortInformation = 0x222002

var (
	wtUser32                   = windows.NewLazySystemDLL("user32.dll")
	procSetWinEventHook        = wtUser32.NewProc("SetWinEventHook")
	procGetMessageW            = wtUser32.NewProc("GetMessageW")
	procDispatchMessageW       = wtUser32.NewProc("DispatchMessageW")
	procPostThreadMessageW     = wtUser32.NewProc("PostThreadMessageW")
	procSetTimer               = wtUser32.NewProc("SetTimer")
	procEnumWindows            = wtUser32.NewProc("EnumWindows")
	procIsWindowVisible        = wtUser32.NewProc("IsWindowVisible")
	procIsIconic               = wtUser32.NewProc("IsIconic")
	procIsWindow               = wtUser32.NewProc("IsWindow")
	procGetWindowThreadProcess = wtUser32.NewProc("GetWindowThreadProcessId")
	procGetWindowRect          = wtUser32.NewProc("GetWindowRect")
	procGetWindowLongPtrW      = wtUser32.NewProc("GetWindowLongPtrW")
	procGetWindow              = wtUser32.NewProc("GetWindow")
	procGetForegroundWindow    = wtUser32.NewProc("GetForegroundWindow")
	procGetWindowTextW         = wtUser32.NewProc("GetWindowTextW")
	procSetForegroundWindow    = wtUser32.NewProc("SetForegroundWindow")
	procBringWindowToTop       = wtUser32.NewProc("BringWindowToTop")
	procPostMessageW           = wtUser32.NewProc("PostMessageW")
	procAttachThreadInput      = wtUser32.NewProc("AttachThreadInput")
	procSystemParametersInfoW  = wtUser32.NewProc("SystemParametersInfoW")
	procSendMessageTimeoutW    = wtUser32.NewProc("SendMessageTimeoutW")
	procGetCursorPos           = wtUser32.NewProc("GetCursorPos")
	procSetWindowPos           = wtUser32.NewProc("SetWindowPos")
	procIsZoomed               = wtUser32.NewProc("IsZoomed")
	procGetSystemMetrics       = wtUser32.NewProc("GetSystemMetrics")
	procGetClassNameW          = wtUser32.NewProc("GetClassNameW")
	procGetShellWindow         = wtUser32.NewProc("GetShellWindow")
	procGetWindowPlacement     = wtUser32.NewProc("GetWindowPlacement")
	procGetSysColor            = wtUser32.NewProc("GetSysColor")
	procSetSysColors           = wtUser32.NewProc("SetSysColors")
	procDwmGetWindowAttribute  = windows.NewLazySystemDLL("dwmapi.dll").NewProc("DwmGetWindowAttribute")
)

const (
	eventSystemForeground  = 0x0003
	eventSystemMoveSizeBeg = 0x000A
	eventSystemMinimizeEnd = 0x0017
	eventObjectCreate      = 0x8000
	eventObjectNameChange  = 0x800C
	wineventOutOfContext   = 0x0000
	objidWindow            = 0
	wmTimer                = 0x0113
	wmApp                  = 0x8000
	wmMoveSizeStarted      = wmApp + 1
	wmClose                = 0x0010
	wmCancelMode           = 0x001F
	wmNcHitTest            = 0x0084
	smtoAbortIfHung        = 0x0002
	gwlStyle               = -16
	gwlExStyle             = -20
	gwOwner                = 4
	wsPopup                = 0x80000000
	smCxScreen             = 0
	smCyScreen             = 1
	wsCaption              = 0x00C00000
	wsThickFrame           = 0x00040000
	wsExTopmost            = 0x00000008
	wsExToolWindow         = 0x00000080
	wsExAppWindow          = 0x00040000
	swMaximize             = 3
	swMinimize             = 6
	swRestore              = 9
	swpNoSize              = 0x0001
	swpNoMove              = 0x0002
	swpNoZOrder            = 0x0004
	swpNoActivate          = 0x0010
	swpNoOwnerZOrder       = 0x0200
	swpAsyncWindowPos      = 0x4000
	spiSetWorkArea         = 0x002F
	spiGetWorkArea         = 0x0030
	spiSetDeskWallpaper    = 0x0014
	spiGetDeskWallpaper    = 0x0073
	colorBackground        = 1
	wpfRestoreToMaximized  = 0x0002
	spifSendChange         = 0x0002
	dwmwaExtendedFrame     = 9
	dwmwaCloaked           = 14
)

type msg struct {
	HWnd    uintptr
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      struct{ X, Y int32 }
}

// What the viewer was told about a window
type trackedWindow struct {
	line  string // its "window" line
	title string
	popup bool
	icon  bool // its icon was looked for
}

type windowTracker struct {
	port     windows.Handle
	writeMu  sync.Mutex
	threadID uint32

	mu      sync.Mutex
	roots   map[uint32]bool // the apps WinBoat started
	pids    map[uint32]bool // their processes and what they started
	exes    map[uint32]string
	paths   map[uint32]string // their executables
	sent    map[uintptr]trackedWindow
	running bool // a tracked process ran since the last "idle"
	// The work area the seamless viewer asked for, while it's connected: Windows resets it when its resolution
	// changes (the viewer's comes after), and the screen size it was set for
	workArea       windows.Rect
	workAreaScreen [2]uintptr

	// Windows shown since adoptSince (the first "track" since the last "idle") are adopted. The latest "track"'s app
	// may take until launchUntil to show its window (one shown since launchSince)
	adoptSince, launchSince, launchUntil time.Time
	// Apps started in Windows, or the instances an app's start was handed to: their processes, or single windows
	// (Explorer's shell)
	adoptedPids    map[uint32]bool
	adoptedWindows map[uintptr]bool
	adoptedShown   bool // one of their windows is shown: the app still runs
	// When each top-level window was first seen shown (zero: already when the tracker started)
	firstShown map[uintptr]time.Time

	// The app windows as the Linux window manager stacks them, from the top down ("order")
	order []uintptr
	// Windows' foreground window, and since when (run thread only)
	lastForeground uintptr
	foregroundAt   time.Time

	// Where the viewer moved windows to (the latest per window; mover applies them)
	movesMu  sync.Mutex
	moves    map[uintptr]pendingMove
	moveWake chan struct{}

	icons map[string]string // "icon" data per executable

	// The desktop's wallpaper and color before "background" changed them (saved: to put back)
	backgroundMu      sync.Mutex
	backgroundSaved   bool
	savedWallpaper    string
	savedDesktopColor uint32

	// The invisible borders around each window's visible frame (left, top, right, bottom), measured while it was shown:
	// a minimized window's restored place includes them
	borders map[uintptr][4]int32

	// snapshot's EnumWindows callback (Go has a limited number of callbacks: made once) and what it collects: the
	// tracked windows, and every window from the top down
	enumCallback uintptr
	enumPids     map[uint32]bool
	enumWindows  map[uintptr]bool
	enumFound    map[uintptr]uint32
	enumOrder    []uintptr
}

// trackerLog: `--window-tracker <log file>` logs the port's lines (debugging)
var trackerLog *os.File

func logTracker(format string, args ...any) {
	if trackerLog != nil {
		fmt.Fprintf(trackerLog, time.Now().Format("15:04:05.000 ")+format+"\n", args...)
	}
}

// handleWindowTrackerArgs runs `--window-tracker` (/desktop/launch starts it in the user's session) and exits
func handleWindowTrackerArgs() {
	if len(os.Args) >= 2 && len(os.Args) <= 3 && os.Args[1] == "--window-tracker" {
		if len(os.Args) == 3 {
			trackerLog, _ = os.Create(os.Args[2])
		}
		if err := runWindowTracker(); err != nil {
			logTracker("%v", err)
			fmt.Fprintln(os.Stderr, "Window tracker:", err)
			os.Exit(1)
		}
		os.Exit(0)
	}
}

// ensureWindowTracker starts the window tracker in the console user's session (a second one exits right away)
func ensureWindowTracker() error {
	tracker, err := trackerExecutable()
	if err != nil {
		return err
	}
	_, err = startOnConsoleDesktop(tracker, "--window-tracker", startOptions{Hidden: true})
	return err
}

// The tracker runs from a copy of this executable, one per version: it keeps running after the Guest Server is
// replaced by an update, and Windows can't replace (or move) an executable that runs
func trackerDir() string {
	return filepath.Join(os.Getenv("ProgramData"), "WinBoat", "WindowTracker")
}

// trackerExecutable is this version's copy, made when needed. Trackers of other versions are ended (the port opens
// once: theirs would keep it) and their copies removed.
func trackerExecutable() (string, error) {
	self, err := os.Executable()
	if err != nil {
		return "", err
	}
	dir := trackerDir()
	name := "winboat_window_tracker-" + strings.Map(func(r rune) rune {
		if r == '.' || r == '-' || (r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
			return r
		}
		return '_'
	}, Version) + ".exe"
	path := filepath.Join(dir, name)
	endOtherTrackers(dir, path)
	if entries, err := os.ReadDir(dir); err == nil {
		for _, entry := range entries {
			if !strings.EqualFold(entry.Name(), name) {
				os.Remove(filepath.Join(dir, entry.Name()))
			}
		}
	}
	if info, err := os.Stat(path); err == nil && info.Size() > 0 {
		if selfInfo, err := os.Stat(self); err == nil && selfInfo.Size() == info.Size() {
			return path, nil
		}
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	data, err := os.ReadFile(self)
	if err != nil {
		return "", err
	}
	temp := path + ".new"
	if err := os.WriteFile(temp, data, 0o755); err != nil {
		return "", err
	}
	if err := os.Rename(temp, path); err != nil {
		os.Remove(temp)
		return "", err
	}
	return path, nil
}

// endOtherTrackers ends the trackers running from dir other than keep, and the ones running from the Guest Server's
// own executable (versions before the copies)
func endOtherTrackers(dir, keep string) {
	self, _ := os.Executable()
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return
	}
	defer windows.CloseHandle(snapshot)
	var entry windows.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		if entry.ProcessID == uint32(os.Getpid()) {
			continue
		}
		process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION|windows.PROCESS_TERMINATE|
			windows.SYNCHRONIZE, false, entry.ProcessID)
		if err != nil {
			continue
		}
		buf := make([]uint16, windows.MAX_PATH)
		size := uint32(len(buf))
		if windows.QueryFullProcessImageName(process, 0, &buf[0], &size) == nil {
			image := windows.UTF16ToString(buf[:size])
			other := strings.EqualFold(filepath.Dir(image), dir) && !strings.EqualFold(image, keep)
			// The Guest Server's own executable in another session: an old tracker (the service runs in session 0)
			var session uint32
			own := strings.EqualFold(image, self) &&
				windows.ProcessIdToSessionId(entry.ProcessID, &session) == nil && session != 0
			if other || own {
				// Its executable can be removed once it has exited
				if windows.TerminateProcess(process, 0) == nil {
					windows.WaitForSingleObject(process, 2000)
				}
			}
		}
		windows.CloseHandle(process)
	}
}

func runWindowTracker() error {
	path, _ := windows.UTF16PtrFromString(windowsPortPath)
	port, err := windows.CreateFile(path, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, nil, windows.OPEN_EXISTING,
		windows.FILE_FLAG_OVERLAPPED, 0)
	if err != nil {
		return fmt.Errorf("opening %s: %w", windowsPortPath, err)
	}
	t := &windowTracker{
		port:     port,
		roots:    map[uint32]bool{},
		pids:     map[uint32]bool{},
		exes:     map[uint32]string{},
		paths:    map[uint32]string{},
		sent:     map[uintptr]trackedWindow{},
		moves:    map[uintptr]pendingMove{},
		moveWake: make(chan struct{}, 1),
		icons:    map[string]string{},
		borders:  map[uintptr][4]int32{},

		adoptedPids:    map[uint32]bool{},
		adoptedWindows: map[uintptr]bool{},
	}
	return t.run()
}

// overlapped runs one read or write on the port and waits for it
func (t *windowTracker) overlapped(write bool, buf []byte) (uint32, error) {
	event, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return 0, err
	}
	defer windows.CloseHandle(event)
	ov := windows.Overlapped{HEvent: event}
	var n uint32
	if write {
		err = windows.WriteFile(t.port, buf, &n, &ov)
	} else {
		err = windows.ReadFile(t.port, buf, &n, &ov)
	}
	if err == windows.ERROR_IO_PENDING {
		err = windows.GetOverlappedResult(t.port, &ov, &n, true)
	}
	return n, err
}

func (t *windowTracker) send(line string) {
	logTracker("> %s", line)
	t.writeMu.Lock()
	defer t.writeMu.Unlock()
	data := []byte(line + "\n")
	for len(data) > 0 {
		n, err := t.overlapped(true, data)
		if err != nil {
			logTracker("write: %v", err)
			return
		}
		data = data[n:]
	}
}

// portReader handles the viewer's commands
type portReader struct{ t *windowTracker }

func (r portReader) Read(p []byte) (int, error) {
	n, err := r.t.overlapped(false, p)
	return int(n), err
}

// readCommands handles the viewer's commands. Reading fails while no viewer is connected (the virtio-serial
// driver reports ERROR_NO_SYSTEM_RESOURCES): it tries again.
func (t *windowTracker) readCommands() {
	for {
		t.readCommandsUntilError()
		// No viewer (anymore): the user's own desktop again
		t.restoreBackground()
		time.Sleep(500 * time.Millisecond)
	}
}

// hostConnected tells whether a viewer has the port open (a pending read doesn't fail when it goes)
func (t *windowTracker) hostConnected() bool {
	event, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return true
	}
	defer windows.CloseHandle(event)
	ov := windows.Overlapped{HEvent: event}
	info := make([]byte, 512)
	var n uint32
	err = windows.DeviceIoControl(t.port, ioctlPortInformation, nil, 0, &info[0], uint32(len(info)), &n, &ov)
	if err == windows.ERROR_IO_PENDING {
		err = windows.GetOverlappedResult(t.port, &ov, &n, true)
	}
	if err != nil || n < 6 {
		return true // can't tell
	}
	return info[5] != 0
}

func setDesktop(wallpaper string, color uint32) {
	element := int32(colorBackground)
	procSetSysColors.Call(1, uintptr(unsafe.Pointer(&element)), uintptr(unsafe.Pointer(&color)))
	path, _ := windows.UTF16PtrFromString(wallpaper)
	// Without SPIF_UPDATEINIFILE: for this session only
	procSystemParametersInfoW.Call(spiSetDeskWallpaper, 0, uintptr(unsafe.Pointer(path)), spifSendChange)
}

// setBackground gives the desktop one color (0xRRGGBB) and no wallpaper
func (t *windowTracker) setBackground(rgb uint32) {
	t.backgroundMu.Lock()
	defer t.backgroundMu.Unlock()
	if !t.backgroundSaved {
		buf := make([]uint16, 1024)
		procSystemParametersInfoW.Call(spiGetDeskWallpaper, uintptr(len(buf)), uintptr(unsafe.Pointer(&buf[0])), 0)
		t.savedWallpaper = windows.UTF16ToString(buf)
		color, _, _ := procGetSysColor.Call(colorBackground)
		t.savedDesktopColor = uint32(color)
		t.backgroundSaved = true
	}
	// COLORREF: 0x00BBGGRR
	setDesktop("", rgb>>16&0xff|rgb&0xff00|(rgb&0xff)<<16)
	logTracker("background %06x (saved wallpaper %q)", rgb, t.savedWallpaper)
}

func (t *windowTracker) restoreBackground() {
	t.backgroundMu.Lock()
	defer t.backgroundMu.Unlock()
	if t.backgroundSaved {
		setDesktop(t.savedWallpaper, t.savedDesktopColor)
		t.backgroundSaved = false
		logTracker("background restored")
	}
}

func (t *windowTracker) readCommandsUntilError() {
	scanner := bufio.NewScanner(portReader{t})
	for scanner.Scan() {
		logTracker("< %s", scanner.Text())
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 {
			continue
		}
		switch fields[0] {
		case "track":
			if len(fields) == 2 {
				if pid, err := strconv.ParseUint(fields[1], 10, 32); err == nil {
					t.mu.Lock()
					t.roots[uint32(pid)] = true
					t.pids[uint32(pid)] = true
					if !t.running {
						t.adoptSince = time.Now().Add(-launchLead)
					}
					t.running = true
					t.launchSince = time.Now().Add(-launchLead)
					t.launchUntil = time.Now().Add(launchGrace)
					// A new viewer knows nothing yet: tell it every window
					t.sent = map[uintptr]trackedWindow{}
					t.mu.Unlock()
					t.refreshProcesses()
					t.send("tracking " + fields[1])
				}
			}
		case "workarea":
			if len(fields) == 5 {
				var v [4]int32
				for i := range v {
					n, _ := strconv.Atoi(fields[i+1])
					v[i] = int32(n)
				}
				area := windows.Rect{Left: v[0], Top: v[1], Right: v[0] + v[2], Bottom: v[1] + v[3]}
				t.mu.Lock()
				t.workArea = area
				t.workAreaScreen = screenSize()
				t.mu.Unlock()
				procSystemParametersInfoW.Call(spiSetWorkArea, 0, uintptr(unsafe.Pointer(&area)), spifSendChange)
			}
		case "activate", "close", "minimize", "maximize", "restore":
			if len(fields) == 2 {
				if hwnd, err := strconv.ParseUint(fields[1], 16, 64); err == nil && t.isTracked(uintptr(hwnd)) {
					windowCommand(fields[0], uintptr(hwnd))
				}
			}
		case "deactivate":
			// The user went to a Linux app: the app's menus close and it isn't the foreground window anymore (the
			// desktop's shell window is: it shows nowhere)
			foreground, _, _ := procGetForegroundWindow.Call()
			if t.isTracked(foreground) {
				procPostMessageW.Call(foreground, wmCancelMode, 0, 0)
				if shell, _, _ := procGetShellWindow.Call(); shell != 0 {
					setForeground(shell, false)
				}
			}
		case "background":
			// background <RRGGBB>
			if len(fields) == 2 {
				if rgb, err := strconv.ParseUint(fields[1], 16, 32); err == nil {
					t.setBackground(uint32(rgb))
				}
			}
		case "order":
			// order <hwnd>...: the app windows as the Linux window manager stacks them, from the top down
			order := make([]uintptr, 0, len(fields)-1)
			for _, field := range fields[1:] {
				if hwnd, err := strconv.ParseUint(field, 16, 64); err == nil {
					order = append(order, uintptr(hwnd))
				}
			}
			t.mu.Lock()
			t.order = order
			t.mu.Unlock()
		case "move":
			// move <hwnd> <x> <y> <width> <height> [M]: the visible frame's new place (M: a maximized window's, which
			// stays maximized there: the host monitor its Linux window is on, when Windows' desktop spans several)
			maximized := len(fields) == 7 && fields[6] == "M"
			if len(fields) == 6 || maximized {
				hwnd, err := strconv.ParseUint(fields[1], 16, 64)
				var v [4]int
				for i := range v {
					if err == nil {
						v[i], err = strconv.Atoi(fields[i+2])
					}
				}
				if err == nil && v[2] > 0 && v[3] > 0 && t.isTracked(uintptr(hwnd)) {
					t.queueMove(uintptr(hwnd), pendingMove{windows.Rect{Left: int32(v[0]), Top: int32(v[1]),
						Right: int32(v[0] + v[2]), Bottom: int32(v[1] + v[3])}, maximized})
				}
			}
			continue // the window reports its new place by itself
		}
		procPostThreadMessageW.Call(uintptr(t.threadID), wmApp, 0, 0)
	}
	logTracker("reading the port: %v", scanner.Err())
}

func (t *windowTracker) isTracked(hwnd uintptr) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	_, ok := t.sent[hwnd]
	return ok
}

func windowCommand(command string, hwnd uintptr) {
	switch command {
	case "close":
		procPostMessageW.Call(hwnd, wmClose, 0, 0)
	case "minimize":
		procShowWindow.Call(hwnd, swMinimize)
	case "maximize":
		procShowWindow.Call(hwnd, swMaximize)
	case "restore":
		procShowWindow.Call(hwnd, swRestore)
		setForeground(hwnd, true)
	case "activate":
		setForeground(hwnd, true)
	}
}

// queueMove has mover put a window somewhere. While the Linux window manager drags it, the viewer reports every step:
// only the latest one counts (the app redraws at every size it gets).
func (t *windowTracker) queueMove(hwnd uintptr, move pendingMove) {
	t.movesMu.Lock()
	t.moves[hwnd] = move
	t.movesMu.Unlock()
	select {
	case t.moveWake <- struct{}{}:
	default:
	}
}

func (t *windowTracker) mover() {
	for range t.moveWake {
		t.movesMu.Lock()
		moves := t.moves
		t.moves = map[uintptr]pendingMove{}
		t.movesMu.Unlock()
		for hwnd, move := range moves {
			moveWindow(hwnd, move.frame, move.maximized)
		}
	}
}

type windowPlacement struct {
	Length         uint32
	Flags          uint32
	ShowCmd        uint32
	MinPosition    struct{ X, Y int32 }
	MaxPosition    struct{ X, Y int32 }
	NormalPosition windows.Rect
}

// restoredFrame is where a minimized window's visible frame goes when it's restored: Windows keeps minimized
// windows far off the screen. The placement is in work area coordinates and includes the invisible borders (as
// measured while it was shown); maximized tells it comes back maximized.
func restoredFrame(hwnd uintptr, borders [4]int32) (frame windows.Rect, maximized bool, ok bool) {
	placement := windowPlacement{Length: uint32(unsafe.Sizeof(windowPlacement{}))}
	if r, _, _ := procGetWindowPlacement.Call(hwnd, uintptr(unsafe.Pointer(&placement))); r == 0 {
		return windows.Rect{}, false, false
	}
	var area windows.Rect
	procSystemParametersInfoW.Call(spiGetWorkArea, 0, uintptr(unsafe.Pointer(&area)), 0)
	frame = placement.NormalPosition
	frame.Left += area.Left
	frame.Right += area.Left
	frame.Top += area.Top
	frame.Bottom += area.Top
	frame.Left += borders[0]
	frame.Top += borders[1]
	frame.Right -= borders[2]
	frame.Bottom -= borders[3]
	return frame, placement.Flags&wpfRestoreToMaximized != 0, frame.Right > frame.Left && frame.Bottom > frame.Top
}

// visibleFrame is a window's visible frame: without the invisible resize borders and the shadow
func visibleFrame(hwnd uintptr) windows.Rect {
	var rect windows.Rect
	if hr, _, _ := procDwmGetWindowAttribute.Call(hwnd, dwmwaExtendedFrame, uintptr(unsafe.Pointer(&rect)),
		unsafe.Sizeof(rect)); hr != 0 {
		procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&rect)))
	}
	return rect
}

type pendingMove struct {
	frame     windows.Rect
	maximized bool
}

// moveWindow gives a window's visible frame a new place and size (its invisible borders stay around it); maximized:
// a maximized window's, which stays maximized (not one that isn't maximized anymore, nor the other way round)
func moveWindow(hwnd uintptr, frame windows.Rect, maximized bool) {
	if zoomed, _, _ := procIsZoomed.Call(hwnd); (zoomed != 0) != maximized {
		return
	}
	if iconic, _, _ := procIsIconic.Call(hwnd); iconic != 0 {
		return
	}
	var outer windows.Rect
	procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&outer)))
	visible := visibleFrame(hwnd)
	left, top := visible.Left-outer.Left, visible.Top-outer.Top
	right, bottom := outer.Right-visible.Right, outer.Bottom-visible.Bottom
	x, y := frame.Left-left, frame.Top-top
	width, height := frame.Right-frame.Left+left+right, frame.Bottom-frame.Top+top+bottom
	procSetWindowPos.Call(hwnd, 0, uintptr(x), uintptr(y), uintptr(width), uintptr(height),
		swpNoZOrder|swpNoActivate|swpNoOwnerZOrder)
}

// Where WM_NCHITTEST says a point is
const (
	htCaption     = 2
	htSysMenu     = 3
	htLeft        = 10
	htBottomRight = 17
)

// moveSizeEdges are the viewer's names of the hit test's edges, from htLeft on
var moveSizeEdges = []string{"w", "e", "n", "nw", "ne", "s", "sw", "se"}

// moveSizeKind tells what Windows' move loop does from where the pointer was: "move", or the edge it resizes
func moveSizeKind(hwnd uintptr, x, y int32) string {
	var result uintptr
	point := uintptr(uint16(int16(x))) | uintptr(uint16(int16(y)))<<16
	if ok, _, _ := procSendMessageTimeoutW.Call(hwnd, wmNcHitTest, 0, point, smtoAbortIfHung, 100,
		uintptr(unsafe.Pointer(&result))); ok != 0 {
		switch hit := int16(result); {
		case hit == htCaption || hit == htSysMenu:
			return "move"
		case hit >= htLeft && hit <= htBottomRight:
			return moveSizeEdges[hit-htLeft]
		}
	}
	// Apps that start the loop from their own title bar: by the pointer's place (outside the frame: an edge)
	frame := visibleFrame(hwnd)
	edge := ""
	if y < frame.Top {
		edge = "n"
	} else if y >= frame.Bottom {
		edge = "s"
	}
	if x < frame.Left {
		edge += "w"
	} else if x >= frame.Right {
		edge += "e"
	}
	if edge == "" {
		return "move"
	}
	return edge
}

// moveSizeStarted: Windows started moving or sizing a window (its title bar or an edge is dragged). For a tracked
// one, the viewer hands the drag to the Linux window manager when the button is still down there.
func (t *windowTracker) moveSizeStarted(hwnd uintptr, x, y int32) {
	t.mu.Lock()
	window, ok := t.sent[hwnd]
	t.mu.Unlock()
	if !ok || window.popup {
		return
	}
	t.send(fmt.Sprintf("movesize %x %s", hwnd, moveSizeKind(hwnd, x, y)))
}

// Windows of the shell that stay where they are
var shellClasses = map[string]bool{"Progman": true, "WorkerW": true, "Shell_TrayWnd": true,
	"Shell_SecondaryTrayWnd": true}

func windowClass(hwnd uintptr) string {
	buf := make([]uint16, 256)
	n, _, _ := procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return windows.UTF16ToString(buf[:n])
}

// shownFrame is a window's visible frame, when it is on the screen
func shownFrame(hwnd uintptr) (windows.Rect, bool) {
	if visible, _, _ := procIsWindowVisible.Call(hwnd); visible == 0 {
		return windows.Rect{}, false
	}
	if iconic, _, _ := procIsIconic.Call(hwnd); iconic != 0 {
		return windows.Rect{}, false
	}
	var cloaked uint32
	procDwmGetWindowAttribute.Call(hwnd, dwmwaCloaked, uintptr(unsafe.Pointer(&cloaked)), 4)
	if cloaked != 0 {
		return windows.Rect{}, false
	}
	frame := visibleFrame(hwnd)
	return frame, frame.Right > frame.Left && frame.Bottom > frame.Top
}

func overlap(a, b windows.Rect) bool {
	return a.Left < b.Right && b.Left < a.Right && a.Top < b.Bottom && b.Top < a.Bottom
}

// keepOthersBelow puts other apps' windows that cover a tracked window under the tracked ones: Windows' screen is
// shared, so they would show in its Linux window. order is every window, from the top down. The foreground window
// and topmost ones (the shell's) stay.
func keepOthersBelow(order []uintptr, tracked map[uintptr]uint32, foreground uintptr) {
	lowest := -1
	frames := map[uintptr]windows.Rect{}
	for i, hwnd := range order {
		if _, ok := tracked[hwnd]; ok {
			if frame, shown := shownFrame(hwnd); shown {
				frames[hwnd] = frame
				lowest = i
			}
		}
	}
	for i := 0; i < lowest; i++ {
		hwnd := order[i]
		if _, ok := tracked[hwnd]; ok || hwnd == foreground {
			continue
		}
		frame, shown := shownFrame(hwnd)
		if !shown {
			continue
		}
		index := gwlExStyle
		exStyle, _, _ := procGetWindowLongPtrW.Call(hwnd, uintptr(index))
		if exStyle&wsExTopmost != 0 || shellClasses[windowClass(hwnd)] {
			continue
		}
		for _, below := range order[i+1 : lowest+1] {
			if trackedFrame, ok := frames[below]; ok && overlap(frame, trackedFrame) {
				logTracker("keeping %x under %x", hwnd, order[lowest])
				procSetWindowPos.Call(hwnd, order[lowest], 0, 0, 0, 0,
					swpNoMove|swpNoSize|swpNoActivate|swpNoOwnerZOrder|swpAsyncWindowPos)
				break
			}
		}
	}
}

// setForeground makes a window the foreground window, and brings it to the front (raise): Windows only lets the
// foreground's thread do that, so this joins its input for a moment
// keepOrder stacks the shown app windows as the Linux window manager does ("order"): each one that is above the one
// before it in the order goes right under that one. A window Windows just brought to the front (a new dialog) stays
// there for a moment: the viewer brings its Linux window to the front, and the order follows. Run thread only.
func (t *windowTracker) keepOrder(foreground uintptr) {
	if foreground != t.lastForeground {
		t.lastForeground, t.foregroundAt = foreground, time.Now()
	}
	t.mu.Lock()
	order := t.order
	_, foregroundTracked := t.sent[foreground]
	shown := make([]uintptr, 0, len(order))
	for _, hwnd := range order {
		if _, ok := t.sent[hwnd]; ok {
			if iconic, _, _ := procIsIconic.Call(hwnd); iconic == 0 {
				shown = append(shown, hwnd)
			}
		}
	}
	t.mu.Unlock()
	if len(shown) < 2 {
		return
	}
	if foregroundTracked && foreground != shown[0] && time.Since(t.foregroundAt) < time.Second {
		return
	}
	position := make(map[uintptr]float64, len(t.enumOrder))
	for i, hwnd := range t.enumOrder {
		position[hwnd] = float64(i)
	}
	for i := 1; i < len(shown); i++ {
		above, hwnd := shown[i-1], shown[i]
		abovePosition, ok1 := position[above]
		hwndPosition, ok2 := position[hwnd]
		if !ok1 || !ok2 || hwndPosition > abovePosition {
			continue
		}
		logTracker("stacking %x under %x", hwnd, above)
		procSetWindowPos.Call(hwnd, above, 0, 0, 0, 0, swpNoMove|swpNoSize|swpNoActivate|swpAsyncWindowPos)
		// Right under it now
		position[hwnd] = abovePosition + 0.001
	}
}

func setForeground(hwnd uintptr, raise bool) {
	foreground, _, _ := procGetForegroundWindow.Call()
	if foreground == hwnd {
		return
	}
	self := windows.GetCurrentThreadId()
	var pid uint32
	thread, _, _ := procGetWindowThreadProcess.Call(foreground, uintptr(unsafe.Pointer(&pid)))
	if thread != 0 && uint32(thread) != self {
		procAttachThreadInput.Call(uintptr(self), thread, 1)
		defer procAttachThreadInput.Call(uintptr(self), thread, 0)
	}
	if raise {
		procBringWindowToTop.Call(hwnd)
	}
	procSetForegroundWindow.Call(hwnd)
}

// refreshProcesses adds the processes the tracked ones started, and tells whether any of them still runs
func (t *windowTracker) refreshProcesses() bool {
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return true
	}
	defer windows.CloseHandle(snapshot)
	parents := map[uint32]uint32{}
	var entry windows.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		parents[entry.ProcessID] = entry.ParentProcessID
	}

	t.mu.Lock()
	defer t.mu.Unlock()
	for added := true; added; {
		added = false
		for pid, parent := range parents {
			if !t.pids[pid] && t.pids[parent] {
				t.pids[pid] = true
				added = true
			}
		}
	}
	for pid := range t.pids {
		if _, ok := parents[pid]; ok {
			return true
		}
	}
	return false
}

func (t *windowTracker) exeName(pid uint32) string {
	if name, ok := t.exes[pid]; ok {
		return name
	}
	name := "app"
	if process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid); err == nil {
		buf := make([]uint16, windows.MAX_PATH)
		size := uint32(len(buf))
		if windows.QueryFullProcessImageName(process, 0, &buf[0], &size) == nil {
			t.paths[pid] = windows.UTF16ToString(buf[:size])
			base := filepath.Base(t.paths[pid])
			name = strings.ToLower(strings.TrimSuffix(base, filepath.Ext(base)))
			name = strings.Map(func(r rune) rune {
				if r <= ' ' {
					return '-'
				}
				return r
			}, name)
		}
		windows.CloseHandle(process)
	}
	t.exes[pid] = name
	return name
}

func windowTitle(hwnd uintptr) string {
	buf := make([]uint16, 512)
	n, _, _ := procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return strings.ReplaceAll(windows.UTF16ToString(buf[:n]), "\n", " ")
}

// windowLine describes a window of a tracked process for the viewer ("" when it can't be shown), and whether it is
// a popup
func (t *windowTracker) windowLine(hwnd uintptr, pid uint32, foreground uintptr) (string, bool) {
	if visible, _, _ := procIsWindowVisible.Call(hwnd); visible == 0 {
		return "", false
	}
	var cloaked uint32
	procDwmGetWindowAttribute.Call(hwnd, dwmwaCloaked, uintptr(unsafe.Pointer(&cloaked)), 4)
	if cloaked != 0 {
		return "", false
	}
	rect := visibleFrame(hwnd)
	iconic, _, _ := procIsIconic.Call(hwnd)
	restoresMaximized := false
	if iconic != 0 {
		// Minimized: where it will be (the viewer keeps its Linux window, minimized)
		borders, known := t.borders[hwnd]
		if !known {
			index := gwlStyle
			if style, _, _ := procGetWindowLongPtrW.Call(hwnd, uintptr(index)); style&wsThickFrame != 0 {
				borders = [4]int32{7, 0, 7, 7} // Windows 10 and 11 at 100 %
			}
		}
		if restored, maximized, ok := restoredFrame(hwnd, borders); ok {
			rect, restoresMaximized = restored, maximized
		}
	} else if zoomed, _, _ := procIsZoomed.Call(hwnd); zoomed == 0 {
		var outer windows.Rect
		procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&outer)))
		t.borders[hwnd] = [4]int32{rect.Left - outer.Left, rect.Top - outer.Top, outer.Right - rect.Right,
			outer.Bottom - rect.Bottom}
	}
	width, height := rect.Right-rect.Left, rect.Bottom-rect.Top
	if width <= 0 || height <= 0 {
		return "", false
	}
	flags := "v"
	if iconic != 0 {
		flags += "m"
		if restoresMaximized {
			flags += "M"
		}
	} else if zoomed, _, _ := procIsZoomed.Call(hwnd); zoomed != 0 {
		flags += "M"
	}
	index := gwlStyle
	style, _, _ := procGetWindowLongPtrW.Call(hwnd, uintptr(index))
	// Menus, tooltips, splash screens: popups or tool windows without a title bar (both caption bits: tooltips
	// have a border, which is one of them)
	exIndex := gwlExStyle
	exStyle, _, _ := procGetWindowLongPtrW.Call(hwnd, uintptr(exIndex))
	// An app's main window drawn without Windows' title bar (FL Studio's, any VCL/Delphi form, many audio apps) is a
	// WS_POPUP too, but WS_EX_APPWINDOW asks for a taskbar button: an app window then, like Windows takes it
	popup := (style&wsPopup != 0 || exStyle&wsExToolWindow != 0) && style&wsCaption != wsCaption &&
		exStyle&wsExAppWindow == 0
	// Full screen: no title bar and exactly Windows' screen (borderless full screen, how most games do it; a
	// maximized window keeps its title bar). Not a popup then, whatever its style
	if iconic == 0 && style&wsCaption != wsCaption && rect.Left == 0 && rect.Top == 0 {
		screenWidth, _, _ := procGetSystemMetrics.Call(smCxScreen)
		screenHeight, _, _ := procGetSystemMetrics.Call(smCyScreen)
		if int32(screenWidth) == width && int32(screenHeight) == height {
			popup = false
			flags += "F"
		}
	}
	if popup {
		flags += "p"
	} else if style&wsThickFrame != 0 {
		flags += "r"
	}
	if hwnd == foreground {
		flags += "f"
	}
	owner, _, _ := procGetWindow.Call(hwnd, gwOwner)
	return fmt.Sprintf("window %x %x %d %d %d %d %s %s", hwnd, owner, rect.Left, rect.Top, width, height, flags,
		t.exeName(pid)), popup
}

// noteShown notes when the windows were first seen shown (on the tracker's first look: before it started)
func (t *windowTracker) noteShown() {
	now, first := time.Now(), t.firstShown == nil
	if first {
		t.firstShown = map[uintptr]time.Time{}
	}
	present := make(map[uintptr]bool, len(t.enumOrder))
	for _, hwnd := range t.enumOrder {
		present[hwnd] = true
		if _, seen := t.firstShown[hwnd]; seen {
			continue
		}
		if visible, _, _ := procIsWindowVisible.Call(hwnd); visible != 0 {
			if first {
				t.firstShown[hwnd] = time.Time{}
			} else {
				t.firstShown[hwnd] = now
			}
		}
	}
	for hwnd := range t.firstShown {
		if !present[hwnd] {
			delete(t.firstShown, hwnd)
		}
	}
}

// adoptStarted adopts the windows of apps started in Windows (or handed an app's start): windows shown since
// adoptSince (for adoptAfter at least) that aren't tracked, minimized or owned and would be windows of their own (not
// popups). Their processes are tracked from then on (Explorer's shell: only the window); the topmost one comes to
// the front. Called with t.mu held; tells whether it adopted any.
func (t *windowTracker) adoptStarted(current map[uintptr]uint32, foreground uintptr) bool {
	adopted := false
	for _, hwnd := range t.enumOrder {
		shown := t.firstShown[hwnd]
		if _, tracked := current[hwnd]; tracked || shown.Before(t.adoptSince) || time.Since(shown) < adoptAfter {
			continue
		}
		if owner, _, _ := procGetWindow.Call(hwnd, gwOwner); owner != 0 {
			continue
		}
		if iconic, _, _ := procIsIconic.Call(hwnd); iconic != 0 {
			continue
		}
		var pid uint32
		procGetWindowThreadProcess.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
		if line, popup := t.windowLine(hwnd, pid, foreground); line == "" || popup {
			continue
		}
		var shellPid uint32
		if shell, _, _ := procGetShellWindow.Call(); shell != 0 {
			procGetWindowThreadProcess.Call(shell, uintptr(unsafe.Pointer(&shellPid)))
		}
		if pid == shellPid {
			t.adoptedWindows[hwnd] = true
		} else if t.adoptedPids[pid] {
			continue // adopted with another of its windows just now
		} else {
			t.adoptedPids[pid] = true
		}
		logTracker("adopted window %x of process %d", hwnd, pid)
		if !adopted && hwnd != foreground {
			setForeground(hwnd, true)
		}
		adopted = true
	}
	return adopted
}

// snapshot tells the viewer what changed
func (t *windowTracker) snapshot() {
	t.mu.Lock()
	pids := make(map[uint32]bool, len(t.pids)+len(t.adoptedPids))
	for pid := range t.pids {
		pids[pid] = true
	}
	for pid := range t.adoptedPids {
		pids[pid] = true
	}
	adopted := make(map[uintptr]bool, len(t.adoptedWindows))
	for hwnd := range t.adoptedWindows {
		adopted[hwnd] = true
	}
	t.mu.Unlock()

	foreground, _, _ := procGetForegroundWindow.Call()
	current := map[uintptr]uint32{}
	t.enumPids, t.enumWindows, t.enumFound, t.enumOrder = pids, adopted, current, t.enumOrder[:0]
	procEnumWindows.Call(t.enumCallback, 0)
	t.noteShown()
	keepOthersBelow(t.enumOrder, current, foreground)
	t.keepOrder(foreground)

	var lines []string
	icons := map[uintptr]uint32{} // windows whose icons are looked for
	adoptedShown := false
	t.mu.Lock()
	launching := time.Now().Before(t.launchUntil)
	for hwnd, pid := range current {
		line, popup := t.windowLine(hwnd, pid, foreground)
		if line != "" && (t.adoptedPids[pid] || t.adoptedWindows[hwnd]) {
			adoptedShown = true
		}
		if line != "" && !popup && launching && !t.firstShown[hwnd].Before(t.launchSince) {
			// The app showed its window
			launching = false
			t.launchUntil = time.Time{}
		}
		previous, known := t.sent[hwnd]
		if line == "" {
			if known {
				delete(t.sent, hwnd)
				lines = append(lines, fmt.Sprintf("gone %x", hwnd))
			}
			continue
		}
		title := windowTitle(hwnd)
		if !known || previous.line != line {
			lines = append(lines, line)
		}
		if !known || previous.title != title {
			lines = append(lines, fmt.Sprintf("title %x %s", hwnd, title))
		}
		if !popup && !previous.icon {
			icons[hwnd] = pid
		}
		t.sent[hwnd] = trackedWindow{line: line, title: title, popup: popup, icon: previous.icon || !popup}
	}
	for hwnd := range t.sent {
		if _, ok := current[hwnd]; !ok {
			delete(t.sent, hwnd)
			lines = append(lines, fmt.Sprintf("gone %x", hwnd))
		}
	}
	for hwnd := range t.borders {
		if _, ok := current[hwnd]; !ok {
			delete(t.borders, hwnd)
		}
	}
	for hwnd := range t.adoptedWindows {
		if _, ok := current[hwnd]; !ok {
			delete(t.adoptedWindows, hwnd)
		}
	}
	t.adoptedShown = adoptedShown
	if t.running && t.adoptStarted(current, foreground) {
		// Their windows go out with the next snapshot, right away
		procPostThreadMessageW.Call(uintptr(t.threadID), wmApp, 0, 0)
	}
	t.mu.Unlock()
	for _, line := range lines {
		t.send(line)
	}
	for hwnd, pid := range icons {
		if line := t.iconLine(hwnd, pid); line != "" {
			t.send(line)
		}
	}
}

func screenSize() [2]uintptr {
	width, _, _ := procGetSystemMetrics.Call(smCxScreen)
	height, _, _ := procGetSystemMetrics.Call(smCyScreen)
	return [2]uintptr{width, height}
}

// keepWorkArea puts the viewer's work area back once Windows has a new resolution (which resets it), if it fits
func (t *windowTracker) keepWorkArea() {
	size := screenSize()
	t.mu.Lock()
	want, changed := t.workArea, size != t.workAreaScreen
	t.workAreaScreen = size
	t.mu.Unlock()
	if !changed || want.Right <= want.Left || want.Right > int32(size[0]) || want.Bottom > int32(size[1]) {
		return
	}
	logTracker("work area %v again (a new resolution: %dx%d)", want, size[0], size[1])
	procSystemParametersInfoW.Call(spiSetWorkArea, 0, uintptr(unsafe.Pointer(&want)), spifSendChange)
}

func (t *windowTracker) run() error {
	// Window events arrive on the thread that hooked them, while it pumps messages
	runtime.LockOSThread()
	t.threadID = windows.GetCurrentThreadId()
	t.enumCallback = syscall.NewCallback(func(hwnd uintptr, _ uintptr) uintptr {
		var pid uint32
		procGetWindowThreadProcess.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
		if t.enumPids[pid] || t.enumWindows[hwnd] {
			t.enumFound[hwnd] = pid
		}
		t.enumOrder = append(t.enumOrder, hwnd)
		return 1
	})
	go t.readCommands()
	go t.mover()

	pending := false
	hook := syscall.NewCallback(func(hook, event, hwnd uintptr, object, child int32, thread, time uint32) uintptr {
		if event == eventSystemMoveSizeBeg && object == objidWindow {
			// Where the pointer is now: where the drag started
			var point struct{ X, Y int32 }
			procGetCursorPos.Call(uintptr(unsafe.Pointer(&point)))
			procPostThreadMessageW.Call(uintptr(t.threadID), wmMoveSizeStarted, hwnd,
				uintptr(uint16(int16(point.X)))|uintptr(uint16(int16(point.Y)))<<16)
		}
		if object == objidWindow && child == 0 && !pending {
			// Coalesce a burst (a window being dragged reports every step)
			pending = true
			procPostThreadMessageW.Call(uintptr(t.threadID), wmApp, 0, 0)
		}
		return 0
	})
	procSetWinEventHook.Call(eventSystemForeground, eventSystemMinimizeEnd, 0, hook, 0, 0, wineventOutOfContext)
	procSetWinEventHook.Call(eventObjectCreate, eventObjectNameChange, 0, hook, 0, 0, wineventOutOfContext)
	// Processes starting and exiting, and anything the hooks miss
	procSetTimer.Call(0, 0, 250, 0)

	lastProcesses := time.Time{}
	lastPortCheck, viewerConnected := time.Time{}, false
	var m msg
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			return nil
		}
		switch m.Message {
		case wmTimer:
			if time.Since(lastPortCheck) >= time.Second {
				lastPortCheck = time.Now()
				if connected := t.hostConnected(); connected != viewerConnected {
					viewerConnected = connected
					logTracker("viewer connected: %v", connected)
					if !connected {
						// The viewer went: the user's own desktop again
						t.restoreBackground()
						t.mu.Lock()
						t.workArea = windows.Rect{}
						t.mu.Unlock()
					}
				}
				if viewerConnected {
					t.keepWorkArea()
				}
			}
			if time.Since(lastProcesses) >= 250*time.Millisecond {
				lastProcesses = time.Now()
				running := t.refreshProcesses()
				t.mu.Lock()
				// Not while an app handed to a running instance may still show up, or shows its windows
				idle := t.running && !running && !t.adoptedShown && !time.Now().Before(t.launchUntil)
				if idle {
					t.running = false
					t.roots = map[uint32]bool{}
					t.pids = map[uint32]bool{}
					t.adoptedPids = map[uint32]bool{}
					t.adoptedWindows = map[uintptr]bool{}
					t.adoptedShown = false
				}
				t.mu.Unlock()
				t.snapshot()
				if idle {
					t.send("idle")
				}
			}
		case wmApp:
			pending = false
			t.snapshot()
		case wmMoveSizeStarted:
			t.moveSizeStarted(m.WParam, int32(int16(m.LParam)), int32(int16(m.LParam>>16)))
		default:
			procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
		}
	}
}
