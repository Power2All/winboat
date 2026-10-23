@echo off
cd /d C:\build\dxtest
winboat-dxtest.exe --out C:\build\dxtest  > C:\build\dxtest\console.txt 2>&1
echo done > C:\build\dxtest\done
