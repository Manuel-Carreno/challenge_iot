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

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_BME280 bme;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const float ALTURA_TANQUE_CM = 20.0; // aca hay que ajustralo dependiendo del recipiente

// umbral
const float NIVEL_NORMAL_MIN = 70.0;
const float NIVEL_ALERTA_MIN = 30.0;
const float TEMP_RIESGO      = 20.0;
const float HUM_ALERTA       = 50.0;
const float HUM_CRITICA      = 40.0;

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);

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

float leerDistanciaCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duracion = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duracion == 0) return -1;
  return duracion * 0.0343 / 2;
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

  // logica para determinar el riesog
  int riesgoNivel = 0;
  if (nivelPorc < NIVEL_ALERTA_MIN) riesgoNivel = 2;
  else if (nivelPorc < NIVEL_NORMAL_MIN) riesgoNivel = 1;

  int riesgoTemp = (temp > TEMP_RIESGO) ? 1 : 0;

  int riesgoHum = 0;
  if (humedad < HUM_CRITICA) riesgoHum = 2;
  else if (humedad < HUM_ALERTA) riesgoHum = 1;

  float pesoNivel = 0.55;
  float pesoHum   = 0.30;
  float pesoTemp  = 0.15;

  float scoreFinal = (riesgoNivel * pesoNivel) +
                      (riesgoHum   * pesoHum) +
                      (riesgoTemp  * pesoTemp);

  String estado;
  apagarLeds();
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

  // monitor serial
  Serial.print("Temp: "); Serial.print(temp); Serial.println(" °C");
  Serial.print("Presion: "); Serial.print(presion); Serial.println(" hPa");
  Serial.print("Humedad: "); Serial.print(humedad); Serial.println(" %");
  Serial.print("Distancia: "); Serial.print(distancia); Serial.println(" cm");
  Serial.print("Nivel: "); Serial.print(nivelPorc); Serial.println(" %");
  Serial.print("Score: "); Serial.println(scoreFinal);
  Serial.print("Estado: "); Serial.println(estado);
  Serial.println("----------------------");

  // pantalla
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