#include <Arduino.h>
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEClient.h"
#include "BLEUtils.h"
#include "BLE2902.h"
#include <esp_log.h>
#include <esp_bt_main.h>
#include <string>
#include "Task.h"
#include <sys/time.h>
#include <time.h>
#include "sdkconfig.h"
#include <U8g2lib.h>

static char LOG_TAG[] = "SampleServer";

// ==== OLED PINS ====
#define SDA_PIN     8
#define SCL_PIN     9
#define RESET_PIN   U8X8_PIN_NONE

U8G2_SSD1306_128X64_NONAME_F_SW_I2C  u8g2_sw_ssd1306(U8G2_R2, /* SCL */ SCL_PIN, /* SDA */ SDA_PIN, RESET_PIN);

// ==== ANCS UUIDs ====
static BLEUUID ancsServiceUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static BLEUUID notificationSourceCharacteristicUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static BLEUUID controlPointCharacteristicUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static BLEUUID dataSourceCharacteristicUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

// ==== GLOBALS ====
static BLERemoteCharacteristic* gCtrlPtChar   = nullptr;
static BLERemoteCharacteristic* gNotifSrcChar = nullptr;
static BLERemoteCharacteristic* gDataSrcChar  = nullptr;

uint8_t latestMessageID[4] = {0,0,0,0};
bool pendingNotification = false;
bool incomingCall = false;
uint8_t acceptCall = 0;

// for OLED (safe to update in loop)
String lastMessage = "";
String lastCategory = "";
String lastSender = "";
// flags to tell loop() to redraw
volatile bool needRedrawCategory = false;
volatile bool needRedrawMessage  = false;
volatile bool needRedrawSender = false;

// ================= SECURITY =================
class MySecurity : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest(){
        ESP_LOGI(LOG_TAG, "PassKeyRequest");
        return 123456;
    }
    void onPassKeyNotify(uint32_t pass_key){
        ESP_LOGI(LOG_TAG, "On passkey Notify number:%d", pass_key);
    }
    bool onSecurityRequest(){
        ESP_LOGI(LOG_TAG, "On Security Request");
        return true;
    }
    bool onConfirmPIN(unsigned int){
        ESP_LOGI(LOG_TAG, "On Confrimed Pin Request");
        return true;
    }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl){
        ESP_LOGI(LOG_TAG, "Starting BLE work!");
        if(cmpl.success){
            uint16_t length;
            esp_ble_gap_get_whitelist_size(&length);
            ESP_LOGD(LOG_TAG, "size: %d", length);
        }
    }
};

// ================= DATA SOURCE CALLBACK =================
// DON'T draw OLED here
// ANCS Data Source callback — parse attributes
static void dataSourceNotifyCallback(
  BLERemoteCharacteristic* pDataSourceCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify)
{
    // ANCS format (Notification Attributes):
    // byte 0: CommandID (0 = Get Notification Attributes)
    // byte 1-4: Notification UID
    // then repeated:
    //   1 byte  AttributeID
    //   2 bytes Length (L) little-endian
    //   L bytes Value

    if (length < 5) {
        return;
    }

    uint8_t commandID = pData[0];
    // we ignore notification UID for now
    size_t index = 5; // start after command + 4-byte UID

    while (index + 3 <= length) {
        uint8_t attrID = pData[index];          // which field (app-id, title, msg, date, ...)
        uint16_t attrLen = pData[index + 1] | (pData[index + 2] << 8);  // little endian
        index += 3;

        if (index + attrLen > length) {
            // malformed / truncated
            break;
        }

        // copy value
        String value = "";
        for (uint16_t i = 0; i < attrLen; i++) {
            value += (char)pData[index + i];
        }

        // DEBUG: see everything
        Serial.print("ANCS attr ");
        Serial.print(attrID);
        Serial.print(" = ");
        Serial.println(value);

        // 0 = App Identifier
        // 1 = Title
        // 2 = Subtitle
        // 3 = Message
        // 5 = Date
        if (attrID == 3) {
            // this is the one we want to show
            lastMessage = value;
            needRedrawMessage = true;  // tell loop() to update OLED
        }
        if (attrID == 1) {
            lastSender = value;
            needRedrawSender = true;   // tell loop() to update OLED
        }

        // move to next attribute
        index += attrLen;
    }
}


// ================= NOTIFICATION SOURCE CALLBACK =================
// DON'T draw OLED here
static void NotificationSourceNotifyCallback(
  BLERemoteCharacteristic* pNotificationSourceCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify)
{
    if (length < 8) return;

    if(pData[0] == 0) { // added
        Serial.println("New notification!");

        latestMessageID[0] = pData[4];
        latestMessageID[1] = pData[5];
        latestMessageID[2] = pData[6];
        latestMessageID[3] = pData[7];

        String cat = "Other";
        switch(pData[2]) {
            case 1: incomingCall = true; cat = "Incoming Call"; break;
            case 2: cat = "Missed Call"; break;
            case 4: cat = "Social"; break;
            case 6: cat = "Email"; break;
            case 7: cat = "News"; break;
            default: break;
        }
        lastCategory = cat;
        needRedrawCategory = true;     // tell loop() to draw
        pendingNotification = true;
    }
    else if(pData[0] == 2) {
        Serial.println("Notification removed");
        incomingCall = false;
    }
}

// ================= CLIENT TASK =================
class MyClient: public Task {
    void run(void* data) {

        BLEAddress* pAddress = (BLEAddress*)data;
        BLEClient*  pClient  = BLEDevice::createClient();

        BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
        BLEDevice::setSecurityCallbacks(new MySecurity());

        BLESecurity *pSecurity = new BLESecurity();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
        pSecurity->setCapability(ESP_IO_CAP_IO);
        pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        if (!pClient->connect(*pAddress)) {
            Serial.println("Failed to connect client");
            vTaskDelete(NULL);
            return;
        }

        BLERemoteService* pAncsService = pClient->getService(ancsServiceUUID);
        if (pAncsService == nullptr) {
            Serial.println("ANCS service not found");
            vTaskDelete(NULL);
            return;
        }

        gNotifSrcChar = pAncsService->getCharacteristic(notificationSourceCharacteristicUUID);
        gCtrlPtChar   = pAncsService->getCharacteristic(controlPointCharacteristicUUID);
        gDataSrcChar  = pAncsService->getCharacteristic(dataSourceCharacteristicUUID);

        if (!gNotifSrcChar || !gCtrlPtChar || !gDataSrcChar) {
            Serial.println("ANCS chars missing");
            vTaskDelete(NULL);
            return;
        }

        // enable notifications
        const uint8_t v[] = {0x1, 0x0};
        gDataSrcChar->registerForNotify(dataSourceNotifyCallback);
        gDataSrcChar->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)v, 2, true);

        gNotifSrcChar->registerForNotify(NotificationSourceNotifyCallback);
        gNotifSrcChar->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)v, 2, true);

        Serial.println("ANCS ready!");

        // main BLE client loop
        while (true) {
            if ((pendingNotification || incomingCall) && gCtrlPtChar != nullptr) {

                // ask iPhone for notification attributes
                const uint8_t vIdentifier[] = {
                    0x0,
                    latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                    0x0
                };
                gCtrlPtChar->writeValue((uint8_t*)vIdentifier, sizeof(vIdentifier), true);

                const uint8_t vTitle[] = {
                    0x0,
                    latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                    0x1, 0x0, 0x10
                };
                gCtrlPtChar->writeValue((uint8_t*)vTitle, sizeof(vTitle), true);

                const uint8_t vMessage[] = {
                    0x0,
                    latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                    0x3, 0x0, 0x10
                };
                gCtrlPtChar->writeValue((uint8_t*)vMessage, sizeof(vMessage), true);

                const uint8_t vDate[] = {
                    0x0,
                    latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                    0x5
                };
                gCtrlPtChar->writeValue((uint8_t*)vDate, sizeof(vDate), true);

                // optional: call accept/reject
                while (incomingCall && gCtrlPtChar != nullptr) {
                    if (Serial.available() > 0) {
                        acceptCall = Serial.read();
                        Serial.println((char)acceptCall);
                    }

                    if (acceptCall == '1') {
                        const uint8_t vResponse[] = {
                            0x02,
                            latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                            0x00
                        };
                        gCtrlPtChar->writeValue((uint8_t*)vResponse, sizeof(vResponse), true);
                        acceptCall = 0;
                    } else if (acceptCall == '0') {
                        const uint8_t vResponse[] = {
                            0x02,
                            latestMessageID[0], latestMessageID[1], latestMessageID[2], latestMessageID[3],
                            0x01
                        };
                        gCtrlPtChar->writeValue((uint8_t*)vResponse, sizeof(vResponse), true);
                        acceptCall = 0;
                        incomingCall = false;
                    }

                    delay(50);
                }

                pendingNotification = false;
            }

            delay(100);
        }
    }
};

// ================= SERVER CALLBACKS =================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        Serial.println("********************");
        Serial.println("**Device connected**");
        Serial.println(BLEAddress(param->connect.remote_bda).toString().c_str());
        Serial.println("********************");

        MyClient* pMyClient = new MyClient();
        pMyClient->setStackSize(18000);
        pMyClient->start(new BLEAddress(param->connect.remote_bda));
    };

    void onDisconnect(BLEServer* pServer) {
        Serial.println("************************");
        Serial.println("**Device  disconnected**");
        Serial.println("************************");
        pServer->getAdvertising()->start();
    }
};

// ================= MAIN BLE SERVER TASK =================
class MainBLEServer: public Task {
    void run(void *data) {
        ESP_LOGD(LOG_TAG, "Starting BLE work!");

        BLEDevice::init("ANCS");
        BLEServer* pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());
        BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
        BLEDevice::setSecurityCallbacks(new MySecurity());

        BLEAdvertising *pAdvertising = pServer->getAdvertising();
        BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
        oAdvertisementData.setFlags(0x01);
        _setServiceSolicitation(&oAdvertisementData, BLEUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0"));
        pAdvertising->setAdvertisementData(oAdvertisementData);

        BLESecurity *pSecurity = new BLESecurity();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
        pSecurity->setCapability(ESP_IO_CAP_OUT);
        pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        pAdvertising->start();
        
        ESP_LOGD(LOG_TAG, "Advertising started!");
        delay(portMAX_DELAY);
    }

    void _setServiceSolicitation(BLEAdvertisementData *a, BLEUUID uuid)
    {
      char cdata[2];
      switch(uuid.bitSize()) {
        case 16: {
          cdata[0] = 3;
          cdata[1] = ESP_BLE_AD_TYPE_SOL_SRV_UUID;
          a->addData(std::string(cdata, 2) + std::string((char *)&uuid.getNative()->uuid.uuid16,2));
          break;
        }
        case 128: {
          cdata[0] = 17;
          cdata[1] = ESP_BLE_AD_TYPE_128SOL_SRV_UUID;
          a->addData(std::string(cdata, 2) + std::string((char *)uuid.getNative()->uuid.uuid128,16));
          break;
        }
        default:
          return;
      }
    }
};

void SampleSecureServer(void)
{
    MainBLEServer* pMainBleServer = new MainBLEServer();
    pMainBleServer->setStackSize(20000);
    pMainBleServer->start();
}

// ================= ARDUINO =================
void setup()
{
    Serial.begin(115200);

    u8g2_sw_ssd1306.begin();
    u8g2_sw_ssd1306.clearBuffer();
    u8g2_sw_ssd1306.setFont(u8g2_font_6x10_tr);
    u8g2_sw_ssd1306.drawStr(0, 12, "ANCS waiting...");
    u8g2_sw_ssd1306.sendBuffer();

    SampleSecureServer();
}

void oledDrawWrappedLines(const String &text, uint8_t startY) {
    const uint8_t maxCharsPerLine = 21;  // for 128px with 6x10 font
    const uint8_t lineHeight = 10;
    uint8_t y = startY;
    String remaining = text;

    while (remaining.length() > 0 && y <= 62) {
        String line = remaining.substring(0, maxCharsPerLine);
        remaining.remove(0, maxCharsPerLine);
        u8g2_sw_ssd1306.drawUTF8(0, y, line.c_str());
        y += lineHeight;
    }

    // if still text left but no space: show ellipsis on last line
    if (remaining.length() > 0 && y - lineHeight <= 62) {
        u8g2_sw_ssd1306.drawStr(0, y - lineHeight, "...");
    }
}

void loop()
{
    // draw category (new notification)
    // if (needRedrawCategory) {
    //     needRedrawCategory = false;
    //     u8g2_sw_ssd1306.clearBuffer();
    //     u8g2_sw_ssd1306.setFont(u8g2_font_6x10_tr);
    //     u8g2_sw_ssd1306.drawStr(0, 10, "New Notification!");
    //     u8g2_sw_ssd1306.drawUTF8(0, 26, lastCategory.c_str());
    //     u8g2_sw_ssd1306.drawStr(0, 42, "Getting details...");
    //     u8g2_sw_ssd1306.sendBuffer();
    // }

    // draw message text
    if (needRedrawMessage) {
        needRedrawMessage = false;
        u8g2_sw_ssd1306.clearBuffer();
        u8g2_sw_ssd1306.setFont(u8g2_font_6x10_tr);

        // 1st line: sender (wrap if long)
        oledDrawWrappedLines(lastSender, 9);

        // figure out where to start the message
        // sender may have 1 or more lines
        uint8_t senderLines = (lastSender.length() + 20) / 21; // ceil
        uint8_t nextY = 10 + senderLines * 10; // 10px per line

        // add a small gap
        nextY += 2;

        // 2nd part: message
        oledDrawWrappedLines(lastMessage, nextY);

        u8g2_sw_ssd1306.sendBuffer();
    }

    delay(50);
}
