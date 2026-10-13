# 1401 Probe: the Windows scan.
# Records what Windows knows about this PC's hardware: processor, graphics, displays, firmware tables, Wi-Fi,
# Bluetooth, network, sound, battery and every other device with its IDs and driver. It only reads; it changes
# nothing and never goes online. The 1401 Probe app runs it; it can also be run by hand:
#   powershell -NoProfile -ExecutionPolicy Bypass -File WindowsScan.ps1 [-OutDir <folder>]
#
# Windows cannot show a GPU's registers or a CPU's model-specific registers to an ordinary program. That part of
# the scan runs from the 1401 Probe USB stick.
#
# Never saved: serial numbers, UUIDs, network (MAC) addresses, Wi-Fi network names, the names of paired
# Bluetooth devices, phones and printers, the computer name, user names and the Windows licence table (MSDM).

param([string]$OutDir = [Environment]::GetFolderPath('Desktop'), [string]$Version = 'dev')

$ErrorActionPreference = 'Continue'
$Stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$Name = "1401-windows-scan-$Stamp"
$Out = Join-Path $env:TEMP $Name
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Log = New-Object System.Collections.Generic.List[string]

function Note($m) { $Log.Add($m); Write-Host $m }

function Save($rel, $obj) {
    $p = Join-Path $Out $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
    if ($obj -is [byte[]]) { [IO.File]::WriteAllBytes($p, $obj) }
    elseif ($obj -is [string]) { [IO.File]::WriteAllText($p, (Scrub $obj)) }
    else { [IO.File]::WriteAllText($p, (Scrub ($obj | ConvertTo-Json -Depth 6))) }
}

$Stages = @('system', 'cpu', 'acpi', 'devices', 'gpu', 'displays', 'radios', 'audio', 'power', 'drivers')
$script:StageNo = 0
# Every stage runs on its own: one that fails is written down with its reason and the next one still runs.
# The "STEP i/n" line is what the app shows as progress.
function Stage($name, [scriptblock]$body) {
    $script:StageNo++
    Write-Host ("STEP {0}/{1} {2}" -f $script:StageNo, $Stages.Count, $name)
    $t = Get-Date
    try { & $body; Note ("ok    {0,-10} {1:N1}s" -f $name, ((Get-Date) - $t).TotalSeconds) }
    catch { Note ("FAIL  {0,-10} {1}" -f $name, $_.Exception.Message) }
}

# Values that identify a person or a machine are removed from every text this script saves.
$Private = @($env:COMPUTERNAME, $env:USERNAME, $env:USERDOMAIN) | Where-Object { $_ -and $_.Length -ge 3 }
function Scrub([string]$s) {
    foreach ($p in $Private) { $s = $s -replace [regex]::Escape($p), '(removed)' }
    # JSON values keep their quotes so the file still parses; text lines lose everything after the label
    $s = $s -replace '(?i)("(SerialNumber|Serial|SSID|BSSID|MacAddress|PermanentAddress|NetworkAddress|UUID|UniqueID)"\s*:\s*)"[^"]*"', '$1"(removed)"'
    $s = $s -replace '(?im)^(?!\s*["{\[])(\s*[^:=\r\n]*(Serial|UUID|Machine name|Machine Id)[^:=\r\n]*[:=]).*$', '$1 (removed)'
    $s = $s -replace '(?im)^(\s*(SSID|BSSID|Physical address)[^:=\r\n]*[:=]).*$', '$1 (removed)'
    $s = $s -replace '(?i)(?<![0-9a-f])([0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])', 'XX:XX:XX:XX:XX:XX'
    return $s
}

function ScrubEdid([byte[]]$b) {
    if ($b.Length -ge 128 -and $b[0] -eq 0 -and $b[1] -eq 255 -and $b[7] -eq 0) {
        for ($i = 12; $i -lt 16; $i++) { $b[$i] = 0 }
        foreach ($off in 54, 72, 90, 108) {
            if ($b[$off] -eq 0 -and $b[$off + 1] -eq 0 -and $b[$off + 2] -eq 0 -and $b[$off + 3] -eq 0xFF) {
                for ($i = $off + 5; $i -lt $off + 18; $i++) { $b[$i] = 32 }
            }
        }
        $sum = 0; for ($i = 0; $i -lt 127; $i++) { $sum += $b[$i] }
        $b[127] = [byte]((256 - ($sum % 256)) % 256)
    }
    return , $b
}

# A device whose name is chosen by a person (a paired phone, headphones, a printer, an audio endpoint named after
# them) keeps its IDs but not its name.
function PersonalName($instance, $class) {
    return ($instance -match '^(BTHENUM|BTHLE|BTHLEDEVICE|BTHHFENUM|BTHMODEM|SWD|WPDBUSENUM|PRINTENUM|WSDPRINT|MMDEVAPI|DOT4|USBPRINT)\\') -or
           ($class -in @('WPD', 'PrintQueue', 'Printer', 'AudioEndpoint', 'SoftwareDevice', 'WSDPrintDevice'))
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Fw {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint EnumSystemFirmwareTables(uint provider, byte[] buf, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint GetSystemFirmwareTable(uint provider, uint id, byte[] buf, uint size);
    [DllImport("kernel32.dll")]
    public static extern bool IsProcessorFeaturePresent(uint f);
    public static byte[] Get(uint provider, uint id) {
        uint n = GetSystemFirmwareTable(provider, id, null, 0);
        if (n == 0) return null;
        byte[] b = new byte[n];
        GetSystemFirmwareTable(provider, id, b, n);
        return b;
    }
    public static uint[] List(uint provider) {
        uint n = EnumSystemFirmwareTables(provider, null, 0);
        if (n == 0) return new uint[0];
        byte[] b = new byte[n];
        EnumSystemFirmwareTables(provider, b, n);
        uint[] ids = new uint[n / 4];
        for (int i = 0; i < ids.Length; i++) ids[i] = BitConverter.ToUInt32(b, i * 4);
        return ids;
    }
}
'@

$Rep = [ordered]@{ tool = "1401 Probe $Version (Windows scan)"; schema = '1401-windows-scan/1'; started_utc = $Stamp }
Note "1401 Probe $Version - Windows scan"

Stage 'system' {
    $os = Get-CimInstance Win32_OperatingSystem
    $cs = Get-CimInstance Win32_ComputerSystem
    $bb = Get-CimInstance Win32_BaseBoard
    $bios = Get-CimInstance Win32_BIOS
    $en = Get-CimInstance Win32_SystemEnclosure
    $Rep.system = [ordered]@{
        windows = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
        maker = $cs.Manufacturer; model = $cs.Model; system_family = $cs.SystemFamily; sku = $cs.SystemSKUNumber
        chassis_types = @($en.ChassisTypes); pc_system_type = $cs.PCSystemType
        board_maker = $bb.Manufacturer; board = $bb.Product; board_version = $bb.Version
        bios = $bios.SMBIOSBIOSVersion; bios_date = "$($bios.ReleaseDate)"; bios_maker = $bios.Manufacturer
        smbios = "$($bios.SMBIOSMajorVersion).$($bios.SMBIOSMinorVersion)"
        memory_gb = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    }
    try { $Rep.system.secure_boot = Confirm-SecureBootUEFI } catch { $Rep.system.secure_boot = "unknown (legacy BIOS or no permission): $($_.Exception.Message)" }
    try { $t = Get-Tpm; $Rep.system.tpm = [ordered]@{ present = $t.TpmPresent; maker = $t.ManufacturerIdTxt; version = $t.ManufacturerVersion } } catch {}
    Save 'system/memory_modules.json' (Get-CimInstance Win32_PhysicalMemory | Select-Object Manufacturer, PartNumber, Capacity, Speed, ConfiguredClockSpeed, ConfiguredVoltage, SMBIOSMemoryType, FormFactor, DeviceLocator, BankLabel)
    Save 'system/disks.json' (Get-PhysicalDisk | Select-Object FriendlyName, Model, BusType, MediaType, Size, FirmwareVersion, SpindleSpeed)
    Save 'system/storage_controllers.json' (Get-CimInstance Win32_SCSIController | Select-Object Name, Manufacturer, DriverName, @{n = 'pnp'; e = { $_.PNPDeviceID -replace '\\[^\\]*$', '\(instance)' } })
    Save 'system/powercfg_sleep_states.txt' ((powercfg /a) -join "`r`n")
}

Stage 'cpu' {
    $p = Get-CimInstance Win32_Processor
    $reg = Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0'
    $ucode = $reg.'Update Revision'
    $Rep.cpu = [ordered]@{
        name = ($p | Select-Object -First 1).Name.Trim()
        maker = ($p | Select-Object -First 1).Manufacturer
        identifier = $reg.Identifier                       # "Intel64 Family 6 Model 140 Stepping 1"
        packages = @($p).Count
        cores = ($p | Measure-Object NumberOfCores -Sum).Sum
        threads = ($p | Measure-Object NumberOfLogicalProcessors -Sum).Sum
        max_mhz = ($p | Select-Object -First 1).MaxClockSpeed
        l2_kb = ($p | Select-Object -First 1).L2CacheSize; l3_kb = ($p | Select-Object -First 1).L3CacheSize
        virtualization_in_firmware = ($p | Select-Object -First 1).VirtualizationFirmwareEnabled
        # Update Revision is 8 little-endian bytes: AMD keeps the revision in the low half, Intel in the high one
        microcode = if ($ucode -and $ucode.Length -ge 8) {
            $q = [BitConverter]::ToUInt64([byte[]]$ucode, 0)
            '0x{0:x}' -f $(if ($q -shr 32) { $q -shr 32 } else { $q -band 0xFFFFFFFF })
        } else { $null }
        feature_set = if ($reg.FeatureSet) { '0x{0:x}' -f $reg.FeatureSet } else { $null }
    }
    # Windows' own feature checks (IsProcessorFeaturePresent ids), so no CPUID code runs here
    $feat = [ordered]@{}
    foreach ($kv in @{ sse3 = 13; sse4_1 = 37; sse4_2 = 38; avx = 39; avx2 = 40; avx512f = 41; rdrand = 28; rdtscp = 32; second_level_address_translation = 20; virt_firmware = 21; fastfail = 23 }.GetEnumerator()) {
        $feat[$kv.Key] = [Fw]::IsProcessorFeaturePresent($kv.Value)
    }
    $Rep.cpu.features = $feat
    Save 'cpu/win32_processor.json' ($p | Select-Object Name, Caption, Manufacturer, Family, Level, Revision, Stepping, Architecture, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed, CurrentClockSpeed, ExtClock, L2CacheSize, L3CacheSize, SocketDesignation, VirtualizationFirmwareEnabled, VMMonitorModeExtensions, SecondLevelAddressTranslationExtensions)
}

Stage 'acpi' {
    # ACPI tables straight from the firmware. Windows hands back only the first of several tables with the same
    # name (several SSDTs), so the registry copies are saved too.
    $ACPI = 0x41435049  # 'ACPI'
    $seen = @{}
    $n = 0
    foreach ($id in [Fw]::List($ACPI)) {
        $sig = [Text.Encoding]::ASCII.GetString([BitConverter]::GetBytes($id))
        if ($sig -eq 'MSDM' -or $sig -eq 'SLIC') { continue }   # Windows licence keys stay home
        $seen[$sig] = 1 + [int]$seen[$sig]
        $b = [Fw]::Get($ACPI, $id)
        if ($b) { Save ("acpi/{0}{1}.aml" -f $sig, $(if ($seen[$sig] -gt 1) { $seen[$sig] } else { '' })) $b; $n++ }
    }
    foreach ($t in 'DSDT', 'FADT', 'RSDT', 'FACS') {
        $k = "HKLM:\HARDWARE\ACPI\$t"
        if (Test-Path $k) {
            Get-ChildItem $k -Recurse | ForEach-Object {
                $v = (Get-ItemProperty $_.PSPath).'00000000'
                if ($v -is [byte[]]) { Save ("acpi/registry/{0}_{1}.aml" -f $t, ($_.PSChildName -replace '[^\w]', '_')) $v }
            }
        }
    }
    $Rep.acpi_tables = $n
}

Stage 'devices' {
    # Every present device: hardware IDs, driver (version, maker, .inf) and, for PCI devices, the location path
    # (the PciRoot(...)/Pci(...) path OpenCore uses). Driver details come from one query; one property call per
    # device took over 5 minutes on a desktop.
    $drv = @{}
    foreach ($sd in Get-CimInstance Win32_PnPSignedDriver) { if ($sd.DeviceID) { $drv[$sd.DeviceID] = $sd } }
    $all = @(Get-PnpDevice -PresentOnly)
    $pci = @($all | Where-Object { $_.InstanceId -like 'PCI\*' })
    $i = 0
    $rows = foreach ($d in $all) {
        $loc = $null
        if ($d.InstanceId -like 'PCI\*') {
            $i++; if ($i % 20 -eq 0) { Write-Host ("  {0} of {1} PCI devices" -f $i, $pci.Count) }
            try { $loc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_LocationPaths' -ErrorAction Stop).Data } catch {}
        }
        $sd = $drv[$d.InstanceId]
        [ordered]@{
            name = if (PersonalName $d.InstanceId $d.Class) { '(removed)' } else { $d.FriendlyName }
            class = $d.Class; status = "$($d.Status)"; problem = "$($d.Problem)"
            instance = ($d.InstanceId -replace '\\[^\\]*$', '\(instance)')   # the last part can carry a serial
            hardware_ids = $d.HardwareID; compatible_ids = $d.CompatibleID; location_paths = $loc; service = $d.Service
            driver_version = $sd.DriverVersion; driver_provider = $sd.DriverProviderName; driver_inf = $sd.InfName
            driver_date = "$($sd.DriverDate)"; maker = $sd.Manufacturer
        }
    }
    Save 'devices/pnp_present.json' @($rows)
    $Rep.devices = @($rows).Count
    $Rep.device_classes = [ordered]@{}
    foreach ($g in ($rows | Group-Object { $_.class } | Sort-Object Name)) { $Rep.device_classes[[string]$g.Name] = $g.Count }
}

Stage 'gpu' {
    $vc = Get-CimInstance Win32_VideoController
    $Rep.gpus = @(foreach ($g in $vc) {
        [ordered]@{
            name = $g.Name; pnp = ($g.PNPDeviceID -replace '\\[^\\]*$', '\(instance)'); driver = $g.DriverVersion
            driver_date = "$($g.DriverDate)"; video_processor = $g.VideoProcessor; adapter_ram = $g.AdapterRAM
            mode = "$($g.CurrentHorizontalResolution)x$($g.CurrentVerticalResolution)@$($g.CurrentRefreshRate)"
        }
    })
    # The display-adapter class keys: chip type, VBIOS string, memory size and the driver's own settings
    $cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    Get-ChildItem $cls -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object {
        $props = Get-ItemProperty $_.PSPath
        $o = [ordered]@{}
        foreach ($pp in $props.PSObject.Properties) {
            if ($pp.Name -like 'PS*') { continue }
            $v = $pp.Value
            if ($v -is [byte[]]) {
                if ($pp.Name -like 'HardwareInformation.*' -and $v.Length -le 256) { $v = [Text.Encoding]::Unicode.GetString($v).Trim([char]0) + " | hex " + (($v | ForEach-Object { '{0:x2}' -f $_ }) -join '') }
                elseif ($v.Length -le 64) { $v = 'hex ' + (($v | ForEach-Object { '{0:x2}' -f $_ }) -join '') }
                else { $v = "($($v.Length) bytes)" }
            }
            $o[$pp.Name] = "$v"
        }
        Save ("gpu/class_{0}.json" -f $_.PSChildName) $o
    }
    $smi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
    if (-not (Test-Path $smi)) { $smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe' }
    if (Test-Path $smi) { Save 'gpu/nvidia-smi.txt' ((& $smi -q) -join "`r`n") }
    $dx = Join-Path $env:TEMP "dxdiag-$Stamp.txt"
    Start-Process -FilePath dxdiag.exe -ArgumentList "/whql:off /t `"$dx`"" -Wait -WindowStyle Hidden
    if (Test-Path $dx) { Save 'gpu/dxdiag.txt' (Get-Content $dx -Raw); Remove-Item $dx }
}

Stage 'displays' {
    $n = 0
    Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY' -ErrorAction SilentlyContinue | ForEach-Object {
        $model = $_.PSChildName
        Get-ChildItem $_.PSPath | ForEach-Object {
            $k = Join-Path $_.PSPath 'Device Parameters'
            $e = (Get-ItemProperty $k -ErrorAction SilentlyContinue).EDID
            if ($e -is [byte[]]) { $n++; Save ("displays/edid_{0}_{1}.bin" -f $model, $n) (ScrubEdid $e) }
        }
    }
    $Rep.edids = $n
    Save 'displays/monitors.json' (Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams -ErrorAction SilentlyContinue | Select-Object InstanceName, VideoInputType, MaxHorizontalImageSize, MaxVerticalImageSize, Active)
    Save 'displays/connections.json' (Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorConnectionParams -ErrorAction SilentlyContinue | Select-Object InstanceName, VideoOutputTechnology, Active)
}

Stage 'radios' {
    # Wi-Fi, Bluetooth and network chips: what the driver says the chip can do, its driver and firmware, and its
    # settings. Never the networks it has seen or joined, its address, or the devices paired with it.
    Save 'radios/wlan_drivers.txt' ((netsh wlan show drivers) -join "`r`n")
    Save 'radios/wlan_capabilities.txt' ((netsh wlan show wirelesscapabilities) -join "`r`n")
    $ad = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.HardwareInterface })
    Save 'radios/adapters.json' @($ad | ForEach-Object {
        [ordered]@{
            description = $_.InterfaceDescription; driver = $_.DriverFileName; driver_version = $_.DriverVersionString
            driver_provider = $_.DriverProvider; driver_date = $_.DriverDate; ndis = $_.NdisVersion
            media = $_.PhysicalMediaType; link_speed = $_.LinkSpeed; status = "$($_.Status)"
            pnp = ($_.PnPDeviceID -replace '\\[^\\]*$', '\(instance)')
        }
    })
    $adv = foreach ($a in $ad) {
        Get-NetAdapterAdvancedProperty -Name $a.Name -ErrorAction SilentlyContinue |
            Where-Object { $_.RegistryKeyword -notmatch '(?i)NetworkAddress|MAC|Address' } |
            ForEach-Object { [ordered]@{ adapter = $a.InterfaceDescription; setting = $_.DisplayName; keyword = $_.RegistryKeyword; value = $_.DisplayValue } }
    }
    Save 'radios/adapter_settings.json' @($adv)
    # the Bluetooth radio itself (a USB or PCI function), never the BTHENUM/BTHLE children (paired devices)
    $bt = @(Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -notmatch '^(BTH|SWD)' })
    $btrows = foreach ($d in $bt) {
        $props = [ordered]@{}
        foreach ($pr in @(Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue)) {
            if ($pr.KeyName -match '(?i)address|serial|container|name|instanceid|parent|children|siblings|lastarrival|lastremoval|installdate|firstinstall') { continue }
            $v = $pr.Data
            if ($v -is [byte[]]) { $v = 'hex ' + (($v | Select-Object -First 64 | ForEach-Object { '{0:x2}' -f $_ }) -join '') }
            $props[$pr.KeyName] = "$v"
        }
        [ordered]@{ name = $d.FriendlyName; instance = ($d.InstanceId -replace '\\[^\\]*$', '\(instance)'); hardware_ids = $d.HardwareID; properties = $props }
    }
    Save 'radios/bluetooth.json' @($btrows)
    $Rep.radios = [ordered]@{
        wifi = @($ad | Where-Object { $_.PhysicalMediaType -match '802\.11' } | ForEach-Object { $_.InterfaceDescription })
        ethernet = @($ad | Where-Object { $_.PhysicalMediaType -eq '802.3' } | ForEach-Object { $_.InterfaceDescription })
        bluetooth = @($bt | ForEach-Object { $_.FriendlyName })
    }
}

Stage 'audio' {
    # HD Audio codec IDs (vendor/device/subsystem/revision): what AppleALC layouts are chosen from
    Save 'audio/hdaudio.json' @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'HDAUDIO\*' -or $_.InstanceId -like 'INTELAUDIO\*' } | ForEach-Object { [ordered]@{ name = $_.FriendlyName; ids = $_.HardwareID } })
    Save 'audio/sound_devices.json' (Get-CimInstance Win32_SoundDevice | Select-Object Name, Manufacturer, @{n = 'pnp'; e = { $_.PNPDeviceID -replace '\\[^\\]*$', '\(instance)' } })
}

Stage 'power' {
    # battery design and chemistry (not its serial or unique id), and what can wake the PC
    Save 'power/battery.json' (Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object Name, Chemistry, DesignVoltage, DesignCapacity, FullChargeCapacity, EstimatedChargeRemaining, BatteryStatus)
    Save 'power/battery_static.json' (Get-CimInstance -Namespace root\wmi -ClassName BatteryStaticData -ErrorAction SilentlyContinue | Select-Object ManufactureName, Chemistry, DesignedCapacity, Technology, Capabilities)
    Save 'power/wake_armed.txt' ((powercfg /devicequery wake_armed) -join "`r`n")
    Save 'power/thermal_zones.json' (Get-CimInstance -Namespace root\wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction SilentlyContinue | Select-Object InstanceName, CurrentTemperature, CriticalTripPoint, PassiveTripPoint)
}

Stage 'drivers' {
    Save 'devices/driverquery.txt' ((driverquery /v /fo csv) -join "`r`n")
}

Save 'stages.txt' ($Log -join "`r`n")
Save 'report.json' $Rep

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Zip = Join-Path $OutDir "$Name.zip"
# .NET's own zip writer: forward-slash names (Compress-Archive on Windows PowerShell 5.1 writes backslashes)
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $Zip) { Remove-Item $Zip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($Out, $Zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Remove-Item $Out -Recurse -Force
Write-Host ''
Write-Host "CPU  : $($Rep.cpu.name) ($($Rep.cpu.identifier))"
foreach ($g in $Rep.gpus) { Write-Host "GPU  : $($g.name)" }
foreach ($w in $Rep.radios.wifi) { Write-Host "Wi-Fi: $w" }
foreach ($b in $Rep.radios.bluetooth) { Write-Host "BT   : $b" }
Write-Host ''
Write-Host "RESULT $Zip"
