/*
 * =====================================================================
 *  PARQUEADERO INTELIGENTE RFID  --  Arduino MEGA 2560  (nodo principal)
 *  Sistemas Embebidos
 * =====================================================================
 *
 *  El MEGA hace TODA la logica local (maquina de estados, actuadores,
 *  tiempos, conteo de puestos) y transmite los eventos por Serial1 al
 *  Wemos D1 Mini, que los reenvia por WiFi al backend FastAPI.
 *
 *  FLUJO
 *  -----
 *   1. Reposo      : LEDs ROJOS de entrada encendidos, LCD apagada.
 *   2. PIR (INT)   : detecta el vehiculo -> interrupcion -> LCD ON con
 *                    mensaje de bienvenida ("Acerque su tarjeta").
 *   3. RFID RC522  : se lee el UID. Se valida contra la lista local y,
 *                    en paralelo, se pregunta al backend (via Wemos).
 *   4. Autorizado  : ROJOS OFF + VERDES ON, LCD con nombre, se registra
 *                    la entrada/salida.  Denegado: ROJOS parpadean.
 *   5. Puestos     : cada puesto tiene HC-SR04 + LED RGB.
 *                    Libre  -> RGB VERDE
 *                    Ocupado-> RGB ROJO
 *   6. Matriz 8x8  : muestra en tiempo real la cantidad de puestos libres.
 *
 *  LIBRERIAS (Gestor de librerias del Arduino IDE)
 *  -----------------------------------------------
 *    - MFRC522            by GithubCommunity
 *    - LiquidCrystal I2C  by Frank de Brabander
 *    - LedControl         by Eberhard Fahle
 *
 *  PLACA:  Herramientas -> Placa -> "Arduino Mega or Mega 2560"
 *
 *  CONEXIONES  (revisadas: no hay conflicto de timers ni de buses)
 *  ---------------------------------------------------------------
 *   PIR OUT ........ D2   (INT4 - unica interrupcion externa libre)
 *   RC522 SDA/SS ... D53      RC522 SCK ..... D52   (SPI hardware)
 *   RC522 MOSI ..... D51      RC522 MISO .... D50
 *   RC522 RST ...... D5       RC522 VCC ..... 3.3V  (NUNCA 5V)
 *   LCD I2C SDA .... D20      LCD I2C SCL ... D21
 *   MAX7219 DIN .... D40      CS ... D42      CLK ... D44
 *   LED rojo ....... D6       LED verde ..... D7    (con R 220 ohm)
 *   HC-SR04 Trig ... D8       Echo .......... D9
 *   RGB puesto R ... D11(PWM) G ............. D10(PWM)
 *   Wemos: MEGA TX1(D18) -> Wemos RX   |   MEGA RX1(D19) <- Wemos TX
 *          (D18 lleva 5V: usar divisor 1k/2k antes del RX del Wemos)
 * =====================================================================
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <LedControl.h>

// ============================ PINES ==================================
#define PIN_PIR         2     // interrupcion externa
#define PIN_RFID_SS     53
#define PIN_RFID_RST    5
#define PIN_LED_ROJO    6
#define PIN_LED_VERDE   7
#define PIN_MAX_DIN     40
#define PIN_MAX_CS      42
#define PIN_MAX_CLK     44

// ---- Puestos de parqueo (para agregar otro puesto: sube NUM_PUESTOS
// ---- y añade sus pines en las 4 tablas de abajo) --------------------
#define NUM_PUESTOS     1
const uint8_t PIN_TRIG[NUM_PUESTOS]  = {  8 };
const uint8_t PIN_ECHO[NUM_PUESTOS]  = {  9 };
const uint8_t PIN_RGB_R[NUM_PUESTOS] = { 11 };   // PWM
const uint8_t PIN_RGB_G[NUM_PUESTOS] = { 10 };   // PWM

// LED RGB: 0 = catodo comun (el mas usado), 1 = anodo comun
#define RGB_ANODO_COMUN   0

// ========================== PARAMETROS ===============================
#define LCD_ADDR          0x27    // si la LCD no muestra nada, prueba 0x3F
#define LCD_COLS          16
#define LCD_ROWS          2

const uint8_t  UMBRAL_OCUPADO_CM = 15;   // < 15 cm => puesto ocupado
const uint8_t  MUESTRAS_ESTABLES = 3;    // lecturas iguales para cambiar
const uint8_t  BRILLO_MATRIZ     = 6;    // 0..15

const uint16_t T_BIENVENIDA   = 8000;    // LCD encendida tras el PIR
const uint16_t T_CONCEDIDO    = 5000;    // tiempo con LEDs verdes
const uint16_t T_DENEGADO     = 3000;    // tiempo de rechazo
const uint16_t T_MISMA_TARJETA= 2500;    // anti-rebote de la misma tarjeta
const uint16_t T_ESPERA_AUTH  = 1200;    // espera respuesta del backend
const uint16_t T_SONAR        = 150;     // periodo de medicion ultrasonica
const uint16_t T_TELEMETRIA   = 3000;    // envio periodico de estado
const uint16_t T_PARPADEO     = 250;     // parpadeo LEDs rojos (denegado)

// =================== TARJETAS AUTORIZADAS (respaldo local) ===========
// Se usan cuando el backend no responde. Deben coincidir con la tabla
// "cards" del backend. Cambia estos UIDs por los de tus tags reales
// (el UID leido se imprime en el Monitor Serie al pasar la tarjeta).
struct Tarjeta {
  const char* uid;
  const char* nombre;
};
const Tarjeta TARJETAS[] = {
  { "AB:CD:12:34", "Juan Perez"  },
  { "11:22:33:44", "Maria Lopez" },
  { "DE:AD:BE:EF", "Carlos Ruiz" },
};
const uint8_t NUM_TARJETAS = sizeof(TARJETAS) / sizeof(TARJETAS[0]);
bool tarjetaAdentro[NUM_TARJETAS] = { false };   // entrada/salida local

// ========================== OBJETOS ==================================
MFRC522           rfid(PIN_RFID_SS, PIN_RFID_RST);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
LedControl        matriz(PIN_MAX_DIN, PIN_MAX_CLK, PIN_MAX_CS, 1);

// ===================== ESTADO DEL SISTEMA ============================
enum Estado : uint8_t {
  EST_REPOSO,        // rojos fijos, LCD apagada
  EST_BIENVENIDA,    // PIR detecto movimiento, esperando tarjeta
  EST_ESPERA_AUTH,   // UID enviado, esperando veredicto del backend
  EST_CONCEDIDO,     // verdes encendidos
  EST_DENEGADO       // rojos parpadeando
};

Estado   estado          = EST_REPOSO;
uint32_t tEstado         = 0;      // millis() en que entramos al estado

volatile bool pirDisparado = false;         // lo escribe la ISR
bool     presencia       = false;

String   uidPendiente    = "";     // UID en proceso de autorizacion
int8_t   idxPendiente    = -1;     // indice en TARJETAS (-1 = desconocida)
String   ultimoUid       = "";
uint32_t tUltimoUid      = 0;

bool     puestoOcupado[NUM_PUESTOS] = { false };
uint8_t  contadorPuesto[NUM_PUESTOS] = { 0 };
uint8_t  librePrevio     = 255;    // fuerza el primer dibujado

uint32_t tSonar = 0, tTelemetria = 0, tParpadeo = 0;
bool     parpadeoOn = false;
bool     redOnline  = false;
String   lineaSerial1 = "";

// ============================ SETUP ==================================
void setup() {
  Serial.begin(115200);      // depuracion por USB
  Serial1.begin(9600);       // enlace con el Wemos D1 Mini

  // --- LEDs de entrada: rojo encendido por defecto ---
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  ledsEntrada(true, false);

  // --- Puestos: ultrasonico + RGB ---
  for (uint8_t i = 0; i < NUM_PUESTOS; i++) {
    pinMode(PIN_TRIG[i], OUTPUT);
    digitalWrite(PIN_TRIG[i], LOW);
    pinMode(PIN_ECHO[i], INPUT);
    pinMode(PIN_RGB_R[i], OUTPUT);
    pinMode(PIN_RGB_G[i], OUTPUT);
    rgbPuesto(i, false);                  // arranca en verde (libre)
  }

  // --- LCD I2C ---
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Parqueadero RFID"));
  lcd.setCursor(0, 1); lcd.print(F("Iniciando..."));

  // --- Matriz MAX7219 ---
  matriz.shutdown(0, false);
  matriz.setIntensity(0, BRILLO_MATRIZ);
  matriz.clearDisplay(0);

  // --- RFID por SPI hardware ---
  SPI.begin();
  rfid.PCD_Init();

  // --- PIR por interrupcion externa ---
  pinMode(PIN_PIR, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), isrPir, RISING);

  delay(1200);                            // el PIR se calibra solo
  dibujarMatriz(puestosLibres());
  irA(EST_REPOSO);

  Serial.println(F("MEGA listo. Esperando movimiento / tarjeta..."));
  enviarWemos(F("BOOT|mega"));
}

// ============================= LOOP ==================================
void loop() {
  atenderPir();
  atenderRfid();
  atenderSerial1();
  atenderSonar();
  atenderEstado();
  atenderTelemetria();
}

// ------------------- ISR del PIR (lo mas corta posible) --------------
void isrPir() {
  pirDisparado = true;
}

// ===================== BLOQUES DEL LOOP ==============================

// Al detectar movimiento se enciende la LCD con la bienvenida.
void atenderPir() {
  if (!pirDisparado) return;
  pirDisparado = false;
  presencia = true;

  enviarWemos(F("PIR|1"));

  // Si ya estamos atendiendo a alguien no interrumpimos el mensaje.
  if (estado == EST_REPOSO || estado == EST_BIENVENIDA) {
    irA(EST_BIENVENIDA);
  }
}

// Lectura del RC522 con anti-rebote de la misma tarjeta.
void atenderRfid() {
  if (estado == EST_ESPERA_AUTH) return;              // ya hay una en curso
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())  return;

  String uid = uidAString(&rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == ultimoUid && millis() - tUltimoUid < T_MISMA_TARJETA) return;
  ultimoUid  = uid;
  tUltimoUid = millis();

  Serial.print(F("UID leido: ")); Serial.println(uid);

  uidPendiente = uid;
  idxPendiente = buscarTarjeta(uid);

  enviarWemos("SCAN|" + uid);       // el backend registra y responde
  lcdTexto(F("Verificando..."), uid);
  irA(EST_ESPERA_AUTH);
}

// Respuestas del Wemos:  AUTH|1|mensaje   AUTH|0|mensaje   NET|1
void atenderSerial1() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (lineaSerial1.length()) procesarLinea(lineaSerial1);
      lineaSerial1 = "";
    } else if (lineaSerial1.length() < 80) {
      lineaSerial1 += c;
    }
  }
}

void procesarLinea(const String& linea) {
  if (linea.startsWith(F("AUTH|"))) {
    bool   ok  = (linea.charAt(5) == '1');
    String msg = linea.substring(linea.indexOf('|', 5) + 1);
    if (estado == EST_ESPERA_AUTH) resolverAcceso(ok, msg);
  }
  else if (linea.startsWith(F("NET|"))) {
    redOnline = (linea.charAt(4) == '1');
  }
}

// Medicion ultrasonica de cada puesto + LED RGB.
void atenderSonar() {
  if (millis() - tSonar < T_SONAR) return;
  tSonar = millis();

  for (uint8_t i = 0; i < NUM_PUESTOS; i++) {
    uint16_t cm = medirDistancia(i);
    bool ocupadoAhora = (cm > 0 && cm <= UMBRAL_OCUPADO_CM);

    // Se exige N lecturas seguidas iguales para evitar falsos positivos.
    if (ocupadoAhora != puestoOcupado[i]) {
      if (++contadorPuesto[i] >= MUESTRAS_ESTABLES) {
        contadorPuesto[i] = 0;
        puestoOcupado[i]  = ocupadoAhora;
        rgbPuesto(i, ocupadoAhora);
        Serial.print(F("Puesto ")); Serial.print(i + 1);
        Serial.println(ocupadoAhora ? F(": OCUPADO") : F(": LIBRE"));
        enviarWemos("SLOT|" + String(i + 1) + "|" + (ocupadoAhora ? "1" : "0"));
        enviarEstado();
      }
    } else {
      contadorPuesto[i] = 0;
    }
  }

  uint8_t libres = puestosLibres();
  if (libres != librePrevio) dibujarMatriz(libres);
}

// Maquina de estados: LEDs, LCD y tiempos.
void atenderEstado() {
  uint32_t t = millis() - tEstado;

  switch (estado) {

    case EST_REPOSO:
      break;                                  // rojos fijos, nada que hacer

    case EST_BIENVENIDA:
      if (t > T_BIENVENIDA) { presencia = false; irA(EST_REPOSO); }
      break;

    case EST_ESPERA_AUTH:
      // Si el backend no contesta, decide el MEGA con su lista local.
      if (t > T_ESPERA_AUTH) {
        bool ok = (idxPendiente >= 0);
        resolverAcceso(ok, ok ? mensajeLocal(idxPendiente)
                              : String(F("Tarjeta invalida")));
      }
      break;

    case EST_CONCEDIDO:
      if (t > T_CONCEDIDO) { presencia = false; irA(EST_REPOSO); }
      break;

    case EST_DENEGADO:
      if (millis() - tParpadeo > T_PARPADEO) {   // rojos intermitentes
        tParpadeo = millis();
        parpadeoOn = !parpadeoOn;
        ledsEntrada(parpadeoOn, false);
      }
      if (t > T_DENEGADO) { presencia = false; irA(EST_REPOSO); }
      break;
  }
}

// Latido de estado hacia el backend (puestos libres, presencia).
void atenderTelemetria() {
  if (millis() - tTelemetria < T_TELEMETRIA) return;
  enviarEstado();
}

// ===================== ACCIONES DE ACCESO ============================

void resolverAcceso(bool concedido, const String& mensaje) {
  if (concedido) {
    if (idxPendiente >= 0) {                 // alterna entrada / salida
      tarjetaAdentro[idxPendiente] = !tarjetaAdentro[idxPendiente];
    }
    ledsEntrada(false, true);                // rojos OFF, verdes ON
    lcdTexto(F("Acceso concedido"), mensaje.length() ? mensaje
                                                     : String(F("Adelante")));
    Serial.print(F("ACCESO OK   ")); Serial.print(uidPendiente);
    Serial.print(F(" -> ")); Serial.println(mensaje);
    irA(EST_CONCEDIDO);
  } else {
    ledsEntrada(true, false);
    lcdTexto(F("Acceso denegado"), mensaje.length() ? mensaje
                                                    : String(F("No registrada")));
    Serial.print(F("DENEGADO    ")); Serial.print(uidPendiente);
    Serial.print(F(" -> ")); Serial.println(mensaje);
    irA(EST_DENEGADO);
  }
  uidPendiente = "";
  idxPendiente = -1;
}

// Se llama ANTES de conmutar tarjetaAdentro: si estaba dentro, sale.
String mensajeLocal(uint8_t idx) {
  return String(tarjetaAdentro[idx] ? F("Hasta luego ") : F("Bienvenido "))
       + TARJETAS[idx].nombre;
}

int8_t buscarTarjeta(const String& uid) {
  for (uint8_t i = 0; i < NUM_TARJETAS; i++) {
    if (uid.equalsIgnoreCase(TARJETAS[i].uid)) return (int8_t)i;
  }
  return -1;
}

// ===================== CAMBIO DE ESTADO ==============================

void irA(Estado nuevo) {
  estado  = nuevo;
  tEstado = millis();

  switch (nuevo) {
    case EST_REPOSO:
      ledsEntrada(true, false);
      lcd.clear();
      lcd.noBacklight();                     // LCD "apagada" en reposo
      break;

    case EST_BIENVENIDA:
      // Si el Wemos no reporta red, se avisa que se valida en modo local.
      lcdTexto(F("Bienvenido!"), redOnline ? F("Acerque su tag")
                                          : F("Sin red: local"));
      break;

    case EST_ESPERA_AUTH:
    case EST_CONCEDIDO:
    case EST_DENEGADO:
      break;                                 // el texto ya se escribio
  }
}

// ===================== PERIFERICOS ===================================

void ledsEntrada(bool rojo, bool verde) {
  digitalWrite(PIN_LED_ROJO,  rojo  ? HIGH : LOW);
  digitalWrite(PIN_LED_VERDE, verde ? HIGH : LOW);
}

// ocupado = true -> rojo ; false -> verde
void rgbPuesto(uint8_t i, bool ocupado) {
  uint8_t r = ocupado ? 255 : 0;
  uint8_t g = ocupado ? 0   : 255;
#if RGB_ANODO_COMUN
  r = 255 - r;  g = 255 - g;
#endif
  analogWrite(PIN_RGB_R[i], r);
  analogWrite(PIN_RGB_G[i], g);
}

uint16_t medirDistancia(uint8_t i) {
  digitalWrite(PIN_TRIG[i], LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG[i], LOW);

  uint32_t us = pulseIn(PIN_ECHO[i], HIGH, 25000UL);   // ~4 m maximo
  if (us == 0) return 0;                               // fuera de rango
  return (uint16_t)(us / 58);                          // us -> cm
}

uint8_t puestosLibres() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_PUESTOS; i++) if (!puestoOcupado[i]) n++;
  return n;
}

void lcdTexto(const String& l1, const String& l2) {
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1.substring(0, LCD_COLS));
  lcd.setCursor(0, 1); lcd.print(l2.substring(0, LCD_COLS));
}

// ================== MATRIZ 8x8: digito de puestos libres =============
// Fuente 5x7 (5 bits utiles por fila), se centra en las columnas 1..5.
const uint8_t DIGITOS[10][7] PROGMEM = {
  {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // 0
  {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 1
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},  // 2
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},  // 3
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},  // 4
  {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 5
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},  // 6
  {0x1F,0x01,0x02,0x04,0x04,0x04,0x04},  // 7
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 8
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},  // 9
};

void dibujarMatriz(uint8_t libres) {
  librePrevio = libres;
  uint8_t d = (libres > 9) ? 9 : libres;      // la matriz muestra 1 digito
  for (uint8_t fila = 0; fila < 7; fila++) {
    uint8_t bits = pgm_read_byte(&DIGITOS[d][fila]);
    matriz.setRow(0, fila, (uint8_t)(bits << 2));
  }
  matriz.setRow(0, 7, 0x00);
}

// ===================== ENLACE CON EL WEMOS ===========================

void enviarWemos(const String& linea) {
  Serial1.println(linea);
}

// STATE|libres|total|mascaraOcupacion|presencia
void enviarEstado() {
  tTelemetria = millis();
  String mascara = "";
  for (uint8_t i = 0; i < NUM_PUESTOS; i++) mascara += puestoOcupado[i] ? '1' : '0';
  enviarWemos("STATE|" + String(puestosLibres()) + "|" + String(NUM_PUESTOS) +
              "|" + mascara + "|" + (presencia ? "1" : "0"));
}

// UID en formato "AB:CD:12:34" (igual que en la tabla cards del backend)
String uidAString(MFRC522::Uid* uid) {
  String s = "";
  for (uint8_t i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) s += '0';
    s += String(uid->uidByte[i], HEX);
    if (i < uid->size - 1) s += ':';
  }
  s.toUpperCase();
  return s;
}
