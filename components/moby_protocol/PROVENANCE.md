# Provenance

These files are a vendored copy of the Moby USB v1 host framing from
`moby-protocol` (<https://github.com/mobyuz/moby-protocol>), branch
`claude/usb-control-full`, commit `9032acb` (`9032acbdc8ad12b57368deb0875f0640db55207d`),
protocol 0.2.0. They are byte for byte what `git show 9032acb:<path>` prints
in that repository. Do not edit them here. License: `GPL-3.0-only`, as the
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
| `tests/fixtures/transition-vectors.tsv` | `tests/fixtures/transition-vectors.tsv` |

SHA-256 of the vendored files (LF line endings; `.gitattributes` keeps them LF):

```
5c6919daac8622a6c0be5b4ca3d4434b28d60f845ba0e742501bb84d8ae1edfa  include/moby_usb.h
d398131904e3dff698591c7c17c606fc372b15eecebea3e9ae5f54e18d29cc43  include/moby_usb_codec.h
fb5466160be48fb09569f4534b249ed3f191d130e327195f9bc8eb70032b2423  c/moby_usb_codec.c
74e21e66db351aa63be94b53f2cbee617e45756c962cd27dd5ff79cb550df127  tests/fixtures/usb-vectors.tsv
29659697921e5c548a3465bdba632e9685c37db3c616df5e4d4340949c63ef1f  tests/fixtures/control-vectors.tsv
d33728e69db35cedef770d47f77ad64d64ec3169f18c6e72316e4d20079324f3  tests/fixtures/cobs-vectors.tsv
4bf9f844a4429d3db8c4d8f807a4dfa90d6c96076049945f77fcac144d0a5479  tests/fixtures/hmac-vectors.tsv
653f0551b13c65fc2b5f8a1de741c8c8dd026cbf4afdec523b4e25a2dff7f98f  tests/fixtures/proof-vectors.tsv
675cdfac906d975cdda215f7e54e9b712388f36678f01d64ac741ae6f76836d4  tests/fixtures/transition-vectors.tsv
```

## Checks

- `tests/vectors.c` in this repository decodes every row of the frame, control
  and COBS vectors through the vendored codec, checks the transition table
  and the proof input bytes, and recomputes every HMAC and session proof
  vector with the mbedTLS code the firmware uses.
- `tests/check_vendored.sh` compares the SHA-256 sums above with the files.
- Upstream `tests/usb_vectors.c` is not vendored. Its checks on the codec
  alone are covered by the rows above plus `tests/vectors.c`.

## Known upstream quirk

`moby_usb.h` uses `bool` without including `<stdbool.h>`. Include
`moby_usb_codec.h` first, as the firmware does, or include `<stdbool.h>`
before `moby_usb.h`.
