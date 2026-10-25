# Use your own mpv.conf in Nuvio Desktop (no AnimeJaNai)

Nuvio starts its embedded mpv with `config=no`, so `%APPDATA%\mpv\mpv.conf`
is ignored. This setup makes Nuvio load that directory instead, using only a
small per-user shim and two environment variables.

It does **not** install AnimeJaNai, models, TensorRT, or anything under
`Program Files`. It keeps Nuvio's own `libmpv-2.dll`; the shim only forwards
mpv's C API to it.

Tested on Nuvio Desktop 1.1.32, Windows 11 x64.

---

## Install

Open PowerShell and run:

```powershell
$d = "$env:TEMP\nuvio-config-only"
New-Item -ItemType Directory -Force $d | Out-Null
Invoke-WebRequest "https://raw.githubusercontent.com/anh-chu/nuvio-animejanai/main/install-config-only.ps1" -OutFile "$d\install-config-only.ps1"
powershell -ExecutionPolicy Bypass -File "$d\install-config-only.ps1"
```

The script:

1. downloads the tested shim to
   `%LOCALAPPDATA%\NuvioMpvConfig\libmpv-2.dll` and verifies its SHA-256;
2. sets two **user** environment variables:

   | Variable | Value |
   | --- | --- |
   | `NUVIO_LIBMPV_PATH` | `%LOCALAPPDATA%\NuvioMpvConfig\libmpv-2.dll` |
   | `NUVIO_MPV_CONFIG_DIR` | `%APPDATA%\mpv` |

3. clears `NUVIO_MPV_REAL_LIBRARY`, so the shim auto-discovers Nuvio's
   current libmpv instead of using a fixed cache path.

If `NUVIO_MPV_REAL_LIBRARY` already points to an existing custom libmpv (for
example an AnimeJaNai build), the script stops instead of silently replacing
it. Re-run with `-Force` only if you really want config-only mode.

Fully quit Nuvio — **including the tray icon** — then start it again. A
running process cannot pick up new environment variables.

### Manual install

If you prefer not to run a script, the same steps are:

```powershell
$dir = "$env:LOCALAPPDATA\NuvioMpvConfig"
New-Item -ItemType Directory -Force $dir | Out-Null
Invoke-WebRequest "https://raw.githubusercontent.com/anh-chu/nuvio-animejanai/main/shim/libmpv-2.dll" -OutFile "$dir\libmpv-2.dll" -UseBasicParsing
[Environment]::SetEnvironmentVariable('NUVIO_LIBMPV_PATH', "$dir\libmpv-2.dll", 'User')
[Environment]::SetEnvironmentVariable('NUVIO_MPV_CONFIG_DIR', "$env:APPDATA\mpv", 'User')
[Environment]::SetEnvironmentVariable('NUVIO_MPV_REAL_LIBRARY', $null, 'User')
```

Restart Nuvio afterwards.

---

## Verify

Add a temporary log line to `%APPDATA%\mpv\mpv.conf`:

```ini
log-file=~~home/mpv.log
```

Fully quit Nuvio (tray too), start it again, and play any video. Then check:

```powershell
Get-Content "$env:APPDATA\mpv\mpv.log" -Tail 60
```

You should see mpv's config directory and options from your file, for
example:

```
config-dir = C:\Users\<you>\AppData\Roaming\mpv
config = yes
vo = gpu-next
gpu-api = d3d11
```

Remove the `log-file` line when you are done if you do not want a persistent
log.

---

## After a Nuvio update

**Nothing to re-patch or reinstall.** Nuvio extracts
`player_bridge.dll` and `libmpv-2.dll` into the same content-hash directory
under:

```
%LOCALAPPDATA%\Nuvio\Cache\native-player-bridge\windows\<hash>\
```

The shim asks Windows for the directory of the **running**
`player_bridge.dll`, then loads `libmpv-2.dll` from that same directory. A
Nuvio update creates a new `<hash>` directory; the relationship still holds,
so no fixed cache path is involved. Environment variables and the shim live
under `%LOCALAPPDATA%`, which Nuvio updates do not touch.

Resolution order in the shim (v2):

1. `NUVIO_MPV_REAL_LIBRARY`, if set to a usable library (explicit override;
   the AnimeJaNai installer uses this).
2. `libmpv-2.dll` beside the loaded `player_bridge.dll`.
3. `<shim>\..\libmpv\libmpv-2.dll` (the AnimeJaNai layout).
4. The newest directory under the native-player-bridge cache.

If you later install AnimeJaNai, use its installer instead; it replaces the
env values and points `NUVIO_MPV_REAL_LIBRARY` at its own libmpv build.

---

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File "$env:TEMP\nuvio-config-only\install-config-only.ps1" -Uninstall
```

Or, from a clone:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-config-only.ps1 -Uninstall
```

This removes the shim directory and the two variables set by the installer.
Your `%APPDATA%\mpv` directory is left untouched. Restart Nuvio to get the
stock player back.

---

## Troubleshooting

**The log file is missing, or my options are not applied.**

- Nuvio was still running when the variables were set. Quit it from the tray,
  then start it again.
- Check the variables:
  ```powershell
  [Environment]::GetEnvironmentVariable('NUVIO_LIBMPV_PATH','User')
  [Environment]::GetEnvironmentVariable('NUVIO_MPV_CONFIG_DIR','User')
  [Environment]::GetEnvironmentVariable('NUVIO_MPV_REAL_LIBRARY','User')
  ```
  `NUVIO_MPV_REAL_LIBRARY` should be empty for this setup.
- Make sure `mpv.conf` is directly inside `%APPDATA%\mpv` (not a subfolder).

**Nuvio says it cannot load libmpv, or the player is black.**

Check that the shim exists and has the expected size:

```powershell
Get-Item "$env:LOCALAPPDATA\NuvioMpvConfig\libmpv-2.dll"
```

If it is missing, re-run the installer. If Nuvio's native cache is in a
different location, the shim's bridge-sibling lookup still works; the
newest-cache fallback is only for unusual layouts.

**I want to keep AnimeJaNai and also use my mpv.conf.**

Use the AnimeJaNai installer in the main [README](README.md). It sets
`NUVIO_MPV_REAL_LIBRARY` to its own libmpv and carries your settings into its
managed config directory.

**Does this survive Nuvio updates?**

Yes. The shim is not compiled against Nuvio and does not touch its files. It
only needs `NUVIO_LIBMPV_PATH`, `config=no`, and the libmpv C ABI, all of
which Nuvio has used across the tested releases.
