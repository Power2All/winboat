#!/bin/bash
# vkfilters-test.sh [case...]: ffmpeg's Vulkan filters in the spike (Venus: the host GPU), each output against the
# software filter's on the host. A case: gpu filter | software filter | expectation (exact, psnr:N, runs, or native:
# byte for byte the same Vulkan filter's output on the host's own driver, where software differs by design).
# Not in the default set: chromaber_vulkan (ffmpeg crashes with it on the host's own driver too)
T=~/winboat-gpu-spike/deploy/tools
OUT=/tmp/claude-1000/vkf; mkdir -p "$OUT"
SRC="testsrc2=s=640x360:r=30"
declare -A CASE=(
   [hflip]="hflip_vulkan|hflip|exact"
   [vflip]="vflip_vulkan|vflip|exact"
   [transpose]="transpose_vulkan=dir=clock|transpose=dir=clock|exact"
   [scale]="scale_vulkan=w=1280:h=720|scale=1280:720:flags=bilinear|psnr:35"
   [gblur]="gblur_vulkan=sigma=2|gblur=sigma=2|psnr:35"
   [avgblur]="avgblur_vulkan=sizeX=3:sizeY=3|avgblur=sizeX=3:sizeY=3|psnr:35"
   [bwdif]="bwdif_vulkan|bwdif|native"
   [nlmeans]="nlmeans_vulkan|null|runs"
   [chromaber]="chromaber_vulkan=dist_x=2|null|runs"
   [libplacebo]="libplacebo=w=960:h=540|scale=960:540|psnr:30"
)
for name in ${@:-hflip vflip transpose scale gblur avgblur bwdif nlmeans libplacebo}; do
   IFS='|' read -r gpu cpu expect <<<"${CASE[$name]}"
   pre=""; [ "$name" = bwdif ] && pre="setfield=tff,"
   res=$("$T/spike-run.sh" "C:\\build\\vdec\\ffmpeg.exe -hide_banner -v error -init_hw_device vulkan=vk -filter_hw_device vk -f lavfi -i $SRC -frames:v 30 -vf \"${pre}format=yuv420p,hwupload,$gpu,hwdownload,format=yuv420p\" -f rawvideo -y C:\\build\\run\\vkf.yuv" 120 | tr '\r' '\n')
   code=$(grep -o '^exit -\?[0-9]*' <<<"$res" | tail -1 | cut -d' ' -f2)
   rm -f "$OUT/gpu.yuv"; SP_TIMEOUT=120 "$T/sp.sh" --get C:/build/run/vkf.yuv "$OUT/gpu.yuv" 2>/dev/null
   if [ "$code" != 0 ] || [ ! -s "$OUT/gpu.yuv" ]; then echo "$name: FAIL (exit $code) $(grep -v '^exit' <<<"$res" | grep -v 'vkd3d\|fixme' | head -2 | tr '\n' ' ')"; continue; fi
   if [ "$expect" = native ]; then
      timeout 120 ffmpeg -v error -init_hw_device vulkan=vk -filter_hw_device vk -f lavfi -i $SRC -frames:v 30 -vf "${pre}format=yuv420p,hwupload,$gpu,hwdownload,format=yuv420p" -f rawvideo -y "$OUT/cpu.yuv"
      cmp -s "$OUT/gpu.yuv" "$OUT/cpu.yuv" && v=ok || v=FAIL
      echo "$name: $v (byte for byte against the host's own driver)"; continue
   fi
   timeout 120 ffmpeg -v error -f lavfi -i $SRC -frames:v 30 -vf "${pre}format=yuv420p,$cpu,format=yuv420p" -f rawvideo -y "$OUT/cpu.yuv"
   size=$(timeout 60 ffmpeg -v error -f lavfi -i $SRC -frames:v 1 -vf "${pre}format=yuv420p,$cpu" -f null - -show_entries 2>/dev/null; timeout 60 ffprobe -v error -f lavfi -i "$SRC,${pre}format=yuv420p,$cpu" -show_entries stream=width,height -of csv=s=x:p=0 2>/dev/null | head -1)
   g=$(stat -c %s "$OUT/gpu.yuv"); c=$(stat -c %s "$OUT/cpu.yuv")
   if [ "$expect" = runs ]; then
      nz=$(od -An -tu1 -N100000 -j$((g / 3)) "$OUT/gpu.yuv" | tr -s ' ' '\n' | sort -u | wc -l)
      [ "$g" = "$c" ] && [ "$nz" -gt 10 ] && v=ok || v=FAIL
      echo "$name: $v (runs: $g bytes, $nz distinct byte values in a sample)"; continue
   fi
   [ "$g" != "$c" ] && { echo "$name: FAIL (sizes gpu $g, cpu $c)"; continue; }
   p=$(timeout 300 ffmpeg -hide_banner -f rawvideo -pix_fmt yuv420p -s $size -i "$OUT/gpu.yuv" -f rawvideo -pix_fmt yuv420p -s $size -i "$OUT/cpu.yuv" -lavfi psnr -f null - 2>&1 | grep -o 'average:[0-9.inf]*' | cut -d: -f2)
   if [ "$expect" = exact ]; then [ "$p" = inf ] && v=ok || v=FAIL
   else min=${expect#psnr:}; awk -v p="$p" -v m="$min" 'BEGIN { exit !(p == "inf" || p + 0 >= m) }' && v=ok || v=FAIL; fi
   echo "$name: $v ($size, PSNR vs software $p, expected $expect)"
done
