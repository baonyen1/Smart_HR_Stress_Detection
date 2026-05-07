import serial
import csv
import time
import pandas as pd

SERIAL_PORT = 'COM7'     
BAUD_RATE = 115200
CSV_FILE = 'data_log.csv'

DURATION = 120

ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
print(f"✅ Connected to {SERIAL_PORT} at {BAUD_RATE} baud")

with open(CSV_FILE, mode='w', newline='') as file:
    writer = csv.writer(file)
    writer.writerow(["timestamp(ms)", "HR(bpm)", "SpO2(%)", "HRV(ms)"])

    start_time = time.time()
    print("🟢 Logging data... (Press Ctrl+C to stop)")

    try:
        while (time.time() - start_time) < DURATION:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line and ',' in line:
                try:
                    values = [v.strip() for v in line.split(',')]
                    if len(values) == 4:
                        writer.writerow(values)
                        print("📊", values)
                except Exception as e:
                    print("⚠️ Parse error:", e)
    except KeyboardInterrupt:
        print("\n🛑 Logging stopped manually.")

print(f"\n💾 Data saved to: {CSV_FILE}")

try:
    df = pd.read_csv(CSV_FILE)
    print("\n===== Data preview =====")
    print(df.head(10))
except Exception as e:
    print("⚠️ Couldn't read CSV:", e)
