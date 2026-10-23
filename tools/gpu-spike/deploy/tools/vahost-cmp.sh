#!/bin/bash
# vahost-cmp.sh <clip>...: decode each clip natively with VA-API (env LIBVA_DRIVERS_PATH/LIBVA_DRIVER_NAME as set)
# and in software; compare frame by frame (framemd5 in the clip's own pixel format)
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-nvidia} NVD_BACKEND=${NVD_BACKEND:-direct}
for f in "$@"; do
  pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$f")
  sw=$(timeout 120 ffmpeg -v error -i "$f" -f framemd5 -pix_fmt $pf - | grep -v '^#')
  hw=$(timeout 120 ffmpeg -v error -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -i "$f" -f framemd5 -pix_fmt $pf - 2>/tmp/vahost-err.$$ | grep -v '^#')
  n=$(echo "$sw" | grep -c .); nh=$(echo "$hw" | grep -c .)
  diff=$(diff <(echo "$sw" | awk -F, '{print $NF}') <(echo "$hw" | awk -F, '{print $NF}') | grep -c '^>')
  echo "$(basename $f): $pf frames sw $n hw $nh differing $diff $(head -c 200 /tmp/vahost-err.$$ | tr '\n' ' ')"
  rm -f /tmp/vahost-err.$$
done
