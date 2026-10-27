#pragma once

// =============================================================================
// custom_version.h — Eigene Build-Informationen
// Bei jeder Änderung CUSTOM_BUILD_VERSION hochzählen und docs/CHANGELOG ergänzen.
// =============================================================================

#define CUSTOM_BUILD_VERSION "1.2.0"
#define CUSTOM_BUILD_FEATURES                                                  \
  "DS3231-RTC | NTP-Boot-Sync | SD-Aggregation(10min) | ISO8601-Lokalzeit | "  \
  "Restart-safe-Summary"
#define CUSTOM_BUILD_DATE __DATE__
#define CUSTOM_BUILD_TIME __TIME__
