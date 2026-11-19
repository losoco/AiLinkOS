#include "ScreenMirrorService.h"

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <esp_random.h>

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"

namespace {

// 8080/8081 keep port 80 free for File Transfer and Calibre, which share a
// single CrossPointWebServer and their own mDNS host name.
constexpr uint16_t kWsPort = 8081;
constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kWifiLossGraceMs = 30000;
constexpr uint32_t kViewerTimeoutMs = 15000;
constexpr size_t kHeaderBytes = 32;
constexpr char kHostName[] = "ailinkos";
// USB stream framing: magic, u32 payload length, payload, u16 CRC. Log text and
// half-written frames are skipped by the host while it scans for the magic.
constexpr char kUsbMagic[4] = {'M', 'S', 'F', '1'};
constexpr uint32_t kUsbUnplugGraceMs = 2000;

uint16_t crc16(const uint8_t* data, const size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

RemoteButton remoteButtonFromName(const char* name) {
  if (std::strcmp(name, "BACK") == 0) return RemoteButton::Back;
  if (std::strcmp(name, "CONFIRM") == 0) return RemoteButton::Confirm;
  if (std::strcmp(name, "UP") == 0) return RemoteButton::Up;
  if (std::strcmp(name, "DOWN") == 0) return RemoteButton::Down;
  if (std::strcmp(name, "LEFT") == 0) return RemoteButton::Left;
  if (std::strcmp(name, "RIGHT") == 0) return RemoteButton::Right;
  if (std::strcmp(name, "NEXT") == 0) return RemoteButton::Next;
  if (std::strcmp(name, "PREV") == 0) return RemoteButton::Previous;
  if (std::strcmp(name, "HOME") == 0) return RemoteButton::Home;
  return RemoteButton::None;
}

const char kPageHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AiLinkOS screen</title>
<style>
body{margin:0;background:#111;color:#ddd;font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;display:flex;flex-direction:column;align-items:center;gap:10px;padding:12px}
canvas{max-width:100%;height:auto;background:#fff;border:1px solid #444;image-rendering:pixelated;cursor:crosshair}
#s{font-size:12px;color:#9a9}
button{background:#222;color:#ddd;border:1px solid #555;border-radius:6px;padding:6px 12px}
#pad{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;max-width:480px}
#pad button{min-width:58px;height:44px;font-size:16px}
#pad button.ok{border-color:#3a3;background:#163}
#pad button.no{border-color:#a33;background:#311}
#hint{font-size:12px;color:#777;text-align:center}
</style></head><body>
<canvas id="c" width="480" height="800"></canvas>
<div id="s">connecting…</div>
<div id="pad">
<button data-k="BACK">返回</button>
<button data-k="LEFT">◀</button>
<button data-k="UP">▲</button>
<button data-k="DOWN">▼</button>
<button data-k="RIGHT">▶</button>
<button data-k="CONFIRM">OK</button>
<button data-k="HOME">主屏</button>
</div>
<div id="hint">单击触摸 · 滚轮上下 · 双击返回；方向键 / Enter / Esc / H（主屏）也可遥控</div>
<div><button id="again">重新连接</button></div>
<script>
let ws=null, pin=sessionStorage.getItem('mirrorPin')||'', panel=null, retry=null;
const s=document.getElementById('s'), c=document.getElementById('c'), ctx=c.getContext('2d');
function set(t){s.textContent=t;}
function draw(buf){
  const dv=new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if(buf[0]!==65||buf[1]!==76||buf[2]!==77||buf[3]!==2) return;
  const seq=dv.getUint32(4,true), pw=dv.getUint16(8,true), ph=dv.getUint16(10,true),
        rb=dv.getUint16(12,true), ori=dv.getUint8(14), flags=dv.getUint8(15),
        inv=flags&1, full=(flags&2)!==0, lw=dv.getUint16(16,true), lh=dv.getUint16(18,true),
        rx=dv.getUint16(20,true), ry=dv.getUint16(22,true), rw=dv.getUint16(24,true),
        rh=dv.getUint16(26,true), rle=dv.getUint32(28,true);
  if(!panel||panel.length!==rb*ph||full) panel=new Uint8Array(rb*ph);
  const rowBytes=rw>>3, xByte=rx>>3;
  let p=32;
  for(let row=ry; row<ry+rh; row++){
    const base=row*rb+xByte;
    let col=0;
    while(col<rowBytes && p<32+rle){
      const n=buf[p]||256, v=buf[p+1]; p+=2;
      const end=Math.min(col+n,rowBytes);
      panel.fill(v, base+col, base+end);
      col=end;
    }
  }
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
      const bit=(panel[py*rb+(px>>3)]>>(7-(px&7)))&1;
      const i4=(y*lw+x)*4, v=(bit^inv)?255:0;
      d[i4]=v; d[i4+1]=v; d[i4+2]=v; d[i4+3]=255;
    }
  }
  ctx.putImageData(img,0,0);
  set('frame #'+seq+(full?'  full':'  patch '+rw+'x'+rh));
}
function connect(){
  if(ws){ws.onclose=null; ws.onmessage=null; try{ws.close();}catch(e){}}ws=null;
  ws=new WebSocket('ws://'+location.hostname+':8081/');
  ws.binaryType='arraybuffer';
  // The server decides: it either answers OK at once (PIN disabled in Settings)
  // or asks for the PIN with "PIN".
  ws.onopen=()=>{ if(retry){clearTimeout(retry); retry=null;} set('auth…'); if(pin) ws.send('AUTH '+pin);};
  ws.onmessage=(e)=>{
    if(typeof e.data==='string'){
      if(e.data==='OK') set('connected');
      else if(e.data==='PIN'){ if(!pin) ask(); if(pin) ws.send('AUTH '+pin); }
      else if(e.data==='ERR'){ sessionStorage.removeItem('mirrorPin'); pin=''; ask(); setTimeout(connect,200); }
      else if(e.data.startsWith('KEY')){ const p=e.data.split(' '); keyFlash(p[1], e.data.startsWith('KEYOK')); }
      return;
    }
    draw(new Uint8Array(e.data));
  };
  // One pending retry at a time: a close fired by an older socket must not
  // schedule a second reconnect that then fights the live one.
  ws.onclose=()=>{ set('disconnected, retrying…'); if(!retry) retry=setTimeout(()=>{retry=null;connect();},3000); };
  ws.onerror=()=>{};
}
function ask(){
  pin=prompt('device PIN')||'';
  if(pin) sessionStorage.setItem('mirrorPin',pin);
}
// Reconnect only; the server asks for the PIN itself when the setting is on.
document.getElementById('again').onclick=()=>{ connect(); };
function sendKey(k){ if(ws&&ws.readyState===1) ws.send('KEY '+k); }
function keyFlash(name,ok){
  const b=document.querySelector('#pad button[data-k="'+name+'"]'); if(!b) return;
  b.classList.remove('ok','no'); void b.offsetWidth; b.classList.add(ok?'ok':'no');
  setTimeout(()=>b.classList.remove('ok','no'),700);
}
document.querySelectorAll('#pad button').forEach(b=>b.onclick=()=>{keyFlash(b.dataset.k,true);sendKey(b.dataset.k);});
// Single click = touch tap, double click = Back. The tap is held briefly so a
// double click can cancel it instead of tapping the UI twice on the way out.
let tapTimer=null;
c.addEventListener('click',(e)=>{
  if(!ws||ws.readyState!==1) return;
  if(tapTimer){ clearTimeout(tapTimer); tapTimer=null; keyFlash('BACK',true); sendKey('BACK'); return; }
  const r=c.getBoundingClientRect();
  const x=Math.round((e.clientX-r.left)*c.width/r.width);
  const y=Math.round((e.clientY-r.top)*c.height/r.height);
  tapTimer=setTimeout(()=>{ tapTimer=null; if(ws&&ws.readyState===1) ws.send('TAP '+x+' '+y); },220);
});
// Wheel nudges map to Up/Down, throttled to one step per gesture frame.
let lastWheel=0;
c.addEventListener('wheel',(e)=>{
  e.preventDefault();
  const now=Date.now();
  if(now-lastWheel<140||Math.abs(e.deltaY)<1) return;
  lastWheel=now;
  sendKey(e.deltaY<0?'UP':'DOWN');
},{passive:false});
addEventListener('keydown',(e)=>{const m={ArrowLeft:'LEFT',ArrowRight:'RIGHT',ArrowUp:'UP',ArrowDown:'DOWN',Enter:'CONFIRM',Escape:'BACK',h:'HOME',H:'HOME'};
  if(m[e.key]){e.preventDefault();keyFlash(m[e.key],true);sendKey(m[e.key]);}});
setInterval(()=>{if(ws&&ws.readyState===1) ws.send('PING');},4000);
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

bool ScreenMirrorService::pinRequired() const { return SETTINGS.mirrorPinEnabled != 0; }

bool ScreenMirrorService::ensureBuffers(GfxRenderer& renderer) {
  const size_t bufferBytes = renderer.getBufferSize();
  rowBytes_ = renderer.getPanelWidth() / 8;
  panelW_ = renderer.getPanelWidth();
  panelH_ = renderer.getPanelHeight();
  if (bufferBytes == 0 || rowBytes_ * panelH_ != bufferBytes) {
    std::snprintf(error_, sizeof(error_), "framebuffer layout %ux%u", panelW_, panelH_);
    return false;
  }
  if (slots_[0].buf && slots_[1].buf && last_ && packet_ && frameBytes_ == bufferBytes) return true;

  slots_[0].buf = memory::makePsramByteBufferNoThrow(bufferBytes);
  slots_[1].buf = memory::makePsramByteBufferNoThrow(bufferBytes);
  last_ = memory::makePsramByteBufferNoThrow(bufferBytes);
  packet_ = memory::makePsramByteBufferNoThrow(bufferBytes * 2 + kHeaderBytes);
  if (!slots_[0].buf || !slots_[1].buf || !last_ || !packet_) {
    slots_[0].buf.reset();
    slots_[1].buf.reset();
    last_.reset();
    packet_.reset();
    std::snprintf(error_, sizeof(error_), "no PSRAM for %u-byte frame", static_cast<unsigned>(bufferBytes));
    return false;
  }
  frameBytes_ = bufferBytes;
  haveLast_ = false;
  newFrame_.store(false);
  forceSnapshot_.store(true);
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
  sentSeq_ = 0;
  framesSent_ = 0;
  // Snapshot on the render task from now on; stop() removes the hook.
  renderer_ = &renderer;
  renderer.setFrameCommitHook(&ScreenMirrorService::commitThunk, this);
  forceSnapshot_.store(true);

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

bool ScreenMirrorService::startUsb(GfxRenderer& renderer) {
  if (state_ == State::Running) return usbMode_;
  if (state_ == State::WifiConnecting) return false;
  error_[0] = '\0';
  if (!ensureBuffers(renderer)) {
    state_ = State::Error;
    stateSince_ = millis();
    LOG_ERR("MIRROR", "usb start failed: %s", error_);
    return false;
  }

  // USB mirror is a wired, physical-trust transport: the radio is turned off so
  // the device cannot stay associated (and burning power) while the cable is in,
  // and no PIN or mDNS name is involved. Skip the teardown when Wi-Fi was never
  // brought up: disconnect() logs an error on an uninitialised driver.
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  usbMode_ = true;
  usbLostSince_ = 0;
  // One sink: the host bridge. Keeping clients_ at 1 reuses the existing
  // capture-on-commit and pump path unchanged.
  clients_ = 1;
  std::memset(authed_, 0, sizeof(authed_));
  sentSeq_ = 0;
  framesSent_ = 0;
  renderer_ = &renderer;
  renderer.setFrameCommitHook(&ScreenMirrorService::commitThunk, this);
  forceSnapshot_.store(true);
  // Frames own the CDC port from here on; text logs would corrupt them.
  setLogTransportMuted(true);
  state_ = State::Running;
  stateSince_ = millis();
  LOG_INF("MIRROR", "usb mirror started");
  return true;
}

void ScreenMirrorService::emitLine(const char* format, ...) {
  char buf[96];
  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buf, sizeof(buf) - 1, format, args);
  va_end(args);
  if (written <= 0) return;
  size_t length = static_cast<size_t>(written);
  if (length > sizeof(buf) - 2) length = sizeof(buf) - 2;
  buf[length++] = '\n';
  logSerial.write(reinterpret_cast<const uint8_t*>(buf), length);
}

bool ScreenMirrorService::writeUsbFrame(const uint8_t* data, const size_t length) {
  uint8_t head[8];
  std::memcpy(head, kUsbMagic, sizeof(kUsbMagic));
  head[4] = static_cast<uint8_t>(length & 0xFF);
  head[5] = static_cast<uint8_t>((length >> 8) & 0xFF);
  head[6] = static_cast<uint8_t>((length >> 16) & 0xFF);
  head[7] = static_cast<uint8_t>((length >> 24) & 0xFF);
  const uint16_t crc = crc16(data, length);
  const uint8_t tail[2] = {static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>(crc >> 8)};

  // HWCDC::write is bounded (its TX timeout caps consecutive backpressure), but
  // it may return a short count. A zero-length write means the host stopped
  // draining; the caller drops the frame rather than blocking the main loop.
  const auto writeAll = [](const uint8_t* bytes, const size_t count) {
    size_t off = 0;
    while (off < count) {
      const size_t sent = logSerial.write(bytes + off, count - off);
      if (sent == 0) return false;
      off += sent;
    }
    return true;
  };

  if (!writeAll(head, sizeof(head))) return false;
  if (!writeAll(data, length)) return false;
  return writeAll(tail, sizeof(tail));
}

bool ScreenMirrorService::handleSerialLine(const char* line, GfxRenderer& renderer) {
  if (std::strncmp(line, "MIRROR", 6) != 0) return false;
  const char* arg = line + 6;
  while (*arg == ' ') ++arg;

  if (std::strcmp(arg, "ON") == 0) {
    if (state_ == State::Running) {
      if (!usbMode_) {
        emitLine("MIRROR BUSY wifi");
        return true;
      }
      // A bridge that reconnects within the unplug grace period finds the
      // service still running with no baseline, so force a full frame.
      clients_ = 1;
      haveLast_ = false;
      forceSnapshot_.store(true);
      emitLine("MIRROR READY %u %u", static_cast<unsigned>(panelW_), static_cast<unsigned>(panelH_));
      return true;
    }
    if (startUsb(renderer)) {
      emitLine("MIRROR READY %u %u", static_cast<unsigned>(panelW_), static_cast<unsigned>(panelH_));
    } else {
      emitLine("MIRROR ERR %s", error_);
    }
    return true;
  }

  if (std::strcmp(arg, "OFF") == 0) {
    if (!usbMode_) {
      emitLine("MIRROR ERR not-usb");
      return true;
    }
    emitLine("MIRROR OFF");
    stop("usb-host");
    return true;
  }

  if (std::strcmp(arg, "PAGE") == 0) {
    // The bridge has no copy of the viewer page: hand it the exact bytes this
    // firmware serves so the two can never drift apart. Silence log text for the
    // transfer, then restore whatever the current mode needs.
    const size_t length = std::strlen(kPageHtml);
    emitLine("MIRROR PAGE %u", static_cast<unsigned>(length));
    setLogTransportMuted(true);
    logSerial.write(reinterpret_cast<const uint8_t*>(kPageHtml), length);
    const uint8_t newline = '\n';
    logSerial.write(&newline, 1);
    setLogTransportMuted(usbMode_);
    return true;
  }

  if (std::strcmp(arg, "PING") == 0) {
    emitLine("MIRROR PONG");
    return true;
  }

  if (std::strcmp(arg, "STATUS") == 0) {
    emitLine(
        "MIRROR STATUS usb=%d state=%u clients=%u frameBytes=%u commits=%u frames=%u drops=%u snapshots=%u "
        "force=%d new=%d last=%d",
        usbMode_ ? 1 : 0, static_cast<unsigned>(state_), static_cast<unsigned>(clients_),
        static_cast<unsigned>(frameBytes_), static_cast<unsigned>(commits_), static_cast<unsigned>(framesSent_),
        static_cast<unsigned>(usbDrops_), static_cast<unsigned>(snapshots_), forceSnapshot_.load() ? 1 : 0,
        newFrame_.load() ? 1 : 0, haveLast_ ? 1 : 0);
    return true;
  }

  if (std::strcmp(arg, "RESYNC") == 0) {
    // The SDK's display driver prints straight to the CDC port, so log text can
    // land between a frame's header and payload. The bridge detects the bad CRC
    // and asks for a clean, full-page frame.
    if (!usbMode_) {
      emitLine("MIRROR ERR not-usb");
      return true;
    }
    haveLast_ = false;
    forceSnapshot_.store(true);
    emitLine("MIRROR RESYNCING");
    return true;
  }

  if (std::strncmp(arg, "KEY ", 4) == 0) {
    const char* name = arg + 4;
    const RemoteButton button = remoteButtonFromName(name);
    const bool accepted = button != RemoteButton::None && input_ != nullptr && input_->injectRemoteClick(button);
    LOG_INF("MIRROR", "usb key %s accepted=%d", name, accepted ? 1 : 0);
    emitLine("MIRROR KEY%s %s", accepted ? "OK" : "NO", name);
    return true;
  }

  if (std::strncmp(arg, "TAP ", 4) == 0) {
    int x = 0;
    int y = 0;
    const bool parsed = std::sscanf(arg, "TAP %d %d", &x, &y) == 2;
    const bool accepted = parsed && input_ != nullptr && input_->injectRemoteTap(x, y);
    LOG_INF("MIRROR", "usb tap %d,%d accepted=%d", x, y, accepted ? 1 : 0);
    emitLine("MIRROR TOUCH%s", accepted ? "OK" : "NO");
    return true;
  }

  emitLine("MIRROR ERR unknown");
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
    LOG_INF("MIRROR", "mDNS %s.local:%u", kHostName, kHttpPort);
  } else {
    LOG_ERR("MIRROR", "mDNS start failed");
  }

  http_ = makeUniqueNoThrow<WebServer>(kHttpPort);
  if (!http_) {
    std::snprintf(error_, sizeof(error_), "OOM http");
    state_ = State::Error;
    return;
  }
  http_->on("/", HTTP_GET, [this]() {
    // Never let a browser keep an older viewer script: the takeover handshake
    // and the remote-key handling change with the firmware.
    http_->sendHeader("Cache-Control", "no-store");
    http_->send(200, "text/html; charset=utf-8", kPageHtml);
  });
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
  LOG_INF("MIRROR", "serving http://%s.local:%u (pin %s)", kHostName, kHttpPort, pin_);
}

void ScreenMirrorService::restoreMdns() {
  if (state_ != State::Running || WiFi.status() != WL_CONNECTED) return;
  MDNS.end();
  mdnsStarted_ = false;
  if (MDNS.begin(kHostName)) {
    MDNS.addService("http", "tcp", kHttpPort);
    mdnsStarted_ = true;
    LOG_INF("MIRROR", "mDNS restored %s.local:%u", kHostName, kHttpPort);
  } else {
    LOG_ERR("MIRROR", "mDNS restore failed");
  }
}

void ScreenMirrorService::suspendMdns() {
  if (!mdnsStarted_) return;
  MDNS.end();
  mdnsStarted_ = false;
  LOG_INF("MIRROR", "mDNS suspended before WiFi restart");
}

void ScreenMirrorService::sendStatusJson() {
  char body[192];
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  // Dev builds only: lets the serial automation pair a viewer without watching
  // the panel. Stable/nightly never expose the PIN over the network.
  std::snprintf(body, sizeof(body),
                "{\"state\":\"%s\",\"clients\":%u,\"frames\":%u,\"ip\":\"%s\",\"pinRequired\":%d,"
                "\"orientation\":0,\"pin\":\"%s\"}",
                stateName(state_), static_cast<unsigned>(clients_), static_cast<unsigned>(framesSent_),
                WiFi.localIP().toString().c_str(), pinRequired() ? 1 : 0, pin_);
#else
  std::snprintf(body, sizeof(body),
                "{\"state\":\"%s\",\"clients\":%u,\"frames\":%u,\"ip\":\"%s\",\"pinRequired\":%d,\"orientation\":0}",
                stateName(state_), static_cast<unsigned>(clients_), static_cast<unsigned>(framesSent_),
                WiFi.localIP().toString().c_str(), pinRequired() ? 1 : 0);
#endif
  http_->send(200, "application/json", body);
}

void ScreenMirrorService::wsThunk(const uint8_t num, const WStype_t type, uint8_t* payload, const size_t length) {
  instance().handleWsEvent(num, type, payload, length);
}

bool ScreenMirrorService::handleTextCommand(const uint8_t num, const char* text) {
  if (input_ == nullptr) return false;
  if (std::strncmp(text, "TAP ", 4) == 0) {
    // Logical screen coordinates from the viewer canvas; the input manager
    // exposes it as a one-frame touch tap, so touch-only screens respond even
    // though this board has no touch panel.
    int x = 0;
    int y = 0;
    if (std::sscanf(text, "TAP %d %d", &x, &y) != 2) return false;
    const bool tapAccepted = input_->injectRemoteTap(x, y);
    LOG_INF("MIRROR", "remote tap %d,%d accepted=%d", x, y, tapAccepted ? 1 : 0);
    if (ws_) ws_->sendTXT(num, tapAccepted ? "TOUCHOK" : "TOUCHNO");
    return tapAccepted;
  }
  if (std::strncmp(text, "KEY ", 4) != 0) return false;
  const char* name = text + 4;
  const RemoteButton button = remoteButtonFromName(name);
  if (button == RemoteButton::None) return false;
  const bool accepted = input_->injectRemoteClick(button);
  LOG_INF("MIRROR", "remote key %s accepted=%d", name, accepted ? 1 : 0);
  // Acknowledge at once so the page can paint the button state even though the
  // panel itself needs a full e-ink refresh before the mirrored frame changes.
  char ack[24];
  std::snprintf(ack, sizeof(ack), "KEY%s %s", accepted ? "OK" : "NO", name);
  if (ws_) ws_->sendTXT(num, ack);
  return accepted;
}

void ScreenMirrorService::handleWsEvent(const uint8_t num, const WStype_t type, uint8_t* payload, const size_t length) {
  if (num >= WEBSOCKETS_SERVER_CLIENT_MAX) return;
  switch (type) {
    case WStype_CONNECTED:
      authed_[num] = false;
      if (pinRequired()) {
        ws_->sendTXT(num, "PIN");
      } else {
        // PIN checks are off by default: the viewer joins as soon as it connects.
        acceptViewer(num);
      }
      break;
    case WStype_DISCONNECTED:
      if (authed_[num]) {
        authed_[num] = false;
        if (clients_ > 0) --clients_;
      }
      break;
    case WStype_TEXT: {
      char text[24] = {};
      const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
      std::memcpy(text, payload, n);
      if (authed_[num]) {
        lastSeenMs_[num] = millis();
        if (std::strcmp(text, "PING") != 0) handleTextCommand(num, text);
        break;
      }
      const bool pinOk = !pinRequired() || (std::strncmp(text, "AUTH ", 5) == 0 && std::strcmp(text + 5, pin_) == 0);
      if (!pinOk) {
        ws_->sendTXT(num, "ERR");
        ws_->disconnect(num);
        break;
      }
      acceptViewer(num);
      break;
    }
    default:
      break;
  }
}

void ScreenMirrorService::acceptViewer(const uint8_t num) {
  // Every authenticated viewer receives the same frames. Kicking an older tab
  // made two open pages fight over the slot: each takeover closed the other
  // socket, whose reconnect (3 s later) closed the first, so the status text
  // never settled. A viewer that stops reading is instead evicted by the
  // heartbeat timeout or by a failed send in sendSlot().
  authed_[num] = true;
  lastSeenMs_[num] = millis();
  clients_ = 0;
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (authed_[i]) ++clients_;
  }
  sentSeq_ = 0;
  haveLast_ = false;  // the new viewer needs a full frame
  forceSnapshot_.store(true);
  ws_->sendTXT(num, "OK");
  LOG_INF("MIRROR", "viewer %u authenticated (%u live)", static_cast<unsigned>(num), static_cast<unsigned>(clients_));
}

void ScreenMirrorService::commitThunk(void* ctx) { static_cast<ScreenMirrorService*>(ctx)->onFrameCommitted(); }

void ScreenMirrorService::onFrameCommitted() {
  if (usbMode_) ++commits_;
  if (state_ != State::Running || clients_ == 0 || renderer_ == nullptr) return;
  const uint8_t* fb = renderer_->getFrameBuffer();
  if (!fb || renderer_->getBufferSize() != frameBytes_) return;
  const uint8_t slot = writeSlot_ ^ 1;
  if (!slots_[slot].buf) return;
  std::memcpy(slots_[slot].buf.get(), fb, frameBytes_);
  slots_[slot].seq = renderer_->frameSequence();
  slots_[slot].logicalW = static_cast<uint16_t>(renderer_->getScreenWidth());
  slots_[slot].logicalH = static_cast<uint16_t>(renderer_->getScreenHeight());
  slots_[slot].orientation = static_cast<uint8_t>(renderer_->getOrientation());
  slots_[slot].inverted = renderer_->isInverted();
  writeSlot_ = slot;
  readySlot_.store(slot, std::memory_order_release);
  newFrame_.store(true, std::memory_order_release);
}

bool ScreenMirrorService::captureToSlot() {
  if (renderer_ == nullptr) return false;
  // One-shot snapshot for a freshly authenticated viewer on a static screen.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  const uint8_t* fb = renderer_->getFrameBuffer();
  if (!fb || renderer_->getBufferSize() != frameBytes_) return false;
  const uint8_t slot = writeSlot_ ^ 1;
  std::memcpy(slots_[slot].buf.get(), fb, frameBytes_);
  slots_[slot].seq = renderer_->frameSequence();
  slots_[slot].logicalW = static_cast<uint16_t>(renderer_->getScreenWidth());
  slots_[slot].logicalH = static_cast<uint16_t>(renderer_->getScreenHeight());
  slots_[slot].orientation = static_cast<uint8_t>(renderer_->getOrientation());
  slots_[slot].inverted = renderer_->isInverted();
  writeSlot_ = slot;
  readySlot_.store(slot, std::memory_order_release);
  newFrame_.store(true, std::memory_order_release);
  ++snapshots_;
  return true;
}

void ScreenMirrorService::sendSlot(const uint8_t slotIndex) {
  const FrameSlot& slot = slots_[slotIndex];
  if (!slot.buf) return;
  const uint8_t* cur = slot.buf.get();
  const uint8_t* prev = last_.get();
  const size_t widthBytes = rowBytes_;

  // Only send what changed. A progress-bar tick touches a few rows, while a
  // full page change falls back to the whole panel. Bounds are byte-aligned so
  // the rectangle can be copied straight out of the packed buffer.
  uint16_t x = 0, y = 0, w = panelW_, h = panelH_;
  bool full = !haveLast_;
  if (!full) {
    size_t minByte = widthBytes, maxByte = 0;
    uint16_t y0 = panelH_, y1 = 0;
    for (uint16_t row = 0; row < panelH_; ++row) {
      const uint8_t* a = cur + static_cast<size_t>(row) * widthBytes;
      const uint8_t* b = prev + static_cast<size_t>(row) * widthBytes;
      if (std::memcmp(a, b, widthBytes) == 0) continue;
      size_t first = 0;
      while (first < widthBytes && a[first] == b[first]) ++first;
      size_t lastByte = widthBytes - 1;
      while (lastByte > first && a[lastByte] == b[lastByte]) --lastByte;
      if (first < minByte) minByte = first;
      if (lastByte > maxByte) maxByte = lastByte;
      if (row < y0) y0 = row;
      if (row > y1) y1 = row;
    }
    if (y0 > y1) {
      sentSeq_ = slot.seq;
      return;
    }
    x = static_cast<uint16_t>(minByte * 8);
    w = static_cast<uint16_t>((maxByte - minByte + 1) * 8);
    y = y0;
    h = static_cast<uint16_t>(y1 - y0 + 1);
  }

  uint8_t* out = packet_.get();
  size_t rleLen = 0;
  const size_t regionRowBytes = w / 8;
  for (uint16_t row = y; row < static_cast<uint16_t>(y + h); ++row) {
    const uint8_t* rowPtr = cur + static_cast<size_t>(row) * widthBytes + (x / 8);
    size_t i = 0;
    while (i < regionRowBytes) {
      const uint8_t value = rowPtr[i];
      size_t run = 1;
      while (i + run < regionRowBytes && rowPtr[i + run] == value && run < 255) ++run;
      out[kHeaderBytes + rleLen++] = static_cast<uint8_t>(run);
      out[kHeaderBytes + rleLen++] = value;
      i += run;
    }
  }

  out[0] = 'A';
  out[1] = 'L';
  out[2] = 'M';
  out[3] = 2;
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
  put32(4, slot.seq);
  put16(8, panelW_);
  put16(10, panelH_);
  put16(12, static_cast<uint16_t>(widthBytes));
  out[14] = slot.orientation;
  out[15] = static_cast<uint8_t>((slot.inverted ? 1 : 0) | (full ? 2 : 0));
  put16(16, slot.logicalW);
  put16(18, slot.logicalH);
  put16(20, x);
  put16(22, y);
  put16(24, w);
  put16(26, h);
  put32(28, static_cast<uint32_t>(rleLen));

  const size_t packetLen = kHeaderBytes + rleLen;
  if (usbMode_) {
    if (!writeUsbFrame(packet_.get(), packetLen)) {
      // The host stopped draining. Drop this frame and force a full snapshot
      // next time so a half-written packet cannot leave the page desynced.
      haveLast_ = false;
      forceSnapshot_.store(true);
      ++usbDrops_;
      LOG_INF("MIRROR", "usb frame dropped");
      return;
    }
  } else {
    for (uint8_t num = 0; num < WEBSOCKETS_SERVER_CLIENT_MAX; ++num) {
      if (!authed_[num]) continue;
      if (ws_->sendBIN(num, packet_.get(), packetLen)) continue;
      // A socket that cannot take one frame is gone (or stalled past the TCP
      // timeout). Drop it so it cannot hold the send path hostage.
      authed_[num] = false;
      lastSeenMs_[num] = 0;
      ws_->disconnect(num);
      if (clients_ > 0) --clients_;
      LOG_INF("MIRROR", "viewer %u dropped (send failed)", static_cast<unsigned>(num));
    }
  }
  std::memcpy(last_.get(), cur, frameBytes_);
  haveLast_ = true;
  sentSeq_ = slot.seq;
  ++framesSent_;
  LOG_DBG("MIRROR", "frame %u rect=%u,%u %ux%u full=%d rle=%u", static_cast<unsigned>(slot.seq), x, y, w, h,
          full ? 1 : 0, static_cast<unsigned>(rleLen));
}

void ScreenMirrorService::pumpFrame() {
  if (clients_ == 0) return;
  if (forceSnapshot_.exchange(false)) {
    if (!captureToSlot()) {
      forceSnapshot_.store(true);
      return;
    }
  }
  if (!newFrame_.exchange(false)) return;
  sendSlot(readySlot_.load(std::memory_order_acquire));
}

void ScreenMirrorService::tick(GfxRenderer& renderer, MappedInputManager& input) {
  input_ = &input;
  if (state_ == State::Off || state_ == State::Error) return;

  if (usbMode_) {
    // The bridge is the only sink. Stop when the cable or the host app goes
    // away instead of streaming into a closed port and draining the battery.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    if (!HWCDC::isConnected()) {
      if (usbLostSince_ == 0) {
        usbLostSince_ = millis();
      } else if (millis() - usbLostSince_ > kUsbUnplugGraceMs) {
        stop("usb-unplugged");
        return;
      }
    } else {
      usbLostSince_ = 0;
    }
#endif
    pumpFrame();
    return;
  }

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
  // Drop viewers that stopped heartbeating so a dead tab cannot hold the send
  // path (and the panel's refresh) hostage.
  const uint32_t now = millis();
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (!authed_[i] || now - lastSeenMs_[i] <= kViewerTimeoutMs) continue;
    authed_[i] = false;
    lastSeenMs_[i] = 0;
    ws_->disconnect(i);
    if (clients_ > 0) --clients_;
    LOG_INF("MIRROR", "viewer %u timed out", static_cast<unsigned>(i));
  }
  pumpFrame();
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

void ScreenMirrorService::stop(const char* reason) {
  if (state_ == State::Off) return;
  const bool wasUsb = usbMode_;
  stopServers();
  if (renderer_) {
    renderer_->setFrameCommitHook(nullptr, nullptr);
    renderer_ = nullptr;
  }
  if (wasUsb) {
    // Say goodbye while the port is still the stream's: the bridge shows this
    // instead of silently freezing on the last frame.
    emitLine("MIRROR STOPPED %s", reason);
    setLogTransportMuted(false);
    usbMode_ = false;
    usbLostSince_ = 0;
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  slots_[0].buf.reset();
  slots_[1].buf.reset();
  last_.reset();
  packet_.reset();
  frameBytes_ = 0;
  haveLast_ = false;
  newFrame_.store(false);
  forceSnapshot_.store(false);
  pin_[0] = '\0';
  error_[0] = '\0';
  state_ = State::Off;
  stateSince_ = millis();
  LOG_INF("MIRROR", "stopped (%s)", reason);
}

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
