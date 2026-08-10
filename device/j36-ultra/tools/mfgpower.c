/*
 * mfgpower.c -- power up the MT6592 MFG (GPU) domain from userspace, then prove
 * a Mali-450 MP4 is answering, before anything hands the block to a driver.
 *
 * WHY THIS EXISTS AT ALL.  DRM lima needs three things from this SoC: a device
 * tree node (the tree carries one), a clock provider (two fixed-clocks stand in,
 * because MT6592 has no clock driver in mainline), and a GPU that is powered.
 * The third is the problem.  MT6592 power-gates the MFG subsystem through the
 * SPM's MTCMOS, nothing on the Linux boot path un-gates it -- the MVII LK's own
 * mfg_power_on() is called by the MVII kernel's GPU offload, not by the LK's
 * hand-off to Linux -- and a register read into an unpowered MTK subsystem does
 * not return 0xdeadbeef, it stalls the AXI.  A built-in lima would therefore
 * probe at boot and hang the machine into a watchdog reset with nothing in any
 * log to say why.
 *
 * So lima is built as a module, this runs first, and /init loads the module only
 * if this exits 0.  The whole feature is a directory on the FAT partition and one
 * word on the kernel command line: deleting either restores the previous boot
 * exactly, from any machine that can read an SD card, with no reflash.
 *
 * WHERE THE SEQUENCE COMES FROM.  It is the MVII LK's mfg_power_on() in
 * PowerEngine OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_gpu_offload.c,
 * transcribed step for step and register for register.  That is not reference
 * code someone wrote from a datasheet: the same file goes on to disable the
 * per-core MMUs, submit a PLBU job for a fullscreen quad and run a linked
 * fragment shader on PP0 with a read-back self-test, on this board.  Its
 * constants are silicon, and its comment about the two clock gates -- that the
 * display path already owns SMI common, so writing it again is additive and
 * idempotent -- is why this can run after the LK has lit the panel.
 *
 * The MMU/reset/L2 half of that file is deliberately NOT transcribed.  lima
 * resets the GP, the PPs, both L2s and every MMU itself during probe, and it
 * wants the MMUs enabled with its own page tables, which is the opposite of what
 * the LK sets up for its identity-mapped physical buffers.  Doing any of it here
 * would be work lima undoes, and the version registers can be read without it.
 *
 * The mapping is O_SYNC and non-RAM, which drivers/char/mem.c turns into
 * pgprot_noncached, so these are ordinary device accesses in program order and
 * not something the CPU may cache or reorder against each other.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BIT(n) (1u << (n))

/* SPM, the MTCMOS controller.  Offsets and bits from mt6592_gpu_offload.c. */
#define SPM_BASE 0x10006000u
#define SPM_SIZE 0x1000u
#define SPM_MFG_PWR_CON 0x0214u
#define SPM_PWR_STATUS 0x060cu
#define SPM_PWR_STATUS_S 0x0610u

#define MFG_PWR_STA_MASK BIT(4)
#define PWR_RST_B BIT(0)
#define PWR_ISO BIT(1)
#define PWR_ON BIT(2)
#define PWR_ON_S BIT(3)
#define PWR_CLK_DIS BIT(4)
#define SRAM_PDN 0x0f00u
#define MFG_SRAM_ACK BIT(12)

/* The MFG subsystem's own clock gate, inside the domain being powered. */
#define MFG_CONFIG_BASE 0x13000000u
#define MFG_CONFIG_SIZE 0x1000u
#define MFG_CG_CLR 0x0008u
#define MFG_CG_G3D BIT(0)

/* SMI common lives in the display subsystem, which is already up: the LK left
 * the panel scanning out.  Writing this gate again is what the LK does too. */
#define DISPSYS_BASE 0x14000000u
#define DISPSYS_SIZE 0x1000u
#define DISP_CG_CLR0 0x0108u
#define DISP_SMI_COMMON_GATE BIT(0)

/*
 * The Mali-450 MP4 itself.  0x10000 is mapped rather than the 0x30000 the device
 * tree claims, because the only registers read here are the GP version at
 * +0x0006c and the four PP version registers at +0x09000, +0x0b000, +0x0d000 and
 * +0x0f000 (PPn base + 0x1000).  Nothing is written to the GPU at all.
 *
 * The PP offset had a +0xc on it and that was wrong.  The GP's version register
 * is at 0x6c, so a stray 0xc looks plausible, but the two blocks are not laid out
 * alike: drivers/gpu/drm/lima/lima_regs.h has LIMA_GP_VERSION 0x6C and
 * LIMA_PP_VERSION 0x1000, and PPn base + 0x100c is LIMA_PP_CTRL.  So this read
 * the control register of each pixel processor, got 0 four times, decided the
 * silicon was not a Mali-450 and exited 1 -- while the GP, whose offset was
 * right, reported product 0x0d07 in the same breath.  A GP that answers next to
 * four PPs that read as zero is the signature of this bug, not of a GPU that is
 * powered down: an unpowered Utgard core aborts the access, it does not return 0.
 */
#define MALI_BASE 0x13040000u
#define MALI_SIZE 0x10000u
#define MALI_GP_VERSION 0x0006cu
#define MALI_PP_VERSION_AT(n) (0x08000u + (n) * 0x2000u + 0x1000u)

/* Product IDs the LK's probe already verified against this silicon. */
#define MALI450_GP_PRODUCT 0x0d07u
#define MALI450_PP_PRODUCT 0xcf07u

/*
 * The LK spins on these polls with no delay because it owns the CPU.  Here the
 * wait is 1 ms a round for up to 200 ms, which is four orders of magnitude more
 * patience than an MTCMOS ramp needs and still bounded: the failure mode this
 * guards is "the domain never came up", and the answer to that is to report it
 * and refuse to load the driver, not to hang /init.
 */
#define POLL_ROUNDS 200

static volatile uint32_t *spm;
static volatile uint32_t *mfgcfg;
static volatile uint32_t *dispsys;
static volatile uint32_t *mali;

static uint32_t rd(volatile uint32_t *window, uint32_t offset)
{
	return window[offset / 4];
}

static void wr(volatile uint32_t *window, uint32_t offset, uint32_t value)
{
	window[offset / 4] = value;
}

static volatile uint32_t *map_window(int fd, uint32_t base, uint32_t size,
				     const char *what)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       (off_t)base);

	if (p == MAP_FAILED) {
		fprintf(stderr, "mfgpower: mmap %s at 0x%08x: %s\n", what, base,
			strerror(errno));
		return NULL;
	}
	return (volatile uint32_t *)p;
}

/* Poll until (reg & mask) == expected.  Returns 0 on success. */
static int wait_mask(volatile uint32_t *window, uint32_t offset, uint32_t mask,
		     uint32_t expected)
{
	struct timespec ms = { .tv_sec = 0, .tv_nsec = 1000000 };
	int round;

	for (round = 0; round < POLL_ROUNDS; ++round) {
		if ((rd(window, offset) & mask) == expected)
			return 0;
		nanosleep(&ms, NULL);
	}
	return -1;
}

static int mfg_power_on(void)
{
	uint32_t value;

	printf("mfgpower: MFG_PWR_CON=0x%08x PWR_STATUS=0x%08x PWR_STATUS_S=0x%08x\n",
	       rd(spm, SPM_MFG_PWR_CON), rd(spm, SPM_PWR_STATUS),
	       rd(spm, SPM_PWR_STATUS_S));

	if ((rd(spm, SPM_PWR_STATUS) & MFG_PWR_STA_MASK) &&
	    (rd(spm, SPM_PWR_STATUS_S) & MFG_PWR_STA_MASK)) {
		/* Both status bits already set: something powered this domain
		 * before us.  The LK's own sequence skips the ramp in exactly
		 * this case, and so does this -- re-running PWR_ON on a live
		 * domain is not a no-op. */
		printf("mfgpower: the MFG domain was already powered; leaving the ramp alone\n");
	} else {
		wr(spm, SPM_MFG_PWR_CON, rd(spm, SPM_MFG_PWR_CON) | PWR_ON);
		wr(spm, SPM_MFG_PWR_CON, rd(spm, SPM_MFG_PWR_CON) | PWR_ON_S);
		if (wait_mask(spm, SPM_PWR_STATUS, MFG_PWR_STA_MASK,
			      MFG_PWR_STA_MASK) ||
		    wait_mask(spm, SPM_PWR_STATUS_S, MFG_PWR_STA_MASK,
			      MFG_PWR_STA_MASK)) {
			fprintf(stderr,
				"mfgpower: MFG never acknowledged power on (MFG_PWR_CON=0x%08x)\n",
				rd(spm, SPM_MFG_PWR_CON));
			return -1;
		}

		/* Order matters and it is the LK's order: release the clock and
		 * the isolation cell, assert reset-not, and only then bring the
		 * SRAM out of power-down and wait for its ack. */
		value = rd(spm, SPM_MFG_PWR_CON);
		value &= ~(PWR_CLK_DIS | PWR_ISO);
		value |= PWR_RST_B;
		wr(spm, SPM_MFG_PWR_CON, value);
		wr(spm, SPM_MFG_PWR_CON, rd(spm, SPM_MFG_PWR_CON) & ~SRAM_PDN);
		if (wait_mask(spm, SPM_MFG_PWR_CON, MFG_SRAM_ACK, 0u)) {
			fprintf(stderr,
				"mfgpower: MFG SRAM stayed powered down (MFG_PWR_CON=0x%08x)\n",
				rd(spm, SPM_MFG_PWR_CON));
			return -1;
		}
		printf("mfgpower: MFG domain powered, MFG_PWR_CON=0x%08x\n",
		       rd(spm, SPM_MFG_PWR_CON));
	}

	/* Both gates, in the LK's order.  SMI common first, because the GPU's
	 * path to memory goes through it, and it is already on for the display
	 * -- these registers are write-1-to-clear a gate, so neither write can
	 * disturb what the display owns. */
	wr(dispsys, DISP_CG_CLR0, DISP_SMI_COMMON_GATE);
	wr(mfgcfg, MFG_CG_CLR, MFG_CG_G3D);
	return 0;
}

static int identify(void)
{
	uint32_t gp = rd(mali, MALI_GP_VERSION);
	int cores = 0;
	int n;

	printf("mfgpower: GP version 0x%08x (product 0x%04x, r%up%u)\n", gp,
	       gp >> 16, (gp >> 8) & 0xff, gp & 0xff);
	if ((gp >> 16) != MALI450_GP_PRODUCT) {
		fprintf(stderr,
			"mfgpower: GP product 0x%04x is not the Mali-450's 0x%04x\n",
			gp >> 16, MALI450_GP_PRODUCT);
		return -1;
	}

	for (n = 0; n < 4; ++n) {
		uint32_t pp = rd(mali, MALI_PP_VERSION_AT((uint32_t)n));

		printf("mfgpower: PP%d version 0x%08x (product 0x%04x)\n", n, pp,
		       pp >> 16);
		if ((pp >> 16) == MALI450_PP_PRODUCT)
			cores++;
	}
	if (cores == 0) {
		fprintf(stderr,
			"mfgpower: no pixel processor answered with product 0x%04x\n",
			MALI450_PP_PRODUCT);
		return -1;
	}
	/* Fewer than four is reported and not refused: lima marks pp1..pp3 as
	 * optional for a 450 and runs on whatever answers.  It is still worth
	 * saying, because "MP4" is a claim about this part. */
	printf("mfgpower: Mali-450 MP%d confirmed\n", cores);
	return 0;
}

int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);

	if (fd < 0) {
		fprintf(stderr, "mfgpower: /dev/mem: %s\n", strerror(errno));
		fprintf(stderr,
			"mfgpower: this needs CONFIG_DEVMEM=y; without it the MFG domain cannot be reached from here\n");
		return 1;
	}

	spm = map_window(fd, SPM_BASE, SPM_SIZE, "SPM");
	mfgcfg = map_window(fd, MFG_CONFIG_BASE, MFG_CONFIG_SIZE, "MFG config");
	dispsys = map_window(fd, DISPSYS_BASE, DISPSYS_SIZE, "DISPSYS");
	mali = map_window(fd, MALI_BASE, MALI_SIZE, "Mali");
	close(fd);
	if (!spm || !mfgcfg || !dispsys || !mali)
		return 1;

	if (mfg_power_on())
		return 1;
	if (identify())
		return 1;

	printf("mfgpower: ready for lima\n");
	return 0;
}
