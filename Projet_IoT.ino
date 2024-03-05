#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <string>
#include <cstdlib>
#include <Adafruit_NeoPixel.h>
#include "Ultrasonic.h"
#include <ESP32Servo.h>
#include <cstring>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

using namespace std;

const int rgbLedId = 5;
const int numPixels = 1;
const int distanceCaptorId = 13;
const int servoPin = 15;
const char* ssid = "MIAOBARK";
const char* password = "123$0L3!L";
const char* mqtt_server = "test.mosquitto.org";
const char* side_mqtt_server = "broker.emqx.io";
const std::string prefix = "upec/W007/";
const std::string barriereTopic = prefix + "communicate/barriere";
const std::string reservationTopic = prefix + "communicate/reservation";
const std::string telemetrie = prefix + "telemetrie";

static size_t timeout = 0;
int reservationId = -1;
int userUsingParkingId = -1;  

enum ParkingState {Free, Occupied, Reserved};
enum ServoState {Opened, Closed};

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(numPixels, rgbLedId, NEO_GRB + NEO_KHZ800);
Ultrasonic distCaptor(distanceCaptorId);
Servo servo;

unsigned long lastMsg = 0;
int value = 0;
size_t servoPos = 0;
bool servoState = Closed;
ParkingState state = Free;

void setupServo() {
  ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);
	servo.setPeriodHertz(10);
	servo.attach(servoPin);
}

void makeServoDoItsThing() {
    servo.write(servoPos);
    delay(200);
}

void dealWithReservations(byte* payload, unsigned int length) {
  char buffer[length];
  for (size_t i = 0 ; i < length; ++i) buffer[i] = (char)payload[i];
  long int userId = strtol(buffer, nullptr, 10);

  switch(state) {
    case Free :
      reservationId = userId; 
      state = Reserved;
      break;
    case Occupied :
      break;
    case Reserved :
      break;
  }

  matchLedColorToParkingState();
}

void openServo() {
  if (servoState == Closed) {
    servoPos = 90;
    makeServoDoItsThing();
    servoState = Opened;
  }
}

void closeServo() {
  if (servoState == Opened) {
    servoPos = 0;
    makeServoDoItsThing();
    servoState = Closed;
  }
}

void setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
    pixels.clear();
    pixels.setPixelColor(0, pixels.Color(red, green, blue));
    pixels.show();
}

void setLedRed() {
  setLedColor(255, 0, 0);
}
void setLedOrange() {
  setLedColor(255,69,0);
}
void setLedGreen() {
  setLedColor(0,255,0);
}

void matchLedColorToParkingState() {
  switch (state) {
    case Free : 
      setLedGreen();
      // openServo();
      break;
    case Occupied :
      setLedRed();
      // closeServo();
      break;
    case Reserved : 
      setLedOrange();
      // closeServo();
      break;
  }
}

void processDistanceCaptorData() {
  long distInCms;
  distInCms = distCaptor.MeasureInCentimeters();

  Serial.print("Object at : ");
  Serial.print(distInCms);
  Serial.println("cm");

  switch (state) {
    case Free : 
      if (distInCms < 10) state = Occupied;
      break; 
    case Occupied : 
      if (distInCms >= 10) state = Free; 
      break;
    case Reserved : 
    default :
      break;
  }

  matchLedColorToParkingState();
  delay(800);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  randomSeed(micros());
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      const char* userId = pCharacteristic->getValue().c_str();
      auto userIdLongInt = strtol(userId, nullptr, 10);
        long distInCms;
        distInCms = distCaptor.MeasureInCentimeters();
      switch (state) {
        case Free : 
            if (distInCms < 10) {
              userUsingParkingId = userIdLongInt;
              servoState = Closed;
              closeServo();
              state = Occupied; 
            }
          break;
        case Occupied : 
            if (distInCms < 10 && userUsingParkingId == userIdLongInt) {
              servoState = Opened;
              state = Free;
              openServo();
            }
          break;
        case Reserved : 
            if (reservationId == userIdLongInt) {
              servoState = Opened; 
              openServo();
              delay(2000);
              if (distInCms < 10) {
                state = Occupied;
                servoState = Closed;
                closeServo();
              }
              state = Occupied;
            }
          break;
      }
      matchLedColorToParkingState();
    }
};

void bleSetup() {
  BLEDevice::init("MyESP32");
  BLEServer *pServer = BLEDevice::createServer();

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID,
                                         BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE
                                       );

  pCharacteristic->setCallbacks(new MyCallbacks());

  pCharacteristic->setValue("Active waiting for BLE signal");
  pService->start();

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->start();
}

void processPayload(char payload) {
  if (payload == '0') state = Free;  
  else if (payload == '1') state = Occupied;
  else state = Reserved;
  matchLedColorToParkingState();
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  if (strcmp(topic, barriereTopic.c_str()) == 0) {
    for (int i = 0; i < length; i++) {
      Serial.print((char)payload[i]);
    }
    Serial.println();
    processPayload((char)payload[0]);
  }
  else {
    dealWithReservations(payload, length);
  }
}

void reconnect() {
  timeout++;
  while (!client.connected()) {
    if (timeout >= 10) {
      client.setServer(side_mqtt_server, 1883);
      Serial.println("Switching to emqx server");
    }
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      client.publish("outTopic", telemetrie.c_str());
      client.subscribe(barriereTopic.c_str());
      client.subscribe(reservationTopic.c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pixels.begin();
  processDistanceCaptorData();
  setupServo();
  setup_wifi();
  bleSetup();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void loop() {
  //Delay de au moins 500 ms pour le 
  processDistanceCaptorData();
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  unsigned long now = millis();
  if (now - lastMsg > 2000) {
    lastMsg = now;
    ++value;
    client.publish(telemetrie.c_str(), state == ParkingState::Free ? "0" : state == ParkingState::Occupied ? "1" : "2");
  }
}