<div align="center">

# 💧 Sistema IoT de Monitoreo de Riesgo Hídrico
### Sabana Centro, Cundinamarca — Alerta temprana de desabastecimiento por El Niño

**IoT 262 · 2026-2 · Challenge #1 · Universidad de La Sabana**

**Camilo Gutierrez, Manuel Carreño, Daniel Sanabria**

[![Wiki](https://img.shields.io/badge/📖-Wiki_completa-blue)](../../wiki)
[![Wokwi](https://img.shields.io/badge/🔧-Simulación_Wokwi-orange)](#-simulación-en-wokwi)
[![Video](https://img.shields.io/badge/🎥-Video_demo-red)](#-video-demostrativo)
[![Estado](https://img.shields.io/badge/prototipo-completado-green)]()

</div>

---

## 🌎 El problema

En 2026, el Gobierno Nacional y el IDEAM confirmaron el inicio anticipado del fenómeno de El Niño, agravando la disponibilidad de agua dulce en la Sabana de Bogotá y Cundinamarca. La CAR identificó **20 municipios en riesgo extremo** de desabastecimiento hídrico por agotamiento de fuentes superficiales y reservorios locales.

**¿Cómo diseñar un sistema IoT que mida en tiempo real el nivel de agua y variables meteorológicas clave, fusione esas señales mediante lógica de alerta, y notifique a la comunidad de forma inmediata — sin redes de comunicación?**

Este repositorio contiene la respuesta de nuestro equipo a ese reto.

## ⚙️ ¿Qué hace el sistema?

Un ESP32 lee, cada 2 segundos, cuatro señales físicas de un punto de almacenamiento de agua:

| Sensor | Variable | Rol |
|---|---|---|
| 🔊 HC-SR04 | Nivel de agua | Disponibilidad física directa del recurso |
| 🌡️ DHT22 | Temperatura | Detecta anomalías de calor |
| 💧 DHT22 | Humedad relativa | Indicador de sequía atmosférica |
| ☀️ LDR | Radiación / luz | Proxy de evaporación potencial |

Estas señales se combinan en un **puntaje de riesgo ponderado**, y el sistema responde con una alerta **completamente local** — sin WiFi, sin LoRa, sin ninguna red — mediante:

- 🟢🟡🔴 LEDs de estado (Normal / Alerta / Crítico)
- 🔊 Buzzer sonoro en estado crítico
- 🖥️ Pantalla OLED con las lecturas en tiempo real

> **Nota:** el diseño original contemplaba un sensor BME280 (temp+hum+presión integrado). En la simulación de Wokwi se usó **DHT22 + BMP180** en su lugar, ya que el BME280 no está disponible en ese entorno. Ver el detalle en la [página 2 de la wiki](../../wiki/02‐Solucion‐Propuesta#20-nota-sobre-el-hardware-simulado-bme280--bmp180--dht22).

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

## 🎥 Video demostrativo

> **⬅️ Pegar aquí el link del video (máx. 5 min), reproducible sin necesidad de descarga dentro de MS Teams, con todos los integrantes en cámara.**

[![Video demostrativo](https://img.shields.io/badge/▶️-Ver_video_demostrativo-critical?style=for-the-badge)](PEGAR_AQUÍ_EL_LINK_DEL_VIDEO)

## 🔧 Simulación en Wokwi

> **⬅️ Pegar aquí el link público del proyecto de Wokwi.**

[![Abrir en Wokwi](https://img.shields.io/badge/Wokwi-Abrir_simulación-00979D?style=for-the-badge)](PEGAR_AQUÍ_EL_LINK_DE_WOKWI)

## 🧩 Hardware usado

| Componente | Función | Estado |
|---|---|---|
| ESP32 DevKit | Microcontrolador central | ✅ Disponible |
| DHT22 | Temperatura + humedad (simulación Wokwi) | ✅ Disponible en simulación |
| BMP180 | Presión atmosférica (simulación Wokwi) | ✅ Disponible en simulación |
| HC-SR04 | Nivel de agua (ultrasónico) | ✅ Disponible |
| LDR | Radiación solar / luz | ✅ Disponible |
| OLED SSD1306 | Visualización local | ✅ Disponible |
| LEDs verde/amarillo/rojo + buzzer | Alerta in situ | ✅ Disponible |

## 👥 Equipo y roles

| Integrante | Rol | Contribución principal |
|---|---|---|
| Manuel Carreño | Líder | Simulación del proyecto y montaje |
| Camilo Gutiérrez | Colaborador | Grabación y edición del video, compra de materiales y montaje |
| Daniel Sanabria | Colaborador | Documentación de la Wiki, montaje y maqueta |

## 🤖 Uso de Inteligencia Artificial

Este proyecto usó Claude (Anthropic) como apoyo en la redacción de la documentación y la verificación de fuentes climáticas. El detalle completo, con transcripción/enlace de la conversación y el proceso de validación por el equipo, está documentado en la **[página de Uso de IA de la wiki](../../wiki/07-Uso-de-IA)**, tal como lo exige el enunciado del reto.

## 📚 Referencias principales

Ver la lista completa en formato IEEE en la [página de Conclusiones y Referencias](../../wiki/05‐Conclusiones‐y‐Referencias) de la wiki.

---

<div align="center">

*Proyecto desarrollado para el curso Internet de las Cosas (IoT 262), Facultad de Ingeniería, Universidad de La Sabana, 2026-2.*

</div>
