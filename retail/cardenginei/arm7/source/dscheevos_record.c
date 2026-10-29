#ifndef TWLSDK /* DScheevos runs for DS-mode games only */
/*
 * Cheevotron: the streamer's recording, mirrored onto the SD card.
 *
 * The module records the watched words into a ring in RAM (dscm_rec_t,
 * pointed to by its header; dscm_record.h). Every VBlank this copies at most
 * one sector of it into sd:/_nds/dscheevos/recording.bin, into the slot the
 * bootloader claimed for the run (dscheevos_hooks.c): the next completed
 * stream sector to its data sector, or, now and then, the slot's header
 * sector (how far the copy goes, the run, the game). The file's clusters are
 * fixed, so no FAT or directory sector is touched while the game runs, and a
 * header is written only after the data it describes: a power-off costs at
 * most the seconds since the last header.
 *
 * A write is started inside the game's VBlank handler and left to the card:
 * the command and the 512 bytes go to the controller (tens of microseconds,
 * measured in scanlines into the stats), the card's programming time
 * (milliseconds, more on a slow card) passes with the game running, and a
 * later VBlank sees the end. A sector counts as written only when the
 * controller reported the data's end. The SD controller is shared with the
 * card engine's ROM reads (none from a real cartridge) and the game's saves
 * (saveMutex, on the game's ARM7 thread): a VBlank with either in progress
 * is skipped, and a write whose end another command cleared away (they wait
 * for the card to finish first, so the data is safe) is written again.
 */
#include <nds/ndstypes.h>
#include "my_sdmmc.h"
#include "tonccpy.h"
#include "dscheevos_hooks.h"
#include "dscm_record.h"
#include "dscheevos_record.h"

extern struct mmcdevice handleSD;   // my_sdmmc.c
extern int tryLockMutex(int* addr);
extern int unlockMutex(int* addr);
// card_engine_header.s, set by the bootloader
extern u32 dscheevosRecordSector;        // the slot's header sector on the SD card, 0 = no mirror
extern u32 dscheevosRecordDataSectors;
extern u32 dscheevosRecordSeq;
extern char dscheevosRecordTitle[12];
extern char dscheevosRecordCode[4];

#define REG_VCOUNT_REC   (*(vu16*)0x04000006)
#define SECTOR           DSCM_REC_SECTOR_BYTES
#define WRITE_CMD        0x52C19u  // CMD25 WRITE_MULTIPLE_BLOCK, one block, auto-stop: what my_sdmmc's writes send
#define START_SPINS      3000u     // register polls a write start may take (~1 ms): a stuck controller can't hold the IRQ
#define FLUSH_SPINS      400000u   // polls a flush waits for the card (~0.3 s)
#define FINISH_VBLANKS   180u      // a write not ended after this many VBlanks (3 s) is given up and started again
#define BACKOFF_VBLANKS  60u       // after an error: VBlanks without a write
#define HEADER_VBLANKS   120u      // the header at most every 2 s while data flows ...
#define HEADER_SECTORS   32u       // ... or after this many data sectors
#define RETRY_MAX        3u        // errors on one sector before it is skipped
#define RING_MARGIN      16384u    // bytes the module may have written past `written` as this reads it (one publish)
#define FLUSH_SECTORS    8u        // data sectors a flush writes at most

enum { FLIGHT_NONE = 0, FLIGHT_DATA, FLIGHT_HEADER };
enum { END_NOT_YET = 0, END_OK, END_ERROR, END_LOST };

static u8 flight = FLIGHT_NONE;
static u8 retries = 0;
static u8 headerDirty = 0;        // sd_written moved since the last header
static u16 runSeen = 0;           // the module's run the state is for
static u32 flightSector;          // the stream sector in flight (FLIGHT_DATA)
static u32 flightVBlanks;
static u32 sinceHeader;           // VBlanks since the last header write
static u32 sectorsSinceHeader;
static u32 backoff;

// The module's recording header, at its physical address (the module's data
// may be linked through the 0x0C000000 mirror); NULL until it is published
// and sane.
static dscm_rec_t* recHeader(void) {
	if (*(vu32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_MAGIC) != DSCHEEVOS_MAGIC) {
		return NULL;
	}
	u32 p = *(vu32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_REC);
	if (!p) {
		return NULL;
	}
	p = (p & 0x00FFFFFF) | 0x02000000;
	if (p < DSCHEEVOS_LOCATION || p + sizeof(dscm_rec_t) > DSCHEEVOS_LOCATION + DSCHEEVOS_SIZE || (p & 31)) {
		return NULL;
	}
	dscm_rec_t* r = (dscm_rec_t*)p;
	if (r->magic != DSCM_REC_MAGIC || r->run == 0 || r->ring_size < 4 * RING_MARGIN || (r->ring_size % SECTOR)
	 || r->ring < 0x02000000 || r->ring + r->ring_size > 0x03000000 || (r->ring & 3)) {
		return NULL;
	}
	return r;
}

// Sends one sector to the card and returns once the controller has taken the
// data, as my_sdmmc_sdcard_writesectors does up to there: it doesn't wait for
// the card to finish. 0, or the step that failed.
static int sdWriteStart(u32 sector, const u32* data) {
	u32 spins = START_SPINS;
	if (handleSD.isSDHC == 0) {
		sector <<= 9;
	}
	while (sdmmc_read16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY) {
		if (!--spins) return 1;
	}
	sdmmc_mask16(REG_SDPORTSEL, 0x3, (u16)handleSD.devicenumber);   // the SD card, its clock and bus width
	setckl(handleSD.clk);
	if (handleSD.SDOPT == 0) {
		sdmmc_mask16(REG_SDOPT, 0, 0x8000);
	} else {
		sdmmc_mask16(REG_SDOPT, 0x8000, 0);
	}
	sdmmc_write16(REG_SDSTOP, 0x100);
	sdmmc_write16(REG_SDBLKCOUNT32, 1);
	sdmmc_write16(REG_SDBLKLEN32, 0x200);
	sdmmc_write16(REG_SDBLKCOUNT, 1);
	sdmmc_write16(REG_SDIRMASK0, 0);
	sdmmc_write16(REG_SDIRMASK1, 0);
	sdmmc_write16(REG_SDSTATUS0, 0);
	sdmmc_write16(REG_SDSTATUS1, 0);
	sdmmc_mask16(REG_SDDATACTL32, 0x1800, 0x400);   // TX32RQ and RX32RDY IRQs off, FIFO cleared
	sdmmc_write16(REG_SDCMDARG0, sector & 0xFFFF);
	sdmmc_write16(REG_SDCMDARG1, sector >> 16);
	sdmmc_write16(REG_SDCMD, WRITE_CMD & 0xFFFF);
	for (;;) {
		const u16 status1 = sdmmc_read16(REG_SDSTATUS1);
		if (status1 & TMIO_MASK_GW) return 2;
		if (!(sdmmc_read16(REG_SDDATACTL32) & 0x200)) break;   // the FIFO takes a block
		if (!--spins) return 3;
	}
	sdmmc_mask16(REG_SDSTATUS1, TMIO_STAT1_TXRQ, 0);
	for (u32 i = 0; i < SECTOR / 4; i++) {
		sdmmc_write32(REG_SDFIFO32, data[i]);
	}
	sdmmc_mask16(REG_SDDATACTL32, 0x1000, 0);
	return 0;
}

// How a started write stands (END_*). Its status flags are cleared once it
// ended; they are read only, so a command of the game's save running now
// isn't disturbed either way.
static int sdWriteEnd(dscm_rec_t* rec) {
	const u16 status1 = sdmmc_read16(REG_SDSTATUS1);
	if (status1 & TMIO_MASK_GW) {
		if (rec) rec->sd_last_error = (4u << 16) | status1;
		sdmmc_write16(REG_SDSTATUS0, 0);
		sdmmc_write16(REG_SDSTATUS1, 0);
		return END_ERROR;
	}
	if (status1 & TMIO_STAT1_CMD_BUSY) {
		return END_NOT_YET;
	}
	const u16 status0 = sdmmc_read16(REG_SDSTATUS0);
	if (!(status0 & TMIO_STAT0_DATAEND)) {
		// no end and no response left either: another command cleared the controller
		return (status0 & TMIO_STAT0_CMDRESPEND) ? END_NOT_YET : END_LOST;
	}
	sdmmc_write16(REG_SDSTATUS0, 0);
	sdmmc_write16(REG_SDSTATUS1, 0);
	return END_OK;
}

// Stream sector k is still whole in the ring (the module may be a publish
// ahead of the `written` read here).
static bool sectorInRing(const dscm_rec_t* rec, u32 k) {
	return rec->written + RING_MARGIN <= k * SECTOR + rec->ring_size;
}

static const u32* ringSector(const dscm_rec_t* rec, u32 k) {
	return (const u32*)(rec->ring + (k * SECTOR) % rec->ring_size);
}

static u32 dataSector(u32 k) {
	return dscheevosRecordSector + 1 + k % dscheevosRecordDataSectors;
}

// A new recording (the module started a run, or another): the copy starts over.
static void startRun(dscm_rec_t* rec) {
	runSeen = rec->run;
	rec->sd_written = 0;
	rec->sd_valid_from = 0;
	rec->sd_skipped = 0;
	rec->sd_state = DSCM_REC_SD_ON;
	toncset(rec->sd_header, 0, sizeof(rec->sd_header));
	headerDirty = 1;              // the claim's header has no run yet
	sinceHeader = HEADER_VBLANKS; // so it goes out at once
	sectorsSinceHeader = 0;
	retries = 0;
}

// The slot's header sector image, from the copy's state. Written after the
// data it describes, so a power-off between leaves an older but true header.
static void fillHeader(dscm_rec_t* rec, u8 flags) {
	dscm_recslot_t* h = (dscm_recslot_t*)rec->sd_header;
	h->magic = DSCM_RECSLOT_MAGIC;
	h->size = sizeof(*h);
	h->seq = dscheevosRecordSeq;
	h->run = rec->run;
	h->replayed = 0;
	h->flags = DSCM_RECSLOT_CLAIMED | flags;
	tonccpy(h->game_code, dscheevosRecordCode, sizeof(h->game_code));
	tonccpy(h->title, dscheevosRecordTitle, sizeof(h->title));
	tonccpy(h->game_hash, rec->game_hash, sizeof(h->game_hash));
	h->data_sectors = dscheevosRecordDataSectors;
	h->written = rec->sd_written;
	h->valid_from = rec->sd_valid_from;
	h->segment = rec->segment;
	h->end_frame = rec->end_frame;
	h->skipped = rec->sd_skipped;
	h->errors = rec->sd_errors;
	h->reserved = 0;
}

// The next stream sector to write, skipping what the ring has overwritten;
// (u32)-1 when the copy is up to date.
static u32 nextSector(dscm_rec_t* rec) {
	const u32 written = rec->written;
	u32 k = rec->sd_written / SECTOR;
	if ((k + 1) * SECTOR > written) {
		return (u32)-1;
	}
	if (!sectorInRing(rec, k)) {
		// the ring got ahead of the writer: on from the oldest whole sector
		u32 oldest = (written + RING_MARGIN - rec->ring_size + SECTOR - 1) / SECTOR;
		if ((oldest + 1) * SECTOR > written) {
			oldest = written / SECTOR - 1;
		}
		rec->sd_skipped += oldest - k;
		rec->sd_written = oldest * SECTOR;
		rec->sd_valid_from = oldest * SECTOR;
		headerDirty = 1;
		k = oldest;
	}
	return k;
}

static void failed(dscm_rec_t* rec, int step) {
	rec->sd_errors++;
	if (step) rec->sd_last_error = ((u32)step << 16) | sdmmc_read16(REG_SDSTATUS1);
	backoff = BACKOFF_VBLANKS;
	flight = FLIGHT_NONE;
	rec->sd_state = DSCM_REC_SD_ON;
	if (++retries >= RETRY_MAX) {
		// the sector can't be written: on without it, the file's copy starts over after it
		retries = 0;
		if (flightSector != (u32)-1) {
			rec->sd_skipped++;
			rec->sd_written = (flightSector + 1) * SECTOR;
			rec->sd_valid_from = rec->sd_written;
			headerDirty = 1;
		}
	}
}

// Starts a data sector write (stream sector k) or, with FLIGHT_HEADER, the
// header's; true when one was started. The cost goes into the stats.
static bool startWrite(dscm_rec_t* rec, int what, u32 k, u8 headerFlags) {
	const u16 v0 = REG_VCOUNT_REC;
	int step;
	if (what == FLIGHT_HEADER) {
		fillHeader(rec, headerFlags);
		flightSector = (u32)-1;
		step = sdWriteStart(dscheevosRecordSector, (const u32*)rec->sd_header);
	} else {
		flightSector = k;
		step = sdWriteStart(dataSector(k), ringSector(rec, k));
	}
	if (step) {
		failed(rec, step);
		return false;
	}
	flight = what;
	flightVBlanks = 0;
	rec->sd_state = DSCM_REC_SD_FLIGHT;
	const u16 v1 = REG_VCOUNT_REC;
	const u32 lines = (v1 >= v0) ? (v1 - v0) : (v1 + 263 - v0);
	if (what == FLIGHT_HEADER) rec->sd_header_writes++; else rec->sd_writes++;
	rec->sd_lines_sum += lines;
	if (lines > rec->sd_lines_max) rec->sd_lines_max = lines;
	return true;
}

// Takes the ended write (END_OK, END_ERROR or END_LOST) into the copy's state.
static void finishWrite(dscm_rec_t* rec, int end) {
	if (flightVBlanks > rec->sd_finish_max) rec->sd_finish_max = flightVBlanks;
	if (end == END_ERROR) {
		failed(rec, 0);
		return;
	}
	if (end == END_LOST) {
		rec->sd_lost++;              // on the card or not: written again to be sure
	} else if (flight == FLIGHT_DATA) {
		retries = 0;
		rec->sd_written = (flightSector + 1) * SECTOR;
		if (!sectorInRing(rec, flightSector)) {
			// the ring overtook the sector while the card took it: its copy may mix two laps
			rec->sd_skipped++;
			rec->sd_valid_from = rec->sd_written;
		}
		headerDirty = 1;
		sectorsSinceHeader++;
	} else {
		headerDirty = 0;
		sinceHeader = 0;
		sectorsSinceHeader = 0;
	}
	flight = FLIGHT_NONE;
	rec->sd_state = DSCM_REC_SD_ON;
}

void dscheevosRecordVBlank(int* saveMutex, bool sdBusy) {
	if (!dscheevosRecordSector) {
		return;
	}
	dscm_rec_t* rec = recHeader();
	if (!rec) {
		// the module took its ring away (a reset): a write in flight still ends
		if (flight != FLIGHT_NONE && !sdBusy && tryLockMutex(saveMutex)) {
			if (sdWriteEnd(NULL) != END_NOT_YET || ++flightVBlanks >= FINISH_VBLANKS) {
				flight = FLIGHT_NONE;
			}
			unlockMutex(saveMutex);
		}
		if (flight == FLIGHT_NONE) {
			runSeen = 0;
		}
		return;
	}
	if (rec->run != runSeen && flight == FLIGHT_NONE) {
		startRun(rec);
	}
	sinceHeader++;
	if (backoff) {
		backoff--;
		return;
	}
	if (sdBusy || !tryLockMutex(saveMutex)) {
		rec->sd_busy++;
		return;
	}
	if (flight != FLIGHT_NONE) {
		// one SD step a VBlank: the next write waits for the next one
		const int end = sdWriteEnd(rec);
		if (end != END_NOT_YET) {
			finishWrite(rec, end);
		} else if (++flightVBlanks >= FINISH_VBLANKS) {
			finishWrite(rec, END_LOST);   // given up: started again
		}
	} else {
		const u32 k = nextSector(rec);
		const bool headerDue = headerDirty && (sinceHeader >= HEADER_VBLANKS || sectorsSinceHeader >= HEADER_SECTORS);
		if (headerDue || k == (u32)-1) {
			if (headerDirty) {
				startWrite(rec, FLIGHT_HEADER, 0, 0);
			}
		} else {
			startWrite(rec, FLIGHT_DATA, k, 0);
		}
	}
	unlockMutex(saveMutex);
}

// Waits for the write in flight, bounded; true when it ended well.
static bool waitWrite(dscm_rec_t* rec) {
	u32 spins = FLUSH_SPINS;
	while (flight != FLIGHT_NONE) {
		const int end = sdWriteEnd(rec);
		if (end != END_NOT_YET) {
			finishWrite(rec, end);
			return end == END_OK;
		}
		if (!--spins) {
			finishWrite(rec, END_LOST);
			return false;
		}
	}
	return true;
}

void dscheevosRecordFlush(void) {
	if (!dscheevosRecordSector) {
		return;
	}
	dscm_rec_t* rec = recHeader();
	if (!rec || rec->run != runSeen) {
		return;
	}
	waitWrite(rec);
	backoff = 0;
	for (u32 n = 0; n < FLUSH_SECTORS; n++) {
		const u32 k = nextSector(rec);
		if (k == (u32)-1 || !startWrite(rec, FLIGHT_DATA, k, 0) || !waitWrite(rec)) {
			break;
		}
	}
	if (startWrite(rec, FLIGHT_HEADER, 0, DSCM_RECSLOT_FLUSHED)) {
		waitWrite(rec);
	}
}
#endif /* TWLSDK */
