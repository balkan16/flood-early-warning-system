/***************************************************
 *  EWS RECEIVER (siren tower), as deployed
 *  TTGO T-CALL ESP32 + SIM800L + DFPlayer Mini
 *  Plays the stage audio through an always-on amplifier to the horn speakers.
 *
 *  LED:
 *  - GPIO 25 : HEARTBEAT (always blinking)
 *
 *  SMS COMMANDS (EXACT MATCH):
 *  - "NORMAL"  -> Stop audio
 *  - "SIAGA 1" -> Loop audio 0001.mp3
 *  - "SIAGA 2" -> Loop audio 0002.mp3
 *  - "SIAGA 3" -> Loop audio 0003.mp3
 *  - "SIAGA 4" -> Loop audio 0004.mp3
 ***************************************************/

#define SIM800L_IP5306_VERSION_20200811
#define TINY_GSM_MODEM_SIM800
#include "utilities.h"

#include <DFRobotDFPlayerMini.h>

/* ================= SERIAL ================= */
#define SerialMon Serial
#define SerialAT  Serial1

/* ================= DFPLAYER ================= */
#define DFPLAYER_RX 32
#define DFPLAYER_TX 33

HardwareSerial DFSerial(2);
DFRobotDFPlayerMini dfplayer;

/* ================= LED (HEARTBEAT) ================= */
#define PIN_LED 25
unsigned long lastBlinkTime = 0;
const unsigned long BLINK_INTERVAL = 500;

/* ================= AUDIO STATE ================= */
int activeSiaga = 0;                      // 0 = NORMAL, 1–4 = SIAGA
unsigned long lastAudioTime = 0;
const unsigned long AUDIO_LOOP_INTERVAL = 35000;

/* ================= SMS CLEAN TIMER ================= */
unsigned long lastClearTime = 0;
const unsigned long clearInterval = 60000;

/* ==================================================== */
void setup() {
    SerialMon.begin(115200);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    setupPMU();

    pinMode(MODEM_PWRKEY, OUTPUT);
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);

    digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
    digitalWrite(MODEM_PWRKEY, LOW);  delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(5000);

    SerialAT.println("AT+CMGF=1");       delay(500);
    SerialAT.println("AT+CSCS=\"GSM\""); delay(500);
    SerialAT.println("AT+CMGD=1,4");     delay(1000);

    DFSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);

    if (!dfplayer.begin(DFSerial)) {
        SerialMon.println("❌ DFPlayer NOT detected");
    } else {
        SerialMon.println("✅ DFPlayer ready");
        dfplayer.volume(20);
    }

    SerialMon.println("=== EWS RECEIVER READY (HEARTBEAT ACTIVE) ===");
}

/* ==================================================== */
void loop() {

    /* ---------- 1. CHECK SMS (UNCHANGED) ---------- */
    if (SerialAT.available()) {
        String data = SerialAT.readString();

        if (data.indexOf("+CMTI") != -1 ||
            data.indexOf("SIAGA") != -1 ||
            data.indexOf("NORMAL") != -1) {

            SerialAT.println("AT+CMGL=\"ALL\"");
            delay(800);
            String allMessages = SerialAT.readString();

            int newState = -1;

            if      (allMessages.indexOf("\r\nSIAGA 4\r\n") != -1) newState = 4;
            else if (allMessages.indexOf("\r\nSIAGA 3\r\n") != -1) newState = 3;
            else if (allMessages.indexOf("\r\nSIAGA 2\r\n") != -1) newState = 2;
            else if (allMessages.indexOf("\r\nSIAGA 1\r\n") != -1) newState = 1;
            else if (allMessages.indexOf("\r\nNORMAL\r\n")  != -1) newState = 0;

            if (newState != -1 && newState != activeSiaga) {

                SerialMon.printf("🔁 STATE CHANGE: %d → %d\n", activeSiaga, newState);

                dfplayer.stop();
                activeSiaga = newState;

                if (newState >= 1 && newState <= 4) {
                    dfplayer.play(newState);
                    lastAudioTime = millis();
                }
            }

            SerialAT.println("AT+CMGD=1,4");
            lastClearTime = millis();
        }
    }

    /* ---------- 2. AUDIO LOOP (UNCHANGED) ---------- */
    if (activeSiaga >= 1 && activeSiaga <= 4) {
        if (millis() - lastAudioTime >= AUDIO_LOOP_INTERVAL) {
            dfplayer.play(activeSiaga);
            lastAudioTime = millis();
        }
    }

    /* ---------- 3. HEARTBEAT LED (SAFE) ---------- */
    if (millis() - lastBlinkTime >= BLINK_INTERVAL) {
        lastBlinkTime = millis();
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    /* ---------- 4. SMS CLEAN FAILSAFE ---------- */
    if (millis() - lastClearTime >= clearInterval) {
        SerialAT.println("AT+CMGD=1,4");
        lastClearTime = millis();
    }
}