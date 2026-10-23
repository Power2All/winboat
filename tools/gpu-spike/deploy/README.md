# GPU spike test harness

The scripts that built, deployed and tested the virtual GPU driver: `guard.sh` (runs a command with a hard time limit
and a stall timeout; everything that can hang runs under it) and `tools/`. They expect the spike's workspace in
`~/winboat-gpu-spike` (its build VM, the sources `../sync-sources.sh` prepares, and `credentials` with `WIN_USER` and
`WIN_PASSWORD` plus `ssh/` with the key the VMs trust: neither is in the repository).

- `sp.sh`: a PowerShell command, or a file copy, in the build VM or a test VM over SSH (`SP_PORT`, `SP_TIMEOUT`)
- builds and packages: `full-build.sh`, `kinstall.sh`, `win81-*.sh`, `win7-*.sh`, `new-patch.sh` (the work beyond
  `../patches/<repo>` as a diff)
- tests: `regress.sh` (video decoding/encoding, dx-test), `spike-dxtest.sh`, `win7-dxtest.sh`, `vdec-test.sh` and the
  others; results go to `tools/results/` (ignored)
- plain QEMU and libvirt test VMs: `plain-qemu.sh`, `testvm-install.sh`, `testvm-define.sh`, `testvm-prep.sh`,
  `testvm-gui.sh`, `win7-installer-test.sh`
