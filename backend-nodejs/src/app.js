require("dotenv").config();
const express = require("express");
const cors    = require("cors");
const mqtt    = require("mqtt");
const path    = require("path");
const { Pool } = require("pg");

const CONFIG = {
  port:     process.env.PORT        || 3000,
  broker:   process.env.MQTT_BROKER || "mqtt://broker.hivemq.com:1883",
  topicAll: "viettel/sensor/all",
  topicAck: "viettel/sensor/ack",
  topicCmd: "viettel/sensor/cmd",
};

// ══════════════ DATABASE (POSTGRESQL) ══════════════
const pool = new Pool({
  host:     process.env.PGHOST,
  user:     process.env.PGUSER,
  password: process.env.PGPASSWORD,
  database: process.env.PGDATABASE,
  port:     process.env.PGPORT,
});

const initDB = async () => {
  try {
    await pool.query(`CREATE TABLE IF NOT EXISTS sensor_data (
      id SERIAL PRIMARY KEY,
      device_id TEXT DEFAULT 'stm32_01',
      ts TIMESTAMPTZ NOT NULL,
      voltage REAL, current REAL, power REAL,
      temperature REAL, humidity REAL, signal INTEGER,
      pressure REAL, pres_status TEXT,
      flow_rate REAL, total_fwd REAL, total_rev REAL,
      velocity REAL, mag_status TEXT
    )`);
    await pool.query(`CREATE INDEX IF NOT EXISTS idx_ts ON sensor_data(ts)`);
    await pool.query(`CREATE INDEX IF NOT EXISTS idx_dev ON sensor_data(device_id)`);
    console.log("🐘 DB: PostgreSQL Connected & Initialized");
  } catch (err) {
    console.error("❌ DB Error:", err.message);
  }
};
initDB();

const dbAll = (s, p=[]) => pool.query(s, p).then(r => r.rows);
const dbGet = (s, p=[]) => pool.query(s, p).then(r => r.rows[0]);

// ══════════════ MQTT 24/7 ══════════════
let mqttOK=false, totalRx=0, lastRx=null;

const mc = mqtt.connect(CONFIG.broker, {
  clientId: "backend_"+Math.random().toString(16).substr(2,8),
  reconnectPeriod: 5000,
  keepalive: 20,
});

mc.on("connect", () => {
  mqttOK = true;
  console.log(`✅ [${ts()}] MQTT → ${CONFIG.broker}`);
  mc.subscribe(CONFIG.topicAll, {qos:1});
});

mc.on("message", async (_, buf) => {
  try {
    const clean = buf.toString().replace(/:\s*nan/gi, ":null").replace(/:\s*-?inf/gi, ":null");
    const d = JSON.parse(clean);
    const dev = d.device_id || "stm32_01";
    const s = v => (v!=null && isFinite(v)) ? v : null;
    
    await pool.query(
      `INSERT INTO sensor_data(device_id,ts,voltage,current,power,temperature,humidity,signal,pressure,pres_status,flow_rate,total_fwd,total_rev,velocity,mag_status)
       VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15)`,
      [dev, new Date().toISOString(), s(d.voltage_ina), s(d.current_ina), s(d.power_ina),
       s(d.temperature), s(d.humidity), s(d.signal),
       s(d.pressure), d.pres_status,
       s(d.flow_rate), s(d.total_fwd), s(d.total_rev),
       s(d.velocity), d.mag_status]
    );
    totalRx++; lastRx = new Date().toISOString();
    mc.publish(CONFIG.topicAck, `${dev}:DISPLAY_OK`);
    process.stdout.write(`\r💾 #${totalRx} ${ts()} [${dev}] Received `);
  } catch(e) { console.error(`\n⚠️ ${e.message}`); }
});

mc.on("reconnect", () => { mqttOK=false; console.log(`\n🔄 [${ts()}] Reconnecting…`); });
mc.on("offline",   () => { mqttOK=false; });
mc.on("error",      e => { mqttOK=false; console.error(`\n❌ ${e.message}`); });

// ══════════════ HTTP API ══════════════
const app = express();
app.use(cors());
app.use(express.json());

app.get("/api/history", async (req,res) => {
  try {
    const dev   = req.query.device || "stm32_01";
    const hours = Math.min(+req.query.hours||24, 8760);
    const limit = Math.min(+req.query.limit||5000, 100000);
    const since = new Date(Date.now()-hours*3600000).toISOString();
    const rows  = await dbAll("SELECT * FROM sensor_data WHERE device_id=$1 AND ts>$2 ORDER BY ts ASC LIMIT $3", [dev,since,limit]);
    res.json({ count:rows.length, data:rows });
  } catch(e) { res.status(500).json({error:e.message}); }
});

app.get("/api/stats", async (req,res) => {
  try {
    const dev    = req.query.device || "stm32_01";
    const total  = (await dbGet("SELECT COUNT(*) n FROM sensor_data")).n;
    const today  = new Date().toISOString().slice(0,10);
    const todayN = (await dbGet("SELECT COUNT(*) n FROM sensor_data WHERE device_id=$1 AND ts>=$2",[dev,today+"T00:00:00Z"])).n;
    
    const a      = await dbGet(`SELECT 
      ROUND(AVG(voltage)::numeric,2) avg_v, ROUND(AVG(temperature)::numeric,2) avg_t,
      ROUND(MIN(temperature)::numeric,2) min_t, ROUND(MAX(temperature)::numeric,2) max_t,
      ROUND(AVG(pressure)::numeric,3) avg_p, ROUND(MIN(pressure)::numeric,3) min_p, ROUND(MAX(pressure)::numeric,3) max_p,
      ROUND(AVG(flow_rate)::numeric,4) avg_flow, ROUND(MAX(flow_rate)::numeric,4) max_flow,
      ROUND(MAX(total_fwd)::numeric,4) max_total_fwd, ROUND(MAX(total_rev)::numeric,4) max_total_rev,
      MIN(ts) first_ts, MAX(ts) last_ts FROM sensor_data WHERE device_id=$1`, [dev]);
    
    const devs   = await dbAll("SELECT device_id, MAX(ts) last_ts FROM sensor_data GROUP BY device_id");

    res.json({ total, today:todayN, mqtt:mqttOK, received:totalRx,
               last_rx:lastRx, uptime:Math.floor(process.uptime()), devices:devs, ...a });
  } catch(e) { res.status(500).json({error:e.message}); }
});

app.get("/api/health", async (req,res) => {
  const n = (await dbGet("SELECT COUNT(*) n FROM sensor_data")).n;
  res.json({ status:"ok", mqtt:mqttOK?"connected":"disconnected",
             records:n, last_rx:lastRx, uptime:Math.floor(process.uptime()) });
});

app.get("/api/export/csv", async (req,res) => {
  try {
    const hours = +req.query.hours||720;
    const since = new Date(Date.now()-hours*3600000).toISOString();
    const rows  = await dbAll("SELECT * FROM sensor_data WHERE ts>$1 ORDER BY ts",[since]);
    const csv   = ["id,timestamp,device_id,voltage_V,current_mA,power_mW,temperature_C,humidity_%,signal_CSQ,pressure_bar,pres_status,flow_rate_m3h,total_fwd_m3,total_rev_m3,velocity_ms,mag_status",
      ...rows.map(r=>`${r.id},${r.ts},${r.device_id},${r.voltage??''},${r.current??''},${r.power??''},${r.temperature??''},${r.humidity??''},${r.signal??''},${r.pressure??''},${r.pres_status??''},${r.flow_rate??''},${r.total_fwd??''},${r.total_rev??''},${r.velocity??''},${r.mag_status??''}`)
    ].join("\n");
    res.setHeader("Content-Type","text/csv;charset=utf-8");
    res.setHeader("Content-Disposition",`attachment;filename=esp32_${new Date().toISOString().slice(0,10)}.csv`);
    res.send("\uFEFF"+csv);
  } catch(e) { res.status(500).json({error:e.message}); }
});

app.post("/api/command", async (req,res) => {
  const { command, device } = req.body;
  if (!command) return res.status(400).json({ error: "Thiếu command" });
  if (!mqttOK) return res.status(503).json({ error: "MQTT chưa sẵn sàng" });
  const dev = device || "stm32_01";
  const topic = `${CONFIG.topicCmd}/${dev}`;
  mc.publish(topic, command, { qos: 1 });
  res.json({ success: true, command, topic });
});

// ══════════════ DASHBOARD HTML ══════════════
app.get("/", (req, res) => res.sendFile(path.join(__dirname, "index.html")));

// Auto cleanup 180 ngày
setInterval(async () => {
  const b = new Date(Date.now()-180*86400000).toISOString();
  try {
    const res = await pool.query("DELETE FROM sensor_data WHERE ts<$1", [b]);
    if (res.rowCount > 0) console.log(`\n🗑️ Auto-cleanup: ${res.rowCount} bản ghi`);
  } catch(e) {}
}, 24*3600000);

function ts() { return new Date().toLocaleTimeString("vi-VN"); }

app.listen(CONFIG.port, () => {
  console.log("\n╔════════════════════════════════════════╗");
  console.log("║  STM32 MAG8000 PG Server 24/7 (v3.2)   ║");
  console.log("╚════════════════════════════════════════╝");
  console.log(`🚀 API port: ${CONFIG.port}`);
  console.log(`🐘 DB: PostgreSQL`);
});