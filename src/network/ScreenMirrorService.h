#pragma once

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include "Memory.h"

class GfxRenderer;

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
  void tick(GfxRenderer& renderer);
  // Stops servers, mDNS and WiFi. Safe to call when already off.
  void stop();

  bool enabled() const { return state_ != State::Off; }
  State state() const { return state_; }
  const char* pin() const { return pin_; }
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
  void handleWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsThunk(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void pumpFrame(GfxRenderer& renderer);
  bool captureFrame(GfxRenderer& renderer);

  std::unique_ptr<WebServer> http_;
  std::unique_ptr<WebSocketsServer> ws_;

  // Frame snapshot (panel layout) and one packet (24-byte header + RLE payload).
  // Both live in PSRAM and are released by stop(); ~48 KB + ~96 KB while enabled.
  memory::ByteBuffer frame_;
  memory::ByteBuffer packet_;
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
  uint8_t clients_ = 0;
  uint32_t observedSeq_ = 0;
  uint32_t observedAt_ = 0;
  uint32_t sentSeq_ = 0;
  uint32_t framesSent_ = 0;
  bool mdnsStarted_ = false;
};

#define SCREEN_MIRROR ScreenMirrorService::instance()

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
