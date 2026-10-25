# AnimeJaNai for Nuvio Desktop (Windows)

AnimeJaNai is a neural upscaler for anime that runs as a native mpv filter
(`animejanai`) inside a forked libmpv. Nuvio Desktop ships its own libmpv and
starts it with `config=no`, so mpv never reads `mpv.conf`, `input.conf`,
`scripts/` or `shaders/` — which is exactly what AnimeJaNai needs.

This repository contains everything needed to drop AnimeJaNai into Nuvio's
embedded player, and to keep it working after Nuvio updates.

**The point:** you install once. After a Nuvio update there is nothing to
re-apply — the integration does not patch Nuvio and does not depend on any
Nuvio release.

> **Only want your own `mpv.conf`, without AnimeJaNai?** Use the config-only
> setup in [CONFIG-ONLY.md](CONFIG-ONLY.md). It installs the same shim but
> keeps Nuvio's stock libmpv, so there are no models, TensorRT, or large
> downloads.

> **Status:** user-side proof of concept, not an upstream feature. Verified on
> Nuvio Desktop 0.1.27-alpha; see
> [NuvioMedia/NuvioDesktop#738](https://github.com/NuvioMedia/NuvioDesktop/issues/738)
> for the upstream discussion. Nothing here is endorsed by Nuvio.

---

## Requirements

| | |
| --- | --- |
| OS | Windows 10/11, x64 |
| GPU | NVIDIA RTX (TensorRT). Other GPUs are not covered by this installer. |
| App | Nuvio Desktop 0.1.27-alpha or newer (AnimeJaNai tested in-app on 0.1.27; 0.1.32 checked against its source; config-only shim v2 verified in-app on 1.1.32) |
| Disk | ~1.6 GB, of which ~0.8 GB is a download cache that re-runs reuse (safe to delete) |
| Network | ~600 MB download on first install |

Everything installs under `%LOCALAPPDATA%`. **No administrator rights, no files
in `Program Files`, no changes to `%APPDATA%\mpv`.**

---

## Install

Open PowerShell and run:

```powershell
$d = "$env:TEMP\nuvio-animejanai"
New-Item -ItemType Directory -Force $d | Out-Null
$b = "https://raw.githubusercontent.com/anh-chu/nuvio-animejanai/main"
foreach ($f in 'install.ps1', 'aji-slot.ps1', 'uninstall.ps1') {
    iwr "$b/$f" -OutFile "$d\$f"
}
powershell -ExecutionPolicy Bypass -File "$d\install.ps1"
```

Or clone the repository and run it from there:

```powershell
git clone --depth 1 https://github.com/anh-chu/nuvio-animejanai.git
powershell -ExecutionPolicy Bypass -File .\nuvio-animejanai\install.ps1
```

The installer:

1. downloads AnimeJaNai 3.8.0 from the official `the-database/mpv-AnimeJaNai`
   release and extracts only what the player needs (the AnimeJaNai libmpv, the
   models, the TensorRT runtime, and the builder matching your GPU);
2. downloads the tested `libmpv-2.dll` shim committed at
   [`shim/libmpv-2.dll`](shim/libmpv-2.dll) and verifies its SHA-256 (the
   [GitHub Actions workflow](.github/workflows/nuvio-animejanai-shim.yml)
   separately rebuilds `src/mpv_shim.c` to check the source still compiles);
3. generates an mpv config directory, carrying over settings from
   `%APPDATA%\mpv\mpv.conf` if present;
4. sets three **user** environment variables:

   | Variable | Value |
   | --- | --- |
   | `NUVIO_LIBMPV_PATH` | `%LOCALAPPDATA%\NuvioAnimeJaNai\shim\libmpv-2.dll` |
   | `NUVIO_MPV_CONFIG_DIR` | `%LOCALAPPDATA%\NuvioAnimeJaNai\config` |
   | `NUVIO_MPV_REAL_LIBRARY` | `%LOCALAPPDATA%\NuvioAnimeJaNai\libmpv\libmpv-2.dll` |

Fully quit Nuvio — including the tray icon — then start it again. Environment
variables of a running process cannot be changed from outside, so a restart is
required the first time (and only the first time).

---

## Verify it is running

Play an anime episode, then:

```powershell
Get-Content "$env:LOCALAPPDATA\NuvioAnimeJaNai\logs\mpv.log" -Tail 40
```

You should see the filter being created and AnimeJaNai reporting a slot:

```
[v][vf] [animejanai] 1920x1080 yuv420p10 ...
[v][animejanai] [aji] backend: TensorRT (aji_trt)
[v][animejanai] Upscale Profile: Balanced
```

The first playback at a given resolution builds a TensorRT engine. That takes
roughly 10–90 seconds on a modern GPU; playback pauses for the build (AnimeJaNai
ships a script that does this, and the installer keeps it) and resumes
automatically. Later plays reuse the cached engine and start instantly.
AnimeJaNai's own status file is
`%LOCALAPPDATA%\NuvioAnimeJaNai\animejanai\currentanimejanai.log`.

Verified end-to-end on Nuvio 0.1.27 with a 1920x1080 23.976 fps episode: the
Balanced engine built in ~80 s, the filter reported
`Configured slot 1002: 1920x1080 -> 3840x2160`, and switching to slot `0` and
back to `1002` from another shell re-applied the cached engine without a
rebuild. That run had an earlier patched bridge installed, so it exercised the
AnimeJaNai side; the shim's `config=no` rewrite was validated separately by
replaying the stock bridge's exact call sequence. Nuvio 0.1.32's source still
makes that same call.

If the log lists `[upscale-on]` but no `[animejanai]` lines, see
[Troubleshooting](#troubleshooting).

---

## Switching quality slots

Nuvio's embedded player sets `input-vo-keyboard=no` and handles only `F11`
itself, so mpv never receives keystrokes inside Nuvio. AnimeJaNai's own keybinds
(`Shift+1`, `Ctrl+J`, …) therefore do nothing. Slots are switched over mpv's IPC
pipe instead, which the installer enables:

```powershell
& "$env:LOCALAPPDATA\NuvioAnimeJaNai\aji-slot.ps1" 1002
```

| Slot | Meaning |
| --- | --- |
| `0` | Off — plain mpv, upscaling bypassed |
| `1`–`9` | Custom profiles from `animejanai\animejanai.conf` |
| `1001` | Quality |
| `1002` | Balanced (default) |
| `1003` | Performance |

The switch is immediate (a TensorRT engine may load once per slot/resolution).
The script must run while a video is playing, because that is when mpv's IPC
pipe exists.

---

## After a Nuvio update

**Nothing to do.** Nuvio has a built-in hook, `NUVIO_LIBMPV_PATH`, which it
reads when loading its player library — this repository's shim sits there. The
shim is not compiled against Nuvio, does not patch Nuvio, and only relies on
libmpv's stable C API, so a new Nuvio release keeps loading it. The
`config=no` call that a new release makes is rewritten by the shim at runtime.
The shim also auto-discovers the real libmpv next to the running
`player_bridge.dll`, so a changed native cache hash does not matter.

Only re-run the installer if you want to change options, or if AnimeJaNai ships
a newer version you want to move to.

---

## Options

Run `Get-Help .\install.ps1 -Detailed` for the full list. The useful ones:

| Option | Effect |
| --- | --- |
| `-WithRife` | Also install RIFE frame interpolation models (~440 MB). Not needed for upscaling. |
| `-KeepAnime4K` | Keep `glsl-shaders` profiles from your mpv.conf. By default they are dropped, because AnimeJaNai has already upscaled the frame to 4K and Anime4K would then run on that output. |
| `-TritonComponent sm90` | Force a TensorRT builder instead of detecting it from `nvidia-smi`. |
| `-UserConfig <path>` | Carry over a different mpv.conf (default `%APPDATA%\mpv\mpv.conf`). |
| `-InstallDir <path>` | Install somewhere other than `%LOCALAPPDATA%\NuvioAnimeJaNai`. |
| `-NoEnv` | Prepare everything but do not touch environment variables. |
| `-Force` | Re-download and re-extract everything. |

The generated config is `%LOCALAPPDATA%\NuvioAnimeJaNai\config\mpv.conf`. It is
rewritten on every install run; put your own settings in `%APPDATA%\mpv\mpv.conf`
instead and they will be carried over.

---

## Uninstall

The installer copies `uninstall.ps1` next to the install, so the folder can be
removed even after the download folder is gone:

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\NuvioAnimeJaNai\uninstall.ps1"
```

(From a clone, `.\install.ps1 -Uninstall` does the same thing.)

This removes the three environment variables and the
`%LOCALAPPDATA%\NuvioAnimeJaNai` folder. Nuvio itself is never modified, so that
is all it takes to get the stock player back. Restart Nuvio afterwards.

---

## Troubleshooting

**Nuvio plays audio but the picture is black / the window stays empty.**
A rejected `vf` string kills video silently in mpv. Check
`logs\mpv.log` for `Creating filter '...' failed` or `[animejanai]` errors.
Re-run the installer; if you edited `config\mpv.conf`, delete it and re-run to
regenerate.

**Nothing in `logs\mpv.log`, or the log is from an older run.**
Nuvio was still running when the installer set the variables, or only the main
window was closed and the tray icon kept the process alive. Quit Nuvio from the
tray, then start it again.

**`logs\mpv.log` has no `[animejanai]` lines, but video plays.**
The shim did not intercept the `config=no` call: `NUVIO_MPV_CONFIG_DIR` is
missing or points at a folder that does not exist. Check with
`[Environment]::GetEnvironmentVariable('NUVIO_MPV_CONFIG_DIR','User')` and
re-run the installer if it is empty.

**AnimeJaNai is active but playback stutters.**
Try a lower slot (`1003`), and check that the GPU is not also running a game or
another CUDA workload. TensorRT engine caches live under
`%LOCALAPPDATA%\NuvioAnimeJaNai\animejanai`.

**First playback at a new resolution pauses.**
Expected: a TensorRT engine is being built for that resolution. It is cached;
the pause only happens once per resolution per slot.

**You want to go back to a specific AnimeJaNai version.**
`.\install.ps1 -AnimeJaNaiVersion 3.7.0 -Force`.

---

## How it works

Nuvio's `player_bridge.dll` loads libmpv from `NUVIO_LIBMPV_PATH` if it is set,
then calls:

```c
mpv_set_option_string(mpv, "config", "no");
```

The shim in [`src/mpv_shim.c`](src/mpv_shim.c) is a tiny `libmpv-2.dll` that
forwards every libmpv entry point Nuvio uses to the real AnimeJaNai libmpv, but
intercepts that one call and replaces it with:

```c
mpv_set_option_string(mpv, "config-dir", "%LOCALAPPDATA%\\NuvioAnimeJaNai\\config");
mpv_set_option_string(mpv, "config", "yes");
```

Unset `NUVIO_MPV_CONFIG_DIR` and the shim forwards the original call untouched,
so a normal Nuvio install keeps its exact stock behaviour.

The alternative — patching `player_bridge.cpp` to honour a config directory —
needs a matching build for every Nuvio release. The shim is compiled from
[`src/mpv_shim.c`](src/mpv_shim.c) by GitHub Actions, committed here as
[`shim/libmpv-2.dll`](shim/libmpv-2.dll) (the tested binary used by the
installer and the release), and depends only on the libmpv C ABI.

```
%LOCALAPPDATA%\NuvioAnimeJaNai\
  animejanai\            AnimeJaNai models, aji.dll, TensorRT runtime/builder
  libmpv\libmpv-2.dll    AnimeJaNai's forked libmpv (the real player library)
  shim\libmpv-2.dll      the pass-through shim Nuvio loads
  config\mpv.conf        generated config (includes the managed profile below)
  config\input.conf      slot bindings, for completeness
  config\scripts\        AnimeJaNai Lua scripts (backend, slot, stats, monitor)
  config\shaders\        AnimeJaNai's noise shaders
  portable_config\       upstream mpv-animejanai.conf, unmodified
  logs\mpv.log           player log
  cache\                 downloaded archives and the extracted AnimeJaNai data
```

---

## FAQ

**Do I have to patch Nuvio?**
No. Nuvio's files are not modified at all: the integration is three per-user
environment variables plus a DLL under `%LOCALAPPDATA%`. An earlier version of
this project used a patched `player_bridge.dll`; that is no longer needed and is
not what ships here.

**Does a Nuvio update break it?**
No. An update replaces Nuvio's own files, not user environment variables or
`%LOCALAPPDATA%`. The shim only relies on behaviour that is present in both
0.1.27 and 0.1.32: load libmpv from `NUVIO_LIBMPV_PATH`, then call `config=no`.

**Do I need administrator rights?**
No. Everything is per-user and `Program Files` is untouched.

**Does this only work with AnimeJaNai?**
No. The shim is generic. With `NUVIO_MPV_REAL_LIBRARY` unset it finds the
libmpv next to Nuvio's `player_bridge.dll`, so it works with the stock player
and needs no fixed cache path. See [CONFIG-ONLY.md](CONFIG-ONLY.md) for that
setup. The AnimeJaNai installer is the part that adds models and TensorRT.

**Which Nuvio versions were tested?**
Nuvio 0.1.27-alpha end to end in the app (1080p episode → 3840x2160, slot
switching over IPC; that run had the earlier patched bridge installed, so the
shim's `config=no` rewrite was validated by replaying the stock bridge's exact
call sequence rather than by a second in-app run). Nuvio 0.1.32-alpha was
checked against its source. The config-only path (shim v2, stock libmpv,
auto-discovery, no `NUVIO_MPV_REAL_LIBRARY`) was verified in-app on Nuvio
Desktop 1.1.32: mpv read `%APPDATA%\mpv\mpv.conf`, applied `vo=gpu-next`,
`gpu-api=d3d11`, `target-peak=730`, and used d3d11va hardware decoding.

---

## Credits and licensing

- **AnimeJaNai** — [the-database/mpv-AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai).
  Its libmpv fork, models and scripts are downloaded from its official releases
  and are **not** redistributed here. Their licences apply.
- **Nuvio Desktop** — [NuvioMedia/NuvioDesktop](https://github.com/NuvioMedia/NuvioDesktop).
- The shim, installer and helper scripts in this repository are licensed under
  **GPL-3.0** ([LICENSE](LICENSE)) and provided as-is, without warranty. Nothing
  here is endorsed by either project.
