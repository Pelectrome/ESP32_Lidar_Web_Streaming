#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <AsyncElegantOTA.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>

#define RX 16
#define TX 17

#define SERIAL_SIZE_RX 10240

WebSocketsServer webSocket = WebSocketsServer(81);
AsyncWebServer server(80);

const char *ssid = "your_wifi_ssid";
const char *password = "your_wifi_password";

void handleWebSocketMessage(uint8_t num, String msg)
{
  // Handle WebSocket messages here
  Serial.printf("WebSocket message from client [%u]: %s\n", num, msg.c_str());
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("Client [%u] disconnected\n", num);
    break;
  case WStype_TEXT:
    handleWebSocketMessage(num, String((char *)payload));
    break;
  default:
    break;
  }
}

void notFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain", "Not found");
}

xQueueHandle broadcastQueue;

#define BUFFER_SIZE 656
#define QUEUE_LENGTH 1
#define ITEM_SIZE sizeof(float[2][BUFFER_SIZE])

const size_t capacity = JSON_OBJECT_SIZE(2000);
DynamicJsonDocument jsonDoc(capacity);
String jsonString;
float dataOutBuffer[2][BUFFER_SIZE];

void broadcastTask(void *pvParameters)
{
  for (;;)
  {
    webSocket.loop();

    if (xQueueReceive(broadcastQueue, &dataOutBuffer, portMAX_DELAY) == pdPASS)
    {
      jsonDoc.clear();
      jsonString = "";
      copyArray(dataOutBuffer, 2, jsonDoc);
      serializeJson(jsonDoc, jsonString);
      webSocket.broadcastTXT(jsonString);
    }
  }
}

void setup()
{
  // Initialize SPIFFS

  Serial1.setRxBufferSize(SERIAL_SIZE_RX);
  Serial1.begin(230400, SERIAL_8N1, RX, TX);
  Serial.begin(230400);
  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.println("\nConnecting...");

  while (WiFi.status() != WL_CONNECTED)
  {
  }

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/sketch.html", String(), false); });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        request->send(200, "text/plain", "ok");
        ESP.restart(); });

  // Set up WebSocket event handlers
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  AsyncElegantOTA.begin(&server, "sohaib", "sohaib1995");

  server.onNotFound(notFound);

  server.begin();

  broadcastQueue = xQueueCreate(QUEUE_LENGTH, ITEM_SIZE);

  // Check if the queue creation was successful
  if (broadcastQueue == NULL)
  {
    Serial.println("broadcastQueue Error.");
  }

  xTaskCreatePinnedToCore(
      broadcastTask,   /* Task function. */
      "broadcastTask", /* name of task. */
      10000,           /* Stack size of task */
      NULL,            /* parameter of the task */
      1,               /* priority of the task */
      NULL,            /* Task handle to keep track of created task */
      1);              /* pin task to core 0 */
}

float dataBuffer[2][BUFFER_SIZE];

void loop()
{

  while (!Serial1.available()){};//wait for new data

  byte StartByte = Serial1.read();
  if (StartByte == 0xAA)
  {
    byte HByteLength = Serial1.read();
    byte LByteLength = Serial1.read();
    uint16_t Length = (uint16_t(HByteLength) << 8) | LByteLength;

    if (Length > 11 && Length < 500)
    {
      byte data[Length + 3];

      data[0] = 0xAA;
      data[1] = HByteLength;
      data[2] = LByteLength;

      uint16_t checkSum = data[0] + data[1] + data[2];

      for (size_t i = 3; i < Length; i++)
      {
        data[i] = Serial1.read();
        checkSum += data[i];
      }

      byte HByteCheckSumResult = Serial1.read();
      byte LByteCheckSumResult = Serial1.read();

      uint16_t checkSumResult = (uint16_t(HByteCheckSumResult) << 8) | LByteCheckSumResult;

      if (checkSum != checkSumResult)
      {
        // Serial.println("checkSum error!");
      }
      else
      {

        byte HBytePayloadLength = data[6];
        byte LBytePayloadLength = data[7];
        uint16_t PayloadLength = (uint16_t(HBytePayloadLength) << 8) | LBytePayloadLength;

        uint16_t sampleCnt = (PayloadLength - 5) / 3;

        float distances[sampleCnt];
        byte signalQualitys[sampleCnt];
        float angles[sampleCnt];

        byte HByteStartAngle = data[11];
        byte LByteStartAngle = data[12];
        uint16_t StartAngle = (uint16_t(HByteStartAngle) << 8) | LByteStartAngle;

        float RPM = (float)data[8] * 0.05; //(r/s)
        float FStartAngle = (float)StartAngle * 0.01;

        byte FrameIndex = FStartAngle / 22.5;

        for (size_t i = 0; i < sampleCnt; i++)
        {
          signalQualitys[i] = data[13 + (3 * i)];                                                  // get signalQualitys
          distances[i] = (float)((uint16_t(data[14 + (3 * i)]) << 8) | data[15 + (3 * i)]) * 0.25; // get distances 2 bytes after the signalQuality byte
          angles[i] = FStartAngle + i * (22.5 / (float)sampleCnt);

          uint16_t index = FrameIndex * sampleCnt + i;//get index of the array
          dataBuffer[0][index] = angles[i]; //load angles data
          dataBuffer[1][index] = distances[i];//load distances data

          // Serial.printf("%d,%.2f,%.2f\n", index, angles[i], distances[i]);
        }
        xQueueSend(broadcastQueue, &dataBuffer, 0);

      }
    }
  }
}
