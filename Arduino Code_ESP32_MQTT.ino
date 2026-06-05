#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ================= WIFI =================
const char* ssid = "awas kontrakan";
const char* password = "123horee";

// ================= HIVEMQ CLOUD =================
const char* mqtt_server = "d9f9a8170ace4b888983e922fe4c25a0.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "skripsi";
const char* mqtt_pass = "Skripsi123";

const char* topic_detection = "sensor/result/bolt";
const char* topic_log = "sensor/log/bolt";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ================= PIN =================
const int SERVO_A_PIN = 13;   // Flange
const int SERVO_B_PIN = 12;   // Hexagonal
const int SERVO_C_PIN = 14;   // Socket Cap

const int IR_1_PIN = 32;
const int IR_2_PIN = 26;
const int IR_3_PIN = 25;

// Kalau sensor IR kamu aktif HIGH, ganti LOW menjadi HIGH
const int IR_DETECTED = LOW;

Servo servoA;
Servo servoB;
Servo servoC;

// ================= VARIABEL =================
String currentType = "";
int targetServo = -1;
bool dataReady = false;

unsigned long tReceive = 0;
unsigned long tSort = 0;

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(IR_1_PIN, INPUT_PULLUP);
  pinMode(IR_2_PIN, INPUT_PULLUP);
  pinMode(IR_3_PIN, INPUT_PULLUP);

  servoA.attach(SERVO_A_PIN);
  servoB.attach(SERVO_B_PIN);
  servoC.attach(SERVO_C_PIN);

  resetServo();

  connectWiFi();

  espClient.setInsecure(); // untuk tahap awal TLS tanpa sertifikat CA

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  reconnectMQTT();

  Serial.println("ESP32 siap.");
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
  Serial.print("Menghubungkan WiFi");

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
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  tReceive = millis();

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.println("Gagal parsing JSON MQTT");
    return;
  }

  String type = doc["type"];

  currentType = type;

  if (type == "Flange") {
    targetServo = 1;
  } 
  else if (type == "Hexagonal") {
    targetServo = 2;
  } 
  else if (type == "Socket Cap") {
    targetServo = 3;
  } 
  else if (type == "Carriage") {
    targetServo = 0;
  } 
  else {
    Serial.print("Jenis baut tidak dikenal: ");
    Serial.println(type);
    return;
  }

  dataReady = true;

  Serial.println("==============================");
  Serial.print("Data diterima: ");
  Serial.println(currentType);
  Serial.print("Target servo: ");
  Serial.println(targetServo);
}

// ================= PROSES SORTIR =================
void prosesSortir() {
  if (targetServo == 1 && digitalRead(IR_1_PIN) == IR_DETECTED) {
    Serial.println("IR1 aktif → Servo A");
    activateServo(servoA);
    finishSorting("Servo A");
  }

  else if (targetServo == 2 && digitalRead(IR_2_PIN) == IR_DETECTED) {
    Serial.println("IR2 aktif → Servo B");
    activateServo(servoB);
    finishSorting("Servo B");
  }

  else if (targetServo == 3 && digitalRead(IR_3_PIN) == IR_DETECTED) {
    Serial.println("IR3 aktif → Servo C");
    activateServo(servoC);
    finishSorting("Servo C");
  }

  else if (targetServo == 0 && digitalRead(IR_3_PIN) == IR_DETECTED) {
    Serial.println("Carriage → tanpa servo → jatuh ke box ujung");
    delay(800);
    finishSorting("No Servo");
  }
}

// ================= SERVO =================
void activateServo(Servo &servo) {
  servo.write(90);
  delay(1000);
  servo.write(0);
  delay(300);
}

void resetServo() {
  servoA.write(0);
  servoB.write(0);
  servoC.write(0);
}

// ================= SELESAI SORTIR =================
void finishSorting(String actuator) {
  tSort = millis();

  unsigned long responseTime = tSort - tReceive;

  Serial.print("Selesai sortir: ");
  Serial.println(currentType);

  Serial.print("Response time: ");
  Serial.print(responseTime);
  Serial.println(" ms");

  publishLog(responseTime, actuator);

  currentType = "";
  targetServo = -1;
  dataReady = false;
}

// ================= PUBLISH LOG =================
void publishLog(unsigned long responseTime, String actuator) {
  StaticJsonDocument<256> log;

  log["protocol"] = "MQTT";
  log["type"] = currentType;
  log["actuator"] = actuator;
  log["response_time_ms"] = responseTime;
  log["status"] = "sorted";

  char buffer[256];
  serializeJson(log, buffer);

  client.publish(topic_log, buffer);
}

// ================= RECONNECT MQTT =================
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Menghubungkan ke HiveMQ Cloud... ");

    String clientId = "ESP32_Conveyor_" + String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("berhasil!");
      client.subscribe(topic_detection);
      Serial.print("Subscribe topic: ");
      Serial.println(topic_detection);
    } else {
      Serial.print("gagal, rc=");
      Serial.println(client.state());
      delay(5000);
    }
  }
}
