# OPPO A77 4G out-of-tree drivers: none yet.
#
# Drivers land here as mt67xx-family modules (mt67xx_<subsystem>.ko) once
# BRINGUP.md step 2 yields hardware facts. Family rules, set now so the
# first driver sets the pattern: SoC specifics arrive via DTS match data,
# never via #ifdef per-SoC; compatibles name the exact SoC
# (mediatek,mt6765-*) -- never a family wildcard.
#
# Expected first: panel (model from stock DTB), touch (IC from stock DTB),
# consys/wifi (firmware names from stock vendor), modem (CCCI load path
# from stock), PMIC keys/power (mt6357 is mainline-bound already).

modules:
	@echo "oppo-a77-4g: no out-of-tree drivers yet (BRINGUP.md step 3)"

modules_install:
	@echo "oppo-a77-4g: nothing to install"

clean:
	@true
