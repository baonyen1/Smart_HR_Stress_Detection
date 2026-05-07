import pandas as pd
import numpy as np
from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, confusion_matrix, classification_report
import seaborn as sns
import matplotlib.pyplot as plt
!pip install micromlgen
from micromlgen import port

print("✅ Tất cả thư viện đã được import thành công!")

df = pd.read_csv('data_log.csv', on_bad_lines='skip')
print("✅ Đã đọc thành công file data_log.csv")

# --- Tiền xử lý và làm sạch dữ liệu ---
print(f"Số dòng đọc được từ file ban đầu: {len(df)}")
df.columns = df.columns.str.strip()
columns_to_process = ['HR(bpm)', 'SpO2(%)', 'HRV(ms)']
for col in columns_to_process:
    df[col] = pd.to_numeric(df[col], errors='coerce')
original_rows = len(df)
df.dropna(subset=columns_to_process, inplace=True)
print(f"Số dòng còn lại sau khi làm sạch: {len(df)} (đã xóa {original_rows - len(df)} dòng không hợp lệ)")
df['HR(bpm)'] = df['HR(bpm)'].astype(int)
df['SpO2(%)'] = df['SpO2(%)'].astype(int)
print("✅ Dữ liệu đã được làm sạch và chuẩn hóa thành công.")

def assign_stress_label_hybrid(row):
    hr = row['HR(bpm)']
    hrv = row['HRV(ms)']
    spo2 = row['SpO2(%)']

    # --- Đặt mức stress CƠ BẢN dựa trên thang đo HRV ---
    base_level = 0
    # Thang điểm 76-100 (Cao) -> HRV rất thấp
    if hrv < 30:
        base_level = 3
    # Thang điểm 51-75 (Trung bình) -> HRV thấp
    elif hrv < 50:
        base_level = 2
    # Thang điểm 26-50 (Nhẹ) -> HRV trung bình
    elif hrv < 75:
        base_level = 1
    # Thang điểm 0-25 (Nghỉ ngơi) -> HRV cao
    else: # hrv >= 75
        base_level = 0

    # Nếu nhịp tim rất cao và HRV thấp, đây là dấu hiệu stress cao rõ rệt,
    # bất kể mức cơ bản là gì.
    if hr > 115 and hrv < 40:
        return 3 # Cao

    # Nếu nhịp tim cao, nâng cấp lên mức Trung bình (nếu mức cơ bản đang thấp hơn)
    if hr > 100 and hrv < 60:
        return max(2, base_level) # Lấy mức cao hơn giữa 2 và mức cơ bản

    # Nếu không có điều kiện đặc biệt, trả về mức stress cơ bản đã tính
    return base_level

df['stress_level'] = df.apply(assign_stress_label_hybrid, axis=1)

print("\nĐã gán nhãn xong cho dữ liệu. Phân bổ các nhãn như sau:")
print(df['stress_level'].value_counts().sort_index())

features = ['HR(bpm)', 'SpO2(%)', 'HRV(ms)']
X = df[features]
y = df['stress_level']

X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.3, random_state=42, stratify=y)

print(f"\nTổng số mẫu: {len(df)}")
print(f"Số mẫu huấn luyện (train): {len(X_train)}")
print(f"Số mẫu kiểm thử (test): {len(X_test)}")

model = RandomForestClassifier(n_estimators=30, max_depth=10, random_state=42)
print("\nBắt đầu huấn luyện mô hình Random Forest...")
model.fit(X_train, y_train)
print("✅ Huấn luyện hoàn tất!")

#danh gia
y_pred = model.predict(X_test)

accuracy = accuracy_score(y_test, y_pred)
print(f"\n--- ĐÁNH GIÁ MÔ HÌNH ---")
print(f"🎯 Độ chính xác tổng thể: {accuracy * 100:.2f}%")

print("\n📊 Báo cáo chi tiết (Classification Report):")

unique_labels = np.unique(y.tolist())
unique_labels.sort()
label_map = {0: 'Nghỉ ngơi (0)', 1: 'Nhẹ (1)', 2: 'Trung bình (2)', 3: 'Cao (3)'}
dynamic_target_names = [label_map[label] for label in unique_labels if label in label_map]

print(classification_report(y_test, y_pred, target_names=dynamic_target_names, zero_division=0))

print("\n📈 Ma trận nhầm lẫn (Confusion Matrix):")
cm = confusion_matrix(y_test, y_pred, labels=unique_labels)
plt.figure(figsize=(8, 6))
sns.heatmap(cm, annot=True, fmt='d', cmap='Blues',
            xticklabels=dynamic_target_names,
            yticklabels=dynamic_target_names)
plt.ylabel('Giá trị thực tế')
plt.xlabel('Giá trị dự đoán')
plt.title('Ma trận nhầm lẫn')
plt.show()

print("\nChuyển đổi mô hình sang C++...")
cpp_code = port(model, classname='StressModel', features=features)
print(cpp_code)
