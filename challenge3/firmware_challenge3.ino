#include <Wire.h>
  #include <math.h>
  #include <Adafruit_BME280.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>

  #include <WiFi.h>
  #include <ESPAsyncWebServer.h>

// MQTT
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>          // NUEVO: buscar la Raspberry Pi por nombre

// ============================================================
// CREDENCIALES: escribe aqui los valores reales ANTES de compilar.
// No subas este archivo a GitHub con las claves puestas.
// ============================================================
#define WIFI_SSID           "<SSID_DEL_SITIO>"
#define WIFI_PASS           "********"
#define WEB_USER            "********"
#define WEB_PASS            "********"
#define MQTT_USUARIO_ESP32  "esp32"
#define MQTT_CLAVE_ESP32    "********"

  // NUEVO (registro en flash): sistema de archivos, contador de reinicios y hora
  #include <LittleFS.h>
  #include <Preferences.h>
  #include <time.h>

  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>

  // ============================================================
  // WIFI / SERVIDOR
  // ============================================================

  // Las credenciales vienen de los #define de arriba (en la Wiki van ocultas con ********).
  const char* ssid       = WIFI_SSID;
  const char* password   = WIFI_PASS;
  const char* authUser   = WEB_USER;
  const char* authPass   = WEB_PASS;

  // NUEVO: el broker es el Mosquitto de la Raspberry Pi. Se busca por su
  // nombre en la red (mDNS: gateway-hidrico.local) en cada reconexion, asi
  // que funciona aunque el hotspot le cambie la IP. Si no aparece, se usa
  // la IP de respaldo.
  const char* MQTT_HOSTNAME = "gateway-hidrico";            // sin ".local"
  const IPAddress MQTT_IP_RESPALDO(10, 226, 141, 58);       // ultima IP conocida
  const uint16_t MQTT_PORT = 1883;
  const char* MQTT_USER = MQTT_USUARIO_ESP32;
  const char* MQTT_PASS = MQTT_CLAVE_ESP32;

  const char* MQTT_TOPIC_TELEMETRIA = "sabana/hidrico/nodo01/telemetria";
  const char* MQTT_TOPIC_ESTADO = "sabana/hidrico/nodo01/estado_dispositivo";
  const char* MQTT_TOPIC_COMANDO = "sabana/hidrico/nodo01/cmd/silenciar";
  const unsigned long MQTT_INTERVALO_MS = 10000UL;

  AsyncWebServer server(80);

  WiFiClient mqttWifiClient;
  PubSubClient mqttClient(mqttWifiClient);
  uint32_t secuenciaMQTT = 0;
  unsigned long ultimaPublicacionMQTT = 0;

  // ============================================================
  // PINES
  // ============================================================

  #define SDA_PIN 21
  #define SCL_PIN 22

  #define TRIG_PIN 5
  #define ECHO_PIN 18

  #define LED_ROJO 25
  #define LED_AMARILLO 26
  #define LED_VERDE 27

  #define LDR_PIN 34
  #define BUZZER_PIN 14

  #define SCREEN_WIDTH 128
  #define SCREEN_HEIGHT 64

  // ============================================================
  // SENSORES
  // ============================================================

  Adafruit_BME280 bme;

  Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    &Wire,
    -1
  );

  // ============================================================
  // CONFIGURACIÓN
  // ============================================================

  const float ALTURA_TANQUE_CM = 40.0;

  const float NIVEL_NORMAL_MIN = 70.0;
  const float NIVEL_ALERTA_MIN = 30.0;

  const float TEMP_RIESGO = 20.0;

  const float HUM_ALERTA = 60.0;
  const float HUM_CRITICA = 50.0;

  const int UMBRAL_LUZ = 2000;

  const float LATITUD_GRADOS = 4.86;
  const int DIA_JULIANO = 268;

  const float KRS = 0.16;

  const unsigned long VENTANA_TMAX_TMIN_MS = 30000UL;

  const float ET0_ALERTA = 3.0;
  const float ET0_CRITICO = 4.5;

  float tMaxHoy = -1000.0;
  float tMinHoy = 1000.0;

  unsigned long inicioVentana = 0;

  // ============================================================
  // DATOS COMPARTIDOS
  // ============================================================

  struct DatosSensor {

    float temp = 0;
    float presion = 0;
    float humedad = 0;
    float nivelPorc = 0;
    float et0Actual = 0;

    int luzAnalog = 0;

    // NUEVO (registro): datos que el registro en flash necesita
    float distCm = 0;
    float puntaje = 0;
    bool falloHcsr04 = false;
    bool falloBme280 = false;

    String estado = "NORMAL";

    // NUEVO: bandera compartida para silenciar el buzzer desde el tablero
    bool alarmaSilenciada = false;
  };

  DatosSensor datosCompartidos;

  SemaphoreHandle_t datosMutex;

  // NUEVO: guarda el estado del ciclo anterior para detectar cuándo empieza
  // un episodio de riesgo NUEVO (y así reactivar el sonido automáticamente)
  String estadoAnterior = "NORMAL";

  // ============================================================
  // HISTÓRICO
  // ============================================================

  #define HIST_TAM 20

  struct Muestra {

    unsigned long t;

    float nivelPorc;
    float temp;
    float humedad;

    String estado;
  };

  Muestra historial[HIST_TAM];

  int histIndice = 0;

  bool histLleno = false;

  // ============================================================
  // REGISTRO EN FLASH (NUEVO)
  // El ESP32 guarda una fila en un archivo CSV cada cierto tiempo,
  // sin depender del WiFi ni de la Raspberry Pi.
  // ============================================================

  // NUEVO: la recoleccion real termino (CSV final descargado el 9 nov).
  // 0 = no se guardan filas nuevas en la flash; 1 = se vuelve a guardar.
  // El archivo /datos.csv NO se borra y se sigue pudiendo descargar.
  #define REGISTRO_FLASH_ACTIVO 0

  // Intervalo del registro: 30000UL = 30 s (valor usado en la recoleccion real).
  const unsigned long INTERVALO_REGISTRO_MS = 30000UL;

  const char* ARCHIVO_DATOS = "/datos.csv";

  // Tope de seguridad para no llenar la particion (si se alcanza, deja de guardar)
  const size_t MAX_BYTES_ARCHIVO = 1000000;

  const char* ENCABEZADO_CSV =
    "ts_dispositivo,uptime_s,boot,seq,temp_c,hum_pct,pres_hpa,nivel_pct,dist_cm,"
    "luz_adc,et0_mm_dia,puntaje,estado,alarma_silenciada,fallo_hcsr04,fallo_bme280";

  bool archivoListo = false;
  uint32_t bootNumero = 0;
  SemaphoreHandle_t archivoMutex;

  // ============================================================
  // PROTOTIPOS
  // ============================================================

  void tareaMedicion(void *parametro);
  void tareaRed(void *parametro);
  void tareaRegistro(void *parametro);
  void tareaMQTT(void *parametro);
  void callbackMQTT(char* topic, byte* payload, unsigned int length);
  bool publicarTelemetriaMQTT(const DatosSensor &d);
  IPAddress resolverBroker();

  String campoFloat(float valor, int decimales, bool valido);
  int codigoEstado(const String &estado);
  bool registrarFila(const DatosSensor &d, uint32_t seq);
  void reiniciarArchivoDatos();

  float leerDistanciaCruda();
  float leerDistanciaCM();

  void apagarLeds();

  float calcularRa(float latitudGrados, int diaJuliano);
  float calcularRs(float tMax, float tMin, float Ra);
  float calcularET0Turc(
    float tMax,
    float tMin,
    float humedadPromedio
  );

  String construirJSONActual();
  String construirJSONHistorial();

  String construirHTML();

  // ============================================================
  // SETUP
  // ============================================================

  void setup() {

    Serial.begin(115200);

    Wire.begin(SDA_PIN, SCL_PIN);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    pinMode(LED_ROJO, OUTPUT);
    pinMode(LED_AMARILLO, OUTPUT);
    pinMode(LED_VERDE, OUTPUT);

    pinMode(BUZZER_PIN, OUTPUT);

    if (!bme.begin(0x76)) {
      Serial.println("BME280 no encontrado");
      while (1);
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      Serial.println("OLED no encontrado");
      while (1);
    }

    display.clearDisplay();
    display.display();

    inicioVentana = millis();

    datosMutex = xSemaphoreCreateMutex();
    archivoMutex = xSemaphoreCreateMutex();

    // NUEVO: cuenta cuantas veces ha arrancado el dispositivo (memoria no volatil)
    Preferences prefs;
    prefs.begin("registro", false);
    bootNumero = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", bootNumero);
    prefs.end();

    // NUEVO: monta el sistema de archivos (lo formatea solo si esta vacio o daniado)
    if (LittleFS.begin(true)) {
      archivoListo = true;
      if (!LittleFS.exists(ARCHIVO_DATOS)) {
        reiniciarArchivoDatos();
      }
      Serial.print("Registro listo. Boot #");
      Serial.println(bootNumero);
    } else {
      Serial.println("ERROR: LittleFS no pudo montarse, no se guardaran datos");
    }

    Serial.println("Sensores listos");

    xTaskCreatePinnedToCore(
      tareaMedicion, "Medicion", 4096, NULL, 1, NULL, 1
    );

    xTaskCreatePinnedToCore(
      tareaRed, "Red", 8192, NULL, 1, NULL, 0
    );

    // Tarea que guarda una fila cada INTERVALO_REGISTRO_MS (solo si esta activa)
  #if REGISTRO_FLASH_ACTIVO
    xTaskCreatePinnedToCore(
      tareaRegistro, "Registro", 6144, NULL, 1, NULL, 0
    );
  #else
    Serial.println("Registro en flash DESACTIVADO (recoleccion terminada)");
  #endif

    // NUEVO: MQTT corre en una tarea independiente en el core 0.
    xTaskCreatePinnedToCore(
      tareaMQTT, "MQTT", 8192, NULL, 1, NULL, 0
    );
  }

  // ============================================================
  // LOOP
  // ============================================================

  void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // ============================================================
  // TAREA DE MEDICIÓN
  // ============================================================

  void tareaMedicion(void *parametro) {

    for (;;) {

      float temp = bme.readTemperature();
      float presion = bme.readPressure() / 100.0F;
      float humedad = bme.readHumidity();
      float distancia = leerDistanciaCM();

      // NUEVO: un sensor en falla no debe confundirse con una lectura valida
      bool falloHcsr04 = (distancia < 0);
      bool falloBme280 = isnan(temp) || isnan(humedad) || isnan(presion);

      float nivelCM = ALTURA_TANQUE_CM - distancia;
      float nivelPorc = (nivelCM / ALTURA_TANQUE_CM) * 100.0;
      nivelPorc = constrain(nivelPorc, 0, 100);

      int luzAnalog = analogRead(LDR_PIN);

      if (millis() - inicioVentana >= VENTANA_TMAX_TMIN_MS) {
        tMaxHoy = temp;
        tMinHoy = temp;
        inicioVentana = millis();
      } else {
        if (temp > tMaxHoy) tMaxHoy = temp;
        if (temp < tMinHoy) tMinHoy = temp;
      }

      float et0Actual = calcularET0Turc(tMaxHoy, tMinHoy, humedad);

      int riesgoNivel = 0;
      if (nivelPorc < NIVEL_ALERTA_MIN) riesgoNivel = 2;
      else if (nivelPorc < NIVEL_NORMAL_MIN) riesgoNivel = 1;

      int riesgoTemp = (temp > TEMP_RIESGO) ? 1 : 0;

      int riesgoHum = 0;
      if (humedad < HUM_CRITICA) riesgoHum = 2;
      else if (humedad < HUM_ALERTA) riesgoHum = 1;

      int riesgoLuz = (luzAnalog < UMBRAL_LUZ) ? 1 : 0;

      int riesgoET = 0;
      if (et0Actual >= ET0_CRITICO) riesgoET = 2;
      else if (et0Actual >= ET0_ALERTA) riesgoET = 1;

      float pesoNivel = 0.40, pesoHum = 0.20, pesoTemp = 0.10, pesoLuz = 0.10, pesoET = 0.20;

      float scoreFinal =
        (riesgoNivel * pesoNivel) + (riesgoHum * pesoHum) +
        (riesgoTemp * pesoTemp) + (riesgoLuz * pesoLuz) +
        (riesgoET * pesoET);

      String estado;
      apagarLeds();

      // NUEVO: leemos (bajo mutex) si el usuario silenció la alarma desde el tablero
      bool silenciada = false;
      if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {
        silenciada = datosCompartidos.alarmaSilenciada;
        xSemaphoreGive(datosMutex);
      }

      if (scoreFinal >= 1.2) {
        estado = "CRITICO";
        digitalWrite(LED_ROJO, HIGH);
      } else if (scoreFinal >= 0.5) {
        estado = "ALERTA";
        digitalWrite(LED_AMARILLO, HIGH);
      } else {
        estado = "NORMAL";
        digitalWrite(LED_VERDE, HIGH);
      }

      // NUEVO: si empieza un episodio de riesgo nuevo (antes NORMAL, ahora no),
      // reactivamos el sonido automáticamente para el nuevo evento
          if ((estado != "NORMAL" && estadoAnterior == "NORMAL") ||
          (estado == "CRITICO" && estadoAnterior == "ALERTA")) {
        silenciada = false;
        if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {
          datosCompartidos.alarmaSilenciada = false;
          xSemaphoreGive(datosMutex);
        }
      }
      estadoAnterior = estado;

      // NUEVO: el buzzer solo suena si hay riesgo Y no está silenciada
      if (estado == "CRITICO" && !silenciada) {
        tone(BUZZER_PIN, 1000);
      } else if (estado == "ALERTA" && !silenciada) {
        tone(BUZZER_PIN, 800, 200);
      } else {
        noTone(BUZZER_PIN);
      }

      Serial.print("Temp: "); Serial.print(temp); Serial.println(" °C");
      Serial.print("Nivel: "); Serial.print(nivelPorc); Serial.println(" %");
      Serial.print("ET0: "); Serial.print(et0Actual); Serial.println(" mm/dia");
      Serial.print("Estado: "); Serial.println(estado);
      Serial.print("Distancia: "); Serial.print(distancia); Serial.println(" cm"); 
      Serial.print("Alarma silenciada: "); Serial.println(silenciada ? "SI" : "NO");
      Serial.println("----------------------");

      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print("Temp: "); display.print(temp); display.println(" C");
      display.print("Hum: "); display.print(humedad); display.println(" %");
      display.print("Nivel: "); display.print(nivelPorc); display.println(" %");
      display.print("ET0: "); display.print(et0Actual); display.println(" mm/d");
      display.setTextSize(2);
      display.setCursor(0, 40);
      display.println(estado);
      display.display();

      if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {
        datosCompartidos.temp = temp;
        datosCompartidos.presion = presion;
        datosCompartidos.humedad = humedad;
        datosCompartidos.nivelPorc = nivelPorc;
        datosCompartidos.et0Actual = et0Actual;
        datosCompartidos.luzAnalog = luzAnalog;
        datosCompartidos.estado = estado;

        // NUEVO: datos para el registro en flash
        datosCompartidos.distCm = distancia;
        datosCompartidos.puntaje = scoreFinal;
        datosCompartidos.falloHcsr04 = falloHcsr04;
        datosCompartidos.falloBme280 = falloBme280;

        historial[histIndice] = { millis(), nivelPorc, temp, humedad, estado };
        histIndice = (histIndice + 1) % HIST_TAM;
        if (histIndice == 0) histLleno = true;

        xSemaphoreGive(datosMutex);
      }

      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }

  // ============================================================
  // TAREA DE RED
  // ============================================================

  void tareaRed(void *parametro) {

    WiFi.begin(ssid, password);
    Serial.print("Conectando a WiFi");
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      Serial.print(".");
      // NUEVO: si la red aparece mas tarde (p. ej. hotspot que se enciende despues),
      // volvemos a intentar cada ~10 s en lugar de depender de un unico WiFi.begin
      intentos++;
      if (intentos % 20 == 0) {
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }
    }
    Serial.println();
    Serial.print("Conectado. IP: ");
    Serial.println(WiFi.localIP());

    // NUEVO: hora real por NTP (UTC). Se mantiene sincronizada mientras haya WiFi.
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    server.on("/api/estado", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      request->send(200, "application/json", construirJSONActual());
    });

    server.on("/api/historial", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      request->send(200, "application/json", construirJSONHistorial());
    });

    // NUEVO: endpoint para silenciar la alarma desde el tablero
    server.on("/api/silenciar", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {
        datosCompartidos.alarmaSilenciada = true;
        xSemaphoreGive(datosMutex);
      }
      request->send(200, "application/json", "{\"ok\":true}");
    });

    // NUEVO: descarga del archivo de datos guardados en flash
    server.on("/api/datos.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      if (!archivoListo || !LittleFS.exists(ARCHIVO_DATOS)) {
        request->send(404, "text/plain", "Sin datos");
        return;
      }
      request->send(LittleFS, ARCHIVO_DATOS, "text/csv", true);
    });

    // NUEVO: borra el archivo (solo POST y con ?confirmar=si, para evitar accidentes)
    server.on("/api/datos/borrar", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      if (!archivoListo) {
        request->send(500, "text/plain", "Sin sistema de archivos");
        return;
      }
      if (!request->hasParam("confirmar") ||
          request->getParam("confirmar")->value() != "si") {
        request->send(400, "text/plain", "Falta ?confirmar=si");
        return;
      }
      reiniciarArchivoDatos();
      request->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (!request->authenticate(authUser, authPass)) {
        return request->requestAuthentication();
      }
      request->send(200, "text/html", construirHTML());
    });

    server.begin();
    Serial.println("Servidor web iniciado");

    for (;;) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi perdido, reconectando...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }

  // ============================================================
  // HTML DEL DASHBOARD
  // ============================================================

  String construirHTML() {

    String html = R"rawliteral(
  <!DOCTYPE html>
  <html lang="es">
  <head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Monitoreo Ambiental</title>
  <style>
  * { box-sizing: border-box; }
  body { margin: 0; font-family: Arial, Helvetica, sans-serif; background: #f1f5f9; color: #1e293b; }
  .header { background: #0f172a; color: white; padding: 25px; text-align: center; }
  .header h1 { margin: 0; font-size: 28px; }
  .header p { margin-top: 8px; color: #94a3b8; }
  .container { max-width: 1200px; margin: auto; padding: 25px; }
  .estado { padding: 25px; border-radius: 15px; color: white; text-align: center; margin-bottom: 15px; background: #16a34a; transition: 0.3s; }
  .estado h2 { margin: 0; font-size: 28px; }

  /* NUEVO: estilos del botón de silenciar */
  .silenciar-container { text-align: center; margin-bottom: 25px; }
  #btnSilenciar {
      background: #1e293b; color: white; border: none; padding: 12px 24px;
      border-radius: 10px; font-size: 15px; cursor: pointer; transition: 0.2s;
  }
  #btnSilenciar:disabled { background: #cbd5e1; color: #64748b; cursor: not-allowed; }
  #btnSilenciar:not(:disabled):hover { background: #334155; }

  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 18px; }
  .card { background: white; border-radius: 15px; padding: 22px; box-shadow: 0 4px 12px rgba(0,0,0,0.08); }
  .card-title { color: #64748b; font-size: 15px; margin-bottom: 10px; }
  .valor { font-size: 30px; font-weight: bold; }
  .icon { font-size: 30px; margin-bottom: 10px; }
  .nivel-container { margin-top: 12px; background: #e2e8f0; height: 18px; border-radius: 20px; overflow: hidden; }
  .nivel-bar { height: 100%; width: 0%; background: #2563eb; transition: width 0.5s; }
  .historial { background: white; margin-top: 25px; padding: 25px; border-radius: 15px; box-shadow: 0 4px 12px rgba(0,0,0,0.08); }
  .historial h2 { margin-top: 0; }
  canvas { width: 100% !important; height: 300px !important; }
  .footer { text-align: center; color: #64748b; padding: 25px; font-size: 14px; }
  .actualizado { text-align: right; color: #64748b; font-size: 13px; margin-top: 10px; }
  @media(max-width:600px) { .header h1 { font-size: 22px; } .container { padding: 15px; } .valor { font-size: 25px; } }
  </style>
  </head>
  <body>

  <div class="header">
      <h1>🌱 Monitoreo Ambiental</h1>
      <p>Sistema de monitoreo basado en ESP32</p>
  </div>

  <div class="container">

      <div id="estadoBox" class="estado">
          <h2>ESTADO: <span id="estado">CARGANDO...</span></h2>
      </div>

      <!-- NUEVO: botón para silenciar la alarma -->
      <div class="silenciar-container">
          <button id="btnSilenciar" onclick="silenciarAlarma()">🔇 Silenciar Alarma</button>
      </div>

      <div class="grid">
          <div class="card">
              <div class="icon">🌡️</div>
              <div class="card-title">Temperatura</div>
              <div class="valor"><span id="temp">--</span> °C</div>
          </div>
          <div class="card">
              <div class="icon">💧</div>
              <div class="card-title">Humedad</div>
              <div class="valor"><span id="humedad">--</span> %</div>
          </div>
          <div class="card">
              <div class="icon">🌊</div>
              <div class="card-title">Nivel del tanque</div>
              <div class="valor"><span id="nivel">--</span> %</div>
              <div class="nivel-container"><div id="nivelBar" class="nivel-bar"></div></div>
          </div>
          <div class="card">
              <div class="icon">☀️</div>
              <div class="card-title">Luz</div>
              <div class="valor"><span id="luz">--</span></div>
          </div>
          <div class="card">
              <div class="icon">💨</div>
              <div class="card-title">Presión atmosférica</div>
              <div class="valor"><span id="presion">--</span> hPa</div>
          </div>
          <div class="card">
              <div class="icon">🌱</div>
              <div class="card-title">Evapotranspiración ET0</div>
              <div class="valor"><span id="et0">--</span> mm/día</div>
          </div>
      </div>

      <div class="historial">
          <h2>📈 Histórico</h2>
          <canvas id="grafica"></canvas>
          <div class="actualizado">Última actualización: <span id="hora">--</span></div>
      </div>

  </div>

  <div class="footer">ESP32 • Sistema de monitoreo ambiental</div>

  <script>

  // NUEVO: guarda si el estado actual necesita alarma, para habilitar/deshabilitar el botón
  let estadoActual = 'NORMAL';

  // NUEVO: función que llama al endpoint /api/silenciar
  async function silenciarAlarma() {
      try {
          const boton = document.getElementById('btnSilenciar');
          boton.disabled = true;
          boton.innerText = 'Silenciando...';

          await fetch('/api/silenciar', { method: 'POST' });

          boton.innerText = '🔇 Alarma silenciada';
      } catch (error) {
          console.log('Error silenciando alarma:', error);
          document.getElementById('btnSilenciar').innerText = '🔇 Silenciar Alarma';
          document.getElementById('btnSilenciar').disabled = false;
      }
  }

  async function actualizarDatos() {
      try {
          const respuesta = await fetch('/api/estado');
          const datos = await respuesta.json();

          document.getElementById('temp').innerText = datos.temp.toFixed(2);
          document.getElementById('humedad').innerText = datos.humedad.toFixed(2);
          document.getElementById('nivel').innerText = datos.nivelPorc.toFixed(1);
          document.getElementById('luz').innerText = datos.luz;
          document.getElementById('presion').innerText = datos.presion.toFixed(1);
          document.getElementById('et0').innerText = datos.et0.toFixed(2);
          document.getElementById('estado').innerText = datos.estado;

          document.getElementById('nivelBar').style.width = datos.nivelPorc + '%';

          const caja = document.getElementById('estadoBox');
          if (datos.estado === 'CRITICO') {
              caja.style.background = '#dc2626';
          } else if (datos.estado === 'ALERTA') {
              caja.style.background = '#f59e0b';
          } else {
              caja.style.background = '#16a34a';
          }

          // NUEVO: lógica del botón de silenciar según el estado y la bandera del ESP32
          const boton = document.getElementById('btnSilenciar');
          estadoActual = datos.estado;

          if (datos.estado === 'NORMAL') {
              boton.disabled = true;
              boton.innerText = '🔇 Silenciar Alarma';
          } else if (datos.alarmaSilenciada) {
              boton.disabled = true;
              boton.innerText = '🔇 Alarma silenciada';
          } else {
              boton.disabled = false;
              boton.innerText = '🔇 Silenciar Alarma';
          }

          const ahora = new Date();
          document.getElementById('hora').innerText = ahora.toLocaleTimeString();

      } catch (error) {
          console.log('Error obteniendo datos:', error);
      }
  }

  async function actualizarGrafica() {
      try {
          const respuesta = await fetch('/api/historial');
          const datos = await respuesta.json();

          const canvas = document.getElementById('grafica');
          const ctx = canvas.getContext('2d');

          ctx.clearRect(0, 0, canvas.width, canvas.height);

          if (datos.length < 2) return;

          const ancho = canvas.width;
          const alto = canvas.height;

          ctx.beginPath();
          ctx.strokeStyle = '#2563eb';
          ctx.lineWidth = 3;

          datos.forEach((dato, i) => {
              const x = (i / (datos.length - 1)) * ancho;
              const y = alto - (dato.nivelPorc / 100) * alto;
              if (i === 0) ctx.moveTo(x, y);
              else ctx.lineTo(x, y);
          });

          ctx.stroke();

          let minTemp = datos[0].temp;
          let maxTemp = datos[0].temp;

          datos.forEach(dato => {
              if (dato.temp < minTemp) minTemp = dato.temp;
              if (dato.temp > maxTemp) maxTemp = dato.temp;
          });

          if (maxTemp === minTemp) maxTemp += 1;

          ctx.beginPath();
          ctx.strokeStyle = '#f97316';
          ctx.lineWidth = 2;

          datos.forEach((dato, i) => {
              const x = (i / (datos.length - 1)) * ancho;
              const y = alto - ((dato.temp - minTemp) / (maxTemp - minTemp)) * alto;
              if (i === 0) ctx.moveTo(x, y);
              else ctx.lineTo(x, y);
          });

          ctx.stroke();

      } catch (error) {
          console.log('Error en gráfica:', error);
      }
  }

  actualizarDatos();
  actualizarGrafica();

  setInterval(actualizarDatos, 2000);
  setInterval(actualizarGrafica, 4000);

  </script>

  </body>
  </html>
  )rawliteral";

    return html;
  }

  // ============================================================
  // JSON ACTUAL
  // ============================================================

  String construirJSONActual() {

    String json;

    if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {

      json = "{";
      json += "\"temp\":" + String(datosCompartidos.temp, 2) + ",";
      json += "\"presion\":" + String(datosCompartidos.presion, 2) + ",";
      json += "\"humedad\":" + String(datosCompartidos.humedad, 2) + ",";
      json += "\"nivelPorc\":" + String(datosCompartidos.nivelPorc, 2) + ",";
      json += "\"et0\":" + String(datosCompartidos.et0Actual, 2) + ",";
      json += "\"luz\":" + String(datosCompartidos.luzAnalog) + ",";
      json += "\"estado\":\"" + datosCompartidos.estado + "\",";
      // NUEVO: exponemos el estado de silencio al frontend
      json += "\"alarmaSilenciada\":" + String(datosCompartidos.alarmaSilenciada ? "true" : "false");
      json += "}";

      xSemaphoreGive(datosMutex);
    }

    return json;
  }

  // ============================================================
  // JSON HISTORIAL
  // ============================================================

  String construirJSONHistorial() {

    String json = "[";

    if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {

      int total = histLleno ? HIST_TAM : histIndice;
      int inicio = histLleno ? histIndice : 0;

      for (int i = 0; i < total; i++) {
        int idx = (inicio + i) % HIST_TAM;
        Muestra &m = historial[idx];

        json += "{";
        json += "\"t\":" + String(m.t) + ",";
        json += "\"nivelPorc\":" + String(m.nivelPorc, 2) + ",";
        json += "\"temp\":" + String(m.temp, 2) + ",";
        json += "\"humedad\":" + String(m.humedad, 2) + ",";
        json += "\"estado\":\"" + m.estado + "\"";
        json += "}";

        if (i < total - 1) json += ",";
      }

      xSemaphoreGive(datosMutex);
    }

    json += "]";
    return json;
  }

  // ============================================================
  // ULTRASÓNICO
  // ============================================================

  float leerDistanciaCruda() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duracion = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duracion == 0) return -1;
    return duracion * 0.0343 / 2;
  }

  float leerDistanciaCM() {
    const int N = 5;
    float lecturas[N];
    int validas = 0;

    for (int i = 0; i < N; i++) {
      float d = leerDistanciaCruda();
      if (d > 0) {
        lecturas[validas] = d;
        validas++;
      }
      delay(30);
    }

    if (validas == 0) return -1;

    for (int i = 0; i < validas - 1; i++) {
      for (int j = 0; j < validas - i - 1; j++) {
        if (lecturas[j] > lecturas[j + 1]) {
          float tmp = lecturas[j];
          lecturas[j] = lecturas[j + 1];
          lecturas[j + 1] = tmp;
        }
      }
    }

    return lecturas[validas / 2];
  }

  void apagarLeds() {
    digitalWrite(LED_ROJO, LOW);
    digitalWrite(LED_AMARILLO, LOW);
    digitalWrite(LED_VERDE, LOW);
  }

  float calcularRa(float latitudGrados, int diaJuliano) {
    float phi = latitudGrados * PI / 180.0;
    const float Gsc = 0.0820;

    float dr = 1.0 + 0.033 * cos(2.0 * PI * diaJuliano / 365.0);
    float delta = 0.409 * sin(2.0 * PI * diaJuliano / 365.0 - 1.39);
    float ws = acos(-tan(phi) * tan(delta));

    return (24.0 * 60.0 / PI) * Gsc * dr *
          (ws * sin(phi) * sin(delta) + cos(phi) * cos(delta) * sin(ws));
  }

  float calcularRs(float tMax, float tMin, float Ra) {
    float amplitud = tMax - tMin;
    if (amplitud < 0) amplitud = 0;
    return KRS * sqrt(amplitud) * Ra;
  }

  float calcularET0Turc(float tMax, float tMin, float humedadPromedio) {
    float Ra = calcularRa(LATITUD_GRADOS, DIA_JULIANO);
    float Rs = calcularRs(tMax, tMin, Ra);
    float tMedia = (tMax + tMin) / 2.0;

    float K = 1.0;
    if (humedadPromedio < 50.0) {
      K = 1.0 + (50.0 - humedadPromedio) / 70.0;
    }

    float et0 = 0.013 * (tMedia / (tMedia + 15.0)) * (23.8846 * Rs + 50.0) * K;
    if (et0 < 0) et0 = 0;
    return et0;
  }

  // ============================================================
  // REGISTRO EN FLASH (NUEVO)
  // ============================================================

  // Devuelve el numero como texto, o vacio si el dato no es valido
  String campoFloat(float valor, int decimales, bool valido) {
    if (!valido || isnan(valor)) return "";
    return String(valor, decimales);
  }

  // Mismos codigos que el contrato del equipo: 0 NORMAL, 1 ALERTA, 2 CRITICO
  int codigoEstado(const String &estado) {
    if (estado == "CRITICO") return 2;
    if (estado == "ALERTA") return 1;
    return 0;
  }

  // Crea el archivo desde cero, solo con el encabezado
  void reiniciarArchivoDatos() {
    if (xSemaphoreTake(archivoMutex, portMAX_DELAY) == pdTRUE) {
      LittleFS.remove(ARCHIVO_DATOS);
      File f = LittleFS.open(ARCHIVO_DATOS, FILE_WRITE);
      if (f) {
        f.println(ENCABEZADO_CSV);
        f.close();
      }
      xSemaphoreGive(archivoMutex);
    }
  }

  // Agrega una fila al archivo. Devuelve false si no pudo guardarla.
  bool registrarFila(const DatosSensor &d, uint32_t seq) {

    time_t ahora = time(nullptr);
    unsigned long ts = (ahora > 1700000000UL) ? (unsigned long)ahora : 0;  // 0 = sin hora NTP todavia

    bool bmeOk = !d.falloBme280;
    bool hcOk = !d.falloHcsr04;

    String fila;
    fila.reserve(128);
    fila += String(ts) + ",";
    fila += String(millis() / 1000UL) + ",";
    fila += String(bootNumero) + ",";
    fila += String(seq) + ",";
    fila += campoFloat(d.temp, 2, bmeOk) + ",";
    fila += campoFloat(d.humedad, 2, bmeOk) + ",";
    fila += campoFloat(d.presion, 2, bmeOk) + ",";
    fila += campoFloat(d.nivelPorc, 2, hcOk) + ",";
    fila += campoFloat(d.distCm, 2, hcOk) + ",";
    fila += String(d.luzAnalog) + ",";
    fila += campoFloat(d.et0Actual, 2, bmeOk) + ",";
    fila += campoFloat(d.puntaje, 2, true) + ",";
    fila += String(codigoEstado(d.estado)) + ",";
    fila += String(d.alarmaSilenciada ? 1 : 0) + ",";
    fila += String(d.falloHcsr04 ? 1 : 0) + ",";
    fila += String(d.falloBme280 ? 1 : 0);

    bool guardada = false;

    if (xSemaphoreTake(archivoMutex, portMAX_DELAY) == pdTRUE) {
      File f = LittleFS.open(ARCHIVO_DATOS, FILE_APPEND);
      if (f) {
        if (f.size() < MAX_BYTES_ARCHIVO) {
          f.println(fila);
          guardada = true;
        } else {
          Serial.println("Registro: archivo lleno, no se guarda la fila");
        }
        f.close();
      } else {
        Serial.println("Registro: no se pudo abrir el archivo");
      }
      xSemaphoreGive(archivoMutex);
    }

    return guardada;
  }

  // Tarea que, cada INTERVALO_REGISTRO_MS, copia la ultima medicion y la guarda
  void tareaRegistro(void *parametro) {

    if (!archivoListo) {
      Serial.println("Registro desactivado: no hay sistema de archivos");
      vTaskDelete(NULL);
    }

    // Da tiempo a que tareaMedicion complete su primera lectura
    vTaskDelay(pdMS_TO_TICKS(5000));

    uint32_t seq = 0;

    for (;;) {

      DatosSensor copia;

      if (xSemaphoreTake(datosMutex, portMAX_DELAY) == pdTRUE) {
        copia = datosCompartidos;
        xSemaphoreGive(datosMutex);
      }

      if (registrarFila(copia, seq)) {
        Serial.print("Registro: fila ");
        Serial.print(seq);
        Serial.println(" guardada");
      }
      seq++;

      vTaskDelay(pdMS_TO_TICKS(INTERVALO_REGISTRO_MS));
    }
  }

// ============================================================
// MQTT: PUBLICACION DE TELEMETRIA Y RECEPCION DE COMANDOS
// Todas las operaciones de PubSubClient se ejecutan solo aqui.
// ============================================================

void callbackMQTT(char* topic, byte* payload, unsigned int length) {

  if (String(topic) != MQTT_TOPIC_COMANDO) return;

  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("MQTT: JSON de comando invalido: ");
    Serial.println(error.c_str());
    return;
  }

  bool silenciar = doc["silenciar"] | false;

  if (silenciar) {
    if (xSemaphoreTake(datosMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      datosCompartidos.alarmaSilenciada = true;
      xSemaphoreGive(datosMutex);
      Serial.println("MQTT: alarma silenciada por comando remoto");
    }
  }
}

bool publicarTelemetriaMQTT(const DatosSensor &d) {

  StaticJsonDocument<512> doc;

  time_t ahora = time(nullptr);
  uint32_t timestamp = (ahora > 1700000000UL) ? (uint32_t)ahora : 0;

  doc["seq"] = secuenciaMQTT;
  doc["ts_dispositivo"] = timestamp;

  // Si un sensor falla, se envia null y se conserva la bandera de fallo.
  if (d.falloBme280 || isnan(d.temp)) doc["temp_c"] = nullptr;
  else doc["temp_c"] = d.temp;

  if (d.falloBme280 || isnan(d.humedad)) doc["hum_pct"] = nullptr;
  else doc["hum_pct"] = d.humedad;

  if (d.falloBme280 || isnan(d.presion)) doc["pres_hpa"] = nullptr;
  else doc["pres_hpa"] = d.presion;

  if (d.falloHcsr04 || isnan(d.nivelPorc)) doc["nivel_pct"] = nullptr;
  else doc["nivel_pct"] = d.nivelPorc;

  if (d.falloHcsr04 || isnan(d.distCm)) doc["dist_cm"] = nullptr;
  else doc["dist_cm"] = d.distCm;

  doc["luz_adc"] = d.luzAnalog;

  if (d.falloBme280 || isnan(d.et0Actual)) doc["et0_mm_dia"] = nullptr;
  else doc["et0_mm_dia"] = d.et0Actual;

  doc["puntaje"] = d.puntaje;
  doc["estado"] = codigoEstado(d.estado);
  doc["alarma_silenciada"] = d.alarmaSilenciada;
  doc["fallo_hcsr04"] = d.falloHcsr04;
  doc["fallo_bme280"] = d.falloBme280;
  doc["rssi"] = WiFi.RSSI();

  char payload[512];
  size_t longitud = serializeJson(doc, payload, sizeof(payload));

  if (longitud == 0 || longitud >= sizeof(payload)) {
    Serial.println("MQTT: no se pudo serializar el JSON");
    return false;
  }

  bool publicado = mqttClient.publish(
    MQTT_TOPIC_TELEMETRIA,
    payload,
    false
  );

  if (publicado) {
    Serial.print("MQTT publish OK: ");
    Serial.println(payload);
    secuenciaMQTT++;
  } else {
    Serial.println("MQTT: fallo al publicar telemetria");
  }

  return publicado;
}

// NUEVO: busca la IP de la Raspberry Pi por su nombre (mDNS).
// Si no la encuentra en 3 s, devuelve la IP de respaldo.
IPAddress resolverBroker() {

  static bool mdnsIniciado = false;
  if (!mdnsIniciado) {
    mdnsIniciado = MDNS.begin("nodo01");   // nombre del ESP32 en la red
  }

  IPAddress ip = MDNS.queryHost(MQTT_HOSTNAME, 3000);

  if (ip == IPAddress(0, 0, 0, 0)) {
    Serial.print("mDNS: no encontre ");
    Serial.print(MQTT_HOSTNAME);
    Serial.print(".local; uso la IP de respaldo ");
    Serial.println(MQTT_IP_RESPALDO);
    return MQTT_IP_RESPALDO;
  }

  Serial.print("mDNS: ");
  Serial.print(MQTT_HOSTNAME);
  Serial.print(".local esta en ");
  Serial.println(ip);
  return ip;
}

void tareaMQTT(void *parametro) {

  mqttClient.setCallback(callbackMQTT);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);

  String clientId = "ESP32-HIDRICO-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);

  for (;;) {

    // Espera a que tareaRed conecte el ESP32 al Wi-Fi.
    if (WiFi.status() != WL_CONNECTED) {
      if (mqttClient.connected()) mqttClient.disconnect();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // Reconexion MQTT sin bloquear las tareas de medicion y registro.
    if (!mqttClient.connected()) {
      // NUEVO: se resuelve la IP en cada intento, por si el hotspot la cambio
      IPAddress ipBroker = resolverBroker();
      mqttClient.setServer(ipBroker, MQTT_PORT);

      Serial.print("MQTT: conectando al broker ");
      Serial.println(ipBroker);

      bool conectado;
      if (strlen(MQTT_USER) > 0) {
        conectado = mqttClient.connect(
          clientId.c_str(),
          MQTT_USER,
          MQTT_PASS,
          MQTT_TOPIC_ESTADO,
          0,
          true,
          "offline"
        );
      } else {
        conectado = mqttClient.connect(
          clientId.c_str(),
          MQTT_TOPIC_ESTADO,
          0,
          true,
          "offline"
        );
      }

      if (conectado) {
        Serial.println("MQTT conectado");
        mqttClient.publish(MQTT_TOPIC_ESTADO, "online", true);
        mqttClient.subscribe(MQTT_TOPIC_COMANDO, 1);
        ultimaPublicacionMQTT = millis();
      } else {
        Serial.print("MQTT: error de conexion, estado=");
        Serial.println(mqttClient.state());
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    // Mantiene viva la conexion y procesa comandos entrantes.
    mqttClient.loop();

    if (millis() - ultimaPublicacionMQTT >= MQTT_INTERVALO_MS) {
      DatosSensor copia;
      bool copiaValida = false;

      if (xSemaphoreTake(datosMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copia = datosCompartidos;
        xSemaphoreGive(datosMutex);
        copiaValida = true;
      }

      if (copiaValida) {
        publicarTelemetriaMQTT(copia);
      }
      ultimaPublicacionMQTT = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}