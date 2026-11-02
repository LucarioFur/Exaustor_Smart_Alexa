/*
  Extractor con ESP32 y Alexa
  Autor: Lorhan Fernandes Ribeiro
  Copyright (c) 2026. Todos los derechos reservados. Consulte LICENSE.md.

  Bibliotecas necesarias en Arduino IDE:
  - WiFiManager
  - Espalexa
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#ifndef CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER
#define CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER 78
#endif

#include <WiFiManager.h>
#include <Preferences.h>
#include <Espalexa.h>

/* =========================
 * HOSTNAME / ALEXA
 * ========================= */
#define HOSTNAME "exaustor-smart"
#define ALEXA_DEVICE_NAME "Exaustor"

/* =========================
 * PINES
 * ========================= */
#define FAN1_PWM_PIN        25
#define FAN2_PWM_PIN        26
#define FAN3_PWM_PIN        27

#define FAN1_TACH_PIN       32
#define FAN2_TACH_PIN       33
#define FAN3_TACH_PIN       4

#define FAN_POWER_EN_PIN    23
#define WIFI_RESET_PIN      13
#define WIFI_STATUS_LED_PIN 12

/* =========================
 * PWM
 * ========================= */
#define FAN_PWM_FREQ        20000
#define FAN_PWM_RES_BITS    8

/* =========================
 * TACOMETROS / CONTROL DE RPM
 * ========================= */
#define RPM_CTRL_PERIOD_MS      250
#define TACH_DEBOUNCE_US        200

#define FAN1_PPR                2
#define FAN2_PPR                2
#define FAN3_PPR                2

#define FAN_PWM_MIN_GLOBAL      70
#define FAN_PWM_START_GLOBAL    120
#define FAN_PWM_MAX_ALL         255

#define FAN_CTRL_DEADBAND_RPM   80
#define FAN_CTRL_STEP_UP        2
#define FAN_CTRL_STEP_DOWN      2

#define RPM_GLOBAL_MIN          1500
#define RPM_GLOBAL_MAX          5500
#define FAN_START_BOOST_MS      700

/* =========================
 * PARADA GRADUAL
 * ========================= */
#define FAN_SOFTSTOP_STEP_MS    250
#define FAN_SOFTSTOP_STEP_PCT   1

/* =========================
 * RESTABLECER WIFI
 * ========================= */
#define WIFI_RESET_HOLD_MS  3000

/* =========================
 * PERSISTENCIA
 * ========================= */
Preferences prefs;

/* =========================
 * SERVIDOR WEB / ALEXA
 * ========================= */
WebServer server(80);
Espalexa espalexa;
EspalexaDevice *exaustorAlexa = nullptr;

/* =========================
 * CONFIGURACION WIFI
 * ========================= */
WiFiManager wm;
bool wifiPortalAtivo = false;
bool mdnsIniciado = false;
bool servidorIniciado = false;
bool alexaIniciada = false;

/* =========================
 * ESTADO
 * ========================= */
bool fansLigados = false;
uint8_t manualSpeedPercent = 70;
uint8_t fanSpeedPercent = 0;

volatile uint32_t tachCount1 = 0;
volatile uint32_t tachCount2 = 0;
volatile uint32_t tachCount3 = 0;

volatile uint32_t lastTachUs1 = 0;
volatile uint32_t lastTachUs2 = 0;
volatile uint32_t lastTachUs3 = 0;

uint32_t rpmFan1 = 0;
uint32_t rpmFan2 = 0;
uint32_t rpmFan3 = 0;

uint32_t rpmTargetGlobal = 0;
uint32_t lastCtrlMs = 0;

/* Parada gradual. */
bool softStopActive = false;
uint32_t lastSoftStopMs = 0;

/* Restablecer la red con el pulsador. */
bool wifiResetPressed = false;
bool wifiResetDone = false;
uint32_t wifiResetStartMs = 0;

/* LED de estado de la conexion Wi-Fi. */
bool wifiLedState = false;
uint32_t wifiLedLastMs = 0;
uint8_t wifiLedToggleCount = 0;

/* =========================
 * ESTRUCTURA DE CONTROL
 * ========================= */
typedef struct
{
  volatile uint32_t *tachCount;
  uint32_t rpm;
  uint32_t rpmTarget;
  uint8_t  ppr;

  uint8_t  pwmPin;
  uint16_t pwmCmd;
  uint16_t pwmMin;
  uint16_t pwmStart;
  uint16_t pwmMax;

  bool     running;
  bool     startBoostActive;
  uint32_t startBoostUntilMs;
} fan_ctrl_t;

fan_ctrl_t fan1;
fan_ctrl_t fan2;
fan_ctrl_t fan3;

/* =========================
 * PROTOTIPOS
 * ========================= */
void AtualizarAlexaEstado();

/* =========================
 * INTERRUPCIONES DE TACOMETRO
 * ========================= */
void IRAM_ATTR isrFan1Tach()
{
  uint32_t now = micros();
  if ((now - lastTachUs1) >= TACH_DEBOUNCE_US)
  {
    tachCount1++;
    lastTachUs1 = now;
  }
}

void IRAM_ATTR isrFan2Tach()
{
  uint32_t now = micros();
  if ((now - lastTachUs2) >= TACH_DEBOUNCE_US)
  {
    tachCount2++;
    lastTachUs2 = now;
  }
}

void IRAM_ATTR isrFan3Tach()
{
  uint32_t now = micros();
  if ((now - lastTachUs3) >= TACH_DEBOUNCE_US)
  {
    tachCount3++;
    lastTachUs3 = now;
  }
}

/* =========================
 * HABILITACION DE 12 V
 * ========================= */
void FAN_Power12V(bool on)
{
  digitalWrite(FAN_POWER_EN_PIN, on ? HIGH : LOW);
}

/* =========================
 * FUNCIONES AUXILIARES DE CONTROL
 * ========================= */
uint32_t ConverterPercentParaRPM(uint8_t percent)
{
  if (percent == 0) return 0;
  return map(percent, 1, 100, RPM_GLOBAL_MIN, RPM_GLOBAL_MAX);
}

uint8_t ConverterAlexaBrightnessParaPercent(uint8_t brightness)
{
  if (brightness == 0) return 0;
  return map(brightness, 1, 255, 1, 100);
}

uint8_t ConverterPercentParaAlexaBrightness(uint8_t percent)
{
  if (percent == 0) return 0;
  return map(percent, 1, 100, 1, 255);
}

void AplicarPWMFan(fan_ctrl_t *fan)
{
  ledcWrite(fan->pwmPin, fan->pwmCmd);
}

void IniciarControleFans()
{
  fan1.tachCount = &tachCount1;
  fan1.rpm = 0;
  fan1.rpmTarget = 0;
  fan1.ppr = FAN1_PPR;
  fan1.pwmPin = FAN1_PWM_PIN;
  fan1.pwmCmd = 0;
  fan1.pwmMin = FAN_PWM_MIN_GLOBAL;
  fan1.pwmStart = FAN_PWM_START_GLOBAL;
  fan1.pwmMax = FAN_PWM_MAX_ALL;
  fan1.running = false;
  fan1.startBoostActive = false;
  fan1.startBoostUntilMs = 0;

  fan2.tachCount = &tachCount2;
  fan2.rpm = 0;
  fan2.rpmTarget = 0;
  fan2.ppr = FAN2_PPR;
  fan2.pwmPin = FAN2_PWM_PIN;
  fan2.pwmCmd = 0;
  fan2.pwmMin = FAN_PWM_MIN_GLOBAL;
  fan2.pwmStart = FAN_PWM_START_GLOBAL;
  fan2.pwmMax = FAN_PWM_MAX_ALL;
  fan2.running = false;
  fan2.startBoostActive = false;
  fan2.startBoostUntilMs = 0;

  fan3.tachCount = &tachCount3;
  fan3.rpm = 0;
  fan3.rpmTarget = 0;
  fan3.ppr = FAN3_PPR;
  fan3.pwmPin = FAN3_PWM_PIN;
  fan3.pwmCmd = 0;
  fan3.pwmMin = FAN_PWM_MIN_GLOBAL;
  fan3.pwmStart = FAN_PWM_START_GLOBAL;
  fan3.pwmMax = FAN_PWM_MAX_ALL;
  fan3.running = false;
  fan3.startBoostActive = false;
  fan3.startBoostUntilMs = 0;
}

void AtualizarRPMFan(fan_ctrl_t *fan, uint32_t janelaMs)
{
  uint32_t pulsos = 0;

  noInterrupts();
  pulsos = *(fan->tachCount);
  *(fan->tachCount) = 0;
  interrupts();

  if (!fansLigados || fan->rpmTarget == 0)
  {
    fan->rpm = 0;
    return;
  }

  if (janelaMs == 0 || fan->ppr == 0)
  {
    fan->rpm = 0;
    return;
  }

  fan->rpm = (pulsos * 60000UL) / ((uint32_t)fan->ppr * janelaMs);
}

void ProcessarPartidaFan(fan_ctrl_t *fan, uint32_t now)
{
  if (fan->rpmTarget == 0)
  {
    fan->startBoostActive = false;
    fan->startBoostUntilMs = 0;
    fan->running = false;
    return;
  }

  if (!fan->running && fan->rpm < 300)
  {
    fan->pwmCmd = fan->pwmStart;
    fan->startBoostActive = true;
    fan->startBoostUntilMs = now + FAN_START_BOOST_MS;
    fan->running = true;
  }

  if (fan->startBoostActive && now >= fan->startBoostUntilMs)
  {
    fan->startBoostActive = false;

    if (fan->pwmCmd < fan->pwmMin)
    {
      fan->pwmCmd = fan->pwmMin;
    }
  }
}

void ControlarFanRPM(fan_ctrl_t *fan)
{
  if (fan->rpmTarget == 0)
  {
    fan->pwmCmd = 0;
    fan->running = false;
    fan->startBoostActive = false;
    return;
  }

  if (fan->startBoostActive)
  {
    return;
  }

  int32_t erro = (int32_t)fan->rpmTarget - (int32_t)fan->rpm;

  if (erro > FAN_CTRL_DEADBAND_RPM)
  {
    if (fan->pwmCmd < fan->pwmMin)
      fan->pwmCmd = fan->pwmMin;
    else if ((fan->pwmCmd + FAN_CTRL_STEP_UP) <= fan->pwmMax)
      fan->pwmCmd += FAN_CTRL_STEP_UP;
    else
      fan->pwmCmd = fan->pwmMax;
  }
  else if (erro < -FAN_CTRL_DEADBAND_RPM)
  {
    if (fan->pwmCmd > fan->pwmMin)
    {
      if (fan->pwmCmd > FAN_CTRL_STEP_DOWN)
        fan->pwmCmd -= FAN_CTRL_STEP_DOWN;
      else
        fan->pwmCmd = fan->pwmMin;
    }
  }

  if (fan->pwmCmd > fan->pwmMax)
    fan->pwmCmd = fan->pwmMax;

  if (fan->pwmCmd < fan->pwmMin)
    fan->pwmCmd = fan->pwmMin;
}

void ControleRPMTodosFans()
{
  uint32_t now = millis();
  if ((now - lastCtrlMs) < RPM_CTRL_PERIOD_MS) return;

  uint32_t janelaMs = now - lastCtrlMs;
  lastCtrlMs = now;

  fan1.rpmTarget = rpmTargetGlobal;
  fan2.rpmTarget = rpmTargetGlobal;
  fan3.rpmTarget = rpmTargetGlobal;

  AtualizarRPMFan(&fan1, janelaMs);
  AtualizarRPMFan(&fan2, janelaMs);
  AtualizarRPMFan(&fan3, janelaMs);

  ProcessarPartidaFan(&fan1, now);
  ProcessarPartidaFan(&fan2, now);
  ProcessarPartidaFan(&fan3, now);

  ControlarFanRPM(&fan1);
  ControlarFanRPM(&fan2);
  ControlarFanRPM(&fan3);

  AplicarPWMFan(&fan1);
  AplicarPWMFan(&fan2);
  AplicarPWMFan(&fan3);

  rpmFan1 = fan1.rpm;
  rpmFan2 = fan2.rpm;
  rpmFan3 = fan3.rpm;
}

/* =========================
 * CONTROL DE VENTILADORES
 * ========================= */
void FAN_EnableAll(bool on)
{
  fansLigados = on;

  if (!on)
  {
    softStopActive = false;

    fan1.pwmCmd = 0;
    fan2.pwmCmd = 0;
    fan3.pwmCmd = 0;

    fan1.rpmTarget = 0;
    fan2.rpmTarget = 0;
    fan3.rpmTarget = 0;
    rpmTargetGlobal = 0;

    fan1.running = false;
    fan2.running = false;
    fan3.running = false;

    fan1.startBoostActive = false;
    fan2.startBoostActive = false;
    fan3.startBoostActive = false;

    ledcWrite(FAN1_PWM_PIN, 0);
    ledcWrite(FAN2_PWM_PIN, 0);
    ledcWrite(FAN3_PWM_PIN, 0);

    FAN_Power12V(false);
    fanSpeedPercent = 0;
    rpmFan1 = 0;
    rpmFan2 = 0;
    rpmFan3 = 0;
    AtualizarAlexaEstado();
    return;
  }

  FAN_Power12V(true);
  AtualizarAlexaEstado();
}

void FAN_SetSpeedAll(uint8_t percent)
{
  if (percent > 100) percent = 100;

  softStopActive = false;

  manualSpeedPercent = percent;
  fanSpeedPercent = percent;

  if (percent == 0)
  {
    FAN_EnableAll(false);
    return;
  }

  FAN_Power12V(true);
  fansLigados = true;

  rpmTargetGlobal = ConverterPercentParaRPM(percent);

  fan1.rpmTarget = rpmTargetGlobal;
  fan2.rpmTarget = rpmTargetGlobal;
  fan3.rpmTarget = rpmTargetGlobal;

  AtualizarAlexaEstado();
}

void FAN_RequestSoftStop()
{
  if (!fansLigados)
  {
    FAN_EnableAll(false);
    return;
  }

  softStopActive = true;
  lastSoftStopMs = millis();
}

void ProcessarSoftStop()
{
  if (!softStopActive) return;
  if (!fansLigados)   { softStopActive = false; return; }

  uint32_t now = millis();
  if ((now - lastSoftStopMs) < FAN_SOFTSTOP_STEP_MS) return;
  lastSoftStopMs = now;

  if (fanSpeedPercent > 1)
  {
    if (fanSpeedPercent > FAN_SOFTSTOP_STEP_PCT)
      fanSpeedPercent -= FAN_SOFTSTOP_STEP_PCT;
    else
      fanSpeedPercent = 1;

    rpmTargetGlobal = ConverterPercentParaRPM(fanSpeedPercent);

    fan1.rpmTarget = rpmTargetGlobal;
    fan2.rpmTarget = rpmTargetGlobal;
    fan3.rpmTarget = rpmTargetGlobal;

    AtualizarAlexaEstado();
  }
  else
  {
    FAN_EnableAll(false);
    softStopActive = false;
  }
}

/* =========================
 * ALEXA
 * ========================= */
void AtualizarAlexaEstado()
{
  if (exaustorAlexa == nullptr) return;

  uint8_t brightness = fansLigados ? ConverterPercentParaAlexaBrightness(fanSpeedPercent) : 0;
  exaustorAlexa->setValue(brightness);
}

void AlexaExaustorChanged(uint8_t brightness)
{
  uint8_t percent = ConverterAlexaBrightnessParaPercent(brightness);

  if (percent == 0)
  {
    FAN_RequestSoftStop();
    return;
  }

  if (percent <= 1)
  {
    percent = (manualSpeedPercent > 1) ? manualSpeedPercent : 70;
  }

  FAN_SetSpeedAll(percent);
  SalvarParametros();
}

void IniciarAlexa()
{
  if (alexaIniciada) return;

  exaustorAlexa = new EspalexaDevice(ALEXA_DEVICE_NAME, AlexaExaustorChanged);
  espalexa.addDevice(exaustorAlexa);
  alexaIniciada = true;

  AtualizarAlexaEstado();
}

/* =========================
 * PERSISTENCIA
 * ========================= */
void SalvarParametros()
{
  prefs.putUChar("man_speed", manualSpeedPercent);
}

void CarregarParametros()
{
  manualSpeedPercent = prefs.getUChar("man_speed", 70);

  if (manualSpeedPercent > 100)
  {
    manualSpeedPercent = 70;
  }
}

/* =========================
 * LED DE ESTADO WIFI
 * ========================= */
void IniciarWiFiStatusLed()
{
  pinMode(WIFI_STATUS_LED_PIN, OUTPUT);
  digitalWrite(WIFI_STATUS_LED_PIN, LOW);
  wifiLedState = false;
  wifiLedLastMs = millis();
  wifiLedToggleCount = 0;
}

void AtualizarWiFiStatusLed()
{
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (wifiLedToggleCount < 10)
    {
      if ((now - wifiLedLastMs) >= 25)
      {
        wifiLedLastMs = now;
        wifiLedState = !wifiLedState;
        digitalWrite(WIFI_STATUS_LED_PIN, wifiLedState ? HIGH : LOW);
        wifiLedToggleCount++;
      }
    }
    else
    {
      if (wifiLedState)
      {
        wifiLedState = false;
        digitalWrite(WIFI_STATUS_LED_PIN, LOW);
      }

      if ((now - wifiLedLastMs) >= 1000)
      {
        wifiLedLastMs = now;
        wifiLedToggleCount = 0;
      }
    }
  }
  else
  {
    wifiLedToggleCount = 0;

    if ((now - wifiLedLastMs) >= 500)
    {
      wifiLedLastMs = now;
      wifiLedState = !wifiLedState;
      digitalWrite(WIFI_STATUS_LED_PIN, wifiLedState ? HIGH : LOW);
    }
  }
}

/* =========================
 * RESTABLECER WIFI
 * ========================= */
void ResetarWiFiAgora()
{
  wm.resetSettings();
  delay(300);
  ESP.restart();
}

void IniciarResetWiFiInput()
{
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
}

void VerificarResetWiFiInput()
{
  bool pressed = (digitalRead(WIFI_RESET_PIN) == LOW);
  uint32_t now = millis();

  if (pressed && !wifiResetPressed)
  {
    wifiResetPressed = true;
    wifiResetDone = false;
    wifiResetStartMs = now;
  }

  if (!pressed)
  {
    wifiResetPressed = false;
    wifiResetDone = false;
    wifiResetStartMs = 0;
    return;
  }

  if (wifiResetPressed && !wifiResetDone && ((now - wifiResetStartMs) >= WIFI_RESET_HOLD_MS))
  {
    wifiResetDone = true;
    ResetarWiFiAgora();
  }
}

/* =========================
 * HTML
 * ========================= */
String BuildHtml()
{
  String html;

  html += "<!DOCTYPE html><html lang='pt-BR'>";
  html += "<head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Exaustor Smart Alexa</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{margin:0;font-family:Arial,Helvetica,sans-serif;background:linear-gradient(135deg,#0f172a,#1e293b);color:#e5e7eb;}";
  html += ".wrap{max-width:1150px;margin:auto;padding:20px;}";
  html += ".title{font-size:30px;font-weight:bold;margin-bottom:8px;}";
  html += ".subtitle{color:#94a3b8;margin-bottom:22px;}";
  html += ".topLayout{display:grid;grid-template-columns:260px 1fr;gap:16px;align-items:start;}";
  html += ".stateCol{display:flex;flex-direction:column;gap:16px;}";
  html += ".rpmRow{display:grid;grid-template-columns:repeat(3,minmax(180px,1fr));gap:16px;}";
  html += ".card{background:rgba(30,41,59,.92);padding:20px;border-radius:20px;box-shadow:0 10px 30px rgba(0,0,0,.25);}";
  html += ".label{font-size:14px;color:#94a3b8;}";
  html += ".value{font-size:30px;font-weight:bold;margin-top:10px;}";
  html += ".toolbar{display:flex;flex-wrap:wrap;gap:12px;margin-top:20px;}";
  html += "button{border:none;padding:14px 18px;border-radius:14px;font-size:16px;font-weight:bold;cursor:pointer;box-shadow:0 6px 18px rgba(0,0,0,.18);}";
  html += ".btnOn{background:#16a34a;color:#fff;}";
  html += ".btnOff{background:#dc2626;color:#fff;}";
  html += ".btnGray{background:#475569;color:#fff;}";
  html += ".setbox{margin-top:20px;background:rgba(30,41,59,.92);padding:20px;border-radius:20px;box-shadow:0 10px 30px rgba(0,0,0,.25);}";
  html += ".rangeLabel{font-size:18px;margin-bottom:12px;}";
  html += ".sliderValue{font-weight:bold;color:#93c5fd;}";
  html += "input[type=range]{width:100%;margin-top:8px;}";
  html += ".footer{margin-top:18px;color:#94a3b8;font-size:14px;line-height:1.6;}";
  html += "@media(max-width:860px){";
  html += ".topLayout{grid-template-columns:1fr;}";
  html += ".rpmRow{grid-template-columns:1fr;}";
  html += "}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='wrap'>";
  html += "<div class='title'>Exaustor Smart Alexa</div>";
  html += "<div class='subtitle'>Dev: Lorhan F.R</div>";

  html += "<div class='topLayout'>";

  html += "<div class='stateCol'>";
  html += "<div class='card'><div class='label'>Estado Geral</div><div class='value' id='fans'>--</div></div>";
  html += "<div class='card'><div class='label'>Velocidade</div><div class='value' id='speed'>--</div></div>";
  html += "</div>";

  html += "<div class='rpmRow'>";
  html += "<div class='card'><div class='label'>FAN 1 RPM</div><div class='value' id='rpm1'>--</div></div>";
  html += "<div class='card'><div class='label'>FAN 2 RPM</div><div class='value' id='rpm2'>--</div></div>";
  html += "<div class='card'><div class='label'>FAN 3 RPM</div><div class='value' id='rpm3'>--</div></div>";
  html += "</div>";

  html += "</div>";

  html += "<div class='toolbar'>";
  html += "<button class='btnOn' onclick='fansOn()'>Ligar Exaustor</button>";
  html += "<button class='btnOff' onclick='fansOff()'>Desligar Exaustor</button>";
  html += "</div>";

  html += "<div class='setbox'>";
  html += "<div class='rangeLabel'>Velocidade Manual: <span class='sliderValue' id='msv'>70</span>%</div>";
  html += "<input type='range' min='0' max='100' step='1' id='sliderMS'>";
  html += "<div class='toolbar'>";
  html += "<button class='btnGray' onclick='resetWifi()'>Resetar Wi-Fi</button>";
  html += "</div>";
  html += "<div class='footer'>Acesso: <span id='host'>lab.local</span><br>Alexa: procure pelo dispositivo \"" ALEXA_DEVICE_NAME "\" no app Alexa.</div>";
  html += "</div>";

  html += "<script>";
  html += "let editing=false;";
  html += "function updateUI(d){";
  html += "document.getElementById('fans').innerText=d.fans_on?'Ligados':'Desligados';";
  html += "document.getElementById('speed').innerText=d.fan_speed+' %';";
  html += "document.getElementById('rpm1').innerText=d.rpm1+' RPM';";
  html += "document.getElementById('rpm2').innerText=d.rpm2+' RPM';";
  html += "document.getElementById('rpm3').innerText=d.rpm3+' RPM';";
  html += "document.getElementById('host').innerText=d.host;";
  html += "if(!editing){";
  html += "document.getElementById('sliderMS').value=d.manual_speed;";
  html += "document.getElementById('msv').innerText=d.manual_speed;";
  html += "}";
  html += "}";

  html += "function fetchStatus(){fetch('/status').then(r=>r.json()).then(d=>updateUI(d)).catch(()=>{});}";

  html += "function fansOn(){fetch('/fans/on').then(()=>fetchStatus());}";
  html += "function fansOff(){fetch('/fans/off').then(()=>fetchStatus());}";

  html += "const slider=document.getElementById('sliderMS');";
  html += "slider.addEventListener('input',()=>{editing=true;document.getElementById('msv').innerText=slider.value;});";
  html += "slider.addEventListener('change',()=>{fetch('/manualspeed?value='+slider.value).then(()=>{editing=false;fetchStatus();});});";
  html += "slider.addEventListener('mouseup',()=>{editing=false;});";
  html += "slider.addEventListener('touchend',()=>{editing=false;});";

  html += "function resetWifi(){if(confirm('Apagar Wi-Fi salvo e reiniciar?')) fetch('/resetwifi');}";
  html += "setInterval(fetchStatus,1000);";
  html += "fetchStatus();";
  html += "</script>";

  html += "</div></body></html>";

  return html;
}

/* =========================
 * PETICIONES HTTP
 * ========================= */
void HandleRoot()
{
  server.send(200, "text/html", BuildHtml());
}

void HandleStatus()
{
  String json = "{";
  json += "\"fans_on\":" + String(fansLigados ? "true" : "false");
  json += ",\"fan_speed\":" + String(fanSpeedPercent);
  json += ",\"manual_speed\":" + String(manualSpeedPercent);
  json += ",\"rpm_target\":" + String(rpmTargetGlobal);
  json += ",\"rpm1\":" + String(rpmFan1);
  json += ",\"rpm2\":" + String(rpmFan2);
  json += ",\"rpm3\":" + String(rpmFan3);
  json += ",\"alexa_device\":\"" + String(ALEXA_DEVICE_NAME) + "\"";
  json += ",\"host\":\"" + String(HOSTNAME) + ".local\"";
  json += "}";
  server.send(200, "application/json", json);
}

void HandleFansOn()
{
  FAN_SetSpeedAll(manualSpeedPercent > 0 ? manualSpeedPercent : 70);
  server.send(200, "text/plain", "OK");
}

void HandleFansOff()
{
  FAN_RequestSoftStop();
  server.send(200, "text/plain", "OK");
}

void HandleManualSpeed()
{
  if (!server.hasArg("value"))
  {
    server.send(400, "text/plain", "Parametro ausente");
    return;
  }

  int novo = server.arg("value").toInt();

  if (novo < 0 || novo > 100)
  {
    server.send(400, "text/plain", "Velocidade invalida");
    return;
  }

  manualSpeedPercent = (uint8_t)novo;
  SalvarParametros();

  if (manualSpeedPercent == 0)
  {
    FAN_RequestSoftStop();
  }
  else
  {
    FAN_SetSpeedAll(manualSpeedPercent);
  }

  server.send(200, "text/plain", "OK");
}

void HandleResetWiFi()
{
  server.send(200, "text/plain", "Resetando Wi-Fi...");
  delay(300);
  ResetarWiFiAgora();
}

/* =========================
 * INICIALIZACION
 * ========================= */
void IniciarPWM()
{
  ledcAttach(FAN1_PWM_PIN, FAN_PWM_FREQ, FAN_PWM_RES_BITS);
  ledcAttach(FAN2_PWM_PIN, FAN_PWM_FREQ, FAN_PWM_RES_BITS);
  ledcAttach(FAN3_PWM_PIN, FAN_PWM_FREQ, FAN_PWM_RES_BITS);

  ledcWrite(FAN1_PWM_PIN, 0);
  ledcWrite(FAN2_PWM_PIN, 0);
  ledcWrite(FAN3_PWM_PIN, 0);
}

void IniciarPowerOutput()
{
  pinMode(FAN_POWER_EN_PIN, OUTPUT);
  FAN_Power12V(false);
}

void IniciarTach()
{
  pinMode(FAN1_TACH_PIN, INPUT);
  pinMode(FAN2_TACH_PIN, INPUT);
  pinMode(FAN3_TACH_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(FAN1_TACH_PIN), isrFan1Tach, FALLING);
  attachInterrupt(digitalPinToInterrupt(FAN2_TACH_PIN), isrFan2Tach, FALLING);
  attachInterrupt(digitalPinToInterrupt(FAN3_TACH_PIN), isrFan3Tach, FALLING);
}

void IniciarMDNS()
{
  if (!mdnsIniciado)
  {
    if (MDNS.begin(HOSTNAME))
    {
      MDNS.addService("http", "tcp", 80);
      mdnsIniciado = true;
    }
  }
}

void IniciarServidor()
{
  if (!servidorIniciado)
  {
    server.on("/", HandleRoot);
    server.on("/status", HandleStatus);
    server.on("/fans/on", HandleFansOn);
    server.on("/fans/off", HandleFansOff);
    server.on("/manualspeed", HandleManualSpeed);
    server.on("/resetwifi", HandleResetWiFi);
    server.onNotFound([]() {
      if (!espalexa.handleAlexaApiCall(server.uri(), server.arg(0)))
      {
        server.send(404, "text/plain", "Not found");
      }
    });

    IniciarAlexa();
    espalexa.begin(&server);
    servidorIniciado = true;
  }
}

void IniciarWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);

  wm.setConfigPortalTimeout(180);
  wm.setConfigPortalBlocking(false);

  bool conectado = wm.autoConnect("LAB_FAN_SETUP");

  if (conectado)
  {
    wifiPortalAtivo = false;

    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("mDNS: http://");
    Serial.print(HOSTNAME);
    Serial.println(".local");
    Serial.print("Alexa: ");
    Serial.println(ALEXA_DEVICE_NAME);

    IniciarMDNS();
    IniciarServidor();
  }
  else
  {
    wifiPortalAtivo = true;
    Serial.println("Portal Wi-Fi ativo: LAB_FAN_SETUP");
  }
}

void ManterWiFiManager()
{
  if (wifiPortalAtivo)
  {
    wm.process();

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiPortalAtivo = false;

      Serial.println();
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("mDNS: http://");
      Serial.print(HOSTNAME);
      Serial.println(".local");
      Serial.print("Alexa: ");
      Serial.println(ALEXA_DEVICE_NAME);

      IniciarMDNS();
      IniciarServidor();
    }
  }
}

/* =========================
 * SETUP / LOOP
 * ========================= */
void setup()
{
  Serial.begin(115200);

  prefs.begin("labfan", false);

  CarregarParametros();

  IniciarPWM();
  IniciarPowerOutput();
  IniciarTach();
  IniciarResetWiFiInput();
  IniciarWiFiStatusLed();
  IniciarControleFans();
  lastCtrlMs = millis();

  FAN_EnableAll(false);

  IniciarWiFi();

  if (WiFi.status() == WL_CONNECTED)
  {
    IniciarMDNS();
    IniciarServidor();
  }
}

void loop()
{
  ControleRPMTodosFans();
  ProcessarSoftStop();
  VerificarResetWiFiInput();
  AtualizarWiFiStatusLed();
  ManterWiFiManager();

  if (WiFi.status() == WL_CONNECTED)
  {
    espalexa.loop();
  }
}
