/*
 * Parqueadero RFID - Firmware ESP8266 (Wemos D1 Mini)
 * Sistemas Embebidos
 *
 * Lee el UID de una tarjeta con el modulo RC522 y lo envia por WiFi
 * al backend (POST /scan). Toda la logica (entrada/salida/duracion)
 * vive en el backend; el ESP solo lee, manda y reacciona a la respuesta.
 *
 * Librerias necesarias (Gestor de librerias del Arduino IDE):
 *   - MFRC522  (by GithubCommunity)
 *   Las de WiFi/HTTP ya vienen con el core de ESP8266.
 *
 * Placa: "LOLIN(WeMos) D1 R2 & mini"  (instala el core esp8266 by ESP8266 Community)
 *
 * ---- Conexiones RC522 -> Wemos D1 Mini ----
 *   RC522 SDA (SS) -> D2   (GPIO4)
 *   RC522 SCK      -> D5   (GPIO14)
 *   RC522 MOSI     -> D7   (GPIO13)
 *   RC522 MISO     -> D6   (GPIO12)
 *   RC522 RST      -> D1   (GPIO5)
 *   RC522 3.3V     -> 3V3   (¡OJO! el RC522 va a 3.3V, NO a 5V)
 *   RC522 GND      -> GND
 *   RC522 IRQ      -> sin conectar
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ======================= CONFIGURA ESTO =======================
const char* WIFI_SSID     = "TU_WIFI";              // <-- nombre de tu red
const char* WIFI_PASSWORD = "TU_PASSWORD";          // <-- clave de tu red

// IP local del PC donde corre el backend + puerto. Averigua la IP con
// "ipconfig" (Windows). El ESP y el PC deben estar en la MISMA red WiFi.
const char* SERVER_URL    = "http://192.168.1.100:8000/scan";  // <-- cambia la IP
// ==============================================================

// Pines del RC522
#define SS_PIN   D2   // GPIO4
#define RST_PIN  D1   // GPIO5

MFRC522 mfrc522(SS_PIN, RST_PIN);

// LED integrado del D1 Mini (GPIO2, se enciende en LOW)
#define LED_PIN  LED_BUILTIN

// -----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // apagado (activo en bajo)

  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("Lector RC522 listo. Acerca una tarjeta...");

  connectWiFi();
}

// -----------------------------------------------------------------
void loop() {
  // Reconecta si se cayo el WiFi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // ¿Hay una tarjeta nueva?
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String uid = uidToString(&mfrc522.uid);
  Serial.print("Tarjeta detectada -> UID: ");
  Serial.println(uid);

  sendScan(uid);

  // Termina la comunicacion con la tarjeta y espera un poco para
  // no registrar la misma tarjeta varias veces seguidas.
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(2500);
}

// -----------------------------------------------------------------
// Conecta al WiFi
void connectWiFi() {
  Serial.print("Conectando a WiFi ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Conectado. IP del ESP: ");
  Serial.println(WiFi.localIP());
}

// -----------------------------------------------------------------
// Convierte el UID a texto tipo "AB:CD:12:34" (mayusculas, con ':')
// para que coincida con el formato guardado en la tabla cards.
String uidToString(MFRC522::Uid* uid) {
  String s = "";
  for (byte i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) s += "0";     // padding a 2 digitos
    s += String(uid->uidByte[i], HEX);
    if (i < uid->size - 1) s += ":";
  }
  s.toUpperCase();
  return s;
}

// -----------------------------------------------------------------
// Envia el POST /scan con {"uid": "..."} y reacciona a la respuesta
void sendScan(const String& uid) {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"uid\":\"" + uid + "\"}";
  int code = http.POST(body);

  if (code > 0) {
    String payload = http.getString();
    Serial.print("HTTP ");
    Serial.print(code);
    Serial.print(" -> ");
    Serial.println(payload);

    // Datos utiles de la respuesta (listos para LCD/RGB si los agregas)
    String action  = jsonString(payload, "action");    // entry / exit / denied
    String message = jsonString(payload, "message");
    Serial.print("  Accion : "); Serial.println(action);
    Serial.print("  Mensaje: "); Serial.println(message);

    bool granted = payload.indexOf("\"granted\":true") >= 0;
    if (granted) {
      blink(2, 120);   // acceso concedido: 2 parpadeos rapidos
    } else {
      blink(1, 600);   // denegado: 1 parpadeo largo
    }
  } else {
    Serial.print("Error en la peticion: ");
    Serial.println(http.errorToString(code));
    blink(3, 80);      // error de red
  }

  http.end();
}

// -----------------------------------------------------------------
// Extrae el valor string de una clave del JSON: busca "clave":"valor"
String jsonString(const String& payload, const String& key) {
  String pat = "\"" + key + "\":\"";
  int i = payload.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int j = payload.indexOf("\"", i);
  if (j < 0) return "";
  return payload.substring(i, j);
}

// -----------------------------------------------------------------
// Parpadea el LED integrado (activo en bajo)
void blink(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);   // encendido
    delay(ms);
    digitalWrite(LED_PIN, HIGH);  // apagado
    delay(ms);
  }
}
