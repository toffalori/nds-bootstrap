/*
 * DScheevos hook points (RetroAchievements). The module itself lives in the
 * DScheevos repository (dscheevos/module); these are the few addresses and
 * offsets nds-bootstrap needs to load it and call into it. They must match
 * dscheevos_module.h there.
 */
#ifndef DSCHEEVOS_HOOKS_H
#define DSCHEEVOS_HOOKS_H

#include <nds/ndstypes.h>

#define DSCHEEVOS_LOCATION        0x02D80000  // module image (2 MB reserved)
#define DSCHEEVOS_SIZE            0x00200000
#define DSCHEEVOS_IMAGE_MAX       0x00080000
#define DSCHEEVOS_SESSION         (DSCHEEVOS_LOCATION + DSCHEEVOS_IMAGE_MAX)
#define DSCHEEVOS_SESSION_MAX     0x00060000  // a file past 256 KB runs into the module heap; the module moves it (heap.h there)
#define DSCHEEVOS_CACHE_END       0x0CD80000  // ROM cache must end here (same RAM as DSCHEEVOS_LOCATION)
#define DSCHEEVOS_CACHE_RESERVE   0x0027C000  // ROM cache lost to the cut: 159 of 760 16 KB slots, every layout

#define DSCHEEVOS_MAGIC           0x56435344  // "DSCV"
#define DSCHEEVOS_VERSION         7
#define DSCHEEVOS_SESSION_MAGIC   0x53435344  // "DSCS"

// header words
#define DSCHEEVOS_H_MAGIC         0x00
#define DSCHEEVOS_H_VERSION       0x04
#define DSCHEEVOS_H_IMAGE_SIZE    0x08
#define DSCHEEVOS_H_INIT          0x0C
#define DSCHEEVOS_H_VBLANK        0x10
#define DSCHEEVOS_H_HALT          0x14
#define DSCHEEVOS_H_STATUS        0x1C
#define DSCHEEVOS_H_VBLANK_COUNT  0x24
#define DSCHEEVOS_H_RESET         0x28
#define DSCHEEVOS_H_FLUSH         0x38  // ARM code: evaluate the frames still queued (game stopped), before a journal copy; 0 in older modules
#define DSCHEEVOS_H_LIST          0x2C  // DScheevosList*, 0 until the set is loaded; swapped whole after a rebuild
#define DSCHEEVOS_H_NOTICES       0x30  // DScheevosNotices*
#define DSCHEEVOS_H_JOURNAL       0x34  // DScheevosJournal*, kept current in RAM (the ARM7 reads it directly)
#define DSCHEEVOS_H_NOTIFY        0x40  // unlock count, kept in RAM by the ARM9
#define DSCHEEVOS_H_NETRING       0x44  // the module's frame ring for the ARM7 SDIO shim (dscm_shim.h), 0 = none
#define DSCHEEVOS_H_CUES          0x48  // LED cues, kept in RAM by the ARM9 (DSCHEEVOS_CUE_*)

// header word 0x48: counters wrap, act on a change
#define DSCHEEVOS_CUE_PROGRESS_MASK     0x000000FF  // measured progress updates
#define DSCHEEVOS_CUE_LEADERBOARD_MASK  0x0000FF00  // leaderboard attempts started, failed or submitted
#define DSCHEEVOS_CUE_LEADERBOARD_SHIFT 8
#define DSCHEEVOS_CUE_CHALLENGE         0x00010000  // a challenge indicator is showing

// session file words
#define DSCHEEVOS_S_SIZE          0x04  // sizeof(dscm_boot_t)
#define DSCHEEVOS_S_FILE_SIZE     0x08
#define DSCHEEVOS_S_ROM_PATH      0x0C  // char[256]
#define DSCHEEVOS_S_HARDCORE      0x1CD // u8
#define DSCHEEVOS_S_GAME_HASH     0x1AC // char[33]: the ROM's RetroAchievements hash (lower-case hex)
#define DSCHEEVOS_S_FLAGS         0x1CE // u8: DSCHEEVOS_BOOT_*
// dscheevosFindSession()
#define DSCHEEVOS_SESSION_NONE      0
#define DSCHEEVOS_SESSION_OK        1
#define DSCHEEVOS_SESSION_WRONG_ROM 2 // the ROM's hash isn't the session's: achievements off
#define DSCHEEVOS_S_UNIX_TIME     0x1D4 // u32: console clock when the launcher wrote the file
#define DSCHEEVOS_S_NET_VALID     0x1F6 // u8: the game keeps the launcher's Wi-Fi connection
#define DSCHEEVOS_S_CLOCK         0x1F7 // u8, set here in RAM only: DSCHEEVOS_CLOCK_*
#define DSCHEEVOS_CLOCK_CPU_133   0x01  // ARM9 at 133 MHz (BOOST_CPU, or DSI_MODE = 2 on a DS game)
#define DSCHEEVOS_CLOCK_VRAM      0x02  // BOOST_VRAM
#define DSCHEEVOS_S_AP_SSID       0x210 // char[32]: the access point the launcher joined
#define DSCHEEVOS_S_AP_SSID_LEN   0x230 // u8
#define DSCHEEVOS_S_AP_SECURITY   0x238 // u8: 3 = WPA2
#define DSCHEEVOS_S_PMK_VALID     0x23B // u8, set here in RAM only
#define DSCHEEVOS_S_PMK           0x23C // u8[32], filled here in RAM only
#define DSCHEEVOS_S_BOOT_SIZE     0x260
#define DSCHEEVOS_AP_WPA2         3
#define DSCHEEVOS_BOOT_SPECTATOR  0x01  // evaluate and show, send nothing
#define DSCHEEVOS_BOOT_STREAM     0x04  // Cheevotron: stream RAM to the phone, no on-console placard
#define DSCHEEVOS_BOOT_CART       0x08  // Cheevotron: boot the inserted Slot-1 cartridge natively, not the SD ROM

// valueBits flag in both ce7 and ce9: hooks are live
#define DSCHEEVOS_VALUEBIT        BIT(28)

// DScheevosStatus.hardcore: hardcore rules on, but the server sent its
// "unsupported emulator" warning, so it files unlocks as softcore
#define DSCHEEVOS_HARDCORE_REFUSED 2

// The module's status (dscm_status_t in dscheevos_module.h), read by the in-game menu
typedef struct DScheevosUnlock {
	u32 id;
	u32 points;
	u32 frame;
	char title[48];
} DScheevosUnlock;

typedef struct DScheevosStatus {
	u32 active;
	u32 hardcore;
	u32 framesSnapshotted;
	u32 framesEvaluated;
	u32 framesDropped;
	u32 framesMissed;
	u32 evalLines;
	u32 unlockCount;
	DScheevosUnlock recent[8];
	u32 pendingRequests;
	u32 error;
	char gameTitle[48];
	u32 achievements;
	u32 achievementsUnlocked;
	u32 points;
	u32 pointsUnlocked;
	u32 netState; // DSCHEEVOS_NET_*
	u32 netDetail;
	u32 unlocksSent;
	u32 tlsHandshakeMs;
	u32 netRejoins; // times the module reassociated (AP drop, group rekey)
	// self-timing in scanlines (263 a frame, ~4,260 ARM9 cycles each at 67 MHz)
	u32 timedFrames;
	u32 irqLinesLast; // the module's VBlank work, inside the game's IPC IRQ
	u32 irqLinesMax;
	u32 irqLinesTotal;
	u32 idleLinesLast; // idle work (halt path) in the last frame
	u32 idleLinesMax;
	u32 idleLinesTotal;
	u32 idleNone; // frames with no idle pass
	u32 idleCut; // idle passes a VBlank preempted
} DScheevosStatus;

enum {
	DSCHEEVOS_NET_NONE = 0,
	DSCHEEVOS_NET_CONNECTING,
	DSCHEEVOS_NET_ONLINE,
	DSCHEEVOS_NET_SENDING,
	DSCHEEVOS_NET_PAUSED,
	DSCHEEVOS_NET_OFFLINE,
	DSCHEEVOS_NET_ERROR,
};

// ABI v6: the achievement list, notices and offline journal (dscm_list_t,
// dscm_notices_t, dscm_journal_t in dscheevos_module.h)
#define DSCHEEVOS_LIST_MAGIC      0x4C435344  // "DSCL"
#define DSCHEEVOS_NOTICES_MAGIC   0x4E435344  // "DSCN"
#define DSCHEEVOS_JOURNAL_MAGIC   0x4A435344  // "DSCJ"
#define DSCHEEVOS_JOURNAL_FILE_SIZE 4096      // sd:/_nds/dscheevos/pending.bin, exactly this size
#define DSCHEEVOS_JOURNAL_MAX     96
#define DSCHEEVOS_NOTICE_RING     16
#define DSCHEEVOS_INDICATOR_MAX   8

// DScheevosListEntry.state
enum {
	DSCHEEVOS_ACH_LOCKED = 0,
	DSCHEEVOS_ACH_UNLOCKED,
	DSCHEEVOS_ACH_UNLOCKED_SOFTCORE, // hardcore session, unlocked earlier in softcore only
	DSCHEEVOS_ACH_DISABLED,
};
// DScheevosListEntry.flags
#define DSCHEEVOS_ACH_FLAG_MEASURED    0x01
#define DSCHEEVOS_ACH_FLAG_CHALLENGE   0x02
#define DSCHEEVOS_ACH_FLAG_MISSABLE    0x04
#define DSCHEEVOS_ACH_FLAG_PROGRESSION 0x08
#define DSCHEEVOS_ACH_FLAG_WIN         0x10

typedef struct DScheevosListEntry {
	u32 id;
	u32 unlockTime;
	u16 points;
	u8 state;
	u8 flags;
	char title[48];
	char description[100];
	char measured[16];
} DScheevosListEntry;

typedef struct DScheevosList {
	u32 magic;
	u32 generation;
	u32 count;
	u32 entrySize; // sizeof(DScheevosListEntry)
	DScheevosListEntry e[];
} DScheevosList;

// DScheevosNotice.kind
enum {
	DSCHEEVOS_NOTICE_NONE = 0,
	DSCHEEVOS_NOTICE_SESSION_START,
	DSCHEEVOS_NOTICE_UNLOCK,
	DSCHEEVOS_NOTICE_MASTERY,
	DSCHEEVOS_NOTICE_PROGRESS,
	DSCHEEVOS_NOTICE_LEADERBOARD_STARTED,
	DSCHEEVOS_NOTICE_LEADERBOARD_FAILED,
	DSCHEEVOS_NOTICE_LEADERBOARD_SUBMITTED,
	DSCHEEVOS_NOTICE_LEADERBOARD_RANK,
	DSCHEEVOS_NOTICE_OFFLINE,
	DSCHEEVOS_NOTICE_ONLINE,
};

typedef struct DScheevosNotice {
	u32 seq;
	u16 kind;
	u16 reserved;
	u32 id;
	s32 value;
	u32 frame;
	char text[48];
	char detail[48];
} DScheevosNotice;

typedef struct DScheevosIndicator {
	u32 id; // 0 = slot free
	char text[24]; // challenge: title; tracker: formatted value
} DScheevosIndicator;

typedef struct DScheevosNotices {
	u32 magic;
	u32 head;
	DScheevosNotice ring[DSCHEEVOS_NOTICE_RING];
	u32 indicatorsGeneration;
	DScheevosIndicator challenges[DSCHEEVOS_INDICATOR_MAX];
	DScheevosIndicator trackers[DSCHEEVOS_INDICATOR_MAX];
} DScheevosNotices;

typedef struct DScheevosJournalEntry {
	u8 kind;
	u8 hardcore;
	u8 sent;
	u8 reserved;
	u32 id;
	s32 score;
	u32 unixTime;
} DScheevosJournalEntry;

typedef struct DScheevosJournal {
	u32 magic;
	u32 size; // sizeof(DScheevosJournal)
	u32 count;
	u32 gameId;
	char username[32];
	char gameHash[33];
	u8 reserved[3];
	DScheevosJournalEntry e[DSCHEEVOS_JOURNAL_MAX];
} DScheevosJournal;

#ifndef __cplusplus
_Static_assert(sizeof(DScheevosListEntry) == 176, "dscm_list_entry_t");
_Static_assert(sizeof(DScheevosList) == 16, "dscm_list_t");
_Static_assert(sizeof(DScheevosNotice) == 116, "dscm_notice_t");
_Static_assert(sizeof(DScheevosNotices) == 2316, "dscm_notices_t");
_Static_assert(sizeof(DScheevosJournalEntry) == 16, "dscm_journal_entry_t");
_Static_assert(sizeof(DScheevosJournal) == 1620, "dscm_journal_t");
_Static_assert(sizeof(DScheevosJournal) <= DSCHEEVOS_JOURNAL_FILE_SIZE, "journal fits pending.bin");
#endif

#endif // DSCHEEVOS_HOOKS_H
