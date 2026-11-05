# ADR 0002: F4 push-to-talk audio stream

Date: 2026-11-05

Status: accepted for host tests and a real ESP-IDF build. Nothing here has run on a board.

## Context

F3 gave the robot an authenticated USB session but no microphone path. F4 streams the INMP441 to the desktop while
the Talk button is held. The plan makes the hardware mute authoritative, forbids hidden recording, and forbids a
wake word or always-on listening at this stage. The protocol (`moby-protocol` at `9032acb`) already defines
`audio_start`, `audio_end`, robot audio frames (kind 2), `E_MUTED`, `E_AUDIO_OVERRUN` and the `audio_end` reasons,
so nothing in the protocol had to change.

## Decisions

### 1. The default image stays without audio; push to talk is an option

Alternatives:

- **Make push to talk part of the default image**, gated by mute, the button and authentication. One image to
  test, but every default build and every flashed board carries a microphone path before any hardware evidence
  exists, and `check_image.sh` could no longer prove "this image cannot record" by looking for symbols.
- **Opt in with `CONFIG_MOBY_F4_PTT_AUDIO` (default n)**, built through `sdkconfig.f4` into `build/f4`.
  Chosen. It is the safer option: the default image is provably free of capture, ring and I2S code, advertises no
  `usb_pcm`, and the F2 bench stays a third, separate image. The cost is a third image to build and test (CI does
  both). The owner can flip the default after bench evidence; that is a one-line Kconfig and test change, listed
  under decisions to confirm.

The default image still contains the session and link code for streams (they are pure and shared). It is inert
there: the `usb_pcm` capability is absent and the session needs `capture_set` and `capture_discard` callbacks, which
only the F4 image provides, so `can_start` is false forever. `check_image.sh` checks symbols per image
(`default`, `ptt`, `f2`) and `check_sources.sh` checks that the platform layer names the capture path only under
the option.

### 2. One fact for capture, derived everywhere

`stream.phase == CAPTURING` in `moby_session.c` is the only statement that the microphone may run. It is assigned
in one function (`enter_phase`), which is the only caller of the platform's `capture_set(on)`. That callback opens
the sample gate, lights the LED and wakes the I2S task, and `moby_session_recording()` and the listening face read
the same phase. A source check counts the assignment and the call sites. The LED is lit when the gate is open or the
clocks run, so it can be lit slightly longer than capture but never dark while the microphone can hear.
Listening is no longer a face the desktop can command (`E_STATE`, like muted and disconnected); a desktop face
command during capture gets `E_BUSY`. The F3 transition test and the mute-in-every-state test were adjusted for
this.

### 3. When a stream may start

On a Talk press edge only, and only if all of: the `usb_pcm` capability and both platform callbacks exist, the link
is up, the session is established and **authenticated** (so an unpaired robot never streams, as the protocol says),
the mute switch is open, the button is down, the session state is idle (not pairing, thinking or speaking) and the
transition to listening is allowed from the shown face, no earlier stream is open or owes its `audio_end`, and the
outbox can take `audio_start`. A held button never restarts a stream; a new press is needed. The press that
confirms pairing starts nothing.

Thinking and speaking are refused for now (barge-in is a decision to confirm, since playback does not exist yet).

### 4. How a stream stops

| Cause | Capture | Buffered audio | `audio_end.reason` |
| --- | --- | --- | --- |
| Button release | off at once | whole frames still sent, then end | `button_release` |
| `max_utterance_ms` (default 15 s) | off at once | same | `max_duration` |
| Hardware mute | off first, before anything is queued | discarded and zeroed | `mute` |
| Desktop `cancel` with the stream id | off | discarded | `cancel` |
| 10 s long press (pairing wipe) | off | discarded | `cancel` |
| Ring overrun | off | discarded | `error`, plus `error` event `E_AUDIO_OVERRUN` |
| Drain not collected within 1 s | n/a | discarded | `error`, `E_AUDIO_OVERRUN` |
| Capture hardware fault | off | discarded | `error`, plus `E_HW` |
| Link down, desktop silent | off | discarded | none: nobody is told |

A release keeps the audio captured while the button was held. A part of a frame (under 20 ms) at the end is dropped.
`audio_end` is owed until the outbox takes it. If `audio_start` cannot be written, the stream is dropped and its
frames never exist (the link only builds frames for a stream whose start it wrote). An `audio_end` that the transport
refuses is counted in `frames_dropped` and not retried; the desktop will see the next `hello` or time out.

### 5. Frames, sequence, ids

Frame kind 2, stream id nonzero, payload 640 bytes of `pcm_s16le` (20 ms, 16 kHz, mono). The protocol says only
"sequence counter for the stream", so this firmware starts every stream at 0 and increments by one per frame sent;
a frame that was not written does not use its number, so there are no gaps. Stream ids count from 1 and skip 0 when
they wrap. `audio_start` carries `reason` `button`, `preroll_ms` 0. `audio_end` carries `frames` and `duration_ms =
frames * 20`. The heartbeat is not held back for an idle transmitter while a stream is open (audio keeps it busy).

### 6. The sample conversion contract (the wake word work depends on this)

`pcm16 = clamp(((int32)slot >> 16) * 256 / 256)`, i.e. the top 16 bits of the left slot's 24-bit sample, arithmetic
shift, truncation toward minus infinity, no rounding or dither, unity gain, no DC removal and no filter. Right slot
ignored (L/R tied to GND). The gain is a Q8 constant (`MOBY_PCM_GAIN_Q8 = 256`) with saturation and a 16x cap, so a
later gain is a one-constant change that training must copy. It is pinned by compile-time asserts and tests against
an independent floor-division reference over a million random words.

### 7. Ring buffer and back pressure

`moby_ring` is a bounded ring of `int16` in PSRAM (64 blocks, 1.28 s, 40 KiB; no internal RAM fallback: without
PSRAM there is no audio). It is also a gate: while closed it stores nothing, so a capture task racing a mute or stop
cannot leave a sample behind. The gate opens only through `moby_ring_open`, which also drops the first 13 blocks
(260 ms) for the microphone start-up. Consumed or cleared samples are overwritten with zeros. A write that does not
fit is dropped whole and counted (`overruns`, `dropped_samples`); the link reports it and the stream ends with the
protocol's `E_AUDIO_OVERRUN`. Nothing ever blocks the face or the controls: the capture task waits at most 5 ms for
the ring lock and counts a miss as an overrun. Ending the stream on the first overrun was preferred over sending
audio with a hole, because the desktop would reject a gap anyway.

### 8. Hardware mute and defence in depth

The session stops capture on the mute reading before it queues any message. In addition the I2S task reads GPIO10
itself before every block and again after every read, drops the block it just read if the switch closed, and stops
the clocks. It also stops if the Talk pin reads released for 120 ms, and after 65 s in any case. The I2S channel is
created per stream and deleted after it, so between streams the microphone gets no BCLK or WS at all. The switch also
cuts the SD line in hardware (README, F2).

### 9. Text

Display text is decoded from UTF-8 into one-byte cells (ASCII, plus 75 glyphs for Russian and Uzbek Cyrillic and the
Uzbek turned comma). A malformed sequence is one `?`; letters are never transliterated between scripts, so Cyrillic
stays Cyrillic and Latin stays Latin. This is a separate commit and independent of audio.

## Evidence

- `tests/f4.c`, 37 tests with MSVC `/W4 /WX` (and `-Wall -Wextra -Werror -pedantic` in the Linux script): conversion,
  ring (including a 300 000 step model comparison), framing and sequences, release, timeout, cancel, drain timeout,
  mute at every one of 21 points of two scripted talks (frames after the mute: zero, ring empty, mic off, no resume on
  unmute), unpaired, bad or missing proof, before a session, on boot, no capability or callbacks, pairing, busy states,
  disconnect, silent desktop, overrun, fault, a start that cannot be written, a source that always has audio, closed
  log vocabulary, and a random walk of 72 000 operations that checks after every operation that the microphone, LED,
  gate and listening face agree, that mute and missing authentication imply no stream and an empty ring, and that
  every frame belongs to the open stream.
- `tests/text.c`: UTF-8 and glyph tests including 200 000 mutated or random inputs.
- `idf.py build size` of the default, F4 and F2 images (sizes in the README), `check_image.sh` per image.

## Not proven; needs a board

Real I2S levels and the 24-to-16-bit result on an INMP441, the 260 ms start-up time, DMA behaviour at 16 kHz with
32-bit slots, PSRAM write and read timing next to the OLED task, USB-Serial-JTAG throughput for 32 KB/s of audio
plus control traffic and a real desktop reading it, whether the 1.28 s ring is long enough, the LED, the mute switch
timing, create/delete latency of the I2S channel per press, and audio quality (the DC offset is passed on as is).
Latency from press to first frame and sustained throughput are unmeasured.

## Decisions to confirm

1. Default image without audio (this ADR) versus push to talk by default after bench tests.
2. Refuse the button while the robot is thinking or speaking.
3. End the stream on the first ring overrun rather than sending a stream with a gap.
4. Sequence numbers start at 0 per stream (the protocol does not say).
5. Unpaired robots never stream (the protocol's rule); a first-run flow must pair before talking.
6. `listening` is no longer commandable by the desktop.
7. A lost `audio_end` is counted, not retried.
