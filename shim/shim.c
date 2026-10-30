/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"
#include "usb.h"
#include "panel_init.h"
#include "panel_init_nodsc.h"

#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL

#define GICD_BASE		0x17100000UL	/* mondrian: intc@17100000, GICD 64K */
#define GICR_BASE		0x17180000UL	/* GICR, 8 x 0x40000 stride */
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x40000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* mondrian DT: <GIC_PPI 11>, INTID = 16 + 11 */
#define PTIMER_INTID		30	/* DT: <GIC_PPI 14> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define DISP_CC_BASE		0x0af00000UL	/* disp_cc clock controller */
#define GDSCR_MDSS		0x9000UL	/* mdss_gdsc power control */
#define MDSS_BASE		0x0ae00000UL	/* qcom,mdss_mdp (SDE) */

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	/* CNTFRQ_EL0 can read 0 right after the ABL hand-off on this SoC, which
	 * made every delay return immediately (all bring-up timing was silently
	 * zero). Fall back to the known 19.2 MHz architected timer frequency. */
	uint64_t f = rsr("cntfrq_el0");
	if (!f)
		f = 19200000;
	uint64_t end = now() + f / 1000 * ms;
	while (now() < end)
		;
}

/* ---- bring-up report channel -------------------------------------------
 * 4-bit result encoded as a hold time, then reset straight back to fastboot:
 *   hold = 1 s + code * 4 s
 * The host measures how long the device is off USB and subtracts a
 * calibrated offset, so one boot reports a 4-bit code without Android.
 */
#define DISP_CC_AHB_CMD_RCGR	0x8324UL	/* disp_cc_mdss_ahb_clk_src */
#define DISP_CC_AHB_BRANCH	0x80a4UL	/* disp_cc_mdss_ahb_clk */

static void __attribute__((noreturn)) report_code(int code)
{
	if (code < 0)
		code = 0;
	if (code > 15)
		code = 15;
	/* The IMEM restart reason does not survive to ABL on this SoC (writes from
	 * non-secure EL1 seem to be dropped), so we cannot force fastboot. Encode
	 * the result in the hold time and let Android boot normally instead. */
	/* No IMEM write here: touching 0x146aa65c from non-secure EL1 appears to
	 * stall the bus on this SoC (it is why the "restart reason = fastboot"
	 * breadcrumb never worked). Hold time alone is our channel. */
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(1000 + (unsigned)code * 4000u);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	/* mondrian (SM8475) has no PSHOLD MMIO register: reset goes through the
	 * PMIC PON on SPMI, so PSCI_SYSTEM_RESET above is the only path we have. */
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	/* Print the registers FIRST, and never dereference 'stage' without a
	 * NULL check: an unset stage pointer made the handler itself fault while
	 * printing, which turned every probe failure into a silent fault loop that
	 * never reached reset(). */
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	if (stage)
		con_str("STAGE", stage);
	reset(10);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

/* MIPI DSI packet-header ECC (standard 24-bit Hamming table). */
static uint8_t dsi_ecc3(uint8_t b0, uint8_t b1, uint8_t b2)
{
	static const uint8_t tbl[24] = {
		0x07, 0x0b, 0x0d, 0x0e, 0x13, 0x15, 0x16, 0x19,
		0x1a, 0x1c, 0x23, 0x25, 0x26, 0x29, 0x2a, 0x2c,
		0x31, 0x32, 0x34, 0x38, 0x1f, 0x2f, 0x37, 0x3b,
	};
	uint32_t v = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
	uint8_t ecc = 0;
	for (int i = 0; i < 24; i++)
		if (v & (1u << i))
			ecc ^= tbl[i];
	return ecc;
}


/* ---- panel init sequence sender ---------------------------------------
 * The DTS stream layout is: [DT][last][VC][ACK][wait_hi][wait_lo][len][payload]
 * Convert each entry into a MIPI DSI packet and hand it to the controller.
 * ---------------------------------------------------------------------- */
/* The real driver (msm_drm.ko: dsi_ctrl_hw_cmn_cmd_engine_en) enables the
 * command engine by setting bit 2 of the register at +0x004, and triggers a
 * command-DMA transfer by writing 1 to +0x090
 * (dsi_ctrl_hw_cmn_trigger_command_dma). We previously only used TRIG_CTRL
 * (0x080), which may be the video/MDP path: if the command engine was left off
 * by ABL, none of our packets could ever go out - and ACK_ERR_STATUS would
 * still read 0. */
static void dsi_cmd_engine_on(uint32_t base)
{
	wr32(base + 0x004UL, rd32(base + 0x004UL) | (1u << 2));
	__asm__ volatile("dsb sy" ::: "memory");
}

static void dsi_sw_trigger(uint32_t base)
{
	wr32(base + 0x080UL, (rd32(base + 0x080UL) & ~0x7u) | 4u);
	wr32(base + 0x090UL, 1u);
	__asm__ volatile("dsb sy" ::: "memory");
}

static int dsi_one_pkt(uint32_t base, uint8_t dt, const uint8_t *pl, unsigned len, uint8_t *buf)
{
	unsigned total;

	if (dt == 0x39) {
		buf[0] = 0x39;
		buf[1] = (uint8_t)(len & 0xff);
		buf[2] = (uint8_t)((len >> 8) & 0xff);
		for (unsigned i = 0; i < len; i++)
			buf[3 + i] = pl[i];
		buf[3 + len] = dsi_ecc3(buf[0], buf[1], buf[2]);
		total = len + 4;
	} else if (dt == 0x15) {
		buf[0] = 0x15;
		buf[1] = len > 0 ? pl[0] : 0;
		buf[2] = len > 1 ? pl[1] : 0;
		buf[3] = dsi_ecc3(buf[0], buf[1], buf[2]);
		total = 4;
	} else {			/* 0x05: zero-parameter short write */
		buf[0] = 0x05;
		buf[1] = len > 0 ? pl[0] : 0;
		buf[2] = 0x00;
		buf[3] = dsi_ecc3(buf[0], buf[1], buf[2]);
		total = 4;
	}

	__asm__ volatile("dsb sy" ::: "memory");
	/* packet mode: see dsi_packet_mode() below */
	wr32(base + 0x038UL, rd32(base + 0x038UL) & ~(1u << 28));
	wr32(base + 0x040UL, rd32(base + 0x040UL) & ~(1u << 16));
	*(volatile uint32_t *)(base + 0x044UL) = (uint32_t)(uintptr_t)buf;
	*(volatile uint32_t *)(base + 0x048UL) = total;
	*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);	/* LP for DCS */
	dsi_sw_trigger(base);
	return 0;
}

static void dsi_settle(unsigned ms)
{
	uint64_t f = rsr("cntfrq_el0");
	uint64_t e;

	if (!f)
		f = 19200000;
	e = now() + f / 1000u * ms;
	while (now() < e)
		;
}

/* Fire one packet from 'buf' (length 'len' includes header+ECC).
 *
 * The controller has two command-DMA modes and ABL leaves it in the wrong one:
 *   CMD_DMA_CTRL.FromFrameBuffer (bit 28) set => the DMA source is a raw frame
 *   buffer and the *hardware* wraps it into DSI packets (cmd-mdp path).
 *   Cleared => the DMA source already contains complete DSI packets, which is
 *   what msm's dsi_cmd_dma_tx() does for DCS writes.
 * Every packet we build by hand is in the second form, so bit 28 must be clear;
 * if it is set the hardware re-wraps our bytes as pixels and the panel only
 * ever sees garbage (or nothing at all).
 * INSERT_DCS_COMMAND (CMD_CFG1 bit 16) is likewise cleared so the hardware
 * does not inject an extra 0x2C/0x3C into an already-built packet. */
static void dsi_packet_mode(uint32_t base)
{
	uint32_t c = rd32(base + 0x038UL);
	uint32_t g = rd32(base + 0x040UL);

	c &= ~(1u << 28);
	wr32(base + 0x038UL, c);
	g &= ~(1u << 16);
	wr32(base + 0x040UL, g);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void dsi_trigger(uint32_t base, uint8_t *buf, unsigned len, int hs)
{
	dsi_packet_mode(base);
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(base + 0x044UL) = (uint32_t)(uintptr_t)buf;
	*(volatile uint32_t *)(base + 0x048UL) = len;
	if (hs)
		*(volatile uint32_t *)(base + 0x038UL) &= ~(1u << 26);
	else
		*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);
	dsi_sw_trigger(base);
}

static unsigned dsi_send_seq(uint32_t base, const uint8_t *seq, unsigned n)
{
	uint8_t *buf = (uint8_t *)0xb8020000UL;		/* low DMA staging area */
	uint64_t f = rsr("cntfrq_el0");
	unsigned i = 0, count = 0;

	if (!f)
		f = 19200000;

	while (i + 7 <= n) {
		uint8_t dt = seq[i];
		unsigned wait = ((unsigned)seq[i + 4] << 8) | seq[i + 5];
		unsigned len = seq[i + 6];
		const uint8_t *pl = seq + i + 7;

		if (i + 7 + len > n)
			break;
		dsi_one_pkt(base, dt, pl, len, buf);
		/* small settle, then the command's own delay */
		uint64_t e = now() + f / 200;		/* 5 ms */
		while (now() < e)
			;
		if (wait) {
			e = now() + f / 1000 * wait;
			while (now() < e)
				;
		}
		i += 7 + len;
		count++;
	}
	return count;
}

uint64_t g_entry_ticks;	/* set at entry: lets probes self-calibrate */

/* Wait until 'ms' milliseconds after shim entry, so internal work (retries,
 * link bring-up) does not pollute the result encoded in the total time. */
static void report_at(unsigned ms)
{
	uint64_t f = rsr("cntfrq_el0");

	if (!f)
		f = 19200000;
	uint64_t target = g_entry_ticks + f / 1000 * ms;
	while (now() < target)
		;
}

/* ---------------------------------------------------------------------------
 * Persistent log channel: ramoops.
 *
 * The DTB reserves 0xa7000000+0x400000 as "ramoops" with console-size=0x200000
 * and pmsg-size=0x200000 and no record-size, so fs/pstore/ram.c (5.10) carves:
 *   console zone @ 0xa7000000, 2 MiB, persistent_ram sig = 0 ^ 0x43474244
 *   pmsg    zone @ 0xa7200000, 2 MiB
 * A zone starts with struct persistent_ram_buffer {u32 sig,start,size; u8 data[]}
 * and persistent_ram_post_init() keeps the data when
 *   sig == 0x43474244 && 0 < size <= zone-12 && 0 <= start <= size
 * so after the next Android boot our text shows up as
 *   /sys/fs/pstore/console-ramoops-0
 * The first 8 GiB are mapped Device-nGnRnE, so stores land in DRAM directly.
 * ------------------------------------------------------------------------- */
#define RAMOOPS_CONSOLE	0xa7000000UL
#define PRZ_SIG		0x43474244u

static char *g_log_p;
static uint32_t g_log_n;

static void log_begin(void)
{
	g_log_p = (char *)(RAMOOPS_CONSOLE + 12);
	g_log_n = 0;
}

static void log_ch(char c)
{
	if (g_log_n < 0x1f0000u) {
		g_log_p[g_log_n++] = c;
		*(volatile uint32_t *)(RAMOOPS_CONSOLE + 8) = g_log_n;
	}
}

static void log_str(const char *s)
{
	while (*s)
		log_ch(*s++);
}

static void log_hex(uint32_t v)
{
	char t[11];
	static const char d[] = "0123456789abcdef";

	t[0] = '0';
	t[1] = 'x';
	for (int i = 0; i < 8; i++)
		t[2 + i] = d[(v >> (28 - 4 * i)) & 0xf];
	t[10] = 0;
	log_str(t);
}

static void log_dec(unsigned v)
{
	char t[11];
	int i = 10;

	t[i] = 0;
	do {
		t[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	log_str(&t[i]);
}

static void log_kv(const char *k, uint32_t v)
{
	log_str(k);
	log_str("=");
	log_hex(v);
	log_str("\n");
}

/* Publish the buffer: start stays 0 so the reader copies data[0..size). */
static void log_commit(void)
{
	*(volatile uint32_t *)(RAMOOPS_CONSOLE + 8) = g_log_n;
	*(volatile uint32_t *)(RAMOOPS_CONSOLE + 4) = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(RAMOOPS_CONSOLE + 0) = PRZ_SIG;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Safety net: the K60 has no PSHOLD register and the only reset path is PSCI,
 * which needs a live CPU. A stalled AXI read (an unclocked register block) or
 * any runaway loop therefore leaves the phone dead until the user holds the
 * power button - which has already cost several rounds.
 *
 * The APSS/Kryo watchdog can turn that into an automatic reboot. Qualcomm
 * layout (SM8250/SM8450 family):
 *   0x00 WDT_RST (write 1 clears)   0x04 WDT_EN   0x08 WDT_STS
 *   0x10 WDT_BARK_TIME   0x14 WDT_BITE_TIME   ticks of the 19.2 MHz XO
 * Both candidate bases are armed; only the real one responds.
 * ------------------------------------------------------------------------- */
#define APSS_WDT_LOW	0x17c10000UL
#define APSS_WDT_HIGH	0x17c20000UL

static void wdt_arm(unsigned seconds)
{
	uint32_t tick = 19200u;			/* 1 ms of 19.2 MHz ticks */
	uint32_t bark = tick * 1000u * (seconds > 2 ? seconds - 2u : seconds);
	uint32_t bite = tick * 1000u * seconds;
	static const uint32_t base[2] = { APSS_WDT_LOW, APSS_WDT_HIGH };

	if (!seconds)
		return;
	for (int i = 0; i < 2; i++) {
		wr32(base[i] + 0x00UL, 1u);	/* clear */
		wr32(base[i] + 0x10UL, bark);
		wr32(base[i] + 0x14UL, bite);
		wr32(base[i] + 0x04UL, 1u);	/* enable */
	}
	__asm__ volatile("dsb sy" ::: "memory");
}


/* prefix a line with its 3-digit index so several photos can be stitched */
static void print_numbered(int *lines, const char *text)
{
	char pre[5];
	int v = *lines;

	pre[0] = (char)('0' + (v / 100) % 10);
	pre[1] = (char)('0' + (v / 10) % 10);
	pre[2] = (char)('0' + v % 10);
	pre[3] = ':';
	pre[4] = 0;
	con_str(pre, text);
	(*lines)++;
}

static void fmt_hex_s(char *out, uint32_t v)
{
	static const char d[] = "0123456789abcdef";

	out[0] = '0';
	out[1] = 'x';
	for (int i = 0; i < 8; i++)
		out[2 + i] = d[(v >> (28 - 4 * i)) & 0xf];
	out[10] = 0;
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	g_entry_ticks = now();
#ifdef K60_WDT_SAFE
	/* Arm the watchdog before touching any hardware: if this image hangs, the
	 * phone reboots itself instead of needing a manual long-press. */
	wdt_arm(K60_WDT_SAFE);
#endif

#ifdef K60_BREADCRUMB
	/* Bring-up probe: prove ABL actually jumped into us. Writing 0x77665500
	 * ("bootloader") to the IMEM restart reason and resetting makes the next
	 * ABL boot land in fastboot, which the host sees on USB. No writes to any
	 * partition are involved. */
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
#endif

#ifdef K60_RSCPROBE
	{
		/* Can the shim read the RSC before XNU takes over? XNU dies on the
		 * first read of 0x17a00000, so this answers whether the block is
		 * reachable at all or only once some owner has been set up. */
		/* RSC is unreachable (AOP never grants the AP its XPU), so vote the
		 * touch rails straight over SPMI: PM8350C LDO2 is periph 0x41 on SID 2.
		 * QPNP APID: bit27 R/W, bits19-16 data high nibble, bits11-4 register,
		 * bits3-0 data low nibble. */
		const uint32_t CORE = 0x0c400000UL, OBS = 0x0c440000UL;
		uint32_t total = (rd32(CORE + 0x04) & 0x3ffu) + (rd32(CORE + 0x08) & 0x3ffu);
		int apid = -1; uint32_t ppid = 0;
		for (uint32_t i = 0; i < total && i < 1024; i++) {
			uint32_t m = rd32(CORE + 0x900u + 4u * i);
			if (!m) continue;
			uint32_t p = (m >> 8) & 0xfffu;
			if ((p >> 8) == 2 && (p & 0xff) == 0x41) { apid = (int)i; ppid = p; break; }
		}
		con_clear(0x00101420);
		con_banner("ios-sweet", "ldoc2 over SPMI", 0xff5f87d7U, 0x00101420, 3);
		con_skip();
		con_hex2("ldoc2 apid", "i=", (uint32_t)(apid < 0 ? 0xffff : apid), "ppid", ppid);
		if (apid >= 0) {
			uint32_t off = OBS + 0x20u * (uint32_t)apid;
			uint32_t en = (uint32_t)(ppid & 0xff) + 0x46u;
			uint32_t cmd_r = (1u << 27) | ((en & 0xffu) << 4);
			uint32_t st = 0, val = 0;
			wr32(off + 0x18, 0);
			wr32(off + 0x00, cmd_r);
			__asm__ volatile("dsb sy" ::: "memory");
			for (unsigned w = 0; w < 2000; w++) { st = rd32(off + 0x08); if (st & 1u) { val = rd32(off + 0x18); break; } }
			con_hex2("ldoc2 en before", "s=", st, "v", val & 0xffu);
			/* write 0x80 (enable) */
			uint32_t cmd_w = ((0x8u) << 16) | ((en & 0xffu) << 4) | 0x0u;
			wr32(off + 0x18, 0);
			wr32(off + 0x00, cmd_w);
			__asm__ volatile("dsb sy" ::: "memory");
			st = 0; val = 0;
			for (unsigned w = 0; w < 2000; w++) { st = rd32(off + 0x08); if (st & 1u) { val = rd32(off + 0x18); break; } }
			con_hex2("ldoc2 en after", "s=", st, "v", val & 0xffu);
		}
		report_at(35000);
	}
#endif

#if defined(K60_SCREENTEST)
	{
		/* Is the panel really driven by our framebuffer writes?
		 * Paint the whole screen a colour no system UI uses (pure red), draw
		 * big white text over it, and hold long enough that a human cannot miss
		 * it. msm_drm would never show this, so "red + text" is unambiguous
		 * proof that writing 0xb8000000 reaches the panel. */
		con_clear(0x00ff0000);
		con_banner("K60 SHIM", "framebuffer console test", 0xffffffff, 0x00ff0000, 3);
		con_str("PROOF", "these pixels were written by our own code");
		con_str("FB", "0xb8000000 1440x3200 stride 5760");
		con_hex2("MDSS", "hw=", rd32(0x0ae00000UL), "dsi_ctrl", rd32(0x0ae94000UL + 0x004UL));
		con_hex2("GCC", "usb_gdsc=", rd32(0x00149004UL), "usb_master", rd32(0x00149018UL));
		con_str("NEXT", "tell me: red screen with white text?");
		report_at(240000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_SURVWRITE) || defined(K60_SURVCHECK)
	{
		/* Which DRAM does ABL leave alone across a reset? Write a marker to
		 * several candidates, reset, then read them back. The splash region is
		 * cleared by ABL (measured), so the panic log needs a home that
		 * survives. print each result immediately so a fault still leaves the
		 * earlier lines on screen. */
		static const uint32_t cand[5] = {
			0xb9200000UL,	/* splash region, past the framebuffer */
			0xa7200000UL,	/* ramoops pmsg zone */
			0x80600000UL,	/* xbl_dtlog_region */
			0xf8000000UL,	/* bank 2, where the payload is relocated */
			0xf8300000UL,	/* bank 2, 3 MiB further up */
		};
		char buf[40];

		con_clear(0x00101420);
#ifdef K60_SURVWRITE
		con_banner("ios-sweet", "memory survival: write", 0xff5f87d7U, 0x00101420, 4);
#else
		con_banner("ios-sweet", "memory survival: read back", 0xff5f87d7U, 0x00101420, 4);
#endif
		con_skip();
		con_rule('-');

		for (int i = 0; i < 5; i++) {
			volatile uint32_t *w = (volatile uint32_t *)cand[i];
			uint32_t tag = 0x53304b36u | ((uint32_t)i << 24);	/* "S0K6" + index */
			uint32_t got;

			fmt_hex_s(buf, cand[i]);
#ifdef K60_SURVWRITE
			w[0] = tag;
			w[1] = ~tag;
			__asm__ volatile("dsb sy" ::: "memory");
			got = w[0];
			con_kv(buf, got == tag ? "written" : "write failed",
			       got == tag ? "OK" : "FAIL", got == tag ? 1 : -1);
#else
			got = w[0];
			if (got == tag) {
				con_kv(buf, "marker intact", "SURVIVED", 1);
			} else if (got == 0) {
				con_kv(buf, "zeroed", "CLEARED", -1);
			} else {
				uint32_t hi = w[1];

				con_kv(buf, hi == ~got ? "partial/other" : "garbage",
				       "OVERWRITTEN", -1);
				fmt_hex_s(buf, got);
				con_kv(buf, buf, "VALUE", 0);
			}
#endif
		}
		con_rule('-');
#ifdef K60_SURVWRITE
		con_str("NEXT", "reset, then boot a K60_SURVCHECK image");
		report_at(20000);
#else
		con_str("DONE", "pick the SURVIVED address for the panic log");
		report_at(180000);
#endif
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PANICBIG)
	{
		/* Read the panic log XNU left in /pram and print it at 3x scale so it
		 * is legible in a phone photo. Pages every 25 s. */
		const uint8_t *log = (const uint8_t *)0x0b9200000UL;
		char line[120];
		int col = 0, lines = 0, page = 0;
		uint32_t first = 0, zero_run = 0;

		while (first < 0x10000u && log[first] == 0)
			first++;

		for (int pg = 0; pg < 3; pg++) {
			int shown = 0;

			con_clear(0x00101420);
			con_big_reset();
			con_big("ios-sweet: XNU panic log", 0xff5f87d7u, 3);

			col = 0;
			zero_run = 0;
			for (uint32_t i = first; i < 0x100000u && shown < 22; i++) {
				uint8_t c = log[i];

				if (c == 0) {
					if (++zero_run > 32768)
						break;
					continue;
				}
				zero_run = 0;
				if (c == '\n' || c == '\r') {
					if (col) {
						line[col] = 0;
						if (lines >= pg * 22 && lines < (pg + 1) * 22) {
							con_big(line, 0xffffffffu, 3);
							shown++;
						}
						lines++;
						col = 0;
					}
					continue;
				}
				if (c < 0x20 || c > 0x7e) {
					if (col < 110)
						line[col++] = '?';
				} else if (col < 110) {
					line[col++] = (char)c;
				} else {
					line[col] = 0;
					if (lines >= pg * 22 && lines < (pg + 1) * 22) {
						con_big(line, 0xffffffffu, 3);
						shown++;
					}
					lines++;
					col = 0;
				}
				if (shown >= 22)
					break;
			}
			if (col && shown < 22) {
				line[col] = 0;
				con_big(line, 0xffffffffu, 3);
			}
			{
				char foot[40];
				int n = 0;
				const char *pre = "page ";
				char num = (char)('1' + pg);

				while (*pre)
					foot[n++] = *pre++;
				foot[n++] = num;
				foot[n++] = '/';
				foot[n++] = '3';
				foot[n] = 0;
				con_big(foot, 0xffaaaaaau, 2);
			}
			report_at((unsigned)(pg + 1) * 25000u);
		}
		(void)page;
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_ELPROBE)
	{
		/* Which exception level do we actually run at, and can EL1 touch the
		 * physical timer? XNU's first panic is "undefined instruction" on
		 * "msr CNTP_CTL_EL0", which at EL1 can only mean CNTHCTL_EL2.EL1PCEN=0
		 * (ABL leaves the counter readable but not the timer controllable). */
		uint64_t cel = rsr("CurrentEL") & 0xc;
		uint32_t hcr = 0, cnthctl = 0;
		int hcr_ok = 0, cnthctl_ok = 0, timer_ok = 0;
		char buf[40];

		/* These are EL2 registers: reading them at EL1 traps, so guard with
		 * the handler by doing them only when CurrentEL == 2. */
		if (cel == 0x8) {
			__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
			hcr_ok = 1;
			__asm__ volatile("mrs %0, cnthctl_el2" : "=r"(cnthctl));
			cnthctl_ok = 1;
		}

		/* Can we program the EL1 physical timer? If EL1PCEN is clear this
		 * traps to EL2 and (with no EL2 handler) can look like an undefined
		 * instruction to the layer above. */
		__asm__ volatile("msr CNTP_CTL_EL0, %0" : : "r"(0UL));
		timer_ok = 1;

		con_clear(0x00101420);
		con_banner("ios-sweet", "exception level / timer probe", 0xff5f87d7U, 0x00101420, 3);
		con_skip();
		fmt_hex_s(buf, (uint32_t)cel);
		con_kv("CurrentEL", buf, cel == 0x8 ? "EL2" : cel == 0x4 ? "EL1" : "EL3/EL0",
		       cel == 0x4 ? 1 : 0);
		if (hcr_ok) {
			fmt_hex_s(buf, hcr);
			con_kv("HCR_EL2", buf, hcr & (1u << 31) ? "RW=1" : "RW=0 (aarch32 EL1!)",
			       (hcr & (1u << 31)) ? 1 : -1);
		} else {
			con_kv("HCR_EL2", "not readable", "we are EL1", 0);
		}
		if (cnthctl_ok) {
			fmt_hex_s(buf, cnthctl);
			con_kv("CNTHCTL_EL2", buf, (cnthctl & 2) ? "EL1PCEN=1" : "EL1PCEN=0 (EL1 timer trapped)",
			       (cnthctl & 2) ? 1 : -1);
		} else {
			con_kv("CNTHCTL_EL2", "not readable", "we are EL1", 0);
		}
		con_kv("timer write", timer_ok ? "accepted" : "faulted", timer_ok ? "OK" : "PANIC", timer_ok ? 1 : -1);
		con_str("NOTE", "hold 60 s, then reset");
		report_at(60000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_GPUPOWER2)
	{
		/* GPU power-up, following drivers/clk/qcom/gdsc.c exactly:
		 *   GDSCR: bit31 PWR_ON (status), bit2 SW_OVERRIDE, bit1 HW_CONTROL,
		 *          bit0 SW_COLLAPSE (1 = collapse = OFF, so power ON = clear it)
		 *   gpu_cx_gdsc : gdscr 0x9108, status in gds_hw_ctrl 0x953c bit31,
		 *                 flags VOTABLE | RETAIN_FF_ENABLE
		 *   gpu_gx_gdsc : gdscr 0x905c, clamp_io 0x9504 bit0, 3 resets,
		 *                 status in gdscr+4 (0x9060) bit16 POWER_UP_COMPLETE
		 * My first attempt wrote bit31, which is the status read-back, not the
		 * request - hence "no effect".
		 *   hold 20 s  CX was already on
		 *   hold 25 s  CX powered by us
		 *   hold 30 s  GX powered too
		 *   hold 35 s  CX refused
		 */
		const uint32_t CX_DSCR = 0x03d99108UL, CX_HW = 0x03d9953cUL, CX_CFG = 0x03d9910cUL;
		const uint32_t GX_DSCR = 0x03d9905cUL, GX_CFG = 0x03d99060UL, GX_CLAMP = 0x03d99504UL;
		uint32_t cx0 = rd32(CX_DSCR), cx_hw0 = rd32(CX_HW);
		uint32_t cx1, gx0 = rd32(GX_DSCR), gx1 = 0, cfg = 0;
		unsigned hold = 35;
		char buf[40];
		int cx_ok = 0, gx_ok = 0;

		con_clear(0x00101420);
		con_banner("ios-sweet", "GPU power (gdsc semantics)", 0xff5f87d7U, 0x00101420, 3);
		con_skip();
		fmt_hex_s(buf, cx0);   con_kv("CX gdscr before", buf, (cx0 & 1) ? "COLLAPSED" : "on", (cx0 & 1) ? -1 : 1);
		fmt_hex_s(buf, cx_hw0); con_kv("CX hw_ctrl", buf, (cx_hw0 >> 31) ? "up" : "down", (cx_hw0 >> 31) ? 1 : -1);

		if ((cx_hw0 >> 31) && !(cx0 & 1)) {
			cx_ok = 1;
			hold = 25;
		} else {
			/* SW_OVERRIDE=1, HW_CONTROL=0, SW_COLLAPSE=0 ; keep wait fields */
			uint32_t w = (cx0 & ~0x3u) | 0x4u;

			wr32(CX_DSCR, w);
			/* retain memories/peripherals + retain flops, as gdsc_enable does */
			wr32(CX_CFG, rd32(CX_CFG) | (1u << 11));
			wr32(CX_DSCR, rd32(CX_DSCR) | (3u << 13));
			for (int i = 0; i < 300; i++) {
				if (rd32(CX_HW) >> 31)
					break;
				delay_ms(1);
			}
			delay_ms(2);
			cx_ok = (int)(rd32(CX_HW) >> 31);
			hold = cx_ok ? 30 : 35;
		}
		cx1 = rd32(CX_DSCR);
		fmt_hex_s(buf, cx1); con_kv("CX gdscr after", buf, cx_ok ? "ON" : "still collapsed", cx_ok ? 1 : -1);
		fmt_hex_s(buf, rd32(CX_HW)); con_kv("CX hw_ctrl after", buf, cx_ok ? "up" : "down", cx_ok ? 1 : -1);

		if (cx_ok) {
			/* GX: deassert its three resets (BCR writes), clear clamp, then power */
			wr32(0x03d99058UL, 0);	/* GPU_CC_GX_BCR: 0 = out of reset */
			wr32(0x03d99358UL, 0);	/* ACD BCR */
			wr32(0x03d9958cUL, 0);	/* ACD IROOT BCR */
			wr32(GX_CLAMP, rd32(GX_CLAMP) & ~1u);	/* deassert clamp_io */
			wr32(GX_DSCR, (gx0 & ~0x3u) | 0x4u);
			for (int i = 0; i < 300; i++) {
				if (rd32(GX_CFG) & (1u << 16))
					break;
				delay_ms(1);
			}
			gx1 = rd32(GX_DSCR);
			cfg = rd32(GX_CFG);
			gx_ok = (cfg >> 16) & 1;
			fmt_hex_s(buf, gx1); con_kv("GX gdscr", buf, gx_ok ? "UP" : "not up", gx_ok ? 1 : -1);
			fmt_hex_s(buf, cfg); con_kv("GX cfg", buf, gx_ok ? "power up complete" : "waiting", gx_ok ? 1 : -1);
			if (gx_ok) {
				hold = 30;
				fmt_hex_s(buf, rd32(0x03d00000UL));
				con_kv("RBBM", buf, "readable", 1);
			}
		}
		con_hex2("HOLD", "s=", hold, "cx_ok", (uint32_t)cx_ok);
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_GPUPOWER)
	{
		/* Can the shim bring the GPU's power domains up without RPMh?
		 * Self-reporting (no photo needed) so it can be run from fastboot:
		 *   hold 20 s  cx GDSC was already powered and RBBM reads
		 *   hold 25 s  cx GDSC off -> powered up by us, RBBM reads
		 *   hold 30 s  powered up but RBBM still unreadable
		 *   hold 35 s  cx GDSC write had no effect (still off)
		 * Stages print on screen as well, so a photo adds detail. */
		uint32_t cx_before = rd32(0x03d99108UL);
		uint32_t cx_after, rbbm = 0;
		int powered = 0;
		unsigned hold = 20;

		con_clear(0x00101420);
		con_banner("ios-sweet", "GPU power domain", 0xff5f87d7U, 0x00101420, 4);
		con_skip();
		{
			char buf[40];

			fmt_hex_s(buf, cx_before);
			con_kv("CX GDSC before", buf, (cx_before >> 31) ? "ON" : "off",
			       (cx_before >> 31) ? 1 : -1);
		}
		if (cx_before >> 31) {
			con_str("ACTION", "already powered, reading RBBM");
			hold = 20;
		} else {
			con_str("ACTION", "requesting power (GDSC bit31)");
			wr32(0x03d99108UL, cx_before | 0x80000000u);
			for (int i = 0; i < 200; i++) {
				if (rd32(0x03d99108UL) >> 31)
					break;
				delay_ms(1);
			}
			hold = (rd32(0x03d99108UL) >> 31) ? 25 : 35;
		}
		cx_after = rd32(0x03d99108UL);
		powered = (int)(cx_after >> 31);
		{
			char buf[40];

			fmt_hex_s(buf, cx_after);
			con_kv("CX GDSC after", buf, powered ? "ON" : "off", powered ? 1 : -1);
		}
		if (powered) {
			rbbm = rd32(0x03d00000UL);
			{
				char buf[40];

				fmt_hex_s(buf, rbbm);
				con_kv("RBBM", buf, rbbm ? "READABLE" : "zero?", rbbm ? 1 : -1);
			}
			if (!rbbm && hold < 30)
				hold = 30;
		}
		con_hex2("RESULT", "hold=", hold, "0x3d00000", rbbm ? 1 : 0);
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_GPUPROBE)
	{
		/* GPU step 1: is the block even reachable? READ ONLY, and each result
		 * is printed immediately so a bus stall still leaves the earlier lines
		 * on screen. GCC is always alive; GPU_CC and the 3D0 block may not be. */
		char buf[40];

		con_clear(0x00101420);
		con_banner("ios-sweet", "GPU reachability (read only)", 0xff5f87d7U, 0x00101420, 4);
		con_skip();
		con_rule('-');

		fmt_hex_s(buf, rd32(0x00181000UL));
		con_kv("GCC gpu_bcr", buf, "GCC alive", 1);
		fmt_hex_s(buf, rd32(0x00181010UL));
		con_kv("GCC memnoc_gfx", buf, "gpu ddr clk", 0);
		fmt_hex_s(buf, rd32(0x00181018UL));
		con_kv("GCC snoc_dvm", buf, "gpu axi clk", 0);
		con_rule('-');

		/* GPU_CC power domain status: bit31 of the GDSC register */
		{
			uint32_t cx = rd32(0x03d99108UL);
			uint32_t gx = rd32(0x03d9905cUL);

			fmt_hex_s(buf, cx);
			con_kv("GPU_CC cx_gdsc", buf, (cx >> 31) ? "POWERED" : "off",
			       (cx >> 31) ? 1 : -1);
			fmt_hex_s(buf, gx);
			con_kv("GPU_CC gx_gdsc", buf, (gx >> 31) ? "POWERED" : "off",
			       (gx >> 31) ? 1 : -1);
		}
		con_rule('-');

		/* Only touch the 3D0 block when the CX domain says it is on. */
		if (rd32(0x03d99108UL) >> 31) {
			fmt_hex_s(buf, rd32(0x03d00000UL));
			con_kv("GPU RBBM", buf, "block readable", 1);
			fmt_hex_s(buf, rd32(0x03d00004UL));
			con_kv("GPU RBBM+4", buf, "id reg", 0);
		} else {
			con_kv("GPU RBBM", "skipped", "CX DOMAIN OFF", -1);
		}
		con_rule('-');
		con_str("NOTE", "reset in 60 s, back to fastboot");
		report_at(60000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PRAMTEST)
	{
		/* Validate the panic-log chain end to end without involving XNU:
		 * write a marker into the pram region, reset, and the next boot's
		 * K60_XNULOG image must show it. Proves (a) the region is readable and
		 * writable, (b) it survives an ABL round trip. */
		char *p2 = (char *)0x0b9200000UL;
		const char *msg = "K60PRAMTEST ok - this text survived an ABL round trip\n"
				  "second line: pram at 0xb9200000 is readable and writable\n";

		for (uint32_t i = 0; i < 0x1000; i++)
			p2[i] = 0;
		for (uint32_t i = 0; msg[i]; i++)
			p2[i] = msg[i];
		__asm__ volatile("dsb sy" ::: "memory");

		con_clear(0x00101420);
		con_banner("ios-sweet", "pram write test", 0xff5f87d7U, 0x00101420, 4);
		con_skip();
		con_str("WROTE", msg);
		con_str("NEXT", "reset, then boot xnu-log2.img to read it back");
		report_at(20000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PANICREAD)
	{
		/* XNU writes its panic log into the /pram region that our ADT
		 * advertises (phys_base + mem_size - 1 MiB = 0xe9f00000, 1 MiB).
		 * DRAM survives the reset, so after a power cycle we can just read it
		 * back and put it on screen - no serial cable needed. */
		stage = "panicread: read log";
		const uint8_t *log = (const uint8_t *)0x0b9200000UL;
		char line[64];
		int col = 0, lines = 0, started = 0;
		uint32_t zero_run = 0;

		con_clear(0x00101420);
		con_banner("ios-sweet", "XNU panic log @ 0xb9200000", 0xff5f87d7U, 0x00101420, 4);
		con_skip();

		for (uint32_t i = 0; i < 0x100000u && lines < 60; i++) {
			uint8_t c = log[i];

			if (c == 0) {
				if (++zero_run > 4096)
					break;
				continue;
			}
			zero_run = 0;
			started = 1;
			if (c == '\n' || c == '\r') {
				if (col) {
					line[col] = 0;
					con_str("", line);
					col = 0;
					lines++;
				}
				continue;
			}
			if (c < 0x20 || c > 0x7e) {
				if (col < (int)sizeof(line) - 2)
					line[col++] = '?';
			} else if (col < 62) {
				line[col++] = (char)c;
			} else {
				line[col] = 0;
				con_str("", line);
				col = 0;
				lines++;
			}
		}
		if (col) {
			line[col] = 0;
			con_str("", line);
			lines++;
		}
		if (!started)
			con_str("EMPTY", "no text in the panic region");
		con_skip();
		con_rule('-');
		con_hex2("END", "lines=", (uint32_t)lines, 0, 0);
		con_str("HOLD", "600 s - photo this screen");
		report_at(600000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_XNULOOK)
	{
		/* What did ABL actually leave in the initrd region? fastfetch layout,
		 * one photo answers everything. */
		uint64_t lo = 0, hi = 0;
		uint32_t w[4] = { 0, 0, 0, 0 };
		uint32_t zeros = 0;
		uint32_t magic_off = 0xffffffffu;
		uint32_t span_kb = 0;
		char buf[40];

		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &lo) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &hi) || hi <= lo) {
			lo = hi = 0;
		}
		if (lo) {
			uint64_t span = hi - lo;

			span_kb = (uint32_t)(span >> 10);
			for (int i = 0; i < 4; i++)
				w[i] = rd32(lo + 4 * i);
			for (uint32_t off = 0; off < 0x10000; off += 4)
				if (rd32(lo + off) == 0)
					zeros++;
			uint64_t cap = span > 0x4000000ULL ? 0x4000000ULL : span;
			for (uint64_t off = 0; off + 8 <= cap; off += 8) {
				const char *m = (const char *)(lo + off);

				if (m[0] == 'S' && m[1] == 'W' && m[2] == 'E' &&
				    m[3] == 'E' && m[4] == 'T' && m[5] == 'X' &&
				    m[6] == 'N' && m[7] == 'U') {
					magic_off = (uint32_t)off;
					break;
				}
			}
		}

		con_clear(0x00101420);
		con_banner("ios-sweet", "initrd inspection", 0xff5f87d7U, 0x00101420, 5);
		con_skip();
		con_rule('=');
		con_section("INITRD");
		fmt_hex_s(buf, (uint32_t)lo);
		con_kv("START", buf, lo ? "PRESENT" : "MISSING", lo ? 1 : -1);
		fmt_hex_s(buf, (uint32_t)hi);
		con_kv("END", buf, lo ? "PRESENT" : "MISSING", lo ? 1 : -1);
		if (lo) {
			char kb[16];
			int n = 0, v = (int)span_kb;
			char t[12];
			int k = 0;

			do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
			while (k) kb[n++] = t[--k];
			kb[n++] = 'K'; kb[n++] = 'B'; kb[n] = 0;
			con_kv("SPAN", kb, "REGION", 0);
			fmt_hex_s(buf, w[0]);
			con_kv("WORD0", buf, w[0] ? "DATA" : "ZERO", w[0] ? 1 : 0);
			fmt_hex_s(buf, w[1]);
			con_kv("WORD1", buf, w[1] ? "DATA" : "ZERO", w[1] ? 1 : 0);
			fmt_hex_s(buf, w[2]);
			con_kv("WORD2", buf, w[2] ? "DATA" : "ZERO", w[2] ? 1 : 0);
			fmt_hex_s(buf, w[3]);
			con_kv("WORD3", buf, w[3] ? "DATA" : "ZERO", w[3] ? 1 : 0);
			{
				uint32_t b0 = rd32(lo) & 0xff, b1 = (rd32(lo) >> 8) & 0xff;
				char gz[20];
				int n2 = 0;
				const char *h = "0123456789abcdef";
				gz[n2++] = '0'; gz[n2++] = 'x';
				gz[n2++] = h[b0 >> 4]; gz[n2++] = h[b0 & 0xf];
				gz[n2++] = h[b1 >> 4]; gz[n2++] = h[b1 & 0xf];
				gz[n2] = 0;
				con_kv("BYTES 0-1", gz,
				       b0 == 0x1f && b1 == 0x8b ? "GZIP!" : "not gzip",
				       b0 == 0x1f && b1 == 0x8b ? 1 : -1);
			}
			{
				char z[16];
				int n2 = 0, v = (int)zeros;
				char t[12];
				int k = 0;

				do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
				while (k) z[n2++] = t[--k];
				z[n2] = 0;
				con_kv("ZERO WORDS/64K", z, zeros ? "mostly empty" : "all data",
				       zeros ? 0 : 1);
			}
			if (magic_off != 0xffffffffu) {
				fmt_hex_s(buf, magic_off);
				con_kv("MAGIC", buf, "FOUND", 1);
			} else {
				con_kv("MAGIC", "SWEETXNU", "NOT FOUND", -1);
			}
		}
		con_rule('=');
		con_kv("CONCLUSION", "ABL did not deliver our ramdisk here", "SEE ABOVE", -1);
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_XNUSCAN)
	{
		/* Self-reporting (no photo needed): scan the ABL initrd region for the
		 * SWEETXNU magic and encode the answer in the hold time.
		 *   hold = 20 + offset_in_MB      magic found (20..80)
		 *   hold = 100                    not found in the first 64 MiB
		 *   hold = 150                    no initrd in /chosen
		 * The probe adds ~15 s of boot overhead on top. */
		uint64_t lo = 0, hi = 0;
		unsigned hold = 150;
		uint32_t found_mb = 0xffffffffu;

		if (fdt_chosen_u64(dtb, "linux,initrd-start", &lo) &&
		    fdt_chosen_u64(dtb, "linux,initrd-end", &hi) && hi > lo) {
			uint64_t span = hi - lo;

			if (span > 0x4000000ULL)
				span = 0x4000000ULL;
			hold = 100;
			for (uint64_t off = 0; off + 8 <= span; off += 8) {
				const char *m = (const char *)(lo + off);

				if (m[0] == 'S' && m[1] == 'W' && m[2] == 'E' &&
				    m[3] == 'E' && m[4] == 'T' && m[5] == 'X' &&
				    m[6] == 'N' && m[7] == 'U') {
					found_mb = (uint32_t)(off >> 20);
					hold = 20u + found_mb;
					break;
				}
			}
			(void)found_mb;
		}
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_FF)
	{
		/* fastfetch-style status board: the shim's whole state on one page,
		 * aligned and colour coded, so a photo is easy to read (and it does not
		 * look like a crash dump). */
		uint32_t hw_mdss = rd32(0x0ae00000UL);
		uint32_t dsi_ctrl = rd32(0x0ae94000UL + 0x004UL);
		uint32_t ufs_hcs = 0, ufs_ver = 0;
		char buf[40];

		con_clear(0x00101420);
		con_banner("ios-sweet", "K60 . mondrian . SM8475", 0xff5f87d7U, 0x00101420, 5);
		con_skip();
		con_rule('=');
		con_kv("SYSTEM", "ios-sweet port of HackiOS", "RUNNING", 1);
		con_kv("HARDWARE", "Redmi K60 (mondrian) . SM8475 . 8 cores", "OK", 1);
		con_kv("BOOT", "fastboot boot . boot.img v4 . bare-metal shim", "OK", 1);
		con_kv("CONSOLE", "framebuffer 1440x3200 @ 0xb8000000", "OK", 1);
		con_rule('-');
		con_section("DISPLAY");
		fmt_hex_s(buf, hw_mdss);
		con_kv("MDSS", buf, hw_mdss ? "RESPONDING" : "SILENT", hw_mdss ? 1 : -1);
		fmt_hex_s(buf, dsi_ctrl);
		con_kv("DSI", buf, "BOOTLOADER OWNS IT", 0);
		con_rule('-');
		con_section("STORAGE");
		ufs_ver = rd32(0x01d84000UL + 0x000UL);
		ufs_hcs = rd32(0x01d84000UL + 0x008UL);
		fmt_hex_s(buf, ufs_ver);
		con_kv("UFS VERSION", buf, "HS-G4 LINK", 1);
		fmt_hex_s(buf, ufs_hcs);
		con_kv("UFS HCS", buf, "SCSI BLOCKED", -1);
		con_rule('-');
		con_section("USB");
		con_kv("EUSB2 PHY", "0x088e3000 reads 0x0b0b0b0b (bus junk)", "RAILS OFF", -1);
		con_kv("LOG LINK", "screen + photo is the working channel", "ACTIVE", 1);
		con_rule('-');
		con_section("NEXT");
		con_kv("STEP", "boot XNU (payload now gzip'd for this ABL)", "PENDING", 0);
		con_rule('=');
		con_str("  hold", "300 s, then reset to Android");
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBPHY)
	{
		/* Wake the eUSB2 PHY and print a before/after snapshot.
		 * Only GCC (known-alive) and the documented PHY registers are written;
		 * if the PHY stays silent we stop instead of poking further. */
		uint32_t before[8], after[8];
		int rc;

		usb_phy_snapshot(before);
		rc = usb_phy_init();
		usb_phy_snapshot(after);

		con_clear(0x00102030);
		con_banner("K60 USB PHY", rc ? "still silent" : "PHY alive", 0xffffffff, 0x00102030, 5);
		con_hex2("BEFORE", "rev=", before[0], "rev4", before[1]);
		con_hex2("BEFORE", "common0=", before[2], "ctrl2", before[3]);
		con_hex2("BEFORE", "utmi5=", before[4], "gcc_bcr1", before[5]);
		con_hex2("BEFORE", "gcc_bcr2=", before[6], "phy_aux", before[7]);
		con_hex2("AFTER", "rev=", after[0], "rev4", after[1]);
		con_hex2("AFTER", "common0=", after[2], "ctrl2", after[3]);
		con_hex2("AFTER", "utmi5=", after[4], "gcc_bcr1", after[5]);
		con_hex2("AFTER", "gcc_bcr2=", after[6], "phy_aux", after[7]);
		con_str("RESULT", rc ? "PHY does not answer - rails or clock missing"
				     : "PHY initialised, ready for DWC3");
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBDUMP)
	{
		/* Photograph-friendly register dump: 4 words per row with the offset
		 * printed alongside, so a single screenshot carries ~60 values.
		 * READ ONLY. */
		con_clear(0x00102030);
		con_banner("K60 USB DUMP", "read-only", 0xffffffff, 0x00102030, 5);
		for (unsigned row = 0; row < 8; row++) {
			uint32_t off = 0x0c100UL + row * 0x10UL;
			char lab[8];
			uint32_t a = rd32(0x0a600000UL + off);
			uint32_t b = rd32(0x0a600000UL + off + 4);
			uint32_t c = rd32(0x0a600000UL + off + 8);
			uint32_t d = rd32(0x0a600000UL + off + 12);

			lab[0] = 'c';
			lab[1] = "0123456789abcdef"[(off >> 12) & 0xf];
			lab[2] = "0123456789abcdef"[(off >> 8) & 0xf];
			lab[3] = "0123456789abcdef"[(off >> 4) & 0xf];
			lab[4] = "0123456789abcdef"[off & 0xf];
			lab[5] = 0;
			con_hex2(lab, "a=", a, "b", b);
			con_hex2("", "c=", c, "d", d);
		}
		con_hex2("DEV", "c700=", rd32(0x0a600000UL + 0xc700UL), "c704", rd32(0x0a600000UL + 0xc704UL));
		con_hex2("DEV", "c708=", rd32(0x0a600000UL + 0xc708UL), "c70c", rd32(0x0a600000UL + 0xc70cUL));
		con_hex2("DEV", "c720=", rd32(0x0a600000UL + 0xc720UL), "c724", rd32(0x0a600000UL + 0xc724UL));
		con_hex2("QSCR", "f8800=", rd32(0x0a6f8800UL), "f8810", rd32(0x0a6f8810UL));
		con_hex2("QSCR", "f8830=", rd32(0x0a6f8830UL), "f8808", rd32(0x0a6f8808UL));
		con_hex2("PHY", "88e3000=", rd32(0x088e3000UL), "88e3004", rd32(0x088e3004UL));
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBSCAN)
	{
		/* Read-only hunt for the real DWC3 core: find the offset inside the
		 * 1 MiB "ssusb" window whose word looks like GSNPSID (0x55xxxxxx), then
		 * print the handful of values needed to reason about the wrapper.
		 * Nothing is written anywhere - the earlier GCTL writes are what wedged
		 * the AXI bus. */
		uint32_t base = 0x0a600000UL;
		uint32_t found_off = 0xffffffffu, found_val = 0;

		for (uint32_t off = 0; off < 0x100000UL; off += 0x1000UL) {
			uint32_t v = rd32(base + off);

			if ((v >> 24) == 0x55) {
				found_off = off;
				found_val = v;
				break;
			}
		}

		con_clear(0x00102030);
		con_banner("K60 USB SCAN", "read-only, nothing written", 0xffffffff, 0x00102030, 6);
		con_str("SEARCH", "ssusb@0xa600000 + off, looking for 0x55xxxxxx");
		con_hex2("FOUND", "off=", found_off, "val", found_val);
		con_hex2("W0", "at 0x0=", rd32(base + 0x0UL), "at 0x1000", rd32(base + 0x1000UL));
		con_hex2("W1", "at 0xc100=", rd32(base + 0xc100UL), "at 0xc120", rd32(base + 0xc120UL));
		con_hex2("W2", "at 0x0c00=", rd32(base + 0x0c00UL), "at 0x2000", rd32(base + 0x2000UL));
		con_hex2("QSCR", "hs_phy=", rd32(0x0a6f8810UL), "ss_phy", rd32(0x0a6f8830UL));
		con_hex2("GCC", "usb_gdsc=", rd32(0x00149004UL), "usb_mast", rd32(0x00149018UL));
		con_hex2("MDSS", "hw=", rd32(0x0ae00000UL), "sanity", rd32(0x0ae00004UL));
		con_str("NOTE", "if FOUND=0xffffffff the core is not at this window");
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBSCREEN)
	{
		/* USB bring-up with the *screen* as the log: everything the DWC3 probe
		 * learns is printed as whole words a human can read out loud, so no
		 * hold-time decoding and no guessing. */
		extern uint32_t usb_dbg[8];
		int rc = usb_log_init();
		const char *txt;

		con_clear(0x00102030);
		con_banner("K60 USB", "log channel bring-up", 0xffffffff, 0x00102030, 6);
		con_hex2("SNPSID", "id=", usb_dbg[0], "want 55", (usb_dbg[0] >> 24));
		con_hex2("GCTL", "after=", usb_dbg[1], "prtcap", (usb_dbg[1] >> 12) & 3u);
		con_hex2("DSTS", "sts=", usb_dbg[2], "DCFG", usb_dbg[3]);
		if (rc == 0)
			txt = "OK - controller running";
		else if (rc == -1)
			txt = "GDSC not powered";
		else if (rc == -2)
			txt = "clock still gated";
		else if (rc == -3)
			txt = "DCTL reset stuck";
		else if (rc == -4)
			txt = "SNPSID wrong - block dead";
		else if (rc == -5)
			txt = "GCTL core reset stuck";
		else if (rc == -6)
			txt = "could not select DEVICE mode";
		else if (rc == -100)
			txt = "DEPSTARTCFG failed";
		else if (rc == -101)
			txt = "ep0 SETEPCONFIG failed";
		else if (rc == -102)
			txt = "ep0 SETTRANSFRESOURCE failed";
		else if (rc == -103)
			txt = "bulk SETEPCONFIG failed";
		else if (rc == -104)
			txt = "bulk SETTRANSFRESOURCE failed";
		else
			txt = "unknown failure";
		con_str("RESULT", txt);
		con_hex2("EPCMD", "last=", usb_dbg[4], "status", usb_dbg[5]);
		if (rc == 0) {
			int ok = usb_log_wait_ready(20000);

			con_str("HOST", ok ? "configured - text log live" : "no SET_CONFIGURATION from host");
			if (ok) {
				usb_log_puts("K60USB hello from usbscreen\n");
				usb_log_hold(4000);
				con_str("SENT", "8 bytes on bulk IN 0x81");
			}
		}
		con_str("NOTE", "hold 300 s, then reset");
		report_at(300000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBPROBE)
	{
		/* The shape every future probe should have: bring the USB log channel
		 * up first, then report hardware state as TEXT instead of encoding it
		 * in hold times and asking the user to watch the panel. */
		int rc = usb_log_init();
		int ok = 0;

		if (rc == 0)
			ok = usb_log_wait_ready(20000);

		if (!ok) {
			/* No channel. Show why: the DWC3 identification registers are the
			 * first thing usb_log_init() checks, so print them raw. */
			char b[40];

			con_clear(0x00102030);
			con_banner("K60 USB", "log channel unavailable", 0xffffffff, 0x00102030, 2);
			con_hex2("init", "rc=", (uint32_t)(-rc), "stage", 0);
			fmt_hex_s(b, usb_dbg[0]);
			con_kv("GSNPSID", b, (usb_dbg[0] >> 24) == 0x55 ? "dwc3 ok" : "no dwc3 here", 0);
			fmt_hex_s(b, usb_dbg[1]);
			con_kv("GCTL", b, "", 0);
			fmt_hex_s(b, usb_dbg[2]);
			con_kv("DSTS", b, "", 0);
			fmt_hex_s(b, usb_dbg[3]);
			con_kv("DCFG", b, "", 0);
			/* Is the eUSB2 PHY reachable at all? Garbage here means the PHY
			 * block is not clocked, which would explain DSTS.connected = 0. */
			fmt_hex_s(b, rd32(0x088e3000UL + 0x00));
			con_kv("PHY 0x00", b, "", 0);
			fmt_hex_s(b, rd32(0x088e3000UL + 0x08));
			con_kv("PHY 0x08", b, "", 0);
			fmt_hex_s(b, rd32(0x088e3000UL + 0x38));
			con_kv("PHY 0x38", b, "", 0);
			fmt_hex_s(b, rd32(0x0a6f8800UL + 0x10));
			con_kv("QSCR HS", b, "", 0);
			fmt_hex_s(b, rd32(0x0a6f8800UL + 0x30));
			con_kv("QSCR SS", b, "", 0);
			report_at(60000);
			smc(PSCI_SYSTEM_RESET, 0, 0, 0);
			for (;;)
				__asm__ volatile("wfe");
		}

		con_clear(0x00102030);
		con_banner("K60 PROBE", "usb log channel", 0xffffffff, 0x00102030, 2);
		con_str("CHAN", "usb text log live");
		con_hex2("MDSS", "hw=", rd32(0x0ae00000UL), "ver", rd32(0x0ae00004UL));
		con_hex2("DSI", "ctrl=", rd32(0x0ae94000UL + 0x004UL),
			 "status0", rd32(0x0ae94008CUL - 0x000UL));
		con_hex2("DSI", "cmd_dma_ctrl=", rd32(0x0ae94000UL + 0x038UL),
			 "cmd_cfg1", rd32(0x0ae94000UL + 0x040UL));
		con_hex2("DSI", "trig_ctrl=", rd32(0x0ae94000UL + 0x080UL),
			 "trig_dma", rd32(0x0ae94000UL + 0x08cUL));
		con_hex2("DSI", "at90=", rd32(0x0ae94000UL + 0x090UL),
			 "at94", rd32(0x0ae94000UL + 0x094UL));
		con_hex2("DSI", "dma_base=", rd32(0x0ae94000UL + 0x044UL),
			 "dma_len", rd32(0x0ae94000UL + 0x048UL));
		con_hex2("PHY", "pll=", rd32(0x0ae94400UL + 0x03cUL),
			 "rev", rd32(0x0ae94400UL));
		con_hex2("GCC", "usb_gdsc=", rd32(0x00149004UL),
			 "usb_master", rd32(0x00149018UL));
		con_hex2("GCC", "ufs_gdsc=", rd32(0x00100000UL + 0x87004UL),
			 "disp_gdsc", rd32(0x0af09000UL));
		con_str("DONE", "end of report");
		usb_log_hold(6000);
		report_at(12000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_WDT)
	{
		/* Watchdog experiment: the single most useful piece of infrastructure
		 * we are missing. A bus-stalled CPU hangs with no exception handler, so
		 * every bad register probe costs the user a manual long-press. If the
		 * APSS watchdog can be armed, a hang turns into a 20 s auto-reboot.
		 *
		 * Qualcomm APSS/Kryo WDT (same block on SM8250/SM8450):
		 *   0x0 WDT_RST (write 1 clears), 0x4 WDT_EN, 0x8 WDT_STS,
		 *   0x10 WDT_BARK_TIME, 0x14 WDT_BITE_TIME (ticks of the 19.2 MHz XO)
		 * Arming both candidate bases; whichever exists will fire. */
		uint32_t ticks = 19200000u / 2;		/* 500 ms of XO ticks */
		uint32_t bark = ticks * (K60_WDT * 2u - 2u);
		uint32_t bite = ticks * (K60_WDT * 2u);

		static const uint32_t base[2] = { 0x17c10000UL, 0x17c20000UL };

		for (int i = 0; i < 2; i++) {
			wr32(base[i] + 0x00UL, 1u);	/* clear */
			wr32(base[i] + 0x10UL, bark);
			wr32(base[i] + 0x14UL, bite);
			wr32(base[i] + 0x04UL, 1u);	/* enable */
		}
		__asm__ volatile("dsb sy" ::: "memory");
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_USBLOG)
	{
		/* USB log channel bring-up. No display, no UFS: just the controller,
		 * then ~20 s of text so the host can prove the channel works.
		 * Failure is reported through the hold time (5 s per stage). */
		int rc = usb_log_init();

		if (rc == 0) {
			char line[64];
			int ok = usb_log_wait_ready(15000);

			usb_log_puts("K60USB log channel up\n");
			for (int i = 0; ok && i < 60; i++) {
				int n = 0;
				const char *p = "K60USB tick ";
				char num[8];
				int k = 0;

				while (*p)
					line[n++] = *p++;
				do {
					num[k++] = (char)('0' + i % 10);
					i /= 10;
				} while (i && k < 7);
				while (k)
					line[n++] = num[--k];
				line[n++] = '\n';
				usb_log_write(line, (unsigned)n);
				usb_log_hold(250);
			}
			usb_log_puts("K60USB done\n");
			usb_log_hold(3000);
			report_at(20000);
		} else {
			/* one code per failure point, compressed into the hold time:
			 *   -1 GDSC / -2 clock / -3 reset stuck / -4 GSNPSID
			 *   -100..-104 the five endpoint commands, in order */
			unsigned hold_s;

			if (rc <= -100)
				hold_s = 25u + 3u * (unsigned)(-rc - 100);
			else
				hold_s = 10u + 3u * (unsigned)(-rc - 1);
			report_at(hold_s * 1000u);
		}
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_LOG)
	{
		/* Prove the ramoops log channel: write a short text buffer, reset,
		 * then read /sys/fs/pstore/console-ramoops-0 on Android. */
		log_begin();
		log_str("K60LOG v1 hello from the shim\n");
		log_kv("entry_ticks", (uint32_t)g_entry_ticks);
		log_kv("cntfrq", (uint32_t)rsr("cntfrq_el0"));
		log_kv("mdss_hw", rd32(0x0ae00000UL));
		log_kv("dsi_pll", rd32(0x0ae94400UL + 0x03cUL));
		log_kv("dsi_clk_status", rd32(0x0ae94000UL + 0x11cUL));
		log_kv("disp_gdsc", rd32(0x0af09000UL));
		log_str("tail: end of log\n");
		log_commit();
		report_at(3000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSGT)
	{
		/* Binary search helper: PASS (-> fastboot) iff the UFSHCI init stage is
		 * <= K60_GT. Stage 0 means it fully succeeded. */
		int stage = ufs_host_init();
		int pass = (stage <= K60_GT);
		wr32(IMEM_RESTART_REASON, pass ? RESTART_BOOTLOADER : 0u);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(500);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSSTAGE)
	{
		/* Report which UFSHCI init stage fails, encoded as a hold time:
		 *   hold = 2 s + 6 s * stage   (stage 0 = success)
		 * then reset into Android so the host can time it. */
		int stage = ufs_take_over();
		delay_ms(2000 + (unsigned)(stage < 0 ? 20 : (stage > 20 ? 20 : stage)) * 3000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_BIN)
	{
		/* Binary probe harness: answer one yes/no question per boot.
		 * Result: go to fastboot when the test PASSES (fast, ~10 s) and stay
		 * in Android when it FAILS, so a pass can be detected immediately.
		 * Select the question with -DK60_Q=<n>:
		 *   1  UFS host takeover succeeds
		 *   2  GPT lookup of "opcust" succeeds
		 *   3  WRITE(10) of one block at opcust+16 returns 0
		 *   4  the read-back of that block matches */
		int pass = 0;
		uint32_t first = 0;
#if K60_Q == 1
		pass = (ufs_part_first("opcust", &first) == 0);
#elif K60_Q == 2
		{
			uint32_t n = 0;
			pass = (ufs_part_first("opcust", &first) == 0 && first != 0);
			(void)n;
		}
#elif K60_Q == 3
		if (ufs_part_first("opcust", &first) == 0 && first) {
			static uint8_t wb[4096] __attribute__((aligned(4096)));
			for (unsigned i = 0; i < sizeof(wb); i++)
				wb[i] = 0;
			static const char m[] = "K60BIN-OK";
			for (int i = 0; m[i]; i++)
				wb[i] = (uint8_t)m[i];
			pass = (ufs_probe_write(first + 16, wb, 0x20, 1) == 0);
		}
#elif K60_Q == 34
		{
			/* Is our own image / descriptor memory above 4 GiB? The UFS host
			 * has no 64-bit addressing (CAP.64AS=0), so that would explain a
			 * rejected request-list base. */
			pass = (((uintptr_t)shim_main >> 32) != 0) ||
			       (((uintptr_t)&ufs_reg_read >> 32) != 0);
		}
#elif K60_Q == 35
		{
			/* Is the image loaded below 4 GiB (i.e. DMA-safe)? */
			pass = (((uintptr_t)shim_main >> 32) == 0);
		}
#elif K60_Q == 65
		{
			/* Control: unconditional pass. If this does not land in fastboot,
			 * the harness/判定 chain itself is broken. */
			pass = 1;
		}
#elif K60_Q == 62
		{
			/* Control: GICD_TYPER at 0x17100004 is always live. */
			uint32_t v = rd32(0x17100004UL);
			pass = (v != 0 && v != 0xffffffffu);
		}
#elif K60_Q == 63
		{
			/* Is the MMU already on when ABL jumps to us? (SCTLR_EL1.M) */
			uint64_t sctlr = rsr("sctlr_el1");
			pass = (int)(sctlr & 1u);
		}
#elif K60_Q == 64
		{
			/* Are we actually at EL2/EL1 as expected? report EL bits */
			uint64_t cur = rsr("CurrentEL");
			pass = (((cur >> 2) & 3) == 1);	/* EL1 */
		}
#elif K60_Q == 60
		{
			/* After the GCC UFS bring-up, is the UFS register block readable? */
			int rc = gcc_ufs_bring_up();
			uint32_t hcs = ufs_reg_read(0x30);
			pass = (rc == 0 && hcs != 0 && hcs != 0xffffffffu);
		}
#elif K60_Q == 61
		{
			/* Does the UFS PHY power domain come up? */
			(void)gcc_ufs_bring_up();
			pass = gcc_ufs_gdsc_on();
		}
#elif K60_Q == 52
		{
			/* Control: is a plain RAM read working? The FDT magic at the pointer
			 * ABL handed us must be 0xd00dfeed. */
			pass = (dtb != 0 && __builtin_bswap32(rd32(dtb)) == 0xd00dfeedu);
		}
#elif K60_Q == 53
		{
			/* Same idea, unswapped, in case the FDT is big-endian already. */
			pass = (dtb != 0 && rd32(dtb) == 0xd00dfeedu);
		}
#elif K60_Q == 50
		{
			/* TLMM GPIO0 config register (0x100000) — always-on block, must be
			 * non-zero on a device whose panel reset line is configured. */
			uint32_t v = rd32(0x0f000000UL + 0x100000UL);
			pass = (v != 0 && v != 0xffffffffu);
		}
#elif K60_Q == 51
		{
			/* TLMM block says something at all (raw != 0)? */
			pass = (rd32(0x0f000000UL + 0x100000UL) != 0u);
		}
#elif K60_Q == 47
		{
			/* GCC GDSCR reads all-ones (no slave / bus fault)? */
			pass = (rd32(0x00100000UL + 0x87004UL) == 0xffffffffu);
		}
#elif K60_Q == 48
		{
			/* GCC GDSCR reads exactly zero (readable but zero)? */
			pass = (rd32(0x00100000UL + 0x87004UL) == 0u);
		}
#elif K60_Q == 49
		{
			/* TLMM (always-on pinctrl) readable? its GPIO cfg regs are non-zero */
			uint32_t v = rd32(0x0f000000UL + 0x1000UL);
			pass = (v != 0 && v != 0xffffffffu);
		}
#elif K60_Q == 45
		{
			/* GCC readable? Use the UFS PHY GDSC status register (0x87004):
			 * both "readable" and "is the UFS power domain on" in one bit. */
			uint32_t g = rd32(0x00100000UL + 0x87004UL);
			pass = (int)((g >> 31) & 1);		/* PWR_ON */
		}
#elif K60_Q == 46
		{
			/* GCC register block readable at all (GDSCR not 0/0xffffffff)? */
			uint32_t g = rd32(0x00100000UL + 0x87004UL);
			pass = (g != 0 && g != 0xffffffffu);
		}
#elif K60_Q == 43
		{
			/* Is the UFS register block alive at all (HCS non-zero)? */
			pass = (ufs_reg_read(0x30) != 0 && ufs_reg_read(0x30) != 0xffffffffu);
		}
#elif K60_Q == 44
		{
			/* HCE bit already set by the bootloader? */
			pass = (int)(ufs_reg_read(0x34) & 1u);
		}
#elif K60_Q == 40
		{
			/* CAP readable (non-zero) after a host init attempt? */
			(void)ufs_host_init();
			pass = (ufs_cap_post != 0 && ufs_cap_post != 0xffffffffu);
		}
#elif K60_Q == 41
		{
			/* CAP.64AS after init (not the pre-init false reading) */
			(void)ufs_host_init();
			pass = (int)((ufs_cap_post >> 24) & 1);
		}
#elif K60_Q == 42
		{
			/* UFSHCI version >= 0x300 (3.0/4.0 layout questions) */
			(void)ufs_host_init();
			pass = ((ufs_ver_post & 0xff00u) >= 0x0300u);
		}
#elif K60_Q == 30
		{
			/* After a host init attempt, is UTRLBA non-zero at all? */
			(void)ufs_host_init();
			pass = (ufs_reg_read(0x50) != 0);
		}
#elif K60_Q == 31
		{
			/* CAP bit 24: 64-bit addressing support */
			pass = (int)((ufs_reg_read(0x00) >> 24) & 1);
		}
#elif K60_Q == 32
		{
			/* CAP bits[4:0] = NUTRS-1 (>= 0x1f means 32+ request slots) */
			pass = ((ufs_reg_read(0x00) & 0x1f) >= 0x1f);
		}
#elif K60_Q == 33
		{
			/* Does UTRLBA match what we wrote (recheck with explicit value)? */
			(void)ufs_host_init();
			pass = ufs_utrl_base_matches();
		}
#elif K60_Q == 5
		{
			/* Is the display clock block readable at all? (non-zero DATA) */
			uint32_t v = rd32(DISP_CC_BASE + DISP_CC_AHB_CMD_RCGR);
			pass = (v != 0 && v != 0xffffffffu);
		}
#elif K60_Q == 6
		{
			/* Can software power up mdss_gdsc? */
			uint32_t g = rd32(DISP_CC_BASE + GDSCR_MDSS);
			uint32_t v = g & ~1u;
			v |= 0xfu << 20;
			v |= 0xfu << 16;
			v |= 0xfu << 12;
			v &= ~(1u << 4);
			wr32(DISP_CC_BASE + GDSCR_MDSS, v);
			__asm__ volatile("dsb sy" ::: "memory");
			delay_ms(100);
			pass = (int)((rd32(DISP_CC_BASE + GDSCR_MDSS) >> 31) & 1);
		}
#elif K60_Q == 4
		if (ufs_part_first("opcust", &first) == 0 && first) {
			static uint8_t rb[4096] __attribute__((aligned(4096)));
			static const char m[] = "K60BIN-OK";
			for (unsigned i = 0; i < sizeof(rb); i++)
				rb[i] = 0;
			int rrc = ufs_probe_read(first + 16, rb);
			pass = (rrc == 0);
			for (int i = 0; pass && m[i]; i++)
				if (rb[i] != (uint8_t)m[i])
					pass = 0;
		}
#endif
		wr32(IMEM_RESTART_REASON, pass ? RESTART_BOOTLOADER : 0u);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(500);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSCHAIN)
	{
		/* One boot, whole chain, result in the hold time:
		 *   0 = GPT lookup of "opcust" failed
		 *   1 = write command returned an error
		 *   2 = write ok but the read-back differs
		 *   3 = write + read-back both fine
		 * hold = 2 s + code * 5 s, then reset into Android so the block can
		 * also be checked from the host afterwards. */
		static uint8_t b1[4096] __attribute__((aligned(4096)));
		static uint8_t b2[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int code = 0;
		int prc = ufs_part_first("opcust", &first);

		if (prc || !first) {
			code = 0;
		} else {
			for (unsigned i = 0; i < sizeof(b1); i++) {
				b1[i] = 0;
				b2[i] = 0x5a;
			}
			static const char m[] = "K60UFSCHAIN-OK";
			for (int i = 0; m[i]; i++)
				b1[i] = (uint8_t)m[i];
			int wrc = ufs_probe_write(first + 16, b1, 0x20, 1);
			if (wrc) {
				code = 1;
			} else {
				int rrc = ufs_probe_read(first + 16, b2);
				int match = (!rrc);
				for (int i = 0; match && m[i]; i++)
					if (b2[i] != (uint8_t)m[i])
						match = 0;
				code = match ? 3 : 2;
			}
		}
		wr32(IMEM_RESTART_REASON, 0);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2000 + (unsigned)code * 5000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_NEGO)
	{
		/* hold = 20 + 5*gear -> negotiated and read back (gear 1..4 expected)
		 *   hold 60 = read of PA_MAXRXHSGEAR failed
		 *   hold 70 = DME_SET sequence failed
		 *   hold 80 = read-back failed
		 *   hold 100 = UFS init failed */
		unsigned hold = 100000;
		if (ufs_take_over() == 0) {
			uint32_t gear = 0xff;
			int rc = ufs_negotiate(&gear);
			if (rc == 0)
				hold = 20000 + 5000 * (unsigned)(gear & 0x7);
			else
				hold = (unsigned)(50000 + rc * 10000);
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_NODSC)
	{
		/* Replay the init sequence with DSC disabled (0x91 PPS dropped, 0x90 -> 0)
		 * then push one frame of raw RGB888. If the panel shows colour, it is
		 * running uncompressed and we can draw real images. */
		uint32_t base = 0x0ae94000UL;
		unsigned hold = 40;
		uint32_t hw = 0;
		uint64_t f = rsr("cntfrq_el0");

		if (!f)
			f = 19200000;

		if (disp_bring_up(&hw) == 0) {
			reg_note(base + 0x044UL);
			reg_note(base + 0x048UL);
			reg_note(base + 0x038UL);
			reg_note(base + 0x080UL);
			(void)dsi_send_seq(base, panel_init_nodsc, sizeof(panel_init_nodsc));

			/* solid red bands: raw RGB888, 16 x 16 KiB */
			uint8_t *pkt = (uint8_t *)0xb8020000UL;
			const unsigned chunk = 16384;
			for (int c = 0; c < 16; c++) {
				pkt[0] = 0x39;
				pkt[1] = (uint8_t)(chunk & 0xff);
				pkt[2] = (uint8_t)((chunk >> 8) & 0xff);
				for (unsigned i2 = 0; i2 < chunk; i2 += 3) {
					pkt[3 + i2] = (uint8_t)(c & 1 ? 0x00 : 0xff);
					pkt[4 + i2] = (uint8_t)(c & 2 ? 0xff : 0x00);
					pkt[5 + i2] = (uint8_t)(c & 4 ? 0xff : 0x00);
				}
				pkt[3 + chunk] = dsi_ecc3(pkt[0], pkt[1], pkt[2]);
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x044UL) = (uint32_t)(uintptr_t)pkt;
				*(volatile uint32_t *)(base + 0x048UL) = chunk + 4;
				*(volatile uint32_t *)(base + 0x038UL) &= ~(1u << 26);	/* HS */
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
				uint64_t e = now() + f / 8;
				while (now() < e)
					;
			}
			hold = 60000;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_RAWFB)
	{
		/* Frame-buffer DMA path: CMD_DMA_CTRL.FromFrameBuffer (bit 28) stays
		 * SET, so the controller reads RAW pixels out of memory and builds the
		 * DSI long packets itself (this is how msm's cmd-mdp path works).
		 * Hand-built 0x39 packets are wrong for that mode, which is why our
		 * earlier "packets" showed up as white noise: the hardware was
		 * wrapping the packet bytes as pixels.
		 *   45 s = done, ACK clean
		 *   55 s = done, ACK error bits
		 *   40 s = display bring-up failed */
		uint32_t base = 0x0ae94000UL;
		uint8_t *fb = (uint8_t *)0xb9000000UL;
		const unsigned LINE = 1440u * 3u;
#ifdef K60_RAWFB_SMALL
		const unsigned LINES = 2u;	/* 8640 B, pix3 packet scale */
		const unsigned pace = 12u;	/* ms between packets */
		const int bands = 1;		/* one frame: four horizontal bands */
#else
		const unsigned LINES = 4u;
		const unsigned pace = 1u;
		const int bands = 0;		/* cycling colours, 2 rounds */
#endif
		const unsigned ROWS = 3200u;
		static const uint8_t cols[4][3] = {
			{ 0xff, 0x00, 0x00 },
			{ 0x00, 0xff, 0x00 },
			{ 0x00, 0x00, 0xff },
			{ 0xff, 0xff, 0xff },
		};
		unsigned hold = 40;
		uint32_t hw = 0, errs = 0;

		if (disp_bring_up(&hw) == 0) {
#ifndef K60_RAWFB_NOCMD
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x11", 1, fb);
			dsi_settle(130);
			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x51\xff", 2, fb);
			dsi_settle(30);
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x29", 1, fb);
			dsi_settle(60);
#endif
#ifdef K60_RAWFB_DSC_OFF
			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x90\x00", 2, fb);
			dsi_settle(30);
#endif
#ifdef K60_RAWFB_ENGINE_ON
			dsi_cmd_engine_on(base);
#endif
			/* frame-buffer mode + high speed */
			wr32(base + 0x038UL,
			     (rd32(base + 0x038UL) | (1u << 28)) & ~(1u << 26));

			for (int round = 0; round < (bands ? 1 : 2) && !errs; round++) {
				for (int c = 0; c < (bands ? 1 : 4) && !errs; c++) {
					for (unsigned y = 0; y < ROWS; y += LINES) {
						unsigned n = (ROWS - y) < LINES ? (ROWS - y) : LINES;
						unsigned len = n * LINE;
						/* bands = 1: colour chosen per row (four 800-row
						 * stripes); otherwise one colour for the frame. */
						int rc = bands ? (int)(y / 800u) : c;

						for (unsigned i = 0; i < len; i += 3) {
							fb[i] = cols[rc > 3 ? 3 : rc][0];
							fb[i + 1] = cols[rc > 3 ? 3 : rc][1];
							fb[i + 2] = cols[rc > 3 ? 3 : rc][2];
						}
						__asm__ volatile("dsb sy" ::: "memory");
						wr32(base + 0x044UL, (uint32_t)(uintptr_t)fb);
						wr32(base + 0x048UL, len);
						__asm__ volatile("dsb sy" ::: "memory");
						wr32(base + 0x080UL,
						     (rd32(base + 0x080UL) & ~0x7u) | 4u);
						dsi_settle(pace);
					}
					errs |= rd32(base + 0x064UL) & 0xffu;
					dsi_settle(1200);
				}
			}
#ifdef K60_RAWFB_SMALL
			hold = 90;
#else
			hold = 45;
#endif
			if (errs)
				hold += 10;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_CYCLE)
	{
		/* Unmissable variant: cycle full-screen solid RED -> GREEN -> BLUE ->
		 * WHITE three times, ~1 s each, for ~35 s. A static pattern was easy to
		 * miss, a flashing saturated full screen is not.
		 *   45 s = done, ACK clean
		 *   55 s = done, ACK error bits
		 *   40 s = display bring-up failed */
		uint32_t base = 0x0ae94000UL;
		uint8_t *pkt = (uint8_t *)0xb9000000UL;
		const unsigned LINE = 1440u * 3u;
		const unsigned LINES = 4u;	/* 17284 B/packet, pix4 scale */
		const unsigned ROWS = 3200u;
		static const uint8_t cols[4][3] = {
			{ 0xff, 0x00, 0x00 },	/* red */
			{ 0x00, 0xff, 0x00 },	/* green */
			{ 0x00, 0x00, 0xff },	/* blue */
			{ 0xff, 0xff, 0xff },	/* white */
		};
		unsigned hold = 40;
		uint32_t hw = 0, errs = 0;

		if (disp_bring_up(&hw) == 0) {
			/* Sleep OUT first: ABL parks the panel in sleep before handing
			 * over, and a panel in sleep shows nothing no matter how much
			 * pixel data we push. pix3/pix4 (the only runs that ever showed
			 * pixels) sent 0x11; rgb8/cyc1 forgot it and stayed black. */
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x11", 1, pkt);
			dsi_settle(130);
			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x51\xff", 2, pkt);
			dsi_settle(30);
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x29", 1, pkt);
			dsi_settle(60);
#ifdef K60_CYCLE_DSC_OFF
			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x90\x00", 2, pkt);
			dsi_settle(30);
#endif
#ifdef K60_CYCLE_ONCE
			const int rounds = 1, ncol = 1;	/* one solid red frame, long hold */
#else
			const int rounds = 3, ncol = 4;
#endif
			for (int round = 0; round < rounds && !errs; round++) {
				for (int c = 0; c < ncol && !errs; c++) {
					dsi_one_pkt(base, 0x05, (const uint8_t *)"\x2c", 1, pkt);
					dsi_settle(5);
					for (unsigned y = 0; y < ROWS; y += LINES) {
						unsigned n = (ROWS - y) < LINES ? (ROWS - y) : LINES;
						unsigned len = n * LINE;

						for (unsigned i = 0; i < len; i += 3) {
							pkt[3 + i] = cols[c][0];
							pkt[4 + i] = cols[c][1];
							pkt[5 + i] = cols[c][2];
						}
						pkt[0] = 0x39;
						pkt[1] = (uint8_t)(len & 0xff);
						pkt[2] = (uint8_t)((len >> 8) & 0xff);
						pkt[3 + len] = dsi_ecc3(pkt[0], pkt[1], pkt[2]);
						dsi_trigger(base, pkt, len + 4, 1);
						dsi_settle(1);
					}
					errs |= rd32(base + 0x064UL) & 0xffu;
					dsi_settle(1000);	/* 1 s of solid colour */
				}
			}
#ifdef K60_CYCLE_LONG
			hold = 120;
#else
			hold = 45;
#endif
			if (errs)
				hold += 10;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_RGB8)
	{
		/* Surgical DSC test: ABL has already initialised the panel with DSC on.
		 * We only flip compression off (DCS 0x90 <- 0x00) and push ONE full
		 * uncompressed RGB888 frame, so the screen either shows four clean
		 * colour bands (non-DSC accepted) or the usual white noise (DSC still on).
		 *   45 s = frame sent, ACK clean
		 *   55 s = frame sent, ACK error bits set
		 *   40 s = display bring-up failed */
		uint32_t base = 0x0ae94000UL;
		uint8_t *pkt = (uint8_t *)0xb9000000UL;	/* low DMA staging */
		const unsigned LINE = 1440u * 3u;	/* 4320 B/row, RGB888 */
		/* 2 rows per DSI packet: 8644 bytes, the same order of magnitude as
		 * the 8/16 KiB packets that demonstrably reached the panel. 15 rows
		 * (64.8 KiB) produced NOTHING visible, so packet size is a suspect. */
		const unsigned LINES = 2u;
		const unsigned ROWS = 3200u;
		unsigned hold = 40;
		uint32_t hw = 0;
		uint32_t errs = 0;

		if (disp_bring_up(&hw) == 0) {
			reg_note(base + 0x044UL);
			reg_note(base + 0x048UL);
			reg_note(base + 0x038UL);
			reg_note(base + 0x080UL);
			reg_note(base + 0x064UL);

			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x51\xff", 2, pkt);
			dsi_settle(30);
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x29", 1, pkt);
			dsi_settle(50);
#ifdef K60_RGB8_DSC_OFF
			/* Only this build disables compression. The other keeps the
			 * panel exactly as ABL configured it, so a full frame with small
			 * packets isolates "packet size" from "DSC mode". */
			dsi_one_pkt(base, 0x15, (const uint8_t *)"\x90\x00", 2, pkt);
			dsi_settle(30);
#endif
			dsi_one_pkt(base, 0x05, (const uint8_t *)"\x2c", 1, pkt);
			dsi_settle(5);

			for (unsigned y = 0; y < ROWS; y += LINES) {
				unsigned n = (ROWS - y) < LINES ? (ROWS - y) : LINES;
				unsigned len = n * LINE;

				for (unsigned r = 0; r < n; r++) {
					unsigned yy = y + r;
					uint8_t *p = pkt + 3 + r * LINE;
					uint8_t v0, v1, v2;

					if (yy < 800) {
						v0 = 0xff; v1 = 0x00; v2 = 0x00;
					} else if (yy < 1600) {
						v0 = 0x00; v1 = 0xff; v2 = 0x00;
					} else if (yy < 2400) {
						v0 = 0x00; v1 = 0x00; v2 = 0xff;
					} else {
						v0 = 0xff; v1 = 0xff; v2 = 0xff;
					}
					for (unsigned i = 0; i < LINE; i += 3) {
						p[i] = v0;
						p[i + 1] = v1;
						p[i + 2] = v2;
					}
				}
				pkt[0] = 0x39;
				pkt[1] = (uint8_t)(len & 0xff);
				pkt[2] = (uint8_t)((len >> 8) & 0xff);
				pkt[3 + len] = dsi_ecc3(pkt[0], pkt[1], pkt[2]);
				dsi_trigger(base, pkt, len + 4, 1);
				dsi_settle(6);	/* let the 64.8 KiB packet drain */
				errs |= rd32(base + 0x064UL) & 0xffu;
			}
			hold = 45;
			if (errs)
				hold += 10;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_INITSEQ)
	{
		/* Replay the panel's own init sequence from the device tree.
		 *   60 s = sequence replayed (hold so the panel can be observed)
		 *   40 s = display bring-up failed */
		uint32_t base = 0x0ae94000UL;
		unsigned hold = 40;
		uint32_t hw = 0;

		if (disp_bring_up(&hw) == 0) {
			reg_note(base + 0x044UL);
			reg_note(base + 0x048UL);
			reg_note(base + 0x038UL);
			reg_note(base + 0x080UL);
			reg_note(base + 0x064UL);
			(void)dsi_send_seq(base, panel_init_seq, sizeof(panel_init_seq));
			hold = 60000;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PIX)
	{
		/* Make the panel show something unmistakable: solid colour bands.
		 *   60 s hold after pushing pixels (user-confirmable)
		 *   40 s = display bring-up failed
		 *   30 s = pixel push reported an error
		 * The panel is DSC-compressed in command mode, so the exact colours may
		 * look wrong — but ANY change proves our write path reaches the panel. */
		uint32_t base = 0x0ae94000UL;
		uint8_t *dma = (uint8_t *)0xb8003000UL;
		unsigned hold = 40;
		uint32_t hw = 0;
		uint64_t f = rsr("cntfrq_el0");

		if (!f)
			f = 19200000;

		if (disp_bring_up(&hw) == 0) {
			reg_note(base + 0x044UL);
			reg_note(base + 0x048UL);
			reg_note(base + 0x038UL);
			reg_note(base + 0x080UL);
			reg_note(base + 0x064UL);

			*(volatile uint32_t *)(base + 0x044UL) = 0xb8003000UL;
			*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);	/* LP */

			/* helper: send one DSI short write of a DCS command */
			for (int step = 0; step < 3; step++) {
				uint8_t cmd = (uint8_t)(step == 0 ? 0x11 : step == 1 ? 0x29 : 0x51);
				/* MIPI DSI short packet is 4 bytes: DI, D0, D1, ECC */
				dma[0] = 0x05;
				dma[1] = cmd;
				dma[2] = (uint8_t)(step == 2 ? 0xff : 0x00);
				dma[3] = dsi_ecc3(dma[0], dma[1], dma[2]);
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x048UL) = 4;
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
				uint64_t e = now() + f / 8;
				while (now() < e)
					;
			}

			/* WR_MEM_START (0x2C) then eight 8 KiB long writes in colour bands */
			dma[0] = 0x05; dma[1] = 0x2c; dma[2] = 0x00;
			dma[3] = dsi_ecc3(dma[0], dma[1], dma[2]);
			__asm__ volatile("dsb sy" ::: "memory");
			*(volatile uint32_t *)(base + 0x048UL) = 4;
			*(volatile uint32_t *)(base + 0x080UL) =
				(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
			{
				uint64_t e = now() + f / 32;
				while (now() < e)
					;
			}

			static const uint8_t cols[16][3] = {
				{0xff, 0x00, 0x00}, {0x00, 0xff, 0x00}, {0x00, 0x00, 0xff},
				{0xff, 0xff, 0xff}, {0xff, 0x00, 0xff}, {0x00, 0xff, 0xff},
				{0xff, 0xff, 0x00}, {0x00, 0x00, 0x00},
				{0xff, 0x00, 0x00}, {0x00, 0xff, 0x00},
				{0x00, 0x00, 0xff}, {0xff, 0xff, 0xff},
				{0xff, 0x00, 0xff}, {0x00, 0xff, 0xff},
				{0xff, 0xff, 0x00}, {0x40, 0x40, 0x40},
			};
			const unsigned chunk = 16384;
			for (int c = 0; c < 16; c++) {
				uint8_t *pkt = (uint8_t *)(0xb8010000UL + (uint32_t)c * 0x2000UL);
				pkt[0] = 0x39;			/* DCS long write */
				pkt[1] = (uint8_t)(chunk & 0xff);
				pkt[2] = (uint8_t)((chunk >> 8) & 0xff);
				for (unsigned i = 0; i < chunk; i += 3) {
					pkt[3 + i] = cols[c][0];
					pkt[4 + i] = cols[c][1];
					pkt[5 + i] = cols[c][2];
				}
				pkt[3 + chunk] = dsi_ecc3(pkt[0], pkt[1], pkt[2]);
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x044UL) = (uint32_t)(uintptr_t)pkt;
				*(volatile uint32_t *)(base + 0x048UL) = chunk + 4;
				/* pixel payloads go in HS mode: clear LOW_POWER */
				*(volatile uint32_t *)(base + 0x038UL) &= ~(1u << 26);
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
				uint64_t e = now() + f / 8;	/* 125 ms per chunk */
				while (now() < e)
					;
				/* and back to LP for the next command */
				*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);
			}
			hold = 60000;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DCS_SEQ)
	{
		/* DCS sequence: 0x11 (sleep out) -> 0x29 (display on) -> 0x51 (brightness)
		 * then hold 60 s so the panel can be observed.
		 *   60 s = sequence sent (expected path)
		 *   40 s = display bring-up failed */
		uint32_t base = 0x0ae94000UL;
		uint8_t *pkt = (uint8_t *)0xb8003000UL;
		unsigned hold = 40;
		uint32_t hw = 0;

		if (disp_bring_up(&hw) == 0) {
			reg_note(base + 0x044UL);
			reg_note(base + 0x048UL);
			reg_note(base + 0x038UL);
			reg_note(base + 0x080UL);
			reg_note(base + 0x064UL);

			*(volatile uint32_t *)(base + 0x044UL) = 0xb8003000UL;
			*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);	/* LP */

			/* helper macro-ish block for each command */
			{
				uint64_t f = rsr("cntfrq_el0");
				if (!f) f = 19200000;
				/* 1. sleep out (0 params) */
				pkt[0] = 0x05; pkt[1] = 0x11; pkt[2] = 0x00;
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x048UL) = 3;
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
				{
					uint64_t e = now() + f / 8;	/* 125 ms */
					while (now() < e) ;
				}
				/* 2. display on (0 params) */
				pkt[0] = 0x05; pkt[1] = 0x29; pkt[2] = 0x00;
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x048UL) = 3;
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
				{
					uint64_t e = now() + f / 32;	/* 30 ms */
					while (now() < e) ;
				}
				/* 3. brightness 0xFF (1 param) */
				pkt[0] = 0x05; pkt[1] = 0x51; pkt[2] = 0xff;
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(base + 0x048UL) = 3;
				*(volatile uint32_t *)(base + 0x080UL) =
					(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
			}
			hold = 60000;	/* hold so the panel can be seen */
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DCS29)
	{
		/* Send MIPI DCS 0x29 (Display On) to the panel:
		 *   20 s = command sent, no error status   -> panel may now be awake
		 *   30 s = error status set after send
		 *   40 s = display bring-up failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		uint32_t base = 0x0ae94000UL;
		uint8_t *pkt = (uint8_t *)0xb8003000UL;	/* low DMA buffer */

		if (disp_bring_up(&hw) == 0) {
			pkt[0] = 0x05;	/* DI: DCS short write */
			pkt[1] = 0x29;	/* D0: Display On */
			pkt[2] = 0x00;	/* D1 */
			pkt[3] = 0x00;	/* ECC */
			__asm__ volatile("dsb sy" ::: "memory");

			reg_note(base + 0x044UL);	/* DMA_BASE  */
			reg_note(base + 0x048UL);	/* DMA_LEN   */
			reg_note(base + 0x038UL);	/* CMD_DMA_CTRL */
			reg_note(base + 0x080UL);	/* TRIG_CTRL */
			reg_note(base + 0x064UL);	/* ACK_ERR_STATUS */

			*(volatile uint32_t *)(base + 0x044UL) = 0xb8003000UL;
			*(volatile uint32_t *)(base + 0x048UL) = 4;	/* 4-byte short packet */
			/* low-power (LP) mode for DCS commands */
			*(volatile uint32_t *)(base + 0x038UL) |= (1u << 26);
			__asm__ volatile("dsb sy" ::: "memory");
			/* DMA_TRIGGER = TRIGGER_SW (4) */
			*(volatile uint32_t *)(base + 0x080UL) =
				(*(volatile uint32_t *)(base + 0x080UL) & ~0x7u) | 4u;
			__asm__ volatile("dsb sy" ::: "memory");

			/* give the panel time to process */
			uint64_t f = rsr("cntfrq_el0");
			if (!f) f = 19200000;
			uint64_t end = now() + f / 4;	/* 250 ms */
			while (now() < end)
				;

			uint32_t err = *(volatile uint32_t *)(base + 0x064UL);
			hold = (err == 0) ? 60000u : 30000u;	/* 60 s so the panel can be seen */
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DSICFG)
	{
		/* Is the DSI command channel already configured by the bootloader?
		 *   20 s = at least one of CMD_CFG0/CMD_CFG1/CMD_DMA_CTRL/DMA_BASE non-zero
		 *   30 s = all zero (we must program it from scratch)
		 *   40 s = disp bring-up failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		if (disp_bring_up(&hw) == 0) {
			uint32_t cfg0 = rd32(0x0ae94000UL + 0x03cUL);
			uint32_t cfg1 = rd32(0x0ae94000UL + 0x040UL);
			uint32_t dma  = rd32(0x0ae94000UL + 0x044UL);
			uint32_t ctrl = rd32(0x0ae94000UL + 0x038UL);
			int any = 0;
			if (cfg0 && cfg0 != 0xffffffffu) any = 1;
			if (cfg1 && cfg1 != 0xffffffffu) any = 1;
			if (dma  && dma  != 0xffffffffu) any = 1;
			if (ctrl && ctrl != 0xffffffffu) any = 1;
			hold = any ? 20u : 30u;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DSICTL)
	{
		/* DSI controller state:
		 *   20 s = CLK_STATUS bit0 set (DSI core clock on, ABL left it)
		 *   30 s = controller readable but clock off
		 *   40 s = disp bring-up failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		if (disp_bring_up(&hw) == 0) {
			uint32_t ctrl = rd32(0x0ae94000UL + 0x000UL);
			uint32_t clks = rd32(0x0ae94000UL + 0x11CUL);
			(void)ctrl;
			hold = (clks & 1u) ? 20u : 30u;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PLLCHK)
	{
		/*   20 s = DSI0 PLL CNTRL bit0 set -> PLL already running (ABL left it)
		 *   30 s = PLL not running (we must program it)
		 *   40 s = disp bring-up failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		if (disp_bring_up(&hw) == 0) {
			uint32_t pllc = rd32(0x0ae94400UL + 0x03cUL);
			hold = (pllc & 1u) ? 20u : 30u;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PHYCHK)
	{
		/* Is the DSI PHY reachable once the display clocks are on?
		 *   20 s = PHY REVISION_ID0 non-zero (block alive)
		 *   30 s = readable but zero
		 *   40 s = disp bring-up failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		if (disp_bring_up(&hw) == 0) {
			uint32_t rev = rd32(0x0ae94400UL + 0x000UL);
			uint32_t cfg0 = rd32(0x0ae94400UL + 0x010UL);
			uint32_t pllc = rd32(0x0ae94400UL + 0x03cUL);
			int alive = (rev != 0 && rev != 0xffffffffu) ||
				    (cfg0 != 0 && cfg0 != 0xffffffffu) ||
				    (pllc != 0 && pllc != 0xffffffffu);
			hold = alive ? 20u : 30u;
		}
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DISP4)
	{
		/*   20 s = display clocks on AND MDSS HW_VERSION sane  ✅
		 *   30 s = clocks on but HW_VERSION reads 0
		 *   40 s = gcc disp clocks failed */
		unsigned hold = 40;
		uint32_t hw = 0;
		if (disp_bring_up(&hw) == 0)
			hold = (hw != 0 && hw != 0xffffffffu) ? 20u : 30u;
		reg_restore_all();
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DISP3)
	{
		/* After GCC disp clocks + disp_cc GDSC:
		 *   20 s = disp_cc readable AND MDSS HW_VERSION sane  ✅✅
		 *   30 s = disp_cc readable, MDSS not
		 *   40 s = disp_cc still zero
		 *   50 s = GCC clocks failed */
		unsigned hold = 50;
		if (gcc_disp_bring_up() == 0) {
			volatile uint32_t *g = (volatile uint32_t *)(0x0af00000UL + 0x9000UL);
			uint32_t v = *g;
			v &= ~1u;
			v |= 0xfu << 20; v |= 0xfu << 16; v |= 0xfu << 12;
			v &= ~(1u << 4);
			*g = v;
			__asm__ volatile("dsb sy" ::: "memory");
			uint64_t f = rsr("cntfrq_el0");
			if (!f) f = 19200000;
			uint64_t end = now() + f / 10;
			while (now() < end && !(*g & (1u << 31)))
				;
			/* disp_cc accessibility: GDSCR is the one register we know changes */
			uint32_t cc = rd32(0x0af00000UL + 0x9000UL);
			int cc_ok = (cc != 0 && cc != 0xffffffffu);
			uint32_t mdp = rd32(0x0ae00000UL);
			int mdp_ok = (mdp != 0 && mdp != 0xffffffffu);
			if (cc_ok && mdp_ok)
				hold = 20;
			else if (cc_ok)
				hold = 30;
			else
				hold = 40;
		}
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DISPEN)
	{
		/* Power the disp_cc domain and report the result:
		 *   20 s = disp_cc GDSC PWR_ON after our write  ✅
		 *   30 s = still off (rail / vdd_mm missing)
		 *   40 s = GCC disp clocks failed to latch */
		unsigned hold = 40;
		if (gcc_disp_bring_up() == 0) {
			volatile uint32_t *g = (volatile uint32_t *)(0x0af00000UL + 0x9000UL);
			uint32_t v = *g;
			v &= ~1u;			/* SW_COLLAPSE = 0 */
			v |= 0xfu << 20;
			v |= 0xfu << 16;
			v |= 0xfu << 12;
			v &= ~(1u << 4);
			*g = v;
			__asm__ volatile("dsb sy" ::: "memory");
			uint64_t f = rsr("cntfrq_el0");
			if (!f)
				f = 19200000;
			uint64_t end = now() + f / 10;
			while (now() < end && !(*g & (1u << 31)))
				;
			hold = (*g & (1u << 31)) ? 20u : 30u;
		}
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DISPCHK)
	{
		/* Does disp_cc become readable after enabling the GCC disp clocks?
		 *   20 s = disp_cc register reads non-zero (readable)  ✅
		 *   30 s = readable but zero
		 *   40 s = gcc_disp_bring_up failed (clock did not latch)
		 */
		unsigned hold = 40;
		if (gcc_disp_bring_up() == 0) {
			uint32_t v = rd32(0x0af00000UL + 0x8324UL);	/* AHB cmd_rcgr */
			hold = (v != 0 && v != 0xffffffffu) ? 20u : 30u;
		}
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DBSCAN)
	{
		/* Layout-independent scan of the cmd_db blob for ASCII "vdd_".
		 *   hold = 10 + 3*count  (count capped at 15) -> how many names found
		 *   hold = 60            -> zero occurrences */
		const volatile uint8_t *b = (const volatile uint8_t *)0x80860000UL;
		int count = 0;
		/* count printable runs of >= 4 chars that start with a letter */
		for (unsigned i = 0; i + 4 < 0x20000u && count < 15; i++) {
			uint8_t c0 = b[i];
			if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')))
				continue;
			int run = 0;
			for (unsigned j = 0; j < 4; j++) {
				uint8_t c = b[i + j];
				if (c >= 0x20 && c < 0x7f)
					run++;
				else
					break;
			}
			if (run == 4) {
				count++;
				i += 3;
			}
		}
		unsigned hold = count ? (unsigned)(10 + 3 * count) : 60u;
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DBMAGIC)
	{
		/* hold 20 = cmd_db magic present at 0x80860000
		 * hold 40 = magic missing (wrong base)
		 * hold 50 = readable RAM at all? (first word non-zero)  */
		const volatile uint8_t *b = (const volatile uint8_t *)0x80860000UL;
		unsigned hold;
		if (b[4] == 0xdb && b[5] == 0x30 && b[6] == 0x03 && b[7] == 0x0c)
			hold = 20;
		else if (b[0] != 0 || b[1] != 0 || b[4] != 0)
			hold = 40;
		else
			hold = 50;
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_CMDDB)
	{
		/* hold = 20 + 5*(addr & 0xf) when "vdd_mm" is found
		 * hold = 60 -> cmd_db missing / not found */
		unsigned hold = 60;
		uint32_t addr = 0;
		uint16_t slv = 0;
		if (cmd_db_entry("vdd_mm", &addr, &slv) == 0)
			hold = 20u + 5u * (addr & 0xfu);
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_QDESC)
	{
		/* Read-only: pull the Unit Descriptor of LUN 0 (no change to the device).
		 *   hold 20 = query ok, bLUEnable = 1
		 *   hold 30 = query ok, bLUEnable = 0  (device not configured)
		 *   hold 40 = query failed (rc 1..3 -> +10*rc: 50/60/70)
		 *   hold 80 = UFS init/negotiate failed */
		unsigned hold = 80;
		if (ufs_take_over() == 0) {
			uint32_t gear = 0;
			if (ufs_negotiate(&gear) == 0) {
				const uint8_t *b = ufs_bounce();
				int rc = ufs_read_unit_desc(0, (uint8_t *)b, 64);
				if (rc == 0)
					hold = (b[3] & 1u) ? 20 : 30;
				else
					hold = (unsigned)(40 + 10 * rc);
			}
		}
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_TUR1)
	{
		/* 20 s = TUR ok (ready)        30 s = rc=0 but SCSI status != 0
		 * 40 s = rc != 0 (transport)   50 s = init failed
		 * 60 s = negotiate failed */
		unsigned hold = 60;
		if (ufs_take_over() == 0) {
			uint32_t gear = 0;
			if (ufs_negotiate(&gear))
				hold = 70;
			else {
				uint32_t st = 0xffff;
				int rc = ufs_tur_once(0, &st);
				if (rc == 0 && st == 0)
					hold = 5;			/* ready */
				else if (rc == 0)
					hold = 10;			/* status error */
				else
					hold = (unsigned)(10 + 10 * rc);	/* rc 1..4 -> 20/30/40/50 */
			}
		}
		gcc_restore();
		report_at(hold * 1000u);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_HSREAD)
	{
		/* 20 s  = negotiated HS + unit ready + READ CAPACITY ok
		 * 40 s  = HS ok, unit ready, but CAP failed
		 * 60 s  = HS ok but the unit never became ready
		 * 80 s  = gear negotiation failed
		 * 100 s = UFS init failed */
		unsigned hold = 100000;
		if (ufs_take_over() == 0) {
			uint32_t gear = 0;
			if (ufs_negotiate(&gear))
				hold = 80000;
			else if (ufs_wait_ready())
				hold = 60000;
			else {
				uint32_t last = 0, blk = 0;
				hold = (ufs_readcap(&last, &blk) == 0) ? 20000 : 40000;
			}
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_DME)
	{
		/* Read PA_MaxRxHSGear through UIC:
		 *   hold = 20 + 5*gear -> got the peer's max gear (0..4 expected)
		 *   hold = 60          -> DME_GET failed
		 *   hold = 100         -> UFS init failed */
		unsigned hold = 100000;
		if (ufs_take_over() == 0) {
			uint32_t gear = 0xff;
			if (ufs_dme_get(0x0083, &gear) == 0)
				hold = 20000 + 5000 * (unsigned)(gear & 0x7);
			else
				hold = 60000;
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_WAKE)
	{
		/* After START STOP UNIT on every LU:
		 *   hold = 20 + 10*lun -> that LU is ready
		 *   hold = 100         -> still none ready
		 *   hold = 130         -> UFS init failed */
		unsigned hold = 130000;
		if (ufs_take_over() == 0) {
			int lun = ufs_wake_lun();
			hold = (lun < 0) ? 100000 : (unsigned)(20000 + 10000 * lun);
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_LUNSCAN)
	{
		/* hold = 20 + 10*lun  -> that LUN is ready
		 * hold = 100          -> no LUN ever became ready
		 * hold = 130          -> UFS init failed */
		unsigned hold = 130000;
		if (ufs_take_over() == 0) {
			int lun = ufs_find_ready_lun();
			hold = (lun < 0) ? 100000 : (unsigned)(20000 + 10000 * lun);
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_CAP2)
	{
		/* Self-calibrated marks:
		 *   2 s  = UFS init OK + TEST UNIT READY OK + READ CAPACITY OK
		 *   10 s = init OK, unit never became ready
		 *   20 s = ready, but READ CAPACITY failed
		 *   70 s = UFS init failed */
		unsigned hold = 110000;	/* init failed */
		if (ufs_take_over() == 0) {
			if (ufs_wait_ready())
				hold = 60000;	/* never ready */
			else {
				uint32_t last = 0, blk = 0;
				hold = (ufs_readcap(&last, &blk) == 0) ? 40000 : 80000;
			}
		}
		gcc_restore();
		report_at(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_REASON)
	{
		/* Does the IMEM restart reason work with the fixed harness?
		 * K60_REASON=1 -> ask for fastboot (expect FASTBOOT)
		 * K60_REASON=0 -> ask for a normal boot (expect ANDROID) */
		wr32(IMEM_RESTART_REASON, K60_REASON ? RESTART_BOOTLOADER : 0u);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(300);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_RD1)
	{
		/* hold 2 s  = a single READ(10) of LBA 1 succeeded
		 * hold 40 s = it failed
		 * hold 70 s = UFS init failed */
		unsigned hold = 70000;
		if (ufs_take_over() == 0) {
			int wrc = ufs_wait_ready();
			if (wrc)
				hold = 25000;			/* never became ready */
			else
				hold = (ufs_read1(1) == 0) ? 2000 : 40000;
		}
		gcc_restore();
		delay_ms(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_GPT2)
	{
		/* hold 2 s  = GPT read OK and "opcust" found in it
		 * hold 6 s  = read OK but no EFI PART signature
		 * hold 10 s = read failed
		 * hold 60 s = UFS init failed */
		unsigned hold = 70000;		/* init failed */
		int rc = ufs_take_over();
		if (rc == 0) {
			if (ufs_read_lowblk(1)) {
				hold = 40000;	/* read failed */
			} else {
				const uint8_t *b = ufs_bounce();
				static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
				int ok = 1;
				for (int i = 0; i < 8; i++)
					if (b[i] != (uint8_t)sig[i])
						ok = 0;
				if (!ok) {
					hold = 20000;	/* read ok, signature wrong */
				} else {
					uint32_t first = 0;
					hold = (ufs_part_first("opcust", &first) == 0 && first) ? 2000 : 55000;
				}
			}
		}
		gcc_restore();
		delay_ms(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_SENSE)
	{
		/* Report the SCSI sense key of a failed READ(10):
		 *   code 0 = the read actually succeeded
		 *   code 1..15 = sense key from the response UPIU (6 = unit attention,
		 *                5 = illegal request, 2 = not ready, 11 = aborted)
		 * hold = 2 s + 3 s * code */
		int code = ufs_take_over();
		unsigned hold;
		if (code != 0) {
			hold = 70000;			/* init failed: fixed 70 s marker */
		} else {
			(void)ufs_wait_ready();
			(void)ufs_read_lowblk(1);
			if (ufs_scsi_rc == 0)
				code = 0;		/* read succeeded (2 s) */
			else
				code = ufs_sense[2] & 0x0f;	/* sense key 1..15 */
			hold = 2000u + (unsigned)(code > 13 ? 13 : code) * 4000u;	/* 2 s + 4 s*key */
		}
		gcc_restore();
		delay_ms(hold);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSCAP)
	{
		/* code 0   = READ CAPACITY ok and block size 4096
		 * code 1   = ok but block size 512
		 * code 2   = ok but some other block size
		 * code 9..15 = failed at scsi() stage */
		int code = ufs_take_over();
		if (code == 0) {
			uint32_t last = 0, blk = 0;
			int rc = ufs_readcap(&last, &blk);
			if (rc) {
				code = 8 + (rc & 7);
			} else if (blk == 4096) {
				code = 0;
			} else if (blk == 512) {
				code = 1;
			} else {
				code = 2;
			}
		}
		delay_ms(2000 + (unsigned)(code > 20 ? 20 : code) * 3000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSREAD)
	{
		/* Where does the storage path break?
		 *   code 2  = GPT entry lookup failed (as before)
		 *   code 9..15 = READ(10) failed at scsi() stage (9=timeout,10=OCS,11=bad resp,12=SCSI status)
		 *   code 5  = read ok but no "EFI PART" signature
		 *   code 0  = read ok and signature present (hold 2 s)
		 * hold = 2 s + 3 s * code */
		int code = ufs_take_over();
		if (code == 0) {
			int rrc = ufs_read_lowblk(1);
			if (rrc) {
				code = 8 + (ufs_scsi_rc & 7);	/* 9..15 = which stage failed */
			} else {
				const uint8_t *b = ufs_bounce();
				static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
				int ok = 1;
				for (int i = 0; i < 8; i++)
					if (b[i] != (uint8_t)sig[i])
						ok = 0;
				code = ok ? 0 : 5;
			}
		}
		delay_ms(2000 + (unsigned)(code > 20 ? 20 : code) * 3000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_UFSGPT)
	{
		/* Full storage path: power+init the host, find "opcust" in the GPT.
		 *   code 0  = found (hold 2 s)
		 *   code 2  = GPT/lookup failed
		 *   code 11..18 = GCC power-up failed at that step
		 * hold = 2 s + 3 s * code */
		uint32_t first = 0;
		int code = ufs_part_first("opcust", &first);
		if (code == 0 && !first)
			code = 3;
		delay_ms(2000 + (unsigned)(code > 20 ? 20 : code) * 3000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_TIMING)
	{
		/* Multi-bit result in the hold time: hold = 2 s + 3 s * code
		 *   bit0 UFS HCS register readable
		 *   bit1 UFS PHY GDSC powered after gcc_ufs_bring_up()
		 *   bit2 GICD_TYPER readable (control: always-live block)
		 *   bit3 always 1 (so "no hold at all" is distinguishable) */
		int code = 8;
		uint32_t hcs = ufs_reg_read(0x30);
		if (hcs != 0 && hcs != 0xffffffffu)
			code |= 1;
		(void)gcc_ufs_bring_up();
		if (gcc_ufs_gdsc_on())
			code |= 2;
		uint32_t gd = rd32(0x17100004UL);
		if (gd != 0 && gd != 0xffffffffu)
			code |= 4;
		delay_ms(2000 + (unsigned)code * 3000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_CLEARREASON)
	{
		/* Clear a sticky "bootloader" restart reason and reset into Android. */
		wr32(IMEM_RESTART_REASON, 0);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(500);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_OFF)
	{
		/* Unmistakable liveness test: if we execute, power the phone off and
		 * leave it off. A device that comes back to Android ~40 s later never
		 * ran this code. */
		delay_ms(2000);
		smc(0x84000008UL, 0, 0, 0);	/* PSCI_SYSTEM_OFF */
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_SLEEP)
	{
		/* Is a plain hold even possible this early? No MMIO at all. */
		delay_ms(60000);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_CTL)
	{
		/* Control probe: touch only the UFS register block (known readable by
		 * our own driver) and report code 8|ok so the hold is 33 s. If this
		 * lands ~74 s after hand-off but disp_cc probes land at ~41 s, then
		 * reading the unpowered display block really does stall the bus. */
		uint32_t cap = rd32(0x01d84000UL + 0x00);
		report_code(8 | (cap ? 1 : 0));
	}
#endif

#if defined(K60_DISP2)
	{
		/* Stage 2: power the MDSS domain, enable the display AHB clock branch
		 * (its source is already BI_TCXO, so no PLL work is needed) and then
		 * prove that MDSS registers are actually readable.
		 * Reported code bits: [0] AHB branch reads back enabled
		 *                     [1] MDSS HW_VERSION looks sane
		 *                     [2] GDSC PWR_ON */
		uint32_t gdscr = rd32(DISP_CC_BASE + GDSCR_MDSS);
		int pwr = (int)((gdscr >> 31) & 1);
		if (!pwr) {
			uint32_t v = gdscr & ~1u;
			v |= 0xfu << 20;
			v |= 0xfu << 16;
			v |= 0xfu << 12;
			v &= ~(1u << 4);
			wr32(DISP_CC_BASE + GDSCR_MDSS, v);
			__asm__ volatile("dsb sy" ::: "memory");
			uint64_t end = now() + rsr("cntfrq_el0") / 10;
			while (now() < end && !(rd32(DISP_CC_BASE + GDSCR_MDSS) & (1u << 31)))
				;
			pwr = (int)((rd32(DISP_CC_BASE + GDSCR_MDSS) >> 31) & 1);
		}
		uint32_t b = rd32(DISP_CC_BASE + DISP_CC_AHB_BRANCH);
		wr32(DISP_CC_BASE + DISP_CC_AHB_BRANCH, b | 1u);
		__asm__ volatile("dsb sy" ::: "memory");
		int ahb_ok = 0;
		uint64_t end2 = now() + rsr("cntfrq_el0") / 10;
		while (now() < end2) {
			if (rd32(DISP_CC_BASE + DISP_CC_AHB_BRANCH) & 1u) {
				ahb_ok = 1;
				break;
			}
		}
		uint32_t hw = 0;
		int mdss_ok = 0;
		if (ahb_ok) {
			hw = rd32(MDSS_BASE);
			mdss_ok = (hw != 0 && hw != 0xffffffffu);
		}
		report_code(ahb_ok | (mdss_ok << 1) | (pwr << 2));
	}
#endif

#if defined(K60_GDSCR)
	{
		/* Report the four interesting GDSCR bits verbatim so we can tell
		 * "software can power it up" from "hardware owns the domain".
		 *   [0] PWR_ON  [1] SW_COLLAPSE  [2] HW_CONTROL  [3] SW_OVERRIDE */
		uint32_t v = rd32(DISP_CC_BASE + GDSCR_MDSS);
		report_code((int)((v >> 31) & 1) | (int)((v & 1u) << 1) |
			    (int)(((v >> 3) & 1) << 2) | (int)(((v >> 4) & 1) << 3));
	}
#endif

#if defined(K60_MMIO)
	{
		/* Sanity-check the MMIO path in one boot:
		 *   [3] always 1 (so the code is never ambiguous with the 1 s default)
		 *   [0] UFS CAP register is non-zero   (known-good block, driver works)
		 *   [1] disp_cc register reads non-zero (is the display clock block alive?)
		 *   [2] GDSCR PWR_ON */
		uint32_t cap = rd32(0x01d84000UL + 0x00);	/* UFSHCI CAP */
		uint32_t dc = rd32(DISP_CC_BASE + DISP_CC_AHB_CMD_RCGR);
		uint32_t g = rd32(DISP_CC_BASE + GDSCR_MDSS);
		int code = 8;
		if (cap) code |= 1;
		if (dc) code |= 2;
		if (g & (1u << 31)) code |= 4;
		klog_hex("ufs_cap", (uint64_t)cap);
		klog_hex("dispcc_rcgr", (uint64_t)dc);
		klog_hex("gdscr", (uint64_t)g);
		report_code(code);
	}
#endif

#if defined(K60_CAL)
	report_code(0);
#endif

#if defined(K60_CLKREAD)
	{
		/* Read what the bootloader left in the display clock controller:
		 *   bits[2:0] = ahb clk src selection (0 = BI_TCXO, always on)
		 *   bit[3]    = ahb branch enable bit
		 * All of disp_cc is in the always-on domain, so this is safe. */
		uint32_t src = rd32(DISP_CC_BASE + DISP_CC_AHB_CMD_RCGR);
		uint32_t br = rd32(DISP_CC_BASE + DISP_CC_AHB_BRANCH);
		int code = (int)(src & 0x7) | ((int)(br & 1u) << 3);
		report_code(code);
	}
#endif

#if defined(K60_DISP0) || defined(K60_DISP1)
	{
		/* ---- display bring-up stage 0/1: MDSS power domain ----
		 * disp_cc lives in the always-on domain, so reading its GDSCR is safe;
		 * touching MDSS itself before the domain is up can hang the bus.
		 * GDSCR bits (Linux drivers/clk/qcom/gdsc.c):
		 *   [31] PWR_ON, [23:20] EN_REST_WAIT, [19:16] EN_FEW_WAIT,
		 *   [15:12] CLK_DIS_WAIT, [4] SW_OVERRIDE, [0] SW_COLLAPSE(1 = off)
		 * Binary channel: success -> reason 0 -> Android, failure -> fastboot. */
		uint32_t gdscr = rd32(DISP_CC_BASE + GDSCR_MDSS);
		int pwr = (int)((gdscr >> 31) & 1);
		int ok = pwr;

#ifdef K60_DISP1
		if (!pwr) {
			uint32_t v = gdscr;
			v &= ~1u;                  /* SW_COLLAPSE = 0 -> power up */
			v |= 0xfu << 20;           /* EN_REST_WAIT  */
			v |= 0xfu << 16;           /* EN_FEW_WAIT   */
			v |= 0xfu << 12;           /* CLK_DIS_WAIT  */
			v &= ~(1u << 4);           /* SW_OVERRIDE = 0 */
			wr32(DISP_CC_BASE + GDSCR_MDSS, v);
			__asm__ volatile("dsb sy" ::: "memory");
			uint64_t end = now() + rsr("cntfrq_el0") / 10;	/* 100 ms */
			while (now() < end) {
				if (rd32(DISP_CC_BASE + GDSCR_MDSS) & (1u << 31)) {
					pwr = 1;
					break;
				}
			}
			ok = pwr;
		}
#endif
		(void)gdscr;
		wr32(IMEM_RESTART_REASON, ok ? 0u : RESTART_BOOTLOADER);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(1500);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PERSIST_W)
	{
		/* Run 1 of the persistence test: write a marker well inside opcust,
		 * flush, then reset to fastboot (no Android involvement). */
		static uint8_t pb[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int prc = ufs_part_first("opcust", &first);
		int wrc = -1;
		for (unsigned b = 0; b < sizeof(pb); b++)
			pb[b] = 0;
		static const char m[] = "K60PERSIST-MARKER";
		for (int i = 0; m[i]; i++)
			pb[i] = (uint8_t)m[i];
		if (!prc && first)
			wrc = ufs_probe_write(first + 16, pb, 0x20, 1);
		wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);	/* stay in fastboot: no Android in between */
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2000);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_PERSIST_R)
	{
		/* Run 2: read the marker back after the reset, and also re-read block 0
		 * (the string Android wrote earlier) to prove shim and Android address
		 * the same bytes.
		 *   marker survives        -> reason 0            -> Android
		 *   marker gone            -> reason "bootloader" -> fastboot */
		static uint8_t rb[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int prc = ufs_part_first("opcust", &first);
		int rrc = -1, match = 0;
		if (!prc && first) {
			rrc = ufs_probe_read(first + 16, rb);
			if (!rrc) {
				static const char m[] = "K60PERSIST-MARKER";
				match = 1;
				for (int i = 0; m[i]; i++)
					if (rb[i] != (uint8_t)m[i])
						match = 0;
			}
		}
		wr32(IMEM_RESTART_REASON, (rrc == 0 && match) ? 0u : RESTART_BOOTLOADER);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2000);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_WRITEBACK)
	{
		/* Write, flush, read back in the same run. Binary channel:
		 *   data visible to the device  -> reason 0            -> Android
		 *   not visible                 -> reason "bootloader" -> fastboot */
		static uint8_t wb[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int prc = ufs_part_first("opcust", &first);
		int wrc = -1, brc = -1, match = 0;

		for (unsigned b = 0; b < sizeof(wb); b++)
			wb[b] = 0;
		static const char m[] = "K60WRITEBACK";
		for (int i = 0; m[i]; i++)
			wb[i] = (uint8_t)m[i];
		if (!prc && first) {
			wrc = ufs_probe_write(first, wb, 0x20, 1);	/* includes sync */
			brc = ufs_probe_read(first, wb);
			if (!brc) {
				match = 1;
				for (int i = 0; m[i]; i++)
					if (wb[i] != (uint8_t)m[i])
						match = 0;
			}
		}
		int ok = (prc == 0 && first != 0 && wrc == 0 && brc == 0 && match);
		wr32(IMEM_RESTART_REASON, ok ? 0u : RESTART_BOOTLOADER);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2000);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_WPROBE)
	{
		/* Binary channel: does a WRITE(10) come back clean?
		 *   rc == 0  -> restart reason 0            -> Android boots
		 *   rc != 0  -> restart reason "bootloader" -> lands in fastboot
		 * Unambiguous from the host, no timing needed. */
		static uint8_t wbuf[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int prc = ufs_part_first("opcust", &first);
		int wrc = -1;

		for (unsigned b = 0; b < sizeof(wbuf); b++)
			wbuf[b] = 0;
		static const char m[] = "K60WPROBE";
		for (int i = 0; m[i]; i++)
			wbuf[i] = (uint8_t)m[i];
		if (!prc && first)
			wrc = ufs_probe_write(first + 8, wbuf, 0x20, 1);

		int ok = (prc == 0 && first != 0 && wrc == 0);
		wr32(IMEM_RESTART_REASON, ok ? 0u : RESTART_BOOTLOADER);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2000);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_HOLD)
	/* Panel-state probe: touch nothing, just sit here. If the screen keeps
	 * showing whatever ABL drew, the panel is still scanning out; if it goes
	 * black, ABL shut the display down before the hand-off. */
	delay_ms(60000);
	wr32(IMEM_RESTART_REASON, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
#endif
#if defined(K60_FAST)
	{
		/* ===== fast bring-up loop =====
		 * Runs the experiment, encodes the result in the hold time, then
		 * resets straight back into fastboot (restart reason "bootloader"),
		 * so the host iterates in ~10 s instead of booting Android.
		 *   hold = 5 + 5*code seconds; code = 5 means "everything worked". */
		static uint8_t fbuf[4096] __attribute__((aligned(4096)));
		uint32_t first = 0;
		int code = 15;

		int prc = ufs_part_first("opcust", &first);
		if (prc || !first) {
			code = 10 + prc;		/* 11..14: GPT lookup failed */
		} else {
			/* 1. read the host-planted probe at the start of the partition */
			int rrc = ufs_probe_read(first, fbuf);
			int rmatch = 0;
			if (!rrc) {
				static const char want[] = "HOSTPROBE";
				rmatch = 1;
				for (int i = 0; want[i]; i++)
					if (fbuf[i] != (uint8_t)want[i])
						rmatch = 0;
			}
			/* 2. write our own record one block later, then read it back */
			static const char mark[] = "K60FAST WRITE OK";
			for (unsigned b = 0; b < sizeof(fbuf); b++)
				fbuf[b] = 0;
			for (int i = 0; mark[i]; i++)
				fbuf[i] = (uint8_t)mark[i];
			int wrc = ufs_probe_write(first + 1, fbuf, 0x20, 1);
			int bmatch = 0, brc = -1;
			if (!wrc) {
				static uint8_t vbuf[4096] __attribute__((aligned(4096)));
				brc = ufs_probe_read(first + 1, vbuf);
				if (!brc) {
					bmatch = 1;
					for (int i = 0; mark[i]; i++)
						if (vbuf[i] != (uint8_t)mark[i])
							bmatch = 0;
				}
			}
			if (!rrc && !rmatch)
				code = 9;			/* read OK but wrong data at that LBA */
			else if (rrc)
				code = 8;			/* read of the scratch LUN failed */
			else if (wrc)
				code = wrc;			/* 1 timeout, 2 OCS, 3 bad resp, 4 SCSI status */
			else
				code = bmatch ? 5 : 7;	/* 5 = write+readback OK, 7 = wrote but no data */
			(void)brc;
		}
		wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms((5u + 5u * (unsigned)code) * 1000u);
		gcc_restore();
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_SPMIPROBE)
	{
		/* Find PM8350's LDO block. ppid = (sid << 8) | periph; the map entry
		 * holds it at bits [19:8]. LDO5 is periph 0x44 (ppid 0x144), so look for
		 * every SID 1 peripheral in 0x40..0x5f and then read one of them. */
		const uint32_t CORE = 0x0c400000UL;
		const uint32_t OBS = 0x0c440000UL;
		uint32_t total = (rd32(CORE + 0x04) & 0x3ffu) + (rd32(CORE + 0x08) & 0x3ffu);
		int found = 0, hit_apid = -1, hit_ppid = -1;

		con_clear(0x00101420);
		con_banner("ios-sweet", "SPMI v7: PM8350 LDO block", 0xff5f87d7U, 0x00101420, 3);
		con_skip();

		for (uint32_t i = 0; i < total && i < 1024; i++) {
			uint32_t m = rd32(CORE + 0x900u + 4u * i);
			uint32_t ppid;

			if (!m) {
				continue;
			}
			ppid = (m >> 8) & 0xfffu;
			if ((ppid >> 8) == 1 && ((ppid & 0xff) >> 4) == 4) {
				if (found == 0) {
					hit_apid = (int)i;
					hit_ppid = (int)ppid;
					con_hex2("ldo ppid", "i=", i, "ppid", ppid);
				}
				found++;
			}
		}
		con_hex2("ldo count", "n=", (uint32_t)found, "hit apid", (uint32_t)hit_apid);

		if (hit_apid >= 0) {
			/* v7 observer offset: 0x8000*ee + 0x20*apid */
			uint32_t off = OBS + 0x20u * (uint32_t)hit_apid;
			uint32_t addr = (uint32_t)(hit_ppid & 0xff) + 0x46u;   /* LDO en reg */
			uint32_t cmd = (1u << 27) | ((addr & 0xffu) << 4);
			uint32_t st = 0, val = 0;

			wr32(off + 0x18, 0);
			wr32(off + 0x00, cmd);
			__asm__ volatile("dsb sy" ::: "memory");
			for (unsigned w = 0; w < 2000; w++) {
				st = rd32(off + 0x08);
				if (st & 1u) {
					val = rd32(off + 0x18);
					break;
				}
			}
			con_hex2("read status", "s=", st, "val", val & 0xffu);
		}
		report_at(35000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_RDCHECK)
	{
		/* Can the AP reach the upper DDR bank? The device tree lists
		 * <0x1_00000000 0xfcc00000> as memory, but loading the kernel there
		 * raised a synchronous external abort. Probe a few addresses read-only
		 * and report which respond. */
		const uint64_t addrs[] = { 0x100000000UL, 0x180000000UL, 0x1c0000000UL,
			0x80000000UL, 0xb4000000UL };
		char buf[40];

		con_clear(0x00101420);
		con_banner("ios-sweet", "DDR accessibility probe", 0xff5f87d7U, 0x00101420, 3);
		con_skip();
		for (unsigned i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
			fmt_hex_s(buf, rd32(addrs[i]));
			con_kv(i == 0 ? "0x100000000" : (i == 1 ? "0x180000000" :
			    (i == 2 ? "0x1c0000000" : (i == 3 ? "0x080000000" : "0x0b4000000"))),
			    buf, "read ok", 1);
		}
		con_str("NEXT", "hold 120 s");
		report_at(120000);
		smc(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
#endif

#if defined(K60_XNULOG)
	{
		/* Show the panic log the *previous* XNU boot left in /pram, then boot
		 * XNU again. Android never runs in between, so nothing can wipe it -
		 * this replaces the fragile two-boot panic-read dance. */
		const uint8_t *log = (const uint8_t *)0x0b9200000UL;
		char line[64];
		int col = 0, lines = 0, started = 0;
		uint32_t zero_run = 0;

		stage = "xnulog: previous panic log";
		con_clear(0x00101420);
		con_banner("ios-sweet", "last XNU panic log", 0xff5f87d7U, 0x00101420, 4);
		con_skip();
		/* The log buffer may start with a zeroed header, so skip leading
		 * zeros instead of treating them as "empty region". */
		uint32_t first = 0;

		while (first < 0x10000u && log[first] == 0)
			first++;
		for (uint32_t i = first; i < 0x100000u && lines < 180; i++) {
			uint8_t c = log[i];

			if (c == 0) {
				if (++zero_run > 32768)
					break;
				continue;
			}
			zero_run = 0;
			started = 1;
			if (c == '\n' || c == '\r') {
				if (col) {
					line[col] = 0;
					print_numbered(&lines, line);
					col = 0;
				}
				continue;
			}
			if (c < 0x20 || c > 0x7e) {
				if (col < (int)sizeof(line) - 2)
					line[col++] = '?';
			} else if (col < 66) {
				line[col++] = (char)c;
			} else {
				line[col] = 0;
				print_numbered(&lines, line);
				col = 0;
			}
		}
		if (col) {
			line[col] = 0;
			print_numbered(&lines, line);
		}
		if (!started)
			con_str("EMPTY", "previous boot left no text here");
		con_rule('-');
		con_hex2("END", "lines=", (uint32_t)lines, 0, 0);
		/* Nothing to read? Don't make the user sit through the hold: only an
		 * actual panic log is worth 2 minutes of the screen. */
		con_str("NEXT", lines ? "booting XNU in 3 s (photo now)"
				      : "empty log, booting XNU in 3 s");
		report_at(3000);
	}
#endif

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
#ifndef K60_NOFB
	con_clear(RED);
	con_banner("XTOM", "hackios bring-up / ios-sweet port / K60 (mondrian, SM8475)",
	    WHITE, RED, 18);
	con_str("SHIM", "sweet milestone 4: xnu loader");
#endif
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	con_skip();
	/* Port bring-up: hold the diagnostics on screen for a short, fixed time so
	 * the host can tell "shim ran" (>=45s before Android returns) from
	 * "ABL never jumped into us" (~25s). */
	con_str("UFS", "probe done, reset in ~45s [k60-bringup]");
	delay_ms(45000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
