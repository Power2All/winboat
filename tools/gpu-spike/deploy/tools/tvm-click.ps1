# tvm-click.ps1: a left click at the position in C:\Users\Public\click.txt ("x y"), run in the user's session (tvm.sh
# click). The pointer moves onto the spot first and rests there: UIs that track hover (3DMark Vantage's) ignore a
# click that arrives without it
Add-Type -Namespace W -Name M -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
'@
$p = (Get-Content C:\Users\Public\click.txt) -split ' '
$x = [int]$p[0]; $y = [int]$p[1]
[W.M]::SetCursorPos($x - 20, $y - 10) | Out-Null; Start-Sleep -Milliseconds 250
[W.M]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 250
[W.M]::mouse_event(1, 1, 0, 0, 0); Start-Sleep -Milliseconds 250
[W.M]::mouse_event(2, 0, 0, 0, 0); Start-Sleep -Milliseconds 150; [W.M]::mouse_event(4, 0, 0, 0, 0)
