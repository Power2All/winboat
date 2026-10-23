#!/bin/bash
# vadec-cmp.sh <clip>...: each clip decoded through VA-API by tools/src/vadec (virglrenderer's way: the stream's
# surface format, vaGetImage in the driver's format) and in software; frames differing and the worst PSNR.
# Env LIBVA_DRIVERS_PATH picks the driver build (default: the system's)
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-nvidia} NVD_BACKEND=${NVD_BACKEND:-direct}
T=$(dirname "$0")
for f in "$@"; do
  pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$f")
  sz=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$f")
  timeout 120 ffmpeg -v error -y -i "$f" -f rawvideo -pix_fmt $pf /tmp/vadec-sw.$$
  msg=$(timeout 120 "$T/src/vadec/vadec" "$f" /tmp/vadec-hw.$$ 2>&1 | tail -1)
  timeout 120 ffmpeg -v error -f rawvideo -pix_fmt $pf -s $sz -i /tmp/vadec-sw.$$ -f rawvideo -pix_fmt $pf -s $sz \
      -i /tmp/vadec-hw.$$ -lavfi "psnr=stats_file=/tmp/vadec-psnr.$$" -f null - 2>/dev/null
  n=$(grep -c . /tmp/vadec-psnr.$$); nd=$(grep -vc 'psnr_avg:inf' /tmp/vadec-psnr.$$)
  min=$(grep -o 'psnr_avg:[0-9.inf]*' /tmp/vadec-psnr.$$ | cut -d: -f2 | sort -g | head -1)
  echo "$(basename $f): $pf $sz frames $n differing $nd worst ${min:-?} dB [$msg]"
  rm -f /tmp/vadec-*.$$
done
