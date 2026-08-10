// SPDX-License-Identifier: GPL-2.0-only
/*
 * J36 Ultra MT6592 AFE playback adapter.
 *
 * One playback PCM on the AFE's DL1 memif, so this kernel has an ALSA card at
 * all. There is no MT6592 audio support anywhere upstream -- sound/soc/mediatek
 * starts at MT2701 -- and the vendor path is an Android HAL that talks to a
 * kernel driver through ioctls that do not exist here. So the register sequences
 * below are lifted, register for register, from the freestanding driver that was
 * written against this board:
 *
 *   PowerEngine OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_audio.c
 *
 * which in turn distilled them from the reference HAL staged under
 * PowerEngine/External/MediaTek/Audio/MT6592 -- AudioDigitalControl for the AFE
 * memif and the I2S/ADDA route, AudioPlatformDevice and AudioMachineDevice for
 * the MT6323 ABB downlink and the class-D speaker.
 *
 * WHAT IS MEASURED AND WHAT IS NOT, because the difference decides what this
 * driver is allowed to switch on by itself:
 *
 *   - The AFE register window answers at 0x11220000 with the boot chain's
 *     clocks: MVII programs DL1 there and reads its BASE back on every boot.
 *   - The AFE FUNCTIONAL CLOCK bring-up is NOT proven. MVII has the sequence but
 *     compiles it out (MVII_MT6592_AUDIO_ENABLE_AFE_CLK defaults to 0), so
 *     nobody has yet seen AFE_DL1_CUR advance on this board. That is precisely
 *     the measurement this driver exists to take, which is why it ungates the
 *     clocks, logs CLK_CFG_AUD before and after, and says in one line per stream
 *     whether the DMA cursor moved.
 *   - The CLASS-D SPEAKER IS A KNOWN POWER HAZARD and stays off unless it is
 *     asked for by name. VBAT on this PMIC family is the system node: with no
 *     cell fitted it is held up only by the charger's current source, and MVII
 *     recorded the amp at full output pulling it under the undervoltage lockout
 *     -- the board switches off a few seconds into playback. So `speaker=1' is
 *     opt-in, it opens at a conservative level, and the amp is only ever powered
 *     after the DL1 cursor has been seen moving, so it is never fed by an
 *     unclocked DAC (which would drive the coil with DC and take the rail down
 *     for a different reason).
 *
 * The consequence, stated plainly: with the default parameters this card is
 * silent by construction. What it gives is /dev/snd, a PCM that accepts and
 * paces audio, and one dmesg line that says whether the hardware consumed it.
 * Sound comes from the boot word, once the log says the DMA is alive and a cell
 * is fitted.
 *
 * No interrupt. The AFE's IRQ block is not in the reference material at all, and
 * a period wakeup does not need it: the DL1 cursor is a register, so it is polled
 * from a delayed work item exactly as MVII polls it, and .pointer reads the
 * hardware. If the cursor never moves the work item paces the stream from the
 * wall clock instead and says so once -- silence that keeps running is a
 * measurement, whereas an ALSA stream that never completes a period hangs every
 * program that opens the card, EmulationStation included.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

/* ---- AFE, at the reg of our own node (AudioAfeReg.h offsets) -------------- */
#define J36_AUDIO_TOP_CON0		0x0000
#define J36_AFE_DAC_CON0		0x0010
#define J36_AFE_DAC_CON1		0x0014
#define J36_AFE_CONN0			0x0020
#define J36_AFE_CONN1			0x0024
#define J36_AFE_CONN2			0x0028
#define J36_AFE_CONN3			0x002c
#define J36_AFE_I2S_CON1		0x0034
#define J36_AFE_DL1_BASE		0x0040
#define J36_AFE_DL1_CUR			0x0044
#define J36_AFE_DL1_END			0x0048
#define J36_AFE_ADDA_DL_SRC2_CON0	0x0108
#define J36_AFE_ADDA_DL_SRC2_CON1	0x010c
#define J36_AFE_ADDA_TOP_CON0		0x0120
#define J36_AFE_ADDA_UL_DL_CON0		0x0124
#define J36_AFE_PREDIS_CON0		0x0260
#define J36_AFE_PREDIS_CON1		0x0264
#define J36_AFE_MEMIF_PBUF_SIZE		0x03d8

/* AUDIO_TOP_CON0 is a power-down register: a set bit gates. The stock CG_AUDIO
 * descriptor covers two bits of it, AFE and I2S, and the rest belongs to blocks
 * this driver does not touch -- hence a read-modify-write and not an assignment.
 */
#define J36_AUDIO_TOP_PDN_I2S		BIT(6)
#define J36_AUDIO_TOP_PDN_AFE		BIT(2)

#define J36_AFE_DAC_CON0_DL1_EN		BIT(1)
#define J36_AFE_DAC_CON0_AFE_ON		BIT(0)

/* ---- The two SoC clock registers, reached through phandles ---------------- */
#define J36_TOPCKGEN_CLK_CFG_AUD		0x0070
#define J36_CLK_CFG_AUD_PDN_AUDINTBUS	BIT(31)
#define J36_CLK_CFG_AUD_PDN_AUDIO	BIT(23)
#define J36_INFRACFG_PDN_CLR		0x0044
#define J36_INFRACFG_PDN_AUDIO		BIT(5)

/* ---- PWRAP WACS2, the same channel and the same recovery as the input
 *      adapter. The two never overlap in practice: j36_mt6592_input touches the
 *      PMIC once, in its probe, to ungate the keypad's 32 kHz clock, and this
 *      driver's PMIC traffic all happens later -- at prepare, at close, and on
 *      the tick that first sees the DMA cursor move.
 */
#define J36_PWRAP_WACS2_CMD		0x009c
#define J36_PWRAP_WACS2_RDATA		0x00a0
#define J36_PWRAP_WACS2_VLDCLR		0x00a4
#define J36_PWRAP_FSM_IDLE		0x0
#define J36_PWRAP_FSM_WFVLDCLR		0x6
#define J36_PWRAP_INIT_DONE0		BIT(21)
#define J36_PWRAP_POLL_LIMIT		10000

/* ---- MT6323 audio banks (AudioAnalogReg.h) ------------------------------- */
#define J36_PMIC_TOP_CKPDN1_SET		0x010a
#define J36_PMIC_TOP_CKPDN1_CLR		0x010c
#define J36_PMIC_CKPDN1_AUD26M		0x0100	/* bit 8  */
#define J36_PMIC_CKPDN1_SPK		0x000e	/* bits 1..3 */
#define J36_PMIC_SPK_CON0		0x0052
#define J36_PMIC_SPK_CON2		0x0056
#define J36_PMIC_SPK_CON9		0x0064
#define J36_PMIC_SPK_CON12		0x006a
#define J36_PMIC_AUDTOP_CON0		0x0700
#define J36_PMIC_AUDTOP_CON4		0x0708
#define J36_PMIC_AUDTOP_CON6		0x070c
#define J36_PMIC_AUDTOP_CON7		0x070e
#define J36_PMIC_ABB_AFE_CON0		0x4000
#define J36_PMIC_ABB_AFE_CON1		0x4002
#define J36_PMIC_ABB_AFE_CON5		0x400a
#define J36_PMIC_ABB_AFE_CON11		0x4016
#define J36_PMIC_AFE_NEWIF_CFG0		0x4024
#define J36_PMIC_ABB_AFE_TOP_CON0	0x402c

/* AUDTOP_CON7 carries the class-D output level in bits 7:4 over a fixed 0x35
 * prefix. The vendor power-up ramps 6 -> 11 and stops; MVII stops at 8 and only
 * climbs further while the rail holds. 5 is the quietest step that is still
 * audible, 11 the vendor maximum.
 */
#define J36_SPK_LEVEL_MIN		5
#define J36_SPK_LEVEL_MAX		11
#define J36_SPK_LEVEL_SAFE		8
#define J36_SPK_CON9_PGA_0DB		0x0100	/* stock is 0x0400, +6 dB */

#define J36_AFE_BUFFER_BYTES		(64 * 1024)
#define J36_AFE_PERIOD_MIN		4096
#define J36_AFE_PERIOD_MAX		16384
#define J36_AFE_POLL_MS			5
/* How long a stream may run with a stationary cursor before the wall clock takes
 * over the pacing. Two periods at the smallest period this driver accepts. */
#define J36_AFE_CURSOR_GRACE_MS		250

static bool afeclk = true;
module_param(afeclk, bool, 0444);
MODULE_PARM_DESC(afeclk, "ungate the AFE functional clocks at probe (default on)");

static bool codec = true;
module_param(codec, bool, 0444);
MODULE_PARM_DESC(codec, "program the MT6323 ABB downlink over PWRAP (default on)");

static bool speaker;
module_param(speaker, bool, 0444);
MODULE_PARM_DESC(speaker,
		 "power the class-D speaker amp once the DL1 DMA is proven live. "
		 "OFF by default: with no cell fitted the amp pulls VBAT, which is "
		 "the system node, under the PMIC's undervoltage lockout");

static int spk_level = J36_SPK_LEVEL_SAFE;
module_param(spk_level, int, 0444);
MODULE_PARM_DESC(spk_level, "class-D output level the amp opens at, 5..11");

struct j36_afe {
	struct device		*dev;
	void __iomem		*afe;
	void __iomem		*topckgen;
	void __iomem		*infracfg;
	void __iomem		*pwrap;

	struct snd_card		*card;
	struct snd_pcm_substream *substream;
	struct delayed_work	poll_work;
	/* WACS2 is a single transaction channel and the speaker sequence is a
	 * dozen transactions long; the mixer callbacks can land in the middle of
	 * it from another thread. */
	struct mutex		pmic_lock;

	dma_addr_t		dma_addr;
	unsigned int		dma_bytes;
	unsigned int		period_bytes;
	unsigned int		rate;
	unsigned int		frame_bytes;

	unsigned int		hw_off;		/* bytes into the ring */
	unsigned long		start_jiffies;
	bool			running;
	bool			cursor_live;
	bool			cursor_reported;
	bool			soft_paced;

	bool			amp_on;
	bool			mute;
	unsigned int		level;		/* J36_SPK_LEVEL_MIN..MAX */
};

/* ---- plain register helpers ---------------------------------------------- */

static u32 j36_afe_read(struct j36_afe *afe, u32 off)
{
	return readl(afe->afe + off);
}

static void j36_afe_write(struct j36_afe *afe, u32 off, u32 value)
{
	writel(value, afe->afe + off);
}

static void j36_afe_rmw(struct j36_afe *afe, u32 off, u32 value, u32 mask)
{
	writel((readl(afe->afe + off) & ~mask) | (value & mask), afe->afe + off);
}

/*
 * One WACS2 transaction, including the leftover-state recovery: a read whose
 * caller timed out before collecting RDATA leaves the FSM parked in WFVLDCLR,
 * and without a VLDCLR write it never returns to IDLE, so every later PMIC
 * transaction times out. This is the same code the input adapter runs, for the
 * same reason, and neither claims the register window.
 */
static int j36_pwrap_xfer(struct j36_afe *afe, bool write, u32 adr, u32 wdata,
			  u32 *rdata)
{
	unsigned int i;
	u32 value;

	if (adr & ~0xffffu || wdata & ~0xffffu)
		return -EINVAL;
	if (!write && !rdata)
		return -EINVAL;

	value = readl(afe->pwrap + J36_PWRAP_WACS2_RDATA);
	if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR)
		writel(1, afe->pwrap + J36_PWRAP_WACS2_VLDCLR);

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(afe->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_IDLE)
			break;
		cpu_relax();
	}
	if (i == J36_PWRAP_POLL_LIMIT)
		return -ETIMEDOUT;

	writel(((u32)write << 31) | ((adr >> 1) << 16) | wdata,
	       afe->pwrap + J36_PWRAP_WACS2_CMD);
	if (write)
		return 0;

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(afe->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR) {
			*rdata = value & 0xffff;
			writel(1, afe->pwrap + J36_PWRAP_WACS2_VLDCLR);
			return 0;
		}
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int j36_pmic_read(struct j36_afe *afe, u32 adr, u32 *value)
{
	return j36_pwrap_xfer(afe, false, adr, 0, value);
}

static int j36_pmic_write(struct j36_afe *afe, u32 adr, u32 value)
{
	return j36_pwrap_xfer(afe, true, adr, value & 0xffff, NULL);
}

static int j36_pmic_rmw(struct j36_afe *afe, u32 adr, u32 value, u32 mask)
{
	u32 current_value;
	int ret;

	ret = j36_pmic_read(afe, adr, &current_value);
	if (ret)
		return ret;
	return j36_pmic_write(afe, adr, (current_value & ~mask) | (value & mask));
}

/* INIT_DONE0 only refreshes after a transaction, so a cold read right after the
 * boot chain's hand-off can be stale -- the same caveat the input adapter
 * carries. Treated as advisory: the transaction itself times out if the wrapper
 * really is dead, and that is the error the caller reports. */
static bool j36_pmic_ready(struct j36_afe *afe)
{
	return !!(readl(afe->pwrap + J36_PWRAP_WACS2_RDATA) & J36_PWRAP_INIT_DONE0);
}

/* ---- rate tables (AudioMEMIFAttribute, GetDLFrequency) ------------------- */

static u32 j36_afe_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0x0;
	case 11025:	return 0x1;
	case 12000:	return 0x2;
	case 16000:	return 0x4;
	case 22050:	return 0x5;
	case 24000:	return 0x6;
	case 32000:	return 0x8;
	case 44100:	return 0x9;
	default:	return 0xa;	/* 48000 */
	}
}

static u32 j36_abb_dl_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 4;
	case 22050:	return 5;
	case 24000:	return 6;
	case 32000:	return 8;
	case 44100:	return 9;
	default:	return 10;	/* 48000 */
	}
}

static u32 j36_abb_newif_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 3;
	case 22050:	return 4;
	case 24000:	return 5;
	case 32000:	return 6;
	case 44100:	return 7;
	default:	return 8;	/* 48000 */
	}
}

/* ---- clocks -------------------------------------------------------------- */

/*
 * The narrow sequence the stock clock manager runs: keep both audio muxes'
 * source selections exactly as the boot chain left them, release their two
 * power-down bits, then clear the dedicated INFRA_AUDIO gate. Nothing else in
 * CLK_CFG_3 is touched -- an earlier version of this in MVII cleared three
 * selector bits for a mux with a one-bit select field, i.e. wrote reserved bits.
 */
static void j36_afe_clocks_ungate(struct j36_afe *afe)
{
	u32 before, after;

	if (!afeclk) {
		dev_info(afe->dev,
			 "afeclk=0: the AFE functional clock is left as the boot chain set it\n");
		return;
	}

	before = readl(afe->topckgen + J36_TOPCKGEN_CLK_CFG_AUD);
	writel(before & ~(J36_CLK_CFG_AUD_PDN_AUDIO | J36_CLK_CFG_AUD_PDN_AUDINTBUS),
	       afe->topckgen + J36_TOPCKGEN_CLK_CFG_AUD);
	writel(J36_INFRACFG_PDN_AUDIO, afe->infracfg + J36_INFRACFG_PDN_CLR);
	after = readl(afe->topckgen + J36_TOPCKGEN_CLK_CFG_AUD);

	/* Read this line first when there is no sound and no moving cursor: bit 31
	 * is MUX_AUDINTBUS's power-down and bit 23 is MUX_AUDIO's, and both have to
	 * read 0 afterwards. A value that did not change is a write that did not
	 * land. */
	dev_info(afe->dev, "CLK_CFG_AUD 0x%08x -> 0x%08x, INFRA_AUDIO released\n",
		 before, after);
}

/* ---- the AFE playback route --------------------------------------------- */

static void j36_afe_route(struct j36_afe *afe, unsigned int rate)
{
	u32 sr = j36_afe_rate_code(rate);
	u32 dl_src;

	/* ADDA downlink SRC: the newif rate code in 31:28, then the four fixed
	 * fields the HAL writes for a 16-bit stereo downlink. */
	dl_src = (j36_abb_newif_code(rate) << 28) | (0x03 << 24) |
		 (0x03 << 11) | BIT(1) | BIT(0);
	j36_afe_write(afe, J36_AFE_ADDA_DL_SRC2_CON0, dl_src);
	j36_afe_write(afe, J36_AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);
	j36_afe_write(afe, J36_AFE_I2S_CON1, (sr << 8) | BIT(3) | BIT(0));
	j36_afe_rmw(afe, J36_AFE_ADDA_UL_DL_CON0, BIT(0), BIT(0));
	j36_afe_rmw(afe, J36_AFE_ADDA_TOP_CON0, 0, BIT(0));	/* loopback off */
}

static int j36_afe_program(struct j36_afe *afe)
{
	u32 base = (u32)afe->dma_addr;
	u32 end = base + afe->dma_bytes - 1;

	/* Ungate AFE and I2S inside the audio block. */
	j36_afe_rmw(afe, J36_AUDIO_TOP_CON0, 0,
		    J36_AUDIO_TOP_PDN_I2S | J36_AUDIO_TOP_PDN_AFE);
	usleep_range(1000, 2000);

	/* Clear the interconnect, then wire DL1 L/R (I05/I06) to the I2S DAC
	 * (O03/O04). The two bit positions are AudioInterConnection's. */
	j36_afe_write(afe, J36_AFE_CONN0, 0);
	j36_afe_write(afe, J36_AFE_CONN1, 0);
	j36_afe_write(afe, J36_AFE_CONN2, 0);
	j36_afe_write(afe, J36_AFE_CONN3, 0);
	j36_afe_rmw(afe, J36_AFE_CONN1, BIT(21), BIT(21));
	j36_afe_rmw(afe, J36_AFE_CONN2, BIT(6), BIT(6));

	j36_afe_write(afe, J36_AFE_PREDIS_CON0, 0);
	j36_afe_write(afe, J36_AFE_PREDIS_CON1, 0);

	/* 16-bit fetch format for DL1, and the DL1 rate. */
	j36_afe_rmw(afe, J36_AFE_MEMIF_PBUF_SIZE, 0, 0x00030000);
	j36_afe_rmw(afe, J36_AFE_DAC_CON1, j36_afe_rate_code(afe->rate), 0xf);

	j36_afe_write(afe, J36_AFE_DL1_BASE, base);
	j36_afe_write(afe, J36_AFE_DL1_END, end);
	j36_afe_write(afe, J36_AFE_DL1_CUR, base);
	j36_afe_route(afe, afe->rate);

	/* The one register that says the block is really there. MVII makes the
	 * same check for the same reason: everything above is write-only until
	 * something reads back. */
	if (j36_afe_read(afe, J36_AFE_DL1_BASE) != base) {
		dev_err(afe->dev, "DL1 BASE reads back 0x%08x, not 0x%08x\n",
			j36_afe_read(afe, J36_AFE_DL1_BASE), base);
		return -EIO;
	}
	return 0;
}

/* ---- the MT6323 downlink ------------------------------------------------- */

/* AudioPlatformDevice's TopCtlChangeTrigger: the ABB latches a configuration
 * change on a toggle of ABB_AFE_CON11 bit 8, so the new value of that bit has to
 * be the opposite of what CON11 bit 0 currently reads. */
static void j36_pmic_top_trigger(struct j36_afe *afe)
{
	u32 top;

	if (j36_pmic_read(afe, J36_PMIC_ABB_AFE_CON11, &top))
		return;
	j36_pmic_rmw(afe, J36_PMIC_ABB_AFE_CON11, (top & BIT(0)) ? 0 : BIT(8),
		     BIT(8));
}

static int j36_pmic_downlink(struct j36_afe *afe, unsigned int rate)
{
	int ret;

	if (!codec) {
		dev_info(afe->dev, "codec=0: the PMIC downlink is left alone\n");
		return 0;
	}
	if (!j36_pmic_ready(afe))
		dev_warn(afe->dev, "PWRAP is not reporting INIT_DONE; trying anyway\n");

	/* Only the documented audio clock. A broad TOP_CKPDN write disturbs
	 * unrelated PMIC consumers, and 0x10c is the CLR alias -- writing a 1 here
	 * RELEASES the clock. Its neighbour 0x10a is SET, and MVII spent a
	 * bring-up powering the speaker clock down through it while believing it
	 * was releasing it. */
	ret = j36_pmic_write(afe, J36_PMIC_TOP_CKPDN1_CLR, J36_PMIC_CKPDN1_AUD26M);
	if (ret)
		goto fail;

	ret = j36_pmic_write(afe, J36_PMIC_AFE_NEWIF_CFG0,
			     (j36_abb_newif_code(rate) << 12) | 0x0330);
	if (ret)
		goto fail;
	ret = j36_pmic_rmw(afe, J36_PMIC_ABB_AFE_CON1, j36_abb_dl_code(rate), 0xf);
	if (ret)
		goto fail;
	j36_pmic_write(afe, J36_PMIC_ABB_AFE_CON5, 0x0028);	/* SDM gain */
	j36_pmic_write(afe, J36_PMIC_ABB_AFE_TOP_CON0, 0x0000);	/* normal path */
	j36_pmic_top_trigger(afe);
	ret = j36_pmic_rmw(afe, J36_PMIC_ABB_AFE_CON0, BIT(0), BIT(0));
	if (ret)
		goto fail;
	j36_pmic_top_trigger(afe);

	dev_info(afe->dev, "MT6323 ABB downlink on at %u Hz\n", rate);
	return 0;

fail:
	dev_warn(afe->dev, "PWRAP transaction failed (%d); no analog downlink\n",
		 ret);
	return ret;
}

/* ---- the class-D speaker, opt-in and never on a dead DAC ----------------- */

static int j36_speaker_level(struct j36_afe *afe, unsigned int level)
{
	level = clamp(level, (unsigned int)J36_SPK_LEVEL_MIN,
		      (unsigned int)J36_SPK_LEVEL_MAX);
	return j36_pmic_write(afe, J36_PMIC_AUDTOP_CON7, 0x3500 | (level << 4));
}

/*
 * AudioMachineDevice::AnalogOpen for SPEAKERL, ramped rather than opened wide.
 * Only ever called from the poll work, and only once the DL1 cursor has moved,
 * so the amp always starts against a clocked DAC carrying real samples.
 */
static int j36_speaker_on(struct j36_afe *afe)
{
	unsigned int i;
	int ret;

	if (afe->amp_on || afe->mute)
		return 0;

	mutex_lock(&afe->pmic_lock);

	ret = j36_pmic_write(afe, J36_PMIC_AUDTOP_CON7, 0x2400);
	if (ret)
		goto fail;
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON6, 0xb7f6);
	j36_pmic_rmw(afe, J36_PMIC_AUDTOP_CON0, 0x7000, 0xf000);
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON4, 0x0014);	/* bias + LCH DAC */
	usleep_range(10000, 12000);

	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON7, 0x3550);
	j36_pmic_write(afe, J36_PMIC_TOP_CKPDN1_CLR, J36_PMIC_CKPDN1_SPK);
	j36_pmic_write(afe, J36_PMIC_SPK_CON2, 0x0214);
	j36_pmic_write(afe, J36_PMIC_SPK_CON9, J36_SPK_CON9_PGA_0DB);
	j36_pmic_write(afe, J36_PMIC_SPK_CON0, 0x3008);
	j36_pmic_write(afe, J36_PMIC_SPK_CON0, 0x3009);
	usleep_range(5000, 6000);
	j36_pmic_write(afe, J36_PMIC_SPK_CON0, 0x3001);		/* class-D on */
	j36_pmic_write(afe, J36_PMIC_SPK_CON12, 0x0a00);

	/* Up one step at a time from the vendor's starting level to ours, which is
	 * below the vendor's maximum on purpose. */
	for (i = 6; i <= afe->level; ++i) {
		usleep_range(2000, 3000);
		ret = j36_speaker_level(afe, i);
		if (ret)
			goto fail;
	}

	afe->amp_on = true;
	mutex_unlock(&afe->pmic_lock);
	dev_info(afe->dev, "class-D speaker amp live at level %u\n", afe->level);
	return 0;

fail:
	mutex_unlock(&afe->pmic_lock);
	dev_warn(afe->dev, "class-D power-up failed (%d)\n", ret);
	return ret;
}

static void j36_speaker_off(struct j36_afe *afe)
{
	unsigned int i;

	if (!afe->amp_on)
		return;

	mutex_lock(&afe->pmic_lock);

	/* Down from wherever the level currently is, never up to the vendor's
	 * ramp-down start: stepping up first would raise the load on the way to
	 * switching the amp off, which is the opposite of what a caller shutting
	 * it down wants. */
	for (i = clamp(afe->level, (unsigned int)J36_SPK_LEVEL_MIN, 10u);
	     i >= J36_SPK_LEVEL_MIN; --i) {
		j36_speaker_level(afe, i);
		usleep_range(2000, 3000);
	}
	j36_pmic_write(afe, J36_PMIC_SPK_CON0, 0x0004);
	j36_pmic_write(afe, J36_PMIC_SPK_CON12, 0x0000);
	j36_pmic_write(afe, J36_PMIC_TOP_CKPDN1_SET, J36_PMIC_CKPDN1_SPK);
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON7, 0x2500);
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON7, 0x2400);
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON4, 0x0000);
	j36_pmic_rmw(afe, J36_PMIC_AUDTOP_CON0, 0x0000, 0x1000);
	j36_pmic_write(afe, J36_PMIC_AUDTOP_CON6, 0x37e2);

	afe->amp_on = false;
	mutex_unlock(&afe->pmic_lock);
}

/* ---- the cursor, and what to do when it does not move -------------------- */

static unsigned int j36_afe_cursor(struct j36_afe *afe)
{
	u32 base = (u32)afe->dma_addr;
	u32 cur = j36_afe_read(afe, J36_AFE_DL1_CUR);

	if (cur < base || cur >= base + afe->dma_bytes)
		return afe->hw_off;
	/* Four-byte granularity, which is one stereo 16-bit frame: a cursor caught
	 * mid-frame would otherwise report a fractional frame to ALSA. */
	return (cur - base) & ~3u;
}

static void j36_afe_poll(struct work_struct *work)
{
	struct j36_afe *afe = container_of(work, struct j36_afe, poll_work.work);
	struct snd_pcm_substream *substream = afe->substream;
	unsigned int off;

	if (!afe->running || !substream)
		return;

	off = j36_afe_cursor(afe);

	if (off != afe->hw_off) {
		afe->hw_off = off;
		if (!afe->cursor_live) {
			afe->cursor_live = true;
			/* THE line to look for. If it is here, the clock bring-up
			 * above worked and the DAC is being fed; if it never
			 * appears, everything below the memif is still dark. */
			dev_info(afe->dev, "AFE DL1 DMA is live (cursor moving)\n");
			if (speaker)
				j36_speaker_on(afe);
		}
	} else if (!afe->cursor_live && !afe->soft_paced &&
		   time_after(jiffies, afe->start_jiffies +
			      msecs_to_jiffies(J36_AFE_CURSOR_GRACE_MS))) {
		afe->soft_paced = true;
		if (!afe->cursor_reported) {
			afe->cursor_reported = true;
			dev_warn(afe->dev,
				 "AFE DL1 cursor has not moved in %u ms; pacing this stream from the wall clock. Nothing is reaching the DAC\n",
				 J36_AFE_CURSOR_GRACE_MS);
		}
	}

	if (afe->soft_paced) {
		/* Wall-clock pacing, so a stream on a dead memif drains at the
		 * rate it would have played at instead of blocking the writer
		 * for ever. Recomputed from the start of the stream rather than
		 * accumulated per tick, so a late work item does not drift. */
		u64 bytes = (u64)jiffies_to_msecs(jiffies - afe->start_jiffies) *
			    afe->rate * afe->frame_bytes;

		do_div(bytes, 1000);
		afe->hw_off = do_div(bytes, afe->dma_bytes) & ~3u;
	}

	snd_pcm_period_elapsed(substream);
	schedule_delayed_work(&afe->poll_work, msecs_to_jiffies(J36_AFE_POLL_MS));
}

/* ---- PCM ----------------------------------------------------------------- */

static const struct snd_pcm_hardware j36_afe_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_8000_48000,
	.rate_min		= 8000,
	.rate_max		= 48000,
	/* Stereo only, because DL1 fetches interleaved pairs and the ADDA route
	 * above is programmed for two channels. A mono stream reaches this card
	 * through ALSA's own plug layer, which is where MVII's hand-written
	 * upmix belongs. */
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= J36_AFE_BUFFER_BYTES,
	.period_bytes_min	= J36_AFE_PERIOD_MIN,
	.period_bytes_max	= J36_AFE_PERIOD_MAX,
	.periods_min		= 2,
	.periods_max		= J36_AFE_BUFFER_BYTES / J36_AFE_PERIOD_MIN,
};

static int j36_afe_pcm_open(struct snd_pcm_substream *substream)
{
	struct j36_afe *afe = snd_pcm_substream_chip(substream);

	substream->runtime->hw = j36_afe_pcm_hw;
	afe->substream = substream;
	return 0;
}

static int j36_afe_pcm_close(struct snd_pcm_substream *substream)
{
	struct j36_afe *afe = snd_pcm_substream_chip(substream);

	cancel_delayed_work_sync(&afe->poll_work);
	j36_speaker_off(afe);
	/* Stop the consumer but keep the route programmed: an idle dashboard has
	 * no reason to hold a clocked DAC, and the next stream reprograms
	 * everything it depends on anyway. */
	j36_afe_rmw(afe, J36_AFE_DAC_CON0, 0,
		    J36_AFE_DAC_CON0_DL1_EN | J36_AFE_DAC_CON0_AFE_ON);
	afe->substream = NULL;
	return 0;
}

static int j36_afe_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct j36_afe *afe = snd_pcm_substream_chip(substream);
	int ret;

	afe->dma_addr = runtime->dma_addr;
	afe->dma_bytes = runtime->dma_bytes;
	afe->period_bytes = snd_pcm_lib_period_bytes(substream);
	afe->rate = runtime->rate;
	afe->frame_bytes = runtime->channels * 2;
	afe->hw_off = 0;
	afe->cursor_live = false;
	afe->soft_paced = false;

	ret = j36_afe_program(afe);
	if (ret)
		return ret;

	/* Everything that can sleep happens here and not in .trigger, which ALSA
	 * calls with the stream lock held: the PWRAP poll loops and the amp's
	 * sequenced delays have no business in atomic context. */
	mutex_lock(&afe->pmic_lock);
	j36_pmic_downlink(afe, afe->rate);
	mutex_unlock(&afe->pmic_lock);

	dev_dbg(afe->dev, "prepare: %u Hz, ring 0x%08x+%u, period %u\n",
		afe->rate, (u32)afe->dma_addr, afe->dma_bytes,
		afe->period_bytes);
	return 0;
}

static int j36_afe_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct j36_afe *afe = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		afe->start_jiffies = jiffies;
		afe->running = true;
		j36_afe_rmw(afe, J36_AFE_DAC_CON0,
			    J36_AFE_DAC_CON0_DL1_EN | J36_AFE_DAC_CON0_AFE_ON,
			    J36_AFE_DAC_CON0_DL1_EN | J36_AFE_DAC_CON0_AFE_ON);
		schedule_delayed_work(&afe->poll_work,
				      msecs_to_jiffies(J36_AFE_POLL_MS));
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		afe->running = false;
		j36_afe_rmw(afe, J36_AFE_DAC_CON0, 0, J36_AFE_DAC_CON0_DL1_EN);
		/* Not the _sync variant: this runs under the stream lock, and the
		 * work item takes that same lock through snd_pcm_period_elapsed.
		 * .close does the sync. */
		cancel_delayed_work(&afe->poll_work);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t j36_afe_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct j36_afe *afe = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime, afe->hw_off);
}

static const struct snd_pcm_ops j36_afe_pcm_ops = {
	.open		= j36_afe_pcm_open,
	.close		= j36_afe_pcm_close,
	.prepare	= j36_afe_pcm_prepare,
	.trigger	= j36_afe_pcm_trigger,
	.pointer	= j36_afe_pcm_pointer,
};

/* ---- mixer ---------------------------------------------------------------
 *
 * Two controls, and they exist as much for the userspace on the card as for a
 * person with amixer: alsa-restore wants a controlC0 to restore into, and the
 * dArkOS units and EmulationStation both look for a "Master" element. Seven
 * steps and not a hundred, because the class-D level field really is four bits
 * with seven usable values -- a 0..100 scale here would be a fiction.
 */
static int j36_volume_info(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = J36_SPK_LEVEL_MAX - J36_SPK_LEVEL_MIN;
	return 0;
}

static int j36_volume_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct j36_afe *afe = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = afe->level - J36_SPK_LEVEL_MIN;
	return 0;
}

static int j36_volume_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct j36_afe *afe = snd_kcontrol_chip(kcontrol);
	unsigned int level;

	level = clamp((unsigned int)ucontrol->value.integer.value[0] +
		      J36_SPK_LEVEL_MIN,
		      (unsigned int)J36_SPK_LEVEL_MIN,
		      (unsigned int)J36_SPK_LEVEL_MAX);
	if (level == afe->level)
		return 0;

	afe->level = level;
	/* Written through only while the amp is actually powered. With the amp
	 * down this is a remembered setting and not a PMIC transaction, which
	 * matters on a board where every PMIC write is on the power path. */
	if (afe->amp_on) {
		mutex_lock(&afe->pmic_lock);
		j36_speaker_level(afe, level);
		mutex_unlock(&afe->pmic_lock);
	}
	return 1;
}

static int j36_switch_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct j36_afe *afe = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = !afe->mute;
	return 0;
}

static int j36_switch_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct j36_afe *afe = snd_kcontrol_chip(kcontrol);
	bool mute = !ucontrol->value.integer.value[0];

	if (mute == afe->mute)
		return 0;

	afe->mute = mute;
	if (mute)
		j36_speaker_off(afe);
	else if (speaker && afe->cursor_live)
		j36_speaker_on(afe);
	return 1;
}

static const struct snd_kcontrol_new j36_afe_controls[] = {
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Master Playback Volume",
		.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info	= j36_volume_info,
		.get	= j36_volume_get,
		.put	= j36_volume_put,
	},
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Master Playback Switch",
		.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info	= snd_ctl_boolean_mono_info,
		.get	= j36_switch_get,
		.put	= j36_switch_put,
	},
};

/* ---- probe --------------------------------------------------------------- */

/*
 * Shared SoC blocks are mapped through phandles and their regions are not
 * claimed, exactly as the input adapter maps GPIO, KPD, AUXADC and PWRAP: an
 * eventual native provider for TOPCKGEN, INFRACFG or PWRAP will want the same
 * registers, and a claim here would be the thing that stops it binding.
 */
static void __iomem *j36_iomap_phandle(struct device *dev, const char *property)
{
	struct device_node *node;
	struct resource resource;
	void __iomem *base;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);

	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return ERR_PTR(ret);

	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		return ERR_PTR(-ENOMEM);
	return base;
}

static void j36_afe_shutdown(void *data)
{
	struct j36_afe *afe = data;

	afe->running = false;
	cancel_delayed_work_sync(&afe->poll_work);
	j36_speaker_off(afe);
	j36_afe_rmw(afe, J36_AFE_DAC_CON0, 0,
		    J36_AFE_DAC_CON0_DL1_EN | J36_AFE_DAC_CON0_AFE_ON);
}

static int j36_afe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_pcm *pcm;
	struct j36_afe *afe;
	unsigned int i;
	int ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe->dev = dev;
	afe->rate = 48000;
	afe->frame_bytes = 4;
	afe->level = clamp(spk_level, J36_SPK_LEVEL_MIN, J36_SPK_LEVEL_MAX);
	mutex_init(&afe->pmic_lock);
	INIT_DELAYED_WORK(&afe->poll_work, j36_afe_poll);

	/* DL1 takes a physical address, so the ring has to be one contiguous
	 * allocation below 4 GiB -- which is every address on this SoC, but the
	 * mask has to be stated or dma_alloc_coherent has nothing to honour. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA mask\n");

	afe->afe = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->afe))
		return dev_err_probe(dev, PTR_ERR(afe->afe), "map AFE\n");
	afe->topckgen = j36_iomap_phandle(dev, "j36,topckgen-controller");
	if (IS_ERR(afe->topckgen))
		return dev_err_probe(dev, PTR_ERR(afe->topckgen), "map TOPCKGEN\n");
	afe->infracfg = j36_iomap_phandle(dev, "j36,infracfg-controller");
	if (IS_ERR(afe->infracfg))
		return dev_err_probe(dev, PTR_ERR(afe->infracfg), "map INFRACFG\n");
	afe->pwrap = j36_iomap_phandle(dev, "j36,pwrap-controller");
	if (IS_ERR(afe->pwrap))
		return dev_err_probe(dev, PTR_ERR(afe->pwrap), "map PWRAP\n");

	/* Before the first AFE read, in that order: the block answers with the
	 * boot chain's clocks -- MVII reads it on every boot -- but its DMA
	 * cannot run on them. */
	j36_afe_clocks_ungate(afe);
	dev_info(dev, "AUDIO_TOP_CON0 0x%08x, DAC_CON0 0x%08x\n",
		 j36_afe_read(afe, J36_AUDIO_TOP_CON0),
		 j36_afe_read(afe, J36_AFE_DAC_CON0));

	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "j36", THIS_MODULE, 0,
				&afe->card);
	if (ret)
		return dev_err_probe(dev, ret, "no ALSA card\n");

	/* After the card, so devres tears it down before the card goes away. */
	ret = devm_add_action_or_reset(dev, j36_afe_shutdown, afe);
	if (ret)
		return ret;

	strscpy(afe->card->driver, "j36-afe");
	strscpy(afe->card->shortname, "J36 Ultra");
	strscpy(afe->card->longname, "MediaTek MT6592 AFE DL1 (J36 Ultra)");
	afe->card->private_data = afe;

	ret = snd_pcm_new(afe->card, "MT6592 AFE", 0, 1, 0, &pcm);
	if (ret)
		return dev_err_probe(dev, ret, "no PCM\n");
	pcm->private_data = afe;
	strscpy(pcm->name, "AFE DL1");
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &j36_afe_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev,
				       J36_AFE_BUFFER_BYTES,
				       J36_AFE_BUFFER_BYTES);

	for (i = 0; i < ARRAY_SIZE(j36_afe_controls); ++i) {
		ret = snd_ctl_add(afe->card,
				  snd_ctl_new1(&j36_afe_controls[i], afe));
		if (ret)
			return dev_err_probe(dev, ret, "add %s\n",
					     j36_afe_controls[i].name);
	}

	ret = snd_card_register(afe->card);
	if (ret)
		return dev_err_probe(dev, ret, "register the card\n");

	platform_set_drvdata(pdev, afe);
	dev_info(dev,
		 "playback on DL1, %u KiB ring, speaker amp %s, downlink %s\n",
		 J36_AFE_BUFFER_BYTES / 1024,
		 speaker ? "enabled" : "OFF (speaker=1 asks for it)",
		 codec ? "on" : "off");
	return 0;
}

static const struct of_device_id j36_afe_of_match[] = {
	{ .compatible = "j36,j36-ultra-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, j36_afe_of_match);

static struct platform_driver j36_afe_driver = {
	.probe = j36_afe_probe,
	.driver = {
		.name = "j36-mt6592-audio",
		.of_match_table = j36_afe_of_match,
	},
};
module_platform_driver(j36_afe_driver);

MODULE_DESCRIPTION("J36 Ultra MT6592 AFE DL1 playback adapter");
MODULE_AUTHOR("dArkOS / PowerEngine integration");
MODULE_LICENSE("GPL");
