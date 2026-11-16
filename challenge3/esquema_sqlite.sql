-- Esquema de lecturas.db (tomado de gateway.py; igual en importar_csv.py)
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
