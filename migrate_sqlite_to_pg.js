require("dotenv").config();
const sqlite3 = require("sqlite3").verbose();
const { Pool } = require("pg");

const pgPool = new Pool({
  host:     process.env.PGHOST     || 'localhost',
  user:     process.env.PGUSER     || 'iot_user',
  password: process.env.PGPASSWORD || 'iot_password',
  database: process.env.PGDATABASE || 'iot_db',
  port:     process.env.PGPORT     || 5432,
});

async function migrate() {
  const db = new sqlite3.Database("sensors.db");
  console.log("📂 Đang đọc dữ liệu từ sensors.db...");

  db.all("SELECT * FROM sensor_data", async (err, rows) => {
    if (err) return console.error(err);
    console.log(`📦 Tìm thấy ${rows.length} bản ghi. Đang chuyển sang Postgres...`);

    for (const d of rows) {
      await pgPool.query(
        `INSERT INTO sensor_data(device_id,ts,voltage,current,power,temperature,humidity,signal,pressure,pres_status,flow_rate,total_fwd,total_rev,velocity,mag_status)
         VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15)`,
        [d.device_id, d.ts, d.voltage, d.current, d.power,
         d.temperature, d.humidity, d.signal,
         d.pressure, d.pres_status,
         d.flow_rate, d.total_fwd, d.total_rev,
         d.velocity, d.mag_status]
      );
    }
    console.log("✅ Chuyển đổi dữ liệu hoàn tất!");
    process.exit(0);
  });
}

migrate();
