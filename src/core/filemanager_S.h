#pragma once
#include <Arduino.h>
#include <SD.h>
#include <WebServer.h>

// WebServer separat pe port 8080
extern WebServer fmServer;

// Rute publice
void fms_handleList();
void fms_handleUpload();
void fms_handleDownload();
void fms_handleDelete();
void fms_handleRename();
void fms_handleMove();
void fms_handleMkdir();

// Inițializare rute
void fms_registerRoutes();

bool fms_deleteRecursive(const char *path);
