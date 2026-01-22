/*
 * M5Stack QRコードリーダー & OMRON PLCへ FINS UDP送信して書込み
 * 
 * 機能概要:
 *   - M5Stack Basic + QRコードユニットでQRコードを読み取り
 *   - 読み取ったデータをUDPソケット通信で送信
 *   - 整備開始/完了のステータスを付与
 * 
 * ボタン操作:
 *   A (左): 整備開始モード → QR読み取り時に STATUS="01" を送信
 *   B (中): QR読み取り中止
 *   C (右): 整備完了モード → QR読み取り時に STATUS="02" を送信
 *
 * QRサンプル データ例:
 *   - "DK37173JB352101 00" 
 */

#include <M5Unified.h>
#include <M5GFX.h>
#include "M5UnitQRCode.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SD.h>
#include <FS.h>

// ========================================
// 設定項目
// ========================================

// WiFi設定（デフォルト値、CSVから上書き可能）
String WIFI_SSID     = "oobu_local_wireless";
String WIFI_PASSWORD = "Cw5j0YE2Akj3X1bBek3P";

// 固定IP設定（デフォルト値、CSVから上書き可能）
IPAddress LOCAL_IP(192, 168, 181, 250);
const IPAddress GATEWAY(192, 168, 181, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress PRIMARY_DNS(0, 0, 0, 0);
const IPAddress SECONDARY_DNS(0, 0, 0, 0);

// OMRON PLC FINS/UDP設定
const char* PLC_IP = "192.168.181.2";     // ← PLCのIPアドレス
const int   PLC_PORT = 9600;                  // ← FINSポート番号（通常9600）
const int   LOCAL_UDP_PORT = 9600;            // ← ローカル受信ポート番号
const uint8_t PLC_NODE_ADDRESS = 0x02;       // ← PLCのノードアドレス
const uint8_t PC_NODE_ADDRESS = 0xFA;        // ← PC側のノードアドレス（250）
const int   UDP_RETRY_MAX = 3;                // 送信失敗時のリトライ回数
const int   UDP_RETRY_DELAY = 1000;           // リトライ間隔(ms)
const int   FINS_RESPONSE_TIMEOUT = 3000;     // FINSレスポンス待機時間(ms)

// FINSメモリエリア設定（データ書き込み先）
const uint8_t FINS_MEMORY_AREA_DM = 0x82;    // DMエリア
const uint16_t FINS_START_ADDRESS = 0x0FA0;  // 開始アドレス（DM4000～）

// 画面設定
const int BUTTON_LABEL_HEIGHT = 25;
const int BUTTON_LABEL_Y_OFFSET = 13;

// I2C設定
const uint8_t  QRCODE_I2C_ADDR = UNIT_QRCODE_ADDR;  // 0x21
const uint8_t  QRCODE_SDA_PIN = 21;
const uint8_t  QRCODE_SCL_PIN = 22;
const uint32_t QRCODE_I2C_SPEED = 100000U;
// SDカード設定
const int SD_CS_PIN = 4;  // M5Stack BasicのSDカードCSピン
// その他設定
String OP_NUM = "10";  // 工程番号（CSVから上書き可能）
String LINE_NUM = "01";  // ライン番号（CSVから上書き可能）
const int WIFI_CONNECT_RETRY_MAX = 20;
const int WIFI_CONNECT_RETRY_DELAY = 1000;  // ms
const unsigned long SCAN_TIMEOUT_MS = 20000;  // QRスキャンタイムアウト(20秒)
const unsigned long KEEPALIVE_INTERVAL = 15000;  // キープアライブ間隔(30秒)

// 自動スキャンモード（通常はコメントアウト）
// #define I2C_AUTO_SCAN_MODE

// ========================================
// グローバル変数
// ========================================

M5Canvas canvas(&M5.Display);
M5UnitQRCodeI2C qrcode;
WiFiUDP udp;  // UDP通信用
String currentStatus = "";  // "01" or "02"
unsigned long scanStartTime = 0;  // スキャン開始時刻（0=スキャン未実行）
unsigned long lastCommunicationTime = 0;  // 最終通信時刻
uint8_t sequenceNumber = 0;  // 通し番号（00-99）

// ========================================
// 関数プロトタイプ
// ========================================

void loadSettingsFromSD();
void drawButtonLabels();
void updateDisplay();
void connectWiFi();
void sendQRCodeData(const char* data, const char* status);
void sendKeepAlive();
void handleQRCodeScan();
void handleButtonInput();
void checkScanTimeout();
void checkKeepAlive();

// ========================================
// 画面表示関数
// ========================================

/**
 * 画面下部のボタンラベルを描画
 * A(整備開始) B(読取中止) C(整備完了)
 */
void drawButtonLabels() {
  int displayHeight = M5.Display.height();
  int displayWidth = M5.Display.width();
  int labelY = displayHeight - BUTTON_LABEL_Y_OFFSET;
  
  // 各ボタンの背景色を設定
  M5.Display.fillRect(0, displayHeight - BUTTON_LABEL_HEIGHT, displayWidth / 3 - 1, BUTTON_LABEL_HEIGHT, BLUE);
  M5.Display.fillRect(displayWidth / 3, displayHeight - BUTTON_LABEL_HEIGHT, displayWidth / 3 - 1, BUTTON_LABEL_HEIGHT, DARKGREY);
  M5.Display.fillRect(2 * displayWidth / 3, displayHeight - BUTTON_LABEL_HEIGHT, displayWidth / 3 - 1, BUTTON_LABEL_HEIGHT, GREEN);
  
  M5.Display.fillRect(0,displayHeight - BUTTON_LABEL_HEIGHT * 2, displayWidth, BUTTON_LABEL_HEIGHT -1 , WHITE);

  // ボタンラベルテキストを描画
  M5.Display.setFont(&fonts::lgfxJapanGothic_20);
  M5.Display.setTextDatum(middle_center);
  
  M5.Display.setTextColor(WHITE);
  M5.Display.drawString("整備開始", displayWidth / 6, labelY);
  M5.Display.setTextColor(YELLOW);
  M5.Display.drawString("読取中止", displayWidth / 2, labelY);
  M5.Display.setTextColor(BLACK);
  M5.Display.drawString("整備完了", displayWidth * 5 / 6, labelY);

  M5.Display.setTextColor(BLACK);
  M5.Display.drawString(("ライン：" + LINE_NUM + "  工程：" + OP_NUM).c_str(), displayWidth / 2, displayHeight - BUTTON_LABEL_HEIGHT - BUTTON_LABEL_Y_OFFSET);

}

/**
 * 画面を更新（canvas表示 + ボタンラベル再描画）
 */
void updateDisplay() {
  canvas.pushSprite(0, 0);
  drawButtonLabels();
}

// ========================================
// SD設定読み込み関数
// ========================================

/**
 * SDカードからsetting.csvを読み込んで設定値を更新
 */
void loadSettingsFromSD() {
  canvas.println("SD初期化中...");
  updateDisplay();
  
  // SDカード初期化（M5Stack BasicはCSピン4を使用）
  // 最大25MHzでSPIバスを初期化
  SPI.begin(18, 19, 23, SD_CS_PIN);  // SCK=18, MISO=19, MOSI=23, CS=4
  if (!SD.begin(SD_CS_PIN, SPI, 25000000)) {
    canvas.println("SDカード初期化失敗");
    canvas.println("- SDカードが挿入されていますか?");
    canvas.println("- カードがフォーマット済みですか?");
    canvas.println("デフォルト設定を使用");
    updateDisplay();
    delay(3000);
    return;
  }
  
  canvas.println("SDカード初期化成功");
  updateDisplay();
  
  // setting.csvを開く
  File file = SD.open("/setting.csv", FILE_READ);
  if (!file) {
    canvas.println("setting.csv が見つかりません");
    canvas.println("デフォルト設定を使用");
    updateDisplay();
    delay(2000);
    return;
  }
  
  canvas.println("setting.csv 読み込み中...");
  updateDisplay();
  
  // CSVファイルを1行ずつ読み込み
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    
    if (line.length() == 0) continue;
    
    // カンマで分割
    int commaIndex = line.indexOf(',');
    if (commaIndex == -1) continue;
    
    String key = line.substring(0, commaIndex);
    String value = line.substring(commaIndex + 1);
    
    // ダブルクォートを削除
    key.replace("\"", "");
    value.replace("\"", "");
    key.trim();
    value.trim();
    
    // キーに応じて値を設定
    if (key == "LINE_NUM") {
      LINE_NUM = value;
      canvas.printf("  LINE_NUM: %s\n", value.c_str());
    } else if (key == "OP_NUM") {
      OP_NUM = value;
      canvas.printf("  OP_NUM: %s\n", value.c_str());
    } else if (key == "LOCAL_IP") {
      // IPアドレスを解析
      if (LOCAL_IP.fromString(value)) {
        canvas.printf("  LOCAL_IP: %s\n", value.c_str());
      } else {
        canvas.printf("  LOCAL_IP解析失敗: %s\n", value.c_str());
      }
    } else if (key == "WIFI_SSID") {
      WIFI_SSID = value;
      canvas.printf("  WIFI_SSID: %s\n", value.c_str());
    } else if (key == "WIFI_PASSWORD") {
      WIFI_PASSWORD = value;
      canvas.println("  WIFI_PASSWORD: ****");
    }
    
    updateDisplay();
  }
  
  file.close();
  canvas.println("設定読み込み完了\n");
  updateDisplay();
  delay(1000);
}

// ========================================
// WiFi接続関数
// ========================================

/**
 * WiFiに接続（固定IP使用）
 */
void connectWiFi() {
  canvas.println("WiFi 接続中...");
  updateDisplay();
  
  // 固定IPアドレスを設定
  if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) {
    canvas.println("固定IP設定失敗");
    updateDisplay();
  }
  
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());
  
  // 接続待機
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_RETRY_MAX) {
    delay(WIFI_CONNECT_RETRY_DELAY);
    canvas.print(".");
    updateDisplay();
    attempts++;
  }
  
  // 接続結果表示
  if (WiFi.status() == WL_CONNECTED) {
    canvas.println("\nWiFi接続成功！");
    canvas.printf("IPアドレス: %s\n\n", WiFi.localIP().toString().c_str());
  } else {
    canvas.println("\nWiFi接続失敗！\n");
    canvas.println("3秒後に再起動します...");
    updateDisplay();
    delay(3000);
    ESP.restart();  // ESP32を再起動
  }
  updateDisplay();
}

// ========================================
// HTTP通信関数
// ========================================
// UDP通信関数
// ========================================

/**
 * FINSコマンドを作成してPLCのDMエリアにデータを書き込む
 * @param data QRコードから読み取ったデータ
 * @param status "START" または "END"
 */
void sendQRCodeData(const char* data, const char* status) {
  // WiFi接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    canvas.println("WiFi未接続！");
    updateDisplay();
    return;
  }

  static uint8_t sid = 0x00;  // Service ID（セッション管理用、0～255で循環）
  
  // 新たな送信開始時に通し番号をインクリメント（00-99で循環）
  sequenceNumber++;
  if (sequenceNumber > 99) {
    sequenceNumber = 0;
  }
  
  // 今回の送信で使う通し番号を保存（リトライでは同じ番号を使う）
  uint8_t currentSequence = sequenceNumber;
  
  // リトライループ
  bool success = false;
  for (int attempt = 1; attempt <= UDP_RETRY_MAX; attempt++) {
    // 送信試行表示
    if (attempt == 1) {
      canvas.println("PLCへFINS送信中...");
    } else {
      canvas.printf("リトライ中 (%d/%d)...\n", attempt, UDP_RETRY_MAX);
    }
    canvas.printf("PLC: %s:%d\n", PLC_IP, PLC_PORT);
    updateDisplay();
    
    // FINSヘッダー作成（10バイト）
    uint8_t finsHeader[10];
    finsHeader[0] = 0x80;              // ICF
    finsHeader[1] = 0x00;              // RSV
    finsHeader[2] = 0x02;              // GCT
    finsHeader[3] = 0x00;              // DNA
    finsHeader[4] = PLC_NODE_ADDRESS;  // DA1（PLCノードアドレス）
    finsHeader[5] = 0x00;              // DA2
    finsHeader[6] = 0x00;              // SNA
    finsHeader[7] = PC_NODE_ADDRESS;   // SA1（PCノードアドレス）
    finsHeader[8] = 0x00;              // SA2
    finsHeader[9] = sid++;             // SID
    
    // FINSコマンド：メモリエリア書き込み（0x01, 0x02）
    uint8_t finsCommand[2];
    finsCommand[0] = 0x01;  // メインコマンド：メモリエリア書き込み
    finsCommand[1] = 0x02;  // サブコマンド
    
    // 書き込みデータ準備（最後に通し番号を付与）
    char seqStr[3];
    sprintf(seqStr, "%02d", currentSequence);
    String writeData = LINE_NUM + OP_NUM + String(status) + String(data) + String(seqStr);
    int dataLen = writeData.length();
    
    // データ長をワード単位に変換（2バイト=1ワード、奇数なら切り上げ）
    int wordCount = (dataLen + 1) / 2;
    
    // 書き込みパラメータ
    uint8_t writeParams[6];
    writeParams[0] = FINS_MEMORY_AREA_DM;          // メモリエリアコード（DM）
    writeParams[1] = (FINS_START_ADDRESS >> 8) & 0xFF;  // 開始アドレス上位
    writeParams[2] = FINS_START_ADDRESS & 0xFF;         // 開始アドレス下位
    writeParams[3] = 0x00;                         // ビット位置（0固定）
    writeParams[4] = (wordCount >> 8) & 0xFF;      // 書き込みワード数上位
    writeParams[5] = wordCount & 0xFF;             // 書き込みワード数下位
    
    // FINSパケット全体を構築
    // パケットサイズ = ヘッダー(10) + コマンド(2) + パラメータ(6) + データ(ワード数×2)
    int packetSize = 10 + 2 + 6 + (wordCount * 2);
    uint8_t finsPacket[512];  // 最大512バイト
    int offset = 0;
    
    // FINSヘッダーをパケットに追加
    memcpy(finsPacket + offset, finsHeader, 10);
    offset += 10;
    
    // FINSコマンドをパケットに追加
    memcpy(finsPacket + offset, finsCommand, 2);
    offset += 2;
    
    // 書き込みパラメータをパケットに追加
    memcpy(finsPacket + offset, writeParams, 6);
    offset += 6;
    
    // データをパケットに追加（2バイトずつ、奇数の場合は最後に0x00を追加）
    for (int i = 0; i < dataLen; i += 2) {
      finsPacket[offset++] = (uint8_t)writeData[i];
      if (i + 1 < dataLen) {
        finsPacket[offset++] = (uint8_t)writeData[i + 1];
      } else {
        finsPacket[offset++] = 0x00;  // パディング
      }
    }

    // UDPバッファをクリア（古いパケットを破棄）
    while (udp.parsePacket() > 0) {
      udp.flush();
    }
    
    // UDPパケット送信（一度に全データを送信）
    udp.beginPacket(PLC_IP, PLC_PORT);
    udp.write(finsPacket, packetSize);
    int result = udp.endPacket();
    
    // 送信失敗チェック
    if (result != 1) {
      canvas.println("UDP送信失敗");
      updateDisplay();
      
      // 最終試行でなければ待機
      if (attempt < UDP_RETRY_MAX) {
        canvas.printf("%d秒後に再試行...\n", UDP_RETRY_DELAY / 1000);
        updateDisplay();
        delay(UDP_RETRY_DELAY);
      }
      continue;
    }
    
    canvas.println("FINSコマンド送信完了");
    canvas.println("レスポンス待機中...");
    updateDisplay();
    
    // 最終通信時刻を更新
    lastCommunicationTime = millis();
    
    // レスポンス待機
    unsigned long startTime = millis();
    while (millis() - startTime < FINS_RESPONSE_TIMEOUT) {
      int packetSize = udp.parsePacket();
      if (packetSize >= 14) {  // 最小レスポンスサイズ：ヘッダー10 + コマンド2 + エンドコード2
        // レスポンス受信
        uint8_t response[256];
        int len = udp.read(response, sizeof(response));
        
        // FINSヘッデーをスキップ（10バイト）
        // レスポンスコマンド（2バイト）もスキップ
        // エンドコードを確認（オフセット12番目と13番目）
        if (len >= 14) {
          uint8_t mainEndCode = response[12];
          uint8_t subEndCode = response[13];
          
          canvas.println("レスポンス受信");
          canvas.printf("エンドコード: %02X %02X\n", mainEndCode, subEndCode);
          
          // エンドコード判定
          if (mainEndCode == 0x00 && subEndCode == 0x00) {
            canvas.println("〇 PLC書き込み成功！");
            canvas.printf("DM%d～: %s\n", FINS_START_ADDRESS, writeData.c_str());
            updateDisplay();
            success = true;
            break;
          } else {
            // エラーコードの詳細表示
            canvas.println("× PLCエラー発生");
            
            // 主なエラーコードの解説
            if (mainEndCode == 0x01) {
              canvas.println("ローカルノードエラー");
              if (subEndCode == 0x01) canvas.println("サービス中断");
              else if (subEndCode == 0x02) canvas.println("メモリアクセスエラー");
              else if (subEndCode == 0x03) canvas.println("コマンド長エラー");
            } else if (mainEndCode == 0x02) {
              canvas.println("デスティネーションエラー");
              if (subEndCode == 0x01) canvas.println("ノードアドレス設定エラー");
              else if (subEndCode == 0x05) canvas.println("ノードなし");
            } else if (mainEndCode == 0x03) {
              canvas.println("通信エラー");
            } else if (mainEndCode == 0x11) {
              canvas.println("コマンドエラー");
              if (subEndCode == 0x01) canvas.println("未定義コマンド");
              else if (subEndCode == 0x02) canvas.println("非サポート");
            } else if (mainEndCode == 0x21) {
              canvas.println("メモリエラー");
              if (subEndCode == 0x01) canvas.println("読み書き不可");
              else if (subEndCode == 0x02) canvas.println("アドレス範囲外");
              else if (subEndCode == 0x03) canvas.println("ワード数エラー");
            }
            updateDisplay();
            
            // エラーの場合はリトライ
            if (attempt < UDP_RETRY_MAX) {
              canvas.printf("%d秒後に再試行...\n", UDP_RETRY_DELAY / 1000);
              updateDisplay();
              delay(UDP_RETRY_DELAY);
            }
            break;
          }
        }
      }
      delay(10);  // 短い待機
    }
    
    // レスポンス受信できた場合はループを抜ける
    if (success) {
      break;
    }
    
    // タイムアウトの場合
    // タイムアウトの場合
    if (!success) {
      canvas.println("× レスポンスタイムアウト");
      updateDisplay();
      
      // 最終試行でなければ待機
      if (attempt < UDP_RETRY_MAX) {
        canvas.printf("%d秒後に再試行...\n", UDP_RETRY_DELAY / 1000);
        updateDisplay();
        delay(UDP_RETRY_DELAY);
      }
    }
  }
  
  // 最終結果表示
  if (!success) {
    canvas.println("× PLC書き込み失敗：リトライ上限到達");
    updateDisplay();
  }
}

/**
 * キープアライブパケットを送信
 * FINS 時間情報読み出し（0x07, 0x01）を使用
 */
void sendKeepAlive() {
  // WiFi接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  static uint8_t sid = 0x80;  // キープアライブ用Service ID
  
  // FINSヘッダー作成（10バイト）
  uint8_t finsHeader[10];
  finsHeader[0] = 0x80;              // ICF
  finsHeader[1] = 0x00;              // RSV
  finsHeader[2] = 0x02;              // GCT
  finsHeader[3] = 0x00;              // DNA
  finsHeader[4] = PLC_NODE_ADDRESS;  // DA1
  finsHeader[5] = 0x00;              // DA2
  finsHeader[6] = 0x00;              // SNA
  finsHeader[7] = PC_NODE_ADDRESS;   // SA1
  finsHeader[8] = 0x00;              // SA2
  finsHeader[9] = sid++;             // SID
  
  // FINSコマンド：時間情報読み出し（0x07, 0x01）
  uint8_t finsCommand[2];
  finsCommand[0] = 0x07;  // 時間情報読み出し
  finsCommand[1] = 0x01;  // サブコマンド
  
  // FINSパケット全体を構築
  int packetSize = 10 + 2;  // ヘッダー + コマンド
  uint8_t finsPacket[12];
  int offset = 0;
  
  memcpy(finsPacket + offset, finsHeader, 10);
  offset += 10;
  memcpy(finsPacket + offset, finsCommand, 2);
  offset += 2;
  
  // UDPパケット送信
  udp.beginPacket(PLC_IP, PLC_PORT);
  udp.write(finsPacket, packetSize);
  udp.endPacket();
  
  // 最終通信時刻を更新
  lastCommunicationTime = millis();
}

/**
 * キープアライブチェック
 * 30秒間無通信ならキープアライブパケットを送信
 */
void checkKeepAlive() {
  // WiFi接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  // 30秒経過したかチェック
  if (millis() - lastCommunicationTime >= KEEPALIVE_INTERVAL) {
    sendKeepAlive();
  }
}

// ========================================
// QRコード処理関数
// ========================================

/**
 * QRコード読み取り処理
 */
void handleQRCodeScan() {
  if (qrcode.getDecodeReadyStatus() != 1) {
    return;  // デコード未完了
  }
  
  // データ取得
  uint8_t buffer[512] = {0};
  uint16_t length = qrcode.getDecodeLength();
  qrcode.getDecodeData(buffer, length);
  buffer[length] = '\0';  // Null終端
  
  // 画面表示
  canvas.printf("QR: %s [%s]\n", buffer, currentStatus.c_str());
  updateDisplay();

  // サーバーに送信
  if (currentStatus != "") {
    sendQRCodeData((const char*)buffer, currentStatus.c_str());
  }
  
  // QR読み取り成功したのでタイマーをリセット
  scanStartTime = 0;
}

/**
 * ボタン入力処理
 */
void handleButtonInput() {
  M5.update();
  
  // Aボタン: 整備開始モード
  if (M5.BtnA.wasPressed()) {
    currentStatus = "01";
    qrcode.setDecodeTrigger(1);
    scanStartTime = millis();  // スキャン開始時刻を記録
    canvas.println("整備開始登録します");
    updateDisplay();
  }
  
  // Bボタン: 読み取り中止
  if (M5.BtnB.wasPressed()) {
    qrcode.setDecodeTrigger(0);
    scanStartTime = 0;  // タイマーをリセット
    canvas.println("QR読取中止します");
    updateDisplay();
  }

  // Cボタン: 整備完了モード
  if (M5.BtnC.wasPressed()) {
    currentStatus = "02";
    qrcode.setDecodeTrigger(1);
    scanStartTime = millis();  // スキャン開始時刻を記録
    canvas.println("整備完了登録します");
    updateDisplay();
  }
}

/**
 * スキャンタイムアウトチェック
 * 20秒経過したら自動的に読み取りを中止
 */
void checkScanTimeout() {
  // スキャン中でない場合は何もしない
  if (scanStartTime == 0) {
    return;
  }
  
  // タイムアウトチェック
  if (millis() - scanStartTime >= SCAN_TIMEOUT_MS) {
    qrcode.setDecodeTrigger(0);
    scanStartTime = 0;
    canvas.println("QR読取タイムアウト");
    updateDisplay();
  }
}

// ========================================
// メイン処理
// ========================================

void setup() {
  // M5Stack初期化
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(0);  // スピーカー無音化

  // Canvas初期化（ボタンラベル領域を除く）
  canvas.setColorDepth(1);
  canvas.createSprite(M5.Display.width(), M5.Display.height() - BUTTON_LABEL_HEIGHT * 2 - 10);
  canvas.setFont(&fonts::lgfxJapanGothic_20);
  canvas.setTextScroll(true);
  M5.Display.clear();
  
  drawButtonLabels();

  // SDカードから設定読み込み
  loadSettingsFromSD();

  // QRコードユニット初期化
  while (!qrcode.begin(&Wire, QRCODE_I2C_ADDR, QRCODE_SDA_PIN, QRCODE_SCL_PIN, QRCODE_I2C_SPEED)) {
    canvas.println("QRCodeユニット 初期化失敗");
    updateDisplay();
    delay(1000);
  }
  canvas.println("QRCodeユニット 初期化成功");
  updateDisplay();

  // WiFi接続
  connectWiFi();

  // UDP受信ポートを開く
  if (udp.begin(LOCAL_UDP_PORT)) {
    canvas.printf("UDPポート%dで受信開始\n", LOCAL_UDP_PORT);
  } else {
    canvas.println("UDPポート開放失敗！");
  }
  updateDisplay();
  
  // 最終通信時刻を初期化
  lastCommunicationTime = millis();

  // スキャンモード設定
#ifdef I2C_AUTO_SCAN_MODE
  qrcode.setTriggerMode(AUTO_SCAN_MODE);
  canvas.println("自動スキャンモード");
#else
  qrcode.setTriggerMode(MANUAL_SCAN_MODE);
#endif
  updateDisplay();
}

void loop() {
  handleQRCodeScan();      // QRコード読み取り処理
  
#ifndef I2C_AUTO_SCAN_MODE
  handleButtonInput();     // ボタン入力処理
  checkScanTimeout();      // スキャンタイムアウトチェック
#endif
  
  checkKeepAlive();        // キープアライブチェック
}