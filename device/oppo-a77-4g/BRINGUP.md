# OPPO A77 4G bring-up playbook

Every step here converts one unknown into a fact with its source. Work
top to bottom; each step names the exact files it fills.

## Prerequisites

A CPH2385 with an unlocked bootloader and a way back (SP Flash Tool +
stock OFP, or mtkclient backup). Without those, stop: nothing below is
testable and flashing is a brick risk.

## Step 0 -- which RAM row

Settings > About phone > RAM (or `adb shell grep MemTotal
/proc/meminfo`: roughly 2900000 kB means 3GB, roughly 3800000 kB means
4GB): 3GB is the default `cph2385` row, 4GB means
`A77_4G_DEVICE=cph2385-4gb`. When in doubt stay on the default -- it is
the safe direction.

## Step 1 -- stock boot image

Two routes, same prize (the stock DTB): pull `/dev/block/by-name/boot`
(rooted shell: `dd`; else SP Flash readback), or decrypt a CPH2385
`.ofp` offline (`firmware/fetch-ofp.sh` does the whole OFP; the boot
image is among the decrypted partitions). Unpack in the VM (multipass
exec, `apt install abootimg device-tree-compiler`):

```
abootimg -x stock-boot.img          # kernel + ramdisk + second (DTB)
dtc -I dtb -O dts -o stock.dts second  # or boot.img-dtb
```

## Step 2 -- the DTB gives the map

From `stock.dts`, read off and record with file:line: DRAM base/size
(check against `MTK_DRAM_BASE_PRIOR`; confirms the RAM rows), console
UART base + clock, `pwrap` base + PMIC model (instantiate the matching
mainline `.dtsi` under it if one exists), MMC base + vmmc/vqmmc rails,
clock/pinctrl compatibles, CPU MPIDRs (SMP!), panel endpoint + panel
model, touch IC (I2C bus/address), LK framebuffer base (enables
simplefb via `A77_4G_FB_BASE`).

Fill: `devices.sh` rows (PMIC/panel/touch/model columns),
`generate_dts_a77_4g.py` (new nodes -- extend via
`device/common/mtk_bringup_dts.py` if two families need the shape),
`tests/test-dts.py` (assert the new nodes).

## Step 3 -- first drivers (mt67xx-family modules)

Panel first (visible proof of life), then touch, then MMC/serial if
mainline's `mtk-sd`/`8250_mtk` don't bind as-is. Rules: `mt67xx_*`
prefix, SoC specifics via DTS match data, exact-SoC compatibles only.
Wire each into `linux/Makefile`, `build-in-vm.sh` module+payload
staging, and the `/init` insmod list.

## Step 4 -- firmware + telephony

Run `firmware/extract-stock.sh` (or `fetch-ofp.sh`) to confirm the
blob names on this model, stage the results in `build-in-vm.sh` once
drivers request them, then modem/audio following the oppo-mt6877
tree's shape.

## Step 5 -- first boot

`fastboot boot` the mix-only `boot.img` (never flash first). No serial
yet: diagnose by behavior (adb? fastboot still reachable? display
response?). Iterate DTB/nodes until the rescue shell talks, then the
full image until login.

## Step 6 -- promote

When login works: remove the `OPPO_A77_4G_BRINGUP_ACK` gate, promote
the family into `build-mixos.sh`'s default plan (it is `--only`
opt-in today), extend the wifi-bringup-style checks, and update this
file into history (what each fact turned out to be, with sources).
