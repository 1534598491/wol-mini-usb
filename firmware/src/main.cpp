/**
 * WOL-Mini-USB ESP32固件 V1.0
 * 即插即用远程唤醒/睡眠方案
 *
 * 功能:
 * 1. WiFi配网（热点模式 + 扫描WiFi）
 * 2. USB HID键盘（Sleep键唤醒/睡眠）
 * 3. 公共MQTT连接（broker-cn.emqx.io:1883）
 * 4. Token本地验证（安全机制）
 * 5. 心跳上报
 * 6. WiFi自动重连
 * 7. 24小时定期重启
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDSystemControl.h>
#include "mbedtls/sha256.h"

const char* FIRMWARE_VERSION = "1.0.0";
const char* FIRMWARE_DATE = "2026-05-27";
#define BOOT_BUTTON_PIN 0
#define BOOT_PRESS_TIME 5000
#define WIFI_RECONNECT_DELAY 5000
#define REBOOT_INTERVAL (24 * 60 * 60 * 1000)

// AP配置
const char* AP_SSID = "WOL-Mini-USB";

// 公共MQTT服务器（TLS加密）
const char* DEFAULT_MQTT_SERVER = "broker-cn.emqx.io";
const int DEFAULT_MQTT_PORT = 8883;  // TLS端口

// 全局变量
Preferences preferences;
WebServer configServer(80);
DNSServer dnsServer;
WiFiClientSecure mqttWifiClient;  // TLS加密客户端（改名避免冲突）
PubSubClient mqttClient(mqttWifiClient);
USBHIDKeyboard keyboard;
USBHIDSystemControl systemControl;

struct Config {
  String wifiSSID;
  String wifiPassword;
  String deviceName;     // 用户自定义名称
  String deviceId;       // 完整ID = 名称-MAC后4位
  String controlToken;
  String activationCode; // 激活码
} config;

bool isActivated = false;  // 激活状态
bool configMode = false;
unsigned long lastHeartbeat = 0;
unsigned long bootPressStart = 0;
bool bootPressed = false;
unsigned long lastWifiReconnect = 0;
unsigned long bootTime = 0;

// Forward declarations
void loadConfig();
void saveConfig();
void clearWifiConfig();
void checkBootButton();
void enterConfigMode();
void handleConfigRoot();
void handleWiFiScan();
void handleConfigSave();
void handleStatus();
void setupNormalMode();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void triggerWake();
void triggerSleep();
void triggerWakeOrSleep();
void sendHeartbeat();
void sendResult(const char* action, const char* result, const char* reason);
String getSignalLevel(int rssi);
String getBandInfo(int channel);
bool is5GHz(int channel);
bool validateActivationCode(String code);
bool checkActivation();

void setup() {
  // 关键：USB HID必须先初始化（在WiFi之前）
  USB.begin();
  keyboard.begin();
  systemControl.begin();

  Serial.begin(115200);
  Serial.println("\n\n=== WOL-Mini-USB V1.0 ===");
  Serial.println("USB HID initialized");

  // 等待USB枚举完成
  delay(1000);

  bootTime = millis();

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  loadConfig();

  if (config.wifiSSID.length() == 0) {
    Serial.println("No WiFi config, entering provisioning mode...");
    enterConfigMode();
  } else {
    Serial.println("WiFi config found, entering normal mode...");
    setupNormalMode();
  }
}

void loop() {
  checkBootButton();
  configServer.handleClient();

  if (configMode) {
    dnsServer.processNextRequest();
    digitalWrite(LED_BUILTIN, (millis() % 500 < 250) ? HIGH : LOW);
  } else {
    // 定期重启（24小时）
    if (millis() - bootTime > REBOOT_INTERVAL) {
      Serial.println("\n=== Periodic reboot (24h) ===");
      delay(1000);
      ESP.restart();
    }

    // WiFi状态检查
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqttClient.connected()) {
        connectMQTT();
        digitalWrite(LED_BUILTIN, HIGH);
      } else {
        digitalWrite(LED_BUILTIN, LOW);
        mqttClient.loop();

        // 心跳上报（30秒间隔）
        if (millis() - lastHeartbeat > 30000) {
          sendHeartbeat();
          lastHeartbeat = millis();
        }
      }
    } else {
      digitalWrite(LED_BUILTIN, HIGH);

      if (millis() - lastWifiReconnect > WIFI_RECONNECT_DELAY) {
        Serial.println("WiFi lost, attempting reconnect...");
        WiFi.reconnect();
        lastWifiReconnect = millis();

        delay(2000);
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("WiFi reconnected! IP: " + WiFi.localIP().toString());
          connectMQTT();
        }
      }
    }
  }
}

// ===== 配置管理 =====

void loadConfig() {
  preferences.begin("wol-mini-usb", true);
  config.wifiSSID = preferences.getString("wifi_ssid", "");
  config.wifiPassword = preferences.getString("wifi_pass", "");
  config.deviceName = preferences.getString("device_name", "");
  config.deviceId = preferences.getString("device_id", "");
  config.controlToken = preferences.getString("control_token", "");
  config.activationCode = preferences.getString("activation_code", "");
  preferences.end();

  // 检查激活状态
  isActivated = checkActivation();

  // 如果没有设备ID，生成默认值
  if (config.deviceId.length() == 0) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String macSuffix = mac.substring(mac.length() - 4);
    String defaultName = config.deviceName.length() > 0 ? config.deviceName : "mypc";
    config.deviceId = defaultName + "-" + macSuffix;
  }

  Serial.println("Config loaded:");
  Serial.println("  WiFi: " + config.wifiSSID);
  Serial.println("  Device Name: " + config.deviceName);
  Serial.println("  Device ID: " + config.deviceId);
  Serial.println("  Token: " + (config.controlToken.length() > 0 ? String("(已设置)") : String("(未设置)")));
  Serial.println("  Activated: " + String(isActivated ? "Yes" : "No"));
}

void saveConfig() {
  preferences.begin("wol-mini-usb", false);
  preferences.putString("wifi_ssid", config.wifiSSID);
  preferences.putString("wifi_pass", config.wifiPassword);
  preferences.putString("device_name", config.deviceName);
  preferences.putString("device_id", config.deviceId);
  preferences.putString("control_token", config.controlToken);
  if (config.activationCode.length() > 0) {
    preferences.putString("activation_code", config.activationCode);
  }
  preferences.end();
  isActivated = checkActivation();
  Serial.println("Config saved! Activated: " + String(isActivated ? "Yes" : "No"));
}

// ===== 激活码验证 =====

// 密钥（与Python脚本保持一致）
const char* ACTIVATION_SECRET = "WOL-MINI-USB-2024-SECRET";

// HMAC计算函数（用于消息签名）
String computeHMAC(String data, String key) {
  uint8_t hash[32];
  mbedtls_sha256_context sha_ctx;

  String combined = key + data;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, false);
  mbedtls_sha256_update(&sha_ctx, (uint8_t*)combined.c_str(), combined.length());
  mbedtls_sha256_finish(&sha_ctx, hash);
  mbedtls_sha256_free(&sha_ctx);

  // 返回前16位十六进制
  String result = "";
  for (int i = 0; i < 8; i++) {
    char hex[3];
    sprintf(hex, "%02X", hash[i]);
    result += hex;
  }
  return result;
}

bool validateActivationCode(String code) {
  // 激活码格式：XXXX-XXXX-XXXX（12位十六进制）
  if (code.length() != 14) return false;

  // 检查格式：XXXX-XXXX-XXXX
  if (code.charAt(4) != '-' || code.charAt(9) != '-') return false;

  // 检查每段是否为有效十六进制字符
  for (int i = 0; i < 14; i++) {
    if (i == 4 || i == 9) continue;
    char c = code.charAt(i);
    if (!((c >= 'A' && c <= 'F') || (c >= '0' && c <= '9'))) return false;
  }

  // 基于MAC地址验证激活码
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toUpperCase();

  // 组合MAC和密钥
  String data = mac + String(ACTIVATION_SECRET);

  // SHA256哈希（ESP32硬件支持）
  uint8_t hash[32];
  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, false);  // false = SHA256 (not SHA224)
  mbedtls_sha256_update(&sha_ctx, (uint8_t*)data.c_str(), data.length());
  mbedtls_sha256_finish(&sha_ctx, hash);
  mbedtls_sha256_free(&sha_ctx);

  // 将哈希转换为十六进制字符串，取前12位
  String expectedCode = "";
  for (int i = 0; i < 6; i++) {
    char hex[3];
    sprintf(hex, "%02X", hash[i]);
    expectedCode += hex;
  }

  // 格式化期望的激活码
  String expectedFormatted = expectedCode.substring(0, 4) + "-" +
                             expectedCode.substring(4, 8) + "-" +
                             expectedCode.substring(8, 12);

  // 对比输入的激活码和期望的激活码
  code.toUpperCase();
  return code == expectedFormatted;
}

bool checkActivation() {
  // 检查是否已激活
  if (config.activationCode.length() == 0) {
    return false;
  }
  return validateActivationCode(config.activationCode);
}

void clearWifiConfig() {
  preferences.begin("wol-mini-usb", false);
  preferences.remove("wifi_ssid");
  preferences.remove("wifi_pass");
  preferences.end();
  Serial.println("WiFi config cleared!");
}

// ===== BOOT按键检测 =====

void checkBootButton() {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (!bootPressed) {
      bootPressed = true;
      bootPressStart = millis();
      Serial.println("BOOT pressed, waiting 5s...");
    } else if (millis() - bootPressStart >= BOOT_PRESS_TIME) {
      Serial.println("\n=== BOOT long press! Entering config mode (config preserved) ===");
      // 不清空配置，直接进入配网模式查看/修改
      enterConfigMode();
    }
  } else {
    if (bootPressed) {
      bootPressed = false;
      Serial.println("BOOT released");
    }
  }
}

// ===== WiFi扫描辅助函数 =====

String getSignalLevel(int rssi) {
  if (rssi >= -50) return "极强";
  if (rssi >= -60) return "强";
  if (rssi >= -70) return "良好";
  if (rssi >= -80) return "弱";
  return "极弱";
}

String getBandInfo(int channel) {
  if (channel >= 1 && channel <= 14) return "2.4G";
  return "5G(不支持)";
}

bool is5GHz(int channel) {
  return channel > 14;
}

void handleWiFiScan() {
  Serial.println("=== SCAN START ===");
  int num = WiFi.scanNetworks(false, false, false, 300);
  Serial.println("Scan result: " + String(num) + " networks");

  struct NetworkInfo {
    String ssid;
    int rssi;
    int channel;
    String band;
    bool secure;
    String signal;
    bool is5g;
  };

  NetworkInfo networks[20];
  int count = num > 20 ? 20 : num;

  for (int i = 0; i < count; i++) {
    networks[i].ssid = WiFi.SSID(i);
    if (networks[i].ssid.length() == 0) networks[i].ssid = "(hidden)";
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].channel = WiFi.channel(i);
    networks[i].band = getBandInfo(networks[i].channel);
    networks[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    networks[i].signal = getSignalLevel(networks[i].rssi);
    networks[i].is5g = is5GHz(networks[i].channel);
  }

  // 按RSSI排序
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (networks[i].rssi < networks[j].rssi) {
        NetworkInfo temp = networks[i];
        networks[i] = networks[j];
        networks[j] = temp;
      }
    }
  }

  String json = "{\"count\":" + String(count) + ",\"networks\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    String ssid = networks[i].ssid;
    ssid.replace("\"", "\\\"");
    ssid.replace("\\", "\\\\");
    json += "{\"ssid\":\"" + ssid + "\"";
    json += ",\"rssi\":" + String(networks[i].rssi);
    json += ",\"channel\":" + String(networks[i].channel);
    json += ",\"band\":\"" + networks[i].band + "\"";
    json += ",\"secure\":" + String(networks[i].secure ? "true" : "false");
    json += ",\"signal\":\"" + networks[i].signal + "\"";
    json += ",\"is5g\":" + String(networks[i].is5g ? "true" : "false");
    json += "}";
  }
  json += "]}";

  configServer.send(200, "application/json", json);
  Serial.println("JSON sent, sorted by signal strength");
}

// ===== 配网模式 =====

void enterConfigMode() {
  configMode = true;
  Serial.println("\n=== ENTER CONFIG MODE ===");

  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_AP_STA);
  delay(500);

  WiFi.softAP(AP_SSID);
  delay(500);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("AP: " + String(AP_SSID));
  Serial.println("IP: " + apIP.toString());

  dnsServer.start(53, "*", apIP);

  configServer.on("/", handleConfigRoot);
  configServer.on("/scan", handleWiFiScan);
  configServer.on("/save", HTTP_POST, handleConfigSave);
  configServer.on("/status", handleStatus);
  configServer.onNotFound(handleConfigRoot);
  configServer.begin();
  Serial.println("Web server started");
}

void handleConfigRoot() {
  String html = F("<!DOCTYPE html><html lang='zh-CN'><head>");
  html += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>");
  html += F("<title>WOL-Mini-USB 配网</title>");
  html += F("<style>");
  html += F("*{margin:0;padding:0;box-sizing:border-box}");
  html += F("body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;");
  html += F("background:linear-gradient(135deg,#1e293b,#0f172a);color:#f8fafc;min-height:100vh;padding:20px}");
  html += F(".container{max-width:420px;margin:0 auto}");
  html += F("h1{text-align:center;color:#22c55e;margin-bottom:8px}");
  html += F(".subtitle{text-align:center;color:#64748b;margin-bottom:24px}");
  html += F(".card{background:#1e293b;border-radius:12px;padding:20px;margin-bottom:16px}");
  html += F(".card-title{color:#94a3b8;font-size:13px;margin-bottom:12px}");
  html += F("label{display:block;color:#94a3b8;font-size:13px;margin-bottom:6px}");
  html += F("input{width:100%;padding:12px;background:#334155;border:1px solid #475569;");
  html += F("border-radius:8px;color:#f8fafc;font-size:15px}");
  html += F(".hint{font-size:11px;color:#64748b;margin-top:4px}");
  html += F(".wifi-list{max-height:200px;overflow-y:auto;margin-bottom:12px}");
  html += F(".wifi-item{display:flex;padding:12px;background:#334155;border-radius:8px;margin-bottom:8px;cursor:pointer}");
  html += F(".wifi-item:hover{background:#475569}");
  html += F(".wifi-item.selected{background:#22c55e}");
  html += F(".wifi-name{font-size:15px;color:#f8fafc}");
  html += F(".wifi-signal{font-size:12px;color:#64748b}");
  html += F(".wifi-band{font-size:11px;color:#94a3b8;margin-left:4px}");
  html += F(".wifi-5g{color:#f59e0b}");
  html += F(".wifi-item.disabled{opacity:0.5;pointer-events:none}");
  html += F(".scan-btn{width:100%;padding:12px;background:#334155;border-radius:8px;color:#94a3b8;cursor:pointer;margin-bottom:12px;border:none}");
  html += F(".submit-btn{width:100%;padding:16px;background:#22c55e;border-radius:8px;color:#fff;font-size:16px;cursor:pointer;border:none}");
  html += F(".info-box{background:#334155;padding:12px;border-radius:8px;margin:12px 0;color:#f59e0b;font-size:12px}");
  html += F("</style></head><body>");

  html += F("<div class='container'>");
  html += F("<h1>WOL-Mini-USB</h1>");
  html += F("<p class='subtitle'>即插即用远程唤醒/睡眠</p>");

  // 使用说明
  html += F("<div class='info-box'>");
  html += F("使用方式：ESP32插入PC USB口 → 配网 → 打开Web控制页面 → 点击唤醒/睡眠");
  html += F("</div>");

  // 显示MAC地址（用于生成激活码）
  String macAddress = WiFi.macAddress();
  html += F("<div class='card' style='background:#1e3a5f'>");
  html += F("<div class='card-title' style='color:#22c55e'>设备信息</div>");
  html += F("<label style='font-size:12px;color:#64748b'>MAC地址（用于生成激活码）</label>");
  html += "<div style='padding:12px;background:#334155;border-radius:8px;font-size:16px;color:#22c55e;font-weight:bold;text-align:center;font-family:monospace'>" + macAddress + "</div>";
  html += F("<p style='font-size:11px;color:#94a3b8;margin-top:8px'>购买激活码请联系卖家，提供此MAC地址</p>");
  html += F("</div>");

  // WiFi选择
  html += F("<div class='card'>");
  html += F("<div class='card-title'>WiFi 网络</div>");
  html += F("<button class='scan-btn' onclick='scanWiFi()'>点击扫描WiFi</button>");
  html += F("<div class='wifi-list' id='wifiList' style='color:#64748b;text-align:center;padding:20px'>请点击上方按钮扫描WiFi</div>");
  html += F("<input type='hidden' id='selectedSSID'>");
  html += F("<div id='passwordField' style='display:none'>");
  html += F("<label>WiFi 密码</label>");
  html += F("<input type='password' id='wifi_pass' placeholder='输入WiFi密码'>");
  html += F("</div></div>");

  // 设备配置
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String macSuffix = mac.substring(mac.length() - 4);
  String defaultName = config.deviceName.length() > 0 ? config.deviceName : "mypc";
  String fullId = defaultName + "-" + macSuffix;

  html += F("<div class='card'>");
  html += F("<div class='card-title'>设备设置</div>");
  html += F("<label>设备名称（简单易记）</label>");
  html += "<input id='device_name' value='" + defaultName + "' oninput='updateDeviceId()'>";
  html += F("<p class='hint'>输入简单名称，如 mypc、office、home</p>");
  html += F("<label>完整设备ID（自动生成）</label>");
  html += "<div id='full_id_display' style='padding:12px;background:#334155;border-radius:8px;color:#22c55e;font-weight:bold'>" + fullId + "</div>";
  html += F("<p class='hint'>登录Web时需要输入这个完整ID，注意大小写敏感</p>");
  html += F("</div>");

  // 安全设置（Token）
  html += F("<div class='card'>");
  html += F("<div class='card-title'>安全设置</div>");
  html += F("<label>控制Token</label>");
  html += "<input id='control_token' value='" + config.controlToken + "' placeholder='请设置控制密码'>";
  html += F("<p class='hint'>Token用于验证远程控制指令，请牢记此密码（建议8-16位）</p>");
  html += F("</div>");

  // 激活码设置
  html += F("<div class='card'>");
  html += F("<div class='card-title'>激活码（可选）</div>");
  html += F("<label>设备激活码</label>");
  html += "<input id='activation_code' value='" + config.activationCode + "' placeholder='格式: XXXX-XXXX-XXXX'>";
  html += F("<p class='hint'>输入激活码解锁完整功能，无激活码仅能唤醒</p>");
  if (!isActivated && config.activationCode.length() == 0) {
    html += F("<p style='color:#f59e0b;font-size:11px'>⚠️ 未激活：仅唤醒功能可用</p>");
  } else if (isActivated) {
    html += F("<p style='color:#22c55e;font-size:11px'>✅ 已激活：完整功能可用</p>");
  }
  html += F("</div>");

  html += F("<button class='submit-btn' onclick='saveConfig()'>保存配置</button>");
  html += F("</div>");

  // JavaScript
  html += F("<script>");
  html += "const MAC_SUFFIX = '" + macSuffix + "';";
  html += F("function updateDeviceId(){");
  html += F("let name=document.getElementById('device_name').value.trim();");
  html += F("if(name.length==0) name='mypc';");
  html += F("let fullId=name+'-'+MAC_SUFFIX;");
  html += F("document.getElementById('full_id_display').textContent=fullId;");
  html += F("}");
  html += F("function scanWiFi(){");
  html += F("document.getElementById('wifiList').innerHTML='<p style=\"color:#f59e0b\">正在扫描，请耐心等待约30秒...</p>';");
  html += F("fetch('/scan').then(r=>r.json()).then(d=>{");
  html += F("let list=document.getElementById('wifiList');");
  html += F("if(d.count==0){list.innerHTML='<p style=\"color:#64748b\">未发现WiFi</p>';return;}");
  html += F("list.innerHTML='';");
  html += F("d.networks.forEach(n=>{");
  html += F("let item=document.createElement('div');item.className='wifi-item';");
  html += F("if(n.is5g){item.className+=' disabled';}");
  html += F("if(!n.is5g){item.onclick=()=>selectWiFi(n.ssid,n.secure);}");
  html += F("let bandClass=n.is5g?'wifi-band wifi-5g':'wifi-band';");
  html += F("item.innerHTML='<div><div class=\"wifi-name\">'+n.ssid+' <span class=\"'+bandClass+'\">'+n.band+'</span></div>");
  html += F("<div class=\"wifi-signal\">'+n.signal+' ('+n.rssi+'dBm)</div></div>';");
  html += F("if(n.is5g){item.innerHTML+='<div style=\"color:#f59e0b;font-size:11px\">不支持5G</div>';}");
  html += F("list.appendChild(item);});}).catch(e=>{document.getElementById('wifiList').innerHTML='<p style=\"color:#ef4444\">扫描失败，请重试</p>';});}");
  html += F("function selectWiFi(ssid,secure){");
  html += F("document.querySelectorAll('.wifi-item:not(.disabled)').forEach(i=>i.classList.remove('selected'));");
  html += F("event.currentTarget.classList.add('selected');");
  html += F("document.getElementById('selectedSSID').value=ssid;");
  html += F("document.getElementById('passwordField').style.display=secure?'block':'none';}");
  html += F("function saveConfig(){");
  html += F("let ssid=document.getElementById('selectedSSID').value;");
  html += F("let token=document.getElementById('control_token').value;");
  html += F("let name=document.getElementById('device_name').value.trim();");
  html += F("if(!token){alert('请设置控制Token');return;}");
  html += F("if(!name){alert('请设置设备名称');return;}");
  html += F("if(ssid.length==0){alert('请选择WiFi');return;}");
  html += F("let fullId=name+'-'+MAC_SUFFIX;");
  html += F("let activationCode=document.getElementById('activation_code').value.trim();");
  html += F("let data={wifi_ssid:ssid,wifi_pass:document.getElementById('wifi_pass').value,");
  html += F("device_name:name,device_id:fullId,");
  html += F("control_token:token,activation_code:activationCode};");
  html += F("fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})");
  html += F(".then(r=>r.text()).then(h=>{document.body.innerHTML=h;}).catch(e=>{alert('保存失败:'+e);});}");
  html += F("</script></body></html>");

  configServer.send(200, "text/html", html);
}

void handleConfigSave() {
  String body = configServer.arg("plain");
  JsonDocument doc;
  deserializeJson(doc, body);

  String newSSID = doc["wifi_ssid"].as<String>();
  if (newSSID.length() > 0 && newSSID != "(hidden)") {
    config.wifiSSID = newSSID;
    config.wifiPassword = doc["wifi_pass"].as<String>();
  }

  config.deviceName = doc["device_name"].as<String>();
  config.deviceId = doc["device_id"].as<String>();
  config.controlToken = doc["control_token"].as<String>();

  // 保存激活码
  String newActivationCode = doc["activation_code"].as<String>();
  if (newActivationCode.length() > 0) {
    if (validateActivationCode(newActivationCode)) {
      config.activationCode = newActivationCode;
      Serial.println("Activation code valid: " + newActivationCode);
    } else {
      Serial.println("Activation code invalid: " + newActivationCode);
    }
  }

  saveConfig();

  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'></head>");
  html += F("<body style='background:#1e293b;color:#f8fafc;text-align:center;padding:40px'>");
  html += F("<h1 style='color:#22c55e'>配置已保存!</h1>");
  html += "<p style='font-size:18px;color:#22c55e;margin:20px 0'>设备ID: <b>" + config.deviceId + "</b></p>";
  html += "<p>Token: <b>" + config.controlToken + "</b></p>";

  // 显示激活状态
  if (isActivated) {
    html += F("<p style='color:#22c55e'>✅ 已激活：完整功能可用</p>");
  } else {
    html += F("<p style='color:#f59e0b'>⚠️ 未激活：仅唤醒功能可用</p>");
  }

  html += F("<p style='margin-top:20px;color:#64748b;font-size:14px'>请将ESP32插入PC USB口，设备即将重启...</p></body></html>");
  html += F("<p style='margin-top:20px;color:#64748b;font-size:14px'>请将ESP32插入PC USB口，设备即将重启...</p></body></html>");

  configServer.send(200, "text/html", html);

  delay(2000);
  ESP.restart();
}

void handleStatus() {
  JsonDocument doc;
  doc["mode"] = configMode ? "config" : "normal";
  doc["wifi"] = config.wifiSSID;
  doc["mqtt_connected"] = mqttClient.connected();
  doc["mqtt_state"] = mqttClient.state();
  doc["device_name"] = config.deviceName;
  doc["device_id"] = config.deviceId;
  doc["token_set"] = config.controlToken.length() > 0;
  doc["ip"] = WiFi.localIP().toString();
  doc["usb_hid"] = "ready";
  String json;
  serializeJson(doc, json);
  configServer.send(200, "application/json", json);
}

// ===== 正常模式 =====

void setupNormalMode() {
  Serial.println("Connecting to WiFi: " + config.wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSSID.c_str(), config.wifiPassword.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

    configServer.on("/status", handleStatus);
    configServer.begin();
    Serial.println("HTTP server started");

    // 配置TLS（使用内置CA证书）
    mqttWifiClient.setInsecure();  // 公共MQTT，不验证服务器证书（简化）
    // 或者使用CA证书验证（更安全但需要证书）

    mqttClient.setServer(DEFAULT_MQTT_SERVER, DEFAULT_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);
    Serial.println("MQTT configured (TLS): " + String(DEFAULT_MQTT_SERVER) + ":" + String(DEFAULT_MQTT_PORT));
  } else {
    Serial.println("\nWiFi failed, re-entering config mode...");
    enterConfigMode();
  }
}

void connectMQTT() {
  String clientId = config.deviceId + "-" + WiFi.macAddress();
  Serial.println("\n=== MQTT Connection ===");
  Serial.println("Server: " + String(DEFAULT_MQTT_SERVER) + ":" + String(DEFAULT_MQTT_PORT));
  Serial.println("ClientId: " + clientId);
  Serial.println("Connecting...");

  // 公共MQTT无认证
  for (int i = 0; i < 3; i++) {
    Serial.println("Attempt " + String(i+1) + "/3...");
    bool result = mqttClient.connect(clientId.c_str());
    int state = mqttClient.state();
    Serial.println("Result: " + String(result) + ", State: " + String(state));

    if (result) {
      Serial.println("MQTT connected!");
      // 订阅控制Topic
      String controlTopic = "wol-mini/" + config.deviceId + "/control";
      mqttClient.subscribe(controlTopic.c_str(), 1);
      Serial.println("Subscribed: " + controlTopic);
      sendHeartbeat();
      return;
    }
    delay(5000);
  }

  Serial.println("MQTT connection failed after 3 attempts");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("\n=== MQTT Message ===");
  Serial.println("Topic: " + String(topic));

  JsonDocument doc;
  deserializeJson(doc, payload, length);

  String action = doc["action"].as<String>();
  long timestamp = doc["timestamp"].as<long>();
  String receivedSignature = doc["signature"].as<String>();

  Serial.println("Action: " + action);
  Serial.println("Timestamp: " + String(timestamp));
  // 不打印签名，保持安全

  // 时间戳验证（防止重放攻击）
  long currentTimestamp = millis() / 1000;
  if (abs(currentTimestamp - timestamp) > 300) {  // 5分钟有效期
    Serial.println("时间戳过期，拒绝执行");
    sendResult(action.c_str(), "rejected", "timestamp_expired");
    return;
  }

  // HMAC签名验证（Token不传输，更安全）
  String data = action + String(timestamp);
  String expectedSignature = computeHMAC(data, config.controlToken);

  if (receivedSignature != expectedSignature) {
    Serial.println("签名验证失败，拒绝执行");
    sendResult(action.c_str(), "rejected", "invalid_signature");
    return;
  }

  Serial.println("签名验证通过");

  // 区分唤醒和睡眠操作
  if (action == "wake") {
    // 唤醒：所有设备都可以执行
    Serial.println("触发唤醒...");
    triggerWake();
    sendResult(action.c_str(), "success", "");
  } else if (action == "sleep") {
    // 睡眠：需要激活才能执行
    if (!isActivated) {
      Serial.println("未激活，拒绝执行睡眠操作");
      sendResult(action.c_str(), "rejected", "not_activated");
      return;
    }
    Serial.println("触发睡眠...");
    triggerSleep();
    sendResult(action.c_str(), "success", "");
    triggerSleep();
    sendResult(action.c_str(), "success", "");
  } else if (action == "ping" || action == "status") {
    sendHeartbeat();
    sendResult(action.c_str(), "success", "");
  }
}

// ===== USB HID触发 =====

void triggerWake() {
  // 唤醒：发送空格键唤醒睡眠的PC
  Serial.println("\n=== USB HID Wake ===");
  keyboard.write(' ');
  Serial.println("=== Wake: Space sent ===\n");
}

void triggerSleep() {
  // 睡眠：发送System Standby命令
  Serial.println("\n=== USB HID Sleep ===");
  systemControl.press(SYSTEM_CONTROL_STANDBY);
  delay(50);
  systemControl.release();
  Serial.println("=== Sleep: System Standby sent ===\n");
}

void triggerWakeOrSleep() {
  // 保留此函数作为备用（同时发送唤醒和睡眠）
  Serial.println("\n=== USB HID Combined ===");
  delay(100);

  // System Standby
  systemControl.press(SYSTEM_CONTROL_STANDBY);
  delay(100);
  systemControl.release();
  delay(100);

  // Space key
  keyboard.press(' ');
  delay(100);
  keyboard.release(' ');
  delay(50);

  // Enter key
  keyboard.press(KEY_RETURN);
  delay(100);
  keyboard.release(KEY_RETURN);

  Serial.println("=== Combined Commands Sent ===\n");
}

void sendHeartbeat() {
  JsonDocument doc;
  doc["device_id"] = config.deviceId;
  doc["status"] = "online";
  doc["ip"] = WiFi.localIP().toString();
  doc["timestamp"] = millis() / 1000;
  doc["usb_hid"] = "ready";
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["firmware_date"] = FIRMWARE_DATE;
  doc["activated"] = isActivated;

  String msg;
  serializeJson(doc, msg);

  String statusTopic = "wol-mini/" + config.deviceId + "/status";
  mqttClient.publish(statusTopic.c_str(), msg.c_str());
  Serial.println("Heartbeat sent: " + statusTopic);
}

void sendResult(const char* action, const char* result, const char* reason) {
  JsonDocument doc;
  doc["device_id"] = config.deviceId;
  doc["action"] = action;
  doc["result"] = result;
  if (strlen(reason) > 0) {
    doc["reason"] = reason;
  }
  doc["timestamp"] = millis() / 1000;

  String msg;
  serializeJson(doc, msg);

  String statusTopic = "wol-mini/" + config.deviceId + "/status";
  mqttClient.publish(statusTopic.c_str(), msg.c_str());
  Serial.println("Result sent: " + String(result));
}