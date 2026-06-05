/**
 * mqtt_client.js
 * Koneksi ke HiveMQ Cloud (WSS).
 * Publish ke MQTT HANYA saat event 'bolt_crossed_line' diterima dari Python.
 * Tidak lagi publish di setiap detection_event (mengurangi spam ke ESP32).
 */
document.addEventListener('DOMContentLoaded', () => {
    if (typeof io === 'undefined') {
        console.error("ERROR: Socket.IO library belum dimuat.");
        return;
    }
    if (typeof Paho === 'undefined') {
        console.error("ERROR: Paho MQTT library belum dimuat.");
        return;
    }

    const socket = io();

    // ----------------------------------------------------------------
    // KONFIGURASI MQTT — sesuaikan dengan broker HiveMQ Cloud kamu
    // ----------------------------------------------------------------
    const HOST     = 'd9f9a8170ace4b888983e922fe4c25a0.s1.eu.hivemq.cloud';
    const PORT     = 8884;
    const PATH     = '/mqtt';
    const USERNAME = 'skripsi';
    const PASSWORD = 'Skripsi123';

    // Topik untuk Subscribe (trigger dari ESP32 / sumber lain)
    const TOPIC_TRIGGER = 'sensor/trigger/bolt';

    // Topik untuk Publish hasil crossing ke ESP32
    const TOPIC_RESULT  = 'sensor/result/bolt';

    // ----------------------------------------------------------------
    // MAPPING bolt_type → posisi servo (derajat)
    // Sesuaikan dengan konfigurasi servo ESP32 kamu
    // ----------------------------------------------------------------
    const SERVO_MAP = {
        "Carriage Bolt": 0,
        "Flange Bolt":   60,
        "Hex Bolt":      120,
        "Socket Cap":    180
    };

    // ----------------------------------------------------------------
    // INISIALISASI CLIENT MQTT
    // ----------------------------------------------------------------
    const clientID  = 'web_dashboard_' + Math.random().toString(16).substr(2, 8);
    const mqttClient = new Paho.MQTT.Client(HOST, PORT, PATH, clientID);

    mqttClient.onConnectionLost = onConnectionLost;
    mqttClient.onMessageArrived = onMessageArrived;

    mqttClient.connect({
        userName:  USERNAME,
        password:  PASSWORD,
        useSSL:    true,
        timeout:   5,
        keepAliveInterval: 30,
        onSuccess: onConnect,
        onFailure: onFailure
    });

    // ----------------------------------------------------------------
    // CALLBACK KONEKSI
    // ----------------------------------------------------------------
    function onConnect() {
        console.log("✅ MQTT: Koneksi berhasil ke", HOST);
        mqttClient.subscribe(TOPIC_TRIGGER);
        socket.emit('system_log', { data: `MQTT terhubung. Subscribe: ${TOPIC_TRIGGER}` });
    }

    function onFailure(response) {
        console.error("❌ MQTT: Gagal koneksi.", response.errorMessage);
        socket.emit('system_log', { data: `ERROR MQTT: Gagal koneksi (${response.errorMessage})` });

        // Coba reconnect setelah 5 detik
        setTimeout(() => {
            console.log("🔄 MQTT: Mencoba reconnect...");
            mqttClient.connect({
                userName: USERNAME, password: PASSWORD, useSSL: true,
                timeout: 5, keepAliveInterval: 30,
                onSuccess: onConnect, onFailure: onFailure
            });
        }, 5000);
    }

    function onConnectionLost(responseObject) {
        if (responseObject.errorCode !== 0) {
            console.warn("⚠️ MQTT: Terputus:", responseObject.errorMessage);
            socket.emit('system_log', { data: `MQTT: Koneksi terputus. Reconnecting...` });
            // Coba reconnect
            setTimeout(() => {
                mqttClient.connect({
                    userName: USERNAME, password: PASSWORD, useSSL: true,
                    timeout: 5, keepAliveInterval: 30,
                    onSuccess: onConnect, onFailure: onFailure
                });
            }, 3000);
        }
    }

    // ----------------------------------------------------------------
    // TERIMA PESAN DARI MQTT (trigger dari ESP32 / sumber lain)
    // ----------------------------------------------------------------
    function onMessageArrived(message) {
        const payload = message.payloadString;
        console.log(`📩 MQTT diterima [${message.destinationName}]:`, payload);

        let parsed = {};
        try   { parsed = JSON.parse(payload); }
        catch { parsed = { trigger: payload }; }

        // Teruskan sinyal trigger ke Python via SocketIO
        socket.emit('trigger_detection', parsed);
        socket.emit('system_log', { data: `MQTT → Python: Trigger diterima & diteruskan.` });
    }

    // ----------------------------------------------------------------
    // HELPER: Publish pesan JSON ke MQTT
    // ----------------------------------------------------------------
    function publishToMQTT(topic, payloadObject) {
        if (!mqttClient.isConnected()) {
            console.warn("⚠️ MQTT: Tidak terhubung, pesan tidak terkirim.");
            socket.emit('system_log', { data: `MQTT: Tidak terhubung, gagal kirim ke ${topic}` });
            return false;
        }
        const jsonString = JSON.stringify(payloadObject);
        const msg        = new Paho.MQTT.Message(jsonString);
        msg.destinationName = topic;
        msg.qos             = 1;    // QoS 1: at-least-once delivery
        msg.retained        = false;
        mqttClient.send(msg);
        console.log(`📤 MQTT publish [${topic}]:`, payloadObject);
        return true;
    }

    // ----------------------------------------------------------------
    // EVENT UTAMA: Baut melewati garis merah → Publish ke ESP32
    // ----------------------------------------------------------------
    socket.on('bolt_crossed_line', (data) => {
        console.log(`🚨 Crossing terdeteksi: ${data.bolt} (${data.confidence}%)`);

        const servoAngle = SERVO_MAP[data.bolt] !== undefined ? SERVO_MAP[data.bolt] : -1;

        const payload = {
            event:       "bolt_crossed",        // Identifikasi jenis event
            timestamp:   new Date().toISOString(),
            bolt_type:   data.bolt,             // "Hex Bolt", "Carriage Bolt", dll.
            confidence:  data.confidence,       // 0–100
            servo_angle: servoAngle             // Sudut servo untuk ESP32
        };

        const sent = publishToMQTT(TOPIC_RESULT, payload);

        if (sent) {
            socket.emit('system_log', {
                data: `MQTT → ESP32: ${data.bolt} | servo=${servoAngle}° | conf=${data.confidence}%`
            });
        }
    });

    // ----------------------------------------------------------------
    // Tidak ada publish di detection_event (sudah dipindah ke crossing)
    // Blok ini dikosongkan agar tidak spam MQTT setiap frame
    // ----------------------------------------------------------------
    // socket.on('detection_event', ...) — sengaja tidak diisi di sini
});