# F3 USB link, session and pairing

Date 2026-11-04

Status candidate. Everything below is proven on the host and by a real ESP-IDF
build. None of it has run on a board.

## Alternatives

Transport:

- **USB-Serial-JTAG** (the native USB-C port, no extra component, no extra
  descriptors). Chosen first.
- **TinyUSB CDC.** Gives a proper DTR signal for "the port is open" and
  control of the descriptors, at the cost of a component, more RAM and a
  second device identity. Not built. The `moby_transport_t` interface
  (`connected`, `read`, `write`, `tx_idle`) lets it be added without touching
  the link or session, so the two can be compared on hardware.
- **UART0 for the protocol.** Rejected: UART0 is the console, and mixing logs
  with frames is exactly what must not happen.

Open decision 14 (which USB transport ships) **stays open** until it is
measured on a board: hello delivery after the port opens, throughput of the
640-byte PCM frames of a later stage, behavior when the host closes the port,
and whether the host opening the port resets the chip through DTR/RTS.

Desktop liveness:

- Wait for a desktop heartbeat. The protocol has none.
- Count robot heartbeats that no desktop frame answered. Chosen.
- Rely on `usb_serial_jtag_is_connected()`. It says the cable is enumerated,
  not that a program has the port open.

Authentication of commands on a paired robot: audio only (the protocol text),
or every command. Chosen: every command.

## Choice

- Frames over USB-Serial-JTAG with the vendored `moby_usb_codec` (see
  `components/moby_protocol/PROVENANCE.md`; a temporary copy of commit
  `9032acb`). A bounded `moby_decoder` (1045 bytes) is the only RX buffer; an
  oversized segment is dropped through the next delimiter. The driver's rings
  are 1024 bytes RX and 2048 bytes TX, so one 1046-byte frame fits and the
  driver accepts a write whole or not at all.
- The console stays on UART0 (`CONFIG_ESP_CONSOLE_UART_DEFAULT`,
  `CONFIG_ESP_CONSOLE_SECONDARY_NONE`). A build-time `#error` and
  `tests/check_sources.sh` refuse a USB console.
- The ROM prints boot text on this port at reset. The robot therefore writes a
  lone delimiter first on every connection, and the host should do the same.
- The session (`moby_session.c`) is pure C: BOOT, DISCONNECTED, PAIRING, IDLE,
  LISTENING, THINKING, SPEAKING, with the hardware mute and a timed error face
  as overlays. The shown expression is computed as mute, then no session, then
  error, then the commanded face, so the mute wins in every state.
- Handshake: hello at once on connect, resent every 1 s with the same id and
  nonce, a new attempt (fresh nonce and id) every 3 s until `hello_ack`.
  Heartbeat every 2 s after the ack.
- Liveness: every CRC-valid desktop frame counts as proof of life. Three
  heartbeats in a row without one, and the fourth period ends the session:
  buffers clear, the face shows disconnected, hello restarts. **The protocol
  has no desktop heartbeat, so a desktop must send some command at least every
  2 s** (an empty `set_config` is a valid one) or it is dropped after about
  8 s. The protocol should say so.
- Commands: a paired robot obeys only an authenticated session (bad or missing
  proof gives an `error` event with `E_STATE` and `E_STATE` to commands); an
  unpaired robot's open session takes `expression`, `display_text`,
  `set_config`, `pair_begin`. `servo_pose` and `audio_play_*` get
  `E_UNSUPPORTED` because the capability is absent (checked before
  authentication). `cancel` gets `E_STATE`: this stage opens no stream.
- Expressions: the generated `moby_can_transition` decides, evaluated on the
  shown expression. `muted` and `disconnected` are owned by the robot, so the
  desktop asking for them gets `E_STATE`; any expression command while muted
  gets `E_MUTED` and changes nothing. Known animations are `none` and `blink`;
  another gets `E_UNSUPPORTED` and no change. `duration_ms` above zero returns
  the face to idle afterwards. Unmuting goes to idle.
- `display_text` shows printable ASCII (anything else as `?`), at most 64
  characters in rows of 21, until its `ttl_ms`. It is hidden under the mute,
  disconnected and error faces and cleared when the session ends.
- Pairing: `pair_begin` is honoured only for an unpaired robot in idle,
  unmuted. A paired robot answers `E_STATE` (this rule is already in
  `protocol.md`, step 4), so a local process cannot replace the key. A talk
  press while waiting sends `pair_button_confirmed` and no `button_event`.
  `pair_set_key` is accepted only after that. The window is the
  `pair_begin` timeout; on expiry the robot sends `E_TIMEOUT`. A mute or a
  lost link cancels it. A 10 s hold sends `long_press` and `pair_reset` and
  erases the key; if the erase fails the robot stays paired and sends `E_HW`.
- Identity: `device_id` is `dev_` and 32 hex digits from 128 random bits,
  made on first boot and kept in NVS; the `pair_key` is a 32-byte NVS blob.
  Randomness comes from `esp_fill_random` with the SAR ADC entropy source
  enabled (`bootloader_random_enable`) for the whole run.
- Proof: `HMAC-SHA256(pair_key, nonce || device_id || 0x00 || session_id)`
  through `mbedtls_md_hmac`, compared with `moby_ct_equal`, against a nonce
  made fresh for each attempt. The nonce is wiped when the session starts, the
  key text in the parsed JSON is wiped before cJSON frees it, and no log line
  carries a key, nonce or proof.
- The default image advertises no capability, links no audio or I2S code, and
  never records. The F2 bench image does not include the link.
- Protocol errors from the wire are sent as `error` events at most once per
  500 ms.

## Evidence

- `tests/vectors.c` runs all six vendored vector files through the firmware's
  own code: 11 frame, 95 control (every row classified the way the protocol
  says, 14 robot messages rebuilt byte for byte), 16 COBS, 6 RFC 4231 HMAC and
  4 session-proof vectors computed by the same `moby_hmac.c` that the device
  links (mbedTLS 3.6.7 `md.c` and `sha256.c` from the ESP-IDF tree, host
  config), and the 9 transition rows.
- `tests/f3.c`: 42 tests covering the handshake, retries, window, heartbeat
  and liveness timing, reconnect, clock wrap, corrupted CRC, oversized-frame
  and ROM-text recovery, bad frame version, desktop audio, error rate limit,
  hostile and malformed JSON, every expression transition, display text,
  pairing paths (happy, wrong order, already paired, timeout, mute, lost link,
  store failure), the long press, good, bad, missing, replayed and
  separator-confused proofs, mute priority in nine states, and the privacy
  invariants.
- Results are in the README. MSVC `/W4 /WX`, Debian gcc 12 with
  `-Wall -Wextra -Werror -pedantic`, and a real `idf.py build` of the default
  and F2 images.

## Limits and what hardware must still show

- The USB-Serial-JTAG behavior (delivery after the host opens the port, the
  `tx_idle` use to avoid stacking stale hellos, DTR/RTS resets, boot text),
  the entropy source, NVS persistence across resets, the OLED text and the
  timing of the 10 ms control loop with the link running are all unverified.
- The key crosses the cable in clear text and sits in NVS unencrypted (no
  flash or NVS encryption in this stage); anyone with the board can read it.
- Frame sequence numbers are counted for gaps, not enforced. A recorded
  frame replayed within a session is accepted, as the protocol notes.
- Capability names `usb_pcm` and `servo_pose` and the animation names are this
  firmware's choice where the protocol lists none.
