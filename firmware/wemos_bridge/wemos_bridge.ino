/*
 * =====================================================================
 *  PUENTE WiFi  --  Wemos D1 Mini (ESP8266)
 *  Parqueadero Inteligente RFID / Sistemas Embebidos
 * =====================================================================
 *
 *  Este sketch NO tiene logica del parqueadero: solo traduce el enlace
 *  serial del Arduino MEGA 2560 a peticiones HTTP contra el backend
 *  FastAPI, y devuelve el veredicto al MEGA.
 *
 *  MEGA  -> WEMOS                        WEMOS -> MEGA
 *  ------------------------------        --------------------------------
 *  SCAN|AB:CD:12:34                      AUTH|1|Bienvenido Juan Perez
 *  SLOT|1|1                              AUTH|0|Tarjeta no registrada
 *  STATE|libres|total|mascara|presencia   NET|1   (WiFi + backend OK)
 *  PIR|1                                  NET|0
 *  BOOT|mega
 *
 *  LIBRERIAS: todas vienen con el core "esp8266 by ESP8266 Community".
 *  PLACA:     Herramientas -> Placa -> "LOLIN(WeMos) D1 R2 & mini"
 *
 *  CABLEADO (opcion recomendada, USE_HW_SERIAL = 0)
 *  ------------------------------------------------
 *    MEGA TX1 (D18) --[1k]--+--> Wemos D6   (divisor 5V -> 3.3V)
 *                           |
 *                          [2k]
 *                           |
 *                          GND
 *    MEGA RX1 (D19) <----------- Wemos D5
 *    MEGA GND       <----------> Wemos GND   (masa comun OBLIGATORIA)
 *
 *  Con USE_HW_SERIAL = 1 se usan los pines rotulados RX/TX del Wemos
 *  (como en el enunciado), pero se pierde el Monitor Serie del Wemos
 *  porque ese UART es el mismo del USB.
 * =====================================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SoftwareSerial.h>

// ======================= CONFIGURA ESTO ==============================
const char* WIFI_SSID     = "TU_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";

// IP del PC donde corre uvicorn (averiguala con "ipconfig").
// El Wemos y el PC deben estar en la MISMA red WiFi.
const char* SERVER_BASE   = "http://192.168.1.100:8000";

// 0 = enlace por SoftwareSerial en D6(RX)/D5(TX)  <-- recomendado
// 1 = enlace por el UART de hardware (pines RX/TX rotulados)
#define USE_HW_SERIAL   0
// =====================================================================

#define LINK_BAUD  9600

#if USE_HW_SERIAL
  #define LINK   Serial
  #define DBG(x)                      // sin depuracion: el UART es del MEGA
  #define DBGLN(x)
#else
  SoftwareSerial link(D6, D5);        // RX, TX
  #define LINK   link
  #define DBG(x)    Serial.print(x)
  #define DBGLN(x)  Serial.println(x)
#endif

String  linea      = "";
uint32_t tNet      = 0;
const uint16_t T_NET = 5000;          // aviso de estado de red al MEGA

// ---------------------------------------------------------------------
void setup() {
#if USE_HW_SERIAL
  Serial.begin(LINK_BAUD);
#else
  Serial.begin(115200);               // USB, solo depuracion
  link.begin(LINK_BAUD);              // enlace con el MEGA
#endif
  delay(200);
  DBGLN(F("\nPuente WiFi del parqueadero"));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  conectarWiFi();
}

// ---------------------------------------------------------------------
void loop() {
  // Lectura por lineas del enlace con el MEGA
  while (LINK.available()) {
    char c = LINK.read();
    if (c == '\n' || c == '\r') {
      if (linea.length()) procesar(linea);
      linea = "";
    } else if (linea.length() < 96) {
      linea += c;
    }
  }

  if (WiFi.status() != WL_CONNECTED) conectarWiFi();

  if (millis() - tNet > T_NET) {
    tNet = millis();
    LINK.println(WiFi.status() == WL_CONNECTED ? F("NET|1") : F("NET|0"));
  }
}

// ---------------------------------------------------------------------
void procesar(const String& msg) {
  DBG(F("MEGA -> ")); DBGLN(msg);

  if (msg.startsWith("SCAN|")) {
    String uid = msg.substring(5);
    uid.trim();
    reenviarScan(uid);
  }
  else if (msg.startsWith("STATE|")) {
    // STATE|libres|total|mascara|presencia
    int p1 = msg.indexOf('|');
    int p2 = msg.indexOf('|', p1 + 1);
    int p3 = msg.indexOf('|', p2 + 1);
    int p4 = msg.indexOf('|', p3 + 1);
    if (p4 < 0) return;

    String body = String("{\"free\":")   + msg.substring(p1 + 1, p2) +
                  ",\"total\":"          + msg.substring(p2 + 1, p3) +
                  ",\"slots\":\""        + msg.substring(p3 + 1, p4) +
                  "\",\"presence\":"     + msg.substring(p4 + 1) + "}";
    postJson("/telemetry", body);
  }
  else if (msg.startsWith("SLOT|") || msg.startsWith("PIR|") ||
           msg.startsWith("BOOT|")) {
    // Eventos informativos: quedan en el log del backend.
    int p = msg.indexOf('|');
    String body = String("{\"kind\":\"") + msg.substring(0, p) +
                  "\",\"data\":\""       + msg.substring(p + 1) + "\"}";
    postJson("/event", body);
  }
}

// ---------------------------------------------------------------------
// POST /scan  ->  el backend decide entrada/salida y responde el mensaje
void reenviarScan(const String& uid) {
  String body = "{\"uid\":\"" + uid + "\"}";
  String resp;
  int code = postJson("/scan", body, &resp);

  if (code == 200) {
    bool   granted = resp.indexOf("\"granted\":true") >= 0;
    String mensaje = jsonString(resp, "message");
    if (mensaje.length() > 16) mensaje = mensaje.substring(0, 16);  // cabe en la LCD
    LINK.print(F("AUTH|"));
    LINK.print(granted ? '1' : '0');
    LINK.print('|');
    LINK.println(mensaje);
    DBG(F("WEMOS -> AUTH ")); DBGLN(resp);
  } else {
    // Sin respuesta: el MEGA decide con su lista local al vencer su timeout.
    DBG(F("Error /scan: ")); DBGLN(code);
  }
}

// ---------------------------------------------------------------------
int postJson(const char* ruta, const String& body, String* respuesta) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2000);
  http.begin(client, String(SERVER_BASE) + ruta);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  if (respuesta && code > 0) *respuesta = http.getString();
  http.end();
  return code;
}

int postJson(const char* ruta, const String& body) {
  return postJson(ruta, body, nullptr);
}

// ---------------------------------------------------------------------
String jsonString(const String& payload, const char* clave) {
  String pat = String("\"") + clave + "\":\"";
  int i = payload.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int j = payload.indexOf('"', i);
  return (j < 0) ? "" : payload.substring(i, j);
}

// ---------------------------------------------------------------------
void conectarWiFi() {
  DBG(F("Conectando a ")); DBG(WIFI_SSID);
  uint8_t intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos++ < 40) {   // ~20 s
    delay(500);
    DBG('.');
  }
  DBGLN("");
  if (WiFi.status() == WL_CONNECTED) {
    DBG(F("IP del Wemos: ")); DBGLN(WiFi.localIP());
    LINK.println(F("NET|1"));
  } else {
    DBGLN(F("Sin WiFi, reintentando en el proximo ciclo"));
    LINK.println(F("NET|0"));
  }
}
