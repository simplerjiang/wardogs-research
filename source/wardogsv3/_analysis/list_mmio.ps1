$mmio = Get-CimInstance Win32_DeviceMemoryAddress | Sort-Object { [uint64]$_.StartingAddress }
foreach ($m in $mmio) {
    $s = [uint64]$m.StartingAddress
    $e = [uint64]$m.EndingAddress
    '{0:X10} - {1:X10}  {2,10:N0} KiB' -f $s, $e, (($e - $s + 1) / 1KB)
}
'---- summary ----'
'count      : ' + $mmio.Count
'total MiB  : ' + [math]::Round((($mmio | ForEach-Object { [uint64]$_.EndingAddress - [uint64]$_.StartingAddress + 1 }) | Measure-Object -Sum).Sum / 1MB, 1)
'installed  : ' + [math]::Round((Get-PhysicallyInstalledSystemMemoryBytes / 1GB), 2) + ' GiB'
