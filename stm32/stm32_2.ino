/*
 * ============================================================
 *  STM32L476RG - 4G SIM7600 + INA219 + DHT22 + Siemens MAG 8000
 *  Moi truong : Arduino IDE + STM32duino core
 *  Board      : Nucleo-64 -> Nucleo L476RG
 *
 *  MAPPING CHAN:
 *  Serial debug  : PA2(TX)/PA3(RX) USART2 - ST-Link VCP
 *  SIM7600 TX->  : PA10 (USART1_RX)
 *  SIM7600 RX<-  : PA9  (USART1_TX)
 *  RS485 RX      : PB11 (USART3_RX)
 *  RS485 TX      : PB10 (USART3_TX)
 *  I2C SDA       : PB7  (I2C1_SDA)
 *  I2C SCL       : PB6  (I2C1_SCL)
 *  DHT22         : PA6
 *  LED           : PA5  (LD2)
 * ============================================================
 */

#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_USE_GPRS true

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <IWatchdog.h>
#include <time.h>

/* ==================== UART ==================== */
// Serial mac dinh KHONG hoat dong tren L476RG -> dung USART2 tuong minh
HardwareSerial SerialPC(USART2);       // PA2(TX)/PA3(RX) -> ST-Link VCP
HardwareSerial SerialSIM(PA10, PA9);   // USART1 -> SIM7600
HardwareSerial RS485(PB11, PB10);      // USART3 -> MAG8000
#define Serial SerialPC                // redirect toan bo Serial -> SerialPC

/* ==================== PINS / BAUD ==================== */
#define SIM_BAUD    115200
#define RS485_BAUD  9600
#define DHT_PIN     PA6
#define DHT_TYPE    DHT22

/* ==================== MAG 8000 MODBUS ==================== */
#define SLAVE_ADDR    0x01
#define TIMEOUT_MS    600

#define REG_FLOW_RATE 0x0001
#define REG_TOTAL_FWD 0x0003
#define REG_TOTAL_REV 0x0005
#define REG_VELOCITY  0x0007
#define REG_STATUS    0x0041

/*
 * Byte order MAG 8000:
 *   0 = Big-Endian    (b0 b1 b2 b3)
 *   1 = Word-Swap     (b2 b3 b0 b1) <- thuong dung
 *   2 = Little-Endian (b3 b2 b1 b0)
 *   3 = Word-Reverse  (b1 b0 b3 b2)
 */
#define MAG_BYTE_ORDER 1

/* ==================== APN VIETTEL ==================== */
const char apn[]  = "v-internet";
const char user[] = "";
const char pass[] = "";

/* ==================== MQTT ==================== */
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

const char* topic_voltage_ina = "viettel/sensor/voltage_ina";
const char* topic_current_ina = "viettel/sensor/current_ina";
const char* topic_power_ina   = "viettel/sensor/power_ina";
const char* topic_temperature = "viettel/sensor/temperature";
const char* topic_humidity    = "viettel/sensor/humidity";
const char* topic_flow_rate   = "viettel/mag8000/flow_rate";
const char* topic_total_fwd   = "viettel/mag8000/total_fwd";
const char* topic_total_rev   = "viettel/mag8000/total_rev";
const char* topic_velocity    = "viettel/mag8000/velocity";
const char* topic_mag_status  = "viettel/mag8000/status";
const char* topic_status      = "viettel/sensor/status";
const char* topic_all         = "viettel/sensor/all";
const char* topic_ack         = "viettel/sensor/ack";
const char* topic_cmd         = "viettel/sensor/cmd";

/* ==================== OBJECTS ==================== */
TinyGsm       modem(SerialSIM);
TinyGsmClient gsmClient(modem);
PubSubClient  mqtt(gsmClient);

Adafruit_INA219 ina219;
DHT dht(DHT_PIN, DHT_TYPE);

/* ==================== THOI GIAN ==================== */
static uint32_t epochOffset = 0;
static uint32_t tickAtSync  = 0;
static bool     ntpSynced   = false;

time_t getEpoch() {
  if (!ntpSynced) return 0;
  return (time_t)(epochOffset + (HAL_GetTick() - tickAtSync) / 1000UL);
}

/* ==================== BIEN TOAN CUC ==================== */
bool systemReady = false;
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 10000;  // 10 giay

float voltage_V = 0, current_mA = 0, power_mW = 0;
float temperature = 0, humidity = 0;
float mag_flow_rate = 0, mag_total_fwd = 0, mag_total_rev = 0, mag_velocity = 0;
uint16_t mag_status = 0xFFFF;

const char* device_id = "stm32_02";
unsigned long totalMessages = 0, successMessages = 0, failedMessages = 0;

/* ==================== WATCHDOG ==================== */
#define WDT_TIMEOUT_MS 26000

/* ==================== CRC MODBUS ==================== */
uint16_t calcCRC(uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

/* ==================== GUI REQUEST MODBUS ==================== */
// Giong ESP32 goc: flush RX buffer -> write request -> delay thay flush TX
void sendRequest(uint8_t addr, uint16_t reg, uint16_t count) {
  uint8_t req[8];
  req[0] = addr;
  req[1] = 0x03;
  req[2] = reg >> 8;   req[3] = reg & 0xFF;
  req[4] = count >> 8; req[5] = count & 0xFF;
  uint16_t crc = calcCRC(req, 6);
  req[6] = crc & 0xFF; req[7] = crc >> 8;
  while (RS485.available()) RS485.read();  // xa RX buffer
  RS485.write(req, 8);
  delay(10);  // cho TX xong (thay RS485.flush() cua ESP32)
}

/* ==================== DOC FLOAT 32-bit (2 REGISTER) ==================== */
bool readFloat32(uint8_t addr, uint16_t reg, float *result) {
  sendRequest(addr, reg, 2);
  uint8_t resp[9]; int i = 0;
  unsigned long t = millis();
  while (millis() - t < TIMEOUT_MS && i < 9)
    if (RS485.available()) { resp[i++] = RS485.read(); t = millis(); }
  if (i < 9) return false;
  if (calcCRC(resp, 7) != (uint16_t)(resp[7] | (uint16_t)resp[8] << 8)) return false;
  if (resp[1] & 0x80) return false;
  uint8_t b0=resp[3], b1=resp[4], b2=resp[5], b3=resp[6];
  uint32_t raw = 0;
  switch (MAG_BYTE_ORDER) {
    case 0: raw = ((uint32_t)b0<<24)|((uint32_t)b1<<16)|((uint32_t)b2<<8)|b3; break;
    case 1: raw = ((uint32_t)b2<<24)|((uint32_t)b3<<16)|((uint32_t)b0<<8)|b1; break;
    case 2: raw = ((uint32_t)b3<<24)|((uint32_t)b2<<16)|((uint32_t)b1<<8)|b0; break;
    case 3: raw = ((uint32_t)b1<<24)|((uint32_t)b0<<16)|((uint32_t)b3<<8)|b2; break;
    default:raw = ((uint32_t)b2<<24)|((uint32_t)b3<<16)|((uint32_t)b0<<8)|b1; break;
  }
  memcpy(result, &raw, 4);
  return true;
}

/* ==================== DOC 1 REGISTER 16-bit ==================== */
bool readWord(uint8_t addr, uint16_t reg, uint16_t *result) {
  sendRequest(addr, reg, 1);
  uint8_t resp[7]; int i = 0;
  unsigned long t = millis();
  while (millis() - t < TIMEOUT_MS && i < 7)
    if (RS485.available()) { resp[i++] = RS485.read(); t = millis(); }
  if (i < 7) return false;
  if (calcCRC(resp, 5) != (uint16_t)(resp[5] | (uint16_t)resp[6] << 8)) return false;
  if (resp[1] & 0x80) return false;
  *result = ((uint16_t)resp[3] << 8) | resp[4];
  return true;
}

/* ==================== DOC TOAN BO MAG 8000 ==================== */
bool readMAG8000() {
  bool ok = true;
  if (!readFloat32(SLAVE_ADDR, REG_FLOW_RATE, &mag_flow_rate))
    { mag_flow_rate = -1; ok = false; }
  delay(60);
  if (!readFloat32(SLAVE_ADDR, REG_TOTAL_FWD, &mag_total_fwd))
    { mag_total_fwd = -1; ok = false; }
  delay(60);
  if (!readFloat32(SLAVE_ADDR, REG_TOTAL_REV, &mag_total_rev))
    { mag_total_rev = -1; ok = false; }
  delay(60);
  if (!readFloat32(SLAVE_ADDR, REG_VELOCITY, &mag_velocity))
    { mag_velocity = -1; ok = false; }
  delay(60);
  if (!readWord(SLAVE_ADDR, REG_STATUS, &mag_status))
    mag_status = 0xFFFF;
  return ok;
}

/* ==================== WAIT RESPONSE (AT command) ==================== */
String waitResponse(unsigned long timeout) {
  String r = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (SerialSIM.available()) { r += (char)SerialSIM.read(); start = millis(); }
    if (r.indexOf("OK") >= 0 || r.indexOf("ERROR") >= 0) break;
  }
  return r;
}

/* ==================== KHOI TAO RS485 ==================== */
bool initRS485() {
  Serial.println("-> Khoi dong RS485 - MAG 8000...");
  RS485.begin(RS485_BAUD, SERIAL_8N1);
  delay(300);
  float flowTest = 0;
  if (!readFloat32(SLAVE_ADDR, REG_FLOW_RATE, &flowTest)) {
    Serial.println("  [FAIL] Khong doc duoc MAG 8000!");
    Serial.println("  -> Kiem tra: day A/B RS485, nguon 24V, dia chi slave=1, baud=9600");
    return false;
  }
  Serial.print("  [OK] MAG 8000 | Luu luong: ");
  Serial.print(flowTest, 4); Serial.println(" m3/h");
  return true;
}

/* ==================== KHOI TAO INA219 ==================== */
bool initINA219() {
  Serial.println("-> Tim INA219...");
  if (!ina219.begin()) {
    Serial.println("  [FAIL] Khong tim thay INA219!");
    return false;
  }
  Serial.print("  [OK] INA219 | ");
  Serial.print(ina219.getBusVoltage_V(), 2); Serial.print("V | ");
  Serial.print(ina219.getCurrent_mA(),  2); Serial.println("mA");
  return true;
}

/* ==================== KHOI TAO DHT22 ==================== */
bool initDHT22() {
  Serial.println("-> Khoi dong DHT22...");
  dht.begin();
  delay(2000);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("  [FAIL] DHT22 khong tra ve du lieu!");
    return false;
  }
  Serial.print("  [OK] DHT22 | ");
  Serial.print(t, 1); Serial.print("C | ");
  Serial.print(h, 1); Serial.println("%");
  return true;
}

/* ==================== KHOI TAO SIM ==================== */
bool initSIM() {
  Serial.println("-> Khoi dong UART SIM...");
  SerialSIM.begin(SIM_BAUD);
  delay(3000);
  for (int i = 0; i < 3; i++) {
    SerialSIM.println("AT");
    String resp = waitResponse(1000);
    if (resp.indexOf("OK") >= 0) {
      SerialSIM.println("AT+CPIN?");
      if (waitResponse(2000).indexOf("READY") >= 0) {
        Serial.println("  [OK] SIM READY");
        return true;
      }
    }
    delay(1000);
  }
  Serial.println("  [FAIL] Khong ket noi duoc voi SIM");
  return false;
}

/* ==================== DANG KY MANG ==================== */
bool registerNetwork() {
  SerialSIM.println("AT+CFUN=1"); waitResponse(3000); delay(2000);
  SerialSIM.println("AT+COPS=0"); waitResponse(30000);
  if (!modem.waitForNetwork(60000)) {
    Serial.println("  [FAIL] Khong dang ky duoc mang!");
    return false;
  }
  Serial.print("  [OK] Mang: "); Serial.print(modem.getOperator());
  Serial.print(" | CSQ: ");       Serial.println(modem.getSignalQuality());
  return true;
}

/* ==================== KET NOI GPRS ==================== */
bool connectGPRS() {
  Serial.print("-> Ket noi GPRS (APN: "); Serial.print(apn); Serial.println(")...");
  for (int i = 0; i < 10; i++) {
    SerialSIM.println("AT+CGATT?");
    if (waitResponse(3000).indexOf("+CGATT: 1") >= 0) break;
    delay(2000);
  }
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.print("  -> Lan thu "); Serial.print(attempt); Serial.println("/3...");
    if (modem.gprsConnect(apn, user, pass)) {
      Serial.print("  [OK] GPRS | IP: ");
      Serial.println(modem.localIP().toString());
      return true;
    }
    modem.gprsDisconnect();
    delay(5000);
  }
  Serial.println("  [FAIL] Ket noi GPRS that bai!");
  return false;
}

/* ==================== KET NOI MQTT ==================== */
bool connectMQTT() {
  Serial.print("-> Ket noi MQTT "); Serial.print(mqtt_server);
  Serial.print(":"); Serial.println(mqtt_port);
  randomSeed(HAL_GetTick());
  String clientId = "STM32_MAG8000_" + String(random(0xffff), HEX);
  if (mqtt.connect(clientId.c_str(), NULL, NULL, topic_status, 0, true, "offline")) {
    mqtt.publish(topic_status, "online", true);
    mqtt.subscribe(topic_ack);
    String cmdTopic = String(topic_cmd) + "/" + String(device_id);
    mqtt.subscribe(cmdTopic.c_str());
    Serial.print("  [OK] MQTT da ket noi, sub: "); Serial.println(cmdTopic);
    return true;
  }
  Serial.println("  [FAIL] MQTT ket noi that bai");
  printMQTTState();
  return false;
}

/* ==================== IN TRANG THAI MQTT ==================== */
void printMQTTState() {
  int state = mqtt.state();
  Serial.print("  MQTT State: "); Serial.print(state); Serial.print(" - ");
  switch (state) {
    case  0: Serial.println("CONNECTED");          break;
    case  1: Serial.println("BAD_PROTOCOL");       break;
    case  2: Serial.println("BAD_CLIENT_ID");      break;
    case  3: Serial.println("UNAVAILABLE");        break;
    case  4: Serial.println("BAD_CREDENTIALS");    break;
    case  5: Serial.println("UNAUTHORIZED");       break;
    case -1: Serial.println("DISCONNECTED");       break;
    case -2: Serial.println("CONNECT_FAILED");     break;
    case -3: Serial.println("CONNECTION_LOST");    break;
    case -4: Serial.println("CONNECTION_TIMEOUT"); break;
    default: Serial.println("UNKNOWN");            break;
  }
}

/* ==================== SYNC NTP ==================== */
bool syncNTP() {
  Serial.println("-> Dong bo thoi gian NTP...");
  SerialSIM.println("AT+CLTS=1"); waitResponse(2000);
  SerialSIM.println("AT&W");      waitResponse(2000);
  SerialSIM.println("AT+CCLK?");
  String resp = waitResponse(3000);
  int q1 = resp.indexOf('"'), q2 = resp.lastIndexOf('"');
  if (q1 < 0 || q2 <= q1) {
    Serial.println("  [FAIL] Khong lay duoc gio");
    return false;
  }
  String ts = resp.substring(q1+1, q2);
  struct tm ti = {};
  ti.tm_year = ts.substring(0,  2).toInt() + 100;
  ti.tm_mon  = ts.substring(3,  5).toInt() - 1;
  ti.tm_mday = ts.substring(6,  8).toInt();
  ti.tm_hour = ts.substring(9,  11).toInt();
  ti.tm_min  = ts.substring(12, 14).toInt();
  ti.tm_sec  = ts.substring(15, 17).toInt();
  epochOffset = (uint32_t)mktime(&ti);
  tickAtSync  = HAL_GetTick();
  ntpSynced   = true;
  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
          ti.tm_hour, ti.tm_min, ti.tm_sec);
  Serial.print("  [OK] Gio: "); Serial.println(buf);
  return true;
}

/* ==================== TIMESTAMP ==================== */
String getISOTimestamp() {
  if (!ntpSynced) return String(millis() / 1000);
  time_t now = getEpoch();
  struct tm *ti = gmtime(&now);
  char buf[25];
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
          ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday,
          ti->tm_hour, ti->tm_min, ti->tm_sec);
  return String(buf);
}

/* ==================== MQTT CALLBACK ==================== */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("\n[MQTT IN] ["); Serial.print(topic);
  Serial.print("]: ");           Serial.println(msg);
  if (String(topic) == topic_ack) {
    if (msg == "DISPLAY_OK") Serial.println("[OK] Web da hien thi!");
    else { Serial.print("[ACK] "); Serial.println(msg); }
  } else if (String(topic).startsWith(topic_cmd)) {
    if (msg == "READ_NOW") {
      Serial.println("[CMD] Doc du lieu ngay lap tuc!");
      readAndSendData();
      lastSendTime = millis();
    } else if (msg == "RUN_TEST") {
      Serial.println("[CMD] Chay test he thong!");
      for (int i=0; i<5; i++) {
        digitalWrite(PA5, HIGH); delay(100);
        digitalWrite(PA5, LOW);  delay(100);
      }
      mqtt.publish(topic_ack, "TEST_SUCCESS");
    }
  }
}

/* ==================== TAO JSON ==================== */
String createJSON() {
  char sw[8];
  snprintf(sw, sizeof(sw), "0x%04X", mag_status);
  auto s2 = [](float v) -> String {
    return (isnan(v)||isinf(v)||v<0) ? String("-1") : String(v, 2);
  };
  auto s4 = [](float v) -> String {
    return (isnan(v)||isinf(v)||v<0) ? String("-1") : String(v, 4);
  };
  String j = "{";
  j += "\"voltage_ina\":" + s2(voltage_V)     + ",";
  j += "\"current_ina\":" + s2(current_mA)    + ",";
  j += "\"power_ina\":" + s2(power_mW)      + ",";
  j += "\"temperature\":" + s2(temperature)   + ",";
  j += "\"humidity\":" + s2(humidity)      + ",";
  j += "\"device_id\":\"" + String(device_id) + "\",";
  j += "\"flow_rate\":" + s4(mag_flow_rate) + ",";
  j += "\"total_fwd\":" + s4(mag_total_fwd) + ",";
  j += "\"total_rev\":" + s4(mag_total_rev) + ",";
  j += "\"velocity\":" + s4(mag_velocity)  + ",";
  j += "\"mag_status\":\"" + String(sw) + "\",";
  j += "\"signal\":" + String(modem.getSignalQuality()) + ",";
  j += "\"timestamp\":\"" + getISOTimestamp() + "\"";
  j += "}";
  return j;
}

/* ==================== DOC VA GUI DU LIEU ==================== */
void readAndSendData() {
  Serial.println("========================================");
  Serial.println("       DOC VA GUI DU LIEU SENSOR        ");
  Serial.println("========================================");

  if (!mqtt.connected()) { Serial.println("[SKIP] MQTT chua ket noi"); return; }

  int csq = modem.getSignalQuality();
  Serial.print("-> CSQ: "); Serial.print(csq);
  Serial.println(csq < 10 ? " (YEU)" : " (TOT)");

  // DHT22
  float _t = dht.readTemperature(), _h = dht.readHumidity();
  if (isnan(_t) || isnan(_h)) {
    temperature = 0; humidity = 0; Serial.println("-> DHT22: FAIL");
  } else {
    temperature = _t; humidity = _h;
    Serial.print("-> DHT22: "); Serial.print(_t,1);
    Serial.print("C | ");       Serial.print(_h,1); Serial.println("%");
  }

  // INA219
  float _v = ina219.getBusVoltage_V();
  float _i = ina219.getCurrent_mA();
  float _p = ina219.getPower_mW();
  voltage_V  = (isnan(_v)||isinf(_v)) ? 0 : _v;
  current_mA = (isnan(_i)||isinf(_i)) ? 0 : _i;
  power_mW   = (isnan(_p)||isinf(_p)) ? 0 : _p;
  Serial.print("-> INA219: "); Serial.print(voltage_V, 2);  Serial.print("V | ");
                               Serial.print(current_mA, 2); Serial.print("mA | ");
                               Serial.print(power_mW, 2);   Serial.println("mW");

  // MAG 8000
  Serial.println("-> MAG 8000:");
  if (readMAG8000()) {
    Serial.print("  [OK] Luu luong : "); Serial.print(mag_flow_rate,4); Serial.println(" m3/h");
    Serial.print("  [OK] Tong thuan: "); Serial.print(mag_total_fwd,4); Serial.println(" m3");
    Serial.print("  [OK] Tong nghich:"); Serial.print(mag_total_rev,4); Serial.println(" m3");
    Serial.print("  [OK] Van toc   : "); Serial.print(mag_velocity, 4); Serial.println(" m/s");
    Serial.print("  [OK] Status    : 0x");
    Serial.print(mag_status, HEX);
    Serial.println(mag_status == 0 ? " (OK)" : " (Canh bao!!)");
  } else {
    Serial.println("  [FAIL] Mot so register khong doc duoc!");
  }

  // PUBLISH MQTT
  Serial.println("\n-> Gui MQTT:");
  Serial.println("----------------------------------------");
  int ok = 0, fail = 0;

  #define PUB(t, val, dec) { \
    String s = (val < 0) ? String("-1") : String(val, dec); \
    Serial.print("  "); Serial.print(t); Serial.print(": "); \
    Serial.print(s); Serial.print(" -> "); \
    if (mqtt.publish(t, s.c_str())) { Serial.println("OK"); ok++; } \
    else { Serial.println("FAIL"); fail++; } \
    delay(80); \
  }

  PUB(topic_voltage_ina,  voltage_V,     2)
  PUB(topic_current_ina,  current_mA,    2)
  PUB(topic_power_ina,    power_mW,      2)
  PUB(topic_temperature,  temperature,   1)
  PUB(topic_humidity,     humidity,      1)
  PUB(topic_flow_rate,    mag_flow_rate, 4)
  PUB(topic_total_fwd,    mag_total_fwd, 4)
  PUB(topic_total_rev,    mag_total_rev, 4)
  PUB(topic_velocity,     mag_velocity,  4)

  {
    char sw[8]; snprintf(sw, sizeof(sw), "0x%04X", mag_status);
    Serial.print("  "); Serial.print(topic_mag_status);
    Serial.print(": "); Serial.print(sw); Serial.print(" -> ");
    if (mqtt.publish(topic_mag_status, sw)) { Serial.println("OK"); ok++; }
    else { Serial.println("FAIL"); fail++; }
    delay(80);
  }

  String json = createJSON();
  Serial.print("\n-> JSON: "); Serial.print(json); Serial.print(" -> ");
  if (mqtt.publish(topic_all, json.c_str())) { Serial.println("OK"); ok++; }
  else { Serial.println("FAIL"); fail++; }

  totalMessages   += ok + fail;
  successMessages += ok;
  failedMessages  += fail;

  Serial.println("\n----------------------------------------");
  Serial.print("[OK] Thanh cong: "); Serial.print(ok);
  Serial.print(" | [FAIL] That bai: "); Serial.println(fail);
  Serial.print("Tong: "); Serial.print(totalMessages);
  Serial.print(" | Ty le: ");
  if (totalMessages > 0)
    Serial.print((float)successMessages / totalMessages * 100, 1);
  Serial.println("%");
  Serial.println("========================================\n");
}

/* ==================== SETUP ==================== */
void setup() {
  pinMode(PA5, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(PA5, HIGH); delay(300);
    digitalWrite(PA5, LOW);  delay(300);
  }

  SerialPC.begin(115200);
  delay(2000);

  for (int i = 0; i < 3; i++) {
    digitalWrite(PA5, HIGH); delay(100);
    digitalWrite(PA5, LOW);  delay(100);
  }

  IWatchdog.begin(WDT_TIMEOUT_MS * 1000UL);
  Serial.println("[OK] Watchdog khoi dong (26s)");

  Serial.println("======================================================");
  Serial.println("  STM32L476RG + SIM7600 + INA219 + DHT22 + MAG 8000  ");
  Serial.println("======================================================");

  Serial.println("=== BUOC 1: I2C ===");
  Wire.begin(); delay(100);
  Serial.println("[OK] I2C OK");

  Serial.println("=== BUOC 2: INA219 ===");
  if (!initINA219()) Serial.println("[WARN] INA219 FAILED - tiep tuc");

  Serial.println("=== BUOC 3: DHT22 ===");
  if (!initDHT22()) Serial.println("[WARN] DHT22 FAILED - tiep tuc");

  Serial.println("=== BUOC 4: MAG 8000 RS485 ===");
  if (!initRS485()) Serial.println("[WARN] MAG8000 FAILED - tiep tuc");

  Serial.println("=== BUOC 5: SIM7600 ===");
  if (!initSIM()) {
    Serial.println("[ERROR] SIM HALTED");
    while (1) { IWatchdog.reload(); delay(5000); }
  }

  Serial.println("=== BUOC 6: DANG KY MANG ===");
  if (!registerNetwork()) {
    Serial.println("[ERROR] NETWORK HALTED");
    while (1) { IWatchdog.reload(); delay(5000); }
  }

  Serial.println("=== BUOC 7: GPRS ===");
  if (!connectGPRS()) {
    Serial.println("[ERROR] GPRS HALTED");
    while (1) { IWatchdog.reload(); delay(5000); }
  }

  Serial.println("=== BUOC 8: NTP ===");
  if (!syncNTP()) Serial.println("[WARN] NTP FAILED - dung millis()");

  Serial.println("=== BUOC 9: MQTT CONFIG ===");
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);
  Serial.println("[OK] MQTT configured");

  Serial.println("=== BUOC 10: MQTT CONNECT ===");
  if (!connectMQTT()) Serial.println("[WARN] MQTT - se thu lai trong loop");

  systemReady = true;
  Serial.println("======================================");
  Serial.println("   HE THONG SAN SANG - 10s/lan       ");
  Serial.println("======================================\n");
}

/* ==================== LOOP ==================== */
void loop() {
  IWatchdog.reload();

  if (!mqtt.connected()) {
    Serial.println("[WARN] MQTT mat ket noi...");
    printMQTTState();
    if (!modem.isGprsConnected()) {
      Serial.println("[WARN] GPRS mat ket noi...");
      if (!connectGPRS()) { delay(5000); return; }
    }
    if (!connectMQTT()) { delay(5000); return; }
  }

  static unsigned long lastNTPSync = 0;
  if (ntpSynced && millis() - lastNTPSync > 86400000UL) {
    lastNTPSync = millis(); syncNTP();
  }

  mqtt.loop();

  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    readAndSendData();
  }

  delay(10);
}
