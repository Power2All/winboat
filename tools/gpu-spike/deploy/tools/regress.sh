#!/bin/bash
# regress.sh: decoding on every route, the AMF cases, encoding, dx-test in the spike (one line per result)
T=$(dirname "$0")
for hw in d3d11va d3d12va vulkan; do echo "== $hw"; "$T/vdec-test.sh" $hw; done
# VC-1 (the FATE suite's clips: simple/main, advanced progressive and interlaced; no Vulkan Video/D3D12 route)
for hw in d3d11va dxva2; do echo "== vc1 $hw"; "$T/vdec-test.sh" $hw SMM0015.rcv SA00040.vc1 SA00050.vc1 SA10091.vc1 SA10143.vc1 SA20021.vc1 ilaced_twomv.vc1; done
# HEVC range extensions (4:4:4 8/10/12-bit, 4:2:0 12-bit: FATE's RExt clips, x265 encodes) on d3d11va, with
# src/ffmpeg-rext's ffmpeg (upstream's DXVA code has no RExt GUIDs)
echo "== hevc rext d3d11va"; VDEC_FFMPEG=ffmpeg-rext.exe "$T/vdec-test.sh" d3d11va QMATRIX_A_RExt_Sony_1.bit IPCM_B_RExt_NEC.bit PERSIST_RPARAM_A_RExt_Sony_1.bit SAO_A_RExt_MediaTek_1.bit x265_444_8.hevc x265_444_10.hevc x265_444_12.hevc x265_420_12.hevc
echo "== amf"; "$T/amf-test.sh" h264-cqp h264-cbr hevc-cqp hevc-cbr hevc-high-tier hevc-720p-odd h264-bgr0 hevc-rgb0 h264-argb-odd hevc-rgba-sd hevc-x2bgr10
echo "== venc"; "$T/venc-test.sh" 2>&1 | tail -12
echo "== dx-test"; "$T/spike-dxtest.sh" 2>&1 | tail -4
