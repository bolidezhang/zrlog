param(
    [int]$Rounds = 7,
    [int]$Threads = 4,
    [int]$Per = 4000000,
    [string]$Cpus = "2,3,4,5"
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot 'bin')
$env:BENCH_CPUS = $Cpus

$libs = [string[]]@('zrlog', 'spdlog', 'fmtlog', 'quill')
$nlibs = $libs.Length
$outDir = Join-Path $PWD 'results'
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Path $outDir | Out-Null

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$envLines = @(
    "CPU: $($cpu.Name)",
    "Cores: $($cpu.NumberOfCores) Threads: $($cpu.NumberOfLogicalProcessors)",
    "Pinned: $Cpus",
    "Threads: $Threads PerThread: $Per"
)
Set-Content -Path (Join-Path $outDir 'env.txt') -Value $envLines

for ($r = 0; $r -lt $Rounds; $r++) {
    $order = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $nlibs; $i++) {
        [void]$order.Add($libs[($i + $r) % $nlibs])
    }
    Write-Host ("== round {0} : {1} ==" -f $r, ($order -join ' '))
    foreach ($lib in $order) {
        $exe = Join-Path $PWD ("bench_" + $lib + ".exe")
        # 有些库（如 quill）会往 stderr 打 INFO，PowerShell 在 Stop 模式下会把
        # NativeCommandError 当成致命错误中断整个脚本。这里临时放宽，只取
        # 形如 "  <lib> ..." 的数据行。
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $lines = @(& $exe $Threads $Per both 2>&1 |
                       ForEach-Object { "$_" } |
                       Where-Object { $_ -match '^\s{2}\S' })
        }
        finally {
            $ErrorActionPreference = $prev
        }
        if ($lines.Length -eq 0) {
            Write-Warning ("{0} produced no data" -f $lib)
        }
        Add-Content -Path (Join-Path $outDir ($lib + '.txt')) -Value $lines
    }
}
Write-Host "== done =="
