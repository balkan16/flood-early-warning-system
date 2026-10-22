/*
 * Flood EWS sensor node (transmitter), firmware V9.1, as deployed
 * Board: LilyGO TTGO T-Call (ESP32 + SIM800L, IP5306 version 20200811)
 * TMA = Tinggi Muka Air (water level)
 *
 * - 4 float switches -> alert level SIAGA 1 (highest) to SIAGA 4
 * - 4-channel RF remote can force a level manually
 * - DFPlayer Mini plays voice alerts; relay powers the 12 V siren while audio plays
 * - SMS on every status change; daily self-test (song + SMS)
 *
 * V9.1 change log:
 * 1. Second primary number disabled.
 * 2. All reports go to the primary number only.
 * 3. Daily checks (10:00 and 20:00) still report to the primary number.
 */

#define SIM800L_IP5306_VERSION_20200811
#define TINY_GSM_MODEM_SIM800

#include "utilities.h"   // LilyGO board pin map + setupPMU()
#include "config.h"      // phone numbers (not committed)
#include <TinyGsmClient.h>
#include <DFRobotDFPlayerMini.h>

// ========================================================
//                    PHONE NUMBERS
// ========================================================
const char* nomor_utama_1 = PRIMARY_NUMBER;    // primary number
// const char* nomor_utama_2 = ...;            // second primary number, disabled in V9.1
const char* nomor_kedua   = SECONDARY_NUMBER;  // filtered number (no SIAGA 3 & 4)

// ========================================================
//                    SETTING INTERVAL
// ========================================================
// Audio repeat interval per level
const unsigned long S1_INTERVAL = 10 * 60000; 
const unsigned long S2_INTERVAL = 15 * 60000; 
const unsigned long S3_INTERVAL = 20 * 60000; 
const unsigned long S4_INTERVAL = 20 * 60000; 
const int MAX_AUDIO_REPEATS = 3;

const unsigned long FILTER_TIME   = 200;      // per-float noise filter (ms)
const unsigned long DEBOUNCE_TIME = 5000;     // level must hold this long (ms)
const unsigned long COOLDOWN_TIME = 60000;    // min time between status changes (ms)

// ========================================================
//                    PIN DEFINITION
// ========================================================
#define PIN_RX_DF 32      // DFPlayer UART
#define PIN_TX_DF 33      
#define PIN_BUSY_DF 21    // DFPlayer BUSY (LOW while playing)
#define PIN_RELAY 2       // relay: 12 V siren power
#define PIN_RM_A 35       // RF remote channels A-D (input-only pins)
#define PIN_RM_B 36  
#define PIN_RM_C 39  
#define PIN_RM_D 34  
const int PIN_LED_SYSTEM = 25;
const int PIN_S1 = 12, PIN_S2 = 14, PIN_S3 = 15, PIN_S4 = 13;

HardwareSerial SerialDF(2); 
DFRobotDFPlayerMini myDFPlayer;

#define SerialMon Serial
#define SerialAT  Serial1
TinyGsm modem(SerialAT);

unsigned long lastStatusChangeTime = 0, lastBlinkTime = 0, waktuMulaiStabil = 0;
unsigned long lastAudioRepeatTime = 0, lastDebugTime = 0, lastRemoteTriggerTime = 0; 

int pendingLevel = -1, levelTerverifikasi = 0, audioCount = 0; 
bool isModemOk = false, isPlayerOk = false, check10Done = false, check20Done = false; 

unsigned long t1, t2, t3, t4;
bool stableS1 = false, stableS2 = false, stableS3 = false, stableS4 = false;

// --- NETWORK TIME + DAILY SELF-TEST ---
void syncAndCheckTime() {
    int thn, bln, tgl, jam, mnt, dtk; float tz;
    if (modem.getNetworkTime(&thn, &bln, &tgl, &jam, &mnt, &dtk, &tz)) {
        
        // 10:00 self-test: national anthem (0007.mp3)
        // Note: compares hour 6, not 10 (see README, known issues)
        if (jam == 6 && mnt == 0) {
            if (!check10Done) {
                SerialMon.println(F("\n[DAILY] Jam 10:00 - Indonesia Raya"));
                if (isPlayerOk) myDFPlayer.play(7);
                
                String msg = "EWS sedang memutarkan lagu INDONESIA RAYA, ini adalah untuk daily checking bahwa system EWS berfungsi Normal";
                modem.sendSMS(nomor_utama_1, msg);
                // modem.sendSMS(nomor_utama_2, msg); // disabled
                
                SerialMon.println(F("[SMS] 10:00 report sent to primary number."));
                check10Done = true; 
            }
        } else { check10Done = false; }

        // 20:00 self-test: song "Syukur" (0006.mp3)
        // Note: compares hour 22, not 20 (see README, known issues)
        if (jam == 22 && mnt == 0) {
            if (!check20Done) {
                SerialMon.println(F("\n[DAILY] Jam 20:00 - Syukur"));
                if (isPlayerOk) myDFPlayer.play(6);
                
                String msg = "EWS sedang memutarkan lagu Syukur, ini adalah untuk daily checking system EWS bahwa system berfungsi Normal";
                modem.sendSMS(nomor_utama_1, msg);
                // modem.sendSMS(nomor_utama_2, msg); // disabled
                
                SerialMon.println(F("[SMS] 20:00 report sent to primary number."));
                check20Done = true; 
            }
        } else { check20Done = false; }
    }
}

String getTimestamp() {
    int thn, bln, tgl, jam, mnt, dtk; float tz;
    if (modem.getNetworkTime(&thn, &bln, &tgl, &jam, &mnt, &dtk, &tz)) {
        char buf[32];
        sprintf(buf, "%02d:%02d:%02d %02d/%02d/%04d", jam, mnt, dtk, tgl, bln, thn);
        return String(buf);
    }
    return "Syncing...";
}

void setup() {
    SerialMon.begin(115200);
    pinMode(PIN_S1, INPUT_PULLUP); pinMode(PIN_S2, INPUT_PULLUP);
    pinMode(PIN_S3, INPUT_PULLUP); pinMode(PIN_S4, INPUT_PULLUP);
    pinMode(PIN_RM_A, INPUT_PULLDOWN); pinMode(PIN_RM_B, INPUT_PULLDOWN);
    pinMode(PIN_RM_C, INPUT_PULLDOWN); pinMode(PIN_RM_D, INPUT_PULLDOWN);
    pinMode(PIN_LED_SYSTEM, OUTPUT); pinMode(PIN_BUSY_DF, INPUT);
    pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, LOW); 

    setupPMU(); 
    SerialDF.begin(9600, SERIAL_8N1, PIN_RX_DF, PIN_TX_DF);
    if(myDFPlayer.begin(SerialDF)) {
        isPlayerOk = true;
        myDFPlayer.volume(28);
    }
    
    pinMode(MODEM_PWRKEY, OUTPUT); pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    digitalWrite(MODEM_PWRKEY, HIGH); delay(500);
    digitalWrite(MODEM_PWRKEY, LOW);  delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);
    
    SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
    if (modem.restart()) { 
        modem.sendAT("+CLTS=1"); 
        modem.waitResponse();
        isModemOk = true; 
    }
    SerialMon.println(F(">>> EWS TMA BEKASI V9.1 READY <<<"));
}

void loop() {
    if (millis() - lastBlinkTime >= 500) {
        lastBlinkTime = millis();
        digitalWrite(PIN_LED_SYSTEM, !digitalRead(PIN_LED_SYSTEM));
    }

    static unsigned long lastTimeSync = 0;
    if (millis() - lastTimeSync > 10000) { 
        syncAndCheckTime();
        lastTimeSync = millis();
    }

    // --- 1. READ RF REMOTE ---
    bool rmA = (digitalRead(PIN_RM_A) == HIGH);
    bool rmB = (digitalRead(PIN_RM_B) == HIGH);
    bool rmC = (digitalRead(PIN_RM_C) == HIGH);
    bool rmD = (digitalRead(PIN_RM_D) == HIGH);
    bool anyRemote = (rmA || rmB || rmC || rmD);
    if (anyRemote) lastRemoteTriggerTime = millis();

    // --- 2. READ FLOAT SWITCHES (200 ms filter each) ---
    if (digitalRead(PIN_S1) == LOW) { if (millis() - t1 > FILTER_TIME) stableS1 = true; } else { t1 = millis(); stableS1 = false; }
    if (digitalRead(PIN_S2) == LOW) { if (millis() - t2 > FILTER_TIME) stableS2 = true; } else { t2 = millis(); stableS2 = false; }
    if (digitalRead(PIN_S3) == LOW) { if (millis() - t3 > FILTER_TIME) stableS3 = true; } else { t3 = millis(); stableS3 = false; }
    if (digitalRead(PIN_S4) == LOW) { if (millis() - t4 > FILTER_TIME) stableS4 = true; } else { t4 = millis(); stableS4 = false; }

    // --- 3. PICK LEVEL: remote overrides sensors for 10 s after a press ---
    int levelDideteksi = 0;
    if      (rmA) levelDideteksi = 1; 
    else if (rmB) levelDideteksi = 2;
    else if (rmC) levelDideteksi = 3;
    else if (rmD) levelDideteksi = 4;
    else if (millis() - lastRemoteTriggerTime > 10000) { 
        if      (stableS1) levelDideteksi = 1;
        else if (stableS2) levelDideteksi = 2;
        else if (stableS3) levelDideteksi = 3;
        else if (stableS4) levelDideteksi = 4;
        else               levelDideteksi = 0;
    } else { levelDideteksi = pendingLevel; }

    // Relay ON only while the DFPlayer is playing
    digitalWrite(PIN_RELAY, (digitalRead(PIN_BUSY_DF) == LOW) ? HIGH : LOW);

    // --- 4. VERIFY LEVEL (debounce + cooldown) & SEND SMS ---
    if (millis() - lastStatusChangeTime >= COOLDOWN_TIME || anyRemote) {
        if (levelDideteksi != pendingLevel) {
            pendingLevel = levelDideteksi;
            waktuMulaiStabil = millis();
        }

        unsigned long verifyTime = anyRemote ? 100 : DEBOUNCE_TIME;
        if (millis() - waktuMulaiStabil >= verifyTime) {
            if (levelTerverifikasi != pendingLevel) {
                levelTerverifikasi = pendingLevel;
                lastStatusChangeTime = millis(); 
                audioCount = 0; 

                String ts = getTimestamp();
                String statusStr = (levelTerverifikasi == 0) ? "NORMAL" : "SIAGA " + String(levelTerverifikasi);
                
                SerialMon.println(F("\n--------------------------------------"));
                SerialMon.print(F("[EVENT] Status: ")); SerialMon.println(statusStr);
                
                // SMS to primary number
                String msgStatus = "EWS BEKASI: " + statusStr + "\nWaktu: " + ts;
                modem.sendSMS(nomor_utama_1, msgStatus);
                // modem.sendSMS(nomor_utama_2, msgStatus); // disabled

                SerialMon.println(F("[SMS] Status report sent to primary number."));

                // SMS to secondary number (NORMAL, SIAGA 1, SIAGA 2 only)
                if (levelTerverifikasi <= 2) {
                    modem.sendSMS(nomor_kedua, statusStr);
                    SerialMon.println(F("[SMS] Status report sent to secondary number."));
                }
                
                if (isPlayerOk) {
                    myDFPlayer.play((levelTerverifikasi == 0) ? 5 : levelTerverifikasi);
                    audioCount = 1;
                    lastAudioRepeatTime = millis();
                }
                SerialMon.println(F("--------------------------------------\n"));
            }
        }
    }

    // --- 5. REPEAT VOICE ALERT (max 3 plays) ---
    if (levelTerverifikasi > 0 && audioCount < MAX_AUDIO_REPEATS) {
        unsigned long currentInterval = (levelTerverifikasi == 1) ? S1_INTERVAL : (levelTerverifikasi == 2) ? S2_INTERVAL : S3_INTERVAL;
        if (millis() - lastAudioRepeatTime >= currentInterval) {
            if (isPlayerOk) {
                myDFPlayer.play(levelTerverifikasi);
                audioCount++;
                lastAudioRepeatTime = millis();
            }
        }
    }

    if (millis() - lastDebugTime >= 2000) {
        lastDebugTime = millis();
        SerialMon.print(F("LVL:")); SerialMon.print(levelTerverifikasi);
        SerialMon.print(F(" | TIME:")); SerialMon.println(getTimestamp());
    }
}