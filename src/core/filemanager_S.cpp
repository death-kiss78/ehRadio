#include <FS.h>
#include <SD.h>
#include <WebServer.h>
#include "sdmanager.h"
#include "filemanager_S.h"

// Server pe portul 8080
WebServer fmServer(8080);

// Helper: asigură SD-ul pornit
static inline bool ensureSD() {
    if (!sdman.ready) {
        sdman.start();
    }
    return sdman.ready;
}

// -------------------------
// Recursive delete
// -------------------------
bool fms_deleteRecursive(const char *path) {
    ensureSD();
    File entry = sdman.open(path);
    if (!entry) return false;

    if (!entry.isDirectory()) {
        entry.close();
        return sdman.remove(path);
    }

    File file = entry.openNextFile();
    while (file) {
        String entryPath = String(path);
        if (!entryPath.endsWith("/")) entryPath += "/";
        entryPath += file.name();

        if (file.isDirectory()) {
            file.close();
            fms_deleteRecursive(entryPath.c_str());
        } else {
            file.close();
            sdman.remove(entryPath.c_str());
        }

        file = entry.openNextFile();
    }

    entry.close();
    return sdman.rmdir(path);
}

// -------------------------
// List directory
// -------------------------
void fms_handleList() {
    ensureSD();

    String path = fmServer.hasArg("path") ? fmServer.arg("path") : "/";
    if (path == "") path = "/";

    if (!sdman.exists(path)) {
        fmServer.send(404, "application/json", "{\"error\":\"Path not found\"}");
        return;
    }

    File dir = sdman.open(path);
    if (!dir || !dir.isDirectory()) {
        fmServer.send(400, "application/json", "{\"error\":\"Not a directory\"}");
        return;
    }

    String json = "[";
    File entry = dir.openNextFile();
    bool first = true;

    while (entry) {
        if (!first) json += ",";
        first = false;

        String name = entry.name();
        if (name.startsWith(path) && path != "/") {
            name = name.substring(path.length());
            if (name.startsWith("/")) name.remove(0, 1);
        }

        json += "{";
        json += "\"name\":\"" + name + "\",";
        json += "\"type\":\"" + String(entry.isDirectory() ? "dir" : "file") + "\"";

        if (!entry.isDirectory()) {
            json += ",\"size\":" + String(entry.size());
        }

        json += "}";

        entry = dir.openNextFile();
    }

    json += "]";
    fmServer.send(200, "application/json", json);
}

// -------------------------
// Upload
// -------------------------
void fms_handleUpload() {
    ensureSD();

    if (!fmServer.hasArg("path")) {
        fmServer.send(400, "text/plain", "Missing path");
        return;
    }

    String basePath = fmServer.arg("path");
    if (basePath == "") basePath = "/";
    if (!basePath.startsWith("/")) basePath = "/" + basePath;

    HTTPUpload &upload = fmServer.upload();
    static File uploadFile;

    if (upload.status == UPLOAD_FILE_START) {
        String filename = basePath;
        if (!filename.endsWith("/")) filename += "/";
        filename += upload.filename;

        if (sdman.exists(filename)) sdman.remove(filename);

        uploadFile = sdman.open(filename, FILE_WRITE);
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    }
    else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
        fmServer.send(200, "text/plain", "OK");
    }
}

// -------------------------
// Download
// -------------------------
void fms_handleDownload() {
    ensureSD();

    if (!fmServer.hasArg("file")) {
        fmServer.send(400, "text/plain", "Missing file");
        return;
    }

    String path = fmServer.arg("file");
    if (!path.startsWith("/")) path = "/" + path;

    if (!sdman.exists(path)) {
        fmServer.send(404, "text/plain", "File not found");
        return;
    }

    File file = sdman.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        fmServer.send(400, "text/plain", "Not a file");
        return;
    }

    String fname = file.name();
    int slash = fname.lastIndexOf('/');
    if (slash >= 0) fname = fname.substring(slash + 1);

    fmServer.sendHeader("Content-Type", "application/octet-stream");
    fmServer.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    fmServer.sendHeader("Connection", "close");

    fmServer.streamFile(file, "application/octet-stream");
    file.close();
}

// -------------------------
// Delete
// -------------------------
void fms_handleDelete() {
    ensureSD();

    if (!fmServer.hasArg("path")) {
        fmServer.send(400, "text/plain", "Missing path");
        return;
    }

    String path = fmServer.arg("path");
    if (!path.startsWith("/")) path = "/" + path;

    if (!sdman.exists(path)) {
        fmServer.send(404, "text/plain", "Not found");
        return;
    }

    bool ok = fms_deleteRecursive(path.c_str());
    fmServer.send(200, "text/plain", ok ? "OK" : "FAIL");
}

// -------------------------
// Rename
// -------------------------
void fms_handleRename() {
    ensureSD();

    if (!fmServer.hasArg("old") || !fmServer.hasArg("new")) {
        fmServer.send(400, "text/plain", "Missing args");
        return;
    }

    String oldPath = fmServer.arg("old");
    String newPath = fmServer.arg("new");

    if (!oldPath.startsWith("/")) oldPath = "/" + oldPath;
    if (!newPath.startsWith("/")) newPath = "/" + newPath;

    if (!sdman.exists(oldPath)) {
        fmServer.send(404, "text/plain", "Not found");
        return;
    }

    bool ok = sdman.rename(oldPath, newPath);
    fmServer.send(200, "text/plain", ok ? "OK" : "FAIL");
}

// -------------------------
// Move
// -------------------------
void fms_handleMove() {
    ensureSD();

    if (!fmServer.hasArg("src") || !fmServer.hasArg("dst")) {
        fmServer.send(400, "text/plain", "Missing args");
        return;
    }

    String src = fmServer.arg("src");
    String dst = fmServer.arg("dst");

    if (!src.startsWith("/")) src = "/" + src;
    if (!dst.startsWith("/")) dst = "/" + dst;

    bool ok = sdman.rename(src, dst);
    fmServer.send(200, "text/plain", ok ? "OK" : "FAIL");
}

// -------------------------
// Mkdir
// -------------------------
void fms_handleMkdir() {
    ensureSD();

    if (!fmServer.hasArg("path")) {
        fmServer.send(400, "text/plain", "Missing path");
        return;
    }

    String path = fmServer.arg("path");
    if (!path.startsWith("/")) path = "/" + path;

    bool ok = sdman.mkdir(path);
    fmServer.send(200, "text/plain", ok ? "OK" : "FAIL");
}

// -------------------------
// Info
// -------------------------
void fms_handleInfo() {
    ensureSD();

    uint64_t total = sdman.cardSize();
    uint64_t used  = sdman.usedBytes();
    uint64_t free  = total - used;

    String json = "{";
    json += "\"total\":" + String(total) + ",";
    json += "\"used\":"  + String(used)  + ",";
    json += "\"free\":"  + String(free);
    json += "}";

    fmServer.send(200, "application/json", json);
}

// -------------------------
// Register routes + index + static files
// -------------------------
void fms_registerRoutes() {

    // API routes
    fmServer.on("/list_s", HTTP_GET, fms_handleList);
    fmServer.on("/download_s", HTTP_GET, fms_handleDownload);
    fmServer.on("/delete_s", HTTP_GET, fms_handleDelete);
    fmServer.on("/rename_s", HTTP_GET, fms_handleRename);
    fmServer.on("/move_s", HTTP_GET, fms_handleMove);
    fmServer.on("/mkdir_s", HTTP_GET, fms_handleMkdir);
    fmServer.on("/info_s", HTTP_GET, fms_handleInfo);

    fmServer.on("/upload_s", HTTP_POST,
        []() { fmServer.send(200, "text/plain", "OK"); },
        fms_handleUpload);

    // INDEX.HTML
    fmServer.on("/", HTTP_GET, []() {
        ensureSD();
        File file = sdman.open("/index.html");
        if (!file) {
            fmServer.send(500, "text/plain", "index.html missing");
            return;
        }
        fmServer.streamFile(file, "text/html");
        file.close();
    });

    // Static files
    fmServer.onNotFound([]() {
        ensureSD();

        String path = fmServer.uri();

        if (sdman.exists(path)) {
            File file = sdman.open(path);

            if (path.endsWith(".html")) fmServer.streamFile(file, "text/html");
            else if (path.endsWith(".css")) fmServer.streamFile(file, "text/css");
            else if (path.endsWith(".js")) fmServer.streamFile(file, "application/javascript");
            else if (path.endsWith(".png")) fmServer.streamFile(file, "image/png");
            else if (path.endsWith(".jpg")) fmServer.streamFile(file, "image/jpeg");
            else if (path.endsWith(".svg")) fmServer.streamFile(file, "image/svg+xml");
            else fmServer.streamFile(file, "text/plain");

            file.close();
        } else {
            fmServer.send(404, "text/plain", "Not found");
        }
    });
}
