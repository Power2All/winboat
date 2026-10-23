//go:build windows
// +build windows

package main

// The window tracker's icons: the viewer gives every Linux window the icon of its app (the taskbar shows it), as an
// "icon <hwnd> <width> <height> <base64 RGBA>" line. The executable's icon at 64 px is the sharpest; windows of
// executables without one get their own (usually 32 px).

import (
	"encoding/base64"
	"fmt"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	wiGdi32               = windows.NewLazySystemDLL("gdi32.dll")
	procGetIconInfo       = wtUser32.NewProc("GetIconInfo")
	procDestroyIcon       = wtUser32.NewProc("DestroyIcon")
	procGetClassLongPtrW  = wtUser32.NewProc("GetClassLongPtrW")
	procGetDC             = wtUser32.NewProc("GetDC")
	procReleaseDC         = wtUser32.NewProc("ReleaseDC")
	procGetObjectW        = wiGdi32.NewProc("GetObjectW")
	procGetDIBits         = wiGdi32.NewProc("GetDIBits")
	procDeleteObject      = wiGdi32.NewProc("DeleteObject")
	procSHDefExtractIconW = windows.NewLazySystemDLL("shell32.dll").NewProc("SHDefExtractIconW")
)

const (
	wmGetIcon    = 0x007F
	iconBig      = 1
	gclpHIcon    = -14
	dibRGBColors = 0
	// The size asked of executables' icons
	exeIconSize = 64
)

type iconInfo struct {
	Icon     int32
	HotspotX uint32
	HotspotY uint32
	Mask     uintptr
	Color    uintptr
}

type bitmap struct {
	Type       int32
	Width      int32
	Height     int32
	WidthBytes int32
	Planes     uint16
	BitsPixel  uint16
	Bits       uintptr
}

// BITMAPINFO with room for a color table
type bitmapInfo struct {
	Size          uint32
	Width         int32
	Height        int32
	Planes        uint16
	BitCount      uint16
	Compression   uint32
	SizeImage     uint32
	XPelsPerMeter int32
	YPelsPerMeter int32
	ClrUsed       uint32
	ClrImportant  uint32
	Colors        [256]uint32
}

// bitmapPixels reads a bitmap as 32-bit BGRA rows, from the top down
func bitmapPixels(dc, handle uintptr, width, height int32) []byte {
	info := bitmapInfo{Width: width, Height: -height, Planes: 1, BitCount: 32}
	info.Size = uint32(unsafe.Offsetof(info.Colors))
	pixels := make([]byte, width*height*4)
	if lines, _, _ := procGetDIBits.Call(dc, handle, 0, uintptr(height), uintptr(unsafe.Pointer(&pixels[0])),
		uintptr(unsafe.Pointer(&info)), dibRGBColors); lines == 0 {
		return nil
	}
	return pixels
}

// iconRGBA reads an icon's picture as RGBA
func iconRGBA(icon uintptr) (int32, int32, []byte) {
	var info iconInfo
	if ok, _, _ := procGetIconInfo.Call(icon, uintptr(unsafe.Pointer(&info))); ok == 0 {
		return 0, 0, nil
	}
	defer procDeleteObject.Call(info.Mask)
	if info.Color == 0 {
		return 0, 0, nil // black and white
	}
	defer procDeleteObject.Call(info.Color)
	var bm bitmap
	procGetObjectW.Call(info.Color, unsafe.Sizeof(bm), uintptr(unsafe.Pointer(&bm)))
	width, height := bm.Width, bm.Height
	if width <= 0 || height <= 0 || width > 256 || height > 256 {
		return 0, 0, nil
	}
	dc, _, _ := procGetDC.Call(0)
	defer procReleaseDC.Call(0, dc)
	pixels := bitmapPixels(dc, info.Color, width, height)
	if pixels == nil {
		return 0, 0, nil
	}
	alpha := false
	for i := 3; i < len(pixels); i += 4 {
		if pixels[i] != 0 {
			alpha = true
			break
		}
	}
	if !alpha {
		// An icon without alpha: its mask tells what is transparent (white)
		mask := bitmapPixels(dc, info.Mask, width, height)
		for i := 3; i < len(pixels); i += 4 {
			if mask == nil || mask[i-1] == 0 {
				pixels[i] = 255
			}
		}
	}
	for i := 0; i < len(pixels); i += 4 {
		pixels[i], pixels[i+2] = pixels[i+2], pixels[i]
	}
	return width, height, pixels
}

func iconData(icon uintptr) string {
	width, height, rgba := iconRGBA(icon)
	if rgba == nil {
		return ""
	}
	return fmt.Sprintf("%d %d %s", width, height, base64.StdEncoding.EncodeToString(rgba))
}

// windowIcon is the icon a window shows (not to be destroyed)
func windowIcon(hwnd uintptr) uintptr {
	var icon uintptr
	if ok, _, _ := procSendMessageTimeoutW.Call(hwnd, wmGetIcon, iconBig, 0, smtoAbortIfHung, 100,
		uintptr(unsafe.Pointer(&icon))); ok != 0 && icon != 0 {
		return icon
	}
	index := gclpHIcon
	icon, _, _ = procGetClassLongPtrW.Call(hwnd, uintptr(index))
	return icon
}

// iconLine is a window's "icon" line ("" without an icon)
func (t *windowTracker) iconLine(hwnd uintptr, pid uint32) string {
	t.mu.Lock()
	path := t.paths[pid]
	data, cached := t.icons[path]
	t.mu.Unlock()
	if !cached && path != "" {
		file, _ := windows.UTF16PtrFromString(path)
		var icon uintptr
		if hr, _, _ := procSHDefExtractIconW.Call(uintptr(unsafe.Pointer(file)), 0, 0, uintptr(unsafe.Pointer(&icon)),
			0, exeIconSize); hr == 0 && icon != 0 {
			data = iconData(icon)
			procDestroyIcon.Call(icon)
		}
		t.mu.Lock()
		t.icons[path] = data
		t.mu.Unlock()
	}
	if data == "" {
		data = iconData(windowIcon(hwnd))
	}
	if data == "" {
		return ""
	}
	return fmt.Sprintf("icon %x %s", hwnd, data)
}
