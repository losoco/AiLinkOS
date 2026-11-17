#pragma once

#include <WiFi.h>

class GfxRenderer;

namespace NetworkStartup {

// One snapshot per network lifecycle transition; no persistent buffers.
void logMemory(const char* stage);

// Preserve render memory when PSRAM/internal SRAM headroom is healthy; otherwise
// release it before the Wi-Fi driver starts allocating.
void prepare(GfxRenderer& renderer);

// The only entry point for enabling STA/AP mode in application code.
bool setMode(GfxRenderer& renderer, wifi_mode_t mode);

// True while a background service (the screen mirror) keeps the WiFi session
// alive. Foreground network activities must then stop only their own server
// and leave disconnect / WIFI_OFF / esp_wifi_deinit to the owner.
bool ownedByBackgroundService();

// Re-register the background owner's mDNS host after a foreground activity
// ended mDNS it did not own (Calibre and File Transfer re-bind crosspoint.local).
void restoreBackgroundMdns();

// End the background owner's mDNS while the STA netif is still valid, before a
// WiFi restart invalidates esp-mdns' stored netif pointer.
void suspendBackgroundMdns();

}  // namespace NetworkStartup
