# Provenance

These files are a vendored copy of the Moby USB v1 host framing from
`moby-protocol` (<https://github.com/mobyuz/moby-protocol>), branch
`claude/mutual-auth` (on top of `claude/usb-control-full`), commit `0ddc9a9`
(`0ddc9a9befddb59082ec35e354512b3d72129fcf`), protocol 0.2.0 with the robot
proof of the pairing key (decision 0005). The earlier copy was commit `9032acb`. They are byte for byte what
`git show 0ddc9a9:<path>` prints in that repository. Do not edit them here. License: `GPL-3.0-only`, as the
rest of this repository.

moby-protocol has no release tag yet. **This vendoring is temporary and gets
replaced by the tagged release artifact** once the protocol publishes one. The
replacement is a straight copy of the same paths, so the layout mirrors the
upstream tree.

| Path here | Upstream path |
| --- | --- |
| `include/moby_usb.h` | `include/moby_usb.h` |
| `include/moby_usb_codec.h` | `include/moby_usb_codec.h` |
| `c/moby_usb_codec.c` | `c/moby_usb_codec.c` |
| `tests/fixtures/usb-vectors.tsv` | `tests/fixtures/usb-vectors.tsv` |
| `tests/fixtures/control-vectors.tsv` | `tests/fixtures/control-vectors.tsv` |
| `tests/fixtures/cobs-vectors.tsv` | `tests/fixtures/cobs-vectors.tsv` |
| `tests/fixtures/hmac-vectors.tsv` | `tests/fixtures/hmac-vectors.tsv` |
| `tests/fixtures/proof-vectors.tsv` | `tests/fixtures/proof-vectors.tsv` |
| `tests/fixtures/robot-proof-vectors.tsv` | `tests/fixtures/robot-proof-vectors.tsv` |
| `tests/fixtures/reflection-vectors.tsv` | `tests/fixtures/reflection-vectors.tsv` |
| `tests/fixtures/transition-vectors.tsv` | `tests/fixtures/transition-vectors.tsv` |

SHA-256 of the vendored files (LF line endings; `.gitattributes` keeps them LF):

```
cc7eb29582ac9401d475e3c91abab88c7260227a9d3d4727dba5d4d63c2106d2  include/moby_usb.h
206598a9a5aaf57ed80cb2278495dfbf791b21b82038a9a5cb776c8beb4db826  include/moby_usb_codec.h
c210fe392e2d6da3e84eb605dec5071c08f10c8836a94367541f9a0fed1aa8b2  c/moby_usb_codec.c
74e21e66db351aa63be94b53f2cbee617e45756c962cd27dd5ff79cb550df127  tests/fixtures/usb-vectors.tsv
f9a7292004334f6148b036a882f0960ae681e667eb555c3722addf0e04241858  tests/fixtures/control-vectors.tsv
d33728e69db35cedef770d47f77ad64d64ec3169f18c6e72316e4d20079324f3  tests/fixtures/cobs-vectors.tsv
4bf9f844a4429d3db8c4d8f807a4dfa90d6c96076049945f77fcac144d0a5479  tests/fixtures/hmac-vectors.tsv
653f0551b13c65fc2b5f8a1de741c8c8dd026cbf4afdec523b4e25a2dff7f98f  tests/fixtures/proof-vectors.tsv
c66535f550c09c5cb72bba16ff5d64e202f28f66f2fbd173de4ab660767e0c7d  tests/fixtures/robot-proof-vectors.tsv
e0b85c477415804a2c9c8762d754943cc5afa1afcd74d4e4df161e6cca749b88  tests/fixtures/reflection-vectors.tsv
675cdfac906d975cdda215f7e54e9b712388f36678f01d64ac741ae6f76836d4  tests/fixtures/transition-vectors.tsv
```

## Checks

- `tests/vectors.c` in this repository decodes every row of the frame, control
  and COBS vectors through the vendored codec, checks the transition table
  and the proof input bytes, and recomputes every HMAC, session proof, robot
  proof and reflection vector with the mbedTLS code the firmware uses.
- `tests/check_vendored.sh` compares the SHA-256 sums above with the files.
- Upstream `tests/usb_vectors.c` is not vendored. Its checks on the codec
  alone are covered by the rows above plus `tests/vectors.c`.

## Known upstream quirk

`moby_usb.h` uses `bool` without including `<stdbool.h>`. Include
`moby_usb_codec.h` first, as the firmware does, or include `<stdbool.h>`
before `moby_usb.h`.
