//go:build windows

package main

// Windows' taskbar on the console desktop, for the SPICE app window mode: an app WinBoat starts fills its window
// without the taskbar ("Windows Desktop" shows it again). The taskbar belongs to the user's session, so this server
// runs itself there (--taskbar hide|show) to change it.

import (
	"os"
	"unsafe"

	"golang.org/x/sys/windows"
)

const (
	abmSetState    = 0x0A
	absAutoHide    = 0x01
	absAlwaysOnTop = 0x02
)

type appBarData struct {
	CbSize          uint32
	HWnd            windows.HWND
	CallbackMessage uint32
	Edge            uint32
	Rc              windows.Rect
	LParam          uintptr
}

var (
	taskbarUser32       = windows.NewLazySystemDLL("user32.dll")
	procFindWindowW     = taskbarUser32.NewProc("FindWindowW")
	procShowWindow      = taskbarUser32.NewProc("ShowWindow")
	procSHAppBarMessage = windows.NewLazySystemDLL("shell32.dll").NewProc("SHAppBarMessage")
)

// setTaskbar runs in the user's session. Hidden: auto hide (so maximized windows cover the whole screen, also after
// Explorer shows the bar again on a resolution change) and the bar's window hidden too; visible: back to normal.
func setTaskbar(visible bool) {
	for _, class := range []string{"Shell_TrayWnd", "Shell_SecondaryTrayWnd"} {
		name, _ := windows.UTF16PtrFromString(class)
		hwnd, _, _ := procFindWindowW.Call(uintptr(unsafe.Pointer(name)), 0)
		if hwnd == 0 {
			continue
		}
		if class == "Shell_TrayWnd" {
			data := appBarData{HWnd: windows.HWND(hwnd), LParam: absAutoHide}
			if visible {
				data.LParam = absAlwaysOnTop
			}
			data.CbSize = uint32(unsafe.Sizeof(data))
			procSHAppBarMessage.Call(abmSetState, uintptr(unsafe.Pointer(&data)))
		}
		show := uintptr(windows.SW_HIDE)
		if visible {
			show = windows.SW_SHOW
		}
		procShowWindow.Call(hwnd, show)
	}
}

// handleTaskbarArgs runs `--taskbar hide|show` (setConsoleTaskbar starts it in the user's session) and exits
func handleTaskbarArgs() {
	if len(os.Args) == 3 && os.Args[1] == "--taskbar" {
		setTaskbar(os.Args[2] == "show")
		os.Exit(0)
	}
}

// setConsoleTaskbar shows or hides the console user's taskbar, from this service
func setConsoleTaskbar(visible bool) error {
	self, err := os.Executable()
	if err != nil {
		return err
	}
	arg := "hide"
	if visible {
		arg = "show"
	}
	_, err = startOnConsoleDesktop(self, "--taskbar "+arg, startOptions{Hidden: true, Wait: true})
	return err
}
