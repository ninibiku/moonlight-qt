# uu-lite mods for Moonlight V+ (moonlight-qt)

This branch contains the client-side changes developed for **uu-lite**, a self-hosted
UU-Remote-like remote play stack built on Moonlight + Sunshine + Tailscale.

Base: [`qiin2333/moonlight-qt`](https://github.com/qiin2333/moonlight-qt) v6.5.6 (`4909233`).
The changes are also available as a single patch in the uu-lite repository
(`docs/patches/moonlight-vplus-uulite.patch`).

## Features

### 1. Client-side jitter buffer (pacer latency queue)

A pacing thread in front of the renderer releases decoded frames at a steady cadence,
absorbing network jitter and bursty frame delivery:

- Settings UI: **Settings → Stream → Network and stability → Jitter buffer**
- `Jitter buffer minimum`: 0–120 ms (default 35, step 5, 0 disables)
- **Adaptive jitter buffer**: rises by 20 ms after 2 consecutive seconds with
  loss > 1 %, RTT variance > 25 or pacer late drops > 2; decays by 10 ms every
  second after 5 s of stability; capped at **140 ms** total frame delay.
- Startup grace of 5 s avoids reacting to stream-start bursts.
- Environment override `UULITE_MIN_LATENCY_MS` (used by automation/agents).
- Frame pool enlarged to `13 + 3 + 1 + 1` outstanding frames to hold the larger queue.

### 2. Tailscale integration

- **Automatic discovery**: every 60 s runs `tailscale status --json`, probes online
  peers on TCP 47989 (900 ms) and adds reachable Sunshine hosts to the normal host
  list (skips self, 10-minute per-peer cooldown).
- **Built-in Tailscale panel** in Settings:
  - status (`Connected · 100.64.x.x` / `Needs login` / `Not installed`)
  - custom **login server** (Headscale / self-hosted control server), persisted
  - **auth key** login (`tailscale up --auth-key=... [--login-server=...]`, one UAC prompt)
  - logout
  - install Tailscale from a bundled `tailscale/tailscale-setup.exe` (silent), else opens the download page
  - **import CA certificate** for self-signed control servers: imports into
    `Cert:\LocalMachine\Root` and restarts the Tailscale service (one UAC prompt)
- Proxy sessions (WAN/LAN relay) set `UULITE_DISABLE_TS_DISCOVERY=1` so Tailscale
  discovery does not rewrite the pinned `127.0.0.1` host entry.

## Build (Windows x64, tested)

- Qt 6.11.x MSVC 2022 64-bit
- Moonlight Windows dependencies (FFmpeg, SDL2, etc.) from the upstream deps archive
- Visual Studio 2022 Build Tools
- `qmake app/app.pro && nmake release`

The upstream `scripts/build-arch.bat` in this branch keeps development-machine-specific
VS paths out of the diff; use your own environment or the original script.

## Limitations

- Host-side adaptive bitrate (ABR), QR pairing and clipboard extensions require
  **Foundation Sunshine** (`AlkaidLab/foundation-sunshine`) and/or the uu-lite agent;
  they are not part of this client branch.
- The Tailscale panel controls an **installed Tailscale service**; login/logout and
  certificate import require administrator approval (UAC).
- Only Windows was validated; other platforms compile but the Tailscale GUI/installer
  paths are Windows-oriented.

## License

GPL-3.0, inherited from Moonlight. No additional restrictions are added by these changes.

## Native uu-lite controller (new)

- Settings card: panel URL, device name, DPAPI-encrypted device secret, enable switch, status.
- Async client (QNetworkAccessManager): device register, 3s heartbeat, device list, command channel.
- Native ABR: UDP 47931 echo probe (16B UUL1 packet), loss/RTT/jitter evaluation, `set_bitrate`
  command through the panel; 5s warmup + minimum 6 samples to avoid startup false drops.
- Session hook starts/stops ABR for the active host; no local proxy, direct Tailscale path.
- Requires Foundation Sunshine + uu-lite Agent on the controlled host for `set_bitrate`.

See `docs/NATIVE-UULITE.md` in the companion uu-lite repository.
- Auto-pair (verified end-to-end): discovers unpaired panel hosts, sends `pair_accept` to the host
  agent, receives `pair_pin` via the heartbeat channel and runs Moonlight's native pairing
  (`ComputerManager::pairHost`); the host's LAN address is mapped back to the Tailscale host entry.
- Native ABR verified under induced intermittent loss: 40000 → 24000 → 14400 → 8640 → 8000 kbps,
  with `set_bitrate` commands queued to the host agent (panel showed `total_changes=4`).
- ABR v2: 1s evaluation, conservative startup (70%/60%), additive recovery (+1–3 Mbps per 3s),
  4s hold, relaxed health thresholds (loss<0.8%); verified 21→27.9 Mbps in 30s at 30M preset
  and 31.2→46.2 Mbps at 52M preset without startup freeze.
- ABR v3: Pacer reports real video frame loss to ABR; decisions use max(probe loss, video-loss EWMA),
  one drop per 5–8s hold, learned 85% safe ceiling, 8s startup warmup, 30s ceiling probing.
  Verified 30M preset stable at 25.5→26.8 Mbps with only 8 IDR waits in 70s.

