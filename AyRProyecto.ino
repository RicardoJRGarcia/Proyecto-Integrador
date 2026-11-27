#include <WiFi.h>
#include <ESP32Servo.h>

const char* SSID = "INFINITUMD3AE";
const char* PASSWORD = "fgnUMZEA9x";
const uint16_t PUERTO = 8080;

const int PIN_BOTON_PARO = 4;   // INPUT_PULLUP (LOW = presionado)
const int PIN_SERVO_PUERTA = 13; // PWM: 0° cerrada, 90° abierta
const int PIN_LED_VERDE = 25;    // Puerta abierta / ACTIVE
const int PIN_LED_ROJO = 26;     // SAFE o ERROR

const unsigned long TIMEOUT_MS = 3000; // Fail-Safe tras 3 s sin mensajes

enum EstadoFSM {
  IDLE,
  ACTIVE,
  SAFE,
  ERROR
};

EstadoFSM estadoActual = IDLE;
unsigned long ultimoMensajeMs = 0;
String bufferRx = "";

WiFiServer servidor(PUERTO);
WiFiClient cliente;
Servo servoPuerta;

void actualizarHardware() {
  switch (estadoActual) {
    case IDLE:
      servoPuerta.write(0);
      digitalWrite(PIN_LED_VERDE, LOW);
      digitalWrite(PIN_LED_ROJO, LOW);
      break;
    case ACTIVE:
      servoPuerta.write(90);
      digitalWrite(PIN_LED_VERDE, HIGH);
      digitalWrite(PIN_LED_ROJO, LOW);
      break;
    case SAFE:
      servoPuerta.write(0);
      digitalWrite(PIN_LED_VERDE, LOW);
      digitalWrite(PIN_LED_ROJO, HIGH);
      break;
    case ERROR:
      servoPuerta.write(0);
      digitalWrite(PIN_LED_VERDE, LOW);
      digitalWrite(PIN_LED_ROJO, HIGH);
      break;
  }
}

void enviarACK(const char* trama) {
  if (cliente && cliente.connected()) {
    cliente.print(trama);
    cliente.print("\n");
  }
}

void manejarComando(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  ultimoMensajeMs = millis();

  if (cmd == "CMD:PING") {
    return;
  }

  if (cmd == "CMD:SAFE") {
    estadoActual = SAFE;
    actualizarHardware();
    enviarACK("ACK:SAFE");
  } else if (cmd == "CMD:RESET") {
    if (digitalRead(PIN_BOTON_PARO) == HIGH && WiFi.status() == WL_CONNECTED) {
      estadoActual = IDLE;
      actualizarHardware();
      enviarACK("ACK:IDLE");
    }
  } else if (estadoActual == SAFE || estadoActual == ERROR) {
    enviarACK("ACK:BLOCKED");
  } else if (cmd == "CMD:OPEN") {
    estadoActual = ACTIVE;
    actualizarHardware();
    enviarACK("ACK:OPEN");
  } else if (cmd == "CMD:CLOSE") {
    estadoActual = IDLE;
    actualizarHardware();
    enviarACK("ACK:CLOSE");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOTON_PARO, INPUT_PULLUP);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);

  servoPuerta.attach(PIN_SERVO_PUERTA);
  estadoActual = IDLE;
  actualizarHardware();

  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
  servidor.begin();
  ultimoMensajeMs = millis();

  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Paro físico inmediato
  if (digitalRead(PIN_BOTON_PARO) == LOW) {
    estadoActual = SAFE;
    actualizarHardware();
  }

  if (!cliente || !cliente.connected()) {
    cliente = servidor.available();
  }

  // Lectura de tramas delimitadas por '\n'
  if (cliente && cliente.connected()) {
    while (cliente.available()) {
      char c = cliente.read();
      if (c == '\n') {
        manejarComando(bufferRx);
        bufferRx = "";
      } else if (c != '\r') {
        bufferRx += c;
      }
    }
  }

  // Fail-Safe por timeout con millis()
  if ((estadoActual == IDLE || estadoActual == ACTIVE) &&
      (millis() - ultimoMensajeMs > TIMEOUT_MS)) {
    estadoActual = ERROR;
    actualizarHardware();
    enviarACK("ERR:TIMEOUT");
  }

  delay(10);
}