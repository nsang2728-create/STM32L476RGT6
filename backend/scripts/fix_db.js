const sqlite3 = require('sqlite3').verbose();
const db = new sqlite3.Database('sensors.db');

db.serialize(() => {
  db.get("PRAGMA table_info(sensor_data)", (err, row) => {
    db.all("PRAGMA table_info(sensor_data)", (err, rows) => {
      const columns = rows.map(r => r.name);
      if (!columns.includes('device_id')) {
        console.log('Adding device_id column...');
        db.run("ALTER TABLE sensor_data ADD COLUMN device_id TEXT DEFAULT 'stm32_01'", (err) => {
          if (err) console.error(err);
          else console.log('device_id column added.');
        });
      } else {
        console.log('device_id column already exists.');
      }
    });
  });
});
