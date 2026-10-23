/*
 * Flood EWS receiver node (siren tower), firmware v2
 * Board: LilyGO TTGO T-Call (ESP32 + SIM800L, IP5306 version 20200811)
 * Pairs with sensor node firmware V9.1.
 *
 * - Listens for SMS from the sensor node: "NORMAL" or "SIAGA n" (n = 1..4)
 * - Ignores SMS from any other number (no one can trigger the siren by texting it)
 * - Plays the pre-recorded audio for each stage on a DFPlayer Mini
 *   (tracks 1-4 = SIAGA 1-4, track 5 = NORMAL), through an amplifier to the horn speakers
 * - Relay switches 220 V AC to the amplifier only while audio is playing
 * - Repeats the audio per level (same intervals as the sensor node), max 3
 * - Treats the sensor node's daily self-test SMS as a heartbeat and warns the
 *   admin if nothing arrives for HEARTBEAT_TIMEOUT
 */

#define SIM800L_IP5306_VERSION_20200811
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "utilities.h"   // LilyGO board pin map + setupPMU()
#include "config.h"      // phone numbers (not committed)
#include <TinyGsmClient.h>
#include <DFRobotDFPlayerMini.h>

#define SerialMon Serial
#define SerialAT  Serial1
TinyGsm modem(SerialAT);

HardwareSerial SerialDF(2);
DFRobotDFPlayerMini myDFPlayer;

// ========================================================
//                    PIN DEFINITION
// ========================================================
#define PIN_RX_DF 32                // DFPlayer UART
#define PIN_TX_DF 33
#define PIN_BUSY_DF 21              // DFPlayer BUSY (LOW while playing)
#define PIN_RELAY 2                 // relay: 220 V AC to amplifier
const int PIN_LED_SYSTEM = 25;

// ========================================================
//                    SETTINGS
// ========================================================
// Audio repeat interval per level (same as sensor node)
const unsigned long REPEAT_INTERVAL[5] = {0, 10 * 60000UL, 15 * 60000UL, 20 * 60000UL, 20 * 60000UL};
const int MAX_AUDIO_REPEATS = 3;
const int TRACK_NORMAL = 5;
const unsigned long SMS_SETTLE_TIME = 500;            // wait for the rest of a multi-line SMS
const unsigned long HEARTBEAT_TIMEOUT = 26UL * 3600000UL;  // > 1 day with no SMS from sensor

// ========================================================
//                    STATE
// ========================================================
int currentLevel = 0;               // 0 = NORMAL, 1..4 = SIAGA n
int audioCount = 0;
unsigned long lastAudioTime = 0;
bool isPlayerOk = false;

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
    pinMode(PIN_BUSY_DF, INPUT);

    setupPMU();
    SerialDF.begin(9600, SERIAL_8N1, PIN_RX_DF, PIN_TX_DF);
    if (myDFPlayer.begin(SerialDF)) {
        isPlayerOk = true;
        myDFPlayer.volume(28);
    }
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
    String status = (level == 0) ? "NORMAL" : "SIAGA " + String(level);
    SerialMon.println("[EVENT] Status: " + status);

    playStage(level);   // track 1-4 for SIAGA, track 5 for NORMAL (played once)
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
//                    AUDIO
// ========================================================
void playStage(int level) {
    if (!isPlayerOk) { SerialMon.println(F("[AUDIO] DFPlayer not ready.")); return; }
    myDFPlayer.play(level == 0 ? TRACK_NORMAL : level);
    audioCount = 1;
    lastAudioTime = millis();
    SerialMon.println("[AUDIO] Playing track for level " + String(level));
}

void updateAudio() {
    // Relay ON only while the DFPlayer is playing (powers the amplifier)
    digitalWrite(PIN_RELAY, (digitalRead(PIN_BUSY_DF) == LOW) ? HIGH : LOW);

    // Repeat the stage audio while an alert is active
    if (currentLevel > 0 && isPlayerOk && audioCount < MAX_AUDIO_REPEATS &&
        millis() - lastAudioTime >= REPEAT_INTERVAL[currentLevel]) {
        myDFPlayer.play(currentLevel);
        audioCount++;
        lastAudioTime = millis();
        SerialMon.println("[AUDIO] Repeat " + String(audioCount) + " for level " + String(currentLevel));
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
    updateAudio();

    // Lost contact with the sensor node: warn the admin once
    if (!heartbeatWarned && millis() - lastHeartbeat >= HEARTBEAT_TIMEOUT) {
        heartbeatWarned = true;
        SerialMon.println(F("[WARN] No SMS from sensor node in 26 h."));
        modem.sendSMS(ADMIN_NUMBER, "EWS RECEIVER: tidak ada SMS dari sensor > 26 jam");  // no SMS from sensor > 26 h
    }
}
