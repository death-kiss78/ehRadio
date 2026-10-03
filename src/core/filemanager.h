#ifndef filemanager_h
#define filemanager_h

#include "options.h"

#ifdef USE_SD

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// SD card file manager: the /sdman browser UI and the runtime mode that makes it safe. SD builds only.
// Runtime-only by design - nothing is persisted, so unplugging instead of pressing Done costs nothing.

// Idle time before the mode closes and hands the device back. Overridable from myoptions.h; it lives here
// because nothing outside this module reads it - the countdown asks idleRemainingMs().
#ifndef SDMAN_AUTO_EXIT_MS
  #define SDMAN_AUTO_EXIT_MS 180000UL
#endif

// The countdown is only DRAWN while the time left is at or below this.  Everything above it is "not yet", and a
// clock that is refreshed by activity - every upload chunk, every delete entry - would otherwise sit at the top of
// its range looking stopped, which is exactly what it looked like.  Below the threshold it is a real countdown a
// user can act on, so what is left on screen reads as "two minutes after no activity" and nothing else.
#ifndef SDMAN_COUNTDOWN_FROM_MS
  #define SDMAN_COUNTDOWN_FROM_MS 120000UL
#endif

// Consecutive failed card probes before the mode ends. The probe reads sector 0 through the SD host, and
// FATFS writes use that same host, so a card still busy after a write times out one probe - a folder delete
// is a burst of writes, which is why a single failure must not be trusted.
#ifndef SDMAN_CARD_GONE_STRIKES
  #define SDMAN_CARD_GONE_STRIKES 3
#endif

// How long an upload may go without a chunk before the mode abandons it.  An open upload HOLDS the mode open -
// the 180 s user clock cannot close over one, the same rule hDone() already follows - so this is the only thing
// that can end a transfer whose browser has gone away.  It is deliberately far below the idle timeout, because
// what it protects against is a card that stops answering mid-write: that used to hold the mode, and the batch,
// for the whole 180 s and then take the partial file with it.  Overridable from myoptions.h.
//
// 30 s was too tight in the field.  It fired the instant a chunk arrived, which proves the browser had merely
// gone quiet for a while mid-file - the device then abandoned a transfer that was still alive, and the log read
// "quiet for 0ms" because the arriving chunk had just restamped the clock.  The window is now well clear of any
// plausible pause and still inside the user clock, so it does the one job it was written for: releasing a
// transfer whose browser has gone for good.  A card that answers slowly in the WRITE rather than going quiet is
// not this branch's case at all - that is the WDT's, or the short-write path's.
#ifndef SDMAN_UPLOAD_STALL_MS
  #define SDMAN_UPLOAD_STALL_MS 120000UL
#endif

// ...and the same deadline once a chunk has already failed.  The file is lost at that point and nothing is being
// waited for, so the transfer is released quickly instead of holding the manager - and the whole WebUI - locked
// for the healthy-transfer window.  That lock, seen for two minutes after a card started refusing writes, is what
// this exists to prevent.
//
// This window is now the one thing the page waits for, which is what makes it short.  A failed transfer keeps its
// handle - and the resume offset is recorded when that handle is closed - so a retry may not be sent until this
// branch has run and the page asks /sdman/info?up=1 for `open:false` before it sends anything.  It only has to
// outlast the page's ABORT, not a browser that may come back: a budget card in the field failed 20 to 40 times per
// file, and three seconds each time was over a minute per file of doing nothing.  One second.  (If an aborted request
// can be made to run hUploadDone() as well, this branch stops mattering at all - worth finding out.)
#ifndef SDMAN_UPLOAD_STALL_FAILED_MS
  #define SDMAN_UPLOAD_STALL_FAILED_MS 1000UL
#endif

// Rescues are budgeted by SIZE, because a flat count conflated two unrelated failures seen in the field.  A 10 MB
// file was 24225 bytes from finishing when its ninth isolated refusal met a flat cap of 8 and the transfer was
// thrown away - while a card that was genuinely gone refused 17 chunks in a row.
//
// The divisor is deliberately small (one rescue per 64 KB) so this budget is never the thing that ends a transfer.
// A 14-file batch settled the question: every file that LANDED needed zero rescues, while Stars spent 11 rescues in
// 2.08 MB (one per 190 KB) and the 10 MB file above needed one per 1.25 MB.  The rate is not uniform, so no divisor
// is right for every file - and a transfer that is landing, however grudgingly, should be allowed to land.  The
// BURST rule is what actually decides that a card is gone; this is only a sanity ceiling.  Rescues are counted,
// never logged one per chunk - the log storm was bad enough that logging had to be switched off to read anything.
#ifndef SDMAN_UPLOAD_RETRY_PER_BYTES
  #define SDMAN_UPLOAD_RETRY_PER_BYTES 65536ULL    // one rescue allowed per this much written
#endif
#ifndef SDMAN_UPLOAD_RETRY_FLOOR
  #define SDMAN_UPLOAD_RETRY_FLOOR 8               // plus this many, so a small file is not starved
#endif
#ifndef SDMAN_UPLOAD_RETRY_BURST
  #define SDMAN_UPLOAD_RETRY_BURST 4               // consecutive refusals with no successful chunk between them
#endif
// Give the card a moment before retrying.  The refusal this answers is `0 of 1436 bytes, errno 5, after 0ms` - zero
// bytes accepted and no bus time at all, which is a card declining the next write while it is still programming the
// previous block.  Time is the remedy; retrying instantly only asks the same question again.
#ifndef SDMAN_UPLOAD_RETRY_DELAY_MS
  #define SDMAN_UPLOAD_RETRY_DELAY_MS 2
#endif
// ...and a CEILING for a pause that grows with consecutive refusals.  A flat delay asks a busy card the same question
// the same way for ever; a budget card in the field refuses one chunk in 180 and is then behind by kilobytes, which is
// a card that needs TIME rather than another immediate retry.  The pause doubles from the base for each refusal in a
// row (2, 4, 8, 16, 32, 50, 50...) so an isolated refusal still costs only the base and a card that is struggling gets
// progressively more room.  Reset by any chunk that lands.
#ifndef SDMAN_UPLOAD_RETRY_MAX_DELAY_MS
  #define SDMAN_UPLOAD_RETRY_MAX_DELAY_MS 50
#endif

// How long a card is given to catch up before a short size() is BELIEVED.  The budget card's failure signature is
// refusal, rescue accepted, and then `the card is 3.5-5.2 KB behind` on every single one - consistently a few
// kilobytes, which is exactly what a card still programming the block it just accepted would report.  So the size is
// read, the card is given this long to settle, and the size is read AGAIN: if it catches up, the write was never
// lost and the transfer continues (logged once per file, because it is worth knowing how often this is the real
// story).  If it does not, the bytes are genuinely gone and the file fails exactly as before, with both readings in
// the line.
#ifndef SDMAN_UPLOAD_LAG_SETTLE_MS
  #define SDMAN_UPLOAD_LAG_SETTLE_MS 50
#endif

// How many bytes may be written between two proofs that the stream is real.  flush() plus size() is the only pair
// that tells the truth about the medium - and it is the ONLY truthful instrument in this whole path, because a
// buffered write reports the bytes it accepted, not the bytes that landed.  Checking only at the end is how a card
// that accepts and discards surfaced megabytes later (4815872 bytes into one file); 256 KB was still too coarse,
// because a transfer that is failing spends its rescues long before anyone notices.  64 KB makes the discovery
// early, which is what lets the failure be cheap and retryable.  0 disables the check.
#ifndef SDMAN_UPLOAD_VERIFY_BYTES
  #define SDMAN_UPLOAD_VERIFY_BYTES 65536UL
#endif

// THE CARD CLOCK FOR A MANAGER SESSION, and it is ONE clock for the WHOLE session - no stepping, no switching.
//
// Measured, not guessed: the same batch on the same board and card landed 10 of 10 files first attempt with zero
// refusals at 10 MHz where it failed about fifty times at 20 MHz - and the writes were JUST AS FAST, 226 KB/s against
// the best 20 MHz runs, because the CARD is the limit at any clock, not the bus.  A high clock was wanted for the
// PLAYER's reads, so it belongs to the player: enter() opens the session at this speed and leave() puts SDSPISPEED
// back before the card is handed to playback.
//
// AN AUTOMATIC STEP-DOWN WAS BUILT HERE, AND IT WAS REMOVED.  It halved this clock on every transfer that needed
// resuming, down to a 156250 Hz floor, on the theory that the refusals were a time-between-writes problem.  The
// field test killed it outright: a budget card froze at the SAME byte offset with the SAME ~5 KB deficit at 10, 5,
// 2.5, 1.25, 0.625, 0.3125 and 0.15625 MHz - a 64x range with no change in behaviour - while a 10 MB file went
// through untouched at 10 MHz in the same session.  The clock is not the variable, and each step it did take cost a
// remount plus a permanently slower session.  Do not rebuild it.
//
// So the manager does EVERYTHING at this speed - browsing, listing, deletes and uploads - because a session that
// changed speed part-way would have to remount, and a remount discards every open handle.  One clock, one mount,
// from enter() to leave().  Listing is slower than it would be at SDSPISPEED and that is ACCEPTED: safety over
// speed.  (If it ever needs to be faster, the first thing to measure is listings alone at SDSPISPEED - but that is
// a remount around each listing, and a mid-session switch is exactly what produced the `no_card` race.)
#ifndef SDSPISPEED_MANAGER
  #define SDSPISPEED_MANAGER 10000000
#endif

// Sector alignment of the writes: held back and prepended so that every write is a whole number of sectors long and
// starts on one, on the theory that a 1436-byte chunk makes the file system read-modify-write a PARTIAL SECTOR
// through the same driver that is programming the previous block.  Set to a sector size to enable it, 0 to write each
// chunk as it arrives (the default, and see below).
//
// MEASURED AND REFUTED, on the same 14-file, 96 MB batch, same card:
//     aligned (512)  12 then 17 failed transfers, about 7.3 minutes
//     as-it-arrives   7 then  7 failed transfers, about 5.0 to 5.8 minutes
// So the partial-sector theory is wrong, and the trend points the other way - a longer unbroken burst of sector
// programs is refused MORE readily than a chunk-sized one, which is why a 4096 payload was never tried.  The
// machinery works exactly as designed (every resumed offset in that run is a multiple of 512, so resumeFloor() and
// the assembly both did their jobs); it is the hypothesis underneath it that failed.  Kept because the code is
// small and the measurement is recorded, and because the instinct to try it again should meet this comment first.
//
// When enabled it costs one buffer (SDMAN_WRITE_SECTOR + 1536 bytes), and the held-back bytes are not counted in
// _upBytes until they are written, because _upBytes follows the WRITE, not the arrival - which is why sdFlushTail()
// runs at the end of the stream and again before the final size check.
#ifndef SDMAN_WRITE_SECTOR
  #define SDMAN_WRITE_SECTOR 0
#endif

class FileManager {
  public:
    // True between entering the mode and leaving it: Done, the idle timeout, or the card leaving the slot.
    bool active() const { return _active; }

    void enter();   // GET /sdman - mounts if needed, blocks the player, starts the idle clock
    // Unblocks the player and restores the display; under SmartStart it gives the audio back too.
    // resumeAudio=false for the one exit with nothing to resume: the card leaving the slot.
    // `why` is named in the single line the close prints, so the idle timeout, the Done button and a removed card do
    // not need a line each - they were three lines whose only difference was which branch printed first.
    void leave(bool resumeAudio = true, const char *why = "on request");
    void loop();    // idle timeout and card-present check - call from the main loop
    void registerRoutes(AsyncWebServer &server);

    uint32_t idleRemainingMs() const;   // drives the on-screen countdown

    // True while a file is open for writing.  The Done handler asks before closing the mode: leave() removes a
    // half-written file by design, so a close arriving mid-upload would destroy one the user is still watching.
    bool uploadOpen() const;

    // Milliseconds since the last chunk of the upload in flight, 0 when none is.  The stall deadline in loop()
    // and the countdown both read it: the countdown shows the upload's own remaining time while one is open, so
    // the number keeps moving instead of sitting at a full 180 s that is refreshed by every chunk.
    uint32_t uploadIdleMs() const;

    // True while a mutating request is running - the delete batch, whose whole selection is handled inside one
    // request (uploads report through uploadOpen()).  hDone() refuses while it is set: leave() would close the mode
    // and tear the operation down underneath it, and a second tab is the case the page's own inert buttons cannot
    // cover.  Cleared by the handler that set it, and by leave() as a backstop.
    bool busy() const { return _busy; }
    void markBusy(bool on) { _busy = on; }

    // True while the deferred rebuild is walking the card.  net-server's PLAYLISTREADY case carries this, so the
    // WebUI is told "a rebuild is running: lock and spin" as well as "the list is ready: fetch".  Only being told
    // about the end is what left the playlist on screen - and playable - through a whole walk.
    bool rebuilding() const { return _rebuilding; }

    // Refreshes the idle clock. Public because the handlers are free functions and must stamp activity.
    void touch();

    // Records that a mutation (upload, mkdir, rename, delete) invalidated the card's derived files: the SD
    // index is deleted on the spot, and this bit remembers that a re-index is owed.  Public because the
    // handlers are free functions.  No handler ever walks the card, and neither does the mode while it is
    // open: loop() pays for one re-index once the mode has closed, with the SDCHANGE counting screen up.
    void markCardChanged();

    // The root and everything under /data: playlists, SD index, credentials. No mutation may touch it.
    static bool isProtected(const String &path);

    // True if this path is the playing file, or a directory holding it. Deleting an open file succeeds until
    // the FAT handle closes, which is how a song could be deleted out from under the decoder.
    bool isPlaying(const String &path) const;

  private:
    bool _active = false;
    bool _wasPlaying = false;        // the player was running when the mode was entered - see leave()
    /* A mutation (upload, mkdir, rename, delete) invalidated the card's derived files.  Set by the handler,
       honoured by loop() after the mode closes - never rebuilt from the handler or while the manager is open,
       so a session of a hundred operations pays for one walk instead of a hundred, and the card is not walked
       while FATFS is mid-write from the AsyncTCP task.  leave() also reads it to suppress the resume, because
       a station NUMBER points at a different file once the list has changed. */
    bool _cardChanged = false;
    bool _busy = false;              // a mutating request is in flight, see busy()
    /* An unfinished build leaves NO index at all - indexSDPlaylist() writes a temporary pair and renames it into
       place only when the walk finished - so "the index is missing" is the repair signal, the same one
       initSDPlaylist() acts on at mode entry.  A build that failed here is therefore owed another pass rather than
       being forgotten, but not immediately: a card that is failing for real must not turn loop() into an endless
       walk, so the retry waits and then gives up until the next mode entry asks again. */
    uint32_t _reindexNotBeforeMs = 0;
    uint8_t  _reindexTries = 0;      // attempts since the last build that wrote an index
    bool     _rebuilding = false;    // a deferred rebuild is walking the card right now, see rebuilding()
    static constexpr uint32_t SD_REINDEX_RETRY_MS = 4000;
    static constexpr uint8_t  SD_REINDEX_MAX_RETRIES = 2;
    uint32_t _lastActivity = 0;
    uint32_t _lastCountdownMs = 0;   // the on-screen countdown is redrawn at most once a second
    uint8_t  _cardGone = 0;          // consecutive failed presence probes, see SDMAN_CARD_GONE_STRIKES
    /* The presence probe is a RAW sector read on the SPI bus, so it is rate-limited and suspended while a transfer
       is running.  At one probe per main-loop iteration it was issuing roughly 60 card commands a second straight
       through an upload being written from another task - and through every delete. */
    uint32_t _lastCardCheckMs = 0;
};

extern FileManager filemanager;

#endif  // USE_SD
#endif  // filemanager_h

