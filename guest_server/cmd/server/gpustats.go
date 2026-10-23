//go:build windows
// +build windows

package main

// What the virtual GPU driver (viogpu3d) sent the host since it started, for WinBoat's metrics: it counts its
// commands and their bytes, and the texture and buffer data the host copied (kvm-guest-drivers-windows patch 0010,
// the VIOGPU_GET_STATS escape). Other GPUs, and older viogpu3d builds, have none.

import (
	"encoding/binary"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	gsGdi32                    = windows.NewLazySystemDLL("gdi32.dll")
	procD3DKMTEnumAdapters2    = gsGdi32.NewProc("D3DKMTEnumAdapters2")
	procD3DKMTQueryAdapterInfo = gsGdi32.NewProc("D3DKMTQueryAdapterInfo")
	procD3DKMTEscape           = gsGdi32.NewProc("D3DKMTEscape")
	procD3DKMTCloseAdapter     = gsGdi32.NewProc("D3DKMTCloseAdapter")
)

const (
	kmtqaiTypeUmDriverPrivate = 0
	d3dkmtEscapeDriverPrivate = 0
	// shared/viogpum.h
	viogpuIam      = 0x56696f475055
	viogpuGetStats = 0x003
)

// GPUStats: totals since the driver started (WinBoat makes rates of them)
type GPUStats struct {
	Commands      uint64 `json:"commands"`      // commands on the virtio GPU's control queue
	CommandBytes  uint64 `json:"commandBytes"`  // their bytes, with the data they carry (3D command streams)
	UploadBytes   uint64 `json:"uploadBytes"`   // texture and buffer data the host copied from Windows' memory
	DownloadBytes uint64 `json:"downloadBytes"` // and into it
}

// D3DKMT_ADAPTERINFO
type kmtAdapterInfo struct {
	Adapter        uint32
	LuidLow        uint32
	LuidHigh       int32
	NumOfSources   uint32
	PreciseRegions int32
}

// D3DKMT_ENUMADAPTERS2
type kmtEnumAdapters2 struct {
	NumAdapters uint32
	Adapters    *kmtAdapterInfo
}

// D3DKMT_QUERYADAPTERINFO
type kmtQueryAdapterInfo struct {
	Adapter  uint32
	Type     uint32
	Data     unsafe.Pointer
	DataSize uint32
}

// D3DKMT_ESCAPE
type kmtEscape struct {
	Adapter  uint32
	Device   uint32
	Type     uint32
	Flags    uint32
	Data     unsafe.Pointer
	DataSize uint32
	Context  uint32
}

// virtioGPUStats reads the counters of the first viogpu3d adapter (nil without one)
func virtioGPUStats() *GPUStats {
	var enum kmtEnumAdapters2
	if status, _, _ := procD3DKMTEnumAdapters2.Call(uintptr(unsafe.Pointer(&enum))); status != 0 || enum.NumAdapters == 0 {
		return nil
	}
	adapters := make([]kmtAdapterInfo, enum.NumAdapters)
	enum.Adapters = &adapters[0]
	if status, _, _ := procD3DKMTEnumAdapters2.Call(uintptr(unsafe.Pointer(&enum))); status != 0 {
		return nil
	}
	var stats *GPUStats
	for _, adapter := range adapters[:enum.NumAdapters] {
		if stats == nil && isVioGPU(adapter.Adapter) {
			stats = vioGPUStats(adapter.Adapter)
		}
		close := struct{ Adapter uint32 }{adapter.Adapter}
		procD3DKMTCloseAdapter.Call(uintptr(unsafe.Pointer(&close)))
	}
	return stats
}

// isVioGPU: the adapter's driver is viogpu (VIOGPU_ADAPTERINFO.IamVioGPU), so it understands viogpu's escapes
func isVioGPU(adapter uint32) bool {
	info := make([]byte, 64)
	query := kmtQueryAdapterInfo{Adapter: adapter, Type: kmtqaiTypeUmDriverPrivate, Data: unsafe.Pointer(&info[0]),
		DataSize: uint32(len(info))}
	if status, _, _ := procD3DKMTQueryAdapterInfo.Call(uintptr(unsafe.Pointer(&query))); status != 0 {
		return false
	}
	return binary.LittleEndian.Uint64(info) == viogpuIam
}

func vioGPUStats(adapter uint32) *GPUStats {
	// VIOGPU_ESCAPE (packed): USHORT Type, USHORT DataLength, then the request (VIOGPU_STATS_REQ: 4 UINT64)
	data := make([]byte, 4+32)
	binary.LittleEndian.PutUint16(data[0:], viogpuGetStats)
	binary.LittleEndian.PutUint16(data[2:], 32)
	escape := kmtEscape{Adapter: adapter, Type: d3dkmtEscapeDriverPrivate, Data: unsafe.Pointer(&data[0]),
		DataSize: uint32(len(data))}
	if status, _, _ := procD3DKMTEscape.Call(uintptr(unsafe.Pointer(&escape))); status != 0 {
		return nil // a driver without the counters
	}
	return &GPUStats{
		Commands:      binary.LittleEndian.Uint64(data[4:]),
		CommandBytes:  binary.LittleEndian.Uint64(data[12:]),
		UploadBytes:   binary.LittleEndian.Uint64(data[20:]),
		DownloadBytes: binary.LittleEndian.Uint64(data[28:]),
	}
}
