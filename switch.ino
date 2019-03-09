#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "PubSubClient.h"

//--------- Configuration
// WiFi
const char *ssid = "<your-wifi-ssid>";
const char *password = "<your-wifi-key>";

const char *mqttServer = "<mqtt-broker-ip-or-host>";
const char *mqttUser = "<mqtt-user>";
const char *mqttPass = "<mqtt-password>";
const char *mqttClientName = "<mqtt-client-id>"; //will also be used hostname and OTA name
const char *mqttTopicPrefix = "<mqtt-topic-prefix>";

// I/O
const int btnPin = 14; //IO14 on WiFi Relay

// internal vars
WiFiClient espClient;
PubSubClient client(espClient);

char mqttTopicState[64];
char mqttTopicStatus[64];
char mqttTopicDo[64];
char mqttTopicIp[64];

long lastReconnectAttempt = 0; //For the non blocking mqtt reconnect (in millis)
long lastDebounceTime = 0;     // Holds the last time debounce was evaluated (in millis).
const int debounceDelay = 80;  // The delay threshold for debounce checking.

int onoff = false;       //is relay on or off
int wantedState = false; //wanted state
int debounceState;       //internal state for debouncing

void setup()
{
  // Configure the pin mode as an input.
  pinMode(btnPin, INPUT_PULLUP);

  //output pins for L9110
  pinMode(12, OUTPUT);
  pinMode(13, OUTPUT);

  //switch off relay
  digitalWrite(12, HIGH);
  digitalWrite(13, LOW);

  Serial.begin(115200);

  // Attach an interrupt to the pin, assign the onChange function as a handler and trigger on changes (LOW or HIGH).
  attachInterrupt(btnPin, onChangeButton, CHANGE);

  //put in mqtt prefix
  sprintf(mqttTopicState, "%sstate", mqttTopicPrefix);
  sprintf(mqttTopicStatus, "%sstatus", mqttTopicPrefix);
  sprintf(mqttTopicDo, "%sdo", mqttTopicPrefix);
  sprintf(mqttTopicIp, "%sip", mqttTopicPrefix);

  setup_wifi();
  client.setServer(mqttServer, 1883);
  client.setCallback(mqttCallback);

  //----------- OTA
  ArduinoOTA.setHostname(mqttClientName);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
    {
      type = "sketch";
    }
    else
    { // U_SPIFFS
      type = "filesystem";
    }
    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    delay(1000);
    ESP.restart();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  Serial.println("ready...");
}

void loop()
{
  //handle mqtt connection, non-blocking
  if (!client.connected())
  {
    long now = millis();
    if (now - lastReconnectAttempt > 5000)
    {
      lastReconnectAttempt = now;
      // Attempt to reconnect
      if (MqttReconnect())
      {
        lastReconnectAttempt = 0;
      }
    }
  }
  client.loop();

  //handle state change
  if (onoff != wantedState)
  {
    doOnOff();
  }

  //handle OTA
  ArduinoOTA.handle();
}

// Gets called by the interrupt.
void onChangeButton()
{

  int reading = digitalRead(btnPin); // Get the pin reading.
  if (reading == debounceState)
    return; // Ignore dupe readings.

  boolean debounce = false;

  // Check to see if the change is within a debounce delay threshold.
  if ((millis() - lastDebounceTime) <= debounceDelay)
  {
    debounce = true;
  }

  // This update to the last debounce check is necessary regardless of debounce state.
  lastDebounceTime = millis();

  if (debounce)
    return;                // Ignore reads within a debounce delay threshold.
  debounceState = reading; // All is good, persist the reading as the state.

  if (reading)
  {
    wantedState = !wantedState;
  }
}

void doOnOff()
{
  onoff = !onoff;
  Serial.println("new relay state: " + String(onoff));

  if (onoff)
  {
    turnOn();
  }
  else
  {
    turnOff();
  }
}

void turnOn()
{
  //12 0, 13 1
  digitalWrite(12, LOW);
  digitalWrite(13, HIGH);

  client.publish(mqttTopicState, "1", true);
}

void turnOff()
{
  //12 1, 13 0
  digitalWrite(12, HIGH);
  digitalWrite(13, LOW);

  client.publish(mqttTopicState, "0", true);
}

void setup_wifi()
{

  delay(10);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA); //disable AP mode, only station
  WiFi.hostname(mqttClientName);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (onoff != wantedState)
    {
      doOnOff();
    }
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

bool MqttReconnect()
{

  if (!client.connected())
  {

    Serial.print("Attempting MQTT connection...");

    // Attempt to connect with last will retained
    if (client.connect(mqttClientName, mqttUser, mqttPass, mqttTopicStatus, 1, true, "offline"))
    {

      Serial.println("connected");

      // Once connected, publish an announcement...
      char curIp[16];
      sprintf(curIp, "%d.%d.%d.%d", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3]);

      client.publish(mqttTopicStatus, "online", true);
      client.publish(mqttTopicState, ((onoff) ? "1" : "0"), true);
      client.publish(mqttTopicIp, curIp, true);

      // ... and (re)subscribe
      client.subscribe(mqttTopicDo);
      Serial.print("subscribed to ");
      Serial.println(mqttTopicDo);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
    }
  }
  return client.connected();
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if ((char)payload[0] == '1')
  {
    wantedState = true;
  }
  else
  {
    wantedState = false;
  }
}
