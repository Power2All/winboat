#!/bin/bash
# amf-test.sh [case...]: the AMF runtime (amfrt64.dll in System32) with Topaz Video AI's ffmpeg and Topaz's own AMF
# encoder settings (models/video-encoders.json), each output checked on the host: codec/profile, frames, PSNR against
# the test source, decoder errors. A case: size|container|options[|source filters|reference conversion (RGB input: the
# matrix the runtime picks)]. Cases (default: the first line):
#   h264-cqp h264-cbr hevc-cqp hevc-cbr hevc-high-tier hevc-720p-odd
#   h264-bgr0 hevc-rgb0 h264-argb-odd hevc-rgba-sd hevc-x2bgr10
T=$(dirname "$0")
OUT=/tmp/claude-1000/amf; mkdir -p "$OUT"
FF='"C:\Program Files\Topaz Labs LLC\Topaz Video AI\ffmpeg.exe"'
declare -A CASE=(
   [h264-cqp]="1920x1080|mp4|-c:v h264_amf -profile:v high -pix_fmt yuv420p -g 30 -b:v 0 -quality 0 -rc cqp -qp_i 19 -qp_p 20 -qp_b 20"
   [h264-cbr]="1920x1080|mov|-c:v h264_amf -profile:v high -pix_fmt yuv420p -g 30 -b:v 20M"
   [hevc-cqp]="1920x1080|mp4|-c:v hevc_amf -profile:v main -profile_tier main -tag:v hvc1 -g 30 -b:v 0 -quality 0 -rc cqp -qp_i 19 -qp_p 20"
   [hevc-cbr]="1920x1080|mkv|-c:v hevc_amf -profile:v main -profile_tier main -g 30 -b:v 20M"
   [hevc-high-tier]="3840x2160|mp4|-c:v hevc_amf -profile:v main -profile_tier high -tag:v hvc1 -g 30 -b:v 0 -quality 0 -rc cqp -qp_i 19 -qp_p 20"
   [hevc-cbr-mp4]="1920x1080|mp4|-c:v hevc_amf -profile:v main -profile_tier main -g 30 -b:v 20M"
   [h264-cbr-mp4]="1920x1080|mp4|-c:v h264_amf -profile:v high -pix_fmt yuv420p -g 30 -b:v 20M"
   [hevc-720p-odd]="1278x718|mp4|-c:v hevc_amf -profile:v main -g 30 -b:v 8M"
   [h264-bgr0]="1920x1080|mp4|-c:v h264_amf -profile:v high -g 30 -rc cqp -qp_i 19 -qp_p 20|format=bgr0|out_color_matrix=bt709"
   [hevc-rgb0]="1920x1080|mp4|-c:v hevc_amf -profile:v main -g 30 -rc cqp -qp_i 19 -qp_p 20|format=rgb0|out_color_matrix=bt709"
   [h264-argb-odd]="1279x719|mp4|-c:v h264_amf -g 30 -b:v 8M|format=argb|out_color_matrix=bt601"
   [hevc-rgba-sd]="720x480|mkv|-c:v hevc_amf -g 30 -rc cqp -qp_i 19 -qp_p 20|format=rgba|out_color_matrix=bt601"
   [hevc-x2bgr10]="1920x1080|mkv|-c:v hevc_amf -profile:v main10 -g 30 -rc cqp -qp_i 19 -qp_p 20|format=x2bgr10le|out_color_matrix=bt709|yuv420p10le"
)
for name in ${@:-h264-cqp h264-cbr hevc-cqp hevc-cbr hevc-high-tier hevc-720p-odd}; do
   IFS='|' read -r size ext opts src ref fmt <<<"${CASE[$name]}"
   fmt=${fmt:-yuv420p}
   # the reference untagged like the encoded stream (no VUI), or ffmpeg converts between their matrices
   ref=${ref:+,scale=$ref:out_range=tv,setparams=colorspace=unknown}
   res=$("$T/spike-run.sh" "$FF -hide_banner -v error -stats -f lavfi -i testsrc2=s=$size:r=30${src:+,$src} -t 3 $opts -y C:\\build\\run\\amf.$ext" 240 | tr '\r' '\n')
   code=$(grep -o '^exit -\?[0-9]*' <<<"$res" | tail -1 | cut -d' ' -f2)
   speed=$(grep -o 'speed=[^ ]*' <<<"$res" | tail -1)
   rm -f "$OUT/$name.$ext"
   "$T/sp.sh" --get "C:/build/run/amf.$ext" "$OUT/$name.$ext" 2>/dev/null
   if [ "$code" != 0 ] || [ ! -s "$OUT/$name.$ext" ]; then echo "$name: FAIL (exit $code) $(grep -iv 'frame=\|^exit' <<<"$res" | head -2 | tr '\n' ' ')"; continue; fi
   info=$(ffprobe -v error -select_streams v -show_entries stream=codec_name,profile,pix_fmt,width,height,nb_frames -of csv=p=0 "$OUT/$name.$ext")
   psnr=$(timeout 300 ffmpeg -hide_banner -i "$OUT/$name.$ext" -lavfi "[0:v]settb=1/30,setpts=N,format=$fmt[a];testsrc2=s=$size:r=30${src:+,$src}$ref,format=$fmt,trim=duration=3,settb=1/30,setpts=N[b];[a][b]psnr" -f null - 2>&1)
   avg=$(grep -o 'average:[0-9.inf]*' <<<"$psnr" | cut -d: -f2)
   min=$(grep -o 'min:[0-9.inf]*' <<<"$psnr" | cut -d: -f2)
   errors=$(grep -c -iE "error|invalid|does not exist|out of range" <<<"$psnr")
   v=ok
   awk -v m="${min:-0}" 'BEGIN { exit !(m < 35) }' && v=FAIL
   [ "$errors" != 0 ] && v=FAIL
   echo "$name: $v ($info, $speed, PSNR avg $avg min $min, decoder errors $errors)"
done
