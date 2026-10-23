//go:build windows

package main

// Windows auto logon for the SPICE display route. SPICE shows the console session, so Windows has to sign in there
// by itself; WinBoat's installer turns auto logon off (RDPApps.reg), because a signed-in console session blocks
// RemoteApp logons on client Windows (one session per user). The password is kept as the LSA secret
// "DefaultPassword", like Windows' own auto logon tools do, not in the registry.

import (
	"encoding/json"
	"fmt"
	"net/http"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const winlogonKey = `SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`

type lsaUnicodeString struct {
	Length        uint16
	MaximumLength uint16
	Buffer        *uint16
}

type lsaObjectAttributes struct {
	Length                   uint32
	RootDirectory            windows.Handle
	ObjectName               *lsaUnicodeString
	Attributes               uint32
	SecurityDescriptor       uintptr
	SecurityQualityOfService uintptr
}

var (
	advapi32                  = windows.NewLazySystemDLL("advapi32.dll")
	procLsaOpenPolicy         = advapi32.NewProc("LsaOpenPolicy")
	procLsaStorePrivateData   = advapi32.NewProc("LsaStorePrivateData")
	procLsaClose              = advapi32.NewProc("LsaClose")
	procLsaNtStatusToWinError = advapi32.NewProc("LsaNtStatusToWinError")
)

func newLsaString(s string) (*lsaUnicodeString, error) {
	if s == "" {
		return nil, nil
	}
	buf, err := windows.UTF16FromString(s)
	if err != nil {
		return nil, err
	}
	size := uint16((len(buf) - 1) * 2)
	return &lsaUnicodeString{Length: size, MaximumLength: size + 2, Buffer: &buf[0]}, nil
}

func lsaError(status uintptr) error {
	code, _, _ := procLsaNtStatusToWinError.Call(status)
	return windows.Errno(code)
}

// storeLsaSecret stores (or with an empty value, deletes) an LSA private data secret
func storeLsaSecret(name, value string) error {
	const policyCreateSecret = 0x00000020
	attributes := lsaObjectAttributes{Length: uint32(unsafe.Sizeof(lsaObjectAttributes{}))}
	var policy windows.Handle
	if status, _, _ := procLsaOpenPolicy.Call(0, uintptr(unsafe.Pointer(&attributes)), policyCreateSecret,
		uintptr(unsafe.Pointer(&policy))); status != 0 {
		return fmt.Errorf("LsaOpenPolicy: %w", lsaError(status))
	}
	defer procLsaClose.Call(uintptr(policy))

	key, err := newLsaString(name)
	if err != nil {
		return err
	}
	data, err := newLsaString(value)
	if err != nil {
		return err
	}
	if status, _, _ := procLsaStorePrivateData.Call(uintptr(policy), uintptr(unsafe.Pointer(key)),
		uintptr(unsafe.Pointer(data))); status != 0 {
		return fmt.Errorf("LsaStorePrivateData: %w", lsaError(status))
	}
	return nil
}

// setAutoLogon turns Windows' auto logon on for the user, or off (as WinBoat's installer leaves it)
func setAutoLogon(enable bool, username, password string) error {
	key, err := registry.OpenKey(registry.LOCAL_MACHINE, winlogonKey, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer key.Close()

	// A plain-text DefaultPassword would take precedence over the secret
	key.DeleteValue("DefaultPassword")
	key.DeleteValue("AutoLogonCount")
	if !enable {
		if err := key.SetStringValue("AutoAdminLogon", "0"); err != nil {
			return err
		}
		return storeLsaSecret("DefaultPassword", "")
	}
	if err := storeLsaSecret("DefaultPassword", password); err != nil {
		return err
	}
	for name, value := range map[string]string{"DefaultUserName": username, "DefaultDomainName": ".", "AutoAdminLogon": "1"} {
		if err := key.SetStringValue(name, value); err != nil {
			return err
		}
	}
	return nil
}

type AutoLogonRequest struct {
	Enable   bool   `json:"enable"`
	Username string `json:"username"`
	Password string `json:"password"` // the Windows password auto logon signs in with
}

func setDesktopAutoLogon(w http.ResponseWriter, r *http.Request) {
	var req AutoLogonRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.Enable && (req.Username == "" || req.Password == "") {
		http.Error(w, "username and password are required", http.StatusBadRequest)
		return
	}
	if err := setAutoLogon(req.Enable, req.Username, req.Password); err != nil {
		http.Error(w, "Setting auto logon failed: "+err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
