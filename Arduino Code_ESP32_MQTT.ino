#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ================= WIFI =================
const char* ssid     = "awas kontrakan";
const char* password = "123horee";

// ================= HIVEMQ CLOUD =================
const char* mqtt_server = "d9f9a8170ace4b888983e922fe4c25a0.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "skripsi";
const char* mqtt_pass   = "Skripsi123";

const char* topic_detection = "sensor/result/bolt";
const char* topic_log       = "sensor/log/bolt";

WiFiClientSecure espClient;
PubSubClient     client(espClient);

// ================= PIN SERVO =================
// Setiap servo mendorong baut ke kotak sortirnya masing-masing
const int SERVO_FLANGE_PIN   = 13;   // Servo untuk Flange Bolt
const int SERVO_HEX_PIN      = 12;   // Servo untuk Hex Bolt
const int SERVO_SOCKET_PIN   = 14;   // Servo untuk Socket Cap
// Carriage Bolt → tidak ada servo, jatuh ke kotak ujung conveyor

// ================= PIN SENSOR IR =================
// Setiap sensor IR dipasang di depan kotak sortir masing-masing
const int IR_FLANGE_PIN  = 32;   // IR depan kotak Flange Bolt
const int IR_HEX_PIN     = 26;   // IR depan kotak Hex Bolt
const int IR_SOCKET_PIN  = 25;   // IR depan kotak Socket Cap
// Carriage Bolt: tidak ada IR khusus, jatuh sendiri ke kotak ujung

// Sensor IR aktif LOW (normalnya HIGH, jadi LOW = ada objek)
// Jika sensor kamu aktif HIGH, ganti LOW → HIGH di bawah
const int IR_TRIGGERED = LOW;

Servo servoFlange;
Servo servoHex;
Servo servoSocket;

// ================= VARIABEL =================
String  currentBoltType = "";
int     targetServo     = -1;   // 1=Flange, 2=Hex, 3=Socket, 0=Carriage
bool    dataReady       = false;

unsigned long tReceive = 0;
unsigned long tSort    = 0;

// Timeout menunggu IR (ms) — jika baut tidak terdeteksi IR dalam waktu ini,
// reset dan tunggu data berikutnya (hindari nunggu selamanya)
const unsigned long IR_TIMEOUT_MS = 10000;

// ================= DEKLARASI FUNGSI =================
void connectWiFi();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void prosesSortir();
void activateServo(Servo &servo, String label);
void resetAllServos();
void finishSorting(String actuator);
void publishLog(unsigned long responseTime, String actuator);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Pin IR sebagai input dengan pull-up internal
  pinMode(IR_FLANGE_PIN,  INPUT_PULLUP);
  pinMode(IR_HEX_PIN,     INPUT_PULLUP);
  pinMode(IR_SOCKET_PIN,  INPUT_PULLUP);

  // Pasang servo ke pin
  servoFlange.attach(SERVO_FLANGE_PIN);
  servoHex.attach(SERVO_HEX_PIN);
  servoSocket.attach(SERVO_SOCKET_PIN);
  resetAllServos();

  connectWiFi();

  // TLS tanpa verifikasi sertifikat CA (cukup untuk pengujian)
  espClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  reconnectMQTT();

  Serial.println("ESP32 siap menerima data sortir.");
  Serial.println("------------------------------");
  Serial.println("Mapping baut → servo → IR:");
  Serial.println("  Flange Bolt  → Servo pin 13 → IR pin 32");
  Serial.println("  Hex Bolt     → Servo pin 12 → IR pin 26");
  Serial.println("  Socket Cap   → Servo pin 14 → IR pin 25");
  Serial.println("  Carriage Bolt→ Tanpa servo  → Jatuh ke ujung");
  Serial.println("------------------------------");
}

// ================= LOOP =================
void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  if (dataReady) {
    prosesSortir();
  }
}

// ================= WIFI =================
void connectWiFi() {
  Serial.print("Menghubungkan WiFi ke: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi terhubung!");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());
}

// ================= MQTT CALLBACK =================
// Dipanggil setiap ada pesan masuk dari topic sensor/result/bolt
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  tReceive = millis();

  Serial.println("\n====== MQTT DITERIMA ======");

  // Parse JSON
  // Payload dari Python:
  // {
  //   "event": "bolt_crossed",
  //   "timestamp": "...",
  //   "bolt_type": "Hex Bolt",    ← field yang dipakai
  //   "confidence": 92.3,
  //   "servo_angle": 120
  // }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Gagal parsing JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Ambil field bolt_type (sesuai yang dikirim Python)
  String boltType = doc["bolt_type"].as<String>();
  float  conf     = doc["confidence"] | 0.0;

  Serial.print("Jenis baut : "); Serial.println(boltType);
  Serial.print("Confidence : "); Serial.print(conf); Serial.println("%");

  // Tentukan servo mana yang akan diaktifkan
  // Nama harus SAMA PERSIS dengan yang dikirim Python
  if (boltType == "Flange Bolt") {
    targetServo = 1;
  }
  else if (boltType == "Hex Bolt") {
    targetServo = 2;
  }
  else if (boltType == "Socket Cap") {
    targetServo = 3;
  }
  else if (boltType == "Carriage Bolt") {
    targetServo = 0;   // Tidak ada servo, jatuh ke kotak ujung
  }
  else {
    Serial.print("Jenis baut tidak dikenal: ");
    Serial.println(boltType);
    return;
  }

  currentBoltType = boltType;
  dataReady       = true;

  Serial.print("Menunggu sensor IR untuk: ");
  Serial.println(currentBoltType);
}

// ================= PROSES SORTIR =================
// Dipanggil dari loop() saat dataReady = true
// Menunggu sensor IR yang sesuai aktif, lalu gerakkan servo
void prosesSortir() {
  unsigned long startWait = millis();

  // --- FLANGE BOLT: tunggu IR_FLANGE aktif ---
  if (targetServo == 1) {
    Serial.println("Menunggu IR Flange (pin 32)...");
    while (digitalRead(IR_FLANGE_PIN) != IR_TRIGGERED) {
      client.loop();  // Tetap proses MQTT selama menunggu
      if (millis() - startWait > IR_TIMEOUT_MS) {
        Serial.println("TIMEOUT: IR Flange tidak terdeteksi, batal.");
        currentBoltType = "";
        targetServo     = -1;
        dataReady       = false;
        return;
      }
    }
    Serial.println("IR Flange terdeteksi!");
    activateServo(servoFlange, "Servo Flange");
    finishSorting("Servo Flange");
  }

  // --- HEX BOLT: tunggu IR_HEX aktif ---
  else if (targetServo == 2) {
    Serial.println("Menunggu IR Hex (pin 26)...");
    while (digitalRead(IR_HEX_PIN) != IR_TRIGGERED) {
      client.loop();
      if (millis() - startWait > IR_TIMEOUT_MS) {
        Serial.println("TIMEOUT: IR Hex tidak terdeteksi, batal.");
        currentBoltType = "";
        targetServo     = -1;
        dataReady       = false;
        return;
      }
    }
    Serial.println("IR Hex terdeteksi!");
    activateServo(servoHex, "Servo Hex");
    finishSorting("Servo Hex");
  }

  // --- SOCKET CAP: tunggu IR_SOCKET aktif ---
  else if (targetServo == 3) {
    Serial.println("Menunggu IR Socket (pin 25)...");
    while (digitalRead(IR_SOCKET_PIN) != IR_TRIGGERED) {
      client.loop();
      if (millis() - startWait > IR_TIMEOUT_MS) {
        Serial.println("TIMEOUT: IR Socket tidak terdeteksi, batal.");
        currentBoltType = "";
        targetServo     = -1;
        dataReady       = false;
        return;
      }
    }
    Serial.println("IR Socket terdeteksi!");
    activateServo(servoSocket, "Servo Socket");
    finishSorting("Servo Socket");
  }

  // --- CARRIAGE BOLT: tidak ada servo, tidak ada IR ---
  // Baut jatuh sendiri ke kotak ujung conveyor
  else if (targetServo == 0) {
    Serial.println("Carriage Bolt → tidak ada servo, baut jatuh ke kotak ujung.");
    delay(500);
    finishSorting("No Servo (Carriage)");
  }
}

// ================= GERAKKAN SERVO =================
// Dorong ke 90° (posisi buka) selama 1 detik, lalu balik ke 0° (tutup)
void activateServo(Servo &servo, String label) {
  Serial.print("Menggerakkan: ");
  Serial.println(label);
  servo.write(90);   // Buka
  delay(1000);
  servo.write(0);    // Tutup kembali
  delay(300);
}

// Reset semua servo ke posisi awal (tutup)
void resetAllServos() {
  servoFlange.write(0);
  servoHex.write(0);
  servoSocket.write(0);
}

// ================= SELESAI SORTIR =================
void finishSorting(String actuator) {
  tSort = millis();
  unsigned long responseTime = tSort - tReceive;

  Serial.println("====== SORTIR SELESAI ======");
  Serial.print("Baut     : "); Serial.println(currentBoltType);
  Serial.print("Aktuator : "); Serial.println(actuator);
  Serial.print("Resp time: "); Serial.print(responseTime); Serial.println(" ms");
  Serial.println("============================");

  publishLog(responseTime, actuator);

  // Reset semua variabel untuk siap menerima data berikutnya
  currentBoltType = "";
  targetServo     = -1;
  dataReady       = false;
}

// ================= PUBLISH LOG KE MQTT =================
void publishLog(unsigned long responseTime, String actuator) {
  StaticJsonDocument<256> logDoc;

  logDoc["protocol"]        = "MQTT";
  logDoc["bolt_type"]       = currentBoltType;
  logDoc["actuator"]        = actuator;
  logDoc["response_time_ms"] = responseTime;
  logDoc["status"]          = "sorted";

  char buffer[256];
  serializeJson(logDoc, buffer);

  if (client.publish(topic_log, buffer)) {
    Serial.println("Log MQTT terkirim.");
  } else {
    Serial.println("Gagal kirim log MQTT.");
  }
}

// ================= RECONNECT MQTT =================
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Menghubungkan ke HiveMQ Cloud... ");
    String clientId = "ESP32_Conveyor_" + String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Berhasil!");
      client.subscribe(topic_detection);
      Serial.print("Subscribe: ");
      Serial.println(topic_detection);
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" → coba lagi 5 detik...");
      delay(5000);
    }
  }
}