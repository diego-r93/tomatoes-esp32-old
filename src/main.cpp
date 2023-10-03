#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiUDP.h>

#include <set>

#include "NTPClient.h"
#include "SPIFFS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hydraulicPumpController.h"
#include "mongoDbAtlas.h"
#include "wifiCredentials.h"

/*
Task                Core  Prio     Descrição
----------------------------------------------------------------------------------------------------
vTaskTurnOnPump      1     2     Liga a bomba quando chegar no seu horário de acionamento
vTaskNTP             0     1     Atualiza o horário com base no NTP
vTaskUpdate          0     3     Atualiza as informações através de um POST no MongoDB Atlas
vTaskCheckWiFi       0     2     Verifica a conexão WiFi e tenta reconectar caso esteja deconectado

*/

// Configurações iniciais
#define LED_BUILTIN 25
#define NUMBER_OUTPUTS 4
#define ACTIVE_PUMPS 2

// Delay das tasks
#define CHECK_WIFI_DELAY 100
#define NTP_DELAY 600000
#define UPDATE_DELAY 300000
#define TURN_ON_PUMP_DELAY 100

const uint8_t outputGPIOs[NUMBER_OUTPUTS] = {21, 19, 18, 5};

// Tomatoes
// HydraulicPumpController myPumps[ACTIVE_PUMPS] = {
//     HydraulicPumpController("#01", outputGPIOs[1], 60000),
//     HydraulicPumpController("#02", outputGPIOs[2], 900000),
// };

// Hydroponic
HydraulicPumpController myPumps[ACTIVE_PUMPS] = {
    HydraulicPumpController("#03", outputGPIOs[1], 900000),
    HydraulicPumpController("#04", outputGPIOs[2], 900000),
};

// Variáveis para armazenamento do handle das tasks e mutexes
SemaphoreHandle_t xWifiMutex;

TaskHandle_t handleTurnOnPump = NULL;
TaskHandle_t handleNTP = NULL;
TaskHandle_t handleUpdate = NULL;
TaskHandle_t handleCheckWiFi = NULL;

// Protótipos das Tasks
void vTaskTurnOnPump(void *pvParametes);
void vTaskNTP(void *pvParameters);
void vTaskUpdate(void *pvParameters);
void vTaskCheckWiFi(void *pvParametes);

// Configurações do NTP
WiFiUDP udp;
NTPClient ntp(udp, "a.st1.ntp.br", -3 * 3600, 3600000);

// Configurações do WebServer
String formatedTime;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char *getFormattedTime() {
   formatedTime = String(ntp.getFormattedTime());

   char *ch = new char[formatedTime.length() + 1];
   strcpy(ch, formatedTime.c_str());

   return ch;
}

const char *getRSSI() {
   String rssi = String(WiFi.RSSI()).c_str();

   char *ch = new char[rssi.length() + 1];
   strcpy(ch, rssi.c_str());

   return ch;
}

const char *getHostname() {
   String hostname = WiFi.getHostname();

   char *ch = new char[hostname.length() + 1];
   strcpy(ch, hostname.c_str());

   return ch;
}

const char *getPulseDuration(HydraulicPumpController pump) {
   String pulseDuration = String(pump.getPulseDuration());

   char *ch = new char[pulseDuration.length() + 1];
   strcpy(ch, pulseDuration.c_str());

   return ch;
}

String getID() {
   String id = "";
   id = String((uint32_t)ESP.getEfuseMac(), HEX);
   id.toUpperCase();
   return id;
}

String sendTimers(std::set<String> pumpTimers) {
   String output;

   for (auto index : pumpTimers) {
      output += index;
      output += ',';
   }

   return output;
}

String getOutputStates() {
   DynamicJsonDocument myArray(512);

   for (int i = 0; i < NUMBER_OUTPUTS; i++) {
      myArray["gpios"][i]["output"] = String(outputGPIOs[i]);
      myArray["gpios"][i]["state"] = String(digitalRead(outputGPIOs[i]));
   }
   String jsonString;
   serializeJson(myArray, jsonString);

   return jsonString;
}

void notifyClients(String state) {
   ws.textAll(state);
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
   AwsFrameInfo *info = (AwsFrameInfo *)arg;
   if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      if (strcmp((char *)data, "states") == 0) {
         notifyClients(getOutputStates());
      } else {
         uint8_t gpio = (uint8_t)atoi((char *)data);

         for (int indice; indice < ACTIVE_PUMPS; indice++)
            if (gpio == myPumps[indice].gpioPin)
               myPumps[indice].getPumpState() ? myPumps[indice].stopPump() : myPumps[indice].startPump();

         notifyClients(getOutputStates());
      }
   }
}

void restart() {
   yield();
   delay(1000);
   yield();
   ESP.restart();
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
   switch (type) {
      case WS_EVT_CONNECT:
         Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
         break;
      case WS_EVT_DISCONNECT:
         Serial.printf("WebSocket client #%u disconnected\n", client->id());
         break;
      case WS_EVT_DATA:
         handleWebSocketMessage(arg, data, len);
         break;
      case WS_EVT_PONG:
      case WS_EVT_ERROR:
         break;
   }
}

void loadConfigurationCloud(const char *pumperCode, DynamicJsonDocument *jsonData) {
   DynamicJsonDocument response(MAX_SIZE_DOCUMENT);
   DynamicJsonDocument body(512);  // Pode ser pequeno pois é o que será enviado

   const size_t CAPACITY = JSON_OBJECT_SIZE(1);
   StaticJsonDocument<CAPACITY> doc;

   JsonObject object = doc.to<JsonObject>();
   object["pumperCode"] = pumperCode;

   body["dataSource"] = "Tomatoes";
   body["database"] = "first-api";
   body["collection"] = "sensors";
   body["filter"] = object;

   // Serialize JSON document
   String json;
   serializeJson(body, json);

   WiFiClientSecure client;

   client.setCACert(root_ca);

   HTTPClient http;

   http.begin(client, serverName);
   http.addHeader("api-key", apiKey);
   http.addHeader("Content-Type", "application/json");
   http.addHeader("Accept", "application/json");

   int httpResponseCode = http.POST(json);

   Serial.print("HTTP Response code: ");
   Serial.println(httpResponseCode);

   String payload = http.getString();

   // Disconnect
   http.end();

   DeserializationError error = deserializeJson(response, payload);

   if (error)
      Serial.println("Failed to read document, using default configuration");

   serializeJsonPretty(response["document"], Serial);

   *jsonData = response["document"];
}

void updateConfiguration(DynamicJsonDocument inputDocument, std::set<String> *inputDriveTime, uint32_t *inputDuration) {
   JsonArray tempArray = inputDocument["driveTimes"].as<JsonArray>();

   inputDriveTime->clear();
   for (JsonVariant index : tempArray) {
      JsonObject object = index.as<JsonObject>();
      const char *tempTime = object["time"];
      bool tempState = object["state"];

      if (tempState)
         inputDriveTime->insert(tempTime);
   }

   *inputDuration = inputDocument["pulseDuration"].as<uint32_t>();
}

void initSPIFFS() {
   if (!SPIFFS.begin(true)) {
      Serial.println("An error has occurred while mounting SPIFFS");
   }
   Serial.println("SPIFFS mounted successfully");
}

void initWiFi() {
   WiFi.mode(WIFI_STA);
   WiFi.begin(ssid, password);
   Serial.print("Connecting to WiFi ..");
   while (WiFi.status() != WL_CONNECTED) {
      Serial.print('.');
      delay(1000);
   }
   Serial.println();
   Serial.print("MAC Address:  ");
   Serial.println(WiFi.macAddress());
   Serial.print("Local IP:  ");
   Serial.println(WiFi.localIP());
   Serial.print("Hostname:  ");
   Serial.println(WiFi.getHostname());

   if (!MDNS.begin(WiFi.getHostname())) {
      Serial.println("Error setting up MDNS responder!");
      while (1) {
         delay(1000);
      }
   }
   Serial.println("mDNS responder started");
   Serial.print("mDNS Adress:  ");
   Serial.println(WiFi.getHostname());
}

void initNTP() {
   ntp.begin();
   ntp.forceUpdate();
}

void initWebSocket() {
   ws.onEvent(onEvent);
   server.addHandler(&ws);
}

void initConfiguration() {
   for (int indice = 0; indice < ACTIVE_PUMPS; indice++) {
      loadConfigurationCloud(myPumps[indice].pumperCode, myPumps[indice].getJsonDataPointer());
      updateConfiguration(myPumps[indice].getJsonData(), myPumps[indice].getDriveTimesPointer(), myPumps[indice].pulseDurationPointer);
   }
}

void initRtos() {
   xWifiMutex = xSemaphoreCreateMutex();

   xTaskCreatePinnedToCore(vTaskCheckWiFi, "taskCheckWiFi", configMINIMAL_STACK_SIZE, NULL, 2, &handleCheckWiFi, PRO_CPU_NUM);
   xTaskCreatePinnedToCore(vTaskNTP, "taskNTP", configMINIMAL_STACK_SIZE + 2048, NULL, 1, &handleNTP, PRO_CPU_NUM);

   for (int indice = 0; indice < ACTIVE_PUMPS; indice++) {
      xTaskCreatePinnedToCore(vTaskUpdate, "taskUpdate", configMINIMAL_STACK_SIZE + 8192, &myPumps[indice], 3, &handleUpdate, PRO_CPU_NUM);
      xTaskCreatePinnedToCore(vTaskTurnOnPump, "taskTurnOnPump", configMINIMAL_STACK_SIZE + 1024, &myPumps[indice], 2, &handleTurnOnPump, APP_CPU_NUM);
   }
}

void initServer() {
   server.serveStatic("/", SPIFFS, "/");

   server.onNotFound([](AsyncWebServerRequest *request) {
      if (request->method() == HTTP_GET) {
         if (request->url().startsWith("/js/") || request->url().startsWith("/css/")) {
            // Lidar com solicitações para arquivos estáticos (CSS e JavaScript)
            request->send(SPIFFS, request->url(), String());
         } else {
            // Redirecionar todas as outras solicitações para o index.html
            request->send(SPIFFS, "/index.html", "text/html");
         }
      } else {
         request->send(405);  // Método não permitido
      }
   });

   server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(SPIFFS, "/index.html", "text/html", false);
   });

   server.on("/timers1", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send(200, "text/plain", sendTimers(myPumps[0].getDriveTimes()));
   });

   server.on("/pulse1", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send(200, "text/plain", getPulseDuration(myPumps[0]));
   });

   server.on("/timers2", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send(200, "text/plain", sendTimers(myPumps[1].getDriveTimes()));
   });

   server.on("/pulse2", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send(200, "text/plain", getPulseDuration(myPumps[1]));
   });

   server.on("/time", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send_P(200, "text/plain", getFormattedTime());
   });

   server.on("/rssi", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send_P(200, "text/plain", getRSSI());
   });

   server.on("/hostname", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send_P(200, "text/plain", getHostname());
   });

   server.on("/update/identity", HTTP_GET, [](AsyncWebServerRequest *request) {
      // verificar se a solicitação não vem do AJAX
      if (!request->hasHeader("X-Requested-With") || request->header("X-Requested-With") != "XMLHttpRequest") {
         request->send(403);  // retornar um erro 403 (Proibido)
      } else
         request->send(200, "application/json", "{\"id\": \"" + getID() + "\", \"hardware\": \"ESP32\"}");
   });

   server.on(
       "/ota-update", HTTP_POST, [](AsyncWebServerRequest *request) {
          AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 500 : 200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
          response->addHeader("Connection", "close");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          restart(); },
       [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
          if (!index) {
             int cmd = (filename == "firmware.bin") ? U_FLASH : U_SPIFFS;

             if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
                Update.printError(Serial);
                return request->send(400, "text/plain", "OTA could not begin");
             }
          }
          if (len) {
             if (Update.write(data, len) != len) {
                return request->send(400, "text/plain", "OTA could not begin");
             }
          }
          if (final) {
             if (!Update.end(true)) {
                Update.printError(Serial);
                return request->send(400, "text/plain", "Could not end OTA");
             }
          } else {
             return;
          }
       });

   // DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

   server.begin();
}

void setup() {
   Serial.begin(115200);

   initSPIFFS();
   initWiFi();
   initNTP();
   initWebSocket();
   initConfiguration();
   initRtos();
   initServer();
}

void loop() {
   vTaskDelete(NULL);
}

void vTaskCheckWiFi(void *pvParameters) {
   while (1) {
      if (WiFi.status() != WL_CONNECTED) {
         Serial.println("Reconnecting to WiFi...");
         WiFi.disconnect();
         WiFi.reconnect();
      }

      vTaskDelay(pdMS_TO_TICKS(CHECK_WIFI_DELAY));
   }
}

void vTaskNTP(void *pvParameters) {
   while (1) {
      if (xSemaphoreTake(xWifiMutex, portMAX_DELAY)) {
         ntp.update();
         xSemaphoreGive(xWifiMutex);
      }

      vTaskDelay(pdMS_TO_TICKS(NTP_DELAY));
   }
}

void vTaskUpdate(void *pvParameters) {
   HydraulicPumpController *pump = (HydraulicPumpController *)pvParameters;

   while (1) {
      if (xSemaphoreTake(xWifiMutex, portMAX_DELAY)) {
         loadConfigurationCloud(pump->pumperCode, pump->getJsonDataPointer());
         updateConfiguration(pump->getJsonData(), pump->getDriveTimesPointer(), pump->pulseDurationPointer);
         xSemaphoreGive(xWifiMutex);
      }

      vTaskDelay(pdMS_TO_TICKS(UPDATE_DELAY));
   }
}

void vTaskTurnOnPump(void *pvParameters) {
   HydraulicPumpController *pump = (HydraulicPumpController *)pvParameters;

   while (1) {
      String formatedTime = ntp.getFormattedTime();

      for (String driveTime : pump->getDriveTimes()) {
         if (formatedTime == driveTime)
            pump->startPump();
      }

      vTaskDelay(pdMS_TO_TICKS(TURN_ON_PUMP_DELAY));
   }
}
