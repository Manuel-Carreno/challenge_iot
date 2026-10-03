#include <Wire.h>
#include <math.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
// WIFI / SERVIDOR
// ============================================================

const char* ssid       = "WIFI CORRESPONDIENTE";
const char* password   = "CLAVE DEL WIFI SELECCIONADO";
const char* authUser   = "Credenciales estipuladas por el grupo";
const char* authPass   = "Credenciales estipuladas por el grupo";

AsyncWebServer server(80);

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

const float ALTURA_TANQUE_CM = 38.0;

const float NIVEL_NORMAL_MIN = 70.0;
const float NIVEL_ALERTA_MIN = 30.0;

const float TEMP_RIESGO = 20.0;

const float HUM_ALERTA = 60.0;
const float HUM_CRITICA = 50.0;

const int UMBRAL_LUZ = 2000;

const float LATITUD_GRADOS = 4.86;
const int DIA_JULIANO = 268;

const float KRS = 0.16;

const unsigned long VENTANA_TMAX_TMIN_MS = 300000UL;

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
// PROTOTIPOS
// ============================================================

void tareaMedicion(void *parametro);
void tareaRed(void *parametro);

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

  Serial.println("Sensores listos");

  xTaskCreatePinnedToCore(
    tareaMedicion, "Medicion", 4096, NULL, 1, NULL, 1
  );

  xTaskCreatePinnedToCore(
    tareaRed, "Red", 8192, NULL, 1, NULL, 0
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
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Conectado. IP: ");
  Serial.println(WiFi.localIP());

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
