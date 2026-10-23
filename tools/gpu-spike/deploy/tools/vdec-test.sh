#!/bin/bash
# vdec-test.sh [hwaccel] [clip...]: GPU video decoding in the spike (ffmpeg -hwaccel vulkan|d3d12va|d3d11va, default
# vulkan), each clip's frames compared on the host with libavcodec's (same pixel format). Clips are C:\build\vdec's
# (default: all of them). One line per clip: frames, speed, frames that differ. VDEC_FFMPEG: another ffmpeg in
# C:\build\vdec (ffmpeg-rext.exe: src/ffmpeg-rext's, for HEVC's range extensions on d3d11va)
T=$(dirname "$0")
OUT=/tmp/claude-1000/vdec; mkdir -p "$OUT"
HW=${1:-vulkan}; shift
for clip in ${@:-h264-base.mp4 h264.mp4 h264-1080.mp4 hevc.mp4 hevc-sl.mp4 hevc10.mp4 hevc-4k.mp4 vp9.webm vp9-10.webm av1.mp4 av1-10.mp4 av1-1080.mp4 av1-grain.mp4 av1-grain-10.mp4 i-cama1-sony-c.h264 i-capama3-sand-f.h264 i-cvfi1-sony-d.h264 i-cvma1-toshiba-b.h264 i-cvpa1-toshiba-b.h264}; do
   [ -s "$OUT/$clip" ] || "$T/sp.sh" --get "C:/build/vdec/$clip" "$OUT/$clip" 2>/dev/null
   pf=$(ffprobe -v error -select_streams v -show_entries stream=pix_fmt -of csv=p=0 "$OUT/$clip")
   # 4:4:4 and 12-bit (HEVC range extensions) compared in their own format; else NV12/P010
   case $pf in yuv444p*|yuv420p12le) fmt=$pf ;; *10*) fmt=p010le ;; *) fmt=nv12 ;; esac
   res=$("$T/spike-run.sh" "${VDEC_PRE}C:\\build\\vdec\\${VDEC_FFMPEG:-ffmpeg.exe} -hide_banner -v error -stats ${VDEC_IN}-hwaccel $HW -i C:\\build\\vdec\\$clip -f rawvideo -pix_fmt $fmt -y C:\\build\\run\\hw.yuv" 300 | tr '\r' '\n')
   code=$(grep -o '^exit -\?[0-9]*' <<<"$res" | tail -1 | cut -d' ' -f2)
   speed=$(grep -o 'speed=[^ ]*' <<<"$res" | tail -1)
   rm -f "$OUT/hw.yuv"; "$T/sp.sh" --get C:/build/run/hw.yuv "$OUT/hw.yuv" 2>/dev/null
   [ "$code" != 0 ] && { echo "$clip: FAIL (exit $code) $(grep -v '^exit\|frame=' <<<"$res" | head -3 | tr '\n' ' ')"; continue; }
   size=$(ffprobe -v error -select_streams v -show_entries stream=width,height -of csv=s=x:p=0 "$OUT/$clip")
   ffmpeg -hide_banner -v error -i "$OUT/$clip" -f rawvideo -pix_fmt $fmt -y "$OUT/sw.yuv"
   n=$(( $(stat -c %s "$OUT/sw.yuv") )); m=$(( $(stat -c %s "$OUT/hw.yuv") ))
   diff=$(timeout 300 ffmpeg -hide_banner -f rawvideo -pix_fmt $fmt -s $size -i "$OUT/hw.yuv" -f rawvideo -pix_fmt $fmt -s $size -i "$OUT/sw.yuv" -lavfi psnr=stats_file=- -f null - 2>/dev/null | grep -vc 'psnr_avg:inf')
   [ "$n" = "$m" ] && [ "$diff" = 0 ] && v=ok || v=FAIL
   echo "$clip: $v ($fmt $size, $speed, bytes hw $m sw $n, frames differing $diff)"
done
