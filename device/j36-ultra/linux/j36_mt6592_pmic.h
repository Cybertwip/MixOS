/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The two calls j36_mt6592_pmic lends out, and the only reason it lends them.
 *
 * WACS2 -- the MT6592's bridge to the MT6323 companion die -- is one state
 * machine with one result register, and a transaction on it is three writes and a
 * poll with no hardware arbitration between them.  A second driver holding a
 * second lock over a second ioremap of the same window is not a second lock, it is
 * none: one transaction collects the other's result, clears the valid flag under
 * it, and both come away with a number that looks fine.  So there is exactly one
 * owner of that FSM in this kernel, and everyone else comes through here.
 *
 * The caller today is j36_mt6592_wifi, which needs the four MT6323 connectivity
 * rails (VCN_1V8, VCN28 and the two halves of VCN33) and nothing else from the
 * PMIC.  Because these are real symbols and not an optional lookup, insmod of the
 * WiFi module fails outright, naming the symbol, when the PMIC module is not
 * loaded -- which is a great deal easier to read than a radio brought up with its
 * transmit PA supply still down.
 */
#ifndef J36_MT6592_PMIC_H
#define J36_MT6592_PMIC_H

#include <linux/types.h>

/*
 * Both return -ENODEV until j36_mt6592_pmic has finished probing, and a negative
 * transport error if the bridge does not answer.  j36_pmic_pwrap_update() returns
 * 1 when the register changed and 0 when it already held that value; callers that
 * only care whether it worked can test for < 0.
 *
 * Both take the PMIC's spinlock and are safe from any context that can spin.
 */
int j36_pmic_pwrap_update(u32 adr, u32 clr, u32 set);
int j36_pmic_pwrap_read(u32 adr, u32 *rdata);

#endif /* J36_MT6592_PMIC_H */
