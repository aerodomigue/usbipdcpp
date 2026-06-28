# UsbIpAutoService — Windows USB/IP Auto Attach Service

Automatically attaches all USB devices exported by the Raspberry Pi (`10.2.100.101`),
listens for UDP hotplug notifications (port 3241), and detaches all devices on shutdown/sleep.

## Build

Run as admin in PowerShell:

```powershell
& "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /out:UsbIpAutoService.exe /r:System.ServiceProcess.dll UsbIpAutoService.cs
```

## Test (without installing)

```powershell
.\UsbIpAutoService.exe --console
```

Logs appear in the terminal. `Ctrl+C` triggers `detach --all`.

## Install

```powershell
# Copy to a local path (services cannot run from network shares)
Copy-Item ".\UsbIpAutoService.exe" "C:\KVM\UsbIpAutoService.exe"

# Create and configure the service
sc.exe create UsbIpAutoService binPath= "C:\KVM\UsbIpAutoService.exe" start= auto DisplayName= "USB/IP Auto Attach"
sc.exe triggerinfo UsbIpAutoService start/networkon stop/networkoff

# Start
sc.exe start UsbIpAutoService
```

## Manage

```powershell
sc.exe start   UsbIpAutoService
sc.exe stop    UsbIpAutoService
sc.exe query   UsbIpAutoService
```

## Uninstall

```powershell
sc.exe stop   UsbIpAutoService
sc.exe delete UsbIpAutoService
```

## Logs

```powershell
Get-Content "C:\ProgramData\UsbIpAutoService\service.log" -Wait
```
