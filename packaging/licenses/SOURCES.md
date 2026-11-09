# Where these texts came from

Canonical licence texts, used for dependencies that declare a licence but ship
no copy of it inside the crate (common: the SPDX field in Cargo.toml is the
declaration, and a `LICENSE` file is optional). The generator prefers a crate's
own file and falls back to one of these.

| File | Source |
|---|---|
| `MIT.txt` | <https://spdx.org/licenses/MIT.txt>, fetched 2026-11-09 |
| `Zlib.txt` | <https://spdx.org/licenses/Zlib.txt>, fetched 2026-11-09 |
| `MPL-2.0.txt` | verbatim copy shipped in the `termina` crate, which matches the text Mozilla publishes |
| `Apache-2.0.txt` | verbatim copy shipped in the `cc` crate |

The MIT and Zlib templates carry SPDX's `<year> <copyright holders>`
placeholders rather than any particular holder's name. The generator prints the
crate's declared authors beside the text instead of substituting them in, so
nothing in the notices claims to be a copyright line that upstream did not
write.
