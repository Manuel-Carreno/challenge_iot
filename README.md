<div align="center">

# 💧 Sistema IoT de Monitoreo de Riesgo Hídrico
### Sabana Centro, Cundinamarca — Alerta temprana de desabastecimiento por fenómeno El Niño

**IoT · 2026-2 · Challenge #1 · #2 · #3 · Universidad de La Sabana**

**Camilo Gutierrez, Manuel Carreño, Daniel Sanabria**

[![Wiki](https://img.shields.io/badge/📖-Wiki_completa-blue)](../../wiki)
[![Wokwi](https://img.shields.io/badge/🔧-Simulación_Wokwi-orange)](#-simulación-en-wokwi)
[![Video](https://img.shields.io/badge/🎥-Video_demo-red)](#-video-demostrativo)
[![Estado](https://img.shields.io/badge/prototipo-completado-green)]()

</div>

---

## 🌎 El problema

En 2026, el Gobierno Nacional y el IDEAM confirmaron el inicio anticipado del fenómeno de El Niño, agravando la disponibilidad de agua dulce en la Sabana de Bogotá y Cundinamarca. La CAR identificó **20 municipios en riesgo extremo** de desabastecimiento hídrico por agotamiento de fuentes superficiales y reservorios locales.

**¿Cómo diseñar un sistema IoT que mida y analice en tiempo real el nivel del agua y variables meteorológicas, fusione esas señales para generar alertas válidas y avise a tiempo a la comunidad y a las autoridades, en el sitio y en un tablero de control local y global?**

Este repositorio contiene la respuesta de nuestro equipo a ese reto.

## ⚙️ ¿Qué hace el sistema?

Un ESP32 lee, cada 2 segundos, las señales físicas de un punto de almacenamiento de agua:

| Sensor | Variable | Rol |
|---|---|---|
| 🔊 HC-SR04 | Nivel de agua | Disponibilidad física directa del recurso |
| 🌡️💧🌪️ BME280 | Temperatura, humedad, presión | Anomalías de calor y sequía atmosférica; base del cálculo de ET₀ (Turc) |
| ☀️ LDR | Luz | Proxy de la radiación solar |

Estas señales se combinan en un **puntaje de riesgo ponderado** (NORMAL / ALERTA / CRÍTICO), y el sistema avisa en tres niveles:

- **In situ:** 🟢🟡🔴 LEDs, 🔊 buzzer y 🖥️ OLED, que funcionan aunque no haya internet.
- **Tablero local:** servidor web embebido en el ESP32, con usuario y clave.
- **Tablero global (Challenge #3):** el ESP32 publica por **MQTT** a un **gateway en Raspberry Pi 5** (Mosquitto + SQLite). El gateway valida los datos, calcula la tasa de descenso del nivel y los reenvía por MQTT/TLS a **Ubidots**, que envía **correos de alerta** a las autoridades. Un **módulo de IA** (estadística + Google Gemini) estima las horas a nivel crítico y redacta recomendaciones. La alarma se puede **silenciar a distancia** desde Ubidots.

```
ESP32 ──MQTT──▶ Raspberry Pi 5 (Mosquitto · gateway.py · SQLite · IA) ──MQTT/TLS──▶ Ubidots ──▶ correo
  ◀──────────────── comando "silenciar" ◀──────────────────────────────────────┘
```

El código del Challenge #3 está en la carpeta [`challenge3/`](challenge3).

> **Nota:** el diseño original contemplaba un sensor BME280 (temp+hum+presión integrado). En la simulación de Wokwi se usó **DHT22 + BMP180** en su lugar, ya que el BME280 no está disponible en ese entorno. El prototipo físico usa el BME280.

## 🗂️ Documentación completa

Toda la documentación técnica del proyecto — restricciones de diseño, arquitectura, UML, calibración de sensores con fuentes verificadas, autoevaluación y conclusiones — está en la **[📖 Wiki del repositorio](../../wiki)**:

| Página | Contenido |
|---|---|
| [Home](../../wiki/Home) | Índice general |
| [1. Resumen y motivación](../../wiki/01‐Resumen‐y‐Motivacion) | Contexto, motivación, justificación |
| [2. Solución propuesta](../../wiki/02‐Solucion‐Propuesta) | Arquitectura, criterios de diseño, UML, esquemático |
| [3. Configuración experimental](../../wiki/03‐Configuracion‐Experimental‐y‐Calibracion) | De dónde sale cada valor de calibración del código |
| [4. Autoevaluación](../../wiki/04‐Autoevaluacion) | Protocolo de pruebas y sus límites |
| [5. Conclusiones y referencias](../../wiki/05‐Conclusiones‐y‐Referencias) | Retos, trabajo futuro, bibliografía IEEE |
| [6. Anexos](../../wiki/06‐Anexos) | Código fuente documentado, esquemáticos |
| [7. Uso de IA](../../wiki/07-Uso-de-IA) | Transparencia sobre herramientas de IA usadas |
| [9. Mejoras respecto al Challenge #2](../../wiki/09-Mejoras-respecto-al-Challenge-2) | Qué cambió en el Challenge #3 |
| [10. Módulo de IA](../../wiki/10-Modulo-de-Inteligencia-Artificial) | Estadística + Gemini: anomalías, horas a crítico, recomendación |

## 🎥 Video demostrativo

[![Video Challenge 1](https://img.shields.io/badge/▶️-Video_Challenge_1-critical?style=for-the-badge)](https://drive.google.com/file/d/1P2EVXW7vv0kwAGzXGNZH_OP_AUegjNyq/view?usp=sharing&t=2.583)

[![Video challenge 2](https://img.shields.io/badge/▶️-Video_Challenge_2-critical?style=for-the-badge)](https://drive.google.com/file/d/1lkiqnkszwCIkU-OEZxHKXko38EKrM1s0/view?usp=sharing)

[![Video challenge 3](https://img.shields.io/badge/▶️-Video_Challenge_3-critical?style=for-the-badge)](PEGAR_ENLACE_DEL_VIDEO)

## 🔧 Simulación en Wokwi

[![Abrir en Wokwi](https://img.shields.io/badge/Wokwi-Abrir_simulación-00979D?style=for-the-badge)](https://wokwi.com/projects/472298863408540673)

## 🧩 Hardware usado

| Componente | Función | Estado |
|---|---|---|
| ESP32 DevKit | Microcontrolador central | ✅ Disponible |
| BME280 | Presión + Temperatura + Humedad | ✅ Disponible |
| HC-SR04 | Nivel de agua (ultrasónico) | ✅ Disponible |
| LDR | Radiación solar / luz | ✅ Disponible |
| OLED SSD1306 | Visualización local | ✅ Disponible |
| LEDs verde/amarillo/rojo + buzzer | Alerta in situ | ✅ Disponible |
| Raspberry Pi 5 | Gateway IoT (Mosquitto, SQLite, IA) | ✅ Disponible |

## 👥 Equipo y roles

| Integrante | Rol | Contribuciones en Challenge #1 y #2 | Contribuciones en Challenge #3 |
|---|---|---|---|
| Manuel Carreño | Líder | Simulación del proyecto, montaje y servidor web/tablero de control | Firmware MQTT del ESP32, montaje físico, tomas del prototipo para el video |
| Camilo Gutiérrez | Colaborador | Grabación y edición del video, compra de materiales y montaje | Módulo de IA con Gemini, configuración de Ubidots y de los correos, edición del video |
| Daniel Sanabria | Colaborador | Documentación de la Wiki, evapotranspiración potencial (ET₀) y banco de pruebas | Gateway en la Raspberry Pi (Mosquitto, SQLite, reenvío a Ubidots), recolección de datos reales, Wiki |


## 🤖 Uso de Inteligencia Artificial

Este proyecto usó Claude (Anthropic) como apoyo en la redacción de la documentación, la verificación de fuentes climáticas y, en el Challenge #3, el guion del video. Google Gemini es un componente del sistema (módulo de IA), no una herramienta de apoyo al desarrollo. El detalle completo, con transcripción/enlace de la conversación y el proceso de validación por el equipo, está documentado en la **[página de Uso de IA de la wiki](../../wiki/07-Uso-de-IA)**, tal como lo exige el enunciado del reto.

## 📚 Referencias principales

Ver la lista completa en formato IEEE en la [página de Conclusiones y Referencias](../../wiki/05‐Conclusiones‐y‐Referencias) de la wiki.

---

<div align="center">

*Proyecto desarrollado para el curso Internet de las Cosas (IoT 262), Facultad de Ingeniería, Universidad de La Sabana, 2026-2.*

</div>
