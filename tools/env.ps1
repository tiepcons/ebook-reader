# Dot-source to enter the x64 developer environment of the Visual Studio install that has clang-cl.
# Leaves the caller's $ErrorActionPreference untouched (native stderr must not become terminating errors).
if (-not $env:EB_DEVSHELL) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    if (-not $vs) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { throw 'Visual Studio with "C++ build tools" not found (see docs/PLAN.md T0.1)' }
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $env:EB_DEVSHELL = $vs
}
