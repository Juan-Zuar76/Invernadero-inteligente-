
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <DHT.h>

// ===== DATOS WIFI Y THINGSPEAK =====
const char* WIFI_NOMBRE = "Posada-Martinez1";
const char* WIFI_CLAVE  = "Baronch0y0limpia#";
const char* API_KEY     = "PUM1GTXR6C6059C0";

// ===== SENSORES =====
#define PIN_DHT D1
#define TIPO_DHT DHT11
#define PIN_SUELO A0
#define PIN_LUZ D0

// ===== ACTUADORES =====
#define PIN_VENTILADOR D5
#define PIN_BOMBA D6

DHT dht(PIN_DHT, TIPO_DHT);

// Calibracion FC-28
const int VALOR_SECO = 1024;
const int VALOR_HUMEDO = 391;

// ThingSpeak: envio cada 20 segundos
const unsigned long INTERVALO_ENVIO = 20000;
unsigned long ultimoEnvio = 0;

// ===== CONTROL DE ACTUADORES =====

void encenderVentilador() {
  digitalWrite(PIN_VENTILADOR, HIGH);
  Serial.println("VENTILADOR ENCENDIDO");
}

void apagarVentilador() {
  digitalWrite(PIN_VENTILADOR, LOW);
  Serial.println("VENTILADOR APAGADO");
}

void encenderBomba() {
  digitalWrite(PIN_BOMBA, HIGH);
  Serial.println("BOMBA ENCENDIDA");
}

void apagarBomba() {
  digitalWrite(PIN_BOMBA, LOW);
  Serial.println("BOMBA APAGADA");
}

void apagarTodo() {
  apagarVentilador();
  apagarBomba();
}

// ===== CONFIGURACION =====

void setup() {
  Serial.begin(115200);

  // Estado inicial de las salidas
  digitalWrite(PIN_VENTILADOR, LOW);
  digitalWrite(PIN_BOMBA, LOW);

  pinMode(PIN_VENTILADOR, OUTPUT);
  pinMode(PIN_BOMBA, OUTPUT);

  dht.begin();
  pinMode(PIN_LUZ, INPUT);

  Serial.println();
  Serial.println("=== INVERNADERO INTELIGENTE ===");
  Serial.println("Sensores y actuadores iniciados");

  Serial.println();
  Serial.println("COMANDOS POR MONITOR SERIE:");
  Serial.println("V = Encender ventilador");
  Serial.println("v = Apagar ventilador");
  Serial.println("B = Encender bomba");
  Serial.println("b = Apagar bomba");
  Serial.println("X = Apagar ambos actuadores");

  // Conexion WiFi
  Serial.print("Conectando al WiFi ");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectado");
  Serial.print("IP local: ");
  Serial.println(WiFi.localIP());
}

// ===== LOOP PRINCIPAL =====

void loop() {

  // CONTROL MANUAL POR MONITOR SERIE
  if (Serial.available() > 0) {

    char comando = Serial.read();

    switch (comando) {

      case 'V':
        encenderVentilador();
        break;

      case 'v':
        apagarVentilador();
        break;

      case 'B':
        encenderBomba();
        break;

      case 'b':
        apagarBomba();
        break;

      case 'X':
        apagarTodo();
        break;
    }
  }

  // ENVIO PERIODICO A THINGSPEAK
  if (millis() - ultimoEnvio < INTERVALO_ENVIO) {
    delay(10);
    return;
  }

  ultimoEnvio = millis();

  // LECTURA SUELO
  int lecturaSuelo = analogRead(PIN_SUELO);

  int humedadSuelo = map(
    lecturaSuelo,
    VALOR_SECO,
    VALOR_HUMEDO,
    0,
    100
  );

  humedadSuelo = constrain(humedadSuelo, 0, 100);

  // LECTURA DHT11
  float temperatura = dht.readTemperature();
  float humedadAire = dht.readHumidity();

  // LECTURA LUZ
  int estadoLuz = digitalRead(PIN_LUZ);
  int luzPorcentaje = (estadoLuz == LOW) ? 100 : 0;

  // MOSTRAR DATOS
  Serial.println();
  Serial.println("--- Lecturas ---");

  Serial.print("Suelo: ");
  Serial.print(humedadSuelo);
  Serial.println("%");

  Serial.print("ADC suelo: ");
  Serial.println(lecturaSuelo);

  if (isnan(temperatura) || isnan(humedadAire)) {
    Serial.println("Error leyendo DHT11. No se envian datos.");
    return;
  }

  Serial.print("Temperatura: ");
  Serial.print(temperatura);
  Serial.println(" C");

  Serial.print("Humedad del aire: ");
  Serial.print(humedadAire);
  Serial.println("%");

  Serial.print("Luz digital estimada: ");
  Serial.print(luzPorcentaje);
  Serial.println("%");

  // VERIFICAR WIFI
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Reintentando...");
    WiFi.reconnect();
    return;
  }

  // ENVIO HTTPS A THINGSPEAK
  WiFiClientSecure cliente;
  cliente.setInsecure();

  HTTPClient https;

  String url = "https://api.thingspeak.com/update?api_key=";
  url += API_KEY;
  url += "&field1=" + String(temperatura, 1);
  url += "&field2=" + String(humedadAire, 1);
  url += "&field3=" + String(humedadSuelo);
  url += "&field4=" + String(luzPorcentaje);
  url += "&field5=" + String(lecturaSuelo);

  if (https.begin(cliente, url)) {

    int codigoHTTP = https.GET();

    if (codigoHTTP > 0) {

      String respuesta = https.getString();
      respuesta.trim();

      Serial.print("Respuesta ThingSpeak: ");
      Serial.println(respuesta);

      if (respuesta != "0") {
        Serial.println("Datos enviados correctamente.");
      } else {
        Serial.println("ThingSpeak no acepto el registro.");
      }

    } else {

      Serial.print("Error HTTP: ");
      Serial.println(codigoHTTP);
    }

    https.end();

  } else {

    Serial.println("No se pudo iniciar la conexion HTTPS.");
  }
}
