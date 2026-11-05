# The robot proves the pairing key

Date 2026-11-05

Status candidate for host tests and a first run on a board. Nothing here was
tested on hardware. It implements decision 0005 of `moby-protocol`
(`docs/adr/0005-robot-proof-of-key.md`), approved by the owner on 2026-11-05.

## Choice

- `hello_ack` may carry `desktop_nonce` (16 bytes, lowercase hex) next to
  `proof`. The parser accepts both or neither and rejects `null` for either, so
  an ack with one of them is not admitted.
- When the desktop proof verifies, the session computes
  `HMAC-SHA256(pair_key, "moby-robot-proof-v1" || 0x00 || desktop_nonce ||
  robot_nonce || device_id || 0x00 || session_id)` through the same `ops.hmac_sha256`
  as the desktop proof, with the input from the vendored
  `moby_robot_proof_input`. It queues one `session_proof` event, ahead of the
  opening `mute_changed`. Nothing else is queued earlier on an authenticated
  session, and `audio_start` needs `authenticated`, so the proof precedes audio.
- A bad or missing desktop proof makes no HMAC for the robot and queues no
  `session_proof`. The one HMAC in that path only checks the peer's value. An
  unpaired robot has no key and computes nothing, whatever the ack carries.
- If the robot cannot compute its proof (no HMAC op), the session is not
  authenticated, like a failed check.
- The hello nonce is wiped when the session starts, as before. The desktop nonce
  lives only in the parsed message on the stack. The robot's proof is a MAC and is
  not a secret once sent; the key, both nonces and the proof are never logged.
- A session authenticated by `pair_set_key` has no `desktop_nonce`, so no
  `session_proof` is sent. The desktop treats that session as unproven and
  reconnects; the next session is proven.

## Consequences

- If the `session_proof` frame cannot be written, the link counts a dropped
  frame and does not retry it, so the desktop sees no proof and treats the robot
  as unproven. This fails closed and costs a reconnect.
- The robot still allows audio on the session that `pair_set_key` opened. The
  desktop, which got no proof there, is the party that must discard it. Making
  the robot refuse would be a firmware-only change and was not asked for.

## Evidence

Host tests (`tests/f3.c`, `tests/f4.c`, `tests/vectors.c`): the proof matches
`robot-proof-vectors.tsv` row `robot_proof_1` and an independent computation in
the test; it is the first message of the session and comes before `audio_start`;
it is sent once and is new for every session; a wrong, missing, wrong-session or
replayed desktop proof, and an unpaired robot, produce no `session_proof` and no
HMAC call; an ack with only one of `proof` and `desktop_nonce`, or with a null or
malformed one, is not admitted; logs and the wire never carry the key. Every
vendored robot-proof and reflection vector is recomputed with mbedTLS.
