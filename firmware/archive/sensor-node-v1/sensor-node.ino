// Flood EWS sensor node (transmitter, v1, as deployed)
// Board: LilyGO TTGO T-Call (ESP32 + SIM800L, IP5306 version 20200811)
// Reads 4 water-level contacts and sends "SIAGA <n>" SMS to the receiver nodes.
// SIAGA 1 is the most severe level (Indonesian alert convention).

#define SIM800L_IP5306_VERSION_20200811
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "utilities.h"     // LilyGO board pin map + setupPMU()
#include "config.h"
#include <TinyGsmClient.h>

#define SerialMon Serial
#define SerialAT  Serial1

TinyGsm modem(SerialAT);

// --- PIN CONFIGURATION ---
const int PIN_LED_SYSTEM = 25;
const int PIN_S1 = 12;
const int PIN_S2 = 14;
const int PIN_S3 = 27;
const int PIN_S4 = 13; // Moved from 26 to 13 (avoids conflict with MODEM_RX)

const char* daftar_nomor[] = RECEIVER_NUMBERS;  // receiver numbers
int jumlah_nomor = sizeof(daftar_nomor) / sizeof(daftar_nomor[0]);

const unsigned long DEBOUNCE_TIME = 5000;        // level must be stable for 5 s
const unsigned long BASE_INTERVAL = 15 * 60000;  // resend every level x 15 min

unsigned long lastSmsTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long waktuMulaiStabil = 0;  // time the current reading became stable
int currentSiaga = 0;                // active alert level (0 = normal)
int pendingLevel = 0;                // raw reading waiting for debounce
int levelTerverifikasi = 0;          // debounced level
String pesanSms = "";

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

    pinMode(PIN_S1, INPUT_PULLUP);
    pinMode(PIN_S2, INPUT_PULLUP);
    pinMode(PIN_S3, INPUT_PULLUP);
    pinMode(PIN_S4, INPUT_PULLUP); // Pin 13
    pinMode(PIN_LED_SYSTEM, OUTPUT);

    setupPMU();
    setupModem();

    // Baud rate lowered to 9600 for character stability
    SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(3000);

    SerialMon.println("Initializing modem...");
    if (!modem.restart()) {
      SerialMon.println("Modem restart failed. Check battery!");
      return;
    }

    SerialMon.println("Transmitter ready...");
}

void loop() {
    // Heartbeat LED
    if (millis() - lastBlinkTime >= 500) {
        lastBlinkTime = millis();
        digitalWrite(PIN_LED_SYSTEM, !digitalRead(PIN_LED_SYSTEM));
    }

    // Read sensors (active LOW, S1 has priority)
    int levelSaatIni = 0;
    if (digitalRead(PIN_S1) == LOW)      levelSaatIni = 1;
    else if (digitalRead(PIN_S2) == LOW) levelSaatIni = 2;
    else if (digitalRead(PIN_S3) == LOW) levelSaatIni = 3;
    else if (digitalRead(PIN_S4) == LOW) levelSaatIni = 4;
    else                                 levelSaatIni = 0;

    // Debounce: restart the timer whenever the reading changes
    if (levelSaatIni != pendingLevel) {
        pendingLevel = levelSaatIni;
        waktuMulaiStabil = millis();
    }

    if (millis() - waktuMulaiStabil >= DEBOUNCE_TIME) {
        levelTerverifikasi = pendingLevel;
    }

    if (levelTerverifikasi > 0) {
        // Escalation (lower number = more severe): send immediately
        if (currentSiaga == 0 || levelTerverifikasi < currentSiaga) {
            currentSiaga = levelTerverifikasi;
            kirimSmsEws(currentSiaga);
            lastSmsTime = millis();
        }
        // De-escalation: update level silently, next periodic SMS carries it
        else if (levelTerverifikasi > currentSiaga) {
            currentSiaga = levelTerverifikasi;
        }

        // Periodic reminder: SIAGA 1 every 15 min ... SIAGA 4 every 60 min
        unsigned long intervalAktif = (unsigned long)currentSiaga * BASE_INTERVAL;
        if (millis() - lastSmsTime >= intervalAktif) {
            kirimSmsEws(currentSiaga);
            lastSmsTime = millis();
        }
    } else {
        currentSiaga = 0;
    }
}

void kirimSmsEws(int level) {
    String teks = "SIAGA " + String(level); // Plain message, no extra characters

    SerialMon.println("\n[SEND] Sending: " + teks);
    for (int i = 0; i < jumlah_nomor; i++) {
        if (modem.sendSMS(daftar_nomor[i], teks)) {
            SerialMon.println("SMS OK");
        } else {
            SerialMon.println("SMS FAILED");
        }
    }
}
