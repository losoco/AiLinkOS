#pragma once

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include <atomic>

#include "Memory.h"

class GfxRenderer;
class MappedInputManager;

// LAN screen mirror: advertises ailinkos.local, serves a small viewer page and
// pushes the panel framebuffer over WebSocket whenever the renderer commits a
// frame. Opt-in from the Apps menu; runs as a background service so the user can
// keep reading while a browser follows the screen. S3-only (needs PSRAM for the
// frame + RLE buffers).
class ScreenMirrorService {
 public:
  enum class State : uint8_t { Off, WifiConnecting, Running, Error };

  static ScreenMirrorService& instance();

  // Brings WiFi up with the last saved credentials and starts mDNS/servers.
  // Returns false when no credentials or frame buffers are available.
  bool start(GfxRenderer& renderer);
  // Pumped from the main loop: WiFi state machine, HTTP/WS polling, frame push.
  // `input` receives injected key clicks from the viewer page.
  void tick(GfxRenderer& renderer, MappedInputManager& input);
  // Stops servers, mDNS and WiFi. Safe to call when already off.
  void stop();
  // Re-registers ailinkos.local after a foreground network activity (Calibre,
  // File Transfer) re-bound mDNS to its own host name and then exited.
  void restoreMdns();
  // Ends mDNS while the STA netif is still valid. esp-mdns keeps a netif
  // pointer that a WiFi re-init invalidates, so a later MDNS.end() would fault
  // inside join_group(); call this before another flow restarts WiFi.
  void suspendMdns();

  bool enabled() const { return state_ != State::Off; }
  State state() const { return state_; }
  const char* pin() const { return pin_; }
  // LAN viewers must prove the PIN first. Off by default; toggled from
  // Settings → System, so an untrusted network can opt in.
  bool pinRequired() const;
  const char* errorText() const { return error_; }
  uint8_t clientCount() const { return clients_; }
  uint32_t framesSent() const { return framesSent_; }
  IPAddress ip() const { return WiFi.localIP(); }

 private:
  ScreenMirrorService() = default;
  ~ScreenMirrorService() = default;
  ScreenMirrorService(const ScreenMirrorService&) = delete;
  ScreenMirrorService& operator=(const ScreenMirrorService&) = delete;

  bool ensureBuffers(GfxRenderer& renderer);
  void startServers();
  void stopServers();
  void sendStatusJson();
  bool handleTextCommand(uint8_t num, const char* text);
  void acceptViewer(uint8_t num);
  void handleWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsThunk(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  struct FrameSlot {
    memory::ByteBuffer buf;
    uint32_t seq = 0;
    uint16_t logicalW = 0;
    uint16_t logicalH = 0;
    uint8_t orientation = 0;
    bool inverted = false;
  };
  // Commit hook: runs on the render task right after the panel finishes a
  // frame, so the snapshot never competes for RenderLock with the next repaint.
  static void commitThunk(void* ctx);
  void onFrameCommitted();
  bool captureToSlot();
  void pumpFrame();
  void sendSlot(uint8_t slot);

  std::unique_ptr<WebServer> http_;
  std::unique_ptr<WebSocketsServer> ws_;
  // Set on every tick; used to inject KEY commands from authenticated viewers.
  MappedInputManager* input_ = nullptr;
  GfxRenderer* renderer_ = nullptr;

  // Double-buffered snapshots (panel layout), the last snapshot actually sent,
  // and one packet (32-byte header + RLE payload). All live in PSRAM and are
  // released by stop(); ~144 KB while enabled.
  FrameSlot slots_[2];
  memory::ByteBuffer last_;
  memory::ByteBuffer packet_;
  std::atomic<uint8_t> readySlot_{0};
  std::atomic<bool> newFrame_{false};
  std::atomic<bool> forceSnapshot_{false};
  uint8_t writeSlot_ = 1;
  bool haveLast_ = false;
  size_t frameBytes_ = 0;
  size_t rowBytes_ = 0;
  uint16_t panelW_ = 0;
  uint16_t panelH_ = 0;

  State state_ = State::Off;
  uint32_t stateSince_ = 0;
  uint32_t lastWifiMs_ = 0;
  char pin_[7] = {};
  char error_[48] = {};
  bool authed_[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
  uint32_t lastSeenMs_[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
  uint8_t clients_ = 0;
  uint32_t sentSeq_ = 0;
  uint32_t framesSent_ = 0;
  bool mdnsStarted_ = false;
};

#define SCREEN_MIRROR ScreenMirrorService::instance()

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
