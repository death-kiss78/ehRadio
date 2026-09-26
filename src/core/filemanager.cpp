#include "filemanager.h"

#ifdef USE_SD

#include <FS.h>
#include <vector>
#include "sdmanager.h"
#include "config.h"
#include "display.h"
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

static void sendError(AsyncWebServerRequest *request, int code, const char *reason) {
  String body = F("{\"ok\":false,\"error\":\"");
  body += reason;
  body += F("\"}");
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
  // The SD index is regenerated rather than restored, and the playlists and credentials live here too.
  if (path == "/" || path == "/data" || path.startsWith("/data/")) return true;
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

// The SD index is derived data, one offset per file. A mutation makes it wrong and deleting it is the
// cheapest correct answer: config.initSDPlaylist() rebuilds it on the next SD entry. A file rather than a
// RAM bit, so it survives a power cycle.
static void invalidateSdIndex() {
  if (sdman.exists(INDEX_SD_PATH)) {
    sdman.remove(INDEX_SD_PATH);
    FUNCTIONLOG("SDFileManager", "SD index dropped; it will be rebuilt on the next SD entry");
  }
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
    children.push_back(basenameOf(child.name()));  // name() is the full path on ESP32 Arduino
    child = entry.openNextFile();
  }
  entry.close();

  String base = path;
  if (!base.endsWith("/")) base += "/";
  for (const String &name : children) {
    if (!removeRecursive(base + name)) return false;
  }
  return sdman.rmdir(path);
}

// ==== Handlers ====

// GET /sdman/enter - opens the mode and is the only way in: the page calls it as it loads, so /sdmanager.html
// works as well as the address on the display. Idempotent; a second call just refreshes the idle clock.
// No route at bare "/sdman": a plain URI matches an exact path OR a prefix plus "/", so a handler there
// would also answer /sdman/list and every sibling. No network test either - entry used to need CONNECTED,
// which shut the feature out of AP mode, and _swichMode() already refuses mode changes unless the status is
// CONNECTED or SDOFFLINE. SD-offline needs no test because NetServer::begin() returns before the server.
static void hEnterApi(AsyncWebServerRequest *request) {
  filemanager.enter();
  sendOk(request);
}

// POST /sdman/done - the Done button at the foot of the page.
static void hDone(AsyncWebServerRequest *request) {
  filemanager.leave();
  request->redirect("/");
}

// GET /sdman/info - the header line and the empty/error states depend on this.
static void hInfo(AsyncWebServerRequest *request) {
  if (!requireActive(request)) return;
  filemanager.touch();
  uint64_t total = sdman.totalBytes();
  uint64_t used  = sdman.usedBytes();
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
  FUNCTIONLOG("SDFileManager", "mkdir %s", path.c_str());
  invalidateSdIndex();
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

  FUNCTIONLOG("SDFileManager", "%s %s -> %s", sameDirectory ? "rename" : "move", from.c_str(), target.c_str());
  invalidateSdIndex();
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
  int deleted = 0, failed = 0;

  int start = 0;
  while (start < (int)_deleteBody.length()) {
    int nl = _deleteBody.indexOf('\n', start);
    if (nl < 0) nl = _deleteBody.length();
    String raw = _deleteBody.substring(start, nl);
    start = nl + 1;
    raw.trim();
    if (raw.length() == 0) continue;

    String path = normalisePath(raw);
    // One count on the way out, but the log keeps them apart: a protected item and a failed rmdir differ.
    if (FileManager::isProtected(path) || filemanager.isPlaying(path)) {
      FUNCTIONLOG("SDFileManager", "refused to delete %s (protected or in use)", path.c_str());
      failed++;
      continue;
    }
    if (!sdman.exists(path)) {
      FUNCTIONLOG("SDFileManager", "nothing to delete at %s", path.c_str());
      failed++;
      continue;
    }
    if (removeRecursive(path)) {
      deleted++;
      FUNCTIONLOG("SDFileManager", "deleted %s", path.c_str());
    } else {
      failed++;
      FUNCTIONLOG("SDFileManager", "could not delete %s", path.c_str());
    }
  }
  _deleteBody = "";

  if (deleted) invalidateSdIndex();

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
      } else {
        _listDir.close();   // the walk is over
        _listOut = F("]}");
        _listDone = true;
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
static bool     _upSkipped = false;
static const char *_upReason = nullptr;
static int      _upCode = 400;

static void onUploadChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  // Re-tested per chunk, not just at the start: the mode can end (timeout, card removal) mid-file. The
  // reason is reported by the request handler at the end.
  if (!filemanager.active()) { _upReason = "not_active"; _upCode = 409; return; }
  // Every chunk refreshes the idle clock, so a slow upload cannot look like an idle browser.
  filemanager.touch();

  if (index == 0) {
    _upReason = nullptr;
    _upFree = 0;
    _upSkipped = false;
    // A cut-off attempt leaves the handle open and the file half-written; closing it here is what the
    // missing UPLOAD_FILE_ABORTED notification would otherwise do.
    if (_upFile) { _upFile.close(); }
    _upPath = "";

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
      if (FileManager::isProtected(target))      { _upReason = "protected";   _upCode = 403; }
      else if (filemanager.isPlaying(target))    { _upReason = "in_use";      _upCode = 409; }
      else if (skip && sdman.exists(target)) {
        // Skip Existing: nothing is opened, so nothing is truncated and no room is reserved for a write that
        // will not happen. The request answers "skipped" and the page names the file after the batch.
        _upSkipped = true;
      }
      else {
        // No exists() test here: opening with FILE_WRITE truncates, so uploading over a name replaces it -
        // which is the point of re-uploading a corrected track. Protection and in-use are the only refusals.
        _upFile = sdman.open(target, FILE_WRITE);
        if (!_upFile) { _upReason = "open_failed"; _upCode = 500; }
        else {
          // Measured AFTER the open: the open truncates, which is what releases the room the replacement
          // needs. Measuring first would refuse an overwrite that has plenty of space once the old copy goes.
          uint64_t total = sdman.totalBytes();
          uint64_t used  = sdman.usedBytes();
          _upFree = (total > used) ? (total - used) : 0;
          _upPath = target;
        }
      }
    }
  }

  if (_upReason == nullptr && len) {
    // Stop at the free-space limit rather than filling the card: a half-written file the user is told about
    // beats a full disk. 507 is the storage audit's code and the page has its own message for it.
    if (_upFree && (_upFile.position() + len) > _upFree) {
      _upReason = "no_space";
      _upCode = 507;
    } else if (_upFile) {
      _upFile.write(data, len);
    }
  }

  if (final && _upFile) {
    _upFile.close();
  }
}

// The request handler runs after the last chunk, so the outcome is reported here.
static void hUploadDone(AsyncWebServerRequest *request) {
  filemanager.touch();
  // Before the mode test below, and before the index invalidation: a skip changed nothing, so there is no
  // write to protect and the playlist and index must stay exactly as they were.
  if (_upReason == nullptr && _upSkipped) {
    _upSkipped = false;
    _upPath = "";
    FUNCTIONLOG("SDFileManager", "upload skipped, name already present");
    sendJson(request, 200, F("{\"ok\":true,\"skipped\":true}"));
    return;
  }
  if (_upReason != nullptr) {
    // Nothing usable was written: drop any partial file, so a failed upload leaves no truncated track.
    if (_upPath.length()) sdman.remove(_upPath);
    const char *reason = _upReason;
    int code = _upCode;
    _upReason = nullptr;
    _upPath = "";
    ERRORLOG("SDFileManager: upload refused (%s)", reason);
    sendError(request, code, reason);
    return;
  }
  // Still answered inside the mode: "ok" from a mode that has since closed would leave the page thinking it
  // can carry on reading the card.
  if (!requireActive(request)) return;
  FUNCTIONLOG("SDFileManager", "uploaded %s", _upPath.c_str());
  _upPath = "";
  invalidateSdIndex();
  sendOk(request);
}

// ==== Mode ====

void FileManager::touch() {
  _lastActivity = millis();
}

uint32_t FileManager::idleRemainingMs() const {
  if (!_active) return 0;
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
    player.sendCommand({PR_STOP, 0});
  }
  // Mount on demand - the one point where we know the user wants the card - and after the stop above, so it
  // cannot fight the player for the volume.
  if (!sdman.ready) sdman.start();
  _lastActivity = millis();
  if (_active) return;
  _active = true;
  display.putRequest(NEWMODE, SDMAN);
  FUNCTIONLOG("SDFileManager", "open (SD %s)", sdman.ready ? "mounted" : "NOT mounted");
}

void FileManager::leave(bool resumeAudio) {
  if (!_active) return;
  _active = false;
  display.putRequest(NEWMODE, PLAYER);
  // SmartStart hands the audio back, with the same two branches as stopStandby(): a web stream resumes from
  // its saved URL, the card plays by station index. The card needs nothing else - _stop() saved the byte
  // offset and the play path resumes from it, and a stale offset after an edit goes to the player's own
  // self-healing.
  if (resumeAudio && _wasPlaying && config.store.smartstart) {
    FUNCTIONLOG("SDFileManager", "resuming %s playback (offset %lu)",
                config.getMode() == PM_WEB ? "web" : "card", (unsigned long)config.sdResumePos);
    if (config.getMode() == PM_WEB) player.resumeLastWebSource();
    else player.sendCommand({PR_PLAY, config.lastStation()});
  } else {
    // Names which condition was false, so a silent close differs from a resume that failed in the player.
    FUNCTIONLOG("SDFileManager", "closed without resume (was playing %d, smartstart %d, allowed %d)",
                (int)_wasPlaying, (int)config.store.smartstart, (int)resumeAudio);
  }
  FUNCTIONLOG("SDFileManager", "closed");
}

void FileManager::loop() {
  if (!_active) return;

  // A card leaving the slot ends the mode: every path in the UI would 404, and the player needs the device
  // back to fall back to web mode. cardPresent() is the player's PR_CHECKSD probe, so both transports agree.
  // Several failures are needed: the probe reads sector 0 through the SD host that FATFS writes use, so a
  // card still busy after a write times out one probe - and a folder delete is a burst of writes, which is
  // when a single failure used to end the mode mid-delete. A card that never mounted is left to the timeout.
  if (sdman.ready) {
    if (sdman.cardPresent()) {
      _cardGone = 0;
    } else if (++_cardGone >= SDMAN_CARD_GONE_STRIKES) {
      FUNCTIONLOG("SDFileManager", "card gone for %u checks, closing", (unsigned)_cardGone);
      _cardGone = 0;
      leave(false);   // nothing to give back - the media it would play from is what vanished
      return;
    }
  }

  // At most once a second: the main loop turns over far faster than the line changes.
  if (millis() - _lastCountdownMs >= 1000) {
    _lastCountdownMs = millis();
    display.sdmanCountdown();
  }

  if (idleRemainingMs() == 0) {
    FUNCTIONLOG("SDFileManager", "idle for %lums, closing", (unsigned long)SDMAN_AUTO_EXIT_MS);
    leave();
  }
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
  // Its own upload handler takes precedence over the global one, which keeps /sdman multipart data out of
  // the LittleFS /webboard path.
  server.on("/sdman/upload", HTTP_POST, hUploadDone, onUploadChunk);
}

#endif  // USE_SD
