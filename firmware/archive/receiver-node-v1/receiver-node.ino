// Flood EWS receiver node v1 (SMS receive/acknowledge test)
// Board: LilyGO TTGO T-Call (ESP32 + SIM800L)
// Listens for SMS alert levels from the sensor node and acknowledges them.

#include "config.h"

#define SIM800L_RX     26
#define SIM800L_TX     27
#define SIM800L_PWRKEY 4
#define SIM800L_RST    5
#define SIM800L_POWER  23
#define LED_PIN        25

String targetNumber = TARGET_NUMBER;

void setup() {
  // Serial to computer (debug)
  Serial.begin(115200);
  while (!Serial);
  Serial.println("--- TTGO T-CALL DEBUG START ---");

  // Serial to SIM800L
  Serial2.begin(115200, SERIAL_8N1, SIM800L_RX, SIM800L_TX);

  pinMode(LED_PIN, OUTPUT);
  pinMode(SIM800L_PWRKEY, OUTPUT);
  pinMode(SIM800L_POWER, OUTPUT);

  // Power-on sequence for the TTGO T-Call modem
  Serial.println("Powering modem...");
  digitalWrite(SIM800L_POWER, HIGH);

  // Pulse PWRKEY to boot the SIM800L
  digitalWrite(SIM800L_PWRKEY, HIGH);
  delay(100);
  digitalWrite(SIM800L_PWRKEY, LOW);
  delay(1000);
  digitalWrite(SIM800L_PWRKEY, HIGH);

  Serial.println("Waiting for modem (10 s)...");
  delay(10000);

  // AT communication check
  Serial.println("AT check:");
  sendAT("AT");                 // Expect OK
  sendAT("AT+CPIN?");           // SIM card detected?
  sendAT("AT+CSQ");             // Signal strength (0-31)
  sendAT("AT+CMGF=1");          // SMS text mode
  sendAT("AT+CNMI=2,2,0,0,0");  // Push incoming SMS straight to Serial2

  Serial.println("---------------------------------------");
  Serial.println("Ready. Send an SMS to the module.");
}

void loop() {
  // Read data from SIM800L and echo it to the serial monitor
  if (Serial2.available()) {
    String incoming = Serial2.readString();
    Serial.println("\n[INCOMING]: " + incoming);

    // SMS keyword filter
    incoming.toUpperCase();

    if (incoming.indexOf("NORMAL") >= 0) {
      Serial.println("Command received: NORMAL");
      sendReply("NORMAL");
    }
    else if (incoming.indexOf("SIAGA") >= 0) {       // SIAGA = alert / standby
      Serial.println("Command received: SIAGA");
      sendReply("SIAGA");
    }
    else if (incoming.indexOf("DARURAT") >= 0) {     // DARURAT = emergency
      Serial.println("Command received: DARURAT");
      sendReply("DARURAT");
    }
    // An SMS arrived (+CMT) but it is none of the three keywords
    else if (incoming.indexOf("+CMT:") >= 0) {
      Serial.println("Unknown SMS. LED on for 15 s.");
      digitalWrite(LED_PIN, HIGH);
      delay(15000);
      digitalWrite(LED_PIN, LOW);
    }
  }

  // AT commands can also be typed directly in the serial monitor
  if (Serial.available()) {
    Serial2.write(Serial.read());
  }
}

void sendAT(String command) {
  Serial.print("Send: " + command + " -> ");
  Serial2.println(command);
  long t = millis();
  while (millis() - t < 2000) {
    if (Serial2.available()) {
      Serial.print(Serial2.readString());
      break;
    }
  }
}

void sendReply(String msg) {
  Serial.println("Sending reply...");
  Serial2.println("AT+CMGS=\"" + targetNumber + "\"");
  delay(1000);
  Serial2.print(msg);
  delay(100);
  Serial2.write(26); // CTRL+Z
  Serial.println("Send command done.");
}
