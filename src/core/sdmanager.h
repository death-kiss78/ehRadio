#ifndef sdmanager_h
#define sdmanager_h

#include "options.h"

// Two transports are possible: the native SDMMC host on ESP32-S3 (selected by defining SDMMC_
// pins in myoptions.h, see SD_USE_MMC) or the classic SPI interface everywhere else.
// fs::SDFS and fs::SDMMCFS are sibling fs::FS subclasses with the same FSImplPtr constructor, so
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

class SDManager : public SDMAN_FS_BASE {
  public:
    bool ready = false;
  public:
    SDManager(FSImplPtr impl) : SDMAN_FS_BASE(impl) {}
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
