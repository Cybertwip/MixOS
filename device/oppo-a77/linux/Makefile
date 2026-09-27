# OPPO A77 5G out-of-tree drivers: none yet.
#
# Drivers land here as mt68xx-family modules (mt68xx_<subsystem>.ko) once
# BRINGUP.md step 2 yields hardware facts. Family rules, set now so the
# first driver sets the pattern: SoC specifics arrive via DTS match data,
# never via #ifdef per-SoC; compatibles name the exact SoC
# (mediatek,mt6833-*) -- never a family wildcard.
#
# Expected first: panel (model from stock DTB), touch (IC from stock DTB),
# consys/wifi (firmware names from stock vendor), modem (CCCI load path
# from stock), PMIC keys/power (PMIC itself still unidentified).

modules:
	@echo "oppo-a77: no out-of-tree drivers yet (BRINGUP.md step 3)"

modules_install:
	@echo "oppo-a77: nothing to install"

clean:
	@true
