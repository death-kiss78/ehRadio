#include "filemanager.h"

#ifdef USE_SD

#include <FS.h>
#include <errno.h>
#include <vector>
#include "sdmanager.h"
#include "config.h"
#include "display.h"
#include "netserver.h"
#include "player.h"
#include "logging.h"

FileManager filemanager;

// ==== Path helpers ====

// The FS wants a rooted path with no trailing slash and the UI can send either form. FATFS treats "/Music"
// and "Music/" as one file, but every protection here is a string comparison, so the two spellings would
// give two different answers. Normalise at the edge.
static String normalisePath(const String &raw) {
  String p = raw;
  p.trim();
  if (p.length() == 0) return String("/");
  if (!p.startsWith("/")) p = "/" + p;
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  return p;
}

// One path component: no separators and no "."/"..", so it cannot escape the folder the user is looking at.
// Upload names and rename targets pass through here - the browser is not a security boundary.
static bool isSafeName(const String &name) {
  if (name.length() == 0 || name.length() > 200) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (name == "." || name == "..") return false;
  return true;
}

static String basenameOf(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash >= 0) return path.substring(slash + 1);
  return path;
}

static String parentOf(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash < 0) return String("/");
  if (slash == 0) return String("/");
  return path.substring(0, slash);
}

// Query-string parameters are parsed with the request line, so unlike a body they are ready in the handler.
static String argOf(AsyncWebServerRequest *request, const char *name) {
  if (!request->hasParam(name, false)) return String();
  return request->getParam(name, false)->value();
}

// Every JSON answer is state as of that request, so none of it may be cached - a stored listing would show
// the folder as it was before the change. netserver.cpp's locale and visuals handlers send the same headers.
static void sendJson(AsyncWebServerRequest *request, int code, const String &body) {
  AsyncWebServerResponse *response = request->beginResponse(code, "application/json", body);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
}

static void sendOk(AsyncWebServerRequest *request) {
  sendJson(request, 200, F("{\"ok\":true}"));
}

// `landed` is the byte count the device can VOUCH FOR in the target file, and it is sent only when there is one.
// It is the whole of the resume protocol on the device's side: the page takes it and re-sends only the remainder.
static void sendError(AsyncWebServerRequest *request, int code, const char *reason, uint64_t landed = 0) {
  String body = F("{\"ok\":false,\"error\":\"");
  body += reason;
  body += F("\"");
  if (landed) {
    char num[24];
    snprintf(num, sizeof(num), "%llu", (unsigned long long)landed);
    body += F(",\"landed\":");
    body += num;
  }
  body += F("}");
  sendJson(request, code, body);
}

// Escapes into a caller-owned buffer and NUL-terminates. A filename off the card is user data: one unescaped
// quote would break the response, and control characters go out as \u00XX.
static int jsonEscapeTo(char *dst, size_t room, const char *src) {
  size_t n = 0;
  for (const char *p = src; *p && n + 7 < room; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':  dst[n++] = '\\'; dst[n++] = '"';  break;
      case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
      case '\n': dst[n++] = '\\'; dst[n++] = 'n';  break;
      case '\r': dst[n++] = '\\'; dst[n++] = 'r';  break;
      case '\t': dst[n++] = '\\'; dst[n++] = 't';  break;
      default:
        if (c < 0x20) n += snprintf(dst + n, room - n, "\\u%04x", (unsigned)c);
        else          dst[n++] = (char)c;
    }
  }
  dst[n] = '\0';
  return (int)n;
}

// Every route except the two entry points needs the mode open; without it a page left in a tab would browse
// the card with playback unblocked, which is the one thing the mode exists to prevent.
static bool requireActive(AsyncWebServerRequest *request) {
  if (filemanager.active()) return true;
  sendError(request, 409, "not_active");
  return false;
}

// ==== Guards ====

bool FileManager::isProtected(const String &path) {
  /* ONLY the data folder, and only under its current name.  Two things are deliberately NOT protected here:
     - THE ROOT.  It never needed protecting for its own sake: a new folder at the root and a rename there are both
       fine, and the one destructive operation that must be refused - deleting the root - is refused by name in
       hDelete(), which is where that rule belongs.  `isProtected("/")` had been the only thing standing between the
       UI and a recursive wipe of the whole card, so removing it here REQUIRED adding it there.
     - `/data`, THE OLD NAME.  That is the point of the rename: a card indexed under the old name now keeps that
       folder as ordinary, deletable clutter, and the user can clear it from the UI.  Nothing reads it any more. */
  if (path == SD_DATA_DIR || path.startsWith(String(SD_DATA_DIR) + "/")) return true;
  return false;
}

bool FileManager::isPlaying(const String &path) const {
  if (config.getMode() != PM_SDCARD) return false;
  // A backstop rather than the normal protection: enter() stops the player, so nothing is usually in use.
  if (!player.isRunning()) return false;
  String playing = config.station.url;
  if (playing.length() == 0) return false;
  if (playing.equalsIgnoreCase(path)) return true;
  // A directory holding the playing file counts, so the next character must be the separator - "/Music" must
  // not match "/Music2". FATFS would unlink the entry and leave the decoder on a dead cluster chain.
  if (path.length() > 1 && playing.length() > path.length() + 1 &&
      playing.charAt(path.length()) == '/' &&
      playing.substring(0, path.length()).equalsIgnoreCase(path)) return true;
  return false;
}

// A mutation makes these wrong:
//   - the pair of derived files: playlistsd.csv (the rows) and indexsd.dat (one offset per row, plus a footer
//     count).  They are one object in two files and are always dropped together - see dropSdDerivedFiles() - and
//     config.initSDPlaylist() rebuilds them.  Files, not RAM bits, so they survive a power cycle.  Dropping only
//     the index is how a valid index came to sit beside a truncated playlist, which SD mode then read as an empty
//     card; the index must go for the rebuild to trigger at all, since its absence is what that test looks for.
//   - the device's in-RAM SD playlist, which the player and the WebUI read: it stays stale until something
//     rebuilds it.  Nothing rebuilds it here, and nothing rebuilds it while the manager is open: a session of
//     many operations pays for one walk, done by FileManager::loop() after the mode closes.  The bit also
//     suppresses the resume, because a station NUMBER means a different file once the list has changed.
static void dropSdDerivedFiles() {
  bool dropped = false;
  if (sdman.exists(INDEX_SD_PATH))    { sdman.remove(INDEX_SD_PATH);    dropped = true; }
  if (sdman.exists(PLAYLIST_SD_PATH)) { sdman.remove(PLAYLIST_SD_PATH); dropped = true; }
  // A build that was interrupted leaves its half-built pair under the temporary names, and nothing else would ever
  // clear them - the next build removes them again, but only if there is a next build.  Sweep them with the pair.
  if (sdman.exists(INDEX_SD_TMP_PATH))    sdman.remove(INDEX_SD_TMP_PATH);
  if (sdman.exists(PLAYLIST_SD_TMP_PATH)) sdman.remove(PLAYLIST_SD_TMP_PATH);
  if (dropped) FUNCTIONLOG("SDFileManager", "SD playlist and index dropped; they will be rebuilt when the mode closes");
  filemanager.markCardChanged();
}

// ==== The SPI upload marker ====

/* A card that has PROVED it cannot be uploaded to through an SPI reader can say so itself, and this is where that is
   read.  The file lives in the SD data folder and its two lines are plain English on purpose - it is a card artefact
   a person may open in a text editor on a PC, not UI; the MESSAGE the firmware shows is a locale key.
   IT BLOCKS UPLOADS.  While it is present, onUploadChunk refuses every upload with `spi_blocked` - that is the real
   guard, and the page's dimmed buttons are only its visible half, so a hand-made request cannot write to a card that
   has already failed this way.
   NOTHING OF IT EXISTS ON SDMMC: the whole feature is compiled out, because the failure it records is the SPI path's
   and an SDMMC card must never be judged by it. */
#define SDMAN_SPI_MARKER SD_DATA_DIR "/spi_upload.txt"

static bool spiUploadBlocked() {
  #if defined(SD_USE_MMC)
    return false;
  #else
    return sdman.exists(SDMAN_SPI_MARKER);
  #endif
}

// ==== Directory removal ====

static bool removeRecursive(const String &path) {
  File entry = sdman.open(path);
  if (!entry) return false;

  if (!entry.isDirectory()) {
    entry.close();
    return sdman.remove(path);
  }

  // Names are collected first: deleting while walking advances the directory position under the removal, so
  // the walk skips entries and can stop early, leaving a partial tree behind.
  std::vector<String> children;
  File child = entry.openNextFile();
  while (child) {
    sdFeedWatchdog();  // a delete walk costs as much as the listing walk - unfed, the task WDT would abort the device
    filemanager.touch();  // and it takes minutes: unfed, the idle timeout would close the mode mid-delete
    children.push_back(basenameOf(child.name()));  // name() is the full path on ESP32 Arduino
    child = entry.openNextFile();
  }
  entry.close();

  String base = path;
  if (!base.endsWith("/")) base += "/";
  for (const String &name : children) {
    sdFeedWatchdog();  // each call removes files or opens another directory
    filemanager.touch();  // same clock, same reason
    if (!removeRecursive(base + name)) return false;
  }
  return sdman.rmdir(path);
}

// ==== Handlers ====

// GET /sdman/enter - opens the mode and is the only way in: the page calls it as it loads, so /sdmanager.html
// works as well as the address on the display. Idempotent; a second call just refreshes the idle clock.
// No route at bare "/sdman": a plain URI matches an exact path OR a prefix plus "/", so a handler there
// would also answer /sdman/list and every sibling. No network test either - entry used to need CONNECTED,
// which shut the feature out of AP mode, and _switchMode() already refuses mode changes unless the status is
// CONNECTED or SDOFFLINE. SD-offline needs no test because NetServer::begin() returns before the server.
static void hEnterApi(AsyncWebServerRequest *request) {
  filemanager.enter();
  sendOk(request);
}

// POST /sdman/done - the Done button at the foot of the page.
static void hDone(AsyncWebServerRequest *request) {
  // Refused while a file is mid-write: leave() removes the half-written file on purpose (that is what a close
  // means), so a Done press from another tab or another device would throw away an upload nobody finished
  // watching.  Between the files of a batch _upFile is already closed, so this only stops a close that would
  // destroy something.
  if (filemanager.uploadOpen()) {
    // Deliberately inert: no touch(), so a Done press cannot be used to keep a stalled upload's mode alive.  The
    // line exists so a refusal - a second tab, a second device - is visible in the log rather than silent.
    FUNCTIONLOG("SDFileManager", "Done refused, upload in progress");
    sendError(request, 409, "uploading");
    return;
  }
  // Same rule for a delete batch, and the same reason to be inert: the page's own buttons are already dimmed and
  // inert while one runs (see busy() in sdmanager.html), so what arrives here is a second tab or a second device.
  if (filemanager.busy()) {
    FUNCTIONLOG("SDFileManager", "Done refused, a delete is in progress");
    sendError(request, 409, "busy");
    return;
  }
  // Named here, because leave() cannot tell a Done press from its own timeout: until this line existed, a close
  // the user asked for and a close the countdown asked for were identical in the log.
  filemanager.leave(true, "Done pressed");
  request->redirect("/");
}

/* Defined with the upload state it reports, further down this file - where the transfer's statics live - and
   declared here because it is a shape of the one request the page makes about that state.  Its comment explains
   why it is not simply part of the /sdman/info body below. */
static void sendUploadState(AsyncWebServerRequest *request);

// GET /sdman/info - the header line and the empty/error states depend on this.
static void hInfo(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  // The transfer-in-flight question goes to its own function, which lives with the state it reports.
  if (argOf(request, "up") == "1") {
    sendUploadState(request);
    return;
  }
  // One walk of the FAT, not two.  usedBytes() counts the free clusters, which means walking the whole FAT; asking
  // for totalBytes() as well runs that same walk a second time for a total we already have, because cardSize() reads
  // the CSD the card reported at init and costs nothing.  On a large card the duplicate is seconds of blocked
  // network task, and this is the handler the page calls on every load and every click.
  const uint32_t t0 = millis();
  const uint64_t used = sdman.usedBytes();
  sdFeedWatchdog();
  const uint64_t total = sdman.cardSize();
  FUNCTIONLOG("SDFileManager", "Figures: used %llu of %llu, %lu ms",
              (unsigned long long)used, (unsigned long long)total, (unsigned long)(millis() - t0));
  // The idle deadline goes out with the card figures, so the page can end itself when the device does.
  String body = F("{\"ok\":true,\"idle\":");
  body += String((unsigned long)filemanager.idleRemainingMs());
  body += F(",\"mounted\":");
  body += sdman.ready ? "true" : "false";
  body += F(",\"total\":");
  body += String((unsigned long)total);
  body += F(",\"used\":");
  body += String((unsigned long)used);
  body += F(",\"free\":");
  body += String((unsigned long)(total >= used ? total - used : 0));
  /* Whether this card has been marked unusable for uploads over an SPI reader, and which transport this build uses.
     The page needs both: the first locks its Upload buttons, and the second decides whether a card-side failure may
     WRITE the marker - an SDMMC card must never be marked, because the fault cannot be its reader. */
  body += F(",\"spi\":");
  #if defined(SD_USE_MMC)
    body += F("false");
  #else
    body += F("true");
  #endif
  body += F(",\"spimarker\":");
  body += spiUploadBlocked() ? "true" : "false";
  /* The allocation unit, so the PAGE can decide whether the remedy sentence applies to this card: the advice is
     "format with 512-byte allocation units", which is nonsense on a card already formatted that way.  0 = unknown,
     which the page treats as "say nothing extra". */
  body += F(",\"au\":");
  body += String((unsigned long)sdman.allocationUnit());
  body += F("}");
  sendJson(request, 200, body);
}

// POST /sdman/mkdir?path=/Music/New
static void hMkdir(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  String path = normalisePath(argOf(request, "path"));
  if (path == "/") { sendError(request, 400, "bad_path"); return; }
  if (FileManager::isProtected(path)) { sendError(request, 403, "protected"); return; }
  String parent = parentOf(path);
  if (!sdman.exists(parent)) { sendError(request, 404, "no_parent"); return; }
  if (sdman.exists(path)) { sendError(request, 409, "exists"); return; }
  if (!isSafeName(basenameOf(path))) { sendError(request, 400, "bad_name"); return; }
  if (!sdman.mkdir(path)) { sendError(request, 500, "mkdir_failed"); return; }
  FUNCTIONLOG("SDFileManager", "Mkdir %s", path.c_str());
  dropSdDerivedFiles();
  sendOk(request);
}

// POST /sdman/rename?from=/a.mp3&to=b.mp3 - same directory
// POST /sdman/move?from=/a.mp3&to=/Music/a.mp3 - possibly another directory
static void doRename(AsyncWebServerRequest *request, bool sameDirectory) {
  if (!requireActive(request)) return;
  filemanager.touch();
  String from = normalisePath(argOf(request, "from"));
  String to   = argOf(request, "to");
  to.trim();

  if (from == "/" || from.length() < 2) { sendError(request, 400, "bad_source"); return; }
  if (to.length() == 0) { sendError(request, 400, "bad_target"); return; }
  if (FileManager::isProtected(from)) { sendError(request, 403, "protected"); return; }
  if (filemanager.isPlaying(from)) { sendError(request, 409, "in_use"); return; }
  if (!sdman.exists(from)) { sendError(request, 404, "not_found"); return; }

  // Built through the parent: parentOf() answers "/" at the root, so a blind "/" + "/" + name would give
  // "//name" - a doubled separator FATFS may reject and no other path here has.
  String parent = parentOf(from);
  String target = sameDirectory ? ((parent == "/") ? ("/" + to) : (parent + "/" + to)) : to;
  target = normalisePath(target);

  if (!sameDirectory && FileManager::isProtected(target)) { sendError(request, 403, "protected"); return; }
  if (!isSafeName(basenameOf(target))) { sendError(request, 400, "bad_name"); return; }
  if (target == from) { sendOk(request); return; }   // a rename to itself is a no-op, not a failure
  if (!sdman.exists(parentOf(target))) { sendError(request, 404, "no_parent"); return; }
  if (sdman.exists(target)) { sendError(request, 409, "exists"); return; }
  if (!sdman.rename(from, target)) { sendError(request, 500, "rename_failed"); return; }

  FUNCTIONLOG("SDFileManager", "%s %s -> %s", sameDirectory ? "Renamed" : "Moved", from.c_str(), target.c_str());
  dropSdDerivedFiles();
  sendOk(request);
}

static void hRename(AsyncWebServerRequest *request) { doRename(request, true); }
static void hMove(AsyncWebServerRequest *request)   { doRename(request, false); }

// POST /sdman/delete - the whole selection as one newline-separated body: a page is 50 rows, the handler
// runs once the body is complete, and one request means one reload and one place to decide what is allowed.
static String _deleteBody;

static void onDeleteBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) _deleteBody = "";
  _deleteBody.concat((const char *)data, len);
}

static void hDelete(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  // The whole selection is handled inside this one request, so this is the operation the mode cannot be closed in
  // the middle of: leave() would tear it down and leave the card's derived files half-written.
  filemanager.markBusy(true);
  int deleted = 0, failed = 0;

  int start = 0;
  while (start < (int)_deleteBody.length()) {
    // The selection is N FATFS deletes in one handler, and a *file* never reaches the fed loops inside
    // removeRecursive() - it returns at that function's isDirectory() branch - so the batch loop is the only
    // place a many-file delete (a whole card) can be fed.  Without it the task WDT aborts mid-write, which also
    // leaves the mount dirty for the next boot.  A tick per item lets the display keep up as well.
    // THE PLAYER QUEUE IS DRAINED HERE FOR THE SAME REASON, and listSD() already does it: the main loop is the
    // only OTHER drain and it is gated on the network status, so through a long burst the ~1-per-2-seconds ticks
    // from ticks() (PR_CHECKSD, PR_VUTONUS) had nowhere to go and the 10-slot queue filled - the field logged
    // "playerQueue overflow, dropped cmd=7" on a whole-card delete.  A dropped tick is harmless and is re-issued;
    // the drain is here because a dropped PR_PLAY or PR_STOP would not be.  One receive per item, and the
    // commands execute in this task exactly as they already do from the index walk.
    sdFeedWatchdog();
    filemanager.touch();
    player.loop();
    int nl = _deleteBody.indexOf('\n', start);
    if (nl < 0) nl = _deleteBody.length();
    String raw = _deleteBody.substring(start, nl);
    start = nl + 1;
    raw.trim();
    if (raw.length() == 0) continue;

    String path = normalisePath(raw);
    /* THE ROOT IS NOT DELETABLE, and this is now the ONLY place that says so.  isProtected() used to carry the rule
       and it was the sole guard against a recursive wipe of the entire card; it was narrowed to the data folder, so
       the guard had to come here with it.  Everything else at the root - a new folder, a rename, an upload - is
       allowed exactly as before. */
    if (path == "/") {
      FUNCTIONLOG("SDFileManager", "Refused to delete the card root");
      failed++;
      continue;
    }
    // One count on the way out, but the log keeps them apart: a protected item and a failed rmdir differ.
    if (FileManager::isProtected(path) || filemanager.isPlaying(path)) {
      FUNCTIONLOG("SDFileManager", "Refused to delete %s (protected or in use)", path.c_str());
      failed++;
      continue;
    }
    if (!sdman.exists(path)) {
      FUNCTIONLOG("SDFileManager", "Nothing to delete at %s", path.c_str());
      failed++;
      continue;
    }
    if (removeRecursive(path)) {
      deleted++;
      FUNCTIONLOG("SDFileManager", "Deleted %s", path.c_str());
    } else {
      failed++;
      FUNCTIONLOG("SDFileManager", "Could not delete %s", path.c_str());
    }
  }
  _deleteBody = "";
  filemanager.markBusy(false);   // the long part is over: the invalidation and the answer are both quick

  if (deleted) dropSdDerivedFiles();

  String body = F("{\"ok\":");
  body += (failed == 0) ? "true" : "false";
  body += F(",\"deleted\":");
  body += deleted;
  body += F(",\"failed\":");
  body += failed;
  body += F("}");
  sendJson(request, 200, body);
}

// GET /sdman/download?path=/Music/a.mp3 - the row's download icon. The response carries the open File, so
// the transfer reads straight off the card with no buffer of our own.
static void hDownload(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  String path = normalisePath(argOf(request, "path"));
  if (!sdman.exists(path)) { sendError(request, 404, "not_found"); return; }
  File file = sdman.open(path, FILE_READ);
  if (!file) { sendError(request, 404, "not_found"); return; }
  bool isDir = file.isDirectory();
  file.close();
  if (isDir) { sendError(request, 400, "is_dir"); return; }
  // The download flag adds Content-Disposition, so the browser saves rather than renders the file.
  request->send(request->beginResponse(sdman, path, "application/octet-stream", true));
}

// ==== Listing ====

// The walk hands over one entry at a time, but the send window decides how much a call may emit and it
// shrinks as data queues. So pending bytes sit in _listOut and drain a windowful at a time, which bounds
// memory to one entry and, more to the point, cannot cut the document short the way writing a whole entry
// per call could.
// One listing at a time, as the playlist export assumes: the UI loads a folder and waits, and the browser
// does the sorting and the paging.
static File   _listDir;
static String _listPath;
static String _listOut;
static bool   _listStarted = false;
static bool   _listFirst   = true;
static bool   _listDone    = false;
/* How long a listing may hold the card before a new request may declare it abandoned and take the handle over.
   A live walk finishes in a second or two of network time, so this only ever catches a client that left. */
static constexpr uint32_t SDMAN_LIST_ABANDON_MS = 5000;
static uint32_t _listT0    = 0;   // when this walk started, for the cost line
static uint32_t _listCount = 0;   // entries emitted, for the same

static size_t sdmanListFiller(uint8_t *buffer, size_t maxLen, size_t index) {
  if (maxLen == 0) return 0;

  if (_listOut.length() == 0) {
    if (_listDone) return 0;   // the footer has gone out; this is what ends the response

    if (!_listStarted) {
      char esc[264];
      jsonEscapeTo(esc, sizeof(esc), _listPath.c_str());
      char head[48];
      snprintf(head, sizeof(head), "{\"ok\":true,\"idle\":%lu,\"path\":\"",
               (unsigned long)filemanager.idleRemainingMs());
      _listStarted = true;
      _listOut  = head;
      _listOut += esc;
      _listOut += F("\",\"entries\":[");
    } else if (_listDir) {
      // One entry per call, but the calls come back to back while the socket window is open, so a large folder
      // turns into seconds of blocked network task here - which is why the watchdog is fed every few entries.
      if ((_listCount & 0x07) == 0) sdFeedWatchdog();
      File entry = _listDir.openNextFile();
      if (entry) {
        String name = basenameOf(entry.name());   // name() is the full path on ESP32 Arduino
        const bool isDir = entry.isDirectory();
        const size_t size = isDir ? 0 : entry.size();
        entry.close();
        char esc[264];
        jsonEscapeTo(esc, sizeof(esc), name.c_str());
        char tail[48];
        snprintf(tail, sizeof(tail), "\",\"d\":%u,\"s\":%lu}", isDir ? 1u : 0u, (unsigned long)size);
        _listOut  = _listFirst ? "" : ",";
        _listOut += F("{\"n\":\"");
        _listOut += esc;
        _listOut += tail;
        _listFirst = false;
        _listCount++;
      } else {
        _listDir.close();   // the walk is over
        _listOut = F("]}");
        _listDone = true;
        FUNCTIONLOG("SDFileManager", "Walked %u entries in %lu ms",
                    (unsigned)_listCount, (unsigned long)(millis() - _listT0));
      }
    } else {
      _listOut = F("]}");
      _listDone = true;
    }
  }

  size_t n = _listOut.length();
  if (n > maxLen) n = maxLen;
  memcpy(buffer, _listOut.c_str(), n);
  _listOut.remove(0, n);
  return n;
}

// GET /sdman/list?path=/Music
static void hList(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  if (!sdman.ready) { sendError(request, 409, "no_card"); return; }
  /* One walker at a time, and never while a file is being written.  A listing is chunked and keeps its directory
     handle in _listDir between callbacks, so a second listing used to close the handle the first one was reading
     from and both came back partial - with a second tab open that is the normal case, not an edge one, and the walk
     times in the log (7-14 s for a hundred entries) are what that contention looked like.  Walking while an upload
     writes is the same hazard from the other side: on an SPI card the two together drop connections.  The page
     retries both of these quietly rather than showing an error. */
  if (filemanager.uploadOpen()) { sendError(request, 409, "uploading"); return; }
  if (_listDir) {
    // A listing that was ABANDONED (the client went away, the page reloaded) leaves its handle open with no more
    // callbacks coming, and this is the only place that can close it - without it the handles accumulate until
    // sdman.open() fails and every listing comes back empty or partial.  So the refusal is time-limited rather
    // than absolute: past the window the stale handle is closed and the new request is served.
    if (!_listDone && (millis() - _listT0) < SDMAN_LIST_ABANDON_MS) { sendError(request, 409, "busy"); return; }
    _listDir.close();
  }
  String path = normalisePath(argOf(request, "path"));
  if (!sdman.exists(path)) { sendError(request, 404, "not_found"); return; }
  _listDir = sdman.open(path);
  if (!_listDir || !_listDir.isDirectory()) {
    if (_listDir) _listDir.close();
    sendError(request, 400, "not_dir");
    return;
  }
  _listPath = path;
  _listOut = "";        // nothing may survive from an attempt that was cut off
  _listStarted = false;
  _listCount = 0;
  _listT0 = millis();
  _listFirst = true;
  _listDone = false;
  AsyncWebServerResponse *response = request->beginChunkedResponse("application/json", sdmanListFiller);
  // Same reason as sendJson(): a listing is state, not an asset.
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
}

// ==== Upload ====

// Multipart chunks are written straight to the card, so a large file never lands in RAM. The target is the
// folder being viewed plus a sanitised name; the multipart filename is only a fallback, since a browser may
// send a path in it.
static File     _upFile;
static String   _upPath;
static uint64_t _upFree = 0;
static uint64_t _upBytes = 0;
static bool     _upSkipped = false;
static const char *_upReason = nullptr;
static int      _upCode = 400;
static uint32_t _upLastChunkMs = 0;   // the last chunk that arrived, for the stall deadline
static uint32_t _upLastIndex = 0;     // the highest multipart index seen, to recognise a superseded request's chunks
static bool     _upInWrite = false;   // a write is executing right now - never close the handle under it
static uint16_t _upRetries = 0;       // chunks the card refused outright and the retry rescued, per file
static uint16_t _upRetryBurst = 0;    // refusals IN A ROW with no successful chunk between them
static uint16_t _upPartialRescues = 0; // partial writes put back together from the card's own position
static bool     _upLagSettled = false; // "the reading was early" has been said once for this file
/* PER-FILE totals, across attempts, because every figure above belongs to ONE attempt and the page sends a file over
   several of them.  _filePath is what tells the two apart: a retry of the same file continues the count, a different
   file starts a new one.  This is what the "Finished Upload" line reports - otherwise a file that took minutes and
   thirty attempts reads as its last attempt: "657001 bytes moved in 3.23s, 172 KB/s, 0 rescued". */
static String   _filePath;             // the file the totals below belong to
static uint32_t _fileStartedMs = 0;    // when its FIRST attempt began
static uint64_t _fileMoved = 0;        // bytes moved by every attempt of it, summed
static uint16_t _fileAttempts = 0;     // attempts started
static uint16_t _fileRetries = 0;      // rescued refusals, summed
static uint16_t _filePartialRescues = 0; // repaired partial writes, summed
static bool     _fileFolded = false;   // this attempt's bytes are already in the file totals

static uint32_t _upStartedMs = 0;      // when the file was opened, for the per-file transfer timing
static uint64_t _upVerified = 0;       // the last position PROVEN to be on the card by the verification check
static String   _upResumePath;         // the file that verified position belongs to
static uint64_t _upResumeAt = 0;       // what the next request may resume from (0 = nothing to resume from)
static uint64_t _upResumedFrom = 0;    // what THIS request resumed from, for the log
static uint64_t _upNextCheck = 0;     // the byte count at which the stream must next be proved real

/* Add the attempt that is ENDING to the file's running totals, ONCE.  An attempt can end three ways - a response
   (hUploadDone), a stall release (abortUpload), or a cut-off - and two of them can run for the same attempt, which is
   why this is called from both and guarded rather than from one place.  It must also be called while `_upPath` is
   still set, which is what tells a retry of the same file from the start of a different one. */
static void fileFoldAttempt() {
  if (_fileFolded || _upPath.length() == 0) return;
  _fileFolded = true;
  _fileMoved += (_upBytes - _upResumedFrom);
  _fileRetries += _upRetries;
  _filePartialRescues += _upPartialRescues;
}
#if SDMAN_WRITE_SECTOR
/* The write assembly.  _upRem carries the sub-sector tail over to the next chunk - never more than
   SDMAN_WRITE_SECTOR-1 bytes - and _upPend is where the payload for one write is built.  _upPend is sized for the
   largest payload the rule can produce (a held-back tail plus one chunk, 1947 bytes) with room to spare; the write
   path falls back to handing a chunk straight over if one ever exceeds it, so nothing here depends on the TCP
   segment size being what it usually is. */
static uint8_t  _upRem[SDMAN_WRITE_SECTOR];
static uint16_t _upRemLen = 0;
static uint8_t  _upPend[SDMAN_WRITE_SECTOR + 1536];
#endif

/* The offset a retry may resume from: the last proven position, rounded DOWN to a whole sector.  The rounding is
   what keeps the alignment true across a resume - a retry that started mid-sector would put every write after it
   half a sector off again, which is the thing the alignment exists to avoid.  It costs nothing: the page re-sends
   from exactly the figure advertised here, so both ends agree without either having to know about sectors. */
static uint64_t resumeFloor(uint64_t verified) {
#if SDMAN_WRITE_SECTOR
  return verified - (verified % SDMAN_WRITE_SECTOR);
#else
  return verified;
#endif
}

/* Writes the tail held back so that the bulk of a file could go to the card in whole sectors.  Called when the
   multipart stream ends, and again inside hUploadDone so that the size check below reads a file which is complete
   rather than one short by up to a sector.  A plain checked write is enough here: it runs once per attempt, and if
   it fails, the size check that follows is what reports it. */
static void sdFlushTail() {
#if SDMAN_WRITE_SECTOR
  if (!_upRemLen) return;
  const size_t n = _upRemLen;
  _upRemLen = 0;
  /* Nothing to write it into, or the transfer is already over: the bytes are dropped, which is correct - the resume
     position is a VERIFIED one, so it never depends on a tail that was never on the card. */
  if (!_upFile || _upReason != nullptr) return;
  errno = 0;
  _upInWrite = true;
  const size_t wrote = _upFile.write(_upRem, n);
  _upInWrite = false;
  _upBytes += wrote;
  if (wrote != n) {
    _upReason = "write_failed";
    _upCode = 500;
    FUNCTIONLOG("SDFileManager", "The last %u bytes of %s would not go down (%u of %u written, errno %d)",
                (unsigned)n, _upPath.c_str(), (unsigned)wrote, (unsigned)n, errno);
  }
#endif
}

// One owner for every way an upload ends badly: a chunk that never came, the mode closing, a browser that
// vanished.  The reason is set BEFORE the close, so a late hUploadDone() reports it instead of a success, and the
// partial file is taken away, because a truncated track under a name the page has already listed is worse than no
// track at all.  The byte count is logged with it: "490496 bytes" and "0 bytes" are the same failure otherwise.
// `resumable` keeps the file instead of deleting it, so the browser can send only the remainder - only ever when
// a position has been VERIFIED, because appending onto bytes we cannot vouch for would splice two different
// versions of a file together.
static void abortUpload(const char *reason, int code, bool resumable = false) {
  if (!_upFile) return;
  const uint32_t wrote = (uint32_t)_upBytes;
  const uint64_t verified = _upVerified;
  /* Fold this attempt into the file's totals BEFORE anything is cleared, because this is one of the three ways an
     attempt can end and it is the one that never reaches hUploadDone() - which is why a released 8.6 MB attempt was
     missing from its file's total and the "Finished Upload" line reported 16 KB/s for a file that ran at 118. */
  fileFoldAttempt();
  _upFile.close();
#if SDMAN_WRITE_SECTOR
  _upRemLen = 0;   // the held-back tail is ours, not the card's: a resume starts from the proven position
#endif
  if (_upPath.length()) {
    if (resumable && verified) {
      _upResumePath = _upPath;
      _upResumeAt = resumeFloor(verified);
      FUNCTIONLOG("SDFileManager", "Upload stopped at %lu bytes with %llu verified on the card - keeping %s so the next attempt sends only the remainder",
                  (unsigned long)wrote, (unsigned long long)verified, _upPath.c_str());
    } else {
      sdman.remove(_upPath);
      FUNCTIONLOG("SDFileManager", "Upload aborted at %lu bytes - removed %s (%s)",
                  (unsigned long)wrote, _upPath.c_str(), reason);
    }
  }
  _upPath = "";
  /* _upBytes is deliberately NOT cleared.  hUploadDone() may still run for this request and reports how far the
     transfer got - zeroing it here made a released 6.4 MB transfer print "0 bytes moved", which reads as a transfer
     that never started.  The next request's index==0 resets it, which is where that belongs. */
  _upVerified = 0;
  /* A reason that is already set is the honest one, and it is kept.  The stall branch runs after a chunk may have
     failed, and overwriting write_failed with stalled destroyed the only evidence of why the transfer died -
     which is what happened in the field: every stalled line had been a short write first. */
  if (_upReason == nullptr) {
    _upReason = reason;
    _upCode = code;
  }
}

/* ?up=1: the state of the transfer in flight, and NOTHING else.  It is a shape of its own because /sdman/info's body
   walks the FAT to count the used bytes - card reads issued while a chunk may be being written - and the caller is
   the page's transfer watchdog, which asks every two seconds for the whole of an upload.  This touches no card.
   The page cannot afford to wait for the request to END to learn that a transfer is lost: the browser goes on
   pushing the rest of the file up the wire while every byte of it is being dropped, and that wasted tail is the
   largest part of what a failure costs.  So:
     open    - a handle is open for writing.  This is what the RETRY waits on: the failed handle is closed by the
               stall branch in loop(), and closing it is also when abortUpload() records the resume offset - so a
               retry sent before that would be taken as a FRESH file, truncating the target the resume was meant to
               continue, and would write the page's tail slice at position zero.
     lost    - the mode is holding an OPEN transfer that has already failed.  Tied to the handle on purpose: a
               reason outlives the transfer it describes, and a stale `lost` would abort the next file's request.
     up      - the path that offset belongs to, so the page can attribute it (or ignore it).
     landed  - the position PROVEN to be on the card, i.e. what a retry may resume from. */
static void sendUploadState(AsyncWebServerRequest *request) {
  char esc[256];
  jsonEscapeTo(esc, sizeof(esc), _upPath.c_str());
  String up = F("{\"ok\":true,\"open\":");
  up += _upFile ? "true" : "false";
  up += F(",\"lost\":");
  up += (_upFile && _upReason) ? "true" : "false";
  up += F(",\"landed\":");
  /* Floored to a sector, exactly as the resume record is: the page sends this figure back as its offset, and the
     device only accepts an offset it advertised, so the two must be the same number. */
  up += String((unsigned long long)resumeFloor(_upVerified));
  up += F(",\"up\":\"");
  up += esc;
  up += F("\"}");
  sendJson(request, 200, up);
}

/* Forget a resume record AND take the file away.  Keeping the partial is what makes a resume possible, but only for
   as long as the page is coming back for it: once a different file starts - or the mode closes - an abandoned
   partial is just a truncated track under a name the page has listed, which is exactly what the whole design
   forbids.  A field run proved it matters: the page gave up after MAX_FILE_TRIES and left a 9.25 MB half-file on
   the card, which the listing then counted as a track. */
static void discardResume() {
  if (_upResumeAt && _upResumePath.length() && sdman.remove(_upResumePath)) {
    FUNCTIONLOG("SDFileManager", "Discarded the abandoned partial %s (%llu bytes)",
                _upResumePath.c_str(), (unsigned long long)_upResumeAt);
  }
  _upResumeAt = 0;
  _upResumePath = "";
  _upVerified = 0;
}

static void onUploadChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  // Re-tested per chunk, not just at the start: the mode can end (timeout, card removal) mid-file. The
  // reason is reported by the request handler at the end.
  if (!filemanager.active()) {
    // Close what this upload opened.  The next attempt's index == 0 would close it, but a batch that stops
    // here would otherwise hold a card handle until then.
    if (_upFile) _upFile.close();
    _upReason = "not_active";
    _upCode = 409;
    return;
  }
  /* A chunk from a request that has been SUPERSEDED, dropped in silence.  The multipart index restarts at 0 with
     every request, so a chunk whose index is below the highest this transfer has seen cannot belong to it: it is
     the tail of a request the page already gave up on, still in flight.  Writing one of those into the file would
     splice two transfers together, and reporting it produced lines about an empty path and a card that was never
     asked anything (`the rescue of a refused chunk at  was refused too`).  Dropped before the clock is stamped, too,
     because a dead request is not activity. */
  if (index && index < _upLastIndex) return;
  _upLastIndex = index;
  // Every chunk refreshes the idle clock, so a slow upload cannot look like an idle browser, and feeds the task
  // watchdog.  This runs in AsyncTCP's task, which is subscribed (CONFIG_ASYNC_TCP_USE_WDT), and a single chunk
  // write can block for seconds on an SPI card - the same hazard the delete batch was fixed for.  Without the feed
  // a slow write reboots the device mid-file, which is exactly what a batch that "just stops, with nothing in the
  // log" looks like from the browser's side.
  filemanager.touch();
  sdFeedWatchdog();
  // Stamped here, before anything else can go wrong: the stall deadline in loop() measures from the last chunk
  // that ARRIVED, so a transfer that goes quiet is visible as silence rather than as an upload still in flight.
  _upLastChunkMs = millis();

  if (index == 0) {
    _upReason = nullptr;
    _upFree = 0;
    _upBytes = 0;
    _upSkipped = false;
    _upStartedMs = 0;
    // A cut-off attempt leaves the handle open and the file half-written; closing it here is what the missing
    // UPLOAD_FILE_ABORTED notification would otherwise do.  It is logged because it used to be silent: the file
    // simply never appeared as uploaded, so a batch that died partway looked like a batch that had finished.
    if (_upFile) {
      FUNCTIONLOG("SDFileManager", "Previous upload was cut off: %s stopped at %lu bytes",
                  _upPath.length() ? _upPath.c_str() : "(name unknown)", (unsigned long)_upBytes);
      _upFile.close();
    }
    _upPath = "";
    _upRetries = 0;
    _upRetryBurst = 0;
    _upPartialRescues = 0;
    _upNextCheck = 0;
    _upVerified = 0;
    _upResumedFrom = 0;
#if SDMAN_WRITE_SECTOR
    _upRemLen = 0;   // a new file starts on a sector boundary, with nothing held back from the last one
#endif

    String dir = normalisePath(argOf(request, "path"));
    String name = argOf(request, "name");
    if (name.length() == 0) name = basenameOf(filename);
    // Set by the Skip Existing button; Replace sends nothing.
    bool skip = argOf(request, "skip") == "1";

    if (name.length() == 0 || !isSafeName(name)) { _upReason = "bad_name";    _upCode = 400; }
    else if (!sdman.ready)                       { _upReason = "no_card";     _upCode = 409; }
    else if (!sdman.exists(dir))                 { _upReason = "no_dir";      _upCode = 404; }
    else {
      String target = (dir == "/") ? ("/" + name) : (dir + "/" + name);
      /* The per-file counters, keyed by path: the same file retried continues them, a different file starts over. */
      if (target != _filePath) {
        _filePath = target;
        _fileStartedMs = millis();
        _fileAttempts = 0;
        _fileMoved = 0;
        _fileRetries = 0;
        _filePartialRescues = 0;
      }
      _fileAttempts++;
      _fileFolded = false;   // the attempt that just ended has been folded; this one is not yet
      _upLagSettled = false;
      /* RESUME, and the whole of the protocol on this side: the page may send only the remainder of a file whose
         previous attempt the card ended, with offset=N.  N must be a position we can VOUCH for - the same path, and
         exactly the value we advertised - because appending at a guess would splice two different versions of the
         file together.  A mismatch is not an error: the file restarts from the beginning, which is only slower than
         it needed to be.
         It is computed BEFORE the refusal chain because it has to beat one of those refusals: keeping the partial is
         what makes the target exist, so Skip Existing would refuse the very retry the partial was kept for - and the
         partial is then discarded, which loses the file twice over.  A field run lost five of fourteen files in one
         batch exactly that way, each with `upload skipped, name already present` followed by `discarded the
         abandoned partial`. */
      const uint64_t wantOffset = strtoull(argOf(request, "offset").c_str(), nullptr, 10);
      const bool canResume = (wantOffset > 0 && wantOffset == _upResumeAt &&
                              target == _upResumePath && sdman.exists(target));
      /* FIRST, ahead of every other refusal: a card carrying the marker has already proved it cannot take uploads
         over this reader, so nothing may be written here whatever the page offered.  See SDMAN_SPI_MARKER. */
      if (spiUploadBlocked())                    { _upReason = "spi_blocked"; _upCode = 403; }
      else if (FileManager::isProtected(target)) { _upReason = "protected";   _upCode = 403; }
      else if (filemanager.isPlaying(target))    { _upReason = "in_use";      _upCode = 409; }
      else if (skip && !canResume && sdman.exists(target)) {
        // Skip Existing: nothing is opened, so nothing is truncated and no room is reserved for a write that
        // will not happen. The request answers "skipped" and the page names the file after the batch.
        _upSkipped = true;
      }
      else {
        if (_upResumePath.length() && _upResumePath != target) {
          // A different file: that position means nothing here, and the file it referred to must not be left
          // half-written on the card either.
          discardResume();
        }
        bool resuming = false;
        if (canResume) {
          _upFile = sdman.open(target, "r+");   // read/write and NO truncate, which is what makes a resume possible
          if (_upFile) {
            resuming = _upFile.seek((uint32_t)wantOffset);
            if (!resuming) _upFile.close();
          }
        }
        if (wantOffset > 0 && !resuming) {
          /* A request that carries an offset is a SLICE of a file, not the file - the page sent only what it
             believed was missing.  Writing that as a fresh upload would truncate the target and put the tail at
             position zero: a corrupt track, reported as an upload.  So an offset that cannot be honoured is
             REFUSED, and the page answers by dropping its figure and sending the whole thing. */
          _upReason = "resume_refused";
          _upCode = 409;
          FUNCTIONLOG("SDFileManager", "Resume refused for %s (asked for %llu, we can vouch for %llu) - the page will send it whole",
                      target.c_str(), (unsigned long long)wantOffset, (unsigned long long)_upResumeAt);
        }
        if (!resuming && _upReason == nullptr) {
          // No exists() test on this branch: FILE_WRITE truncates, so uploading over a name replaces it - which is
          // the point of re-uploading a corrected track. Protection and in-use are the only refusals.
          _upFile = sdman.open(target, FILE_WRITE);
        }
        if (!_upFile) { _upReason = "open_failed"; _upCode = 500; }
        else {
          // Measured AFTER the open: the open truncates, which is what releases the room the replacement
          // needs. Measuring first would refuse an overwrite that has plenty of space once the old copy goes.
          uint64_t total = sdman.totalBytes();
          uint64_t used  = sdman.usedBytes();
          _upFree = (total > used) ? (total - used) : 0;
          if (_upFree == 0) {
            // A zero SWITCHES THE TEST OFF below, so it is said out loud: a card whose figures do not add up
            // would otherwise be treated as a card with endless room.
            FUNCTIONLOG("SDFileManager", "Free space reads as 0 (total %llu, used %llu) - the limit check is off",
                        (unsigned long long)total, (unsigned long long)used);
          }
          _upPath = target;
          _upStartedMs = millis();
          if (resuming) {
            // The accounting continues from the vouchable position, so every check below still means the same
            // thing: _upBytes is what the file should hold and the medium has to agree with it.
            _upBytes = wantOffset;
            _upVerified = wantOffset;
            _upResumedFrom = wantOffset;
            _upNextCheck = wantOffset + SDMAN_UPLOAD_VERIFY_BYTES;
            FUNCTIONLOG("SDFileManager", "Resuming %s at %llu bytes - only the remainder is sent",
                        target.c_str(), (unsigned long long)wantOffset);
          }
          // Logged at the start, not just at the end: "upload refused" and "the request never arrived" were
          // indistinguishable in the log, and that is exactly what made the browser that sent nothing look
          // like a device fault.
          FUNCTIONLOG("SDFileManager", "Upload start %s (client name '%s')", target.c_str(), filename.c_str());
        }
      }
    }
  }

  if (_upReason == nullptr && len) {
#if SDMAN_WRITE_SECTOR
    /* ASSEMBLE WHOLE SECTORS BEFORE WRITING ANY.  A chunk is 1436 bytes - a TCP segment, not a multiple of the
       card's sector - so writing each one as it arrives makes the file system read-modify-write a PARTIAL SECTOR,
       through the very driver that is programming the previous block.  The sub-sector tail is therefore held back
       and prepended to the next chunk, and every write below is a whole number of sectors long AND starts on one
       (the resume offset is rounded down before it is offered, which is what keeps the second half of that true
       after a retry).
       1436 plus at most 511 held-back bytes is 1947, so one chunk can produce at most one payload: len either
       becomes a multiple of SDMAN_WRITE_SECTOR or becomes zero, and never needs splitting across two writes.  A
       zero leaves the bytes in the tail for the next call, and the accounting follows the WRITE, not the arrival,
       which is why _upBytes is not advanced here. */
    if (len > sizeof(_upPend) - (size_t)_upRemLen) {
      /* A chunk bigger than the assembly buffer.  Never seen - a multipart body arrives in TCP-sized pieces - but
         the write path must not depend on that, and an overflow would be far worse than an unaligned write: the
         held-back tail goes with it, and both are written straight through by the code below. */
      _upRemLen = 0;
    } else {
      const size_t have = (size_t)_upRemLen + len;
      const size_t wlen = have - (have % SDMAN_WRITE_SECTOR);
      memcpy(_upPend, _upRem, _upRemLen);
      memcpy(_upPend + _upRemLen, data, len);
      memcpy(_upRem, _upPend + wlen, have - wlen);
      _upRemLen = (uint16_t)(have - wlen);
      if (wlen) {
        data = _upPend;
        len = wlen;
      } else {
        len = 0;   // nothing whole yet: the chunk is now the held-back tail, and there is nothing to write
      }
    }
#endif
    // Stop at the free-space limit rather than filling the card: a half-written file the user is told about
    // beats a full disk. 507 is the storage audit's code and the page has its own message for it.
    // _upBytes, not _upFile.position(): an ftell on a buffered FATFS write stream is a second opinion about a
    // position this function already owns, and asking the stream while it is being written is a hazard with no
    // payoff - the count is right here.
    if (_upFree && (_upBytes + len) > _upFree) {
      _upReason = "no_space";
      _upCode = 507;
    } else if (_upFile && len) {
      // The result is checked: a card that shortens or refuses a write used to leave a truncated file that still
      // reported "uploaded (N bytes)".  A short write ends this file, and hUploadDone reports it as failed.
      // errno and the duration go in the line because a byte count cannot tell the three cases apart: a card that
      // refused the write, a mount that has gone dirty, and a write that took seconds and then failed.  The
      // millisecond figure is the one that matters most - it is what a stalled card looks like on the way down.
      errno = 0;
      const uint32_t writeStart = millis();
      _upInWrite = true;
      size_t wrote = _upFile.write(data, len);
      uint32_t writeMs = millis() - writeStart;
      int writeErr = errno;
      /* A PARTIAL write is put back together, not failed - the medium's position can be measured.  After a flush,
         size() is exactly how many bytes are really on the card; when that has not fallen behind what we have
         already accounted for, the missing bytes are the TAIL of this chunk, which is still in `data`.  So seek to
         the card's own figure and re-send that tail.  This is strictly safer than failing the file:
           - the stream is repositioned from the card's number, so the two cannot drift apart;
           - if the shortfall reaches back past _upBytes, those bytes are gone from our side too and the file is
             failed - we cannot re-supply bytes from chunks we no longer hold.
         It was 3 of the 5 failures in one 14-file batch (192, 93 and 235 bytes of 1436), which makes it the single
         largest source of lost uploads left. */
      if (wrote > 0 && wrote < len) {
        _upFile.flush();
        const uint64_t onCard = (uint64_t)_upFile.size();
        const uint64_t want   = _upBytes + len;
        if (onCard >= want) {
          wrote = len;                  // nothing was actually missing; the short return was the only symptom
        } else if (onCard >= _upBytes) {
          const size_t lost = (size_t)(want - onCard);
          if (_upFile.seek((uint32_t)onCard)) {
            errno = 0;
            const size_t again = _upFile.write(data + (len - lost), lost);
            if (again == lost) {
              wrote = len;              // whole on the medium now, so _upBytes + len is the truth again
              writeErr = 0;
              _upPartialRescues++;
              _upRetryBurst = 0;
              /* A REPAIRED WRITE IS PROOF OF NOTHING: a card that shortened one write can be shortening the next, so
                 the stream is proved on this chunk rather than at the next 64 KB checkpoint.  Half the failures in a
                 field run had this shape, and the loss they hid was always the same size - everything written since
                 the last checkpoint, 30 to 66 KB of it, thrown away for nothing.  See the refusal rescue below. */
              _upNextCheck = _upBytes + wrote;
              if (_upPartialRescues == 1) {
                FUNCTIONLOG("SDFileManager", "A partial write at %s was repaired: %llu bytes were on the card, its last %u were re-sent",
                            _upPath.c_str(), (unsigned long long)onCard, (unsigned)lost);
              }
            } else {
              FUNCTIONLOG("SDFileManager", "A partial write at %s could not be repaired (%u of %u bytes re-sent)",
                          _upPath.c_str(), (unsigned)again, (unsigned)lost);
            }
          }
        }
      }
      /* A chunk the card refused OUTRIGHT is retried, because nothing was accepted - the stream has not moved, so
         re-issuing the same bytes cannot duplicate or skip any.  EIO (errno 5, FR_DISK_ERR beneath it) arrives when
         the card will not take the next write while it is still programming the previous block, which is a timing
         failure that clears on its own.  Two rules decide when to stop, because one number cannot describe both
         kinds of failure:
           - a BURST of consecutive refusals with no success between them is a card that is gone;
           - otherwise the budget is generous by design (see SDMAN_UPLOAD_RETRY_PER_BYTES) so it never becomes the
             limit on a transfer that is landing.  A flat cap of 8 once ended a file 24225 bytes from the end. */
      if (wrote == 0 && len) {
        const uint16_t rescueBudget = (uint16_t)(SDMAN_UPLOAD_RETRY_FLOOR + (_upBytes / SDMAN_UPLOAD_RETRY_PER_BYTES));
        const bool burst      = (_upRetryBurst + 1) >= SDMAN_UPLOAD_RETRY_BURST;
        const bool overBudget = _upRetries >= rescueBudget;
        if (burst || overBudget) {
          // Named, because the two mean opposite things about the card: a burst is it giving up, a spent budget on
          // a large file is the limit of what we are willing to spend on a transfer that is otherwise landing.
          FUNCTIONLOG("SDFileManager", "The card refused a chunk at %s and it was not retried (%s: %u of %u rescues used, refusal %u in a row)",
                      _upPath.c_str(), burst ? "burst - the card is not recovering" : "rescue budget reached",
                      (unsigned)_upRetries, (unsigned)rescueBudget, (unsigned)(_upRetryBurst + 1));
          _upRetryBurst++;
        } else {
          /* The pause grows with consecutive refusals (see SDMAN_UPLOAD_RETRY_MAX_DELAY_MS): an isolated refusal
             pays only the base delay, and a card that refuses again at once gets twice as long to finish what it is
             already programming.  A flat delay asked a busy card the same question the same way for ever. */
          uint32_t pause = SDMAN_UPLOAD_RETRY_DELAY_MS;
          for (uint8_t i = 0; i < _upRetryBurst && pause && pause < SDMAN_UPLOAD_RETRY_MAX_DELAY_MS; i++) pause *= 2;
          if (pause > SDMAN_UPLOAD_RETRY_MAX_DELAY_MS) pause = SDMAN_UPLOAD_RETRY_MAX_DELAY_MS;
          if (pause) vTaskDelay(pdMS_TO_TICKS(pause));
          errno = 0;
          const uint32_t retryStart = millis();
          wrote = _upFile.write(data, len);
          writeMs = millis() - retryStart;
          writeErr = errno;
          if (wrote == len) {
            _upRetries++;
            _upRetryBurst = 0;
            /* A RESCUED REFUSAL IS PROOF OF NOTHING - prove the stream right now instead of at the next checkpoint.
               The field pattern is refusal, rescue accepted, and then the card silently swallowing the whole next
               window: `fell 28765 bytes behind`, `fell 49553 bytes behind`, one 64 KB checkpoint's worth every time.
               Everything written between the rescue and the next checkpoint is therefore written for nothing, and
               the file is failed 30 to 66 KB later than it could have been - which is both wasted writing and wasted
               wall clock, on every failure.  Asking here costs one flush and one size() on this chunk, fails the
               transfer at the moment the card stopped telling the truth, and leaves `_upVerified` as high as it can
               be, so the resume is as short as it can be.  If the card is well, which it usually is, nothing else
               changes at all: the check passes and the next one is armed 64 KB on. */
            _upNextCheck = _upBytes + wrote;
            // ONE line per file, on the first rescue.  A card that is merely flaky can rescue many chunks, and a
            // line each flooded the log badly enough that logging had to be switched off to read anything.
            if (_upRetries == 1) {
              FUNCTIONLOG("SDFileManager", "The card refused a chunk at %s; the retry wrote it (%u bytes in %lums) - further rescues this file are counted, not logged",
                          _upPath.c_str(), (unsigned)len, (unsigned long)writeMs);
            }
          } else {
            /* The rescue was refused TOO, and this used to be silent.  The short-write line that follows ends with
               "refused, see the line above" and there was no line above to see - which reads as a mystery rather
               than as the card refusing the same write twice in a row, one of the two shapes a failing transfer
               actually takes.  Named once, with the burst count, because it is the next refusal that ends the file. */
            _upRetryBurst++;   // a failed retry counts toward the burst
            FUNCTIONLOG("SDFileManager", "The rescue of a refused chunk at %s was refused too (%u bytes, errno %d, %u in a row)",
                        _upPath.c_str(), (unsigned)len, writeErr, (unsigned)_upRetryBurst);
          }
        }
      }
      // Prove the stream is real every so many bytes, not only at the end.  flush() then size() is the only pair
      // that tells the truth about the medium, and a card that accepts into the stream buffer and discards it
      // otherwise surfaces megabytes later - 4815872 bytes into one file in the field log.
      bool streamOk = true;
      if (wrote == len && SDMAN_UPLOAD_VERIFY_BYTES && (_upBytes + wrote) >= _upNextCheck) {
        const uint32_t verifyStart = millis();
        _upFile.flush();
        const uint64_t onCard = (uint64_t)_upFile.size();
        if (onCard < _upBytes + wrote) {
          /* IS THE CARD BEHIND, OR IS THE READING EARLY?  Those two look identical from here and want opposite
             fixes, so the card is given time to finish the block it has just accepted and the size is read AGAIN.
             The budget card's signature is always a shortfall of 3.5 to 5.2 KB, on every refusal, which is the
             figure a card that is still programming would report - and if that is what this is, then nothing has
             been lost at all and the transfer must not be failed for it. */
          vTaskDelay(pdMS_TO_TICKS(SDMAN_UPLOAD_LAG_SETTLE_MS));
          _upFile.flush();
          const uint64_t onCardLater = (uint64_t)_upFile.size();
          if (onCardLater >= _upBytes + wrote) {
            // It caught up: nothing was lost, and the transfer carries on.  Said once per file, so the log keeps its
            // shape while still showing how often the reading - and therefore the failure - would have been wrong.
            if (!_upLagSettled) {
              _upLagSettled = true;
              FUNCTIONLOG("SDFileManager", "The card read %llu bytes short at %s and had caught up after %u ms - an early reading, not a lost write",
                          (unsigned long long)((_upBytes + wrote) - onCard), _upPath.c_str(),
                          (unsigned)SDMAN_UPLOAD_LAG_SETTLE_MS);
            }
          } else {
            streamOk = false;
            /* The card really is behind, and those bytes are gone: they belong to chunks we no longer hold, so this
               is failed at once rather than ground down with rescues.  Both readings are named, because the only
               thing that tells the two cases apart is what the second one says.  `card_lagging` is its own reason
               for exactly this: it is the one the page retries as a whole file, and the browser still has the bytes. */
            _upReason = "card_lagging";
            _upCode = 500;
            /* WHERE THE RETRY RESUMES FROM, and this is what makes a stalling card CONVERGE instead of losing the
               file.  `_upVerified` is the last CHECKPOINT that passed, which can be tens of KB behind what the card
               actually holds - the field resumed one 6.9 MB file at 766 bytes when the card held 29696, so every
               attempt re-sent the same ~29 KB into the same wall, never advanced, spent the whole retry budget and was
               discarded.  The card's own size is a LOWER bound on what is committed (FATFS only reflects flushed
               data), so it is the honest and maximal place to continue from; resumeFloor() rounds it to a sector. */
            _upVerified = onCard;
            FUNCTIONLOG("SDFileManager", "The card is %llu bytes behind at %s (%llu of %llu on the card, %llu after %u ms more, errno %d, check took %lums) - stopping here; the retry continues from %llu",
                        (unsigned long long)((_upBytes + wrote) - onCard), _upPath.c_str(),
                        (unsigned long long)onCard, (unsigned long long)(_upBytes + wrote),
                        (unsigned long long)onCardLater, (unsigned)SDMAN_UPLOAD_LAG_SETTLE_MS, errno,
                        (unsigned long)(millis() - verifyStart), (unsigned long long)_upVerified);
            writeErr = errno;
          }
        } else {
          _upNextCheck = _upBytes + wrote + SDMAN_UPLOAD_VERIFY_BYTES;
          // This position is the one thing in the whole path we have PROVED is on the card, so it is also the only
          // position a later attempt is allowed to resume from.
          _upVerified = _upBytes + wrote;
        }
      }
      _upInWrite = false;
      _upBytes += wrote;
      if (!streamOk) {
        _upReason = "write_failed";
        _upCode = 500;
      } else if (wrote != len) {
        FUNCTIONLOG("SDFileManager", "Short write at %s (%u of %u bytes, errno %d, after %lums)%s",
                    _upPath.c_str(), (unsigned)wrote, (unsigned)len, writeErr, (unsigned long)writeMs,
                    (wrote == 0) ? " - the card refused it" : " - stopping here; the retry sends the remainder");
        _upReason = "write_failed";
        _upCode = 500;
      } else if (writeMs >= 1000) {
        // A chunk that takes a second or more is the card answering slowly.  Reported on the way past, because it
        // is the shape of the write that never returns and the next log is the one that matters.
        FUNCTIONLOG("SDFileManager", "Slow write at %s (%u bytes took %lums)",
                    _upPath.c_str(), (unsigned)len, (unsigned long)writeMs);
      }
    }
  }

  if (final && _upFile) {
    /* The stream ended, so the held-back sub-sector tail has nowhere else to go - and the size check in
       hUploadDone measures the card, so it has to be written before this returns. */
    sdFlushTail();
    _upFile.close();
  }
}

// The request handler runs after the last chunk, so the outcome is reported here.
static void hUploadDone(AsyncWebServerRequest *request) {
  filemanager.touch();
  /* Before anything else - including the line below, which counts bytes: the held-back tail has to be on the card
     before it is counted, measured or reported.  A no-op when onUploadChunk has already flushed it, and the safety
     net for the streams that end without that last callback arriving. */
  sdFlushTail();
  /* Printed only when it says something the outcome line cannot: the handle still OPEN (the browser abandoned the
     stream mid-file, and nothing else will ever report that transfer) or a transfer that was already failing.  On
     the ordinary path this line and the "Uploaded" one below said the same thing twice, and the second said it with
     the figures that matter. */
  if (_upFile || _upReason != nullptr) {
    FUNCTIONLOG("SDFileManager", "Upload request finished (%s, %lu bytes, %u rescued, %u recovered, handle %s)",
                _upReason ? _upReason : "ok", (unsigned long)_upBytes, (unsigned)_upRetries,
                (unsigned)_upPartialRescues, _upFile ? "open" : "closed");
  }
  // The multipart stream ended without its final chunk: the browser abandoned it, or the connection died.  The
  // handle is still open, so nothing is complete, and "uploaded" must never be printed for a truncated file.  A
  // reason set by a short write is kept - this only fills the silence.
  if (_upFile) {
    _upFile.close();
    if (_upReason == nullptr) { _upReason = "cut_off"; _upCode = 409; }
  }
  // Before the mode test below, and before the index invalidation: a skip changed nothing, so there is no
  // write to protect and the playlist and index must stay exactly as they were.
  if (_upReason == nullptr && _upSkipped) {
    _upSkipped = false;
    _upPath = "";
    FUNCTIONLOG("SDFileManager", "Upload skipped, name already present");
    sendJson(request, 200, F("{\"ok\":true,\"skipped\":true}"));
    return;
  }
  if (_upReason != nullptr) {
    const char *reason = _upReason;
    int code = _upCode;
    _upReason = nullptr;
    /* A failure the CARD caused KEEPS the file when a position has been verified, so the browser can send only the
       remainder - that is what the landed field is for, and it is where minutes are saved rather than just bytes.
       Anything else - no space, a protected name, a refusal this page caused - removes the file, because a
       truncated track under a name the page has listed is worse than no track at all. */
    const bool cardFailure = (strcmp(reason, "write_failed") == 0) ||
                             (strcmp(reason, "card_lagging")  == 0) ||
                             (strcmp(reason, "stalled")       == 0);
    uint64_t landed = 0;
    if (cardFailure && _upVerified && _upPath.length()) {
      _upResumePath = _upPath;
      _upResumeAt   = resumeFloor(_upVerified);   // on a sector boundary, so the retry stays aligned
      landed        = _upResumeAt;
    } else if (_upResumeAt && _upResumePath.length()) {
      /* The stall branch in loop() already released THIS transfer and recorded the position a retry may use - the
         page aborts the request the moment it hears the transfer is lost, and the request can outlive the release
         by as long as the browser keeps pushing.  Nothing to remove: the file is still on the card, and forgetting
         the record here would refuse the very retry it was kept for. */
      landed = _upResumeAt;
    } else {
      if (_upPath.length()) sdman.remove(_upPath);
      _upResumeAt = 0;
      _upResumePath = "";
      _upVerified = 0;
    }
    const uint32_t failedMs = _upStartedMs ? (millis() - _upStartedMs) : 0;
    const uint32_t written  = (uint32_t)_upBytes;
    /* Copied out with the byte count, before either is cleared below: the line reports BYTES MOVED, and reading
       _upResumedFrom after it was zeroed made every resumed failure print the file POSITION instead - 6475728
       "moved" by an attempt that had resumed at 3370359 and therefore moved a little over three megabytes. */
    const uint32_t resumedFrom = (uint32_t)_upResumedFrom;
    _upPath = "";
    _upBytes = 0;
    _upResumedFrom = 0;
    /* The time is reported for a failure too, because a comparison run has to count the cost of the failures and not
       only the files that went through - that is exactly where the minutes go.  ERROR is kept for a LOSS: nothing was
       vouched for and the partial is gone, which is the only case the user has to do anything about.  A card-side
       failure retries itself from `landed`, so it is an ordinary line - the ERROR styling here was half of why this
       log read like a list of failed files. */
    if (landed) {
      FUNCTIONLOG("SDFileManager", "Upload did not finish (%s) after %lu.%02lus - %llu bytes moved, %llu is on the card; continuing from there",
                  reason, (unsigned long)(failedMs / 1000), (unsigned long)((failedMs % 1000) / 10),
                  (unsigned long long)(written - resumedFrom), (unsigned long long)landed);
    } else {
      ERRORLOG("SDFileManager: upload refused (%s) after %lu.%02lus, %llu bytes moved, nothing vouchable - the partial file is gone",
               reason, (unsigned long)(failedMs / 1000), (unsigned long)((failedMs % 1000) / 10),
               (unsigned long long)(written - resumedFrom));
    }
    /* Fold this attempt into the file's totals.  Guarded, so an attempt that was first RELEASED by the stall branch
       is not counted twice: abortUpload() folds it the moment it lets go, and this runs later for the same request. */
    fileFoldAttempt();
    sendError(request, code, reason, landed);
    return;
  }
  // Still answered inside the mode: "ok" from a mode that has since closed would leave the page thinking it
  // can carry on reading the card.
  if (!requireActive(request)) return;
  /* _upBytes is how many bytes the WRITE CALLS accepted, which is not the same as how many reached the card: on
     FATFS a write goes into a stream buffer and is reported as done, and the flush behind it can fail later.  That
     is how a card that is failing produces a confident "uploaded (N bytes)" line and a short file - the failure
     the page then has to discover by trying to play it.  So the file is re-opened by name and measured, and only
     the size on the card is reported as uploaded. */
  {
    File landed = _upPath.length() ? sdman.open(_upPath) : File();
    const uint64_t onCard = landed ? (uint64_t)landed.size() : 0;
    if (landed) landed.close();
    if (onCard != _upBytes) {
      /* The card took fewer bytes than the write calls accepted, and the bytes it lost are behind us.  That is the
         same loss as `card_lagging`, reaching us by a different route - this check reads the file, that one reads it
         at a checkpoint - so it gets the same recovery: the file is NOT thrown away when a position has been
         verified, because everything up to that checkpoint is known to be on the card and a retry can resume there.
         Removing it here cost a whole file once: Teardrops landed 7332954 of 7381237 bytes and the next attempt
         sent all 7381237 of them again, when 48 KB would have finished it. */
      const uint64_t keep = resumeFloor(_upVerified);
      if (keep) {
        FUNCTIONLOG("SDFileManager", "%s landed %llu of %llu bytes - short on the card, so the remainder will be sent",
                    _upPath.c_str(), (unsigned long long)onCard, (unsigned long long)_upBytes);
      } else {
        ERRORLOG("SDFileManager: %s landed %llu of %llu bytes: short on the card and nothing vouchable - removing it",
                 _upPath.c_str(), (unsigned long long)onCard, (unsigned long long)_upBytes);
      }
      if (keep) {
        _upResumePath = _upPath;
        _upResumeAt   = keep;
      } else {
        sdman.remove(_upPath);
        _upResumePath = "";
        _upResumeAt   = 0;
      }
      _upPath = "";
      _upBytes = 0;
      _upVerified = 0;
      _upResumedFrom = 0;
      sendError(request, 500, "write_failed", keep);
      return;
    }
  }
  /* How long the file took and how fast that is.  The point of the figure is comparison: 20000000 against
     40000000, or SPI against SDMMC, decided on throughput rather than on a feeling - and a clock can easily be
     good for reading and bad for writing, so this and the listing's `walked N entries in M ms` are read
     together.  A resumed transfer reports only the bytes IT moved, so a retry cannot flatter the average. */
  /* Bytes MOVED, not bytes in the file.  A resumed transfer starts at the offset, so the old version divided the
     WHOLE file size by the time the tail took and reported 1086 KB/s for a transfer that actually ran at 172 - and
     the only reason these figures exist is comparison, so they have to be the same bytes or they are worthless.
     Seconds rather than milliseconds: "33253ms" takes work to read and every figure here is read by eye. */
  /* ONE line per FILE, and these are the file's figures, not this attempt's: the clock runs from the first attempt's
     start, the bytes and the counters are the sum of every attempt, and the attempt count is named.  A retried file
     therefore reports what it really cost - wall clock included - instead of the flattering speed of its last, short
     attempt.  The request-finished line that used to precede this was the other half of a 28-line 14-file batch. */
  fileFoldAttempt();
  const uint32_t fileMs = _fileStartedMs ? (millis() - _fileStartedMs) : 0;
  const uint32_t kbPerSec = (fileMs && _fileMoved) ? (uint32_t)((_fileMoved / 1024ULL) * 1000ULL / fileMs) : 0;
  FUNCTIONLOG("SDFileManager", "Finished Upload %s (%llu bytes moved in %lu.%02lus, %lu KB/s, %u attempt(s), %u refused chunk(s) rescued, %u partial(s) recovered)",
              _upPath.c_str(), (unsigned long long)_fileMoved, (unsigned long)(fileMs / 1000),
              (unsigned long)((fileMs % 1000) / 10), (unsigned long)kbPerSec, (unsigned)_fileAttempts,
              (unsigned)_fileRetries, (unsigned)_filePartialRescues);
  _filePath = "";   // the file is done: the next different path starts a fresh count
  // A whole file leaves nothing to resume from, so the record goes with it.
  _upResumeAt = 0;
  _upResumePath = "";
  _upVerified = 0;
  _upResumedFrom = 0;
  _upPath = "";
  dropSdDerivedFiles();
  sendOk(request);
}

// ==== Mode ====

bool FileManager::uploadOpen() const {
  return _upFile ? true : false;
}

uint32_t FileManager::uploadIdleMs() const {
  if (!_upFile) return 0;
  /* ONE read of the clock and ONE of the stamp, then a clamp.  The subtraction is unsigned and the stamp is written
     by the AsyncTCP task: a chunk landing between those two reads made `millis() - _upLastChunkMs` negative, which
     wraps to 4294967289 ms - and the stall branch, seeing forty-nine days of silence, released a transfer that had
     6.4 MB in and was still sending.  A stamp from the future means the transfer is idle by nothing. */
  const uint32_t now  = millis();
  const uint32_t last = _upLastChunkMs;
  return (now >= last) ? (now - last) : 0;
}

void FileManager::touch() {
  _lastActivity = millis();
  // The manager stops the player, which leaves the display in the state the screensaver branch counts
  // towards (mode PLAYER, nothing playing).  Any API call is the user being here, so it also resets that
  // countdown.  The display-mode guard is the other half of this: only SDMAN can take the screen while the
  // mode is open, so the mode cannot fall back to PLAYER under an active session in the first place.
  config.screensaverTicks = 0;
  config.screensaverPlayingTicks = 0;
}

void FileManager::markCardChanged() {
  _cardChanged = true;
}

uint32_t FileManager::idleRemainingMs() const {
  if (!_active) return 0;
  /* ONE clock, and it is the USER's.  This used to answer with the upload's own deadline while a transfer was open,
     which put a number on the display that nobody asked for and that sat still because every chunk restamped it -
     first a frozen 2:59, then a frozen 1:59 when the window changed.  The deadline is still ENFORCED by the stall
     branch in loop(); what is worth SHOWING is now decided where the showing happens.
     Keeping every chunk's touch() is what makes an upload hold this clock open, so during an upload or a delete the
     remaining time stays near the top and the display draws nothing - which is the point. */
  uint32_t elapsed = millis() - _lastActivity;
  if (elapsed >= SDMAN_AUTO_EXIT_MS) return 0;
  return SDMAN_AUTO_EXIT_MS - elapsed;
}

void FileManager::enter() {
  // The stop and the capture belong to the transition INTO the mode: the page calls this route on load and
  // again after it replaces itself with "/", and doing both twice breaks the resume twice over - the second
  // capture reads a player the first call already stopped, and the second stop overwrites sdResumePos.
  // What gets remembered is whether the player was running, because the mode is not a play button.
  // The stop is what lets the manager assume nothing is reading the card; Player::_play() refusing new
  // playback only stops the next track.
  if (!_active) {
    _wasPlaying = player.isRunning();
    /* STOP THE PLAYER SYNCHRONOUSLY, BEFORE ANYTHING UNMOUNTS THE CARD.  This is not tidiness: a few lines below the
       manager may REMOUNT the card (its session clock is SDSPISPEED_MANAGER and the player's is SDSPISPEED), and a
       remount discards every handle.  A QUEUED `PR_STOP` only ASKS the player task to stop, so the remount ran first
       and the audio's file handle outlived the filesystem it came from - closing it afterwards came back as
       `CORRUPT HEAP: Bad head at ... / assert failed: multi_heap_free` and a reboot, every time the manager was
       opened while PLAYING FROM THE CARD (a web stream holds no SD handle, which is why it only showed up then).
       stopSync() closes the audio file there and then - it is `_stop()` -> `stopSong()` - and it is the same order
       config.cpp's changeMode() already uses for exactly this reason: stopSync(), THEN sdman.stop().
       _wasPlaying must be read BEFORE this, because the stop clears it. */
    player.stopSync();
  }
  // Mount on demand - the one point where we know the user wants the card - and after the stop above, so it cannot
  // fight the player for the volume.  THIS IS THE ONLY SPEED CHANGE A SESSION HAS: ensureSpeed() moves the card off
  // the player's SDSPISPEED to SDSPISPEED_MANAGER and remounts if it has to, and leave() puts SDSPISPEED back.  A
  // remount discards every open handle, which is precisely why there is only this one, at the mode boundary.
  if (!sdman.ready) sdman.start(SDSPISPEED_MANAGER);
  else sdman.ensureSpeed(SDSPISPEED_MANAGER);
  _lastActivity = millis();
  if (_active) return;
  _active = true;
  display.putRequest(NEWMODE, SDMAN);
  #if defined(SD_USE_MMC)
    const char* transport = "MMC";
  #else
    const char* transport = "SPI";
  #endif
  // Transport, card type and capacity all in the entry line: the next report about a card problem should not
  // need a second round trip to know what was actually mounted.
  /* THE ALLOCATION UNIT GOES IN THIS LINE, and it earns its place: a cheap card formatted with the 8 KB default took
     an upload only while it was empty and failed on everything afterwards, while the SAME card at 512-byte units takes
     everything with zero refusals.  Nothing ever printed this figure, so a mis-formatted card was invisible in every
     log we collected - this is the one number that would have ended that hunt in a minute. */
  char au[24];
  const uint32_t auBytes = sdman.allocationUnit();
  if (auBytes == 0)                                   snprintf(au, sizeof(au), "unknown");
  else if (auBytes >= 1024 && (auBytes % 1024) == 0)  snprintf(au, sizeof(au), "%luKB", (unsigned long)(auBytes / 1024));
  else                                                snprintf(au, sizeof(au), "%lu bytes", (unsigned long)auBytes);
  FUNCTIONLOG("SDFileManager", "Open (%s transport, SD %s, type %d, %lu MB, Allocation unit size: %s)", transport,
              sdman.ready ? "mounted" : "NOT mounted", (int)sdman.cardType(),
              (unsigned long)(sdman.cardSize() / (1024ULL * 1024ULL)), au);
}

void FileManager::leave(bool resumeAudio, const char *why) {
  if (!_active) return;
  // An upload still open means the browser was cut off mid-file, and hUploadDone() may never run for it (the
  // browser aborts the request).  abortUpload() closes it and takes the partial file away, and leaves the reason
  // set so a late hUploadDone() reports it instead of a success.  One owner, so the stall branch in loop() and
  // this close cannot drift apart.
  abortUpload("interrupted", 409, false);
  // Nothing may resume into a mode that has ended, and a partial kept for a resume must go with it - the page is
  // not coming back for it.
  discardResume();
  _active = false;
  _busy = false;   // backstop: a handler that died mid-operation must not be able to lock the mode closed
  // Hand the card back at the PLAYER's clock: a session may have stepped down, and the player's reads want the
  // high speed.  This is the second boundary a remount is allowed at (see SDSPISPEED_MANAGER).  Skipped when the
  // card has already left the slot - there is nothing to remount.
  /* ANY HANDLE THE MANAGER STILL HOLDS MUST BE CLOSED FIRST - the same rule as enter(), on the way out.  A listing
     keeps its directory open across the whole walk (`_listDir`), and a listing the user abandoned can still be open
     when the mode closes; the remount below discards the filesystem under it, and a later close would corrupt the
     heap exactly as the audio handle did on the way in.  Closed here rather than waited for. */
  if (_listDir) _listDir.close();
  if (resumeAudio && sdman.ready) sdman.ensureSpeed(SDSPISPEED);
  display.putRequest(NEWMODE, PLAYER);
  /* THE MANAGER ALWAYS HANDS BACK TO SD MODE.  The resume target used to be read from config.getMode() - whatever the
     device was last in - so a session opened while a web stream was playing put the radio back on that stream on exit,
     which is not something "leaving the file manager" can mean.  The mode is forced here, BEFORE the decision that
     uses it, and the web branch is gone: this mode exists to edit the CARD, so it returns to the card.  Only when the
     card is actually mounted - with no card there is nothing to be in SD mode FOR, and the player's own fallback
     applies. */
  if (sdman.ready) {
    config.saveValue(&config.store.play_mode, static_cast<uint8_t>(PM_SDCARD));
    config.syncSDFS();
  }
  // SmartStart hands the audio back: the card plays by station index.  It never resumes once the card has changed,
  // because a station NUMBER points at a different file after the list moves, and the re-index that follows picks a
  // station at random anyway.
  const bool smartStart = resumeAudio && _wasPlaying && config.store.smartstart;
  const bool cardChanged = smartStart && _cardChanged;
  if (smartStart && !cardChanged && sdman.ready) {
    FUNCTIONLOG("SDFileManager", "Resuming card playback (offset %lu)", (unsigned long)config.sdResumePos);
    player.sendCommand({PR_PLAY, config.lastStation()});
  } else {
    /* ONE line for the whole close, naming the REASON.  The idle timeout, the Done button and the card leaving the
       slot used to print a line each - above and below this one - whose only difference was which branch printed
       first.  The four numbers say why the player was not handed back, which is the part worth knowing. */
    FUNCTIONLOG("SDFileManager", "Closed the SD File Manager (%s) without resuming: was playing %d, smartstart %d, resume allowed %d, card changed %d",
                why, (int)_wasPlaying, (int)config.store.smartstart, (int)resumeAudio, (int)cardChanged);
  }
}

void FileManager::loop() {
  // A session's mutations owe ONE re-index, and it is paid here: after the mode has closed, never while the
  // manager is open and never from a request handler.  A hundred deletes therefore cost one walk instead of a
  // hundred, the walk is visible on the SDCHANGE counting screen, and no handler blocks the AsyncTCP task on
  // it.  The card must still be there - a card that left the slot took the derived files with it, so the debt
  // is simply dropped.  This runs on the main loop, which calls loop() whether or not the mode is open.
  if (!_active && _cardChanged && (int32_t)(millis() - _reindexNotBeforeMs) >= 0) {
    _cardChanged = false;
    if (sdman.ready && config.getMode() == PM_SDCARD) {
      /* Before the first flash write, so the player page can blank and spin for the whole walk: it is told "a
         build is running" here and "it is done" at the end of the block.  Sent as a flag read back by the case in
         netserver, because a page that connects mid-walk must be told the same thing by the connect notice. */
      _rebuilding = true;
      netserver.requestOnChange(PLAYLISTREADY, 0);
      FUNCTIONLOG("SDFileManager", "Re-indexing the card after a change");
      display.putRequest(NEWMODE, SDCHANGE);   // same handshake as changeMode(): show the screen, then wait for it
      const unsigned long waitStart = millis();
      while (display.mode() != SDCHANGE && millis() - waitStart < 2000) delay(10);
      config.initSDPlaylist(true);             // forced: the index file is exactly what the mutations dropped
      display.putRequest(NEWMODE, PLAYER);
      // After the mode switch, because a screen we own drops both requests: the meta and title lines are derived
      // from station.name/title, which the re-index above may just have reset to the "nothing to play" state.
      display.putRequest(NEWSTATION);
      display.putRequest(NEWTITLE);
      /* The list is as ready as this pass can make it, so let the player page unlock and fetch.  Sent even when the
         build above failed, because that page must never wait for a build to SUCCEED. */
      netserver.requestOnChange(PLAYLISTREADY, 0);   // and the flag is already false again: this one says "ready"
      _rebuilding = false;
      /* ...but a failed build is owed another pass: it left no index at all, which is the state initSDPlaylist()
         repairs at mode entry.  The flag was consumed at the top of this block, so it is set again here, with a
         delay and a cap so a card that cannot be written does not turn loop() into an endless walk. */
      if (!sdman.exists(INDEX_SD_PATH)) {
        if (_reindexTries < SD_REINDEX_MAX_RETRIES) {
          _reindexTries++;
          _cardChanged = true;
          _reindexNotBeforeMs = millis() + SD_REINDEX_RETRY_MS;
          FUNCTIONLOG("SDFileManager", "No index was written: retry %u of %u in %lums",
                      (unsigned)_reindexTries, (unsigned)SD_REINDEX_MAX_RETRIES, (unsigned long)SD_REINDEX_RETRY_MS);
        } else {
          FUNCTIONLOG("SDFileManager", "No index after %u attempts: leaving it empty until the next mode entry",
                      (unsigned)_reindexTries);
        }
      } else {
        _reindexTries = 0;
      }
    }
  }

  if (!_active) return;

  /* A card leaving the slot ends the mode: every path in the UI would 404, and the player needs the device back to
     fall back to web mode.  Several failures are needed, because the probe reads sector 0 through the SD host that
     FATFS writes use - a card still busy after a write times out one probe, and a folder delete is a burst of
     writes, which is when a single failure used to end the mode mid-delete.
     The probe itself is a RAW sector read on the SPI bus (readRAW via diskio_impl), and this function runs about
     60 times a second - so this used to issue ~60 card commands a second straight through a transfer being written
     from the AsyncTCP task, and through every delete.  That is the strongest candidate for the mid-write EIO
     (errno 5) that followed the upload path rather than any particular card: a brand new Sandisk failed exactly
     like the old one, while a full 8.6 MB upload succeeded when nothing else was touching the bus.
     So two rules.  A transfer in flight is itself the proof that the card is present, so nothing is probed while an
     upload is open or a delete batch is running.  Otherwise it is probed at most once a second, which is all three
     strikes ever needed - at 60 Hz the strikes were spent inside 50 ms, so the debounce was not a debounce. */
  if (sdman.ready && !uploadOpen() && !_busy && (millis() - _lastCardCheckMs) >= 1000) {
    _lastCardCheckMs = millis();
    if (sdman.cardPresent()) {
      _cardGone = 0;
    } else if (++_cardGone >= SDMAN_CARD_GONE_STRIKES) {
      FUNCTIONLOG("SDFileManager", "Card gone for %u checks, closing", (unsigned)_cardGone);
      _cardGone = 0;
      leave(false, "card removed");   // nothing to give back - the media it would play from is what vanished
      return;
    }
  }

  // At most once a second: the main loop turns over far faster than the line changes.
  if (millis() - _lastCountdownMs >= 1000) {
    _lastCountdownMs = millis();
    display.sdmanCountdown();
  }

  /* An upload that has gone quiet is released HERE, and this is the one close allowed over an open upload.  It
     is bounded; the 180 s user clock is not, and that is what used to end a live transfer: the mode closed
     underneath a chunk that was slow to return, leave() took the half-written file with it, and every remaining
     file of the batch then answered not_active.  The numbers are logged because "490496 bytes" and "0 bytes" are
     the same failure until you can see how the transfer got there.
     Never while a write is executing (_upInWrite): closing a handle another task is inside is the cross-task
     hazard the log ring was fixed for.  A wedged write is left to the task watchdog instead, which is fed
     outside the write and will therefore reset the device on its own.  The reason is set before the close so a
     late hUploadDone() reports "stalled" rather than a success. */
  /* The quiet time is read ONCE, before the test, and the value tested is the value logged.  Reading it again in
     the arguments was the same mistake the idle line had: a chunk arriving from the AsyncTCP task between the test
     and the print made a branch that had seen 30 s of silence report "quiet for 0ms", so the log denied the very
     thing it had just acted on. */
  const uint32_t quietMs = uploadIdleMs();
  const uint32_t quietBudget = (_upReason ? SDMAN_UPLOAD_STALL_FAILED_MS : SDMAN_UPLOAD_STALL_MS);
  if (_active && uploadOpen() && quietMs >= quietBudget) {
    if (_upInWrite) {
      if (_upReason == nullptr) {
        _upReason = "stalled";
        _upCode = 409;
        FUNCTIONLOG("SDFileManager", "No chunks for %lums (%lu bytes in), but a write is in flight - leaving it to the watchdog",
                    (unsigned long)quietMs, (unsigned long)_upBytes);
      }
    } else {
      FUNCTIONLOG("SDFileManager", "No chunks for %lums (%lu bytes in) - releasing the transfer so the page can resume it",
                  (unsigned long)quietMs, (unsigned long)_upBytes);
      abortUpload("stalled", 409, true);   // the card may be perfectly well; only the browser went quiet
    }
  }

  /* The user clock.  An upload holds the mode open (the branch above cannot fire over a transfer in flight), and the
     close prints its own single line - so this branch has nothing to report of its own.  It used to print the
     elapsed figures here as well, which is how "idle for 180000ms ... elapsed 1" came to exist; leave() names the
     reason and the state now, and the arithmetic is one clock's so it cannot disagree with itself. */
  if (_active && !uploadOpen() && idleRemainingMs() == 0) {
    leave(true, "idle timeout");
  }
}

/* POST /sdman/spimarker - write the marker that blocks uploads on this card.  Called by the page when it has given up
   on a batch for a CARD-side reason, so the card carries the verdict even if the user comes back with a different
   browser or a different device.  On SDMMC it does NOTHING but answer ok: the page's flow is then the same on both
   transports, and no card is ever blamed for a reader it is not plugged into. */
static void hSpiMarker(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  #if defined(SD_USE_MMC)
    sendOk(request);
    return;
  #else
    if (!sdman.exists(SD_DATA_DIR)) sdman.mkdir(SD_DATA_DIR);
    File marker = sdman.open(SDMAN_SPI_MARKER, FILE_WRITE);
    if (!marker) { sendError(request, 500, "open_failed"); return; }
    const uint32_t au = sdman.allocationUnit();
    marker.print(F("This SD card has problems with uploads on an SPI reader.\n"));
    /* THE REMEDY, printed only when it actually applies.  A card formatted with a large allocation unit is the usual
       cause of this failure and 512-byte units fix it, but on a card already formatted that way the sentence would be
       nonsense - and the marker outlives the session it was written in, so it has to be true when read later. */
    if (au != 0 && au != 512) marker.print(F("Format this card with 512-byte allocation units.\n"));
    marker.print(F("https://trip5.github.io/ehRadio/Hardware/#sd-card-reader\n"));
    marker.print(F("Delete this file to enable uploads.\n"));
    marker.close();
    FUNCTIONLOG("SDFileManager", "Marked this card unusable for SPI uploads (%s)", SDMAN_SPI_MARKER);
    sendOk(request);
  #endif
}

// ==== Routes ====

void FileManager::registerRoutes(AsyncWebServer &server) {
  server.on("/sdman/enter", HTTP_GET, hEnterApi);
  server.on("/sdman/done", HTTP_POST, hDone);
  server.on("/sdman/info", HTTP_GET, hInfo);
  server.on("/sdman/list", HTTP_GET, hList);
  server.on("/sdman/mkdir", HTTP_POST, hMkdir);
  server.on("/sdman/rename", HTTP_POST, hRename);
  server.on("/sdman/move", HTTP_POST, hMove);
  server.on("/sdman/delete", HTTP_POST, hDelete, nullptr, onDeleteBody);
  server.on("/sdman/download", HTTP_GET, hDownload);
  // Writes the SPI upload marker, after which uploads on this card are refused.  POST, no body.
  server.on("/sdman/spimarker", HTTP_POST, hSpiMarker);
  // Its own upload handler takes precedence over the global one, which keeps /sdman multipart data out of
  // the LittleFS /webboard path.
  server.on("/sdman/upload", HTTP_POST, hUploadDone, onUploadChunk);
}

#endif  // USE_SD
