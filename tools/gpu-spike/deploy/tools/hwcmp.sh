#!/bin/bash
# hwcmp.sh <hwaccel> <clip>...: frames differing from software and the worst PSNR (hwaccel: vaapi, cuda)
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-nvidia} NVD_BACKEND=${NVD_BACKEND:-direct}
hw=$1; shift
dev=; [ $hw = vaapi ] && dev="-hwaccel_device /dev/dri/renderD128"
for f in "$@"; do
  pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$f")
  timeout 120 ffmpeg -v error -y -i "$f" -f rawvideo -pix_fmt $pf /tmp/hwcmp-sw.$$ 
  timeout 120 ffmpeg -v error -y -hwaccel $hw $dev -i "$f" -f rawvideo -pix_fmt $pf /tmp/hwcmp-hw.$$ 2>/tmp/hwcmp-err.$$
  sz=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$f")
  r=$(timeout 120 ffmpeg -v info -f rawvideo -pix_fmt $pf -s $sz -i /tmp/hwcmp-sw.$$ -f rawvideo -pix_fmt $pf -s $sz -i /tmp/hwcmp-hw.$$ \
      -lavfi "psnr=stats_file=/tmp/hwcmp-psnr.$$" -f null - 2>&1 | grep -o 'PSNR.*' )
  nd=$(grep -vc 'psnr_avg:inf' /tmp/hwcmp-psnr.$$); n=$(grep -c . /tmp/hwcmp-psnr.$$)
  min=$(grep -o 'psnr_avg:[0-9.inf]*' /tmp/hwcmp-psnr.$$ | cut -d: -f2 | sort -g | head -1)
  echo "$(basename $f) [$hw]: $pf frames $n differing $nd worst $min dB | $r $(head -c 150 /tmp/hwcmp-err.$$ | tr '\n' ' ')"
  rm -f /tmp/hwcmp-*.$$
done
