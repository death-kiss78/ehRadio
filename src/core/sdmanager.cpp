#include "options.h"
#if SD_CS!=255 // ============================== Everything ignored if not defined ==============================
#include <Arduino.h>
#include <SPI.h>
#include <vector>
#include <algorithm>
#include "vfs_api.h"
#if defined(SD_USE_MMC)
  #include "sdmmc_cmd.h"   // sdmmc_read_sectors() for the card-present probe below
#else
  #include <SD.h>
  #include "sd_diskio.h"
#endif
//#define USE_SD
#include "config.h"
#include "logging.h"
#include "sdmanager.h"  // pulls in <SD_MMC.h> (SDMMC transport) or <SD.h> (SPI transport)
#include "display.h"
#include "player.h"
#include "utility.h"
#include "../locale/dsplocale.h"

#if !defined(SD_USE_MMC)
// SPIB is declared and initialized in config.cpp (Config::init) — do not re-declare here.
// SD uses Bus B if assigned via SD_SPI 'B', otherwise Bus A.
#if defined(SD_SPI) && (SD_SPI == 'B') && defined(SPIB_SCK)
  #define SDREALSPI SPIB
#else
  #define SDREALSPI SPIA
#endif
#endif

SDManager sdman(fs::FSImplPtr(new VFSImpl()));

bool SDManager::start() {
  #if defined(SD_USE_MMC)
    // ---- Native SDMMC host (ESP32-S3 only; configured by the SDMMC_ block in options.h) ----
    // Pins must be set before the first begin(); setPins() is a no-op once the card is mounted.
    #if SDMMC_D1==255 || SDMMC_D2==255 || SDMMC_D3==255
      const bool sdmmc1bit = true;   // 1-bit: CLK, CMD, D0
      setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);
    #else
      const bool sdmmc1bit = false;  // 4-bit: CLK, CMD, D0..D3
      setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3);
    #endif
    #if SDMMC_FREQ > 0
      const int sdmmcFreq = SDMMC_FREQ;           // myoptions.h override
    #else
      const int sdmmcFreq = BOARD_MAX_SDMMC_FREQ; // driver default (40 MHz high speed)
    #endif
    ready = begin("/sdcard", sdmmc1bit, false, sdmmcFreq);
    if (ready) return ready;
    vTaskDelay(10);
    ready = begin("/sdcard", sdmmc1bit, false, sdmmcFreq);
    if (ready) return ready;
    vTaskDelay(20);
    ready = begin("/sdcard", sdmmc1bit, false, sdmmcFreq);
    if (ready) return ready;
    vTaskDelay(50);
    ready = begin("/sdcard", sdmmc1bit, false, sdmmcFreq);
    if (!ready) ERRORLOG("SDMMC mount failed");
    return ready;
  #else
    #if defined(SD_SPI) && (SD_SPI == 'B') && defined(SPIB_SCK) && defined(SPIB_SCK) && (SPIB_SCK != 255)
      SPIB.end();
      SPIB.begin(SPIB_SCK, SPIB_MISO, SPIB_MOSI);
    #elif defined(SPIA_SCK) && (SPIA_SCK != 255)
      SPI.end();
      SPI.begin(SPIA_SCK, SPIA_MISO, SPIA_MOSI);
    #endif
    ready = begin(SD_CS, SDREALSPI, SDSPISPEED);
    if (ready) return ready;
    vTaskDelay(10);
    ready = begin(SD_CS, SDREALSPI, SDSPISPEED);
    if (ready) return ready;
    vTaskDelay(20);
    ready = begin(SD_CS, SDREALSPI, SDSPISPEED);
    if (ready) return ready;
    vTaskDelay(50);
    ready = begin(SD_CS, SDREALSPI, SDSPISPEED);
    return ready;
  #endif
}

void SDManager::stop() {
  end();
  ready = false;
}
#if !defined(SD_USE_MMC)
  #include "diskio_impl.h"  // readRAW()/sectorSize() probe below is SPI-transport specific
#endif
bool SDManager::cardPresent() {
  if (!ready) return false;
#if defined(SD_USE_MMC)
  // Must be a real probe, matching what readRAW() does on the SPI side. cardSize() reads the cached CSD
  // out of the card descriptor, and _card stays set until end() - so it still reported a card after the
  // card was physically removed, and PR_CHECKSD therefore never fired on SDMMC builds. Reading a
  // physical sector through the host is the only way to see it go away. The read is non-destructive and
  // does not disturb FATFS's own cached window, since it bypasses the filesystem layer entirely.
  if (_card == nullptr) return false;
  uint8_t probe[512];
  return sdmmc_read_sectors(_card, probe, 0, 1) == ESP_OK;
#else
  if (sectorSize()<1) {
    return false;
  }
  uint8_t buff[sectorSize()] = { 0 };
  bool bread = readRAW(buff, 1);
  if (sectorSize()>0 && !bread) return false;
  return bread;
#endif
}

bool SDManager::_checkNoMedia(const char* path) {
  char nomedia[SD_PATH_LENGTH]= {0};
  strlcat(nomedia, path, SD_PATH_LENGTH);
  strlcat(nomedia, "/.nomedia", SD_PATH_LENGTH);
  bool nm = exists(nomedia);
  return nm;
}

bool SDManager::_endsWith (const char* base, const char* str) {
  const size_t slen = strlen(str);
  const char* end = base + strlen(base);                          // one past the last character
  while (end > base && isspace((unsigned char)end[-1])) end--;    // ignore trailing whitespace
  if ((size_t)(end - base) < slen) return false;
  return (strncmp(end - slen, str, slen) == 0);
}

bool SDManager::listSD(File &plSDfile, File &plSDindex, const char* dirname, uint8_t levels) {
  File root = sdman.open(dirname);
  if (!root) {
    ERRORLOG("Failed to open directory %s", dirname);
    return false;
  }
  if (!root.isDirectory()) {
    ERRORLOG("Not a directory: %s", dirname);
    return false;
  }

  // Collect all entries for sorting (dirs first, then alphanumeric by basename)
  struct DirEntry { String path; bool isDir; };
  std::vector<DirEntry> entries;
  while (true) {
    vTaskDelay(2);
    player.loop();
    bool isDir;
    String fileName = root.getNextFileName(&isDir);
    if (fileName.isEmpty()) break;
    entries.push_back({fileName, isDir});
  }
  root.close();

  // Sort: directories before files, both case-insensitive alphanumeric by basename
  std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
    if (a.isDir != b.isDir) return a.isDir;  // true (dir) > false (file)
    const char* an = strrchr(a.path.c_str(), '/');
    const char* bn = strrchr(b.path.c_str(), '/');
    an = an ? an + 1 : a.path.c_str();
    bn = bn ? bn + 1 : b.path.c_str();
    return strcasecmp(an, bn) < 0;
  });

  // Process sorted entries
  uint32_t pos = 0;
  bool ok = true;
  for (const auto& entry : entries) {
    sdFeedWatchdog();   // the index walk yields here already; this also feeds the watchdog if the caller is subscribed
    player.loop();
    char* filePath = (char*)malloc(entry.path.length() + 1);
    if (filePath == NULL) {
      ERRORLOG("Memory allocation failed");
      ok = false;
      break;
    }
    strcpy(filePath, entry.path.c_str());
    const char* fnSlash = strrchr(filePath, '/');
    const char* fn = fnSlash ? fnSlash + 1 : filePath;
    if (entry.isDir) {
      if (levels && !_checkNoMedia(filePath)) {
        if (!listSD(plSDfile, plSDindex, filePath, levels - 1)) ok = false;
      }
    } else {
      if (_endsWith(strlwr((char*)fn), ".mp3") || _endsWith(fn, ".m4a") || _endsWith(fn, ".aac") ||
          _endsWith(fn, ".wav") || _endsWith(fn, ".flac") || _endsWith(fn, ".ogg") ||
          _endsWith(fn, ".opus")) {
        pos = plSDfile.position();
        const size_t rowBody = (size_t)plSDfile.print(fn) + (size_t)plSDfile.print('\t') + (size_t)plSDfile.print(filePath);
        const size_t rowTail = plSDfile.write((const uint8_t*)"\t0\r\n", 4);
        const size_t idxWrote = plSDindex.write((uint8_t*)&pos, 4);
        if (rowTail != 4 || idxWrote != 4 || rowBody == 0) {
          // a write the card refused: the pair is short from here on, so say so rather than counting the file anyway
          ERRORLOG("SD write failed at %s", filePath);
          ok = false;
        } else {
          SERIALLOGDOT();
          if (display.mode()==SDCHANGE) display.putRequest(SDFILEINDEX, _sdFCount+1);
          _sdFCount++;
          if (_sdFCount % 64 == 0) SERIALLOGLF();
        }
      }
    }
    free(filePath);
    if (!ok) break;   // a failing card only produces more of the same
  }
  return ok;
}

void SDManager::indexSDPlaylist() {
  _sdFCount = 0;
  if (exists(PLAYLIST_SD_TMP_PATH)) remove(PLAYLIST_SD_TMP_PATH);
  if (exists(INDEX_SD_TMP_PATH)) remove(INDEX_SD_TMP_PATH);
  File playlist = open(PLAYLIST_SD_TMP_PATH, "w", true);
  if (!playlist) {
    return;
  }
  File index = open(INDEX_SD_TMP_PATH, "w", true);
  const bool walked = listSD(playlist, index, "/", SD_MAX_LEVELS);

  index.flush();                                 // size() is only accurate after a flush
  const size_t idxRows = index ? index.size() : 0;
  const bool complete = walked && playlist && index && idxRows == (size_t)_sdFCount * 4;

  if (!complete) {
    if (index) index.close();
    playlist.flush();
    playlist.close();
    remove(PLAYLIST_SD_TMP_PATH);
    remove(INDEX_SD_TMP_PATH);
    SERIALLOGLF();
    FUNCTIONLOG("SD", "indexing did not finish (%u files, %u index bytes): the partial pair was discarded, any previous pair is untouched",
                (unsigned)_sdFCount, (unsigned)idxRows);
    delay(50);
    return;
  }

  // Append footer: [magic:4][count:4] = 8 bytes
  // - magic = 0x1867 validates this is our format
  // - count = number of audio files found (staleness check)
  uint32_t magic = 0x1867;
  uint32_t fcount = _sdFCount;
  index.seek(index.size());
  index.write((uint8_t*)&magic, 4);
  index.write((uint8_t*)&fcount, 4);
  index.close();

  playlist.flush();
  playlist.close();

  if (exists(PLAYLIST_SD_PATH)) remove(PLAYLIST_SD_PATH);
  if (exists(INDEX_SD_PATH)) remove(INDEX_SD_PATH);
  bool swapped = rename(PLAYLIST_SD_TMP_PATH, PLAYLIST_SD_PATH);
  swapped = rename(INDEX_SD_TMP_PATH, INDEX_SD_PATH) && swapped;
  if (!swapped) {
    ERRORLOG("could not move the new SD playlist and index into place");
  }
  SERIALLOGLF();
  delay(50);
}

uint32_t SDManager::countAudioFiles() {
  _sdFCount = 0;
  _countAudioFilesRecursive("/", SD_MAX_LEVELS);
  return _sdFCount;
}

uint32_t SDManager::_countAudioFilesRecursive(const char* dirname, uint8_t levels) {
  File root = sdman.open(dirname);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  while (true) {
    sdFeedWatchdog();
    bool isDir;
    String fileName = root.getNextFileName(&isDir);
    if (fileName.isEmpty()) break;

    char* filePath = (char*)malloc(fileName.length() + 1);
    if (!filePath) break;
    strcpy(filePath, fileName.c_str());
    const char* fnSlash = strrchr(filePath, '/');
    const char* fn = fnSlash ? fnSlash + 1 : filePath;

    if (isDir) {
      if (levels && !_checkNoMedia(filePath)) {
        _countAudioFilesRecursive(filePath, levels - 1);
      }
    } else {
      if (_endsWith(strlwr((char*)fn), ".mp3") || _endsWith(fn, ".m4a") || _endsWith(fn, ".aac") ||
          _endsWith(fn, ".wav") || _endsWith(fn, ".flac") || _endsWith(fn, ".ogg") ||
          _endsWith(fn, ".opus")) {
        _sdFCount++;
      }
    }
    free(filePath);
  }
  root.close();
  return 0;
}

void SDManager::trySdRemount() {
  if (ready) return;  // already mounted
  FUNCTIONLOG("SD", "Remount attempt...");
  display.putRequest(NEWMODE, SDCHANGE);
  if (start()) {
    config.initSDPlaylist();
    player.setReady();                    // the one implementation of the mode-entry text: see player.h
    display.putRequest(NEWMODE, PLAYER);
    display.putRequest(NEWSTATION);
  } else {
    display.putRequest(NEWMODE, PLAYER);  // restore from SDCHANGE
    player.setReady();                    // no card mounted: the same helper, which knows that variant
  }
}
#endif


