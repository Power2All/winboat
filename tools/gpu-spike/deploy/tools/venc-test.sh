#!/bin/bash
# venc-test.sh [case...]: GPU video encoding in the spike (ffmpeg, Vulkan Video and D3D12 video encoding), each output
# checked on the host: codec/profile, frame count, PSNR against the same test source. Cases (default: all):
#   vk-h264 vk-hevc d3d12-h264 d3d12-h264-high d3d12-hevc d3d12-hevc10
# Prints one line per case: name, exit, frames, speed, PSNR average/min; "FAIL" when something is off.
T=$(dirname "$0")
OUT=/tmp/claude-1000/venc
mkdir -p "$OUT"
declare -A CASE=(
   [vk-h264]="vulkan|1280x720|nv12|h264_vulkan|"
   [vk-hevc]="vulkan|1920x1080|nv12|hevc_vulkan|"
   [d3d12-h264]="d3d12va|1280x720|nv12|h264_d3d12va|"
   [d3d12-h264-high]="d3d12va|1920x1080|nv12|h264_d3d12va|-profile:v high"
   [d3d12-hevc]="d3d12va|1920x1080|nv12|hevc_d3d12va|"
   [d3d12-hevc10]="d3d12va|1920x1080|p010|hevc_d3d12va|-profile:v main10"
)
SECONDS_PER=3
for name in ${@:-vk-h264 vk-hevc d3d12-h264 d3d12-h264-high d3d12-hevc d3d12-hevc10}; do
   IFS='|' read -r api size fmt enc opts <<<"${CASE[$name]}"
   dev="-init_hw_device $api=hw:0 -filter_hw_device hw"
   res=$("$T/spike-run.sh" "C:\\build\\vdec\\ffmpeg.exe -hide_banner -v error -stats $dev -f lavfi -i testsrc2=s=$size:r=30 -t $SECONDS_PER -vf format=$fmt,hwupload -c:v $enc $opts -y C:\\build\\run\\venc.mp4" 240 | tr '\r' '\n')
   code=$(grep -o '^exit -\?[0-9]*' <<<"$res" | tail -1 | cut -d' ' -f2)
   speed=$(grep -o 'speed=[^ ]*' <<<"$res" | tail -1)
   rm -f "$OUT/$name.mp4"
   "$T/sp.sh" --get 'C:/build/run/venc.mp4' "$OUT/$name.mp4" 2>/dev/null
   if [ "$code" != 0 ] || [ ! -s "$OUT/$name.mp4" ]; then echo "$name: FAIL (exit $code)"; continue; fi
   info=$(ffprobe -v error -show_entries stream=codec_name,profile,nb_frames -of csv=p=0 "$OUT/$name.mp4")
   pixfmt=$([ "$fmt" = p010 ] && echo yuv420p10le || echo yuv420p)
   psnr=$(timeout 300 ffmpeg -hide_banner -i "$OUT/$name.mp4" -lavfi "[0:v]format=$pixfmt[a];testsrc2=s=$size:r=30,format=$pixfmt,trim=duration=$SECONDS_PER[b];[a][b]psnr" -f null - 2>&1)
   avg=$(grep -o 'average:[0-9.inf]*' <<<"$psnr" | cut -d: -f2)
   min=$(grep -o 'min:[0-9.inf]*' <<<"$psnr" | cut -d: -f2)
   errors=$(grep -c -iE "error|invalid|unavailable|outside the valid range" <<<"$psnr")
   verdict=ok
   awk -v m="${min:-0}" 'BEGIN { exit !(m < 35) }' && verdict=FAIL
   [ "$errors" != 0 ] && verdict=FAIL
   echo "$name: $verdict ($info, $speed, PSNR avg $avg min $min, decoder errors $errors)"
done
