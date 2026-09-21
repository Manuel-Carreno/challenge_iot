#include <Wire.h>
#include <math.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

Adafruit_BME280 bme;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const float ALTURA_TANQUE_CM = 20.0; // ajustar según el recipiente

// umbral nivel/humedad/temp
// Calibrados con datos reales de Sabana de Bogota / Cundinamarca (IDEAM, UNAL, climate-data.org)
const float NIVEL_NORMAL_MIN = 70.0;
const float NIVEL_ALERTA_MIN = 30.0;

const float TEMP_RIESGO      = 20.0; // maximo normal en la zona es 18-20C (IDEAM); por encima = anomalia real de calor

const float HUM_ALERTA       = 60.0;
const float HUM_CRITICA      = 50.0;

// umbral LDR (analogico, 0-4095)
const int UMBRAL_LUZ = 2000;

// ===================== Evapotranspiracion potencial (ET0) =====================
// Modelo de Turc (1961), con correccion de humedad de Xu & Singh (2000).
// Se eligio sobre Hargreaves-Samani porque SI incorpora la humedad relativa
// (ya medida por el BME280), ademas de la temperatura. No requiere anemometro
// ni piranometro: la radiacion solar (Rs) se ESTIMA a partir de la amplitud
// termica diaria (formula de radiacion de Hargreaves), reutilizando el mismo
// Tmax/Tmin que ya hay que calcular.

const float LATITUD_GRADOS = 4.86;   // Chia / Sabana Centro. Ajustar segun sitio real de prueba.
const int   DIA_JULIANO    = 268;    // Dia del anio (1-365). Actualizar al dia real de la prueba.
const float KRS            = 0.16;   // 0.16 zona de interior (no costera), 0.19 costera (FAO 56)

// "Dia" simulado para el tracking de Tmax/Tmin, ya que no hay RTC.
// Para el banco de pruebas se puede acortar (ej. 300000 = 5 min) y documentarlo
// como "emulacion acelerada" en el informe.
const unsigned long VENTANA_TMAX_TMIN_MS = 86400000UL; // 24 h reales

const float ET0_ALERTA  = 3.0;  // mm/dia -- valor inicial, calibrar en el banco de pruebas
const float ET0_CRITICO = 4.5;  // mm/dia -- valor inicial, calibrar en el banco de pruebas

float tMaxHoy = -1000.0;
float tMinHoy =  1000.0;
unsigned long inicioVentana = 0;
float et0Actual = 0.0;

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
  Serial.println("Sensores listos");
}

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

  return lecturas[validas / 2]; // mediana
}

void apagarLeds() {
  digitalWrite(LED_ROJO, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_VERDE, LOW);
}

// ---- Radiacion extraterrestre Ra (FAO 56, ecuacion 21) ----
float calcularRa(float latitudGrados, int diaJuliano) {
  float phi = latitudGrados * PI / 180.0;
  const float Gsc = 0.0820; // MJ/m2/min (constante solar)

  float dr    = 1.0 + 0.033 * cos(2.0 * PI * diaJuliano / 365.0);
  float delta = 0.409 * sin(2.0 * PI * diaJuliano / 365.0 - 1.39);
  float ws    = acos(-tan(phi) * tan(delta));

  return (24.0 * 60.0 / PI) * Gsc * dr *
         (ws * sin(phi) * sin(delta) + cos(phi) * cos(delta) * sin(ws)); // MJ/m2/dia
}

// ---- Radiacion solar estimada Rs a partir de la amplitud termica ----
float calcularRs(float tMax, float tMin, float Ra) {
  float amplitud = tMax - tMin;
  if (amplitud < 0) amplitud = 0;
  return KRS * sqrt(amplitud) * Ra; // MJ/m2/dia
}

// ---- ET0 con el modelo de Turc, corregido por humedad ----
float calcularET0Turc(float tMax, float tMin, float humedadPromedio) {
  float Ra = calcularRa(LATITUD_GRADOS, DIA_JULIANO);
  float Rs = calcularRs(tMax, tMin, Ra);
  float tMedia = (tMax + tMin) / 2.0;

  float K = 1.0;
  if (humedadPromedio < 50.0) {
    K = 1.0 + (50.0 - humedadPromedio) / 70.0; // Xu & Singh (2000)
  }

  // 23.8846 convierte Rs de MJ/m2/dia a cal/cm2/dia (unidades originales de Turc)
  float et0 = 0.013 * (tMedia / (tMedia + 15.0)) * (23.8846 * Rs + 50.0) * K;
  if (et0 < 0) et0 = 0;
  return et0; // mm/dia
}

void loop() {
  float temp = bme.readTemperature();
  float presion = bme.readPressure() / 100.0F;
  float humedad = bme.readHumidity();
  float distancia = leerDistanciaCM();

  float nivelCM = ALTURA_TANQUE_CM - distancia;
  float nivelPorc = (nivelCM / ALTURA_TANQUE_CM) * 100.0;
  nivelPorc = constrain(nivelPorc, 0, 100);

  int luzAnalog = analogRead(LDR_PIN);

  // ---- Tracking de Tmax/Tmin de la ventana actual (dia simulado) ----
  if (millis() - inicioVentana >= VENTANA_TMAX_TMIN_MS) {
    tMaxHoy = temp;
    tMinHoy = temp;
    inicioVentana = millis();
  } else {
    if (temp > tMaxHoy) tMaxHoy = temp;
    if (temp < tMinHoy) tMinHoy = temp;
  }

  et0Actual = calcularET0Turc(tMaxHoy, tMinHoy, humedad);

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

  // Pesos redistribuidos para incluir ET0 (suman 1.0)
  float pesoNivel = 0.40;
  float pesoHum   = 0.20;
  float pesoTemp  = 0.10;
  float pesoLuz   = 0.10;
  float pesoET    = 0.20;

  float scoreFinal = (riesgoNivel * pesoNivel) +
                      (riesgoHum   * pesoHum) +
                      (riesgoTemp  * pesoTemp) +
                      (riesgoLuz   * pesoLuz) +
                      (riesgoET    * pesoET);

  String estado;
  apagarLeds();
  if (scoreFinal >= 1.2) {
    estado = "CRITICO";
    digitalWrite(LED_ROJO, HIGH);
    tone(BUZZER_PIN, 1000);
  } else if (scoreFinal >= 0.5) {
    estado = "ALERTA";
    digitalWrite(LED_AMARILLO, HIGH);
    tone(BUZZER_PIN, 800, 200);
  } else {
    estado = "NORMAL";
    digitalWrite(LED_VERDE, HIGH);
    noTone(BUZZER_PIN);
  }

  Serial.print("Temp: "); Serial.print(temp); Serial.println(" °C");
  Serial.print("Presion: "); Serial.print(presion); Serial.println(" hPa");
  Serial.print("Humedad: "); Serial.print(humedad); Serial.println(" %");
  Serial.print("Distancia: "); Serial.print(distancia); Serial.println(" cm");
  Serial.print("Nivel: "); Serial.print(nivelPorc); Serial.println(" %");
  Serial.print("Luz (LDR): "); Serial.println(luzAnalog);
  Serial.print("Tmax/Tmin ventana: "); Serial.print(tMaxHoy); Serial.print(" / "); Serial.println(tMinHoy);
  Serial.print("ET0 (Turc): "); Serial.print(et0Actual); Serial.println(" mm/dia");
  Serial.print("Score: "); Serial.println(scoreFinal);
  Serial.print("Estado: "); Serial.println(estado);
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

  delay(2000);
}
