#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define TRIG_PIN 5
#define ECHO_PIN 18

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_BME280 bme;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const float ALTURA_TANQUE_CM = 20.0; // aca hay que ajustralo dependiendo del recipiente

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

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

void loop() {
  float temp = bme.readTemperature();
  float presion = bme.readPressure() / 100.0F;
  float humedad = bme.readHumidity();

  float distancia = leerDistanciaCM();
  float nivelCM = ALTURA_TANQUE_CM - distancia;
  float nivelPorc = (nivelCM / ALTURA_TANQUE_CM) * 100.0;
  nivelPorc = constrain(nivelPorc, 0, 100);

  // monitor serial
  Serial.print("Temp: "); Serial.print(temp); Serial.println(" °C");
  Serial.print("Presion: "); Serial.print(presion); Serial.println(" hPa");
  Serial.print("Humedad: "); Serial.print(humedad); Serial.println(" %");
  Serial.print("Distancia: "); Serial.print(distancia); Serial.println(" cm");
  Serial.print("Nivel: "); Serial.print(nivelPorc); Serial.println(" %");
  Serial.println("----------------------");

  // pantalla
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Temp: "); display.print(temp); display.println(" C");
  display.print("Presion: "); display.print(presion); display.println(" hPa");
  display.print("Hum: "); display.print(humedad); display.println(" %");
  display.print("Dist: "); display.print(distancia); display.println(" cm");
  display.print("Nivel: "); display.print(nivelPorc); display.println(" %");
 
  display.display();

  delay(2000);
}