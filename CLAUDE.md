# usbipdcpp - Development Guide

## Project Overview
C++ USB/IP server library using libusb (userspace). Deployed on a Raspberry Pi 4 running Raspberry Pi OS Lite (64-bit, aarch64).

## Local Development (Mac M4)
The project lives on the Mac at the current directory. All modifications are made here.

## Target Device
- **Host**: `usbIP` (alias) or `10.2.100.101`
- **User**: `anthony`
- **SSH**: `ssh anthony@usbIP`
- **Project path on Pi**: `~/usbipdcpp/`
- **Binary**: `~/usbipdcpp/build/libusb_server`
- **Service**: `usbipdcpp` (systemd)

## Workflow

### 1. Sync modified files to Pi
After any modification, sync only the source files (not the build directory):
```bash
rsync -av --exclude='build/' --exclude='.git/' ./ anthony@usbIP:~/usbipdcpp/
```

### 2. Build on Pi
```bash
ssh anthony@usbIP "cd ~/usbipdcpp && cmake -B build -DUSBIPDCPP_USE_PKGCONF_ASIO=ON -DUSBIPDCPP_BUILD_LIBUSB_COMPONENTS=ON 2>/dev/null; cmake --build build --target libusb_server 2>&1 | tail -5"
```

### 3. Test (stop service, run manually)
```bash
# Stop service
ssh anthony@usbIP "sudo systemctl stop usbipdcpp"

# Run manually to see logs
ssh anthony@usbIP "sudo ~/usbipdcpp/build/libusb_server -p 3240"
```

### 4. Deploy (replace service binary)
When test is OK, restart the service:
```bash
ssh anthony@usbIP "sudo systemctl restart usbipdcpp"
```

## Custom Modifications Applied
- `src/LibusbHandler/LibusbServer.cpp`: Auto-bind on hotplug and at startup, filter hubs (class 0x09) and internal Ethernet (0424:ec00), UDP broadcast on port 3241 when device bound
- `examples/libusb_server/libusb_server.cpp`: Log level set to info
- `src/Server.cpp`: TCP keepalive (idle=120s, interval=15s, count=10) to detect dead clients (e.g. Windows reboot)
- `src/Session.cpp`: Connection/disconnection logs demoted to debug; info only on real attach (OpReqImport)
- `include/Device.h`: Added `display_name` field (non-serialized) populated from USB product string
- `tools/windows/UsbIpAutoService.cs`: Windows service that auto-attaches all Pi devices, listens for UDP hotplug notifications (port 3241), detaches all on shutdown

## Windows Service (tools/windows/UsbIpAutoService.cs)
Auto-attaches USB/IP devices from the Pi and detaches on shutdown.

### Build (Windows, run as admin)
```powershell
& "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /out:UsbIpAutoService.exe /r:System.ServiceProcess.dll UsbIpAutoService.cs
```

### Test before install
```powershell
.\UsbIpAutoService.exe --console
```

### Install
Note: Windows services cannot run from network shares — copy the exe locally first.
Use `sc.exe` in PowerShell (`sc` is an alias for `Set-Content`).
```powershell
Copy-Item ".\UsbIpAutoService.exe" "C:\KVM\UsbIpAutoService.exe"
sc.exe create UsbIpAutoService binPath= "C:\KVM\UsbIpAutoService.exe" start= auto DisplayName= "USB/IP Auto Attach"
sc.exe triggerinfo UsbIpAutoService start/networkon stop/networkoff
sc.exe start UsbIpAutoService
```

### Uninstall
```powershell
sc stop UsbIpAutoService
sc delete UsbIpAutoService
```

### Logs
`C:\ProgramData\UsbIpAutoService\service.log`

## Build Dependencies (on Pi)
```bash
sudo apt install -y libasio-dev libspdlog-dev libcxxopts-dev libgtest-dev libusb-1.0-0-dev cmake g++ git
```


## Service Management
```bash
sudo systemctl start usbipdcpp
sudo systemctl stop usbipdcpp
sudo systemctl restart usbipdcpp
sudo systemctl status usbipdcpp
```


Be very careful when modifying the protocol parsing section — it is the result of extensive debugging. Other architectural parts can be modified.

When changing various APIs, consider the implementation difficulty on embedded platforms.

Do not add a trailing "_" to member variable names.
