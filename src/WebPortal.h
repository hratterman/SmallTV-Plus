// WebPortal.h — HTTP config UI, REST API, and OTA endpoint
#pragma once
#include <Arduino.h>
#include "Settings.h"

void webPortalBegin(Settings& settings);
void webPortalLoop();
bool webPortalRebootDue();   // main polls this and calls ESP.restart()

// Firmware over the tether cable (see Tether.cpp for the frames). begin gets
// the total image size; write returns nullptr to ack or the reason to stop;
// end returns nullptr when flashed (a reboot is already scheduled).
const char* otaCableBegin(uint32_t totalSize);
const char* otaCableWrite(const uint8_t* p, uint16_t n);
const char* otaCableEnd();
