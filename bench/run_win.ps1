param(
    [int]$Rounds = 7,
    [int]$Threads = 4,
    [int]$Per = 4000000,
    [string]$Cpus = "",          # 空 = 自动探测 P 核；显式指定则原样使用
    [int]$BurnMs = 100,          # 测延迟前的 CPU 预热时长（0 = 关闭）
    [int]$Idle = 0               # 每次运行之间的静默间隔（秒），0 = 不等待
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot 'bin')

# ---------------------------------------------------------------------------
# P 核自动探测
#
# 混合架构上逻辑核编号并不按"物理核成对"排列，硬编码（如 2,3,4,5）很可能
# 落到 E 核上——本开发机正是如此（P 核是 0/1、10/11、12/13、14/15）。
# 这里用 GetLogicalProcessorInformationEx 读 EfficiencyClass：数值最大的
# 那一档是 P 核。每颗物理核只取一个逻辑核（避开 SMT 兄弟），保证 N 个线程
# 各自独占一颗物理核。
# ---------------------------------------------------------------------------
function Get-PCoreCpus {
    if (-not ([System.Management.Automation.PSTypeName]'CpuTopology').Type) {
        Add-Type -Namespace '' -Name 'CpuTopology' -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GetLogicalProcessorInformationEx(
    int relationshipType, IntPtr buffer, ref uint returnedLength);
'@
    }
    # RelationProcessorCore = 0
    [uint32]$len = 0
    [void][CpuTopology]::GetLogicalProcessorInformationEx(0, [IntPtr]::Zero, [ref]$len)
    if ($len -eq 0) { return @() }
    $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal([int]$len)
    try {
        if (-not [CpuTopology]::GetLogicalProcessorInformationEx(0, $buf, [ref]$len)) {
            return @()
        }
        $offset = 0
        $cores = @()   # 每项: @{ Eff = n; Cpus = @(逻辑核...) }
        while ($offset -lt $len) {
            $relationship = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, $offset)
            $size = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, $offset + 4)
            if ($relationship -eq 0) {
                # PROCESSOR_RELATIONSHIP:
                #   +8 Flags(byte) +9 EfficiencyClass(byte) ... +30 GroupCount(WORD)
                #   +32 GroupMask[] (每组 16 字节: 8B mask + 8B reserved)
                $eff = [System.Runtime.InteropServices.Marshal]::ReadByte($buf, $offset + 9)
                $groups = [System.Runtime.InteropServices.Marshal]::ReadInt16($buf, $offset + 30)
                $cpus = @()
                for ($g = 0; $g -lt $groups; $g++) {
                    $mask = [System.Runtime.InteropServices.Marshal]::ReadInt64($buf, $offset + 32 + $g * 16)
                    for ($b = 0; $b -lt 64; $b++) {
                        if (($mask -shr $b) -band 1) { $cpus += $b }
                    }
                }
                if ($cpus.Count -gt 0) {
                    $cores += [pscustomobject]@{ Eff = $eff; Cpus = $cpus }
                }
            }
            if ($size -le 0) { break }
            $offset += $size
        }
    }
    finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
    }
    if ($cores.Count -eq 0) { return @() }

    $maxEff = ($cores | Measure-Object -Property Eff -Maximum).Maximum
    $pc = @($cores | Where-Object { $_.Eff -eq $maxEff })
    # 每颗物理核取第一个逻辑核：避开 SMT 兄弟，N 线程各占一颗物理核
    return @($pc | ForEach-Object { $_.Cpus[0] })
}

if (-not $Cpus) {
    $detected = Get-PCoreCpus
    if ($detected.Count -gt 0) {
        $Cpus = ($detected -join ',')
        Write-Host "auto-detected P-cores: $Cpus"
    }
    else {
        $Cpus = "0,1,2,3"
        Write-Warning "P-core detection failed; falling back to $Cpus"
    }
}
$env:BENCH_CPUS = $Cpus
$env:BENCH_BURN_MS = "$BurnMs"

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
    "BurnMs: $BurnMs",
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
        if ($Idle -gt 0) { Start-Sleep -Seconds $Idle }
    }
}
Write-Host "== done =="
