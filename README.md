# gamestream-test-client

Headless Moonlight client with built-in chaos monkeys, for testing GameStream hosts (Moonshine, Sunshine, Apollo).

Built on [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c), so hosts see a real Moonlight
client.

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

## Quick start

```sh
gtc -H 192.168.1.20 pair 1234      # enter 1234 on the host
gtc -H 192.168.1.20 list
gtc -H 192.168.1.20 stream Desktop --loss-frame-every 120
```

You get a summary: frames, keyframes, decode errors, stalls, recovery requests, and what a display would show.

- `--json` / `--csv` to save it
- `--expect 'idr<=1'` to pass or fail a run (exit 3)
- `--help` for everything else

### Update

```sh
podman pull ghcr.io/porkloin/gamestream-test-client
```

## Stream Behavior Flags

| Behavior | Flag | Example | Notes |
|---|---|---|---|
| Drop whole frames | `--loss-frame-every N` | `--loss-frame-every 120 --loss-frame-run 3` | Every packet of every Nth frame, and the frames after it with `--loss-frame-run`. Starts after `--loss-warmup N` frames (120) |
| Random packet loss | `--loss-random PERCENT` | `--loss-random 1` | |
| Loss bursts | `--loss-burst N:MS` | `--loss-burst 10:500` | N packets in a row, every MS ms |
| Wi-Fi-style loss | `--loss-bursty PCT:LEN` | `--loss-bursty 2:8` | PCT% of packets, lost in runs of LEN on average (Gilbert-Elliott) |
| Latency and jitter | `--delay MS[:JITTER]` | `--delay 30:20` | Packets stay in order |
| Reordering | `--reorder PCT:MS` | `--reorder 5:10` | PCT% of packets held MS longer, so later ones overtake |
| Duplicates | `--duplicate PCT` | `--duplicate 2` | |
| Bandwidth cap | `--rate KBPS[:QUEUE_MS]` | `--rate 15000:50` | A router buffer of QUEUE_MS fills, then drops |
| Small packets | `--packet-size BYTES` | `--packet-size 1024` | Also streams as on a LAN. Without it, LAN or remote follows the host's address, as in Moonlight |
| Hardware decode | `--decode hw` | `--decode hw --decode-device /dev/dri/renderD129` | VAAPI (AMD, Intel); `--decode-device` picks the GPU |
| Slow decoder | `--decode-delay MS` | `--decode-delay 20` | At least MS per frame |
| Click-to-flash latency | `--click-every MS` | `--click-every 1000` | With [clickflash](https://github.com/porkloin/clickflash) as the app |
| Display refresh rate | `--display HZ` | `--display 144` | The display the summary models (default: the stream's frame rate) |
| Picture checks | `--check-picture` | `stream framestamp --check-picture` | With [framestamp](https://github.com/porkloin/framestamp) as the app: every decoded frame is drawn again from its stamp and compared, so corruption the decoder doesn't report shows. `--picture-dump DIR` saves the first corrupt frames beside what they should be |
| Old client | `--no-rfi`, `--no-ack` | `--no-rfi` | No reference frame invalidation, no frame acks |
| H.264 invalidation | `--rfi-h264` | `--rfi-h264` | As some Android decoders offer |

Loss options use `--seed N`: same seed, same drops, every run.

## Compared to Moonlight

| | Moonlight | gamestream-test-client |
|---|---|---|
| Protocol: RTSP, control stream, FEC, IDR and invalidation requests | moonlight-common-c | The same moonlight-common-c |
| LAN or remote | From the host's address; Tailscale counts as remote (1024-byte packets) | The same; `--packet-size` forces LAN |
| Reference frame invalidation | HEVC and AV1 (desktop, iOS); H.264 on some Android decoders | HEVC and AV1; `--rfi-h264` adds H.264 |
| Decoding | D3D11VA, VideoToolbox, MediaCodec, VAAPI or software | libavcodec on VAAPI or in software (Linux) |
| Display | Renders with V-Sync, optionally through a frame pacer | Modelled from decode times through moonlight-qt's V-Sync and frame pacer (`--display`); rendering counts as instant |
| Network | Real | Simulated before moonlight-common-c sees the packets, or real when run on another machine |
| Audio | Played | Received and discarded |
| Input | Mouse, keyboard, controllers | Clicks only (`--click-every`) |

Want to test a Moonshine PR on your GPU? Use [moonshine-test-headless](https://github.com/porkloin/moonshine-test-headless).

## Ideas for future behaviors

- Reconnect and resume mid-stream
- Picture checks against a test pattern, to catch real corruption
- Audio loss and decode checks

## License

GPL-3.0.
