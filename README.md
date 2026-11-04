# gamestream-test-client

Headless Moonlight client with built-in chaos monkeys, for testing GameStream hosts (Moonshine, Sunshine, Apollo).

Built on [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c), so hosts see a real Moonlight
client.

> [!NOTE]
> It's a test client, not a player: it decodes (in software, or on a VAAPI GPU with `--decode hw`) but never
> displays, and its network trouble is simulated.
> What it measures says how a host behaves under those conditions, not what a real Moonlight client on a real
> network and display will see.

Just want to test a Moonshine PR on your GPU? Use [moonshine-test-headless](https://github.com/porkloin/moonshine-test-headless).

## Behaviors

- **Packet loss**: drop whole frames, random packets, bursts, or Wi-Fi-style bursty loss. Same seed, same drops,
  every run.
- **Latency and jitter**: deliver video late, by a varying amount.
- **Reordering and duplicates**: let some packets arrive out of order, or twice.
- **Bandwidth cap**: a link slower than the stream, with a router's buffer that fills up and drops.
- **Small packets**: ask the host for smaller video packets, as on a link with a small MTU.
- **Hardware decode**: decode on a VAAPI GPU (AMD, Intel) instead of the CPU, for modes software can't keep up with.
- **Slow decoder**: take longer to decode each frame than the stream allows.
- **Click-to-flash latency**: click every so often and time how long until the stream shows the flash, with
  [clickflash](https://github.com/porkloin/clickflash) as the app.
- **Old-client mode**: no reference frame invalidation (`--no-rfi`), no frame acks (`--no-ack`).

## Prerequisites

- podman
- `~/.local/bin` in your `PATH`
- A GameStream host to test: Moonshine, Sunshine or Apollo

## Install

```sh
curl -fL https://raw.githubusercontent.com/porkloin/gamestream-test-client/main/bin/gamestream-test-client \
  -o ~/.local/bin/gamestream-test-client
chmod +x ~/.local/bin/gamestream-test-client && ln -sf gamestream-test-client ~/.local/bin/gtc
```

That's `gamestream-test-client`, and `gtc` for short. It runs the client's container image, which podman downloads on
first use; `podman pull ghcr.io/porkloin/gamestream-test-client` updates it.

## Quick start

```sh
gtc -H 192.168.1.20 pair 1234      # enter 1234 on the host
gtc -H 192.168.1.20 list
gtc -H 192.168.1.20 stream Desktop --loss-frame-every 120
```

You get a summary: frames, keyframes, decode errors, stalls, recovery requests.

- `--json` / `--csv` to save it
- `--expect 'idr<=1'` to pass or fail a run (exit 3)
- `--help` for everything else

Each behavior is a `stream` option:

| Behavior | Option | Example |
|---|---|---|
| Drop whole frames | `--loss-frame-every N` (`--loss-frame-run N`) | `--loss-frame-every 120 --loss-frame-run 3` |
| Random packet loss | `--loss-random PERCENT` | `--loss-random 1` |
| Loss bursts | `--loss-burst N:MS` | `--loss-burst 10:500` |
| Wi-Fi-style loss | `--loss-bursty PCT:LEN` | `--loss-bursty 2:8` |
| Latency and jitter | `--delay MS[:JITTER]` | `--delay 30:20` |
| Reordering | `--reorder PCT:MS` | `--reorder 5:10` |
| Duplicates | `--duplicate PCT` | `--duplicate 2` |
| Bandwidth cap | `--rate KBPS[:QUEUE_MS]` | `--rate 15000:50` |
| Small packets | `--packet-size BYTES` | `--packet-size 1024` |
| Hardware decode | `--decode hw` (picks the GPU; `--decode-device PATH` to choose) | `--decode hw --decode-device /dev/dri/renderD129` |
| Slow decoder | `--decode-delay MS` | `--decode-delay 20` |
| Click-to-flash latency | `--click-every MS` | `--click-every 1000` |
| Old client | `--no-rfi`, `--no-ack` | `--no-rfi` |

Loss options use `--seed N` (same seed, same drops), and frame drops start after `--loss-warmup N` frames (120).

## Ideas for future behaviors

- Reconnect and resume mid-stream
- Picture checks against a test pattern, to catch real corruption
- Audio loss and decode checks

## License

GPL-3.0.
