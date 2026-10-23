//go:build windows

package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"os/exec"
)

// The virtual GPU driver (viogpu3d) bundled in server\drivers\viogpu3d, managed by scripts\gpu-driver.ps1. The script
// prints JSON, also for errors ({"error": ...}, exit code 1).

type GPUDriverRequest struct {
	Action string `json:"action"` // "install" or "uninstall"
	Reboot bool   `json:"reboot"` // restart Windows afterwards (both need it)
}

func runGPUDriverScript(w http.ResponseWriter, action string) bool {
	// Positional: executePowerShellScript quotes every argument, so "-Action" would arrive as a value
	output, err := executePowerShellScript(".\\scripts\\gpu-driver.ps1", true, action)
	var exitErr *exec.ExitError
	if err != nil && !(errors.As(err, &exitErr) && len(output) > 0) {
		http.Error(w, "Failed to execute script: "+err.Error(), http.StatusInternalServerError)
		return false
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
	} else {
		w.WriteHeader(http.StatusOK)
	}
	w.Write(output)
	return err == nil
}

func getGPUStatus(w http.ResponseWriter, r *http.Request) {
	runGPUDriverScript(w, "Status")
}

func setGPUDriver(w http.ResponseWriter, r *http.Request) {
	var req GPUDriverRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	var action string
	switch req.Action {
	case "install":
		action = "Install"
	case "uninstall":
		action = "Uninstall"
	default:
		http.Error(w, "Unknown action: "+req.Action, http.StatusBadRequest)
		return
	}
	if runGPUDriverScript(w, action) && req.Reboot {
		// Give the response time to reach the host
		exec.Command("shutdown", "/r", "/t", "10", "/c", "WinBoat: activating the virtual GPU driver").Start()
	}
}
