#ifndef sdmanager_h
#define sdmanager_h

#include "options.h"
// The FS API, for fs::FSImplPtr in the constructor below.  Both transport headers include it as well, but the
// name is used here and <SD_MMC.h> does not carry the "using namespace fs;" that <SD.h> does, so state the
// dependency instead of inheriting it from whichever transport happens to be selected.
#include <FS.h>
#include <esp_task_wdt.h>   // esp_task_wdt_reset(), for the long card walks

// Two transports are possible: the native SDMMC host on ESP32-S3 (selected by defining SDMMC_
// pins in myoptions.h, see SD_USE_MMC) or the classic SPI interface everywhere else.
// fs::SDFS and fs::SDMMCFS are sibling fs::FS subclasses with the same fs::FSImplPtr constructor, so
// the base class can be swapped without touching any caller: sdman is consumed as fs::FS& by
// Config::SDPLFS() and Audio::connecttoFS().
#if defined(SD_USE_MMC)
  #include <SD_MMC.h>
  #define SDMAN_FS_BASE fs::SDMMCFS
#else
  #include <SD.h>
  #define SDMAN_FS_BASE fs::SDFS
#endif

#define SD_PATH_LENGTH 256 // max length for SD filesystem path buffers

// Feed the task watchdog from inside a long card operation.  Card work is slow by nature - a free-space figure walks
// the whole FAT, a listing opens every entry - and it runs on whichever task asked for it, which for the WebUI is
// AsyncTCP's task, and AsyncTCP subscribes that task to the watchdog.  Without this a card that takes seconds does
// not make the page slow, it aborts the device: a slow card should cost time, not a reboot.  The tick also lets the
// other tasks run, and where the caller is not subscribed esp_task_wdt_reset() reports ESP_ERR_NOT_FOUND harmlessly.
static inline void sdFeedWatchdog() {
  esp_task_wdt_reset();
  vTaskDelay(1);
}

class SDManager : public SDMAN_FS_BASE {
  public:
    bool ready = false;
  public:
    SDManager(fs::FSImplPtr impl) : SDMAN_FS_BASE(impl) {}
    bool start();
    void stop();
    bool cardPresent();
    void listSD(File &plSDfile, File &plSDindex, const char * dirname, uint8_t levels);
    void indexSDPlaylist();
    uint32_t countAudioFiles();
    void trySdRemount();  // attempt SD mount + re-index (called from controls in SDOFFLINE mode)
  private:
    uint32_t _sdFCount = 0;
    uint32_t _countAudioFilesRecursive(const char* dirname, uint8_t levels);
    bool _checkNoMedia(const char* path);
    bool _endsWith (const char* base, const char* str);
};

extern SDManager sdman;
#endif
