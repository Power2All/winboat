# uia.ps1 @screen clickat <x> <y> | uia.ps1 @screen keys|slowkeys|paste <SendKeys text>: a real click at screen coordinates, keystrokes to the
# focused window (for windows with no automation controls, like 3DMark's).
# uia.ps1 <window title (substring)> [list | click <name> | check <name> | type <name> <text>]: drives a window of the
# desktop session through UI Automation (the spike viewer gets no input from xdotool under Wayland). list prints the
# window's controls (type, name, enabled); click invokes a button, check toggles a check/radio box, type sets an edit's
# text. Exit 1 when the window or control isn't there.
param([string]$Title, [string]$Action = 'list', [string]$Name, [string]$Text)
Add-Type -Namespace W -Name M -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, System.UIntPtr e);'
function ClickAt([int]$x, [int]$y) {
    [W.M]::SetCursorPos($x, $y) | Out-Null
    [W.M]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [W.M]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}
if ($Title -eq '@screen') {
    if ($Action -eq 'clickat') { ClickAt $Name $Text; "clicked $Name,$Text" }
    else {
        # keys: at once; slowkeys: a character at a time; paste: through the clipboard (some dialogs drop fast input)
        Add-Type -AssemblyName System.Windows.Forms
        if ($Action -eq 'paste') { Set-Clipboard -Value $Name; [System.Windows.Forms.SendKeys]::SendWait('^v') }
        elseif ($Action -eq 'slowkeys') { foreach ($ch in $Name.ToCharArray()) { [System.Windows.Forms.SendKeys]::SendWait("$ch"); Start-Sleep -Milliseconds 80 } }
        else { [System.Windows.Forms.SendKeys]::SendWait($Name) }
        "sent keys"
    }
    exit 0
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$T = [System.Windows.Automation.TreeScope]
$root = $A::RootElement
$win = $root.FindAll($T::Children, [System.Windows.Automation.Condition]::TrueCondition) |
    Where-Object { $_.Current.Name -like "*$Title*" } | Select-Object -First 1
if (-not $win) { "no window '$Title'"; exit 1 }
$all = $win.FindAll($T::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
if ($Action -eq 'list') {
    "window: $($win.Current.Name)"
    foreach ($e in $all) {
        $c = $e.Current
        if ($c.Name -or $c.ControlType.ProgrammaticName -match 'Edit|Button|CheckBox|RadioButton') {
            $n = ($c.Name -replace '\s+', ' ')
            if ($n.Length -gt 90) { $n = $n.Substring(0, 90) + '...' }
            "{0,-28} {1,-50} enabled={2}" -f $c.ControlType.ProgrammaticName, $n, $c.IsEnabled
        }
    }
    exit 0
}
# #<type>: the first control of that type (an edit box without a name: #Edit)
if ($Name.StartsWith('#')) { $el = $all | Where-Object { $_.Current.ControlType.ProgrammaticName -eq "ControlType.$($Name.Substring(1))" } | Select-Object -First 1 }
else { $el = $all | Where-Object { $_.Current.Name -like $Name } | Select-Object -First 1 }
if (-not $el) { "no control '$Name'"; exit 1 }
switch ($Action) {
    'click' {
        $p = $null
        if ($el.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke() }
        else {
            # skinned buttons (InstallShield's are panes): a real click at the control's center
            $r = $el.Current.BoundingRectangle
            ClickAt ($r.X + $r.Width / 2) ($r.Y + $r.Height / 2)
        }
    }
    'check' {
        $p = $null
        if ($el.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$p)) { $p.Select() }
        else { $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
    }
    'type' { $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Text) }
}
"done: $Action '$($el.Current.Name)'"
