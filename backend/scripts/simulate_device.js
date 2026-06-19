const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://broker.hivemq.com:1883');

const createData = (devId, temp) => ({
  device_id: devId,
  voltage_ina: 12.0 + Math.random(),
  current_ina: 100 + Math.random() * 100,
  power_ina: 1200 + Math.random() * 500,
  temperature: temp + Math.random() * 2,
  humidity: 60 + Math.random() * 10,
  flow_rate: 10 + Math.random() * 10,
  total_fwd: 1000 + Math.random() * 100,
  total_rev: 5 + Math.random(),
  velocity: 1.0 + Math.random() * 0.5,
  mag_status: "0x0000",
  signal: 20 + Math.floor(Math.random() * 10),
  timestamp: new Date().toISOString()
});

client.on('connect', () => {
  console.log('Simulated device for stm32_02 connected');
  
  // Publish initial data only for stm32_02
  client.publish('viettel/sensor/all', JSON.stringify(createData('stm32_02', 30)));

  // Periodic updates every 15s for stm32_02
  setInterval(() => {
    client.publish('viettel/sensor/all', JSON.stringify(createData('stm32_02', 30)));
  }, 15000);

  // Subscribe only to commands for stm32_02
  client.subscribe('viettel/sensor/cmd/stm32_02');
  client.on('message', (topic, message) => {
    const cmd = message.toString();
    console.log(`Received command for stm32_02: ${cmd}`);
    
    if (cmd === 'READ_NOW') {
      client.publish('viettel/sensor/all', JSON.stringify(createData('stm32_02', 30)));
    } else if (cmd === 'RUN_TEST') {
      client.publish('viettel/sensor/ack', `stm32_02:TEST_SUCCESS`);
    }
  });
});
