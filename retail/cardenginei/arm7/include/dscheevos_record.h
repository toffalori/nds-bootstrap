/*
 * Cheevotron: the ARM7's copy of the streamer's recording on the SD card
 * (dscheevos_record.c, dscm_record.h).
 */
#ifndef DSCHEEVOS_RECORD_H
#define DSCHEEVOS_RECORD_H

#include <nds/ndstypes.h>

// Once a VBlank, from the DScheevos VBlank hook: starts or ends at most one
// sector write. sdBusy: the card engine has an SD read in flight; driveInited:
// its SD handle is set up (nothing is written before). saveMutex is taken for
// the registers' sake and given back before returning.
void dscheevosRecordVBlank(int* saveMutex, bool sdBusy, bool driveInited);

// At a safe point with the game stopped and saveMutex held (the in-game
// menu, a reset, the way out): ends a write in flight, writes the stream
// sectors still complete in the ring, then the slot's header. Blocks, bounded.
void dscheevosRecordFlush(bool driveInited);

#endif
