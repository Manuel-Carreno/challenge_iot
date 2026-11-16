"""
ia_modulo.py - Paso 8 de la guia de Camilo: modulo de IA que corre en la Raspberry Pi.

Que hace en cada ciclo (por defecto cada 15 minutos):
  1. Abre la base SQLite del Gateway en SOLO LECTURA y toma las lecturas reales
     (origen 'csv' y 'mqtt'; nunca 'prueba'), sin los intervalos de eventos.md.
  2. Estadistica local, predecible y verificable (comun.py):
       - linea de base por hora del dia (mediana y MAD) de temperatura y humedad,
         y anomalia si la lectura actual se aleja mas de 3.5 desviaciones robustas;
       - tasa de descenso del nivel (Theil-Sen, ultimas 6 h) y horas hasta el 30 %,
         solo si el descenso es claro (fuera del ruido del HC-SR04);
       - riesgo a 24 h con una regla fija (BAJO / MEDIO / ALTO).
  3. Le pasa SOLO esos numeros al modelo generativo (Gemini) y le pide una
     recomendacion corta en espanol, en JSON. El modelo no decide ningun numero:
     los numeros del JSON final los pone este script.
  4. Chequeo automatico: si la recomendacion menciona una cifra que no esta en los
     datos de entrada, se descarta y se usa la plantilla fija.
  5. Si la API falla, no hay clave o no hay internet, usa la plantilla fija.
  6. Publica en sabana/hidrico/nodo01/ia (QoS 1, retenido) con el formato del contrato,
     y guarda cada ciclo en ia_log.jsonl para la validacion.

Regla de diseno: este modulo NUNCA dispara ni cancela una alerta. Las alertas son de la
logica de fusion del ESP32.

Configuracion (variables de entorno; en la Pi van en /etc/gateway-hidrico.env, permisos 600):
  DB_PATH            ruta a la base SQLite del Gateway
  EVENTOS_MD         ruta a eventos.md (opcional pero recomendado)
  GEMINI_API_KEY     clave de la API (NUNCA en el codigo ni en el repositorio)
  GEMINI_MODEL       nombre del modelo; confirmar el vigente y su cuota gratuita en ai.google.dev
  MQTT_HOST, MQTT_PUERTO, MQTT_USUARIO, MQTT_CLAVE   broker local Mosquitto (usuario 'gateway')
  IA_INTERVALO_MIN   minutos entre ciclos (por defecto 15)
  IA_LOG             ruta del registro jsonl (por defecto ia_log.jsonl)

Uso:
  python ia_modulo.py --csv datos_2026-11-05.csv --eventos eventos.md --una-vez --imprimir --sin-llm
                                                    # portatil, sin base: directo del CSV real
  python ia_modulo.py --csv datos_finales_recoleccion.xlsx --una-vez --imprimir
                                                    # en la Pi: historia del archivo + lecturas en vivo de SQLite
  python ia_modulo.py --una-vez --imprimir          # un ciclo, muestra el JSON, no publica
  python ia_modulo.py --una-vez --imprimir --sin-llm  # igual, solo plantilla (sin API)
  python ia_modulo.py                               # servicio: un ciclo cada IA_INTERVALO_MIN
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import pandas as pd

import comun

TOPIC_IA = "sabana/hidrico/nodo01/ia"
ESTADOS = {0: "NORMAL", 1: "ALERTA", 2: "CRITICO"}
MAX_CARACTERES = 280
EDAD_MAX_MIN = 30  # si la ultima lectura es mas vieja, se dice en la recomendacion


# ---------------------------------------------------------------------------
# Configuracion
# ---------------------------------------------------------------------------
def cargar_env(ruta: str | None):
    """Lee un archivo KEY=valor sin depender de python-dotenv. No pisa variables ya definidas."""
    if not ruta or not Path(ruta).exists():
        return
    for linea in Path(ruta).read_text(encoding="utf-8-sig").splitlines():  # utf-8-sig: tolera el BOM del Bloc de notas
        linea = linea.strip()
        if not linea or linea.startswith("#") or "=" not in linea:
            continue
        k, v = linea.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def _entero(x) -> int:
    """0 si el campo viene vacio o nulo; nunca rompe el ciclo por un dato faltante."""
    try:
        return 0 if x is None or pd.isna(x) else int(x)
    except (TypeError, ValueError):
        return 0


# ---------------------------------------------------------------------------
# 1 y 2. Datos y estadistica
# ---------------------------------------------------------------------------
def calcular(db_path: str | None, eventos_md: str | None, csvs: list[str] | None = None) -> dict:
    # 'mqtt' siempre, salvo IA_ORIGEN_VIVO=prueba en la sesion 1 (se lee aqui, despues de cargar el .env)
    ORIGEN_VIVO = os.environ.get("IA_ORIGEN_VIVO", "mqtt")
    partes = []
    if csvs:
        # Historia real de la recoleccion (CSV o el consolidado .xlsx). Sirve sola en el portatil, o junto
        # con la base de la Pi: asi no hace falta importar el archivo a SQLite para que la IA lo use.
        hist = comun.cargar_csv(csvs).dropna(subset=["t_utc"]).copy()
        hist["origen"] = "csv"
        partes.append(hist)
    if db_path:
        # IA_ORIGEN_VIVO=prueba solo en la sesion 1: las lecturas del simulador hacen de "lectura en vivo".
        # La linea de base sigue saliendo SOLO de las filas reales (csv).
        vivo = comun.cargar_sqlite(db_path, origenes=("csv", "mqtt", ORIGEN_VIVO))
        if csvs:
            vivo = vivo[vivo["origen"] != "csv"]  # la historia ya viene del archivo: sin duplicar
        partes.append(vivo)
    df = pd.concat(partes, ignore_index=True, sort=False) if partes else pd.DataFrame()
    if not df.empty:
        df = df.sort_values("t_utc").reset_index(drop=True)
        if csvs:
            df.attrs = dict(partes[0].attrs)
    if df.empty:
        raise RuntimeError("La base no tiene lecturas reales (origen csv o mqtt).")
    eventos = comun.leer_eventos(eventos_md) if eventos_md else []
    d = comun.datos_para_analisis(df, eventos)

    # Linea de base: con la semana de recoleccion (origen csv). Si aun no hay csv, con todo lo real.
    historico = d[d["origen"] == "csv"] if (d["origen"] == "csv").any() else d
    base_t = comun.linea_base_por_hora(historico, "temp_c")
    base_h = comun.linea_base_por_hora(historico, "hum_pct")

    # Lectura "actual": la MQTT en vivo mas reciente; si aun no hay en vivo, la ultima del CSV
    # (en ese caso la edad sale grande y la recomendacion dice que los datos estan desactualizados)
    vivas = df[df["origen"] == ORIGEN_VIVO]
    ultima = (vivas if not vivas.empty else df).iloc[-1]
    ahora = pd.Timestamp.now(tz="UTC")
    edad_min = max((ahora - ultima["t_utc"]).total_seconds() / 60, 0.0)
    hora_local = int(ultima["t_local"].hour)

    z_t, base_usada_t = comun.z_con_respaldo(ultima.get("temp_c"), hora_local, base_t, historico, "temp_c")
    z_h, base_usada_h = comun.z_con_respaldo(ultima.get("hum_pct"), hora_local, base_h, historico, "hum_pct")
    anomalia = int(comun.es_anomalia(z_t) or comun.es_anomalia(z_h))

    # Tasa de nivel: lecturas de 10 s se reducen a medianas por minuto antes de Theil-Sen
    fuente_nivel = d[d["origen"] == ORIGEN_VIVO] if not vivas.empty else d
    # Medianas por minuto (lecturas de 10 s) de nivel, distancia y temperatura: la tasa usa las tres
    # para compensar el efecto de la temperatura sobre la velocidad del sonido (comun.nivel_compensado)
    cols = [c for c in ["nivel_pct", "dist_cm", "temp_c"] if c in fuente_nivel]
    nivel = (fuente_nivel.dropna(subset=["nivel_pct"]).set_index("t_utc")[cols]
             .apply(pd.to_numeric, errors="coerce").resample("1min").median()
             .dropna(subset=["nivel_pct"]).reset_index())
    tasa = comun.tasa_nivel(nivel, ventana_h=6.0)

    estado = None if pd.isna(ultima.get("estado")) else int(ultima["estado"])
    riesgo = comun.riesgo_24h(tasa.horas_a_critico, estado, anomalia)

    def r(x, n=1):
        return None if x is None or pd.isna(x) else round(float(x), n)

    return {
        "lectura": {
            "hora_local": ultima["t_local"].strftime("%Y-%m-%d %H:%M"),
            "edad_min": r(edad_min, 0),
            "origen": ultima.get("origen"),
            "temp_c": r(ultima.get("temp_c")), "hum_pct": r(ultima.get("hum_pct")),
            "pres_hpa": r(ultima.get("pres_hpa")), "nivel_pct": r(ultima.get("nivel_pct")),
            "estado": ESTADOS.get(estado, "DESCONOCIDO"),
            "fallo_hcsr04": _entero(ultima.get("fallo_hcsr04")),
            "fallo_bme280": _entero(ultima.get("fallo_bme280")),
        },
        "linea_base_hora": {
            "hora": hora_local,
            "temp_mediana_c": r(base_t.loc[hora_local, "mediana"]) if hora_local in base_t.index else None,
            "hum_mediana_pct": r(base_h.loc[hora_local, "mediana"]) if hora_local in base_h.index else None,
            "z_temp": r(z_t, 2), "z_hum": r(z_h, 2),
            "base_usada": base_usada_t,  # 'hora' (misma hora del dia) o 'global' (sin datos de esa hora)
            "lecturas_historicas": int(len(historico)),
        },
        "nivel": {
            "tasa_pct_h": r(tasa.tasa_pct_h, 3), "ruido_pct": r(tasa.ruido_pct, 2),
            "descenso_claro": tasa.descenso_claro, "motivo": tasa.motivo,
            "horas_a_critico": r(tasa.horas_a_critico, 1), "umbral_critico_pct": comun.NIVEL_CRITICO_PCT,
        },
        "anomalia": anomalia,
        "riesgo_24h": riesgo,
    }


# ---------------------------------------------------------------------------
# 3 y 5. Recomendacion: plantilla fija y modelo generativo
# ---------------------------------------------------------------------------
def plantilla(c: dict) -> str:
    l, n = c["lectura"], c["nivel"]
    partes = [f"Riesgo {c['riesgo_24h']} en 24 h. Nivel {l['nivel_pct']} %, estado {l['estado']}."]
    if n["horas_a_critico"] is not None:
        partes.append(f"Al ritmo actual llegaria al {n['umbral_critico_pct']:.0f} % en unas {n['horas_a_critico']} h: "
                      "programar racionamiento y verificar fugas.")
    else:
        partes.append("Sin descenso claro del nivel.")
    if c["anomalia"]:
        partes.append("Temperatura o humedad fuera de lo habitual para esta hora: vigilar evaporacion.")
    if l["fallo_hcsr04"] or l["fallo_bme280"]:
        partes.append("Hay un sensor en falla: revisar el equipo en sitio.")
    if l["edad_min"] is not None and l["edad_min"] > EDAD_MAX_MIN:
        partes.append(f"Ultima lectura de hace {l['edad_min']:.0f} min: datos desactualizados.")
    return " ".join(partes)[:MAX_CARACTERES]


INSTRUCCION_SISTEMA = (
    "Eres un asistente para autoridades municipales de la Sabana Centro (Cundinamarca, Colombia) "
    "durante el fenomeno de El Nino. Recibes SOLO numeros calculados por un sistema IoT que mide un "
    "tanque de agua. Escribe una recomendacion operativa en espanol, de maximo 2 frases y 280 caracteres, "
    "para alguien que no es ingeniero. Reglas: usa solo los numeros que te doy, sin inventar ni redondear "
    "a otros valores; si 'horas_a_critico' es null, no menciones un plazo; no digas que activaste o "
    "cancelaste alarmas; si 'datos_desactualizados' o 'sensor_en_falla' es true, la primera frase lo dice "
    "y la accion es verificar el equipo; si 'tendencia' dice SIN descenso claro, no hables de tendencia a la "
    "baja, descenso ni disminucion del nivel. "
    "La primera frase dice la situacion con a lo sumo dos cifras; la segunda frase es UNA accion concreta "
    "que la autoridad puede hacer hoy (por ejemplo: programar racionamiento por sectores, revisar fugas, "
    "verificar el sensor en sitio, mantener la vigilancia), proporcional al riesgo_24h. "
    'Responde solo con JSON: {"recomendacion": "..."}'
)


def entrada_modelo(c: dict) -> dict:
    """Lo que ve el modelo: los mismos datos, pero con conclusiones explicitas en vez de numeros
    ambiguos. Sin descenso claro, la pendiente no se le pasa (la leia como 'tendencia a la baja')."""
    m = json.loads(json.dumps(c))
    n, l = m["nivel"], m["lectura"]
    if not n.get("descenso_claro"):
        n["tasa_pct_h"] = None
        n["ruido_pct"] = None
        n["tendencia"] = "SIN descenso claro del nivel (estable o datos insuficientes)"
    else:
        n["tendencia"] = "descenso claro del nivel"
    l["datos_desactualizados"] = bool(l.get("edad_min") is not None and l["edad_min"] > EDAD_MAX_MIN)
    l["sensor_en_falla"] = bool(l.get("fallo_hcsr04") or l.get("fallo_bme280"))
    return m


def llamar_llm(c: dict, modelo: str, clave: str) -> str:
    """Llama a Gemini con el SDK google-genai. Confirmar en ai.google.dev/gemini-api/docs:
    el nombre del modelo vigente, la cuota gratuita y la forma de pedir salida JSON."""
    from google import genai  # pip install google-genai

    cliente = genai.Client(api_key=clave)
    respuesta = cliente.models.generate_content(
        model=modelo,
        contents=json.dumps(entrada_modelo(c), ensure_ascii=False),
        config={
            "system_instruction": INSTRUCCION_SISTEMA,
            "response_mime_type": "application/json",
            "temperature": 0.2,
            "max_output_tokens": 300,
        },
    )
    texto = (respuesta.text or "").strip()
    datos = json.loads(texto)
    reco = str(datos["recomendacion"]).strip()
    if not reco:
        raise ValueError("recomendacion vacia")
    return reco[:MAX_CARACTERES]


_NUM = re.compile(r"-?\d+(?:[.,]\d+)?")


def numeros_permitidos(c: dict) -> list[float]:
    vals = [24.0, 72.0]  # horizontes de la regla de riesgo_24h
    def recorrer(x):
        if isinstance(x, dict):
            for v in x.values():
                recorrer(v)
        elif isinstance(x, (int, float)) and not isinstance(x, bool):
            vals.append(float(x))
            vals.append(abs(float(x)))  # "descenso de 2.5 %/h" para una tasa de -2.5 es correcto
    recorrer(c)
    return vals


def _coincide(texto_num: str, v: float, p: float) -> bool:
    """Igual al dato, o el dato redondeado a los decimales que escribio el modelo (15.2 -> '15')."""
    decimales = len(texto_num.replace(",", ".").split(".")[1]) if ("." in texto_num or "," in texto_num) else 0
    return abs(v - round(p, decimales)) < 1e-9 or abs(v - p) < 1e-9


_HABLA_DE_BAJADA = re.compile(r"(tendencia a la baja|tendencia descendente|est[aá] bajando|bajando|en descenso|"
                              r"descenso de|disminuy|disminuci[oó]n del nivel|ca[ií]da del nivel)", re.I)
_DICE_DESACTUALIZADO = re.compile(r"(desactualizad|antig|no se han recibido|sin datos recientes|"
                                  r"[uú]ltima lectura|hace \d+|no est[aá] reportando|sin reportar)", re.I)


def chequear_coherencia(texto: str, c: dict) -> tuple[bool, list[str]]:
    """Revisa lo que la frase AFIRMA, no solo sus cifras: que no invente una bajada cuando la
    estadistica dijo 'sin descenso claro' y que avise cuando los datos estan viejos."""
    problemas = []
    if not c["nivel"].get("descenso_claro"):
        sin_negar = _HABLA_DE_BAJADA.sub(lambda mt: "" if re.search(r"\b(sin|no hay|ni)\s*$",
                                         texto[:mt.start()], re.I) else mt.group(0), texto)
        if _HABLA_DE_BAJADA.search(sin_negar):
            problemas.append("habla de bajada del nivel pero la estadistica dijo 'sin descenso claro'")
    edad = c["lectura"].get("edad_min")
    if edad is not None and edad > EDAD_MAX_MIN and not _DICE_DESACTUALIZADO.search(texto):
        problemas.append(f"no avisa que la ultima lectura tiene {edad:.0f} min")
    return (len(problemas) == 0, problemas)


def chequear_numeros(texto: str, c: dict) -> tuple[bool, list[str]]:
    """True si toda cifra del texto es un dato de entrada (o ese dato redondeado). Estricto a proposito:
    con tolerancias amplias, un 2.5 inventado pasaria por parecerse a otro numero."""
    permitidos = numeros_permitidos(c)
    malos = []
    for m in _NUM.findall(texto):
        v = float(m.replace(",", "."))
        if not any(_coincide(m, v, p) for p in permitidos):
            malos.append(m)
    return (len(malos) == 0, malos)


# ---------------------------------------------------------------------------
# 6. Publicacion MQTT
# ---------------------------------------------------------------------------
def publicar(payload: dict):
    import paho.mqtt.client as mqtt  # pip install paho-mqtt

    host = os.environ.get("MQTT_HOST", "localhost")
    puerto = int(os.environ.get("MQTT_PUERTO", "1883"))
    try:  # paho-mqtt 2.x
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ia_modulo")
    except AttributeError:  # paho-mqtt 1.x
        cli = mqtt.Client(client_id="ia_modulo")
    if os.environ.get("MQTT_USUARIO"):
        cli.username_pw_set(os.environ["MQTT_USUARIO"], os.environ.get("MQTT_CLAVE", ""))
    cli.connect(host, puerto, keepalive=30)
    cli.loop_start()
    info = cli.publish(TOPIC_IA, json.dumps(payload, ensure_ascii=False), qos=1, retain=True)
    info.wait_for_publish(timeout=10)
    cli.loop_stop()
    cli.disconnect()
    if not info.is_published():
        raise RuntimeError("el broker no confirmo la publicacion (QoS 1)")


# ---------------------------------------------------------------------------
# Ciclo
# ---------------------------------------------------------------------------
def un_ciclo(args) -> dict:
    db = os.environ.get("DB_PATH") or args.db
    if not db and not args.csv:
        raise SystemExit("Falta DB_PATH (variable de entorno), --db o --csv")
    c = calcular(db, os.environ.get("EVENTOS_MD") or args.eventos, args.csv)

    modelo = os.environ.get("GEMINI_MODEL", "")
    clave = os.environ.get("GEMINI_API_KEY", "")
    fuente, motivo, chequeo = "estadistica+plantilla", None, None
    reco = plantilla(c)
    if args.sin_llm:
        motivo = "modo --sin-llm"
    elif not clave or not modelo:
        motivo = "falta GEMINI_API_KEY o GEMINI_MODEL"
    else:
        try:
            candidata = llamar_llm(c, modelo, clave)
            ok_num, malos = chequear_numeros(candidata, c)
            ok_coh, problemas = chequear_coherencia(candidata, c)
            chequeo = "ok" if (ok_num and ok_coh) else "; ".join(
                ([f"cifras no presentes en los datos: {malos}"] if not ok_num else []) + problemas)
            if ok_num and ok_coh:
                reco, fuente = candidata, "estadistica+llm"
            else:
                motivo = f"respuesta del modelo rechazada ({chequeo}): se usa la plantilla"
            registro_candidata = candidata
        except Exception as e:  # sin internet, cuota agotada, JSON invalido, etc.
            motivo = f"fallo la API: {type(e).__name__}: {e}"[:200]

    salida = {
        "ts": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "horas_a_critico": c["nivel"]["horas_a_critico"],
        "anomalia": c["anomalia"],
        "riesgo_24h": c["riesgo_24h"],
        "recomendacion": reco,
        "modelo": modelo if fuente == "estadistica+llm" else "plantilla",
        "fuente": fuente,
    }
    registro = {"entrada": c, "salida": salida, "chequeo_numeros": chequeo, "motivo_plantilla": motivo,
                "respuesta_modelo": locals().get("registro_candidata")}
    with open(os.environ.get("IA_LOG", args.log), "a", encoding="utf-8") as f:
        f.write(json.dumps(registro, ensure_ascii=False) + "\n")

    if args.imprimir:
        print(json.dumps(registro, ensure_ascii=False, indent=2))
    else:
        publicar(salida)
        print(f"[{salida['ts']}] publicado en {TOPIC_IA}: riesgo {salida['riesgo_24h']}, fuente {fuente}"
              + (f" ({motivo})" if motivo else ""))
    return salida


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", default=os.environ.get("IA_ENV", "/etc/gateway-hidrico.env"))
    ap.add_argument("--db", default=None, help="ruta a la base (si no esta DB_PATH)")
    ap.add_argument("--csv", nargs="+", default=None,
                    help="usar directamente uno o varios CSV del ESP32 en vez de la base (prueba en el portatil)")
    ap.add_argument("--eventos", default=None, help="ruta a eventos.md (si no esta EVENTOS_MD)")
    ap.add_argument("--log", default="ia_log.jsonl")
    ap.add_argument("--una-vez", action="store_true")
    ap.add_argument("--imprimir", action="store_true", help="muestra el resultado y NO publica por MQTT")
    ap.add_argument("--sin-llm", action="store_true", help="fuerza la plantilla (prueba de 'API caida')")
    a = ap.parse_args()
    cargar_env(a.env)

    if a.una_vez:
        un_ciclo(a)
        return
    minutos = float(os.environ.get("IA_INTERVALO_MIN", "15"))
    while True:
        try:
            un_ciclo(a)
        except Exception as e:  # el servicio no se cae por un ciclo malo
            print(f"Error en el ciclo: {type(e).__name__}: {e}", file=sys.stderr)
        time.sleep(minutos * 60)


if __name__ == "__main__":
    main()