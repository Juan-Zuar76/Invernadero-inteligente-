#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =====================================================
// CREDENCIALES
// =====================================================
const char* WIFI_NOMBRE = "Posada-Martinez1";
const char* WIFI_CLAVE  = "Baronch0y0limpia#";
const char* API_KEY     = "PH92IS1FKGTJAIN0";
const char* DEVICE_TOKEN = "08b5b90262e58a27f23e890d459b8949bcce044095b132062bb95100bac127ed";

// =====================================================
// IOTERIS MQTT SOBRE TLS
// =====================================================
const char* MQTT_HOST = "ioteris.com.ar";
const uint16_t MQTT_PORT = 8883;

WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

String topicDatos;
String topicComandos;

// =====================================================
// SENSORES
// =====================================================
#define PIN_DHT D1
#define TIPO_DHT DHT11
#define PIN_SUELO A0
#define PIN_LUZ D0

DHT dht(PIN_DHT, TIPO_DHT);

// Calibracion FC-28 (valores originales)
const int VALOR_SECO = 1024;
const int VALOR_HUMEDO = 391;

// =====================================================
// ACTUADORES
// =====================================================
#define PIN_VENTILADOR D5
#define PIN_BOMBA D6

// =====================================================
// UMBRALES AUTOMATICOS (se conservan los originales)
// =====================================================
const float TEMP_ENCENDER = 33.0;
const float TEMP_APAGAR = 24.0;
const int SUELO_ENCENDER = 40;
const int SUELO_APAGAR = 40;

// =====================================================
// TEMPORIZACIONES
// =====================================================
const unsigned long INTERVALO_LECTURA = 2000UL;
const unsigned long INTERVALO_ENVIO_THINGSPEAK = 20000UL;
const unsigned long INTERVALO_WIFI = 10000UL;
const unsigned long INTERVALO_MQTT = 5000UL;

unsigned long ultimaLectura = 0;
unsigned long ultimoEnvioThingSpeak = 0;
unsigned long ultimoIntentoWiFi = 0;
unsigned long ultimoIntentoMQTT = 0;

// =====================================================
// VARIABLES DEL SISTEMA
// =====================================================
bool modoAutomatico = true;
bool ventiladorEncendido = false;
bool bombaEncendida = false;

float temperatura = NAN;
float humedadAire = NAN;
int humedadSuelo = 0;
int lecturaSuelo = 0;
int luzPorcentaje = 0;

// =====================================================
// CONTROL DE SALIDAS
// =====================================================
void apagarVentilador() {
  digitalWrite(PIN_VENTILADOR, LOW);
  ventiladorEncendido = false;
}

void encenderVentilador() {
  digitalWrite(PIN_VENTILADOR, HIGH);
  ventiladorEncendido = true;
}

void apagarBomba() {
  digitalWrite(PIN_BOMBA, LOW);
  bombaEncendida = false;
}

void encenderBomba() {
  digitalWrite(PIN_BOMBA, HIGH);
  bombaEncendida = true;
}

void apagarTodo() {
  apagarVentilador();
  apagarBomba();
}

// =====================================================
// CAMBIO DE MODO
// =====================================================
void activarManual() {
  apagarTodo();
  modoAutomatico = false;
  Serial.println();
  Serial.println("MODO MANUAL ACTIVADO. Actuadores apagados.");
}

void activarAutomatico() {
  apagarTodo();
  modoAutomatico = true;
  Serial.println();
  Serial.println("MODO AUTOMATICO ACTIVADO. Actuadores apagados.");
  Serial.println("Esperando lecturas para aplicar el control automatico.");
}

// =====================================================
// CONTROL AUTOMATICO (funcionamiento local, sin Internet)
// =====================================================
void controlarAutomaticamente() {
  if (!isnan(temperatura)) {
    if (temperatura >= TEMP_ENCENDER && !ventiladorEncendido) {
      encenderVentilador();
      Serial.println("AUTO: VENTILADOR ENCENDIDO");
    }
    if (temperatura <= TEMP_APAGAR && ventiladorEncendido) {
      apagarVentilador();
      Serial.println("AUTO: VENTILADOR APAGADO");
    }
  }

  if (humedadSuelo < SUELO_ENCENDER && !bombaEncendida) {
    encenderBomba();
    Serial.println("AUTO: BOMBA ENCENDIDA");
  }
    if (humedadSuelo >= SUELO_APAGAR && bombaEncendida) {
    apagarBomba();
    Serial.println("AUTO: BOMBA APAGADA");
  }
}

// =====================================================
// COMANDOS POR MONITOR SERIE (se conservan)
// =====================================================
void procesarComandoSerie() {
  if (Serial.available() <= 0) return;

  char comando = Serial.read();
  if (comando == '\n' || comando == '\r') return;

  switch (comando) {
    case 'M': activarManual(); break;
    case 'A': activarAutomatico(); break;
    case 'V':
      if (!modoAutomatico) {
        encenderVentilador();
        Serial.println("MANUAL: VENTILADOR ENCENDIDO");
      } else Serial.println("Cambie a modo M para control manual.");
      break;
    case 'v':
      if (!modoAutomatico) {
        apagarVentilador();
        Serial.println("MANUAL: VENTILADOR APAGADO");
      } else Serial.println("Cambie a modo M para control manual.");
      break;
    case 'B':
      if (!modoAutomatico) {
        encenderBomba();
        Serial.println("MANUAL: BOMBA ENCENDIDA");
      } else Serial.println("Cambie a modo M para control manual.");
      break;
    case 'b':
      if (!modoAutomatico) {
        apagarBomba();
        Serial.println("MANUAL: BOMBA APAGADA");
      } else Serial.println("Cambie a modo M para control manual.");
      break;
    case 'X':
      activarManual();
      Serial.println("APAGADO GENERAL");
      break;
    default:
      Serial.println("Comando no reconocido.");
      break;
  }
}

// =====================================================
// RECEPCION DE COMANDOS MQTT DESDE IOTERIS
// Switch bomba:      {"bomba": true/false}
// Switch ventilador: {"ventilador": true/false}
// Switch de modo:    {"manual": true/false}
// manual=true  -> MANUAL
// manual=false -> AUTOMATICO
// =====================================================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String mensaje;
  mensaje.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  Serial.print("MQTT recibido [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(mensaje);

  StaticJsonDocument<192> doc;
  DeserializationError error = deserializeJson(doc, mensaje);
  if (error) {
    Serial.print("Error leyendo JSON MQTT: ");
    Serial.println(error.c_str());
    return;
  }

  // El control de modo se procesa antes que los actuadores.
  if (doc.containsKey("manual")) {
    bool manualSolicitado = doc["manual"].as<bool>();
    if (manualSolicitado && modoAutomatico) activarManual();
    if (!manualSolicitado && !modoAutomatico) activarAutomatico();
  }

  // Nunca aceptar órdenes de actuadores si estamos en AUTO.
  if (modoAutomatico) {
    if (doc.containsKey("bomba") || doc.containsKey("ventilador")) {
      Serial.println("Orden de actuador ignorada: el sistema esta en AUTO.");
    }
    return;
  }

  if (doc.containsKey("bomba")) {
    bool estado = doc["bomba"].as<bool>();
    if (estado) encenderBomba(); else apagarBomba();
    Serial.println(estado ? "MQTT: BOMBA ENCENDIDA" : "MQTT: BOMBA APAGADA");
  }

  if (doc.containsKey("ventilador")) {
    bool estado = doc["ventilador"].as<bool>();
    if (estado) encenderVentilador(); else apagarVentilador();
    Serial.println(estado ? "MQTT: VENTILADOR ENCENDIDO" : "MQTT: VENTILADOR APAGADO");
  }
}

// =====================================================
// CONEXION WIFI NO BLOQUEANTE
// =====================================================
void gestionarWiFi(unsigned long ahora) {
  if (WiFi.status() == WL_CONNECTED) return;

  if (ahora - ultimoIntentoWiFi >= INTERVALO_WIFI) {
    ultimoIntentoWiFi = ahora;
    Serial.println("WiFi desconectado. Intentando reconectar sin detener el control local...");
    WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  }
}

// =====================================================
// CONEXION MQTT NO BLOQUEANTE ENTRE INTENTOS
// =====================================================
void gestionarMQTT(unsigned long ahora) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (ahora - ultimoIntentoMQTT < INTERVALO_MQTT) return;

  ultimoIntentoMQTT = ahora;
  Serial.print("Conectando a Ioteris MQTT... ");

  // Ioteris documenta el token como usuario y contraseña.
  if (mqttClient.connect("RebelionBotanica-ESP8266", DEVICE_TOKEN, DEVICE_TOKEN)) {
    Serial.println("conectado.");
    if (mqttClient.subscribe(topicComandos.c_str())) {
      Serial.print("Suscripto a: ");
      Serial.println(topicComandos);
    } else {
      Serial.println("No se pudo suscribir al topic de comandos.");
      mqttClient.disconnect();
    }
  } else {
    Serial.print("fallo, estado MQTT: ");
    Serial.println(mqttClient.state());
  }
}

// =====================================================
// LECTURA DE SENSORES
// =====================================================
void leerSensores() {
  lecturaSuelo = analogRead(PIN_SUELO);
  humedadSuelo = map(lecturaSuelo, VALOR_SECO, VALOR_HUMEDO, 0, 100);
  humedadSuelo = constrain(humedadSuelo, 0, 100);

  temperatura = dht.readTemperature();
  humedadAire = dht.readHumidity();

  int estadoLuz = digitalRead(PIN_LUZ);
  luzPorcentaje = (estadoLuz == LOW) ? 100 : 0;

  Serial.println();
  Serial.println("======= LECTURAS =======");
  Serial.print("Temperatura: ");
  if (isnan(temperatura)) Serial.println("Error");
  else { Serial.print(temperatura); Serial.println(" C"); }

  Serial.print("Humedad aire: ");
  if (isnan(humedadAire)) Serial.println("Error");
  else { Serial.print(humedadAire); Serial.println("%"); }

  Serial.print("Humedad suelo: "); Serial.print(humedadSuelo); Serial.println("%");
  Serial.print("ADC suelo: "); Serial.println(lecturaSuelo);
  Serial.print("Luz: "); Serial.print(luzPorcentaje); Serial.println("%");
  Serial.print("Modo: "); Serial.println(modoAutomatico ? "AUTOMATICO" : "MANUAL");
  Serial.print("Ventilador: "); Serial.println(ventiladorEncendido ? "ENCENDIDO" : "APAGADO");
  Serial.print("Bomba: "); Serial.println(bombaEncendida ? "ENCENDIDA" : "APAGADA");

  if (modoAutomatico) controlarAutomaticamente();
}

// =====================================================
// ENVIO DE TELEMETRIA A IOTERIS POR MQTT
// Las variables se crean automaticamente al recibir el primer JSON.
// =====================================================
void enviarIoteris() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  if (!isnan(temperatura)) doc["temperatura"] = temperatura;
  if (!isnan(humedadAire)) doc["humedad"] = humedadAire;
  doc["humedad_suelo"] = humedadSuelo;
  doc["iluminacion"] = luzPorcentaje;
  doc["adc_suelo"] = lecturaSuelo;
  doc["modo"] = modoAutomatico ? "automatico" : "manual";
  doc["ventilador"] = ventiladorEncendido;
  doc["bomba"] = bombaEncendida;

  String payload;
  serializeJson(doc, payload);
  bool publicado = mqttClient.publish(topicDatos.c_str(), payload.c_str());

  Serial.print("Ioteris MQTT: ");
  Serial.println(publicado ? "telemetria publicada." : "fallo al publicar telemetria.");
}

// =====================================================
// ENVIO A THINGSPEAK (cada 20 segundos)
// =====================================================
void enviarThingSpeak() {
  if (isnan(temperatura) || isnan(humedadAire)) {
    Serial.println("DHT11 invalido. No se envian datos a ThingSpeak.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ThingSpeak omitido: WiFi desconectado.");
    return;
  }

  if (String(API_KEY) == "PEGAR_AQUI_NUEVA_WRITE_API_KEY" || strlen(API_KEY) == 0) {
    Serial.println("Falta completar la nueva Write API Key de ThingSpeak.");
    return;
  }

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure(); // Igual que el programa original; validar CA seria mejora posterior.

  HTTPClient https;
  https.setTimeout(4000);

  String url = "https://api.thingspeak.com/update?api_key=";
  url += API_KEY;
  url += "&field1=" + String(temperatura, 1);
  url += "&field2=" + String(humedadAire, 1);
  url += "&field3=" + String(humedadSuelo);
  url += "&field4=" + String(luzPorcentaje);
  url += "&field5=" + String(lecturaSuelo);

  if (https.begin(clienteHTTPS, url)) {
    int codigoHTTP = https.GET();
    if (codigoHTTP > 0) {
      String respuesta = https.getString();
      respuesta.trim();
      Serial.print("Respuesta ThingSpeak: "); Serial.println(respuesta);
      if (respuesta != "0") Serial.println("Datos enviados correctamente.");
      else Serial.println("ThingSpeak no acepto el registro.");
    } else {
      Serial.print("Error HTTP: "); Serial.println(codigoHTTP);
    }
    https.end();
  } else {
    Serial.println("No se pudo iniciar HTTPS hacia ThingSpeak.");
  }
}

// =====================================================
// CONFIGURACION INICIAL
// =====================================================
void setup() {
  Serial.begin(115200);

  // Inicializar salidas apagadas, conservando la logica original.
  digitalWrite(PIN_VENTILADOR, LOW);
  digitalWrite(PIN_BOMBA, LOW);
  pinMode(PIN_VENTILADOR, OUTPUT);
  pinMode(PIN_BOMBA, OUTPUT);
  apagarTodo();

  dht.begin();
  pinMode(PIN_LUZ, INPUT);

  Serial.println();
  Serial.println("================================");
  Serial.println("    REBELION BOTANICA - IOT");
  Serial.println("================================");
  Serial.println("Actuadores inicialmente apagados.");
  Serial.println("Modo inicial: AUTOMATICO");
  Serial.println("AUTO funciona localmente aunque no haya Internet.");

  Serial.println();
  Serial.println("COMANDOS SERIE:");
  Serial.println("A = Modo automatico");
  Serial.println("M = Modo manual");
  Serial.println("V/v = Ventilador ON/OFF (manual)");
  Serial.println("B/b = Bomba ON/OFF (manual)");
  Serial.println("X = Apagar todo y pasar a manual");

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  ultimoIntentoWiFi = millis();

  topicDatos = String("devices/") + DEVICE_TOKEN + "/data";
  topicComandos = String("devices/") + DEVICE_TOKEN + "/cmd";

  // La documentacion de Ioteris muestra TLS en 8883.
  // setInsecure() permite la prueba inicial sin instalar CA raiz;
  // para despliegue definitivo conviene validar el certificado del broker.
  mqttSecureClient.setInsecure();
  mqttSecureClient.setTimeout(2000);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(callbackMQTT);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(2);

  Serial.println("WiFi iniciado. El programa no espera bloqueado la conexion.");
  Serial.println("Completá la nueva API Key y el token Ioteris antes de probar.");
}

// =====================================================
// BUCLE PRINCIPAL
// =====================================================
void loop() {
  unsigned long ahora = millis();

  procesarComandoSerie();
  gestionarWiFi(ahora);
  gestionarMQTT(ahora);

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  // Seguridad solicitada: en MANUAL, si se pierde WiFi o MQTT,
  // apagar salidas. Al reconectar NO restaurar estados anteriores.
  // El modo sigue siendo MANUAL y espera nuevos comandos.
  if (!modoAutomatico &&
      (WiFi.status() != WL_CONNECTED || !mqttClient.connected())) {
    if (ventiladorEncendido || bombaEncendida) {
      apagarTodo();
      Serial.println("SEGURIDAD: conexion perdida en MANUAL. Actuadores apagados.");
      Serial.println("Al reconectar se esperan nuevas ordenes MQTT.");
    }
  }

  // Lectura local cada 2 segundos, haya Internet o no.
  ahora = millis();
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;
    leerSensores();
  }

  // Publicacion en ThingSpeak cada 20 segundos si hay conectividad.
  ahora = millis();
  if (ahora - ultimoEnvioThingSpeak >= INTERVALO_ENVIO_THINGSPEAK) {
    ultimoEnvioThingSpeak = ahora;
    enviarIoteris();
    enviarThingSpeak();
  }

  yield();
}
