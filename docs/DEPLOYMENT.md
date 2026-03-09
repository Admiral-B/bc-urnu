# Bridge Command Deployment Guide

## Hardware

### Bridge PCs (2x identical)
- Windows 10
- Intel UHD Graphics (integrated, DX12 capable)
- User: `sysman002` on domain `2zr1jz3`
- No dedicated GPU

### Dev Laptop
- Windows 11, has dedicated GPU
- Runs WickedEngine DX12 path natively

### Network
- All 3 machines connected via unmanaged Ethernet switch
- No DHCP server -- uses ICS from laptop or static IPs
- Bridge Command networking: ENet UDP port 18304+

## Architecture

### Rendering Paths
| Machine | Renderer | Flag |
|---------|----------|------|
| Laptop (dev) | WickedEngine DX12 | `--wicked` or `use_wicked_engine=1` |
| Bridge PCs | WickedEngine DX12 (Intel UHD) | `use_wicked_engine=1` |
| Bridge PCs (fallback) | Irrlicht DX9 | default (no flag) |

WickedEngine on Intel UHD is untested. If DX12 fails, fall back to Irrlicht.

### Multi-PC Bridge (current Irrlicht path)
- **Primary instance**: `secondary_mode=0`, runs simulation + sends state over ENet
- **Secondary instances**: `secondary_mode=1`, receive state, render at `look_angle` offset
- Each monitor = one instance of `bridgecommand-bc.exe` with its own `bc5.ini`

### Multi-PC Bridge (WickedEngine path) -- NOT YET IMPLEMENTED
- WE path currently only runs in `OperatingMode::Normal` (see `main.cpp:1030`)
- `secondary_mode=1` falls through to Irrlicht
- **Requires code changes** to support WE secondary mode (see Remaining Work below)

### Single-PC Multi-Monitor (WickedEngine path) -- IMPLEMENTED
- `WickedMultiView` class creates extra windows on detected monitors
- Configured via `wicked_views=3` in bc5.ini
- Only works on WE path (requires DX12 GPU)

## Deployment Layout

Each instance needs its own directory with a customized `bc5.ini`:

```
PC1 (Master, 3 TVs):
  C:\BridgeCommand\center\    -- primary instance, look_angle=0
  C:\BridgeCommand\port\      -- secondary, look_angle=-90
  C:\BridgeCommand\starboard\ -- secondary, look_angle=90

PC2 (Secondary, 2 monitors):
  C:\BridgeCommand\radar\     -- secondary, full_radar=1
  C:\BridgeCommand\ecdis\     -- secondary, look_angle=180 (or instruments)
```

All directories share the same exe/dll/data files (hardlinks or copies).
Only `bc5.ini` differs per instance.

## Network Setup (from site visit 2026-03-09)

### Problem: No DHCP on switch
The switch has no router/DHCP. Machines get 169.254.x.x (APIPA) addresses which sort of work but are unreliable.

### Solution A: Internet Connection Sharing (ICS)
1. Plug laptop into switch
2. Share laptop Wi-Fi via ICS to Ethernet adapter
3. PCs get 192.168.137.x addresses + internet access
4. Laptop becomes 192.168.137.1

### Solution B: Static IPs (more reliable, no laptop needed)
On each PC, admin Command Prompt:
```
PC1: netsh interface ip set address "Ethernet" static 192.168.1.10 255.255.255.0
PC2: netsh interface ip set address "Ethernet" static 192.168.1.11 255.255.255.0
```

For Bridge Command secondary_hostname, use these IPs.

### Firewall Requirements
Each PC needs these firewall rules (persist across reboots):
```
netsh advfirewall firewall add rule name="BC ENet UDP" protocol=udp dir=in localport=18300-18400 action=allow
netsh advfirewall firewall add rule name="Allow Ping" protocol=icmpv4:8,any dir=in action=allow
```

For file sharing (deployment only):
```
netsh advfirewall set publicprofile state off
net share CDrive=C:\ /grant:everyone,full
```

### Restoring Broken Sim
If the secondary PC stopped receiving control inputs after firewall changes:
1. Check both PCs can ping each other
2. Ensure UDP 18300-18400 inbound is allowed on both
3. Verify `secondary_hostname` in bc5.ini points to the correct IP
4. Restart Bridge Command on both PCs (primary first, then secondary)

## Remaining Work

### P0: WE Secondary Mode Support
`main.cpp:1030` -- WickedEngine only activates for `OperatingMode::Normal`.
Need to implement WE rendering for `OperatingMode::Secondary` so bridge PCs
can run DX12 in secondary mode. Without this, multi-PC bridge = Irrlicht only.

### P0: Intel UHD DX12 Testing
Test WickedEngine on Intel UHD Graphics. May need:
- Latest Intel GPU drivers (download via `intel.com/content/www/us/en/download`)
- Reduced quality settings (disable HBAO, SSR, lower resolution)
- WE may not create DX12 device at all on older UHD models

### P1: Deployment Automation
- `deploy.bat` to copy files to remote PCs via network share
- Per-instance bc5.ini generation
- Startup scripts to launch all instances in correct order

### P2: Persistent Network Config
- Set static IPs on bridge PCs (survives reboots)
- Create Windows scheduled task to auto-launch Bridge Command instances on boot
