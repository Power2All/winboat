#!/bin/bash
# compile.sh: dx-test's shader model 6 shaders (*.hlsl here) to DXIL headers (dxil_*.h), with Microsoft's DXC release
# v1.8.2505.1 (dxc_2025_07_14.zip, shader model 6.8; the EWDK's stops at 6.7) unpacked at C:\build\tools\dxc in the
# GPU spike; the headers are kept in the repository, so building dx-test needs no DXC.
#   <file>.hlsl: the entry points and targets below (entry "-": a library)
set -euo pipefail
cd "$(dirname "$0")"
SP=~/winboat-gpu-spike/deploy/tools/sp.sh
SHADERS=(
   "mesh.hlsl MS_SOLO ms_6_5 dxil_mesh_solo"
   "mesh.hlsl AS as_6_5 dxil_mesh_as"
   "mesh.hlsl MS_PAYLOAD ms_6_5 dxil_mesh_payload"
   "mesh.hlsl PS ps_6_5 dxil_mesh_ps"
   "rt.hlsl - lib_6_3 dxil_rt_lib"
   "feedback.hlsl CS cs_6_5 dxil_feedback_cs"
   "sm68.hlsl VS vs_6_8 dxil_sm68_vs"
   "sm68.hlsl PS ps_6_8 dxil_sm68_ps"
   "sm68cmp.hlsl CS cs_6_8 dxil_sm68cmp_cs"
   "wg.hlsl - lib_6_8 dxil_wg_lib"
   "sm66atomics.hlsl CS cs_6_6 dxil_sm66atomics_cs"
)
SP_TIMEOUT=60 "$SP" 'New-Item -ItemType Directory -Force C:\build\dxil | Out-Null' >/dev/null
for f in *.hlsl; do SP_TIMEOUT=60 "$SP" --put "$f" "C:/build/dxil/$f"; done
for s in "${SHADERS[@]}"; do
   read -r file entry target name <<<"$s"
   e=""; [ "$entry" != "-" ] && e="-E $entry"
   SP_TIMEOUT=300 "$SP" "& C:\\build\\tools\\dxc\\bin\\x64\\dxc.exe -nologo -T $target $e -Vn $name -Fh C:\\build\\dxil\\$name.h C:\\build\\dxil\\$file; \"exit \$LASTEXITCODE\"" | tr -d '\r' | grep -v "^$"
   SP_TIMEOUT=60 "$SP" --get "C:/build/dxil/$name.h" "$name.h"
done
ls -la dxil_*.h | awk '{print $5, $9}'
