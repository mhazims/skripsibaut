from flask import Flask, render_template, Response, jsonify, request
from flask_socketio import SocketIO, emit
from ultralytics import YOLO
import cv2
import time
import base64
import threading
import sqlite3
from datetime import datetime

app = Flask(__name__)
app.config['SECRET_KEY'] = 'secret!'
socketio = SocketIO(app)

# --- LOAD MODEL YOLO ---
MODEL_PATH = r'D:\skripsi new\runs\detect\train_yolo11s\weights\best.pt'
model = YOLO(MODEL_PATH)
print("✅ Model YOLO berhasil dimuat!")

# Mapping nama output model → nama tampilan dashboard & database
BOLT_NAME_MAP = {
    "Carriage":   "Carriage Bolt",
    "Flange":     "Flange Bolt",
    "Hexagonal":  "Hex Bolt",
    "Socket Cap": "Socket Cap"
}

# Nama kelas untuk dashboard (urutan tampilan)
BOLT_TYPES = list(BOLT_NAME_MAP.values())

# --- PENGATURAN DATABASE ---
DATABASE_FILE = 'sorting_log.db'

def init_database():
    conn = sqlite3.connect(DATABASE_FILE, check_same_thread=False)
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS sort_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME NOT NULL,
            bolt_type TEXT NOT NULL,
            confidence REAL NOT NULL
        )
    ''')
    conn.commit()
    conn.close()
    print("✅ Database siap digunakan.")

# --- Variabel Global ---
camera = [None]
frame_lock = threading.Lock()
latest_frame = [None]
run_background_task = threading.Event()

# --- Pengaturan Realtime ---
DETECTION_INTERVAL       = 0.2    # Interval deteksi (detik)
SAVE_CONFIDENCE_THRESHOLD = 0.75  # Confidence minimum untuk disimpan ke database
DETECT_CONFIDENCE_THRESHOLD = 0.5 # Confidence minimum untuk ditampilkan
SAME_BOLT_COOLDOWN       = 3.0    # Cooldown simpan baut yang sama (detik)

# --- Pengaturan Garis Merah (Crossing Line) ---
RED_LINE_RATIO  = 0.5   # Posisi garis merah: 50% dari TINGGI ROI (horizontal, tengah)
CROSS_COOLDOWN  = 2.0   # Cooldown antar event crossing (detik)


# --- FUNGSI SOCKETIO ---

@socketio.on('connect')
def handle_connect():
    print('✅ Browser terhubung.')
    socketio.emit('system_log', {'data': 'Dasbor pemantau terhubung. Mode: Deteksi Realtime dengan ROI.'})

@socketio.on('disconnect')
def handle_disconnect():
    print('⚠️ Browser terputus.')


# --- FUNGSI DETEKSI REALTIME DENGAN ROI + CROSSING LINE ---

def detection_and_stream_thread():
    print("Membuka kamera...")
    if camera[0] is None:
        camera[0] = cv2.VideoCapture(1)
        camera[0].set(cv2.CAP_PROP_BUFFERSIZE, 1)

    if not camera[0].isOpened():
        print("!!! GAGAL MEMBUKA KAMERA !!!")
        socketio.emit('system_log', {'data': 'ERROR: Gagal membuka kamera!'})
        return

    print("--- Kamera aktif, deteksi realtime dimulai ---")
    socketio.emit('system_log', {'data': 'Kamera aktif. Deteksi realtime berjalan...'})

    bolt_counts_session = {bolt: 0 for bolt in BOLT_TYPES}
    last_detection_time = 0
    last_saved_bolt     = None
    last_saved_time     = 0
    last_crossed_bolt   = None
    last_crossed_time   = 0

    while run_background_task.is_set():
        success, frame = camera[0].read()
        if not success:
            print("⚠️ Gagal baca frame, mencoba ulang...")
            socketio.emit('system_log', {'data': 'WARNING: Gagal membaca frame kamera.'})
            time.sleep(0.5)
            continue

        # Simpan frame mentah untuk video feed
        with frame_lock:
            latest_frame[0] = frame.copy()

        current_time = time.time()

        # Jalankan deteksi sesuai interval
        if current_time - last_detection_time >= DETECTION_INTERVAL:
            last_detection_time = current_time

            # --- DEFINISI AREA ROI (CONVEYOR BELT) ---
            h, w, _ = frame.shape
            ymin, ymax = int(h * 0.0), int(h * 1.0)
            xmin, xmax = int(w * 0.30), int(w * 0.60)

            roi_frame = frame[ymin:ymax, xmin:xmax]

            # --- DETEKSI YOLO PADA AREA ROI ---
            results_yolo = model(roi_frame.copy(), conf=DETECT_CONFIDENCE_THRESHOLD, verbose=False)
            detections = results_yolo[0].boxes

            annotated_frame = frame.copy()

            # Gambar kotak batas area ROI (hijau)
            cv2.rectangle(annotated_frame, (xmin, ymin), (xmax, ymax), (0, 255, 0), 2)
            cv2.putText(annotated_frame, "ZONA ROI DETEKSI", (xmin + 10, ymin + 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

            # --- GARIS MERAH HORIZONTAL DI TENGAH ROI (selalu digambar) ---
            # Garis memotong ROI secara horizontal: objek bergerak vertikal melewati garis ini
            red_line_y = ymin + int((ymax - ymin) * RED_LINE_RATIO)
            cv2.line(annotated_frame, (xmin, red_line_y), (xmax, red_line_y), (0, 0, 255), 2)
            cv2.putText(annotated_frame, "CROSSING LINE", (xmin + 6, red_line_y - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 255), 1)

            if len(detections) > 0:
                # Ambil deteksi dengan confidence tertinggi
                best_idx = detections.conf.argmax()
                class_id = int(detections.cls[best_idx])
                confidence = round(float(detections.conf[best_idx]), 4)
                raw_bolt_type = model.names[class_id]
                bolt_type_display = BOLT_NAME_MAP.get(raw_bolt_type, raw_bolt_type)

                # --- TRANSLASI KOORDINAT ROI → FRAME UTAMA ---
                box = detections.xyxy[best_idx].cpu().numpy()
                rx1, ry1, rx2, ry2 = int(box[0]), int(box[1]), int(box[2]), int(box[3])
                fx1, fy1 = rx1 + xmin, ry1 + ymin
                fx2, fy2 = rx2 + xmin, ry2 + ymin

                # Gambar bounding box (pink/magenta)
                cv2.rectangle(annotated_frame, (fx1, fy1), (fx2, fy2), (254, 168, 249), 2)
                label = f"{bolt_type_display} {confidence*100:.1f}%"
                cv2.putText(annotated_frame, label, (fx1, fy1 - 10),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (254, 168, 249), 2)

                print(f"🔍 Terdeteksi: {bolt_type_display} ({confidence*100:.1f}%)")

                # --- CEK CROSSING: bounding box memotong garis HORIZONTAL ---
                # Objek dianggap crossing jika garis merah berada di antara fy1 (atas bbox) dan fy2 (bawah bbox)
                time_since_cross = current_time - last_crossed_time
                bbox_crosses_line = (fy1 <= red_line_y <= fy2)

                if bbox_crosses_line and time_since_cross >= CROSS_COOLDOWN:
                    last_crossed_bolt = bolt_type_display
                    last_crossed_time = current_time
                    print(f"🚨 CROSSING: {bolt_type_display} ({confidence*100:.1f}%)")
                    socketio.emit('bolt_crossed_line', {
                        'bolt':       bolt_type_display,
                        'confidence': round(confidence * 100, 1),
                        'timestamp':  datetime.now().strftime('%H:%M:%S')
                    })

                # --- LOGIKA PENYIMPANAN DATABASE (tidak berubah) ---
                time_since_last_save = current_time - last_saved_time
                is_new_bolt = (bolt_type_display != last_saved_bolt) or (time_since_last_save >= SAME_BOLT_COOLDOWN)

                if confidence >= SAVE_CONFIDENCE_THRESHOLD and is_new_bolt:
                    bolt_counts_session[bolt_type_display] = bolt_counts_session.get(bolt_type_display, 0) + 1
                    try:
                        conn = sqlite3.connect(DATABASE_FILE, check_same_thread=False)
                        cursor = conn.cursor()
                        cursor.execute(
                            "INSERT INTO sort_log (timestamp, bolt_type, confidence) VALUES (?, ?, ?)",
                            (datetime.now(), bolt_type_display, confidence)
                        )
                        conn.commit()
                        conn.close()
                        last_saved_bolt = bolt_type_display
                        last_saved_time = current_time
                        print(f"✅ Tersimpan: {bolt_type_display} | Sesi: {bolt_counts_session}")
                        socketio.emit('system_log', {
                            'data': f'Tersimpan: {bolt_type_display} ({confidence*100:.1f}%)'
                        })
                    except Exception as e:
                        print(f"❌ Gagal simpan database: {e}")
                        socketio.emit('system_log', {'data': f'ERROR DB: {e}'})

            else:
                bolt_type_display = None
                confidence = 0.0

            # Encode frame ke base64 dan kirim ke dashboard
            _, buffer = cv2.imencode('.jpg', annotated_frame)
            b64_string = base64.b64encode(buffer).decode('utf-8')

            socketio.emit('detection_event', {
                'image_data': b64_string,
                'bolt':       bolt_type_display if bolt_type_display else "Tidak Terdeteksi",
                'confidence': round(confidence * 100, 1),
                'counts':     bolt_counts_session,
                'timestamp':  datetime.now().strftime('%H:%M:%S')
            })

        socketio.sleep(0.01)

    print("--- Thread dihentikan, merilis kamera ---")
    if camera[0]:
        camera[0].release()
        camera[0] = None


def generate_live_frames():
    """Generator untuk video feed mentah pada endpoint streaming."""
    while True:
        with frame_lock:
            if latest_frame[0] is None:
                time.sleep(0.1)
                continue
            ret, buffer = cv2.imencode('.jpg', latest_frame[0])
            if not ret:
                continue
            frame_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
        socketio.sleep(0.033)  # ~30 FPS


# --- RUTE FLASK ---

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/video_feed')
def video_feed():
    return Response(
        generate_live_frames(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )

@app.route('/data_history')
def data_history():
    selected_date = request.args.get('date', default=datetime.now().strftime('%Y-%m-%d'))
    conn = sqlite3.connect(DATABASE_FILE)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute(
        "SELECT bolt_type, COUNT(*) as count FROM sort_log WHERE DATE(timestamp) = ? GROUP BY bolt_type",
        (selected_date,)
    )
    today_counts = {bolt: 0 for bolt in BOLT_TYPES}
    today_total = 0
    for row in cursor.fetchall():
        if row['bolt_type'] in today_counts:
            today_counts[row['bolt_type']] = row['count']
            today_total += row['count']
    conn.close()
    return render_template(
        'data.html',
        today_counts=today_counts,
        today_total=today_total,
        selected_date=selected_date
    )

@app.route('/api/daily_summary')
def daily_summary():
    conn = sqlite3.connect(DATABASE_FILE)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute(
        """SELECT DATE(timestamp) as date, COUNT(*) as total
           FROM sort_log
           GROUP BY DATE(timestamp)
           ORDER BY date ASC
           LIMIT 30"""
    )
    results = cursor.fetchall()
    conn.close()
    return jsonify({
        'labels': [row['date'] for row in results],
        'data':   [row['total'] for row in results]
    })

@app.route('/api/session_counts')
def session_counts():
    today = datetime.now().strftime('%Y-%m-%d')
    conn = sqlite3.connect(DATABASE_FILE)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute(
        "SELECT bolt_type, COUNT(*) as count FROM sort_log WHERE DATE(timestamp) = ? GROUP BY bolt_type",
        (today,)
    )
    counts = {bolt: 0 for bolt in BOLT_TYPES}
    for row in cursor.fetchall():
        if row['bolt_type'] in counts:
            counts[row['bolt_type']] = row['count']
    conn.close()
    return jsonify(counts)

@app.route('/api/recent_detections')
def recent_detections():
    conn = sqlite3.connect(DATABASE_FILE)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute(
        "SELECT timestamp, bolt_type, confidence FROM sort_log ORDER BY timestamp DESC LIMIT 10"
    )
    rows = cursor.fetchall()
    conn.close()
    return jsonify([
        {
            'timestamp':  row['timestamp'],
            'bolt_type':  row['bolt_type'],
            'confidence': round(row['confidence'] * 100, 1)
        }
        for row in rows
    ])


# --- MAIN ---
if __name__ == '__main__':
    init_database()
    run_background_task.set()
    socketio.start_background_task(target=detection_and_stream_thread)
    print("🚀 Server berjalan di http://0.0.0.0:5000")
    print(f"   • Interval deteksi    : {DETECTION_INTERVAL}s")
    print(f"   • Conf. tampil        : {DETECT_CONFIDENCE_THRESHOLD*100:.0f}%")
    print(f"   • Conf. simpan DB     : {SAVE_CONFIDENCE_THRESHOLD*100:.0f}%")
    print(f"   • Cooldown simpan     : {SAME_BOLT_COOLDOWN}s")
    print(f"   • Posisi garis merah  : {int(RED_LINE_RATIO*100)}% dari tinggi ROI (horizontal)")
    print(f"   • Cooldown crossing   : {CROSS_COOLDOWN}s")
    socketio.run(app, host='0.0.0.0', port=5000, debug=False)