@echo off
cd /d C:\build\run
C:\build\vdec\ffmpeg.exe -hide_banner -v error -stats -init_hw_device d3d12va=hw:0 -filter_hw_device hw -f lavfi -i testsrc2=s=1920x1080:r=30 -t 3 -vf format=p010,hwupload -c:v hevc_d3d12va -profile:v main10 -y C:\build\run\venc.mp4 > C:\build\run\out.txt 2>&1
echo exit %ERRORLEVEL% > C:\build\run\done
