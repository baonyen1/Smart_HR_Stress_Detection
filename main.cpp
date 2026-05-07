#include <MAX30105.h>
#include <Adafruit_SSD1306.h>
#include "filters.h"
#include "Wire.h"
#include <vector>
#include <numeric>
#include <deque>
#include "StressModel.h"

// -------- BLYNK CONFIG --------
#define BLYNK_TEMPLATE_ID "TMPL0yKVQJ3Rt"
#define BLYNK_TEMPLATE_NAME "Embedded"
#define BLYNK_AUTH_TOKEN "vdakLwGgtrmQ0n_I8OtQhUIBnescTX"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

char ssid[] = "Trung Cuong";
char pass[] = "27102005";

BlynkTimer timer;

// -------- BUZZER CONFIG --------

#define BUZZER_PIN 23

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

const int DATA_X = 0;
const int DATA_Y = 16;
const int DATA_H = 128;
const int DATA_W = 48;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 sensor;
XPressRModel stressModel;
MAX30105 sensor;

// ----------------------------------------------------
// 1) Vẽ bitmap cho trái tim nhỏ (12x11)
// ----------------------------------------------------
static const unsigned char PROGMEM small_heart_bmp[] = {
  0b00000000, 0b01100000,
  0b00000000, 0b11110000,
  0b00000001, 0b11111000,
  0b00000111, 0b11111110,
  0b00001111, 0b11111111,
  0b00011111, 0b11111111,
  0b00011111, 0b11111111,
  0b00001111, 0b11111110,
  0b00000111, 0b11111100,
  0b00000011, 0b11111000,
  0b00000001, 0b11110000,
  0b00000000, 0b11100000,
  0b00000000, 0b01000000,
};

// ----------------------------------------------------
// 1b) Vẽ bitmap cho trái tim lớn (16x15)
// ----------------------------------------------------
static const unsigned char PROGMEM large_heart_bmp[] = {
  0b00001100, 0b00110000,
  0b00011110, 0b01111000,
  0b00111111, 0b11111100,
  0b01111111, 0b11111110,
  0b11111111, 0b11111111,
  0b11111111, 0b11111111,
  0b11111111, 0b11111111,
  0b01111111, 0b11111110,
  0b00111111, 0b11111100,
  0b00011111, 0b11111000,
  0b00001111, 0b11110000,
  0b00000111, 0b11100000,
  0b00000011, 0b11000000,
  0b00000001, 0b10000000,
  0b00000000, 0b00000000,
};

// -------- Config --------
const unsigned long kFingerThreshold = 10000;
const unsigned int kFingerCooldownMs = 500;
const float kEdgeThreshold = -2000.0;
const float kLowPassCutoff = 5.0;
const float kHighPassCutoff = 0.5;
const bool kEnableAveraging = true;
const int kAveragingSamples = 5;
const int kSampleThreshold = 5;

LowPassFilter low_pass_filter_red{kLowPassCutoff, 50.0};
LowPassFilter low_pass_filter_ir{kLowPassCutoff, 50.0};
HighPassFilter high_pass_filter{kHighPassCutoff, 50.0};
Differentiator differentiator{50.0};
MovingAverageFilter<kAveragingSamples> average_bpm;
MovingAverageFilter<kAveragingSamples> average_spo2;

MinMaxAvgStatistic stat_red;
MinMaxAvgStatistic stat_ir;

// float kSpO2_A = 1.5958422;
// float kSpO2_B = -34.6596622;
// float kSpO2_C = 112.6898759;

long last_heartbeat = 0;
long finger_timestamp = 0;
bool finger_detected = false;
float last_diff = NAN;
long crossed_time = 0;

const int HRV_SMOOTHING_WINDOW = 20;
const int HRV_BUFFER_SIZE = 30;
std::vector<int> rr_intervals;
std::deque<float> hrv_history;

volatile bool t2cbusy = false;
int gBPM = 0;
int gSpO2 = 0;
bool gValid = false;
unsigned long lastAlertTime = 0;
const long ALERT_COOLDOWN = 6000;
const unsigned long OLED_UPDATE_INTERVAL = 300; // 300ms
bool showHeart = false;
bool heartIsLarge = false; // Biến để theo dõi trạng thái trái tim

// ---- Helper ----
void clearDataArea() {
  display.fillRect(DATA_X, DATA_Y, DATA_W, DATA_H, BLACK);
}

// ---- HRV CALCULATION (OPTIMIZED) ----
float computeRMSSD(const std::vector<int>& rr, float artifact_threshold = 0.2f) {
  if (rr.size() < 3) return NAN;

  double sum_sq_diff = 0.0;
  int valid_diff_count = 0;

  for (size_t i = 1; i < rr.size(); i++) {
    int diff = rr[i] - rr[i - 1];

    if (std::abs(diff) > artifact_threshold * rr[i - 1]) continue;

    sum_sq_diff += static_cast<double>(diff * diff);
    valid_diff_count++;
  }

  if (valid_diff_count < 2) return NAN;

  return std::sqrt(sum_sq_diff / valid_diff_count);
}

float getSmoothedHRV(float new_rmssd) {
  if (std::isnan(new_rmssd)) {
    if (hrv_history.empty()) return NAN;
  } else {
    hrv_history.push_back(new_rmssd);

    if (hrv_history.size() > HRV_SMOOTHING_WINDOW)
      hrv_history.pop_front();
  }

  if (hrv_history.empty()) return NAN;

  float sum = std::accumulate(hrv_history.begin(), hrv_history.end(), 0.0f);
  return sum / hrv_history.size();
}

void triggerBuzzer() {
  // Kêu 2 tiếng bíp nhanh
  tone(BUZZER_PIN, 1500, 150); // Tần số 1500Hz trong 150ms
  delay(200);
  tone(BUZZER_PIN, 1500, 150);
}

void checkAlert() {
  unsigned long now = millis();
  bool alertState = false; // Biến để theo dõi trạng thái cảnh báo

  if (gValid) {
    // Kiểm tra các điều kiện cảnh báo
    // Giả sử nhịp tim > 100
    if (gBPM >= 100) {
      if (now - lastAlertTime > 10000) { // Chống spam
        String msg = String("⚠️ Nhịp tim bất thường: ") + gBPM + " bpm";
        Blynk.logEvent("abnormal_hr", msg);
        Serial.println("⚠️ ALERT: ABNORMAL HR!");

        triggerBuzzer();
        lastAlertTime = now;
      }
    } else if (gSpO2 < 90) {
      if (now - lastAlertTime > 10000) { // Chống spam
        String msg = String("⚠️ SpO2 thấp: ") + gSpO2 + "%";
        Blynk.logEvent("low_spo2", msg);
        Serial.println("⚠️ ALERT: SpO2 LOW!");

        triggerBuzzer();
        lastAlertTime = now;
      }
    }
  }

  // Cập nhật widget LED
  if (alertState) {
    Blynk.virtualWrite(V6, 1); // Bật LED
  } else {
    Blynk.virtualWrite(V6, 0); // Tắt LED
  }
}
/* ==== BLYNK SEND ==== */
void sendToBlynk() {
  if (i2cBusy) return;
  if (!Blynk.connected()) return;

  if (gvalid) {
    // Nếu dữ liệu hợp lệ (có ngón tay), gửi các giá trị đo được
    Blynk.virtualWrite(V0, gHR);
    Blynk.virtualWrite(V1, gSpO2);
    Blynk.virtualWrite(V2, isnan(gHRV) ? 0 : gHRV);
    Blynk.virtualWrite(V3, "OK");
    checkAlert();
    // (Các giá trị V4, V5 đã được gửi từ hàm loop)

    checkAlert(); // Kiểm tra cảnh báo chỉ khi có dữ liệu hợp lệ
  } else {
    // Nếu dữ liệu KHÔNG hợp lệ (KHÔNG có ngón tay), gửi các giá trị reset
    Blynk.virtualWrite(V0, 0);          // Reset Nhịp tim về 0
    Blynk.virtualWrite(V1, 0);          // Reset SpO2 về 0
    Blynk.virtualWrite(V2, 0);          // Reset HRV về 0
    Blynk.virtualWrite(V3, "NO FINGER");// Cập nhật trạng thái
    Blynk.virtualWrite(V4, "---");      // Reset mức Stress
    Blynk.virtualWrite(V5, 0);          // Tắt LED cảnh báo khi không có ngón tay
  }
}

/* ==== OLED ==== */
void initDrawScreen(void) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(F("Health Dashboard AI"));
  display.display();
}

// ---------------------------------------------------------
void displayMeasuredValues(bool no_finger, int32_t beatAvg, int32_t spo2, float hrv, String stress = "") {
  unsigned long now = millis();

  // Xử lý khi không có ngón tay
  if (no_finger) {
    clearDataArea();
    display.setCursor(5, 20);
    display.setTextColor(WHITE, BLACK);
    display.setTextSize(2);
    display.println(F("NO Finger"));
    display.display();
    showedWait = false;
    lastOledUpdate = now;
    return;
  }

  // Chỉ cập nhật màn hình sau một khoảng thời gian
  if (now - lastOledUpdate < OLED_UPDATE_INTERVAL) return;

  clearDataArea();

  // Vẽ hình trái tim (tạo hiệu ứng đập bằng cách đổi kích thước bitmap)
  if (heartIsLarge) {
    display.drawBitmap(5, 20, large_heart_bmp, 16, 15, WHITE);
  } else {
    display.drawBitmap(7, 22, small_heart_bmp, 12, 11, WHITE);
  }

  // Vẽ các chỉ số bên cạnh trái tim
  display.setTextSize(2);
  display.setCursor(30, 20);
  display.print(beatAvg);
  display.setTextSize(1);
  display.print(" BPM");

  display.setCursor(5, 40);
  display.printf("SpO2: %d %%", spo2);
  
// Tiếp tục phần hiển thị các chỉ số
  display.setCursor(65, 40);
  display.print("HRV: ");
  if (!isnan(hrv)) display.print(hrv, 1); else display.print("--");
  display.print("ms");

  display.setCursor(5, 55);
  display.printf("Str: %s", stress.c_str());

  display.display();
  lastOledUpdate = now;
}

/* ==== SETUP ==== */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nStarting...");

  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(21, 22, 400000); // Khởi tạo I2C với chân SDA=21, SCL=22 (thường dùng cho ESP32)

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1);
  }

  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(5000);
  timer.setInterval(1000L, sendToBlynk); // Thiết lập gửi dữ liệu lên Blynk mỗi 1 giây
  // (Tiếp tục phần cuối của setup)
  if (sensor.begin(Wire, I2C_SPEED_STANDARD)) {
    sensor.setup();
    Serial.println("Sensor initialized");
    Serial.println("timestamp,HR,SpO2,HRV");
  } else {
    Serial.println("Sensor not found");
    while (1);
  }
  display.clearDisplay();
  initDrawScreen();
}

/* ==== LOOP ==== */
void loop() {
  Blynk.run();
  timer.run();
  i2cBusy = true;

  while (!sensor.available()) sensor.check();
  float current_value_red = sensor.getRed();
  float current_value_ir = sensor.getIR();
  sensor.nextSample();

  // Kiểm tra sự hiện diện của ngón tay
  if (current_value_red < kFingerThreshold) {
    differentiator.reset(); averager_bpm.reset(); averager_spo2.reset();
    low_pass_filter_red.reset(); low_pass_filter_ir.reset(); high_pass_filter.reset();
    stat_red.reset(); stat_ir.reset();
    rr_intervals.clear(); hrv_history.clear();
    finger_detected = false; finger_timestamp = millis();
    displayMeasuredValues(true, 0, 0, NAN);
    gvalid = false; i2cBusy = false;
    return;
  }

  if (!finger_detected) finger_detected = true;

  if (finger_detected) {
    // Lọc tín hiệu bằng Low-pass filter và tính toán thống kê
    current_value_red = low_pass_filter_red.process(current_value_red);
    current_value_ir = low_pass_filter_ir.process(current_value_ir);
    stat_red.process(current_value_red);
    stat_ir.process(current_value_ir);

    float current_value = high_pass_filter.process(current_value_red);
    float current_diff = differentiator.process(current_value);

    // Thuật toán phát hiện đỉnh xung (Heartbeat Detection)
    if (!isnan(current_diff) && !isnan(last_diff)) {
      if (last_diff > 0 && current_diff < 0) {
        crossed = true;
        crossed_time = millis();
      }
      if (current_diff > 0) crossed = false;

      if (crossed && current_diff < kEdgeThreshold) {
        if (last_heartbeat != 0 && crossed_time - last_heartbeat > 300) {
          heartIsLarge = !heartIsLarge; // Hiệu ứng tim đập

          int rr = crossed_time - last_heartbeat;
          int bpm = 60000 / rr;

          if (rr >= 300 && rr <= 2000) {
            rr_intervals.push_back(rr);
            if ((int)rr_intervals.size() > HRV_BUFFER_SIZE) rr_intervals.erase(rr_intervals.begin());
          }

          // Tính toán SpO2 bằng phương pháp R-value (tỉ lệ Red/IR)
          float rred = (stat_red.maximum() - stat_red.minimum()) / stat_red.average();
          float rir = (stat_ir.maximum() - stat_ir.minimum()) / stat_ir.average();
          float r = rred / rir;
          float spo2 = kSpO2_A * r * r + kSpO2_B * r + kSpO2_C;

          // Tính toán HRV (RMSSD)
          float raw_rmssd = computeRMSSD(rr_intervals);
          float smoothed_hrv = getSmoothedHRV(raw_rmssd);

          if (bpm > 50 && bpm < 250) {
            int average_bpm = averager_bpm.process(bpm);
            int average_spo2 = averager_spo2.process(spo2);

        if (averager_bpm.count() < kSampleThreshold) {
    gvalid = false;
    if (!showedWait) {
      clearDataArea();
      display.setCursor(5, 20);
      display.setTextColor(WHITE, BLACK);
      display.setTextSize(2);
      display.println(F("Wait..."));
      display.display();
      showedWait = true;
    }
  } else {
    gvalid = true;

    // Chuẩn bị đầu vào cho mô hình AI
    float features[3] = {(float)average_bpm, (float)average_spo2, smoothed_hrv};
    // Dự đoán mức độ stress bằng mô hình EloquentML
    int stress_level = Eloquent::ML::Port::StressModel().predict(features);

    String stress_text;
    switch (stress_level) {
      case 0: stress_text = "Relax"; break;
      case 1: stress_text = "Low Stress"; break;
      case 2: stress_text = "Medium Stress"; break; // Rút gọn để vừa màn hình
      case 3: stress_text = "High Stress"; break;
      default: stress_text = "N/A"; break;
    }

    Blynk.virtualWrite(V4, stress_text);

    Serial.print("-> Du doan Stress: "); Serial.println(stress_text);
    Serial.printf("%lu,%d,%d,%.1f\n", millis(), average_bpm, average_spo2, smoothed_hrv);
    displayMeasuredValues(false, average_bpm, average_spo2, smoothed_hrv, stress_text);

    gHR = average_bpm;
    gSpO2 = average_spo2;
    gHRV = smoothed_hrv;
  }
} // Kết thúc block phát hiện nhịp tim

    stat_red.reset();
    stat_ir.reset();
  }
  
  crossed = false;
  last_heartbeat = crossed_time;
}

last_diff = current_diff;
}

i2cBusy = false;
}