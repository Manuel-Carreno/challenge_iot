"""
comun.py - Funciones compartidas por los scripts de Camilo (Challenge #3).

Todo lo que se calcula aqui sale de mediciones reales del ESP32 (CSV de la
recoleccion o tabla `lecturas` de SQLite). Ninguna funcion genera datos.

Contrato comun que se respeta (version 2, 30 oct 2026):
  - Seccion 5: encabezado exacto del CSV, campos vacios = dato invalido,
    identidad de fila = (boot, seq), reconstruccion de hora con el ancla de uptime.
  - Seccion 6: columnas de la tabla `lecturas` y columna `origen`.
  - Seccion 8: los intervalos de eventos.md se excluyen del analisis.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

# ---------------------------------------------------------------------------
# Constantes del prototipo (salen de monitoreo_registro.ino; si el .ino cambia,
# se cambian aqui)
# ---------------------------------------------------------------------------
ENCABEZADO_CSV = [
    "ts_dispositivo", "uptime_s", "boot", "seq", "temp_c", "hum_pct", "pres_hpa",
    "nivel_pct", "dist_cm", "luz_adc", "et0_mm_dia", "puntaje", "estado",
    "alarma_silenciada", "fallo_hcsr04", "fallo_bme280",
]
NIVEL_CRITICO_PCT = 30.0          # NIVEL_ALERTA_MIN del .ino: por debajo, riesgoNivel = 2
LATITUD_GRADOS = 4.86             # LATITUD_GRADOS del .ino (cambiar si el prototipo esta en otro punto)
KRS = 0.16                        # KRS del .ino (FAO-56, ec. 50, zona interior)
ZONA_LOCAL = "America/Bogota"     # la linea de base se hace por hora LOCAL del dia
INTERVALO_REGISTRO_S = 300        # valor del contrato (5 min); el real se mide con intervalo_registro()

COLUMNAS_NUM = [
    "temp_c", "hum_pct", "pres_hpa", "nivel_pct", "dist_cm", "luz_adc",
    "et0_mm_dia", "puntaje",
]


# ---------------------------------------------------------------------------
# 1. Carga del CSV de la recoleccion y reconstruccion de la hora
# ---------------------------------------------------------------------------
def cargar_csv(rutas: list[str | Path]) -> pd.DataFrame:
    """Lee uno o varios CSV del ESP32 (o el consolidado .xlsx), quita duplicados por (boot, seq) y
    reconstruye la hora real de cada fila.

    Columnas que agrega:
      t_utc      : pandas Timestamp en UTC (NaT si el boot no tuvo ninguna hora NTP)
      hora_reconstruida : 1 si la hora salio del ancla de uptime, 0 si vino de NTP
      t_local    : la misma hora en America/Bogota
    """
    partes = []
    for r in rutas:
        if str(r).lower().endswith((".xlsx", ".xls")):
            # Consolidado en Excel (primera hoja). Requiere: py -m pip install openpyxl
            df = pd.read_excel(r, sheet_name=0, dtype=str).fillna("")
        else:
            # El ESP32 / Excel puede guardar con ',' o con ';': se detecta por el encabezado
            with open(r, encoding="utf-8-sig") as f:
                encabezado = f.readline()
            sep = ";" if encabezado.count(";") > encabezado.count(",") else ","
            df = pd.read_csv(r, dtype=str, keep_default_na=False, sep=sep, encoding="utf-8-sig")
        df.columns = [str(c).strip() for c in df.columns]
        df = df.apply(lambda col: col.str.strip())
        faltan = [c for c in ENCABEZADO_CSV if c not in df.columns]
        if faltan:
            raise ValueError(f"{r}: faltan columnas del contrato: {faltan}")
        partes.append(df[ENCABEZADO_CSV])
    df = pd.concat(partes, ignore_index=True)

    # Campo vacio = dato invalido (contrato, seccion 5). Se vuelve NaN, nunca 0.
    df = df.replace("", np.nan)
    df = df.dropna(how="all")  # filas totalmente vacias (p. ej. ';;;;;' por un corte al escribir)
    for c in ["ts_dispositivo", "uptime_s", "boot", "seq", "estado",
              "alarma_silenciada", "fallo_hcsr04", "fallo_bme280"]:
        df[c] = pd.to_numeric(df[c], errors="coerce").astype("Int64")
    for c in COLUMNAS_NUM:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    # Los CSV diarios se solapan (cada dia trae todo el archivo): (boot, seq) es la identidad.
    df = df.drop_duplicates(subset=["boot", "seq"], keep="last")
    df = df.sort_values(["boot", "seq"]).reset_index(drop=True)

    df = reconstruir_hora(df)
    df.attrs["intervalo_s"] = intervalo_registro(df)
    return df


def intervalo_registro(df: pd.DataFrame) -> float:
    """Paso real entre filas (mediana dentro de cada boot). El contrato dice 300 s, pero el
    archivo manda: si el ESP32 registro cada 30 s, todo calculo de cobertura usa 30 s."""
    pasos = []
    for _, g in df.dropna(subset=["t_utc"]).groupby("boot"):
        d = g["t_utc"].sort_values().diff().dt.total_seconds().dropna()
        pasos.extend(d[d > 0].tolist())
    return float(np.median(pasos)) if pasos else float(INTERVALO_REGISTRO_S)


def reconstruir_hora(df: pd.DataFrame) -> pd.DataFrame:
    """Contrato, seccion 5: dentro de un mismo boot, la primera fila con
    ts_dispositivo > 0 es el ancla; hora = ts_ancla + (uptime_s - uptime_ancla)."""
    df = df.copy()
    ts = pd.Series(np.nan, index=df.index, dtype="float64")
    recon = pd.Series(0, index=df.index, dtype="int64")
    for boot, g in df.groupby("boot", sort=False):
        con_hora = g[g["ts_dispositivo"].fillna(0) > 0]
        if con_hora.empty:
            continue  # boot sin ninguna hora: sus filas quedan sin hora (no se usan en analisis temporales)
        ancla = con_hora.iloc[0]
        ts_ancla = float(ancla["ts_dispositivo"])
        up_ancla = float(ancla["uptime_s"])
        for i, fila in g.iterrows():
            if pd.notna(fila["ts_dispositivo"]) and fila["ts_dispositivo"] > 0:
                ts.at[i] = float(fila["ts_dispositivo"])
            elif pd.notna(fila["uptime_s"]):
                ts.at[i] = ts_ancla + (float(fila["uptime_s"]) - up_ancla)
                recon.at[i] = 1
    df["t_utc"] = pd.to_datetime(ts, unit="s", utc=True)
    df["hora_reconstruida"] = recon
    df["t_local"] = df["t_utc"].dt.tz_convert(ZONA_LOCAL)
    return df


# ---------------------------------------------------------------------------
# 2. Carga desde SQLite (tabla `lecturas` del contrato, seccion 6)
# ---------------------------------------------------------------------------
def cargar_sqlite(ruta_db: str | Path, origenes=("csv", "mqtt")) -> pd.DataFrame:
    """Abre la base SOLO LECTURA y devuelve las lecturas reales.
    Nunca incluye origen = 'prueba' (contrato, seccion 6)."""
    import sqlite3

    uri = f"file:{Path(ruta_db).as_posix()}?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    try:
        marcas = ",".join("?" for _ in origenes)
        df = pd.read_sql_query(
            f"SELECT * FROM lecturas WHERE origen IN ({marcas}) AND recibido_en IS NOT NULL "
            f"ORDER BY recibido_en",
            con, params=list(origenes),
        )
    finally:
        con.close()
    df["t_utc"] = pd.to_datetime(df["recibido_en"], utc=True, errors="coerce", format="ISO8601")
    df = df.dropna(subset=["t_utc"]).sort_values("t_utc").reset_index(drop=True)
    df["t_local"] = df["t_utc"].dt.tz_convert(ZONA_LOCAL)
    return df


# ---------------------------------------------------------------------------
# 3. Bitacora eventos.md -> intervalos a excluir
# ---------------------------------------------------------------------------
@dataclass
class Evento:
    inicio: pd.Timestamp   # UTC
    fin: pd.Timestamp      # UTC
    tipo: str
    descripcion: str


_PATRON_FECHA = re.compile(r"\d{4}-\d{2}-\d{2}[ T]\d{1,2}:\d{2}")


def leer_eventos(ruta: str | Path | None) -> list[Evento]:
    """Lee las filas de tabla de eventos.md con este formato (hora LOCAL de Colombia):

    | inicio           | fin              | tipo               | descripcion                 |
    | ---------------- | ---------------- | ------------------ | --------------------------- |
    | 2026-11-04 14:20 | 2026-11-04 14:35 | descenso_provocado | Saque 1 L con una jarra     |

    Tipos sugeridos: corte_energia, reinicio, mantenimiento, cambio_lugar,
    llenado, descenso_provocado, sensor_desconectado, otro.
    Las lineas que no tengan dos fechas en las dos primeras celdas se ignoran.
    """
    if not ruta:
        return []
    p = Path(ruta)
    if not p.exists():
        raise FileNotFoundError(f"No encuentro la bitacora de eventos: {p}")
    eventos = []
    for linea in p.read_text(encoding="utf-8-sig").splitlines():
        if "|" not in linea:
            continue
        celdas = [c.strip() for c in linea.strip().strip("|").split("|")]
        if len(celdas) < 2 or not (_PATRON_FECHA.fullmatch(celdas[0]) and _PATRON_FECHA.fullmatch(celdas[1])):
            continue
        ini = pd.Timestamp(celdas[0]).tz_localize(ZONA_LOCAL).tz_convert("UTC")
        fin = pd.Timestamp(celdas[1]).tz_localize(ZONA_LOCAL).tz_convert("UTC")
        if fin < ini:
            raise ValueError(f"eventos.md: el fin es anterior al inicio en la linea: {linea}")
        tipo = celdas[2] if len(celdas) > 2 else "otro"
        desc = celdas[3] if len(celdas) > 3 else ""
        eventos.append(Evento(ini, fin, tipo, desc))
    return eventos


def mascara_eventos(t_utc: pd.Series, eventos: list[Evento], margen_min: int = 15) -> pd.Series:
    """True en las filas que caen dentro de un evento (con un margen despues del fin,
    para que el agua se asiente y el sensor se estabilice)."""
    m = pd.Series(False, index=t_utc.index)
    margen = pd.Timedelta(minutes=margen_min)
    for e in eventos:
        m |= (t_utc >= e.inicio) & (t_utc <= e.fin + margen)
    return m


def datos_para_analisis(df: pd.DataFrame, eventos: list[Evento]) -> pd.DataFrame:
    """Filas con hora conocida y fuera de los intervalos de eventos.md."""
    d = df.dropna(subset=["t_utc"]).copy()
    d = d[~mascara_eventos(d["t_utc"], eventos)]
    d = d.sort_values("t_utc").reset_index(drop=True)
    d.attrs = dict(df.attrs)
    return d


# ---------------------------------------------------------------------------
# 4. Linea de base por hora del dia (mediana y MAD) y anomalia robusta
# ---------------------------------------------------------------------------
# Piso de la desviacion robusta: evita que una hora con MAD = 0 marque anomalia
# por cualquier decima. Son del orden de la resolucion practica del BME280 en
# un montaje casero; se declaran como criterio del equipo, no de la literatura.
PISO_DESVIACION = {"temp_c": 0.3, "hum_pct": 1.5, "pres_hpa": 0.5}
UMBRAL_Z_ROBUSTO = 3.5  # Iglewicz y Hoaglin (1993): |z modificado| > 3.5 = posible atipico


def linea_base_por_hora(d: pd.DataFrame, variable: str) -> pd.DataFrame:
    """Para cada hora local 0..23: mediana, MAD escalada (x1.4826, comparable a una
    desviacion estandar si los datos fueran normales) y numero de lecturas."""
    x = d[["t_local", variable]].dropna()
    g = x.groupby(x["t_local"].dt.hour)[variable]
    med = g.median()
    mad = g.apply(lambda s: float(np.median(np.abs(s - np.median(s))))) * 1.4826
    tabla = pd.DataFrame({"mediana": med, "mad_escalada": mad, "n": g.size()})
    tabla.index.name = "hora_local"
    return tabla.reindex(range(24))


def z_robusto(valor: float, hora: int, base: pd.DataFrame, variable: str) -> float | None:
    fila = base.loc[hora] if hora in base.index else None
    if fila is None or pd.isna(fila["mediana"]) or pd.isna(valor):
        return None
    sigma = max(float(fila["mad_escalada"]), PISO_DESVIACION.get(variable, 0.0))
    if sigma <= 0:
        return None
    return (float(valor) - float(fila["mediana"])) / sigma


MIN_LECTURAS_HORA = 20  # con menos lecturas en esa hora del dia, se usa la linea de base global


def z_con_respaldo(valor, hora: int, base_hora: pd.DataFrame, historico: pd.DataFrame,
                   variable: str) -> tuple[float | None, str]:
    """z robusto contra la linea de base de ESA hora del dia si tiene datos suficientes;
    si no (p. ej. una hora en la que nunca se midio), contra la de TODO el historico.
    Devuelve (z, 'hora' | 'global' | 'sin datos')."""
    if valor is None or pd.isna(valor):
        return None, "sin datos"
    fila = base_hora.loc[hora] if hora in base_hora.index else None
    if fila is not None and pd.notna(fila["mediana"]) and (fila["n"] or 0) >= MIN_LECTURAS_HORA:
        return z_robusto(valor, hora, base_hora, variable), "hora"
    s = historico[variable].dropna().astype(float)
    if s.empty:
        return None, "sin datos"
    med = float(s.median())
    sigma = max(float((s - med).abs().median() * 1.4826), PISO_DESVIACION.get(variable, 0.0))
    return ((float(valor) - med) / sigma if sigma > 0 else None), "global"


def sesiones(d: pd.DataFrame, hueco_s: float = 600) -> pd.Series:
    """Numero de sesion continua de cada fila (una sesion se corta con un hueco > hueco_s)."""
    t = d["t_utc"]
    return (t.sort_values().diff().dt.total_seconds() > hueco_s).cumsum().reindex(t.index)


def es_anomalia(z: float | None) -> int:
    return int(z is not None and abs(z) > UMBRAL_Z_ROBUSTO)


# ---------------------------------------------------------------------------
# 5. Tasa de descenso del nivel y horas hasta el nivel critico
# ---------------------------------------------------------------------------
@dataclass
class ResultadoTasa:
    tasa_pct_h: float | None        # pendiente robusta (Theil-Sen), % por hora
    ic_bajo: float | None
    ic_alto: float | None
    ruido_pct: float | None         # desviacion robusta de los residuos
    n: int
    descenso_claro: bool
    horas_a_critico: float | None
    nivel_actual: float | None
    motivo: str


def altura_tanque(d: pd.DataFrame) -> float | None:
    """Altura configurada en el firmware, deducida de las propias filas: nivel = (H - dist) / H.
    (En la recoleccion real salio 40 cm, no los 27 cm del contrato.)"""
    x = d.dropna(subset=["dist_cm", "nivel_pct"])
    x = x[(x["nivel_pct"] > 1) & (x["nivel_pct"] < 99)]
    if x.empty:
        return None
    return float(np.median(x["dist_cm"] / (1 - x["nivel_pct"] / 100.0)))


def nivel_compensado(d: pd.DataFrame) -> pd.Series:
    """Nivel corregido por la temperatura del aire. El firmware convierte el eco con 343 m/s fijos;
    la velocidad real es 331.3 + 0.606*T m/s, asi que al enfriarse el aire el tanque 'parece' bajar.
    Si falta la temperatura o la distancia de una fila, se deja el nivel crudo."""
    H = altura_tanque(d)
    nivel = d["nivel_pct"].astype(float).copy()
    if H is None or "temp_c" not in d or "dist_cm" not in d:
        return nivel
    ok = d["dist_cm"].notna() & d["temp_c"].notna()
    dist = d.loc[ok, "dist_cm"].astype(float) * (331.3 + 0.606 * d.loc[ok, "temp_c"].astype(float)) / 343.0
    nivel.loc[ok] = ((H - dist) / H * 100.0).clip(0, 100)
    return nivel


MIN_HORAS_TASA = 3.0     # con menos de 3 h seguidas, una pendiente pequena es ruido o efecto de temperatura...
CAIDA_RAPIDA_PCT = 3.0   # ...salvo una caida grande (>= 3 % del tanque), como una extraccion o una fuga


def tasa_nivel(d: pd.DataFrame, ventana_h: float = 6.0, hasta: pd.Timestamp | None = None,
               min_lecturas: int = 12, horizonte_max_h: float = 168.0,
               min_horas: float = MIN_HORAS_TASA) -> ResultadoTasa:
    """Pendiente robusta del nivel en las ultimas `ventana_h` horas.

    'Descenso claro' exige las tres cosas:
      1. pendiente negativa con el intervalo de confianza del 95 % entero por debajo de 0,
      2. caida total en la ventana mayor que 2 veces el ruido del HC-SR04 medido en esa ventana,
      3. al menos `min_lecturas` lecturas validas que cubran al menos `min_horas` horas.
    El nivel se compensa por temperatura antes de calcular (nivel_compensado).
    Solo entonces se extrapola hasta NIVEL_CRITICO_PCT. Si la extrapolacion pasa de
    `horizonte_max_h` (una semana), se devuelve None: es mas lejos que todo lo observado.
    """
    from scipy.stats import theilslopes

    x = d.dropna(subset=["t_utc", "nivel_pct"]).copy()
    if hasta is not None:
        x = x[x["t_utc"] <= hasta]
    if not x.empty:
        x["nivel_pct"] = nivel_compensado(x)
    if x.empty:
        return ResultadoTasa(None, None, None, None, 0, False, None, None, "sin lecturas de nivel")
    fin = x["t_utc"].iloc[-1]
    x = x[x["t_utc"] >= fin - pd.Timedelta(hours=ventana_h)]
    nivel_actual = float(x["nivel_pct"].iloc[-1])
    n = len(x)
    if n < min_lecturas:
        return ResultadoTasa(None, None, None, None, n, False, None, nivel_actual,
                             f"solo {n} lecturas en {ventana_h} h")
    horas = (x["t_utc"] - x["t_utc"].iloc[0]).dt.total_seconds().to_numpy() / 3600.0
    y = x["nivel_pct"].to_numpy(dtype=float)
    if horas[-1] <= 0:
        return ResultadoTasa(None, None, None, None, n, False, None, nivel_actual, "ventana sin duracion")
    corta = horas[-1] < min_horas
    pend, intercepto, ic_b, ic_a = theilslopes(y, horas, 0.95)
    resid = y - (intercepto + pend * horas)
    ruido = float(np.median(np.abs(resid - np.median(resid))) * 1.4826)
    caida = -pend * horas[-1]
    claro = bool(pend < 0 and ic_a < 0 and caida > 2.0 * max(ruido, 1e-9))
    if corta and claro and caida < CAIDA_RAPIDA_PCT:
        return ResultadoTasa(float(pend), float(ic_b), float(ic_a), ruido, n, False, None, nivel_actual,
                             f"datos insuficientes: {horas[-1]:.1f} h seguidas (minimo {min_horas:.0f} h) "
                             f"y caida de {caida:.1f} % (< {CAIDA_RAPIDA_PCT:.0f} %)")
    hc = None
    motivo = "descenso claro" if claro else "sin descenso claro (pendiente dentro del ruido)"
    if claro:
        if nivel_actual <= NIVEL_CRITICO_PCT:
            hc = 0.0
            motivo = "nivel ya en zona critica"
        else:
            hc = (nivel_actual - NIVEL_CRITICO_PCT) / (-pend)
            if hc > horizonte_max_h:
                motivo = f"descenso claro pero la extrapolacion supera {horizonte_max_h:.0f} h"
                hc = None
    return ResultadoTasa(float(pend), float(ic_b), float(ic_a), ruido, n, claro, hc, nivel_actual, motivo)


# ---------------------------------------------------------------------------
# 6. ET0 diario recalculado desde el CSV
# ---------------------------------------------------------------------------
def radiacion_extraterrestre(lat_grados: float, dia_juliano: int) -> float:
    """Ra en MJ m-2 dia-1 (FAO-56, ecuaciones 21 a 25). Igual que calcularRa() del .ino."""
    phi = math.radians(lat_grados)
    dr = 1 + 0.033 * math.cos(2 * math.pi * dia_juliano / 365)
    delta = 0.409 * math.sin(2 * math.pi * dia_juliano / 365 - 1.39)
    ws = math.acos(-math.tan(phi) * math.tan(delta))
    return (24 * 60 / math.pi) * 0.0820 * dr * (
        ws * math.sin(phi) * math.sin(delta) + math.cos(phi) * math.cos(delta) * math.sin(ws))


def et0_diario(d: pd.DataFrame, lat_grados: float = LATITUD_GRADOS,
               cobertura_min: float = 0.8) -> pd.DataFrame:
    """ET0 por dia local con dos metodos, usando Tmax y Tmin del DIA COMPLETO
    (no la ventana de 5 minutos del firmware) y el dia juliano real.

      et0_turc_mm : la misma formula de calcularET0Turc() del .ino (Turc con la
                    correccion de humedad de Xu y Singh y Rs de la ec. 50 de FAO-56),
                    para comparar directamente con lo que calcula el dispositivo.
      et0_hs_mm   : Hargreaves-Samani (FAO-56, ec. 52), referencia cuando solo hay temperatura.

    Solo se calcula en dias con al menos `cobertura_min` de las lecturas esperadas
    (288 por dia a 5 minutos). Advertencia: el sensor esta en un montaje casero y
    probablemente bajo techo, asi que el valor describe ese microclima, no una estacion.
    """
    x = d.dropna(subset=["t_local", "temp_c"]).copy()
    x["dia"] = x["t_local"].dt.date
    esperadas = 86400 / d.attrs.get("intervalo_s", INTERVALO_REGISTRO_S)
    filas = []
    for dia, g in x.groupby("dia"):
        cobertura = len(g) / esperadas
        tmax, tmin = float(g["temp_c"].max()), float(g["temp_c"].min())
        tmed = (tmax + tmin) / 2
        hr = float(g["hum_pct"].mean()) if g["hum_pct"].notna().any() else float("nan")
        dj = pd.Timestamp(dia).dayofyear
        ra = radiacion_extraterrestre(lat_grados, dj)
        rs = KRS * math.sqrt(max(tmax - tmin, 0.0)) * ra
        k = 1.0 + (50.0 - hr) / 70.0 if (not math.isnan(hr) and hr < 50) else 1.0
        turc = max(0.013 * (tmed / (tmed + 15)) * (23.8846 * rs + 50) * k, 0.0)
        hs = max(0.0023 * (tmed + 17.8) * math.sqrt(max(tmax - tmin, 0.0)) * 0.408 * ra, 0.0)
        ok = cobertura >= cobertura_min
        filas.append({
            "dia": dia, "lecturas": len(g), "cobertura": round(cobertura, 3),
            "tmax_c": round(tmax, 2), "tmin_c": round(tmin, 2), "hr_media_pct": round(hr, 1),
            "dia_juliano": dj, "ra_mj": round(ra, 2), "rs_mj": round(rs, 2),
            "et0_turc_mm": round(turc, 2) if ok else None,
            "et0_hs_mm": round(hs, 2) if ok else None,
            "et0_firmware_mediana_mm": round(float(g["et0_mm_dia"].median()), 2)
            if g["et0_mm_dia"].notna().any() else None,
        })
    return pd.DataFrame(filas)


# ---------------------------------------------------------------------------
# 7. Riesgo a 24 h: regla fija y explicable (NO la decide el modelo generativo)
# ---------------------------------------------------------------------------
def riesgo_24h(horas_a_critico: float | None, estado_actual: int | None, anomalia: int) -> str:
    """ALTO  : nivel critico en <= 24 h, o el ESP32 ya esta en CRITICO.
    MEDIO : nivel critico en <= 72 h, o el ESP32 esta en ALERTA, o hay anomalia termica/higrica.
    BAJO  : nada de lo anterior.
    La regla apoya la recomendacion; las alertas oficiales siguen siendo las del ESP32."""
    if (horas_a_critico is not None and horas_a_critico <= 24) or estado_actual == 2:
        return "ALTO"
    if (horas_a_critico is not None and horas_a_critico <= 72) or estado_actual == 1 or anomalia:
        return "MEDIO"
    return "BAJO"