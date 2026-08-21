#include <Wire.h>
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

const float HUM_ALERTA       = 60.0; // humedad normal de temporada seca ronda 65-70%; bajar de 60% ya es anomalo
const float HUM_CRITICA      = 50.0; // muy por debajo del minimo historico normal (~66-67%), indicaria sequia severa

// umbral LDR (analogico, 0-4095). Ajustar con pruebas: tapa el sensor y anota el valor, dale luz y anota el otro
const int UMBRAL_LUZ = 2000;

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  // LDR_PIN no necesita pinMode para analogRead

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

// varias lecturas + mediana, para eliminar los saltos erraticos del ultrasonico
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

void loop() {
  float temp = bme.readTemperature();
  float presion = bme.readPressure() / 100.0F;
  float humedad = bme.readHumidity();
  float distancia = leerDistanciaCM();

  float nivelCM = ALTURA_TANQUE_CM - distancia;
  float nivelPorc = (nivelCM / ALTURA_TANQUE_CM) * 100.0;
  nivelPorc = constrain(nivelPorc, 0, 100);

  int luzAnalog = analogRead(LDR_PIN);

  int riesgoNivel = 0;
  if (nivelPorc < NIVEL_ALERTA_MIN) riesgoNivel = 2;
  else if (nivelPorc < NIVEL_NORMAL_MIN) riesgoNivel = 1;

  int riesgoTemp = (temp > TEMP_RIESGO) ? 1 : 0;

  int riesgoHum = 0;
  if (humedad < HUM_CRITICA) riesgoHum = 2;
  else if (humedad < HUM_ALERTA) riesgoHum = 1;

  int riesgoLuz = (luzAnalog < UMBRAL_LUZ) ? 1 : 0;

  float pesoNivel = 0.45;
  float pesoHum   = 0.25;
  float pesoTemp  = 0.15;
  float pesoLuz   = 0.15;

  float scoreFinal = (riesgoNivel * pesoNivel) +
                      (riesgoHum   * pesoHum) +
                      (riesgoTemp  * pesoTemp) +
                      (riesgoLuz   * pesoLuz);

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
  Serial.print("Score: "); Serial.println(scoreFinal);
  Serial.print("Estado: "); Serial.println(estado);
  Serial.println("----------------------");

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Temp: "); display.print(temp); display.println(" C");
  display.print("Presion: "); display.print(presion); display.println(" hPa");
  display.print("Hum: "); display.print(humedad); display.println(" %");
  display.print("Distancia: "); display.print(distancia); display.println(" cm");
  display.setTextSize(2);
  display.setCursor(0, 40);
  display.println(estado);
  display.display();

  delay(2000);
}