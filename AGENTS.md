# gamestream-test-client: details

What README.md leaves out: every option, the summary and JSON fields, how loss is injected and what is counted
where, the caveats when reading the numbers, and the layout. README.md is the short version for people.

## Layout

- `client/src/main.c`: command line, pairing/list/launch through libgamestream, the stream, the summary and
  `--expect`.
- `client/src/loss.c`: the loss injector, the `recvfrom` wrapper, and the counters for the requests the client sends.
- `client/src/queue.c`: the delay queue (latency, jitter, reordering, duplicates, the bandwidth cap).
- `client/src/click.c`: clicks and flash detection for `--click-every`.
- `client/src/pattern.c`: framestamp's picture (the same function as its `shaders/pattern.frag`, in the same integer
  arithmetic) and `--check-picture`'s comparison; `client/tests/test_pattern.c` checks it against framestamp's own test
  vectors. Change it only with framestamp, and bump moonshine-test-headless's pins of both together.
- `client/src/decode.c`: libavcodec decoding (Moonlight's FFmpeg path), in software or on VAAPI, corrupt-frame flags.
- `client/src/record.c`: per-frame records, intervals, stalls, the CSV.
- `client/src/test_client.h`: shared declarations.
- `client/tests/`: `test_loss.c`, `test_click.c` and `test_expect.c`, with the moonlight-common-c symbols they use stubbed;
  `test_expect.c` includes `main.c` with `#define main client_main`.
- `third_party/moonlight-common-c` (the protocol) and `third_party/moonlight-embedded` (only its libgamestream, for
  pairing, the app list and launching): git submodules, built from source by `client/CMakeLists.txt`.
- `Containerfile`: Arch build and runtime (the harness image's stack): FFmpeg with the H.264, HEVC and AV1 decoders, Mesa's and
  Intel's VAAPI drivers, libnl (Wi-Fi link details) and hwdata (the PCI database `probe` names GPUs from).
- `bin/gamestream-test-client`: the installed command (and `gtc`), a shell script that runs the image with podman:
  host network, `$HOME` passed through, `~/.config/gamestream-test-client` and the current directory mounted at the
  same paths (so keys, and files from `--json`/`--csv`/`--dump` under the current directory, are where a native
  install would put them), SELinux labels off for those mounts instead of relabeling them. `GTC_IMAGE` runs another
  image, e.g. a local build: `GTC_IMAGE=localhost/gamestream-test-client gtc --help`. Paths outside the current
  directory and the config directory aren't visible to the client.
- `.github/workflows/image.yml`: builds the image on every pull request, and pushes it to
  `ghcr.io/porkloin/gamestream-test-client` (`latest` and `sha-<commit>`) from `main` and weekly.

## Command line

```text
gamestream-test-client [--host H] [--port PORT] [--keys DIR] [-v] COMMAND
  --host H, -H              host to reach (127.0.0.1)
  --port PORT               control port (47989)
  --keys DIR                keys directory ($HOME/.config/gamestream-test-client)
  -v, --verbose             the protocol library's log on stderr
  -h, --help                this text, exiting 0
  -V, --version             the commit this client is built from, exiting 0
  pair PIN                  pair with PIN (enter it on the host)
  list                      list the host's apps
  probe [--sysfs DIR] [--os-release FILE]
                            describe this machine as JSON, without a host (see Probe)
  stream APP [options]      stream APP (its name, or the id list prints) and print a summary
    --codec h264|hevc|av1   (hevc)
    --hdr                   request HDR (10-bit HEVC or AV1)
    --decode full|hw|none   decode every picture in software, on a VAAPI device, or
                            skip it (full)
    --decode-device PATH    the VAAPI render node for --decode hw (default: the GPU that
                            decodes the codec, see --avoid-gpu, with the most video memory)
    --avoid-gpu VENDOR:DEVICE  decode on another GPU than this one when one can (the host's
                            encoder, over loopback)
    --decode-delay MS       take at least MS to decode each frame, like a slow decoder
    --dump PATH             write the received bitstream to PATH
    --mode WxHxFPS          (1920x1080x60)
    --bitrate KBPS          (20000)
    --packet-size BYTES     video payload per packet the host sends, and stream as on a LAN (by
                            default 1392, or 1024 to a host outside the private ranges, as
                            Moonlight decides)
    --seconds N             (20)
    --display HZ            the refresh rate of the display the summary shows the frames on, through
                            moonlight-qt's V-Sync and frame pacing (the stream's frame rate)
    --csv PATH              per-frame record
    --stats PATH            per-frame JSON lines as frames decode, times by the wall clock
    --clock HOST[:PORT]     measure the host's clock by NTP before and after the stream, and give
                            --stats and --csv times on it
    --json PATH             write the summary as one JSON object
    --expect EXPR           fail with 3 unless a summary value holds: NAME OP NUMBER, OP one of <= >= == < >
    --no-rfi                don't offer reference frame invalidation
    --rfi-h264              offer it for H.264 too, as some Android decoders do (desktop
                            Moonlight offers it for HEVC and AV1 only)
    --no-ack                don't acknowledge frames the host asks about
    --quit                  quit the app on the host afterwards
    --seed N                loss seed (1)
    --loss-random PERCENT   drop each video packet with this probability
    --loss-frame-every N    drop every packet of every Nth frame
    --loss-frame-run N      ... and of the N-1 frames after it (1)
    --loss-warmup N         frames before the first frame drop (120)
    --loss-burst N:MS       drop N consecutive packets every MS ms
    --loss-bursty PCT:LEN   lose PCT% of packets in runs of LEN on average
                            (Gilbert-Elliott, like Wi-Fi)
    --delay MS[:JITTER]     deliver video packets MS later, give or take JITTER
                            (in order)
    --reorder PCT:MS        hold PCT% of packets MS longer, so later ones overtake
    --duplicate PCT         deliver PCT% of packets twice
    --rate KBPS[:QUEUE_MS]  a link of KBPS that drops packets once QUEUE_MS of
                            them are waiting (50)
    --click-every MS        click in the middle of the stream every MS (plus up to
                            a frame at random) and time each until the picture
                            flashes bright, e.g. with clickflash as the app
```

Exit codes: 0 ok, 1 couldn't stream or stopped early, 2 usage, 3 an expectation failed.

Software decoding uses 4 slice threads, which a one-slice stream can't use: one core decodes, about 67 fps of 4K
HEVC on a 16-core desktop CPU, so above that the client falls behind, its queue drops frames and it asks for keyframes.
`--decode hw` decodes on a VAAPI render node through libavcodec's VAAPI hwaccel; each frame counts as decoded once
`vaSyncSurface` returns, and click streams copy the picture back for the flash detector. Without `--decode-device`
the client picks the node (`probe_pick_decoder`): one whose driver decodes the codec (VLD for H.264, HEVC Main, HEVC
Main10 with `--hdr`, AV1 Profile 0), another than `--avoid-gpu` when there is one, then the most video memory; when
none can, it says so and decodes in software. The summary's `decoder` records what it used.
`--decode none` skips decoding; only the protocol side is measured then.

## Probe

`probe` (`client/src/probe.c`) prints one JSON object describing the machine, read from the kernel and the drivers
rather than other tools: `hostname` (`/proc/sys/kernel/hostname`), `model` (DMI product name, the board's when the
product is a vendor placeholder, or the device tree's model), `cpu`, `memory_gb`, `os` (os-release's
`PRETTY_NAME`), `kernel`, `network` (the default route's link from `/proc/net/route`, else every physical link
that's up: `iface`, `kind` `wired`/`wifi`, `speed_mbps`, and for Wi-Fi in this network namespace `wifi` from
nl80211: `standard` 4–7 from the tx rate's MCS kind, `freq_mhz`, `width_mhz`, `signal_dbm`, `tx_mbps`), and `gpus`:
each render node's `device`, `pci` (vendor:device), `name`, `va_driver`, `vram_mb` (amdgpu's) and `decodes`
(`h264`, `hevc`, `hevc10`, `av1`). VAAPI is asked about each node this process can open (`vaQueryVendorString`,
then the VLD entrypoints): Mesa's "Mesa Gallium driver X for NAME (radeonsi, …)" gives `radeonsi Mesa X` and the
name unless it's the generic "AMD Radeon Graphics"; Intel's iHD gives `iHD X` and no name. A GPU still unnamed is
named from the PCI database: "Granite Ridge [Radeon Graphics]" from AMD is "AMD Radeon Graphics (Granite Ridge)".
`--sysfs DIR` reads another machine's sysfs (a host's, mounted into a container): links and DMI are that machine's,
Wi-Fi details aren't. Run it in the image with `--network host --uts=host --device /dev/dri` and the host's
os-release mounted to describe the host.

## Keys and pairing

The keys directory holds the client certificate and key and the client's unique id, all generated on first use (the
directory is created with mode 0700). A pairing belongs to a keys directory. The pairing request comes from
libgamestream, whose device name is fixed. A script can pair without a person by starting `pair 0000` and posting
the PIN with the id from `<keys>/uniqueid.dat` to the host; for Moonshine:
`curl -X POST http://localhost:47989/submit-pin -d "uniqueid=$(cat <keys>/uniqueid.dat)&pin=0000"`.

## Loss injection

moonlight-common-c's `recvfrom`, `sendto` and `connectionReceivedCompleteFrame` are linked to wrappers in `loss.c`
(`-Wl,--wrap=...`). Video packets are dropped after the kernel receives them and before moonlight-common-c sees
them, so to the library they're lost on the network, encrypted video included. The same `--seed` drops the same
packets every run. Only the video socket is affected.

- `--loss-frame-every N` drops every packet of every Nth frame after `--loss-warmup` frames; `--loss-frame-run N`
  extends each drop to N frames.
- `--loss-random P` drops each video packet with probability P %.
- `--loss-burst N:MS` drops N consecutive packets every MS milliseconds.
- `--loss-bursty PCT:LEN` is the two-state Gilbert-Elliott model: each packet either keeps the current state or
  switches between "passing" and "losing", with chances set so that PCT % of packets are lost in runs of LEN packets
  on average. It's seeded like the rest, so the same seed loses the same packets.
- `--no-ack` makes the `connectionReceivedCompleteFrame` wrapper clear the long-term-reference flag, so the client
  never acknowledges a frame, as a client without long-term reference support.

## Summary

```text
stream app="vkcube" codec=hevc hdr=0 mode=1920x1080x60 bitrate=20000 rfi=1
frames=844 seconds=14.3 fps=59.2 idr=11 missing=15 decode_errors=0 corrupt=0 stalls=5 stalled_ms=333
interval_ms p50=16.67 p95=16.74 p99=16.81 max=66.68
loss video_packets=31750 dropped_packets=185 dropped_frames=5 seed=1
ack_requested_frames=0
requests idr=0 rfi=14 recovery=5
```

`--json` writes the same as one object with these fields:

| Field | |
|---|---|
| `client_version` | the commit the client is built from (`--version`: CMake's `GTC_COMMIT`, which CI and moonshine-test-headless set; `dev` otherwise) |
| `frames`, `seconds`, `fps` | frames handed to the decoder, time from the first frame to the end of the stream, rate over that time |
| `idr` | keyframes received |
| `missing` | frame numbers skipped between the frames handed to the decoder |
| `decode_hw` | 1 when it decoded on a VAAPI device |
| `decoder` | how: `{"how": "vaapi", "gpu": GPU}` (a GPU as `probe` writes it), `{"how": "software", "driver": "libavcodec 61.19.100"}` or `{"how": "none"}` |
| `decode_ms_p50`, `decode_ms_p95`, `decode_ms_p99`, `decode_ms_max` | from handing a frame to the decoder to its picture being ready (null with `--decode none`); a p95 near the frame interval means the client is the bottleneck |
| `display_hz`, `display_shown`, `display_dropped`, `display_judder`, `display_latency_ms_p50`, `display_latency_ms_p99` | the decoded frames replayed against a `display_hz` refresh clock (`--display`, else the stream's fps; refreshes half a refresh after where frames typically arrive within one, so the phase is the same from run to run) through moonlight-qt's V-Sync presenting: a frame shows at the next refresh, the render queue keeps the newest (up to 2 while it recently ran dry); judder counts frames held more refreshes than ceil(hz/fps); latency is decoded to on screen, rendering taken as instant (null with `--decode none`) |
| `paced_shown`, `paced_dropped`, `paced_judder`, `paced_latency_ms_p50`, `paced_latency_ms_p99` | the same through moonlight-qt's frame pacer: at each refresh it takes the oldest frame (waiting up to the refresh less 3 ms for one), shown at the next refresh; it keeps 1 queued, or 3 while it recently held 1 or none at a stream rate at or above the refresh |
| `decode_errors` | frames libavcodec failed to decode |
| `corrupt` | frames libavcodec decoded but flagged corrupt |
| `stalls`, `stalled_ms` | gaps longer than three frame intervals (including the one from the last frame to the end), and their total |
| `interval_ms_p50/p95/p99/max` | time between frames (null when there were no intervals) |
| `video_packets`, `dropped_packets`, `dropped_frames`, `seed` | the loss injector's counts |
| `duplicated_packets`, `reordered_packets` | the delay queue's counts |
| `clicks`, `click_flashes`, `click_missed` | with `--click-every`: clicks sent, flashes seen, clicks without one (null without) |
| `click_ms_mean`, `click_ms_p50`, `click_ms_p95`, `click_ms_p99`, `click_ms_max` | click to flash decoded (null without flashes) |
| `click_ms`, `click_received_ms` | every flash's click-to-decoded and click-to-received time, as arrays (only with `--click-every`) |
| `recovery_frames` | recovery P-frames after an invalidation (moonlight-common-c's `Next post-invalidation frame is: N (P-frame)`); a keyframe instead counts in `idr` |
| `ack_requested_frames` | frames the host asked the client to acknowledge (counted in the `connectionReceivedCompleteFrame` wrapper) |
| `idr_requests`, `rfi_requests` | keyframe and reference-frame-invalidation requests the client sent |
| `idr_requests_start`, `idr_requests_loss`, `idr_requests_drop_limit`, `idr_requests_rfi_overflow`, `idr_requests_client_decoder`, `idr_requests_client_queue` | the keyframe requests by why: moonlight-common-c logs a reason, then asks, and requests made before one goes out are sent as one, so each takes the most telling reason logged since the last. `start`: none logged (the stream's first). `loss`: frames lost that it couldn't recover by invalidation (no RFI for the codec, or still waiting for a keyframe). `drop_limit`: 120 frames in a row dropped while waiting to recover. `rfi_overflow`: more lost ranges than it can ask to invalidate. `client_decoder`: the decoder failed a frame (`DR_NEED_IDR`). `client_queue`: moonlight-common-c's queue of 15 frames waiting for the decoder overflowed, so the decoder couldn't keep up. The client's own reasons win over the stream's: the last two are this machine's doing, not the host's |
| `picture_checked`, `picture_unreadable`, `picture_corrupt` | with `--check-picture`: frames checked against framestamp's, ones whose stamp couldn't be read (not framestamp, or too broken), and corrupt ones: the worst of 84 tiles off by more than `--picture-threshold` (20) luma levels on average from the frame framestamp drew (null without) |
| `picture_corrupt_run_max`, `picture_recovery_corrupt_max` | the longest run of corrupt frames, and the longest from a recovery frame or keyframe on (1: that frame itself didn't make the picture whole) |
| `picture_repeats` | frames whose stamp was the same as the frame before's (the compositor captured one of framestamp's frames twice) |
| `picture_worst_p99`, `picture_check_ms_p99` | the worst tile's error at p99 (clean streams are a level or two), and the time the check takes a frame, after the decode (it isn't counted as decoding). It runs on a thread of its own: the decoder hands each decoded frame over (a reference, so its surface stays alive; the VAAPI pool gets that many more surfaces) and waits for nothing. A VAAPI frame is read back only in the strips the check reads (the stamp's rows and 13 through each row of tiles, about a twentieth of the frame), and whole only for `--picture-dump` |
| `picture_skipped` | frames left unchecked because the checking thread was still busy with the 3 before them (it never holds the decoder up) |
| `queued_max` | the most frames waiting behind one as it went to the decoder (moonlight-common-c's queue holds 15; it overflows past that) |

`--expect` names are these fields. `--csv` writes one row per frame handed to the decoder; its `recovery` column is 1 for
the frames `recovery_frames` counts, `receive_unix_us` is `receive_us` on the wall clock, and `queued` is how many frames were waiting behind it for the
decoder. `--stats` writes the
same frames as JSON lines, flushed as each is written, with every time on the wall clock (`receive_unix_us`,
`enqueue_unix_us`, `decode_start_unix_us`, `decode_end_unix_us`) and the frame's number, which is Moonshine's frame
index: `moonshine-otel client` turns them into spans in the frame's trace beside Moonshine's. The wall clock offset
from moonlight-common-c's clock is taken for every frame. Events go in the same stream as they happen, each line
written whole by any thread: `drop` (the loss model started dropping `frame_number`, from the network thread),
`freeze` (`missing` frames never reached the decoder between `after_frame` and `frame_number`; `duration_us` between
their first packets; `recovered_by` recovery_frame, keyframe or neither), `click` (its id, the frame the flash was decoded from as `flash_frame`, and when the click was sent and its flash
decoded and received) and `second` (each second of the stream from the first frame's arrival: its `bytes` and
`frames`, the busiest sliding one-second window so far as `peak_window_bytes`, as the report measures it, and the
`requested_kbps`), `idr_request` (a keyframe request sent, its `cause` as in `idr_requests_*`), and with `--check-picture` a
`picture` line per decoded frame (`readable`, the `stamp` it read, the `worst` and `mean` tile error, `corrupt`,
`recovery_run`: corrupt frames in a row since a recovery frame or keyframe, and `check_ms`). Frame lines
carry `queued` too. The first line is `client` (its `version`, as `--version`). Once the stream ends, the display model
adds a `display` line per decoded frame (its `frame_number`, `decoded_unix_us`, and for V-Sync and the frame pacer
`vsync_wait_us`/`paced_wait_us`, decoded to on screen, null when the frame was dropped and never shown, and
`vsync_judder`/`paced_judder`, true when it went up a refresh or more later than the stream's rate needs because the
frame before it held), then one `display_summary` (`hz`, `fps`, and `vsync` and `paced` objects with the `--json`
display fields: `shown`, `dropped`, `judder`, `latency_ms_p50`, `latency_ms_p99`). They come at the end because the
model replays the whole stream; a reader that stops at the client's exit has to read to the end of the file first.
`loss.c` and `click.c` report through the `loss_on_drop` and `click_on_flash` hooks, so
their tests build without the recorder. Each click's id travels with it: before the press, the pointer goes to the
middle of cell `id` of a 64 × 32 grid over the stream (`click_position`), and the app reads the id back from where
the pointer is over its window (row × 64 + column), so ids survive scaling and a lost input doesn't shift the rest.
Ids count clicks from 1, modulo 2048.

## The host's clock

Frame times from both machines only line up if they're on one clock, and the client never sets a clock (a container
can't have its own, and the machine's isn't ours to change). `clock-serve [--port N]` answers NTP (RFC 5905, port 123
by default) with the machine's clock; run it where the host is. `stream --clock HOST[:PORT]` sends 16 NTP requests
before the stream and 16 after (`client/src/clock.c`) and keeps the fastest exchange: offset `((t2 − t1) + (t3 −
t4)) / 2`, true to within half its round trip. From the first measurement on, every wall clock time in `--stats`
and `--csv` is on the host's clock (`record_set_clock_offset`), and `clock` lines record both measurements
(`when`, `offset_us`, `uncertainty_us`, `samples`, `applied_offset_us`; `ok: false` when the server didn't answer,
and the times stay on this machine's clock); the stdout summary gains a `clock` line. Any NTP server works, but the
point is the host's own clock: a public server only tells how far each machine is from it.

## Delay queue

`--delay`, `--reorder`, `--duplicate` and `--rate` hold video packets in a queue (`client/src/queue.c`) after the
loss model, and the `recvfrom` wrapper hands each to moonlight-common-c at its delivery time (to within a
millisecond). In order:

- `--rate KBPS[:QUEUE_MS]`: packets leave a link of KBPS one after another; a packet that would wait more than
  QUEUE_MS (50) behind the ones ahead is dropped, like a full router buffer. Drops count in `dropped_packets`.
- `--delay MS[:JITTER]`: each packet is delivered MS later, plus a uniform offset within ±JITTER, but never before a
  packet that arrived earlier, so jitter varies the delay without reordering.
- `--reorder PCT:MS`: PCT % of packets are held MS longer, so the ones behind them overtake (`reordered_packets`).
- `--duplicate PCT`: PCT % of packets are delivered twice in a row (`duplicated_packets`).

The queue draws its own seeded random numbers, so turning it on doesn't change which packets the loss model drops.

## Click-to-flash

`--click-every MS` (at least 100) waits 2 s, puts the pointer in the middle of the stream, then sends a left click
(press, 20 ms, release) every MS plus a random part of a frame, so clicks don't lock to the frame rate. Every
decoded picture's brightness is read from a 32x32 patch in its middle (luma, 0 to 1); the first picture above 0.5
after a click is its flash. Each flash gives two samples: click to the frame received (moonlight-common-c has the
whole frame) and click to the frame decoded. A click with no flash within 1 s, or followed by another click before
its flash, counts as missed. It needs the pictures decoded, so not `--decode none`.

Times are `CLOCK_MONOTONIC_RAW`, the clock [clickflash](https://github.com/porkloin/clickflash) logs with, so its
log lines up with the client's clicks: clickflash's `input` events show when the click reached the app, and its
`flash` events when the white frame was submitted. What's measured stops at the decoded picture: no display.

## Other behaviors

- `--packet-size BYTES` is the `packetSize` the client asks for at launch; the host splits each frame into packets
  of that much video payload, so smaller sizes mean more packets per frame, as on a link with a small MTU.
- `--decode-delay MS` holds the decoder callback until each frame has taken MS since it was handed over (with or
  without `--decode none`). moonlight-common-c queues frames for the decoder; when the queue overflows it drops them
  and asks the host for a keyframe, as a Moonlight client on a slow device does.

## Reading the numbers

- It offers reference frame invalidation on every codec. Many real Moonlight clients don't, so runs here recover more
  often than on those clients; `--no-rfi` behaves like them.
- `corrupt==0` doesn't prove the picture was right after a loss: decoders conceal missing references.
- moonlight-common-c only acknowledges frames when it treats the host as Sunshine-compatible (by its app version
  string), so `ack_requested_frames` stays 0 on other hosts.

## Building and testing

```sh
git submodule update --init --recursive
cmake -S client -B build && cmake --build build && ctest --test-dir build
podman build -t gamestream-test-client .
```

CI builds the client and runs ctest on every push.
