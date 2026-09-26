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

    // Refreshes the idle clock. Public because the handlers are free functions and must stamp activity.
    void touch();

    // The root and everything under /data: playlists, SD index, credentials. No mutation may touch it.
    static bool isProtected(const String &path);

    // True if this path is the playing file, or a directory holding it. Deleting an open file succeeds until
    // the FAT handle closes, which is how a song could be deleted out from under the decoder.
    bool isPlaying(const String &path) const;

  private:
    bool _active = false;
    bool _wasPlaying = false;        // the player was running when the mode was entered - see leave()
    uint32_t _lastActivity = 0;
    uint32_t _lastCountdownMs = 0;   // the on-screen countdown is redrawn at most once a second
    uint8_t  _cardGone = 0;          // consecutive failed presence probes, see SDMAN_CARD_GONE_STRIKES
};

extern FileManager filemanager;

#endif  // USE_SD
#endif  // filemanager_h

