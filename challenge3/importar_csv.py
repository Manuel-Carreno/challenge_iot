#!/usr/bin/env python3
"""
importar_csv.py - Carga el CSV de la recoleccion real (Fase 1) a SQLite.
- Acepta separador ';' o ','. Campo vacio -> NULL.
- Reconstruye la hora de las filas con ts_dispositivo = 0 usando el ancla
  de su mismo boot: hora = ts_ancla + (uptime_s - uptime_ancla).
- Idempotente: INSERT OR IGNORE sobre el indice unico (boot, seq) de origen 'csv'.
- Imprime un informe de calidad de los datos.
Uso: python importar_csv.py datos_AAAA-MM-DD.csv [--db lecturas.db]
"""
import argparse
import csv
import os
import sqlite3
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

BASE = os.path.dirname(os.path.abspath(__file__))
TZ_LOCAL = ZoneInfo("America/Bogota")
HUECO_MIN_S = 600   # huecos de mas de 10 minutos

ENTEROS = ["ts_dispositivo", "uptime_s", "boot", "seq", "luz_adc", "estado"]
BOOLEANOS = ["alarma_silenciada", "fallo_hcsr04", "fallo_bme280"]
REALES = ["temp_c", "hum_pct", "pres_hpa", "nivel_pct", "dist_cm", "et0_mm_dia", "puntaje"]
COLUMNAS = ENTEROS + REALES + BOOLEANOS

# Mismo esquema que gateway.py (contrato comun, seccion 6)
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


def a_entero(t):
    t = (t or "").strip()
    return None if t == "" else int(float(t))


def a_real(t, sep):
    t = (t or "").strip()
    if t == "":
        return None
    if sep == ";":
        t = t.replace(",", ".")   # por si viene coma decimal
    return float(t)


def a_bool(t):
    t = (t or "").strip().lower()
    if t == "":
        return None
    if t in ("1", "true", "si", "yes"):
        return 1
    if t in ("0", "false", "no"):
        return 0
    raise ValueError(f"booleano invalido: {t!r}")


def convertir(r, sep):
    d = {c: a_entero(r[c]) for c in ENTEROS}
    d.update({c: a_real(r[c], sep) for c in REALES})
    d.update({c: a_bool(r[c]) for c in BOOLEANOS})
    for c in ("boot", "seq", "uptime_s"):
        if d[c] is None:
            raise ValueError(f"{c} vacio")
    if d["ts_dispositivo"] is None:
        d["ts_dispositivo"] = 0
    return d


def leer(ruta):
    with open(ruta, newline="", encoding="utf-8-sig") as f:
        primera = f.readline()
        sep = ";" if primera.count(";") > primera.count(",") else ","
        f.seek(0)
        lector = csv.DictReader(f, delimiter=sep)
        encabezado = [c.strip() for c in (lector.fieldnames or [])]
        lector.fieldnames = encabezado
        faltan = [c for c in COLUMNAS if c not in encabezado]
        if faltan:
            sys.exit(f"ERROR: faltan columnas {faltan}. Encabezado leido: {encabezado}")
        filas, rechazadas = [], []
        for n, r in enumerate(lector, start=2):
            try:
                filas.append(convertir(r, sep))
            except (ValueError, TypeError, KeyError) as e:
                rechazadas.append((n, str(e)))
    return sep, filas, rechazadas


def reconstruir(filas):
    """Agrega a cada fila '_epoch' (hora UTC en segundos o None) y '_rec' (0/1)."""
    por_boot = defaultdict(list)
    for f in filas:
        por_boot[f["boot"]].append(f)
    anclas = {}
    for boot, lista in por_boot.items():
        lista.sort(key=lambda f: f["seq"])
        ancla = next((f for f in lista if f["ts_dispositivo"] > 0), None)
        anclas[boot] = ancla
        for f in lista:
            if f["ts_dispositivo"] > 0:
                f["_epoch"], f["_rec"] = f["ts_dispositivo"], 0
            elif ancla is not None:
                f["_epoch"] = ancla["ts_dispositivo"] + (f["uptime_s"] - ancla["uptime_s"])
                f["_rec"] = 1
            else:
                f["_epoch"], f["_rec"] = None, 0
    return por_boot, anclas


def iso_utc(epoch):
    return datetime.fromtimestamp(epoch, timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def local(epoch):
    return datetime.fromtimestamp(epoch, TZ_LOCAL).strftime("%Y-%m-%d %H:%M")


def insertar(db, filas):
    cur = db.cursor()
    insertadas = 0
    with db:
        for f in filas:
            cur.execute(
                """INSERT OR IGNORE INTO lecturas
                   (origen, recibido_en, hora_reconstruida, ts_dispositivo, boot, uptime_s, seq,
                    temp_c, hum_pct, pres_hpa, nivel_pct, dist_cm, luz_adc, et0_mm_dia,
                    puntaje, estado, alarma_silenciada, fallo_hcsr04, fallo_bme280,
                    rssi, enviado_nube)
                   VALUES ('csv',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,NULL,1)""",
                (iso_utc(f["_epoch"]) if f["_epoch"] is not None else None, f["_rec"],
                 f["ts_dispositivo"], f["boot"], f["uptime_s"], f["seq"],
                 f["temp_c"], f["hum_pct"], f["pres_hpa"], f["nivel_pct"], f["dist_cm"],
                 f["luz_adc"], f["et0_mm_dia"], f["puntaje"], f["estado"],
                 f["alarma_silenciada"], f["fallo_hcsr04"], f["fallo_bme280"]))
            insertadas += cur.rowcount
    return insertadas


def informe(ruta, sep, filas, rechazadas, por_boot, anclas, insertadas, total_csv_db):
    p = print
    p("=" * 64)
    p(f"INFORME DE IMPORTACION: {os.path.basename(ruta)}")
    p("=" * 64)
    p(f"Separador detectado:        '{sep}'")
    p(f"Filas de datos leidas:      {len(filas) + len(rechazadas)}")
    p(f"Filas rechazadas (danadas): {len(rechazadas)}")
    for n, e in rechazadas[:10]:
        p(f"   linea {n}: {e}")
    p(f"Filas insertadas ahora:     {insertadas}")
    p(f"Filas ya existentes (no duplicadas): {len(filas) - insertadas}")
    p(f"Total filas 'csv' en la base:        {total_csv_db}")

    con_hora = [f for f in filas if f["_epoch"] is not None]
    p("")
    p(f"Con hora propia (NTP):      {sum(1 for f in filas if f['_epoch'] is not None and f['_rec'] == 0)}")
    p(f"Con hora reconstruida:      {sum(f['_rec'] for f in filas)}")
    p(f"Sin hora (no usar en analisis temporal): {len(filas) - len(con_hora)}")
    if con_hora:
        ep = sorted(f["_epoch"] for f in con_hora)
        p(f"Primera hora: {local(ep[0])} (Colombia) / {iso_utc(ep[0])}")
        p(f"Ultima hora:  {local(ep[-1])} (Colombia) / {iso_utc(ep[-1])}")
        p(f"Duracion cubierta: {(ep[-1] - ep[0]) / 3600:.1f} h")

    p("")
    p(f"Arranques (boot): {len(por_boot)}")
    for boot in sorted(por_boot):
        lista = por_boot[boot]
        seqs = [f["seq"] for f in lista]
        faltan_seq = (max(seqs) - min(seqs) + 1) - len(set(seqs))
        eps = [f["_epoch"] for f in lista if f["_epoch"] is not None]
        rango = f"{local(min(eps))} -> {local(max(eps))}" if eps else "SIN HORA"
        difs = [b - a for a, b in zip(eps, eps[1:]) if b > a]
        intervalo = f"{statistics.median(difs):.0f} s" if difs else "-"
        p(f"  boot {boot}: {len(lista)} filas, seq {min(seqs)}-{max(seqs)} "
          f"(faltan {faltan_seq}), ancla: {'si' if anclas[boot] else 'NO'}, "
          f"intervalo mediano {intervalo}, {rango}")

    p("")
    ordenadas = sorted(con_hora, key=lambda f: f["_epoch"])
    huecos = [(a, b) for a, b in zip(ordenadas, ordenadas[1:])
              if b["_epoch"] - a["_epoch"] > HUECO_MIN_S]
    p(f"Huecos de mas de {HUECO_MIN_S // 60} min: {len(huecos)}")
    for a, b in huecos[:20]:
        dur = (b["_epoch"] - a["_epoch"]) / 60
        p(f"  {local(a['_epoch'])} -> {local(b['_epoch'])}  ({dur:.0f} min) "
          f"boot {a['boot']}->{b['boot']}")
    if len(huecos) > 20:
        p(f"  ... y {len(huecos) - 20} mas")

    p("")
    p(f"Filas con fallo_hcsr04 = 1: {sum(1 for f in filas if f['fallo_hcsr04'] == 1)}")
    p(f"Filas con fallo_bme280 = 1: {sum(1 for f in filas if f['fallo_bme280'] == 1)}")
    p(f"Filas con nivel vacio:      {sum(1 for f in filas if f['nivel_pct'] is None)}")
    p(f"Filas con temperatura vacia: {sum(1 for f in filas if f['temp_c'] is None)}")
    p("=" * 64)


def main():
    ap = argparse.ArgumentParser(description="Importa el CSV de la recoleccion a SQLite")
    ap.add_argument("csv")
    ap.add_argument("--db", default=os.getenv("DB_PATH", os.path.join(BASE, "lecturas.db")))
    a = ap.parse_args()

    sep, filas, rechazadas = leer(a.csv)
    por_boot, anclas = reconstruir(filas)
    db = sqlite3.connect(a.db)
    db.execute("PRAGMA journal_mode=WAL")
    db.executescript(ESQUEMA)
    insertadas = insertar(db, filas)
    total = db.execute("SELECT COUNT(*) FROM lecturas WHERE origen='csv'").fetchone()[0]
    db.close()
    informe(a.csv, sep, filas, rechazadas, por_boot, anclas, insertadas, total)


if __name__ == "__main__":
    main()
