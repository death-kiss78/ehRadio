#include "core/options.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>   // heap_caps_get_largest_free_block, for the [PSRAM] contiguous figure
#include "core/battery.h"
#include "core/backlightcontrols.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/display.h"
#include "core/logging.h"
#include "core/mqtt.h"
#include "core/netserver.h"
#include "core/network.h"
#include "core/player.h"
#include "core/rgbled.h"
#include "core/sdmanager.h"
#include "core/startup.h"
#include "core/telnet.h"
#include "displays/tools/psframebuffer.h"

SET_LOOP_TASK_STACK_SIZE(LOOP_TASK_STACK_SIZE * 1024);

/* PSRAM usage tracking — set by subsystems, consumed by Core Monitor */
size_t psramFrameBufferBytes = 0;

#ifdef CORE_MONITOR
  extern volatile uint32_t cmDspLoopCount;
  extern TaskHandle_t dspTaskHandle;
  extern TaskHandle_t nsTaskHandle;
  static uint32_t cmMainCount     = 0;
  static uint32_t cmMaxMainLoop   = 0;
  static uint32_t cmLoopStart     = 0;
  static unsigned long cmLastPrint = 0;
  static uint8_t cmEtcCount      = 0;  // CORE_MONITOR_ETC_LOOPS counter
#endif

void setup() {
  Serial.begin(115200);
  #if (CORE_DEBUG_LEVEL > 0) || defined(ALL_DEBUG_LOGS)
    if (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_EXT) { // checking if this is a poweron boot
      delay(1000);
      BOOTLOG("1 second delay after cold boot to ensure serial logs are available (CORE_DEBUG_LEVEL > 0 or ALL_DEBUG_LOGS)...");
    }
  #endif

  startup.deassertCsPins();
  if (LED_PIN!=255) pinMode(LED_PIN, OUTPUT);
  rgbled.init();
  battery.init();
  config.init();
  controls.checkButtonsHeldOnBoot();  // check for hold-to-SD before network decision
  backlightControls.init();
  display.init();
  startup.checkSpiffsandVer();
  player.init();
  battery.bootStatus();
  if ((network.offlineMode || config.store.SDoffline)) {
    startup.sdOfflineMode();
  } else {
    startup.checkSafeMode();
    network.begin();
  }
  if (network.status != CONNECTED && network.status != SDOFFLINE) {
    netserver.begin();
    netserver.startLoopTask();
    controls.init();
    display.putRequest(DSP_START);
    while(!display.ready()) delay(10);
    netserver.setBootReady(true);
    return;
  }
  startup.getDefaultPlaylist();
  if (SD_CS!=255 && config.store.play_mode==PM_SDCARD) {
    display.putRequest(WAITFORSD, 0);
    BOOTLOG("SD Search");
  }
  startup.cleanStaleSearchResults();
  config.initPlaylistMode();
  netserver.begin();
  if (network.status != SDOFFLINE) {
    netserver.startLoopTask();
    telnet.begin();
  }
  controls.init();
  display.putRequest(DSP_START);
  while(!display.ready()) delay(10);
  #ifdef MQTT_ENABLE
    if (config.store.mqttenable && network.status != SDOFFLINE) mqtt.init();
  #endif
  #if LED_INVERT
    if (LED_PIN!=255) digitalWrite(LED_PIN, true);
  #endif
  if (config.getMode()==PM_SDCARD) player.initHeaders(config.station.url);
  player.lockOutput=false;
  if (config.store.smartstart) {  // If smart start is enabled
    delay(1000);  // Allow DNS/TCP/SSL stack to stabilize after WiFi connect (esp. after soft restart)
    if (config.getMode() == PM_WEB) {
      player.resumeLastWebSource();
    } else {
      uint16_t stn = config.lastStation();
      if (stn > 0) {  // Only play if there's a valid station
        player.sendCommand({PR_PLAY, stn});
      }
    }
  }
  if (network.status != SDOFFLINE) startup.startupServices();  // needs WiFi — skip in offline SD mode
  netserver.setBootReady(true);
  config.saveValue(&config.store.SDoffline, false);
}

void loop() {
  #ifdef CORE_MONITOR
    cmLoopStart = micros();
  #endif
  // Stage attribution, always on: an iteration over MAIN_LOOP_STALL_MS names the blocking stage, which is
  // the whole diagnosis (this is what found the stream connect in the player stage). */
  const uint32_t tStage0 = micros();

  if (network.status == SOFT_AP) {
    network.loopImprov();
    if (network.dnsServer) network.dnsServer->processNextRequest();
  } else {
    telnet.loop();
  }
  const uint32_t tStage1 = micros();

  rgbled.loop();
  battery.loop();
  const uint32_t tStage2 = micros();

  controls.loop();
  const uint32_t tStage3 = micros();

  if (network.status == CONNECTED || network.status == SDOFFLINE) {
    player.loop();
    config.processDeferredSaves();
  }
  const uint32_t tStage4 = micros();

  startup.loop();
  const uint32_t tStage5 = micros();

  {
    const uint32_t total = tStage5 - tStage0;
    if (total > ((uint32_t)MAIN_LOOP_STALL_MS * 1000UL)) {
      const uint32_t staged[5] = {
        tStage1 - tStage0,   // net: improv or telnet
        tStage2 - tStage1,   // rgbled + battery
        tStage3 - tStage2,   // controls
        tStage4 - tStage3,   // player + deferred saves
        tStage5 - tStage4,   // startup
      };
      static const char* const stageNames[5] = {"net/telnet", "rgbled+battery", "controls", "player+saves", "startup"};
      uint8_t worst = 0;
      for (uint8_t i = 1; i < 5; i++) if (staged[i] > staged[worst]) worst = i;
      ERRORLOG("Main loop stalled %lums, worst stage %s %lums - net %lu, hw %lu, ctl %lu, player %lu, startup %lu",
          (unsigned long)(total / 1000UL), stageNames[worst], (unsigned long)(staged[worst] / 1000UL),
          (unsigned long)(staged[0] / 1000UL), (unsigned long)(staged[1] / 1000UL),
          (unsigned long)(staged[2] / 1000UL), (unsigned long)(staged[3] / 1000UL),
          (unsigned long)(staged[4] / 1000UL));
    }
  }

  #ifdef CORE_MONITOR
    cmMainCount++;
    uint32_t cmDur = micros() - cmLoopStart;
    if (cmDur > cmMaxMainLoop) cmMaxMainLoop = cmDur;
    if (millis() - cmLastPrint >= 5000) {
      uint32_t d = cmDspLoopCount;  cmDspLoopCount = 0;
      uint32_t m = cmMainCount;     cmMainCount = 0;
      uint32_t mx = cmMaxMainLoop;  cmMaxMainLoop = 0;
      #ifdef CONFIG_FREERTOS_UNICORE
        FUNCTIONLOG("Core.monitor", "Core0 loops/s: %u (%.2fms/loop), Core0(Main) loops/s: %u (%.2fms/loop), Max Main Loop Time: %.3fms, Free Heap: %u",
            d/5, d>0 ? 5000.0f/d : 0.0f, m/5, m>0 ? 5000.0f/m : 0.0f, mx / 1000.0f, (unsigned)ESP.getFreeHeap());
      #else
        FUNCTIONLOG("Core.monitor", "Core0" CORE_0 " loops/s: %u (%.2fms/loop), Core1" CORE_1 " loops/s: %u (%.2fms/loop), Max Main Loop Time: %.3fms, Free Heap: %u",
            d/5, d>0 ? 5000.0f/d : 0.0f, m/5, m>0 ? 5000.0f/m : 0.0f, mx / 1000.0f, (unsigned)ESP.getFreeHeap());
      #endif
      FUNCTIONLOG("Core.monitor", "High Water Mark (free bytes in stacks): Main: %u, Display: %u, Netserver: %u",
          (unsigned)uxTaskGetStackHighWaterMark(NULL),
          (unsigned)(dspTaskHandle ? uxTaskGetStackHighWaterMark(dspTaskHandle) : 0),
          (unsigned)(nsTaskHandle  ? uxTaskGetStackHighWaterMark(nsTaskHandle)  : 0));
      // SPIFFS + PSRAM info — rate-limited by CORE_MONITOR_ETC_LOOPS
      if (++cmEtcCount >= CORE_MONITOR_ETC_LOOPS) {
        cmEtcCount = 0;
        FUNCTIONLOG("SPIFFS", "Used: %u / %u bytes, Free: %u bytes", SPIFFS.usedBytes(), SPIFFS.totalBytes(), SPIFFS.totalBytes() - SPIFFS.usedBytes());
        // Internal free and the largest CONTIGUOUS block - the figure a TLS handshake actually needs
        FUNCTIONLOG("Heap", "Internal: %uKB free, %uKB largest block",
            ESP.getFreeHeap() / 1024,
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
        if (psramFound()) {
          size_t psramTotal = ESP.getPsramSize();
          size_t psramUsed  = psramTotal - ESP.getFreePsram();
          size_t audioFill  = player.inBufferFilled();
          FUNCTIONLOG("PSRAM", "Used: %uKB / %uKB: Framebuffer: %uKB, VU FFT: %uKB, WebUI Cache: %uKB, Audio buffered: %uKB, Contiguous Free: %uKB",
              psramUsed / 1024,
              psramTotal / 1024,
              psramFrameBufferBytes / 1024,
              vuPsramBytes / 1024,
              netserver.getFileCache().totalBytes() / 1024,
              audioFill / 1024,
              heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
        }
      }
      cmLastPrint = millis();
    }
  #endif
}


