# LG K20 bring-up playbook

Every step here converts one unknown into a fact with its source. Work
top to bottom; each step names the exact files it fills.

## Prerequisites

An LM-X120 with an unlocked bootloader and a way back (SP Flash Tool +
stock ROM, or mtkclient backup). Without those, stop: nothing below is
testable and flashing is a brick risk.

## Step 1 -- stock boot image

Pull `/dev/block/by-name/boot` (rooted shell: `dd`; else SP Flash
readback) and confirm LG's MTK stock-ROM format for later offline work.
Unpack in the VM (multipass exec, `apt install abootimg device-tree-compiler`):

```
abootimg -x stock-boot.img          # kernel + ramdisk + second (DTB)
dtc -I dtb -O dts -o stock.dts second  # or boot.img-dtb
```

## Step 2 -- the DTB gives the map

From `stock.dts`, read off and record with file:line: DRAM base/size
(check against `MTK_DRAM_BASE_PRIOR`), console UART base + clock,
`pwrap` base (then instantiate mainline `mt6357.dtsi` under it), MMC
base + vmmc/vqmmc rails, clock/pinctrl compatibles, CPU MPIDRs (SMP!),
panel endpoint + panel model, touch IC (I2C bus/address), LK
framebuffer base (enables simplefb via `K20_FB_BASE`).

Fill: `devices.sh` row (panel/touch/model columns), `generate_dts_k20.py`
(new nodes -- extend via `device/common/mtk_bringup_dts.py` if two
families need the shape), `tests/test-dts.py` (assert the new nodes).

## Step 3 -- first drivers (mt67xx-family modules)

Panel first (visible proof of life), then touch, then MMC/serial if
mainline's `mtk-sd`/`8250_mtk` don't bind as-is. Rules: `mt67xx_*`
prefix, SoC specifics via DTS match data, exact-SoC compatibles only.
Wire each into `linux/Makefile`, `build-in-vm.sh` module+payload
staging, and the `/init` insmod list.

## Step 4 -- firmware + telephony

Run `firmware/extract-stock.sh` (fixes the name priors from real
listings), stage the results in `build-in-vm.sh` once drivers request
them, then modem/audio following the oppo-mt6877 tree's shape.

## Step 5 -- first boot

`fastboot boot` the mix-only `boot.img` (never flash first). No serial
yet: diagnose by behavior (adb? fastboot still reachable? display
response?). Iterate DTB/nodes until the rescue shell talks, then the
full image until login.

## Step 6 -- promote

When login works: remove the `LG_K20_BRINGUP_ACK` gate, wire the family
into `build-mixos.sh`, extend the wifi-bringup-style checks, and update
this file into history (what each fact turned out to be, with sources).
