/*
 * Flood EWS receiver node (siren tower), firmware v2
 * Board: LilyGO TTGO T-Call (ESP32 + SIM800L, IP5306 version 20200811)
 * Pairs with sensor node firmware V9.1.
 *
 * - Listens for SMS from the sensor node: "NORMAL" or "SIAGA n" (n = 1..4)
 * - Ignores SMS from any other number (no one can trigger the siren by texting it)
 * - Drives the 12 V tower siren through a relay with a pattern per level
 * - Repeats the siren burst per level (same intervals as the sensor node), max 3
 * - Treats the sensor node's daily self-test SMS as a heartbeat and warns the
 *   admin if nothing arrives for HEARTBEAT_TIMEOUT
 */

#define SIM800L_IP5306_VERSION_20200811
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "utilities.h"   // LilyGO board pin map + setupPMU()
#include "config.h"      // phone numbers (not committed)
#include <TinyGsmClient.h>

#define SerialMon Serial
#define SerialAT  Serial1
TinyGsm modem(SerialAT);

// ========================================================
//                    PIN DEFINITION
// ========================================================
#define PIN_RELAY 2                 // relay: 12 V siren power
const int PIN_LED_SYSTEM = 25;

// ========================================================
//                    SETTINGS
// ========================================================
// Siren repeat interval per level (matches sensor node audio repeats)
const unsigned long REPEAT_INTERVAL[5] = {0, 10 * 60000UL, 15 * 60000UL, 20 * 60000UL, 20 * 60000UL};
const int MAX_SIREN_REPEATS = 3;
const unsigned long BURST_TIME = 60000;               // one siren burst lasts 60 s
const unsigned long SMS_SETTLE_TIME = 500;            // wait for the rest of a multi-line SMS
const unsigned long HEARTBEAT_TIMEOUT = 26UL * 3600000UL;  // > 1 day with no SMS from sensor

// Siren pattern per level: ON ms, OFF ms (OFF = 0 means continuous)
const unsigned long PATTERN_ON[5]  = {0, 1000, 2000, 1000, 500};
const unsigned long PATTERN_OFF[5] = {0,    0, 1000, 1000, 2500};

// ========================================================
//                    STATE
// ========================================================
int currentLevel = 0;               // 0 = NORMAL, 1..4 = SIAGA n
int sirenCount = 0;
bool burstActive = false;
unsigned long burstStart = 0, lastBurstTime = 0, patternTime = 0;
bool relayOn = false;

unsigned long lastBlinkTime = 0, lastHeartbeat = 0;
bool heartbeatWarned = false;

String lineBuf = "";
bool cmtPending = false;            // +CMT header seen, collecting the body
String smsSender = "", smsBody = "";
unsigned long lastSmsLine = 0;

// ========================================================
//                    MODEM
// ========================================================
void setupModem() {
    pinMode(MODEM_PWRKEY, OUTPUT);
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
    digitalWrite(MODEM_PWRKEY, LOW);  delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);
}

void setup() {
    SerialMon.begin(115200);
    pinMode(PIN_LED_SYSTEM, OUTPUT);
    pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, LOW);

    setupPMU();
    setupModem();

    // Same baud rate as the sensor node for character stability
    SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(3000);

    SerialMon.println(F("Initializing modem..."));
    if (!modem.restart()) {
        SerialMon.println(F("Modem restart failed. Check power!"));
    }
    modem.sendAT("+CMGF=1");          modem.waitResponse();   // SMS text mode
    modem.sendAT("+CNMI=2,2,0,0,0");  modem.waitResponse();   // push new SMS to serial
    modem.sendAT("+CMGDA=\"DEL ALL\""); modem.waitResponse(); // clear stored SMS

    lastHeartbeat = millis();
    SerialMon.println(F(">>> EWS RECEIVER READY <<<"));
}

// ========================================================
//                    SMS PARSING
// ========================================================
// Returns 0..4 for a status message, -1 for heartbeat, -2 if not recognised
int parseLevel(String msg) {
    msg.toUpperCase();
    if (msg.indexOf("DAILY") >= 0) return -1;          // sensor self-test message
    int i = msg.indexOf("SIAGA ");
    if (i >= 0 && i + 6 < (int)msg.length()) {
        int n = msg.charAt(i + 6) - '0';
        if (n >= 1 && n <= 4) return n;
    }
    if (msg.indexOf("NORMAL") >= 0) return 0;
    return -2;
}

void handleSms(const String& sender, const String& body) {
    SerialMon.println("\n[SMS] From " + sender + ": " + body);

    if (sender != TRANSMITTER_NUMBER) {
        SerialMon.println(F("[SMS] Ignored: not from sensor node."));
        return;
    }
    lastHeartbeat = millis();
    heartbeatWarned = false;

    int level = parseLevel(body);
    if (level == -1) { SerialMon.println(F("[SMS] Heartbeat received.")); return; }
    if (level == -2) { SerialMon.println(F("[SMS] Unknown format, ignored.")); return; }
    if (level == currentLevel) return;                  // no change

    currentLevel = level;
    sirenCount = 0;
    String status = (level == 0) ? "NORMAL" : "SIAGA " + String(level);
    SerialMon.println("[EVENT] Status: " + status);

    if (level == 0) {
        stopSiren();
    } else {
        startBurst();
    }
    modem.sendSMS(ADMIN_NUMBER, "EWS RECEIVER: " + status + " diterima");  // "received"
}

// Collect +CMT header and body lines without blocking
void readModem() {
    while (SerialAT.available()) {
        char c = SerialAT.read();
        if (c == '\r') continue;
        if (c != '\n') { lineBuf += c; continue; }

        String line = lineBuf; lineBuf = "";
        if (line.length() == 0) continue;

        if (line.startsWith("+CMT:")) {
            if (cmtPending) handleSms(smsSender, smsBody);  // flush previous SMS
            int q1 = line.indexOf('"'), q2 = line.indexOf('"', q1 + 1);
            smsSender = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
            smsBody = "";
            cmtPending = true;
        } else if (cmtPending) {
            if (smsBody.length()) smsBody += "\n";
            smsBody += line;
        }
        lastSmsLine = millis();
    }
    if (cmtPending && millis() - lastSmsLine >= SMS_SETTLE_TIME) {
        cmtPending = false;
        handleSms(smsSender, smsBody);
    }
}

// ========================================================
//                    SIREN
// ========================================================
void setRelay(bool on) {
    if (on != relayOn) { relayOn = on; digitalWrite(PIN_RELAY, on ? HIGH : LOW); }
}

void startBurst() {
    burstActive = true;
    burstStart = patternTime = lastBurstTime = millis();
    sirenCount++;
    setRelay(true);
    SerialMon.println("[SIREN] Burst " + String(sirenCount) + " for level " + String(currentLevel));
}

void stopSiren() {
    burstActive = false;
    setRelay(false);
}

void updateSiren() {
    if (currentLevel == 0) return;

    if (burstActive) {
        if (millis() - burstStart >= BURST_TIME) { stopSiren(); return; }
        unsigned long onT = PATTERN_ON[currentLevel], offT = PATTERN_OFF[currentLevel];
        if (offT == 0) { setRelay(true); return; }      // continuous
        unsigned long phase = (millis() - patternTime) % (onT + offT);
        setRelay(phase < onT);
    } else if (sirenCount < MAX_SIREN_REPEATS &&
               millis() - lastBurstTime >= REPEAT_INTERVAL[currentLevel]) {
        startBurst();
    }
}

// ========================================================
//                    LOOP
// ========================================================
void loop() {
    if (millis() - lastBlinkTime >= 500) {
        lastBlinkTime = millis();
        digitalWrite(PIN_LED_SYSTEM, !digitalRead(PIN_LED_SYSTEM));
    }

    readModem();
    updateSiren();

    // Lost contact with the sensor node: warn the admin once
    if (!heartbeatWarned && millis() - lastHeartbeat >= HEARTBEAT_TIMEOUT) {
        heartbeatWarned = true;
        SerialMon.println(F("[WARN] No SMS from sensor node in 26 h."));
        modem.sendSMS(ADMIN_NUMBER, "EWS RECEIVER: tidak ada SMS dari sensor > 26 jam");  // no SMS from sensor > 26 h
    }
}
