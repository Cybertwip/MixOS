# mt65xx LK family (MT6592, j36-ultra)

The proven LK every other family derives from: `firmware/` builds `lk.bin`,
`lk-release.bin`, `MVIIFlash.bin` and `assets.bin` for the j36-ultra, in two
power modes (`battery`, `without-battery`).

`./tools/mediatek/mt65xx/build.sh --device j36-ultra [--without-battery]`
builds one mode into `build/mt65xx/j36-ultra/<mode>/`; `./build-flashtools.sh`
builds both modes plus every other family. `--device` takes only `j36-ultra`:
it exists so every family builder shares one interface.

Provenance of the sources: `firmware/PROVENANCE.md`.
