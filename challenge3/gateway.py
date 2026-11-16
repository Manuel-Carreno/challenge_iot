#!/usr/bin/env python3
"""
gateway.py (v2) - Gateway Raspberry Pi 5 del sistema de riesgo hidrico.

Flujo:  ESP32 --MQTT local--> gateway.py --> SQLite --> Ubidots (MQTT)
1. Recibe la telemetria del ESP32, la valida y la guarda en SQLite.
2. Calcula tasa_nivel_pct_h: pendiente (recta de minimos cuadrados) del nivel
   en los ultimos 30 minutos, en % por hora.
3. Reenvia a Ubidots (dispositivo nodo01) una lectura cada UBIDOTS_INTERVALO_S
   segundos, y de inmediato si cambian 'estado' o 'fallo_sensor'. Cada envio
   lleva la hora original de la lectura. Si no hay internet, las filas quedan
   con enviado_nube = 0 y se envian al volver.
4. Releva el comando de silenciar: Ubidots (nodo01-ctrl/silenciar_alarma = 1)
   -> topic local cmd/silenciar -> ESP32, y regresa la variable a 0.
5. Reenvia el resultado de la IA (topic local 'ia') a nodo01-ctrl.

Toda la configuracion y las credenciales vienen de variables de entorno.
Si falta UBIDOTS_TOKEN, el Gateway funciona solo en modo local (pasos 1 y 2).
"""
import json
import logging
import os
import sqlite3
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from logging.handlers import RotatingFileHandler

import paho.mqtt.client as mqtt

# ---------------- Configuracion (variables de entorno) ----------------
BASE = os.path.dirname(os.path.abspath(__file__))
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PUERTO = int(os.getenv("MQTT_PUERTO", "1883"))
MQTT_USUARIO = os.getenv("MQTT_USUARIO", "gateway")
MQTT_CLAVE = os.getenv("MQTT_CLAVE")
DB_PATH = os.getenv("DB_PATH", os.path.join(BASE, "lecturas.db"))
ORIGEN = os.getenv("GATEWAY_ORIGEN", "mqtt")          # 'mqtt' o 'prueba'
LOG_PATH = os.getenv("GATEWAY_LOG", os.path.join(BASE, "gateway.log"))

UBIDOTS_TOKEN = os.getenv("UBIDOTS_TOKEN", "")
UBIDOTS_HOST = os.getenv("UBIDOTS_HOST", "industrial.api.ubidots.com")
UBIDOTS_TLS = os.getenv("UBIDOTS_TLS", "1") == "1"
UBIDOTS_PUERTO = int(os.getenv("UBIDOTS_PUERTO", "8883" if UBIDOTS_TLS else "1883"))
UBIDOTS_CA = os.getenv("UBIDOTS_CA") or None        # ruta a un .pem, opcional
UBIDOTS_DISP = os.getenv("UBIDOTS_DISPOSITIVO", "nodo01")
UBIDOTS_DISP_CTRL = os.getenv("UBIDOTS_DISPOSITIVO_CTRL", "nodo01-ctrl")
UBIDOTS_INTERVALO_S = int(os.getenv("UBIDOTS_INTERVALO_S", "300"))
UBIDOTS_PAUSA_S = float(os.getenv("UBIDOTS_PAUSA_S", "1.1"))   # limite de envios por segundo
UBIDOTS_MAX_ANTIGUEDAD_H = float(os.getenv("UBIDOTS_MAX_ANTIGUEDAD_H", "24"))

VENTANA_TASA_MIN = 30      # ventana de la regresion del nivel
TASA_MIN_PUNTOS = 5        # minimo de lecturas para calcular la pendiente
TASA_MIN_SPAN_MIN = 5      # minimo de minutos cubiertos por la ventana

TOPIC_BASE = "sabana/hidrico/nodo01"
TOPIC_TELEMETRIA = TOPIC_BASE + "/telemetria"
TOPIC_ESTADO = TOPIC_BASE + "/estado_dispositivo"
TOPIC_CMD_SILENCIAR = TOPIC_BASE + "/cmd/silenciar"
TOPIC_IA = TOPIC_BASE + "/ia"

# ---------------- Contrato comun (secciones 4 y 6) ----------------
ESQUEMA = """
CREATE TABLE IF NOT EXISTS lecturas (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  origen TEXT NOT NULL DEFAULT 'mqtt',
  recibido_en TEXT,
  hora_reconstruida INTEGER NOT NULL DEFAULT 0,
  ts_dispositivo INTEGER,
  boot INTEGER, uptime_s INTEGER,
  seq INTEGER,
  temp_c REAL, hum_pct REAL, pres_hpa REAL,
  nivel_pct REAL, dist_cm REAL, luz_adc INTEGER, et0_mm_dia REAL,
  puntaje REAL, estado INTEGER, alarma_silenciada INTEGER,
  fallo_hcsr04 INTEGER, fallo_bme280 INTEGER, rssi INTEGER,
  tasa_nivel_pct_h REAL,
  enviado_nube INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_lecturas_recibido ON lecturas(recibido_en);
CREATE INDEX IF NOT EXISTS idx_lecturas_pend ON lecturas(enviado_nube);
CREATE UNIQUE INDEX IF NOT EXISTS idx_csv_unico ON lecturas(boot, seq) WHERE origen = 'csv';
"""

CAMPOS = ["seq", "ts_dispositivo", "temp_c", "hum_pct", "pres_hpa",
          "nivel_pct", "dist_cm", "luz_adc", "et0_mm_dia", "puntaje",
          "estado", "alarma_silenciada", "fallo_hcsr04", "fallo_bme280", "rssi"]
BOOLEANOS = ["alarma_silenciada", "fallo_hcsr04", "fallo_bme280"]

# Rangos fisicamente posibles (minimo, maximo). None = sin limite.
RANGOS = {
    "temp_c": (-40, 85),        # rango del BME280
    "hum_pct": (0, 100),
    "pres_hpa": (300, 1100),    # rango del BME280
    "nivel_pct": (0, 100),
    "dist_cm": (0, 400),        # alcance del HC-SR04
    "luz_adc": (0, 4095),       # ADC de 12 bits del ESP32
    "et0_mm_dia": (0, 20),
    "puntaje": (0, None),
    "rssi": (-120, 0),
}

# ---------------- Log ----------------
log = logging.getLogger("gateway")
log.setLevel(logging.INFO)
_fmt = logging.Formatter("%(asctime)s %(levelname)s [%(threadName)s] %(message)s")
for _h in (logging.StreamHandler(sys.stdout),
           RotatingFileHandler(LOG_PATH, maxBytes=1_000_000, backupCount=3)):
    _h.setFormatter(_fmt)
    log.addHandler(_h)

estado_seq = {"ultimo": None, "perdidos": 0}


# ---------------- Utilidades de tiempo ----------------
def iso_utc(dt):
    return dt.isoformat(timespec="seconds").replace("+00:00", "Z")


def iso_a_epoch(texto):
    return datetime.fromisoformat(texto.replace("Z", "+00:00")).timestamp()


# ---------------- Validacion ----------------
def es_numero(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def es_entero(v):
    return isinstance(v, int) and not isinstance(v, bool)


def validar(d):
    """Devuelve (datos_validos, None) o (None, motivo_del_rechazo)."""
    if not isinstance(d, dict):
        return None, "el JSON no es un objeto"
    faltan = [c for c in CAMPOS if c not in d]
    if faltan:
        return None, f"faltan campos: {faltan}"
    for c in ("seq", "ts_dispositivo", "estado"):
        if not es_entero(d[c]):
            return None, f"{c} debe ser entero, llego {d[c]!r}"
    if d["seq"] < 0 or d["ts_dispositivo"] < 0:
        return None, "seq o ts_dispositivo negativos"
    if d["estado"] not in (0, 1, 2):
        return None, f"estado invalido: {d['estado']}"
    for c in BOOLEANOS:
        if not isinstance(d[c], bool):
            return None, f"{c} debe ser true/false, llego {d[c]!r}"
    for c, (minimo, maximo) in RANGOS.items():
        v = d[c]
        if v is None:
            continue
        if not es_numero(v):
            return None, f"{c} no es numero: {v!r}"
        if (minimo is not None and v < minimo) or (maximo is not None and v > maximo):
            return None, f"{c} fuera de rango: {v}"
    return d, None


def revisar_seq(seq):
    ultimo = estado_seq["ultimo"]
    if ultimo is not None:
        if seq > ultimo + 1:
            hueco = seq - ultimo - 1
            estado_seq["perdidos"] += hueco
            log.warning("Hueco de seq: %d -> %d (%d perdidos; total %d)",
                        ultimo, seq, hueco, estado_seq["perdidos"])
        elif seq <= ultimo:
            log.warning("seq bajo de %d a %d: posible reinicio del ESP32", ultimo, seq)
    estado_seq["ultimo"] = seq


# ---------------- Procesamiento: tasa de descenso del nivel ----------------
def pendiente_pct_h(puntos):
    """puntos: lista de (epoch_s, nivel_pct). Pendiente de la recta de minimos
    cuadrados en % por hora, o None si no hay datos suficientes."""
    if len(puntos) < TASA_MIN_PUNTOS:
        return None
    puntos = sorted(puntos)
    t0 = puntos[0][0]
    xs = [(t - t0) / 3600.0 for t, _ in puntos]
    ys = [n for _, n in puntos]
    if xs[-1] - xs[0] < TASA_MIN_SPAN_MIN / 60.0:
        return None
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    return round(sxy / sxx, 3)


def calcular_tasa(db, ahora, nivel):
    corte = iso_utc(ahora - timedelta(minutes=VENTANA_TASA_MIN))
    filas = db.execute(
        "SELECT recibido_en, nivel_pct FROM lecturas "
        "WHERE origen = ? AND recibido_en >= ? AND nivel_pct IS NOT NULL",
        (ORIGEN, corte)).fetchall()
    puntos = [(iso_a_epoch(r), n) for r, n in filas]
    if nivel is not None:
        puntos.append((ahora.timestamp(), nivel))
    return pendiente_pct_h(puntos)


# ---------------- SQLite ----------------
def abrir_db():
    db = sqlite3.connect(DB_PATH, timeout=10)
    db.execute("PRAGMA journal_mode=WAL")
    db.executescript(ESQUEMA)
    db.commit()
    return db


def guardar(db, d):
    def b(v):
        return None if v is None else int(v)
    ahora = datetime.now(timezone.utc).replace(microsecond=0)
    tasa = calcular_tasa(db, ahora, d["nivel_pct"])
    with db:
        db.execute(
            """INSERT INTO lecturas (origen, recibido_en, ts_dispositivo, seq,
                 temp_c, hum_pct, pres_hpa, nivel_pct, dist_cm, luz_adc, et0_mm_dia,
                 puntaje, estado, alarma_silenciada, fallo_hcsr04, fallo_bme280, rssi,
                 tasa_nivel_pct_h)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (ORIGEN, iso_utc(ahora), d["ts_dispositivo"], d["seq"],
             d["temp_c"], d["hum_pct"], d["pres_hpa"], d["nivel_pct"], d["dist_cm"],
             b(d["luz_adc"]), d["et0_mm_dia"], d["puntaje"], d["estado"],
             b(d["alarma_silenciada"]), b(d["fallo_hcsr04"]), b(d["fallo_bme280"]),
             b(d["rssi"]), tasa))
    return tasa


# ---------------- Reenvio a Ubidots ----------------
class ReenvioUbidots:
    """Cliente MQTT hacia Ubidots, en su propio hilo, con su propia conexion SQLite."""

    def __init__(self, cliente_local):
        self.local = cliente_local
        self.despertador = threading.Event()
        self.conectado = threading.Event()
        self.lock = threading.Lock()
        self.ia_pendiente = None
        self.ultimo_enviado = None      # (epoch, estado, fallo_sensor)
        self.topic_datos = f"/v1.6/devices/{UBIDOTS_DISP}"
        self.topic_ctrl = f"/v1.6/devices/{UBIDOTS_DISP_CTRL}"
        self.topic_silenciar = f"/v1.6/devices/{UBIDOTS_DISP_CTRL}/silenciar_alarma/lv"

        self.c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                             client_id=f"gateway-hidrico-{UBIDOTS_DISP}")
        self.c.username_pw_set(UBIDOTS_TOKEN, "")
        if UBIDOTS_TLS:
            self.c.tls_set(ca_certs=UBIDOTS_CA)
        self.c.on_connect = self._on_connect
        self.c.on_disconnect = self._on_disconnect
        self.c.on_message = self._on_message
        self.c.reconnect_delay_set(min_delay=1, max_delay=60)

    def iniciar(self):
        log.info("Ubidots: %s:%d (TLS=%s), dispositivos %s y %s, intervalo %d s",
                 UBIDOTS_HOST, UBIDOTS_PUERTO, UBIDOTS_TLS, UBIDOTS_DISP,
                 UBIDOTS_DISP_CTRL, UBIDOTS_INTERVALO_S)
        self.c.connect_async(UBIDOTS_HOST, UBIDOTS_PUERTO, keepalive=60)
        self.c.loop_start()
        threading.Thread(target=self._bucle, name="reenvio", daemon=True).start()

    # --- callbacks ---
    def _on_connect(self, c, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("Ubidots rechazo la conexion: %s (revisa el token)", reason_code)
            return
        log.info("Conectado a Ubidots")
        self.conectado.set()
        c.subscribe(self.topic_silenciar, qos=1)
        self.despertador.set()

    def _on_disconnect(self, c, userdata, flags, reason_code, properties):
        self.conectado.clear()
        log.warning("Desconectado de Ubidots (%s); las lecturas quedan pendientes", reason_code)

    def _on_message(self, c, userdata, msg):
        try:
            if msg.topic != self.topic_silenciar:
                return
            texto = msg.payload.decode("utf-8", errors="replace").strip()
            try:
                valor = float(texto)
            except ValueError:
                log.warning("Valor de silenciar_alarma no numerico: %r", texto)
                return
            if valor < 0.5:
                return
            info = self.local.publish(TOPIC_CMD_SILENCIAR,
                                      json.dumps({"silenciar": True}), qos=1)
            log.info("SILENCIAR: Ubidots=%s -> publicado en %s (rc=%s)",
                     texto, TOPIC_CMD_SILENCIAR, info.rc)
            # Regresar el interruptor a 0 para el siguiente uso
            c.publish(self.topic_ctrl, json.dumps({"silenciar_alarma": 0}), qos=1)
        except Exception:
            log.exception("Error en el relevo de silenciar")

    # --- interfaz para el hilo principal ---
    def despertar(self):
        self.despertador.set()

    def recibir_ia(self, datos):
        with self.lock:
            self.ia_pendiente = datos
        self.despertador.set()

    # --- envio ---
    def _publicar(self, topic, carga):
        if not self.conectado.is_set():
            return False
        info = self.c.publish(topic, json.dumps(carga), qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            return False
        try:
            info.wait_for_publish(timeout=10)
        except (RuntimeError, ValueError):
            return False
        return info.is_published()

    def _bucle(self):
        db = sqlite3.connect(DB_PATH, timeout=10)
        while True:
            self.despertador.wait(timeout=30)
            self.despertador.clear()
            try:
                self._enviar_ia()
                self._enviar_pendientes(db)
            except Exception:
                log.exception("Error en el ciclo de reenvio; se reintenta")

    def _enviar_ia(self):
        with self.lock:
            datos = self.ia_pendiente
        if datos is None or not self.conectado.is_set():
            return
        horas = datos.get("horas_a_critico")
        horas = -1 if not es_numero(horas) else horas     # contrato v3: null -> -1
        carga = {
            "ia_horas_a_critico": {
                "value": horas,
                "context": {
                    "texto": str(datos.get("recomendacion", ""))[:500],
                    "riesgo_24h": str(datos.get("riesgo_24h", "")),
                    "fuente": str(datos.get("fuente", "")),
                },
            },
            "ia_anomalia": {"value": 1 if datos.get("anomalia") else 0},
        }
        if self._publicar(self.topic_ctrl, carga):
            with self.lock:
                if self.ia_pendiente is datos:
                    self.ia_pendiente = None
            log.info("IA reenviada a %s: horas_a_critico=%s", UBIDOTS_DISP_CTRL, horas)
            time.sleep(UBIDOTS_PAUSA_S)

    def _carga_lectura(self, f, epoch, fallo):
        (_id, _rec, temp, hum, pres, nivel, luz, et0, puntaje, estado, tasa, _fh, _fb) = f
        ts = int(epoch * 1000)            # Ubidots usa milisegundos
        valores = {
            "temperatura": temp, "humedad": hum, "presion": pres, "nivel": nivel,
            "luz": luz, "et0": et0, "puntaje_riesgo": puntaje, "estado": estado,
            "tasa_nivel": tasa, "fallo_sensor": fallo,
        }
        return {k: {"value": v, "timestamp": ts} for k, v in valores.items() if v is not None}

    def _enviar_pendientes(self, db):
        if not self.conectado.is_set():
            return
        filas = db.execute(
            """SELECT id, recibido_en, temp_c, hum_pct, pres_hpa, nivel_pct, luz_adc,
                      et0_mm_dia, puntaje, estado, tasa_nivel_pct_h, fallo_hcsr04, fallo_bme280
               FROM lecturas
               WHERE enviado_nube = 0 AND origen != 'csv'
               ORDER BY id LIMIT 2000""").fetchall()
        if not filas:
            return
        limite = time.time() - UBIDOTS_MAX_ANTIGUEDAD_H * 3600
        procesado_hasta, enviados, viejas = None, 0, 0
        for f in filas:
            fid, recibido, estado = f[0], f[1], f[9]
            epoch = iso_a_epoch(recibido)
            fallo = 1 if (f[11] or f[12]) else 0
            if epoch < limite:
                viejas += 1
                procesado_hasta = fid
                continue
            u = self.ultimo_enviado
            cambio = u is not None and (estado != u[1] or fallo != u[2])
            toca = u is None or (epoch - u[0]) >= UBIDOTS_INTERVALO_S - 5 or cambio
            if toca:
                if not self._publicar(self.topic_datos, self._carga_lectura(f, epoch, fallo)):
                    log.warning("Fallo el envio a Ubidots; se reintenta en el siguiente ciclo")
                    break
                self.ultimo_enviado = (epoch, estado, fallo)
                enviados += 1
                log.info("Enviada a Ubidots la fila id=%d (%s)%s", fid, recibido,
                         " por cambio de estado/fallo" if cambio else "")
                time.sleep(UBIDOTS_PAUSA_S)
            procesado_hasta = fid
        if procesado_hasta is not None:
            with db:
                db.execute("UPDATE lecturas SET enviado_nube = 1 "
                           "WHERE enviado_nube = 0 AND origen != 'csv' AND id <= ?",
                           (procesado_hasta,))
        if viejas:
            log.warning("%d filas pendientes de mas de %.0f h no se enviaron (solo locales)",
                        viejas, UBIDOTS_MAX_ANTIGUEDAD_H)


# ---------------- Callbacks MQTT locales (API version 2 de paho-mqtt) ----------------
def on_connect(cliente, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        log.error("Conexion al broker local rechazada: %s", reason_code)
        return
    log.info("Conectado a %s:%d como '%s'", MQTT_HOST, MQTT_PUERTO, MQTT_USUARIO)
    cliente.subscribe([(TOPIC_TELEMETRIA, 0), (TOPIC_ESTADO, 0), (TOPIC_IA, 1)])


def on_disconnect(cliente, userdata, flags, reason_code, properties):
    log.warning("Desconectado del broker local (%s); reintentando...", reason_code)


def on_message(cliente, userdata, msg):
    try:
        texto = msg.payload.decode("utf-8", errors="replace")
        reenvio = userdata.get("reenvio")
        if msg.topic == TOPIC_ESTADO:
            log.info("Estado del ESP32: %s", texto)
            return
        if msg.topic == TOPIC_IA:
            try:
                datos = json.loads(texto)
            except json.JSONDecodeError as e:
                log.warning("JSON de IA malformado (%s): %.200s", e, texto)
                return
            if not isinstance(datos, dict):
                log.warning("JSON de IA no es un objeto: %.200s", texto)
                return
            log.info("Resultado de IA recibido (fuente=%s)", datos.get("fuente"))
            if reenvio:
                reenvio.recibir_ia(datos)
            return
        if msg.topic != TOPIC_TELEMETRIA:
            return
        try:
            datos = json.loads(texto)
        except json.JSONDecodeError as e:
            log.warning("JSON malformado (%s): %.200s", e, texto)
            return
        d, error = validar(datos)
        if error:
            log.warning("Lectura rechazada: %s | %.200s", error, texto)
            return
        revisar_seq(d["seq"])
        tasa = guardar(userdata["db"], d)
        log.info("Guardada seq=%d origen=%s nivel=%s tasa=%s %%/h temp=%s estado=%d",
                 d["seq"], ORIGEN, d["nivel_pct"], tasa, d["temp_c"], d["estado"])
        if reenvio:
            reenvio.despertar()
    except Exception:
        log.exception("Error inesperado procesando el mensaje; el servicio sigue")


def main():
    if not MQTT_CLAVE:
        log.error("Falta la variable de entorno MQTT_CLAVE")
        sys.exit(1)
    if ORIGEN not in ("mqtt", "prueba"):
        log.error("GATEWAY_ORIGEN debe ser 'mqtt' o 'prueba', llego %r", ORIGEN)
        sys.exit(1)

    db = abrir_db()
    log.info("Base: %s | origen de lo que llegue por MQTT: %s", DB_PATH, ORIGEN)

    cliente = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                          client_id="gateway-hidrico")
    cliente.username_pw_set(MQTT_USUARIO, MQTT_CLAVE)
    cliente.on_connect = on_connect
    cliente.on_disconnect = on_disconnect
    cliente.on_message = on_message
    cliente.reconnect_delay_set(min_delay=1, max_delay=30)

    reenvio = None
    if UBIDOTS_TOKEN:
        reenvio = ReenvioUbidots(cliente)
    else:
        log.warning("Sin UBIDOTS_TOKEN: modo solo local (no se reenvia a la nube)")
    cliente.user_data_set({"db": db, "reenvio": reenvio})

    cliente.connect_async(MQTT_HOST, MQTT_PUERTO, keepalive=30)
    if reenvio:
        reenvio.iniciar()
    try:
        cliente.loop_forever(retry_first_connection=True)
    except KeyboardInterrupt:
        log.info("Detenido por el usuario. Mensajes perdidos detectados: %d",
                 estado_seq["perdidos"])
    finally:
        db.close()


if __name__ == "__main__":
    main()
