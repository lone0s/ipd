#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
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
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

using namespace std;

const int rgbLedId = 5;
const int numPixels = 1;
const int distanceCaptorId = 13;
const int servoPin = 15;
const char* apSSID = "Vrai Wifi sage et mignon :-)";
const char* apPassword = "123Soleil";

String ssid;
String password;

const char* mqtt_server = "test.mosquitto.org";
const char* side_mqtt_server = "broker.emqx.io";
const std::string prefix = "upec/W007/";
const std::string barriereTopic = prefix + "communicate/barriere";
const std::string reservationTopic = prefix + "communicate/reservation";
const std::string preferencesTopic = prefix + "communicate/clearCredentials";
const std::string dataUploadRateTopic = prefix + "communicate/uploadRate";
const std::string ledColorTopic = prefix + "communicate/LEDColor";
const std::string telemetrieParking = prefix + "telemetrie/parking";
const std::string telemetrieLed = prefix + "telemetrie/led";
const std::string telemetrieDistance = prefix + "telemetrie/distance";

enum ParkingState {Free, Occupied, Reserved};
enum ServoState {Opened, Closed};
enum LEDState {Green, Red, Orange};

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(numPixels, rgbLedId, NEO_GRB + NEO_KHZ800);
Ultrasonic distCaptor(distanceCaptorId);
Servo servo;
Preferences preferences;

static size_t timeout = 0;
unsigned long uploadRate = 1000; 
int reservationId = -1;
int userUsingParkingId = -1;  
unsigned long lastMsg = 0;
int value = 0;
size_t servoPos = 0;
bool servoState = Closed;
ParkingState state = Free;
LEDState ledState;
bool wifiDataExists = false;
WebServer server(80);

/////////////////////////////////////////////////////////////////////////////////////////
                            // OTA ASYNC WEBSERVER //
/////////////////////////////////////////////////////////////////////////////////////////

const char* serverIndex =
"<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
"<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
   "<input type='file' name='update'>"
        "<input type='submit' value='Update'>"
    "</form>"
 "<div id='prg'>progress: 0%</div>"
 "<script>"
  "$('form').submit(function(e){"
  "e.preventDefault();"
  "var form = $('#upload_form')[0];"
  "var data = new FormData(form);"
  " $.ajax({"
  "url: '/update',"
  "type: 'POST',"
  "data: data,"
  "contentType: false,"
  "processData:false,"
  "xhr: function() {"
  "var xhr = new window.XMLHttpRequest();"
  "xhr.upload.addEventListener('progress', function(evt) {"
  "if (evt.lengthComputable) {"
  "var per = evt.loaded / evt.total;"
  "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
  "}"
  "}, false);"
  "return xhr;"
  "},"
  "success:function(d, s) {"
  "console.log('success!')"
 "},"
 "error: function (a, b, c) {"
 "}"
 "});"
 "});"
 "</script>";

const char* host = "esp32OTAServ";

void setupOTA() {
  Serial.print("OTA Web serv IP address: ");
  Serial.println(WiFi.localIP());

  /*use mdns for host name resolution*/
  if (!MDNS.begin(host)) { //http://esp32.local
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");
  /*return index page which is stored in serverIndex */
  server.on("/serverIndex", HTTP_GET, []() {
    server.send(200, "text/html", serverIndex);
  });
  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
}

void loopOTA() {
  server.handleClient();
  delay(1);
}

/////////////////////////////////////////////////////////////////////////////////////////
                                  // PREFERENCES //
/////////////////////////////////////////////////////////////////////////////////////////

void clearPreferences() {
  preferences.clear();
  preferences.end();
  ESP.restart();
}

void wifiCredsAlreadySaved() {
  wifiDataExists = preferences.getString("ssid", "-1") != "-1";
}

void extractWifiCreds() {
  ssid = preferences.getString("ssid", "-1");
  password = preferences.getString("password", "-1");
}

/////////////////////////////////////////////////////////////////////////////////////////
                              // AP SERVER FOR CONFIG //
/////////////////////////////////////////////////////////////////////////////////////////

void handleRoot() {
  server.send(200, "text/html",
              "<form action='/save' method='post'>"
              "SSID: <input type='text' name='ssid'><br>"
              "Password: <input type='password' name='password'><br>"
              "<input type='submit' value='Save'>"
              "</form>");
}

void handleSave() {
  ssid = server.arg("ssid");
  password = server.arg("password");
  
  // Save SSID and password into preferences
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  
  server.send(200, "text/html", "Credentials saved. Rebooting...");
  delay(2000);
  server.stop();
  ESP.restart();
}

void setupWiFiServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

/////////////////////////////////////////////////////////////////////////////////////////
                                  // SERVO //
/////////////////////////////////////////////////////////////////////////////////////////

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

// Pour mimer le comportement d'une place de parking ou y a ouverture puis fermeture 
void servoRoutine() {
  //On ouvre la barrière
  openServo();
  //On laisse du temps pour qu'un utilisateur puisse venir sur la place
  delay(3000);
  //On vérifie si y a quelqu'un ou pas et on met à jour adéquatement l'état du parking
  processDistanceCaptorData();
  //On ferme la barrière
  closeServo();
}

void dealWithBarriere(byte* payload, unsigned int length) {
  for (int i = 0; i < length; i++) Serial.print((char)payload[i]);

  Serial.println();

  char pl = (char)payload[0];

  if (state != Reserved) {
    if (pl == '0' && servoState == Closed) openServo();
    else if (pl == '1' && servoState == Opened) closeServo();
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
                                // RESERVATIONS //
/////////////////////////////////////////////////////////////////////////////////////////

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

/////////////////////////////////////////////////////////////////////////////////////////
                                // DATA UPLOAD RATE //
/////////////////////////////////////////////////////////////////////////////////////////

void dealWithUploadRate(byte* payload, unsigned int length) {
  char buffer[length];
  for (size_t i = 0 ; i < length; ++i) buffer[i] = (char)payload[i];
  unsigned long userUploadRate = strtol(buffer, nullptr, 10) * 1000;

  Serial.print("Old upload rate: ");
  Serial.println(uploadRate);
  uploadRate = userUploadRate;
  Serial.print("New upload rate: ");
  Serial.print(uploadRate);
}

/////////////////////////////////////////////////////////////////////////////////////////
                                      // LED //
/////////////////////////////////////////////////////////////////////////////////////////

void dealWithLEDColor(byte* payload, unsigned int length) {
  char buffer[length];
  for (size_t i = 0 ; i < length; ++i) {buffer[i] = (char)payload[i];};
  unsigned long userLEDColor = strtol(buffer, nullptr, 10);

  Serial.print("Processed input for LED Color : ");
  Serial.println(userLEDColor);

  switch (userLEDColor) {
    case 0 :
      setLedGreen();
      Serial.println("Switched color to green");
      break;
    case 1 : 
      setLedRed();
      Serial.println("Switched color to red");
      break;
    case 2 : 
      setLedOrange();
      Serial.println("Switched color to orange");
      break;
  }

  //Histoire de pouvoir voir le switch de couleur : le parking sera toujours dominant sur la demande de couleur 
  // delay(2000);
}


void setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
    pixels.clear();
    pixels.setPixelColor(0, pixels.Color(red, green, blue));
    pixels.show();
}

void setLedRed() {
  setLedColor(255, 0, 0);
  ledState = Red;
}
void setLedOrange() {
  setLedColor(255,69,0);
  ledState = Orange;
}
void setLedGreen() {
  setLedColor(0,255,0);
  ledState = Green;
}

void matchLedColorToParkingState() {
  switch (state) {
    case Free : 
      setLedGreen();
      break;
    case Occupied :
      setLedRed();
      break;
    case Reserved : 
      setLedOrange();
      break;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
                                // DISTANCE CAPTOR //
/////////////////////////////////////////////////////////////////////////////////////////

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

  // matchLedColorToParkingState();
  delay(1000);
}

/////////////////////////////////////////////////////////////////////////////////////////
                                // WIFI CONNECTION //
/////////////////////////////////////////////////////////////////////////////////////////

void setupWifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
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

/////////////////////////////////////////////////////////////////////////////////////////
                                    // BLE //
/////////////////////////////////////////////////////////////////////////////////////////

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      const char* userId = pCharacteristic->getValue().c_str();
      
      Serial.print("Received new message :");
      Serial.println(userId);
      Serial.print("Avant : ");
      Serial.println(state == Free ? "LIBRE" : state == Occupied ? "OCCUPE" : "RESERVE");

      auto userIdLongInt = strtol(userId, nullptr, 10);
      
      switch (state) {
        case Free : 
          servoRoutine();
          //TODO: Explain this 
          if (state == Occupied) userUsingParkingId = userIdLongInt;
      
        case Occupied : 
        Serial.println("User using parking id | user sent id");
        Serial.print(userUsingParkingId);
        Serial.print(" | ");
        Serial.println(userId);
      
            if (userUsingParkingId == userIdLongInt) {
              servoRoutine();
              //TODO: Explain this 
              if (state == Free) userUsingParkingId = -1;
            }
          break;
        
        case Reserved : 
        Serial.println("User reserved parking id | user sent reservation id");
        Serial.print(reservationId);
        Serial.print(" | ");
        Serial.println(userIdLongInt);

            if (reservationId == userIdLongInt) {
              state = Occupied;
              servoRoutine();
              //TODO: Explain this 
              if (state == Free) {userUsingParkingId = -1; reservationId = -1;}
            }
          break;
      }

      Serial.print("Apres : ");
      Serial.println(state == Free ? "LIBRE" : state == Occupied ? "OCCUPE" : "RESERVE");
      pCharacteristic->setValue(state == Free ? "LIBRE" : state == Occupied ? "OCCUPE" : "RESERVE");
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

/////////////////////////////////////////////////////////////////////////////////////////
                                    // MQTT //
/////////////////////////////////////////////////////////////////////////////////////////

bool equals(const char* first_str, const char* second_str) {
  return strcmp(first_str, second_str) == 0;
}

void processPayload(char payload) {
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");

  if (equals(topic, barriereTopic.c_str())) dealWithBarriere(payload,length);

  else if (equals(topic, reservationTopic.c_str())) dealWithReservations(payload, length);

  else if (equals(topic, preferencesTopic.c_str())) clearPreferences();

  else if (equals(topic,dataUploadRateTopic.c_str())) dealWithUploadRate(payload,length);

  else dealWithLEDColor(payload,length);
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

      client.publish("outTopic", telemetrieParking.c_str());
      client.publish("outTopic", telemetrieLed.c_str());
      client.publish("outTopic", telemetrieDistance.c_str());

      client.subscribe(barriereTopic.c_str());
      client.subscribe(reservationTopic.c_str());
      client.subscribe(preferencesTopic.c_str());
      client.subscribe(dataUploadRateTopic.c_str());
      client.subscribe(ledColorTopic.c_str());
    } 
    else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
                                // CORE ESP32 FUNCS //
/////////////////////////////////////////////////////////////////////////////////////////

void setup() {
  Serial.begin(115200);
  preferences.begin("wifi", false);
  wifiCredsAlreadySaved();
  WiFi.mode(WIFI_STA);

  // On lance la config par wifi si rien de sauvegardé dans Preferences
  if (!wifiDataExists){
  Serial.println("\n[*] Creating AP");
  WiFi.softAP(apSSID, apPassword);
  Serial.print("[+] AP Created with IP Gateway ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Let's setup this wifi up :)");
  setupWiFiServer();
  }

  else{
  extractWifiCreds();
  pixels.begin();
  processDistanceCaptorData();
  matchLedColorToParkingState();
  setupServo();
  setupWifi();
  setupOTA();
  bleSetup();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  }
}

void loop() {
  if (!wifiDataExists) server.handleClient();
  else {
    if (!client.connected()) reconnect();
    
    client.loop();

    unsigned long now = millis();
    if (now - lastMsg > uploadRate) {
      lastMsg = now;
      ++value;
      client.publish(telemetrieParking.c_str(), state == ParkingState::Free ? "0" : state == ParkingState::Occupied ? "1" : "2");
      client.publish(telemetrieLed.c_str(), ledState == Green ? "0" : ledState == Red ? "1" : "2");
      client.publish(telemetrieDistance.c_str(), std::to_string(distCaptor.MeasureInCentimeters()).c_str());
    }

    processDistanceCaptorData();
    matchLedColorToParkingState();

    loopOTA();
  }
}