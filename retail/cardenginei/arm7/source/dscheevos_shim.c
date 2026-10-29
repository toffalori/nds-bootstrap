#ifndef TWLSDK /* DScheevos runs for DS-mode games only */
/*
 * ARM7 SDIO shim for the DSi Atheros Wi-Fi chip.
 *
 * Self-contained on purpose (no dswifi7, no libnds calls): this is the code
 * that would go into nds-bootstrap's ARM7 card engine. It assumes a previous
 * program left the chip initialised (firmware running, HTC/WMI services
 * connected, associated, keys installed) and only moves HTC frames between
 * mailbox 0 and the rings in shim_shared_t.
 *
 * Uses CPU (PIO) access to the 32-bit data FIFO, no NDMA, no interrupts.
 */
/* size over speed: the card engine has 61 KB of WRAM in all */
#pragma GCC optimize ("Os")
#include <stdint.h>
#include "dscm_shim.h"
#include "shim.h"

typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
#define REG_VCOUNT_SHIM (*(vu16*)0x04000006)

#define SDIO_BASE 0x04004A00u
#define R16(o) (*(vu16*)(SDIO_BASE + (o)))
#define R32(o) (*(vu32*)(SDIO_BASE + (o)))

enum {
  CMD = 0x000, PORT_SEL = 0x002, ARG0 = 0x004, ARG1 = 0x006, STOP = 0x008,
  BLKCNT16 = 0x00A, RESP0 = 0x00C, STAT0 = 0x01C, STAT1 = 0x01E,
  MASK0 = 0x020, MASK1 = 0x022, CLKCNT = 0x024, BLKLEN16 = 0x026,
  CARD_OPT = 0x028, DATACTL = 0x0D8, IRQ32 = 0x100, BLKLEN32 = 0x104,
  BLKCNT32 = 0x108, FIFO32 = 0x10C,
};

#define STAT0_RESPEND  0x0001u
#define STAT0_DATAEND  0x0004u
#define STAT1_TXRQ     0x0200u
#define STAT1_RXRDY    0x0100u
#define STAT1_BUSY     0x4000u
#define STAT1_ERR      0x807Fu
#define IRQ32_RXFULL   0x0100u

/* TMIO command words (index | response type << 8 | data flags) */
#define TMIO_CMD52       0x0434u
#define TMIO_CMD53_READ  0x7C35u   /* data, read, multi-block */
#define TMIO_CMD53_WRITE 0x6C35u   /* data, write, multi-block */

#define F1_HOST_INT_STATUS 0x400u
#define F1_RX_LOOKAHEAD0   0x408u
#define MBOX0_END          0x4000u /* extended mbox0 window ends here; last byte = EOM */
#define HTC_EP_DATA        2u      /* best-effort data (WMI events come on 1) */

/* Bounded busy-waits. One loop pass is ~10-15 ARM7 cycles, so 3000 passes
 * is ~1-1.3 ms: far above a CMD52 (~10 us) or a 128-byte block (~17 us at
 * HCLK/2, 4-bit), far below a frame. Runs with IRQs off inside the game's
 * VBlank handler, so this bounds the worst case stall. */
#define POLL_SPINS 2500u   /* wait-loop passes for a whole poll (~1 ms), shared by every wait */
#define POLL_LINES 4u       /* no new RX frame once a poll has used this many scanlines */
#define BACKOFF_POLLS 60u   /* after an SDIO error: skip ~1 s of VBlanks */
#define DEAD_AFTER 5u       /* consecutive failed polls: the ARM9 sees dead=1 until a poll goes through */

static shim_shared_t* sh;
static uint32_t backoff, fails;
static uint32_t spins;      /* left in this poll; a stuck chip can't hold the game's IRQ longer */

static int spin(void)
{
  if (!spins)
    return 0;
  spins--;
  return 1;
}

static void fail(uint32_t step)
{
  sh->stats.sdio_errors++;
  sh->stats.last_error = (step << 16) | R16(STAT1);
  R16(STAT0) = 0;
  R16(STAT1) = 0;
}

static int wait_idle(void)
{
  while (spin())
    if (!(R16(STAT1) & STAT1_BUSY))
      return 0;
  return -1;
}

static void start(uint16_t cmd, uint32_t arg)
{
  R16(STAT0) = 0;
  R16(STAT1) = 0;
  R16(ARG0) = arg & 0xFFFF;
  R16(ARG1) = arg >> 16;
  R16(CMD) = cmd;
}

/* Wait until all bits of need0 are set in STAT0 or an error shows up. */
static int wait_stat0(uint16_t need0)
{
  while (spin()) {
    if (R16(STAT1) & STAT1_ERR)
      return -1;
    if ((R16(STAT0) & need0) == need0)
      return 0;
  }
  return -2;
}

/* CMD52 IO_RW_DIRECT. Returns the byte read (or written back), or -1. */
static int cmd52(uint32_t func, uint32_t addr, int write, uint8_t val)
{
  if (wait_idle()) { fail(1); return -1; }
  uint32_t arg = (func << 28) | ((addr & 0x1FFFF) << 9);
  if (write)
    arg |= 0x80000000u | val;
  start(TMIO_CMD52, arg);
  if (wait_stat0(STAT0_RESPEND)) { fail(2); return -1; }
  int r = R16(RESP0) & 0xFF;
  R16(STAT0) = 0;
  R16(STAT1) = 0;
  return r;
}

static void setup_blocks(uint32_t nblk, uint16_t irq32)
{
  R16(STOP) = 0x100;
  R16(BLKLEN16) = SHIM_BLOCK;
  R16(BLKCNT16) = nblk;
  R16(BLKLEN32) = SHIM_BLOCK;
  R16(BLKCNT32) = nblk;
  R16(DATACTL) = 0x0002;           /* 32-bit FIFO mode */
  R16(IRQ32) = irq32;              /* bit1: 32-bit mode, bit10: clear FIFO */
}

/* CMD53 IO_RW_EXTENDED, function 1, block mode, incrementing address.
 * dst 0: the blocks are read out and thrown away. */
static int cmd53_read(uint32_t addr, uint32_t* dst, uint32_t nblk)
{
  if (wait_idle()) { fail(3); return -1; }
  setup_blocks(nblk, 0x0C02);
  start(TMIO_CMD53_READ, (1u << 28) | (1u << 27) | (1u << 26) | ((addr & 0x1FFFF) << 9) | nblk);
  for (uint32_t b = 0; b < nblk; b++) {
    while (!(R16(IRQ32) & IRQ32_RXFULL)) {
      if ((R16(STAT1) & STAT1_ERR) || !spin()) { fail(4); return -1; }
    }
    for (uint32_t w = 0; w < SHIM_BLOCK / 4; w++) {
      uint32_t v = R32(FIFO32);
      if (dst)
        *dst++ = v;
    }
  }
  if (wait_stat0(STAT0_DATAEND)) { fail(5); return -1; }
  R16(STAT0) = 0;
  R16(STAT1) = 0;
  return 0;
}

static int cmd53_write(uint32_t addr, const uint32_t* src, uint32_t nblk)
{
  if (wait_idle()) { fail(6); return -1; }
  setup_blocks(nblk, 0x1402);
  start(TMIO_CMD53_WRITE, 0x80000000u | (1u << 28) | (1u << 27) | (1u << 26) | ((addr & 0x1FFFF) << 9) | nblk);
  for (uint32_t b = 0; b < nblk; b++) {
    while (!(R16(STAT1) & STAT1_TXRQ)) {
      if ((R16(STAT1) & STAT1_ERR) || !spin()) { fail(7); return -1; }
    }
    R16(STAT1) = (uint16_t)~STAT1_TXRQ;
    for (uint32_t w = 0; w < SHIM_BLOCK / 4; w++)
      R32(FIFO32) = *src++;
  }
  if (wait_stat0(STAT0_DATAEND)) { fail(8); return -1; }
  R16(STAT0) = 0;
  R16(STAT1) = 0;
  return 0;
}

/* Take over a controller another program configured: no soft reset (that
 * would not reset the card on hardware, but melonDS resets its chip model on
 * it), just the settings dswifi uses. The card keeps its own state (RCA,
 * 4-bit bus, func1 block size 128, func1 enabled). */
void shim_attach(shim_shared_t* shared)
{
  sh = shared;
  R16(PORT_SEL) = R16(PORT_SEL) & ~3u;          /* port 0: the Wi-Fi card */
  R16(CLKCNT) = (R16(CLKCNT) & ~0x2FFu) | 0x100; /* HCLK/2, clock on */
  R16(CARD_OPT) = R16(CARD_OPT) & ~0x8000u;     /* 4-bit bus */
  R32(MASK0) = 0xFFFFFFFFu;                     /* controller IRQs off: we poll */
  /* Drop whatever the previous owner left in the data FIFOs. On hardware a
   * controller soft reset (0x0E0 bit 0 = 0 then 1) does this without
   * touching the card; melonDS resets its whole chip model on that write,
   * so read the 16-bit FIFO dry instead (both halves, 2 x 512 bytes). */
  R16(BLKLEN16) = SHIM_BLOCK;
  R16(BLKCNT16) = 0;
  R16(IRQ32) = 0x0400;                          /* 16-bit mode, clear FIFO32 */
  R16(DATACTL) = 0;
  for (uint32_t i = 0; i < 512; i++)
    (void)R16(0x030);
  R16(STAT0) = 0;
  R16(STAT1) = 0;
  sh->attached = 1;
}

static uint32_t read_lookahead(void)
{
  uint32_t la = 0;
  for (uint32_t i = 0; i < 4; i++) {
    int b = cmd52(1, F1_RX_LOOKAHEAD0 + i, 0, 0);
    if (b < 0)
      return 0;
    la |= (uint32_t)b << (i * 8);
  }
  return la;
}

/* One pass, inside the game's VBlank IRQ: at most one TX frame, then up to
 * SHIM_RX_PER_POLL RX frames (one if a frame was sent), while the poll is
 * under POLL_LINES scanlines. All waits share one spin budget, and the first
 * error ends the poll. Returns a bitmask of what it did (1 = sent, 2 = received).
 *
 * RX never stays in the chip: the frames it holds for the host take the
 * buffers its radio receives into, and with those gone it misses the AP's
 * beacons and drops the link (firmware reason 2). So when the ARM9 falls
 * behind (the game gave it no idle time for a while) and the ring is full,
 * the frame is read out and thrown away. The ring's last free slot is kept
 * for WMI events and EAPOL: a plain data frame is thrown away there too.
 * UDP and TCP live with the loss; a lost connect event or key message would
 * stall a rejoin. */
uint32_t shim_poll(void)
{
  uint32_t did = 0, rx_max = SHIM_RX_PER_POLL;
  uint32_t line0 = REG_VCOUNT_SHIM;
  if (!sh || !sh->run)
    return 0;
  if (backoff) {
    backoff--;
    return 0;
  }
  uint32_t errors_before = sh->stats.sdio_errors;
  sh->stats.polls++;
  spins = POLL_SPINS;

  uint32_t tail = sh->tx_tail;
  if (tail != sh->tx_head) {
    shim_slot_tx_t* s = &sh->tx[tail % SHIM_TX_SLOTS];
    uint32_t nblk = (s->len + SHIM_BLOCK - 1) / SHIM_BLOCK;
    if (s->len && nblk * SHIM_BLOCK <= SHIM_TX_MAX
        && cmd53_write(MBOX0_END - nblk * SHIM_BLOCK, (const uint32_t*)s->data, nblk) == 0)
      sh->stats.tx_frames++;
    sh->tx_tail = tail + 1;       /* dropped on error: the ARM9 stack retransmits */
    did |= 1;
    rx_max = 1;
  }

  for (uint32_t k = 0; k < rx_max && sh->stats.sdio_errors == errors_before; k++) {
    uint32_t line = REG_VCOUNT_SHIM;
    if ((line + 263 - line0) % 263 >= POLL_LINES)
      break;
    int st = cmd52(1, F1_HOST_INT_STATUS, 0, 0);
    if (st < 0)
      break;
    sh->stats.last_int_status = st;
    if (!(st & 1))                 /* mailbox 0 empty */
      break;
    uint32_t head = sh->rx_head;
    uint32_t used = head - sh->rx_tail;
    uint32_t la = read_lookahead();
    uint32_t len = la >> 16;
    uint32_t nblk = (len + 6 + SHIM_BLOCK - 1) / SHIM_BLOCK;
    if (!la || len == 0 || nblk * SHIM_BLOCK > SHIM_RX_MAX) {
      fail(9);
      break;
    }
    shim_slot_rx_t* s = &sh->rx[head % SHIM_RX_SLOTS];
    uint32_t* dst = used < SHIM_RX_SLOTS ? (uint32_t*)s->data : 0;
    if (cmd53_read(MBOX0_END - nblk * SHIM_BLOCK, dst, nblk))
      break;
    did |= 2;
    /* the HTC header's endpoint (lookahead byte 0); the ethertype after the
     * 6-byte HTC header, the 2-byte WMI data header, 802.3 and LLC/SNAP */
    if (!dst || (used == SHIM_RX_SLOTS - 1 && (la & 0xFF) == HTC_EP_DATA
                 && !(s->data[28] == 0x88 && s->data[29] == 0x8E))) {
      sh->stats.rx_ring_full++;
      continue;
    }
    s->len = nblk * SHIM_BLOCK;
    s->lookahead = la;
    sh->rx_head = head + 1;
    sh->stats.rx_frames++;
  }

  /* dead is not for good: a chip that answers again (after a firmware
   * hiccup, say) gets its link back, at one try a second meanwhile */
  if (sh->stats.sdio_errors != errors_before) {
    backoff = BACKOFF_POLLS;
    if (++fails >= DEAD_AFTER)
      sh->dead = 1;
  } else {
    fails = 0;
    sh->dead = 0;
  }
  return did;
}
#endif /* TWLSDK */
