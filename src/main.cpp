/*
    Read energy production from ABB Aurora inverter
    Publish to PVOUTPUT
    Log to MQTT
    Uses https://www.arduino.cc/reference/en/libraries/abb-powerone-aurora-inverter-communication-protocol/
*/

#include "defs.h"

#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <Aurora.h>
#include <TimeLib.h>

#include "led.h"


#ifndef NTP_OFFSET
#define NTP_OFFSET 0
#endif

#ifndef UPDATE_PERIOD_PVOUTPUT
#define UPDATE_PERIOD_PVOUTPUT 300 // Update PV Output every 5 minutes
#endif

#ifndef UPDATE_PERIOD_STATS
#define UPDATE_PERIOD_STATS  30  // Update stats every 30 seconds
#endif


WiFiClient wifiClient;
BearSSL::WiFiClientSecure wifiClientSecure;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_OFFSET);
ESP8266WebServer webServer(80);
PubSubClient pubSubClient(wifiClient);
Led led(LED_BUILTIN);


#define STDOUT Serial

#ifdef STDOUT
#define log(...) STDOUT.printf(__VA_ARGS__)
#else
#define log(...) do {} while(0)
#endif

#if defined STDOUT && defined DEBUG
#define debug(...) log(__VA_ARGS__)
#else
#define debug(...) do {} while(0)
#endif


////////////////////////////////////////////////////////////////////////////////////////////////////
// Call handlers in loop
void runLoopHandlers() {
    pubSubClient.loop();
    timeClient.update();
    webServer.handleClient();
    led.loop();
}


void runLoopDelay(unsigned long delayTime, unsigned long delayResolution=10) {
    unsigned long tStart = millis();
    while((millis() - tStart) < delayTime) {
        runLoopHandlers();
        delay(delayResolution);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Time + NTP
#define TIME_STR timeClient.getFormattedTime().c_str()


char * _formatFloat(char *buf, size_t bufLen, float val) {
    memset(buf, 0, bufLen);
    if (isnan(val)) {
        strncpy(buf, "NaN", bufLen-1);
    } else {
        snprintf(buf, bufLen-1, "%.2f", val);
    }
    return buf;
}


static inline unsigned long toLocalTime(unsigned long t) {
    return t + NTP_OFFSET;
}

static inline unsigned long fromLocalTime(unsigned long t) {
    return t - NTP_OFFSET;
}

unsigned long getEpochTime() {
    // NTPClient adds offset to epoch time. This is incorrect. Fix it.
    return fromLocalTime(timeClient.getEpochTime());;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// WiFi
const char wifiSsid[] = WIFI_SSID;
const char wifiPassword[] = WIFI_PASSWORD;
char wifiMac[13] = {0};

char *readWifiMac() {
    byte _wifiMac[6];
    WiFi.macAddress(_wifiMac);
    snprintf(wifiMac, sizeof(wifiMac), "%02x%02x%02x%02x%02x%02x", _wifiMac[0], _wifiMac[1], _wifiMac[2], _wifiMac[3], _wifiMac[4], _wifiMac[5]);
    return wifiMac;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// MQTT using PubSubClient
const char mqttHost[] = MQTT_HOST;
const uint16_t mqttPort = MQTT_PORT;
const char mqttTopicName[] = MQTT_TOPIC;
const char mqttUser[] = MQTT_USER;
const char mqttPassword[] = MQTT_PASSWORD;


const char pvoutputAddStatsUrl[] = "https://pvoutput.org/service/r2/addstatus.jsp";
const char pvoutputApiKey[] = PVOUTPUT_API_KEY;
const char pvoutputApiSID[] = PVOUTPUT_API_SID;


Aurora inverter = Aurora(INVERTER_ADDRESS, PIN_AURORA_RX, PIN_AURORA_TX, PIN_AURORA_TX_CTL);


bool isWifiConnected() {
    return (WiFi.status() == WL_CONNECTED);
}


void waitForWifi() {
    while (WiFi.status() != WL_CONNECTED) {
        log(".");
        runLoopDelay(500);
    }
}


#define MQTT_CLIENT_ID_MAX_LEN 64

const char mqttTopicLwt[] = "tele/%s/LWT";
const char mqttTopicStat[] = "tele/%s/STAT";
const char mqttTopicPower[] = "tele/%s/POWER";
const char mqttTopicLog[] = "tele/%s/LOG";
const char mqttMessageOnline[] = "Online";
const char mqttMessageOffline[] = "Offline";

static char _mqttTopic[64];


char * mqttTopic(const char *topicFmt) {
    snprintf(_mqttTopic, sizeof(_mqttTopic)-1, topicFmt, mqttTopicName);
    return _mqttTopic;
}

bool mqttConnected() {
    return pubSubClient.connected();
}

void mqttConnectCheck() {
    // Connect pubSubClient if it is not already connected
    while (!mqttConnected()) {
        char clientId[MQTT_CLIENT_ID_MAX_LEN];
        snprintf(clientId, MQTT_CLIENT_ID_MAX_LEN-1, "%s-%s", mqttTopicName, wifiMac);
        log("Connecting for MQTT: %s:%d as %s\n", mqttHost, mqttPort, clientId);

        // Attempt to connect
        char *willTopic = mqttTopic(mqttTopicLwt);
        if (pubSubClient.connect(clientId, mqttUser, mqttPassword, willTopic, 1, true, mqttMessageOffline)) {
            log("MQTT connected\n");
            pubSubClient.publish(willTopic, mqttMessageOnline, true);
            // Subscribe to topics of interest if there are any
        } else {
            log("MQTT connection failed! Error code = %d\n", pubSubClient.state());
            runLoopDelay(60*1000);
        }
    }
}


void mqttSend(const char *topic_fmt, const char *msg, bool waitForConnection = false) {
    if (waitForConnection) {
        mqttConnectCheck();
    }
    const char *topic = mqttTopic(topic_fmt);
    debug("MQTT: Publishing '%s': '%s'\n", topic, msg);
    pubSubClient.publish(topic, msg);
}


void mqttLog(const char *fmt, ...) {
    if (mqttConnected()) {
        char *topic = mqttTopic(mqttTopicLog);
        char msg[1024] = {0};
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg)-1, fmt, args);
        va_end(args);
        pubSubClient.publish(topic, msg);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Arduino core functions

void pubSubCallback(char* topic, byte* payload, unsigned int length);
bool inverterReadTodayEnergy();
bool pvOutputSend();
bool inverterSetTime(void);
bool inverterOnline(void);
void inverterUpdateStatus(void);
void webHandle404(void);
void webHandleRoot(void);


void setup() {

    delay(1000);
    led.on();

    readWifiMac();
    // Configure wifiClientSecure. Either add certificate store, or don't care
    wifiClientSecure.setInsecure();

    // Init serial for debuging
    STDOUT.begin(115200);

    // Init Inverter
    inverter.begin();

    for(int i=3; i>0; i--) {
        log("Starting in %d\n", i);
        delay(1000);
    }
    log("Starting: %s\n", wifiMac);

    // Configre + start WiFi
    log("WiFi Connecting to: %s\n", wifiSsid);
    WiFi.begin(wifiSsid, wifiPassword);
    led.flashFast();
    waitForWifi();
    led.on();
    log("\nWiFi Connected: %s\n", WiFi.localIP().toString().c_str());

    // Start NTP
    timeClient.begin();
    timeClient.setUpdateInterval(5 * 60 * 1000); // Set update to 5 minutes
    log("Waiting for NTP update:\n.");
    while (!timeClient.forceUpdate()) {  // Force NTP update
        delay(500);
        log(".");
    }
    log("\n");
    log("NTP time: %lu = %s\n", getEpochTime(), timeClient.getFormattedTime().c_str());

    // Configure web server
    webServer.on("/", webHandleRoot);
    webServer.begin();

    // Configure MQTT
    pubSubClient.setServer(mqttHost, mqttPort);
    pubSubClient.setCallback(pubSubCallback);
    mqttConnectCheck();

    led.on();
    runLoopDelay(2000);
    led.off();
}


void loop() {
    static unsigned long nextUpdateTime = (getEpochTime() / UPDATE_PERIOD_PVOUTPUT) * UPDATE_PERIOD_PVOUTPUT + UPDATE_PERIOD_PVOUTPUT;
    static unsigned long nextStatsTime = (getEpochTime() / UPDATE_PERIOD_STATS) * UPDATE_PERIOD_STATS + UPDATE_PERIOD_STATS;
    bool pvOutputUpdatePending = false;

    runLoopHandlers();

    if (getEpochTime() >= nextUpdateTime) {
        // Update time on inverter
        inverterSetTime();
        // Read daily cumilative energy
        if (inverterReadTodayEnergy()) {
            pvOutputUpdatePending = true;
            led.flashFast(1);
        } else {
            led.flashFast(4);
        }
        nextUpdateTime += UPDATE_PERIOD_PVOUTPUT;
        log("%s: Cumulative Energy updated. Next update scheduled at %lu\n", TIME_STR, nextUpdateTime);
    }
    if (getEpochTime() >= nextStatsTime) {
        inverterUpdateStatus();
        nextStatsTime += UPDATE_PERIOD_STATS;
        led.flashFast(1);
    }
    if (pvOutputUpdatePending) {
        if (WiFi.status() == WL_CONNECTED) {
            // Try to send PVOutput
            if (pvOutputSend()) {
                pvOutputUpdatePending = false;
                led.flashFast(2);
            } else {
                led.flashFast(5);
                runLoopDelay(1000);
            }
        }
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// PubSubClient handlers


void pubSubCallback(char* topic, byte* payload, unsigned int length) {
    log("Message arrived [%s] %s\n", topic, (char *)payload);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Inverter functions


unsigned long energyToday = 0;
unsigned long energyTodayLastUpdate = 0;
unsigned long energyTodayLastPublished = 0;
char inverterStatus[2048] = "{}";


bool inverterOnline() {
    // Check if inverter is online
    Aurora::DataState dataState = inverter.readState();
    return dataState.state.readState;
}


void logInverterState(const char *action, Aurora::OutcomeState *state) {
    // Log Aurora
    log("Inverter Error: %s: %s (%d), %s\n", action, state->getGlobalState().c_str(), state->readState, state->getTransmissionState().c_str());
}


float inverterReadDSP(byte type) {
    Aurora::DataDSP dataDSP = inverter.readDSP(type);
    if (!dataDSP.state.readState) {
        logInverterState("inverterReadDSP", &dataDSP.state);
        return NAN;
    }
    return dataDSP.value;
}


bool inverterReadTodayEnergy() {
    unsigned long now = getEpochTime();
    // Read inverter cumulative daily energy, set globals. Returns true on success.
    Aurora::DataCumulatedEnergy cumulatedEnergy = inverter.readCumulatedEnergy(CUMULATED_DAILY_ENERGY);
    if (!cumulatedEnergy.state.readState) {
        logInverterState("readCumulatedEnergy CUMULATED_DAILY_ENERGY", &cumulatedEnergy.state);
        return false;
    }
    energyToday = cumulatedEnergy.energy;
    energyTodayLastUpdate = now;
    log("%s: Updated Today's energy: %lu (%lu) = %lu\n", TIME_STR, energyTodayLastUpdate, toLocalTime(now), energyToday);
    mqttLog("updated Today's energy: %lu (%lu) = %lu", energyTodayLastUpdate, toLocalTime(now), energyToday);
    return true;
}


bool inverterSetTime() {
    unsigned long inverterEpochLocalTime = 0;
    Aurora::DataTimeDate dataTimeDate = inverter.readTimeDate();
    if (!dataTimeDate.state.readState) {
        logInverterState("readTimeDate", &dataTimeDate.state);
    } else {
        inverterEpochLocalTime = dataTimeDate.epochTime;
    }
    unsigned long newEpochLocalTime = toLocalTime(getEpochTime());
    log("%s: Setting inverter time: was %lu setting to: %lu\n", TIME_STR, inverterEpochLocalTime, newEpochLocalTime);
    if (!inverter.writeTimeDate(newEpochLocalTime)) {
        log("Inverter error writeTimeDate\n");
        return false;
    }
    return true;
}


void inverterUpdateStatus() {
    unsigned long energyToday = 0;
    unsigned long energyLifetime = 0;
    float pIn1 = NAN;
    float pIn2 = NAN;
    float pIn = NAN;
    float vGrid = NAN;
    float fGrid = NAN;
    float tempInverter = NAN;
    float tempBooster = NAN;
    unsigned long now = getEpochTime();
    char pIn1_s[20];
    char pIn2_s[20];
    char pIn_s[20];
    char vGrid_s[20];
    char fGrid_s[20];
    char tempInverter_s[20];
    char tempBooster_s[20];
   
    if (!inverterOnline()) {
        log("%s: Can not update inverter stats - inverter offline\n", TIME_STR);
        return;
    }
 
    Aurora::DataCumulatedEnergy cumulatedEnergy = inverter.readCumulatedEnergy(CUMULATED_DAILY_ENERGY);
    if (!cumulatedEnergy.state.readState) {
        logInverterState("readCumulatedEnergy CUMULATED_DAILY_ENERGY", &cumulatedEnergy.state);
    } else {
        energyToday = cumulatedEnergy.energy;
    }
    cumulatedEnergy = inverter.readCumulatedEnergy(CUMULATED_TOTAL_ENERGY_LIFETIME);
    if (!cumulatedEnergy.state.readState) {
        logInverterState("readCumulatedEnergy CUMULATED_DAILY_ENERGY", &cumulatedEnergy.state);
    } else {
        energyLifetime = cumulatedEnergy.energy;
    }
    pIn1 = inverterReadDSP(DSP_PIN1_ALL);
    pIn2 = inverterReadDSP(DSP_PIN2);
    vGrid = inverterReadDSP(DSP_GRID_VOLTAGE_ALL);
    fGrid = inverterReadDSP(DSP_FREQUENCY_ALL);
    tempInverter = inverterReadDSP(DSP_INVERTER_TEMPERATURE_GT);
    tempBooster = inverterReadDSP(DSP_BOOSTER_TEMPERATURE_GT);
    if (!isnan(pIn1) && !isnan(pIn2)) {
        pIn = pIn1 + pIn2;
    }
    _formatFloat(pIn_s, sizeof(pIn_s), pIn),
    _formatFloat(pIn1_s, sizeof(pIn1_s), pIn1),
    _formatFloat(pIn2_s, sizeof(pIn2_s), pIn2),
    _formatFloat(vGrid_s, sizeof(vGrid_s), vGrid),
    _formatFloat(fGrid_s, sizeof(fGrid_s), fGrid),
    _formatFloat(tempInverter_s, sizeof(tempInverter_s), tempInverter),
    _formatFloat(tempBooster_s, sizeof(tempBooster_s), tempBooster),

    // Format last status
    snprintf(inverterStatus, sizeof(inverterStatus),
        (
            "{"
                "\"last_update\": %lu, "
                "\"energy_today\": %lu, "
                "\"energy_total\": %lu, "
                "\"last_pvoutput_read\": %lu, "
                "\"last_pvoutput_sent\": %lu, "
                "\"p_in\": %s, "
                "\"p_in_1\": %s, "
                "\"p_in_2\": %s, "
                "\"grid_voltage\": %s, "
                "\"grid_frequency\": %s, "
                "\"temp_inverter\": %s, "
                "\"temp_booster\": %s"
            "}"
        ),
        now,
        energyToday,
        energyLifetime,
        energyTodayLastUpdate,
        energyTodayLastPublished,
        pIn_s,
        pIn1_s,
        pIn2_s,
        vGrid_s,
        fGrid_s,
        tempInverter_s,
        tempBooster_s
    );
    log("%s: Status updated: %s\n", TIME_STR, inverterStatus);
    if (mqttConnected()) {
        if (!isnan(pIn)) {
            mqttSend(mqttTopicPower, pIn_s);
        }
        mqttSend(mqttTopicStat, inverterStatus);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// PV Output Functions


bool pvOutputSend() {
    unsigned long timeLocal = toLocalTime(energyTodayLastUpdate);
    log("%s: Sending to PV Output: %lu = %lu\n", TIME_STR, timeLocal, energyToday);
    char post_data[512] = {0};
    snprintf(post_data, sizeof(post_data),
        (
            "d=%04d%02d%02d&"
            "t=%02d:%02d&"
            "v1=%lu&c1=0"
        ),
        year(timeLocal), month(timeLocal), day(timeLocal),
        hour(timeLocal), minute(timeLocal),
        energyToday
    );
    log("%s: Posting to %s: %s\n", TIME_STR, pvoutputAddStatsUrl, post_data);
    HTTPClient http;
    http.setReuse(false);
    if (!http.begin(wifiClientSecure, pvoutputAddStatsUrl)) {
        log("%s: http begin failed\n", TIME_STR);
        return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("X-Pvoutput-Apikey", pvoutputApiKey);
    http.addHeader("X-Pvoutput-SystemId", pvoutputApiSID);
    int httpCode = http.POST((uint8_t*)post_data, strlen(post_data));
    if (httpCode > 0) {
        log("%s: PV Output update returned %d\n", TIME_STR, httpCode);
        log("%s\n", http.getString().c_str());
        mqttLog("PV Output update (%s) returned %d\n", post_data, httpCode);
    } else {
        log("%s: PV Output update error: %s\n", TIME_STR, http.errorToString(httpCode).c_str());
        mqttLog("PV Output update (%s) error %s\n", post_data, http.errorToString(httpCode).c_str());
    }
    http.end();
    if (httpCode == 200) {
        energyTodayLastPublished = getEpochTime();
        return true;
    }
    return false;
}



////////////////////////////////////////////////////////////////////////////////////////////////////
// HTTP Server Handlers


void webHandle404() {
    webServer.send(404, "text/html", "Not Found\r\n");
}


// Serve inverterStatus
void webHandleRoot() {
    if (webServer.method() != HTTP_GET) {
        webHandle404();
        return;
    }
    log("Web request received: %s %d %s\n", webServer.client().remoteIP().toString().c_str(), webServer.method(), webServer.uri().c_str());
    webServer.send(200, "application/json", inverterStatus);
}

