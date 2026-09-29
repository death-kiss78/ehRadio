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

// Consecutive failed card probes before the mode ends. The probe reads sector 0 through the SD host, and
// FATFS writes use that same host, so a card still busy after a write times out one probe - a folder delete
// is a burst of writes, which is why a single failure must not be trusted.
#ifndef SDMAN_CARD_GONE_STRIKES
  #define SDMAN_CARD_GONE_STRIKES 3
#endif

class FileManager {
  public:
    // True between entering the mode and leaving it: Done, the idle timeout, or the card leaving the slot.
    bool active() const { return _active; }

    void enter();   // GET /sdman - mounts if needed, blocks the player, starts the idle clock
    // Unblocks the player and restores the display; under SmartStart it gives the audio back too.
    // resumeAudio=false for the one exit with nothing to resume: the card leaving the slot.
    void leave(bool resumeAudio = true);
    void loop();    // idle timeout and card-present check - call from the main loop
    void registerRoutes(AsyncWebServer &server);

    uint32_t idleRemainingMs() const;   // drives the on-screen countdown

    // True while a file is open for writing.  The Done handler asks before closing the mode: leave() removes a
    // half-written file by design, so a close arriving mid-upload would destroy one the user is still watching.
    bool uploadOpen() const;

    // True while a mutating request is running - the delete batch, whose whole selection is handled inside one
    // request (uploads report through uploadOpen()).  hDone() refuses while it is set: leave() would close the mode
    // and tear the operation down underneath it, and a second tab is the case the page's own inert buttons cannot
    // cover.  Cleared by the handler that set it, and by leave() as a backstop.
    bool busy() const { return _busy; }
    void markBusy(bool on) { _busy = on; }

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
    static constexpr uint32_t SD_REINDEX_RETRY_MS = 4000;
    static constexpr uint8_t  SD_REINDEX_MAX_RETRIES = 2;
    uint32_t _lastActivity = 0;
    uint32_t _lastCountdownMs = 0;   // the on-screen countdown is redrawn at most once a second
    uint8_t  _cardGone = 0;          // consecutive failed presence probes, see SDMAN_CARD_GONE_STRIKES
};

extern FileManager filemanager;

#endif  // USE_SD
#endif  // filemanager_h

