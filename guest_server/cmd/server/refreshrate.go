//go:build windows
// +build windows

package main

// The refresh rate of Windows' display, for the SPICE display route: WinBoat asks for the rate of the monitor the
// viewer's window is on, so Windows composes as many frames as that monitor shows (the GPU driver offers every mode
// at 60 to 240 Hz). Display settings belong to the user's session, so this server runs itself there
// (--refresh-rate <Hz>) to change them.

import (
	"encoding/json"
	"net/http"
	"os"
	"strconv"
	"unsafe"
)

// DEVMODEW, display fields
type devMode struct {
	DeviceName       [32]uint16
	SpecVersion      uint16
	DriverVersion    uint16
	Size             uint16
	DriverExtra      uint16
	Fields           uint32
	PositionX        int32
	PositionY        int32
	DisplayOrient    uint32
	DisplayFixedOut  uint32
	Color            int16
	Duplex           int16
	YResolution      int16
	TTOption         int16
	Collate          int16
	FormName         [32]uint16
	LogPixels        uint16
	BitsPerPel       uint32
	PelsWidth        uint32
	PelsHeight       uint32
	DisplayFlags     uint32
	DisplayFrequency uint32
	ICMMethod        uint32
	ICMIntent        uint32
	MediaType        uint32
	DitherType       uint32
	Reserved1        uint32
	Reserved2        uint32
	PanningWidth     uint32
	PanningHeight    uint32
}

const (
	enumCurrentSettings = 0xFFFFFFFF
	dmDisplayFrequency  = 0x00400000
	cdsUpdateRegistry   = 0x00000001
)

var (
	procEnumDisplaySettingsW    = taskbarUser32.NewProc("EnumDisplaySettingsW")
	procChangeDisplaySettingsEx = taskbarUser32.NewProc("ChangeDisplaySettingsExW")
)

func enumDisplaySettings(index uint32, mode *devMode) bool {
	mode.Size = uint16(unsafe.Sizeof(*mode))
	ok, _, _ := procEnumDisplaySettingsW.Call(0, uintptr(index), uintptr(unsafe.Pointer(mode)))
	return ok != 0
}

// The rates viogpu3d offers every mode at
var driverRefreshRates = []uint32{60, 75, 90, 100, 120, 144, 165, 180, 240}

// setRefreshRate runs in the user's session: the fastest rate the primary display offers at its current resolution
// that is at most maxRate (or its slowest one). Returns the rate it runs at, 0 if that can't be told.
func setRefreshRate(maxRate uint32) uint32 {
	var current devMode
	if !enumDisplaySettings(enumCurrentSettings, &current) {
		return 0
	}
	var rates []uint32
	var mode devMode
	for i := uint32(0); enumDisplaySettings(i, &mode); i++ {
		if mode.PelsWidth == current.PelsWidth && mode.PelsHeight == current.PelsHeight &&
			mode.BitsPerPel == current.BitsPerPel {
			rates = append(rates, mode.DisplayFrequency)
		}
	}
	// A custom resolution (the SPICE agent sizes the display to the viewer's monitor) isn't listed, yet the driver
	// runs it at any of its rates
	if len(rates) == 0 {
		rates = driverRefreshRates
	}
	best, slowest := uint32(0), uint32(0)
	for _, rate := range rates {
		if rate <= maxRate && rate > best {
			best = rate
		}
		if slowest == 0 || rate < slowest {
			slowest = rate
		}
	}
	if best == 0 {
		best = slowest
	}
	if best == 0 || best == current.DisplayFrequency {
		return current.DisplayFrequency
	}
	current.Fields = dmDisplayFrequency
	current.DisplayFrequency = best
	if result, _, _ := procChangeDisplaySettingsEx.Call(0, uintptr(unsafe.Pointer(&current)), 0, cdsUpdateRegistry, 0); result != 0 {
		// DISP_CHANGE_*: not changed
		var now devMode
		if enumDisplaySettings(enumCurrentSettings, &now) {
			return now.DisplayFrequency
		}
		return 0
	}
	return best
}

// handleRefreshRateArgs runs `--refresh-rate <Hz>` (setConsoleRefreshRate starts it in the user's session) and exits
// with the rate the display runs at
func handleRefreshRateArgs() {
	if len(os.Args) == 3 && os.Args[1] == "--refresh-rate" {
		rate, err := strconv.ParseUint(os.Args[2], 10, 32)
		if err != nil {
			os.Exit(0)
		}
		os.Exit(int(setRefreshRate(uint32(rate))))
	}
}

// setConsoleRefreshRate sets the console display's refresh rate (at most maxRate), from this service
func setConsoleRefreshRate(maxRate uint32) (uint32, error) {
	self, err := os.Executable()
	if err != nil {
		return 0, err
	}
	var rate uint32
	_, err = startOnConsoleDesktop(self, "--refresh-rate "+strconv.FormatUint(uint64(maxRate), 10),
		startOptions{Hidden: true, Wait: true, ExitCode: &rate})
	return rate, err
}

type DesktopRefreshRateRequest struct {
	MaxRate uint32 `json:"maxRate"` // the host monitor's rate (Hz)
}

// setDesktopRefreshRate sets the refresh rate of the console display, at most the one asked for
func setDesktopRefreshRate(w http.ResponseWriter, r *http.Request) {
	var req DesktopRefreshRateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.MaxRate < 24 || req.MaxRate > 1000 {
		http.Error(w, "maxRate must be between 24 and 1000", http.StatusBadRequest)
		return
	}
	rate, err := setConsoleRefreshRate(req.MaxRate)
	if err != nil {
		writeDesktopError(w, "Setting the refresh rate", err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]uint32{"rate": rate})
}
