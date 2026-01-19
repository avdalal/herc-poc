#include <GxEPD.h>
#include <GxDEPG0290BS/GxDEPG0290BS.h>
#include <GxIO/GxIO_SPI/GxIO_SPI.h>
#include <GxIO/GxIO.h>
#include <SPI.h>

#include <NimBLEDevice.h>
#include <vector>

// Pins for ESP32-S3
#define EPD_CS   10
#define EPD_DC   9
#define EPD_RST  8
#define EPD_BUSY 7
#define BTN_UP   2
#define BTN_DOWN 3
#define BTN_SEL  4
#define BTN_BACK 5
#define EPD_SCK  12
#define EPD_MOSI 11

// RGB LED pins
#define LED_R 16
#define LED_G 17
#define LED_B 18

GxIO_Class io(SPI, EPD_CS, EPD_DC, EPD_RST);
GxEPD_Class display(io, EPD_RST, EPD_BUSY);

struct DeviceItem {
  String name;
  int rssi;
};
std::vector<DeviceItem> menuItems;
int selectedItem = 0;
String userName = "Israel Israeli";

// ---------- BLE UART (Nordic UART Service / NUS) ----------
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // Write (Android -> ESP32)
static const char* NUS_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // Notify (ESP32 -> Android)

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;

bool bleUartAdvertising = false;
bool bleUartConnected = false;

volatile bool bleNewMsg = false;
String bleLastMsg = "";

void rgbSet(int r, int g, int b) {
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
}

void performAction(String status, int r, int g, int b) {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, 60);
  display.print(status);
  display.update();
  rgbSet(r,g,b);
  delay(400);
  rgbSet(0,0,0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleUartConnected = true;
    Serial.println("BLE UART connected");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleUartConnected = false;
    Serial.println("BLE UART disconnected");
    // Keep advertising if user enabled it
    if (bleUartAdvertising) {
      NimBLEDevice::startAdvertising();
    }
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    if (v.empty()) return;

    // Treat as UTF-8 text for now
    bleLastMsg = String(v.c_str());
    bleNewMsg = true;

    Serial.print("BLE RX: ");
    Serial.println(bleLastMsg.c_str());
  }
};

void bleUartInit() {
  NimBLEDevice::init("ESP32S3-UART"); // This is what Android will see
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max-ish power (adjust if needed)
  const char* devName = "HercPOC";

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = pServer->createService(NUS_SERVICE_UUID);

  pTxChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  pRxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->setName(devName);
  adv->start(false);          // start advertising now (non-blocking)
  bleUartAdvertising = true;  // default to ON; you can default this to false if you prefer
}

void bleUartStartAdvertising() {
  bleUartAdvertising = true;
  NimBLEDevice::startAdvertising();
}

void bleUartStopAdvertising() {
  bleUartAdvertising = false;
  NimBLEDevice::stopAdvertising();
}

void bleUartSend(const String& msg) {
  if (!bleUartConnected || !pTxChar) return;
  pTxChar->setValue(msg.c_str());
  pTxChar->notify();
}

// ---------- BLE Scan Menu ----------
void scan_ble_devices() {
  menuItems.clear();
  menuItems.push_back({"BLE UART (Android)", 0});

  // If you truly want adv off during scan, do it here (optional)
  // bleUartStopAdvertising();

  NimBLEScan* scan = NimBLEDevice::getScan();

  // Hard reset scan state (important when re-scanning)
  scan->stop();
  scan->clearResults();

  // Less aggressive settings (more reliable)
  scan->setActiveScan(true);       // Passive scan first; switch to true only if you need scan response data
  scan->setInterval(200);           // ms (approx)
  scan->setWindow(200);              // ms (must be <= interval)
  scan->setDuplicateFilter(true);   // Reduce spam/duplicates
  
  // Blocking scan; results returned
  NimBLEScanResults results = scan->getResults(5000);
  int count = results.getCount();

  Serial.print("Scan count: ");
  Serial.println(count);

  for (int i = 0; i < count; ++i) {
    // Use a reference (avoids pointer/temp issues across NimBLE versions)
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);

    String n = dev->haveName() ? String(dev->getName().c_str()) : "";
    if (n == "" || n == "Unnamed") {
      n = "Device [" + String(dev->getAddress().toString().c_str()).substring(12) + "]";
    }

    int rssi = dev->getRSSI();

    bool exists = false;
    for (auto &d : menuItems) {
      if (d.name == n) { exists = true; break; }
    }
    if (!exists) menuItems.push_back({n, rssi});
  }

  if ((int)menuItems.size() == 1) {
    menuItems.push_back({"No BLE found", 0});
  }

  selectedItem = 0;
  scan->clearResults();
}

void drawMenu() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(5, 15);
  display.print("User: " + userName);

  int maxVisibleItems = 4;
  int menuCount = menuItems.size();

  int startingIndex = 0;
  if (selectedItem >= maxVisibleItems) {
    startingIndex = selectedItem - (maxVisibleItems - 1);
  }

  for (int i = 0; i < maxVisibleItems; i++) {
    int itemIdx = startingIndex + i;
    if (itemIdx >= menuCount) break;

    int yPos = 40 + (i * 25);
    int rectY = 25 + (i * 25);

    if (itemIdx == selectedItem) {
      display.fillRect(0, rectY, 210, 25, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
    } else {
      display.setTextColor(GxEPD_BLACK);
    }

    display.setCursor(10, yPos);
    String itemStr = menuItems[itemIdx].name;
    if (itemIdx != 0 && menuItems[itemIdx].rssi != 0 && menuItems[itemIdx].name != "No BLE found") {
      itemStr += " [" + String(menuItems[itemIdx].rssi) + "]";
    }
    display.print(itemStr);
  }

  // Side hints
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(220, 30); display.print(selectedItem > 0 ? "^ UP" : "  ---");
  display.setCursor(220, 55); display.print(selectedItem < menuCount - 1 ? "v DOWN" : "  ---");
  display.setCursor(220, 80); display.print("OK SEL");
  display.setCursor(220, 105); display.print("X BACK");

  display.update();
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  display.init();
  display.setRotation(1);
  display.fillScreen(GxEPD_WHITE);
  display.update();

  // Init BLE UART (advertising NUS)
  bleUartInit();

  // Populate menu
  scan_ble_devices();
  drawMenu();
}

void loop() {
  // If a BLE message arrived, show it
  if (bleNewMsg) {
    bleNewMsg = false;
    performAction("BLE RX:\n" + bleLastMsg, 0, 1, 0); // green
    drawMenu();
  }

  if (digitalRead(BTN_UP) == LOW) {
    if (selectedItem > 0) {
      selectedItem--;
      rgbSet(0,1,1);
      drawMenu();
      delay(200);
      rgbSet(0,0,0);
    }
  }

  if (digitalRead(BTN_DOWN) == LOW) {
    if (selectedItem < (int)menuItems.size() - 1) {
      selectedItem++;
      rgbSet(1,0,1);
      drawMenu();
      delay(200);
      rgbSet(0,0,0);
    }
  }

  if (digitalRead(BTN_SEL) == LOW) {
    // If UART menu item selected: toggle advertising
    if (selectedItem == 0) {
      if (bleUartAdvertising) {
        bleUartStopAdvertising();
        performAction("BLE UART: ADV OFF", 1, 0, 0); // red
      } else {
        bleUartStartAdvertising();
        performAction("BLE UART: ADV ON", 0, 0, 1); // blue
      }
      drawMenu();
      delay(250);
      return;
    }

    // For your other menu items, keep your existing behavior
    String selString = menuItems[selectedItem].name + " (RSSI " + String(menuItems[selectedItem].rssi) + ")";
    performAction("Unlocking: " + selString, 0,0,1);
    delay(1000);
    drawMenu();
  }

  if (digitalRead(BTN_BACK) == LOW) {
    performAction("Rescanning BLE...", 1,1,0);
    delay(500);
    scan_ble_devices();
    drawMenu();
    delay(250);
  }
}