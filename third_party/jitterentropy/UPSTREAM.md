# Jitterentropy

Pinned release: [3.6.3](https://github.com/smuellerDD/jitterentropy-library/tree/v3.6.3).
Source archive SHA-256:
`9b3819e275e6473a94c70cfaae7cc2a87a11f60b862e694745daa5d232b0eea3`.

`jitterentropy.h` and `src/` are unmodified upstream files. Only the files
needed by the collector are vendored. `jitterentropy-base-user.h` is the Glaze
platform adapter: microsecond monotonic timer, single CPU, 32 KiB cache/memory
limit, allocation/erasure and yield hooks. The counting-thread timer is disabled.

`lib/libc/entropy_jent.c` combines the upstream compilation units and must be
compiled at `-O0`; upstream enforces this. Health-test thresholds are unchanged.
The Glaze wrapper serializes calls and uses OSR 3 with `JENT_FORCE_FIPS` to
enable continuous health tests. That flag does not imply FIPS certification.

License: upstream BSD-style terms or GPL-2.0-or-later; original notices and
both license texts are retained. Glaze uses the GPL alternative.
