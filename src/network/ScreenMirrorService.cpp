#include "ScreenMirrorService.h"

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"

namespace {

constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kWsPort = 81;
constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kFrameDebounceMs = 200;
constexpr uint32_t kWifiLossGraceMs = 30000;
constexpr uint8_t kMaxViewers = 2;
constexpr size_t kHeaderBytes = 24;
constexpr char kHostName[] = "ailinkos";

const char kPageHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ailinkOS screen</title>
<style>
body{margin:0;background:#111;color:#ddd;font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;display:flex;flex-direction:column;align-items:center;gap:10px;padding:12px}
canvas{max-width:100%;height:auto;background:#fff;border:1px solid #444;image-rendering:pixelated}
#s{font-size:12px;color:#9a9}
button{background:#222;color:#ddd;border:1px solid #555;border-radius:6px;padding:6px 12px}
</style></head><body>
<canvas id="c" width="480" height="800"></canvas>
<div id="s">connecting…</div>
<div><button id="again">重新连接</button></div>
<script>
let ws=null, pin=sessionStorage.getItem('mirrorPin')||'';
const s=document.getElementById('s'), c=document.getElementById('c'), ctx=c.getContext('2d');
function set(t){s.textContent=t;}
function draw(buf){
  const dv=new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if(buf[0]!==65||buf[1]!==76||buf[2]!==77||buf[3]!==1) return;
  const seq=dv.getUint32(4,true), pw=dv.getUint16(8,true), ph=dv.getUint16(10,true),
        rb=dv.getUint16(12,true), ori=dv.getUint8(14), inv=dv.getUint8(15)&1, lw=dv.getUint16(16,true),
        lh=dv.getUint16(18,true), rle=dv.getUint32(20,true);
  const out=new Uint8Array(rb*ph); let o=0;
  for(let i=24;i<24+rle&&o<out.length;i+=2){const n=buf[i]||256,v=buf[i+1];out.fill(v,o,Math.min(o+n,out.length));o+=n;}
  if(c.width!==lw||c.height!==lh){c.width=lw;c.height=lh;}
  const img=ctx.createImageData(lw,lh), d=img.data;
  for(let y=0;y<lh;y++){
    for(let x=0;x<lw;x++){
      let px,py;
      switch(ori){
        case 0: px=y; py=ph-1-x; break;
        case 1: px=pw-1-x; py=ph-1-y; break;
        case 2: px=pw-1-y; py=x; break;
        default: px=x; py=y; break;
      }
      const bit=(out[py*rb+(px>>3)]>>(7-(px&7)))&1;
      const i4=(y*lw+x)*4, v=(bit^inv)?255:0;
      d[i4]=v; d[i4+1]=v; d[i4+2]=v; d[i4+3]=255;
    }
  }
  ctx.putImageData(img,0,0);
  set('frame #'+seq+'  '+lw+'x'+lh);
}
function connect(){
  if(ws){try{ws.close();}catch(e){}} 
  ws=new WebSocket('ws://'+location.hostname+':81/');
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{set('auth…'); ws.send('AUTH '+pin);};
  ws.onmessage=(e)=>{
    if(typeof e.data==='string'){
      if(e.data==='OK') set('connected');
      else if(e.data==='BUSY') set('too many viewers');
      else if(e.data==='ERR'){ sessionStorage.removeItem('mirrorPin'); pin=''; ask(); }
      return;
    }
    draw(new Uint8Array(e.data));
  };
  ws.onclose=()=>{set('disconnected, retrying…'); setTimeout(connect,3000);};
  ws.onerror=()=>{};
}
function ask(){
  pin=prompt('device PIN')||'';
  if(pin) sessionStorage.setItem('mirrorPin',pin);
}
document.getElementById('again').onclick=()=>{ask();connect();};
if(!pin) ask();
connect();
</script></body></html>
)HTML";

const char* stateName(const ScreenMirrorService::State state) {
  switch (state) {
    case ScreenMirrorService::State::Off:
      return "off";
    case ScreenMirrorService::State::WifiConnecting:
      return "connecting";
    case ScreenMirrorService::State::Running:
      return "running";
    case ScreenMirrorService::State::Error:
      return "error";
  }
  return "off";
}

}  // namespace

ScreenMirrorService& ScreenMirrorService::instance() {
  static ScreenMirrorService service;
  return service;
}

bool ScreenMirrorService::ensureBuffers(GfxRenderer& renderer) {
  const size_t bufferBytes = renderer.getBufferSize();
  rowBytes_ = renderer.getPanelWidth() / 8;
  panelW_ = renderer.getPanelWidth();
  panelH_ = renderer.getPanelHeight();
  if (bufferBytes == 0 || rowBytes_ * panelH_ != bufferBytes) {
    std::snprintf(error_, sizeof(error_), "framebuffer layout %ux%u", panelW_, panelH_);
    return false;
  }
  if (frame_ && frameBytes_ == bufferBytes) return true;

  frame_ = memory::makePsramByteBufferNoThrow(bufferBytes);
  packet_ = memory::makePsramByteBufferNoThrow(bufferBytes * 2 + kHeaderBytes);
  if (!frame_ || !packet_) {
    frame_.reset();
    packet_.reset();
    std::snprintf(error_, sizeof(error_), "no PSRAM for %u-byte frame", static_cast<unsigned>(bufferBytes));
    return false;
  }
  frameBytes_ = bufferBytes;
  return true;
}

bool ScreenMirrorService::start(GfxRenderer& renderer) {
  if (state_ == State::Running || state_ == State::WifiConnecting) return true;
  error_[0] = '\0';
  if (!ensureBuffers(renderer)) {
    state_ = State::Error;
    stateSince_ = millis();
    LOG_ERR("MIRROR", "start failed: %s", error_);
    return false;
  }

  std::snprintf(pin_, sizeof(pin_), "%06u", static_cast<unsigned>(esp_random() % 1000000u));
  clients_ = 0;
  std::memset(authed_, 0, sizeof(authed_));
  observedSeq_ = 0;
  sentSeq_ = 0;
  framesSent_ = 0;

  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const auto credential = lastSsid.empty() ? std::nullopt : WIFI_STORE.findCredential(lastSsid);
  if (!credential) {
    std::snprintf(error_, sizeof(error_), "no saved WiFi");
    state_ = State::Error;
    stateSince_ = millis();
    LOG_ERR("MIRROR", "no saved WiFi credentials");
    return false;
  }

  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    std::snprintf(error_, sizeof(error_), "WiFi start failed");
    state_ = State::Error;
    stateSince_ = millis();
    return false;
  }

  state_ = State::WifiConnecting;
  stateSince_ = millis();
  if (WiFi.status() == WL_CONNECTED) {
    startServers();
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    LOG_INF("MIRROR", "connecting WiFi %s", credential->ssid.c_str());
  }
  return true;
}

void ScreenMirrorService::startServers() {
  if (mdnsStarted_) {
    MDNS.end();
    mdnsStarted_ = false;
  }
  if (MDNS.begin(kHostName)) {
    MDNS.addService("http", "tcp", kHttpPort);
    mdnsStarted_ = true;
    LOG_INF("MIRROR", "mDNS %s.local", kHostName);
  } else {
    LOG_ERR("MIRROR", "mDNS start failed");
  }

  http_ = makeUniqueNoThrow<WebServer>(kHttpPort);
  if (!http_) {
    std::snprintf(error_, sizeof(error_), "OOM http");
    state_ = State::Error;
    return;
  }
  http_->on("/", HTTP_GET, [this]() { http_->send(200, "text/html; charset=utf-8", kPageHtml); });
  http_->on("/api/mirror/status", HTTP_GET, [this]() { sendStatusJson(); });
  http_->onNotFound([this]() { http_->send(404, "text/plain", "not found"); });
  http_->begin();

  ws_ = makeUniqueNoThrow<WebSocketsServer>(kWsPort);
  if (!ws_) {
    std::snprintf(error_, sizeof(error_), "OOM ws");
    state_ = State::Error;
    return;
  }
  ws_->onEvent(&ScreenMirrorService::wsThunk);
  ws_->begin();

  lastWifiMs_ = millis();
  state_ = State::Running;
  stateSince_ = millis();
  LOG_INF("MIRROR", "serving http://%s.local (pin %s)", kHostName, pin_);
}

void ScreenMirrorService::sendStatusJson() {
  char body[192];
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  // Dev builds only: lets the serial automation pair a viewer without watching
  // the panel. Stable/nightly never expose the PIN over the network.
  std::snprintf(body, sizeof(body),
                "{\"state\":\"%s\",\"clients\":%u,\"frames\":%u,\"ip\":\"%s\",\"orientation\":0,\"pin\":\"%s\"}",
                stateName(state_), static_cast<unsigned>(clients_), static_cast<unsigned>(framesSent_),
                WiFi.localIP().toString().c_str(), pin_);
#else
  std::snprintf(body, sizeof(body), "{\"state\":\"%s\",\"clients\":%u,\"frames\":%u,\"ip\":\"%s\",\"orientation\":0}",
                stateName(state_), static_cast<unsigned>(clients_), static_cast<unsigned>(framesSent_),
                WiFi.localIP().toString().c_str());
#endif
  http_->send(200, "application/json", body);
}

void ScreenMirrorService::wsThunk(const uint8_t num, const WStype_t type, uint8_t* payload, const size_t length) {
  instance().handleWsEvent(num, type, payload, length);
}

void ScreenMirrorService::handleWsEvent(const uint8_t num, const WStype_t type, uint8_t* payload, const size_t length) {
  if (num >= WEBSOCKETS_SERVER_CLIENT_MAX) return;
  switch (type) {
    case WStype_CONNECTED:
      authed_[num] = false;
      ws_->sendTXT(num, "PIN");
      break;
    case WStype_DISCONNECTED:
      if (authed_[num]) {
        authed_[num] = false;
        if (clients_ > 0) --clients_;
      }
      break;
    case WStype_TEXT: {
      if (authed_[num]) break;
      char text[16] = {};
      const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
      std::memcpy(text, payload, n);
      if (std::strncmp(text, "AUTH ", 5) != 0 || std::strcmp(text + 5, pin_) != 0) {
        ws_->sendTXT(num, "ERR");
        ws_->disconnect(num);
        break;
      }
      if (clients_ >= kMaxViewers) {
        ws_->sendTXT(num, "BUSY");
        ws_->disconnect(num);
        break;
      }
      authed_[num] = true;
      ++clients_;
      observedSeq_ = 0;
      sentSeq_ = 0;
      ws_->sendTXT(num, "OK");
      LOG_INF("MIRROR", "viewer %u authenticated", static_cast<unsigned>(num));
      break;
    }
    default:
      break;
  }
}

bool ScreenMirrorService::captureFrame(GfxRenderer& renderer) {
  uint32_t seq = 0;
  uint16_t logicalW = 0, logicalH = 0;
  uint8_t orientation = 0;
  bool inverted = false;
  {
    // Copy under the render lock so the snapshot cannot interleave with a draw,
    // then release it before the socket writes.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return false;
    const uint8_t* fb = renderer.getFrameBuffer();
    if (!fb || renderer.getBufferSize() != frameBytes_) return false;
    std::memcpy(frame_.get(), fb, frameBytes_);
    seq = renderer.frameSequence();
    logicalW = static_cast<uint16_t>(renderer.getScreenWidth());
    logicalH = static_cast<uint16_t>(renderer.getScreenHeight());
    orientation = static_cast<uint8_t>(renderer.getOrientation());
    inverted = renderer.isInverted();
  }

  uint8_t* out = packet_.get();
  const uint8_t* src = frame_.get();
  size_t rleLen = 0;
  size_t i = 0;
  while (i < frameBytes_) {
    const uint8_t value = src[i];
    size_t run = 1;
    while (i + run < frameBytes_ && src[i + run] == value && run < 255) ++run;
    out[kHeaderBytes + rleLen++] = static_cast<uint8_t>(run);
    out[kHeaderBytes + rleLen++] = value;
    i += run;
  }

  out[0] = 'A';
  out[1] = 'L';
  out[2] = 'M';
  out[3] = 1;
  auto put16 = [&out](const size_t at, const uint16_t v) {
    out[at] = static_cast<uint8_t>(v & 0xFF);
    out[at + 1] = static_cast<uint8_t>(v >> 8);
  };
  auto put32 = [&out](const size_t at, const uint32_t v) {
    out[at] = static_cast<uint8_t>(v & 0xFF);
    out[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(4, seq);
  put16(8, panelW_);
  put16(10, panelH_);
  put16(12, static_cast<uint16_t>(rowBytes_));
  out[14] = orientation;
  out[15] = inverted ? 1 : 0;
  put16(16, logicalW);
  put16(18, logicalH);
  put32(20, static_cast<uint32_t>(rleLen));

  const size_t packetLen = kHeaderBytes + rleLen;
  for (uint8_t num = 0; num < WEBSOCKETS_SERVER_CLIENT_MAX; ++num) {
    if (authed_[num]) ws_->sendBIN(num, packet_.get(), packetLen);
  }
  sentSeq_ = seq;
  ++framesSent_;
  LOG_DBG("MIRROR", "frame %u %ux%u rle=%u", static_cast<unsigned>(seq), logicalW, logicalH,
          static_cast<unsigned>(rleLen));
  return true;
}

void ScreenMirrorService::pumpFrame(GfxRenderer& renderer) {
  if (!ws_ || clients_ == 0) return;
  const uint32_t seq = renderer.frameSequence();
  if (seq != observedSeq_) {
    observedSeq_ = seq;
    observedAt_ = millis();
  }
  if (seq == sentSeq_) return;
  const uint32_t now = millis();
  // Wait for a short quiet window: one user action can drive several panel
  // commits (e.g. the grayscale passes), and only the final frame matters.
  if (observedAt_ != 0 && now - observedAt_ < kFrameDebounceMs) return;
  captureFrame(renderer);
}

void ScreenMirrorService::tick(GfxRenderer& renderer) {
  if (state_ == State::Off || state_ == State::Error) return;

  if (state_ == State::WifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      LOG_INF("MIRROR", "WiFi connected %s", WiFi.localIP().toString().c_str());
      startServers();
    } else if (millis() - stateSince_ > kWifiTimeoutMs) {
      std::snprintf(error_, sizeof(error_), "WiFi timeout");
      LOG_ERR("MIRROR", "WiFi connect timeout");
      state_ = State::Error;
      stopServers();
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiMs_ > kWifiLossGraceMs) {
      std::snprintf(error_, sizeof(error_), "WiFi lost");
      LOG_ERR("MIRROR", "WiFi lost");
      state_ = State::Error;
      stopServers();
    }
    return;
  }
  lastWifiMs_ = millis();

  if (http_) http_->handleClient();
  if (ws_) ws_->loop();
  pumpFrame(renderer);
}

void ScreenMirrorService::stopServers() {
  if (ws_) {
    ws_->close();
    ws_.reset();
  }
  if (http_) {
    http_->stop();
    http_.reset();
  }
  if (mdnsStarted_) {
    MDNS.end();
    mdnsStarted_ = false;
  }
  clients_ = 0;
  std::memset(authed_, 0, sizeof(authed_));
}

void ScreenMirrorService::stop() {
  if (state_ == State::Off) return;
  stopServers();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  frame_.reset();
  packet_.reset();
  frameBytes_ = 0;
  pin_[0] = '\0';
  error_[0] = '\0';
  state_ = State::Off;
  stateSince_ = millis();
  LOG_INF("MIRROR", "stopped");
}

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
