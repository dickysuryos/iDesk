#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEClient.h"
#include "BLEUtils.h"
#include "BLE2902.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_bt.h"
#include "esp_gatts_api.h"
#include <string>
#include <vector>
#include <U8g2lib.h> // library for drawing images to the OLED display
#include <Wire.h> // library requires for IIC communication

// OLED display settings (SPI mode)
// ==== PINS (adjust) ====
#define SDA_PIN     8
#define SCL_PIN     9
#define RESET_PIN   U8X8_PIN_NONE   // e.g. 10 if your OLED RES is wired to GPIO10

U8G2_SSD1306_128X64_NONAME_F_SW_I2C  u8g2(U8G2_R0, /* SCL */ SCL_PIN, /* SDA */ SDA_PIN, RESET_PIN);

// ANCS UUIDs
static BLEUUID ancsServiceUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static BLEUUID notificationSourceCharacteristicUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static BLEUUID controlPointCharacteristicUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static BLEUUID dataSourceCharacteristicUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

// Variables for notification handling
uint8_t latestMessageID[4];
bool pendingNotification = false;
std::string notificationTitle = "";
std::string notificationMessage = "";

// Function to request Title & Message from Control Point
void requestNotificationDetails(BLERemoteCharacteristic* pControlPointCharacteristic, uint32_t notificationUID) {
    if (!pControlPointCharacteristic) return;

    uint8_t request[] = {
        0x00, 
        (uint8_t)(notificationUID & 0xFF),
        (uint8_t)((notificationUID >> 8) & 0xFF),
        (uint8_t)((notificationUID >> 16) & 0xFF),
        (uint8_t)((notificationUID >> 24) & 0xFF),
        0x01, 0x1F, 0x00,  // Title Request
        0x03, 0x1F, 0x00   // Message Request (this was missing in some cases!)
    };

    Serial.println("Requesting Full Notification Details...");
    pControlPointCharacteristic->writeValue(request, sizeof(request), true);
}

// Function to remove non-ASCII characters (like emojis)
std::string removeEmojis(const std::string& text) {
    std::string cleanText;
    for (char c : text) {
        if (c >= 32 && c <= 126) { // Keep only printable ASCII characters
            cleanText += c;
        }
    }
    return cleanText;
}

// Updated function to display notifications on OLED.
// If the message is too long to fit, it will be truncated and "..." will be appended.
void displayNotification(std::string title, std::string message) {
    Serial.println("Updating OLED Display...");

    u8g2.clearDisplay();

    // Display "New Notification" header
    u8g2.setCursor(0, 0);
    u8g2.print("New Notification:");

    int yOffset = 12;  // Start below the header

    // Remove emojis from title and message before displaying
    title = removeEmojis(title);
    message = removeEmojis(message);

    // Handle Title Display
    if (!title.empty()) {
        Serial.print("Displaying Title: ");
        Serial.println(title.c_str());

        int maxLineLength = 21;  // OLED width limit in characters
        for (size_t i = 0; i < title.length(); i += maxLineLength) {
            std::string line = title.substr(i, maxLineLength);
            u8g2.setCursor(0, yOffset);
            u8g2.print(line.c_str());
            yOffset += 10;  // Move to next line
            if (yOffset >= 64) break;  // Prevent overflow
        }
    }

    // Adjust spacing dynamically to avoid overwriting
    yOffset += 4; // Extra spacing if the title had multiple lines

    // Handle Message Display
    if (!message.empty() && yOffset < 54) {  // Ensure space for the message
        Serial.print("Displaying Message: ");
        Serial.println(message.c_str());

        int availableLines = (64 - yOffset) / 10;  // Each line takes ~10 pixels in height
        int maxLineLength = 21;
        int maxChars = availableLines * maxLineLength;

        // If the message is too long, truncate and add ellipsis.
        if (message.length() > (size_t)maxChars) {
            if (maxChars > 3) {
                message = message.substr(0, maxChars - 3) + "...";
            } else {
                message = message.substr(0, maxChars);
            }
        }

        // Print the (possibly truncated) message in chunks.
        for (size_t i = 0; i < message.length(); i += maxLineLength) {
            std::string line = message.substr(i, maxLineLength);
            u8g2.setCursor(0, yOffset);
            u8g2.print(line.c_str());
            yOffset += 10;  // Move to next line
            if (yOffset >= 64) break;  // Prevent overflow
        }
    }

    u8g2.display();  // Force OLED update
    Serial.println("OLED Updated!");
}



static void NotificationSourceNotifyCallback(BLERemoteCharacteristic* pNotificationSourceCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    Serial.println("Received New Notification!");
    if (length < 8) {
        Serial.println("Notification data too short!");
        return;
    }

    latestMessageID[0] = pData[4];
    latestMessageID[1] = pData[5];
    latestMessageID[2] = pData[6];
    latestMessageID[3] = pData[7];

    Serial.print("Notification UID: ");
    Serial.println(*(uint32_t*)latestMessageID);

    pendingNotification = true;
}

static void dataSourceNotifyCallback(BLERemoteCharacteristic* pDataSourceCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("Data Source Response Received (Length: ");
    Serial.print(length);
    Serial.println(" bytes)");

    // Print raw data for debugging
    Serial.print("Raw Data: ");
    for (int i = 0; i < length; i++) {
        Serial.printf("%02X ", pData[i]);
    }
    Serial.println("\n");

    // Extract UID and attribute details
    uint32_t receivedUID = *(uint32_t*)&pData[1]; 

    // Parse multiple attributes (Title & Message)
    int index = 5;  // Start from the first attribute
    while (index < length) {
        uint8_t attributeID = pData[index];  
        uint16_t attributeLength = *(uint16_t*)&pData[index + 1];

        Serial.print("Attribute ID: ");
        Serial.println(attributeID);
        Serial.print("Attribute Length: ");
        Serial.println(attributeLength);

        // Ensure attribute fits in received data
        if (attributeLength > 0 && index + 3 + attributeLength <= length) {
            std::vector<char> value(attributeLength + 1, '\0');  // Buffer for clean extraction
            memcpy(value.data(), &pData[index + 3], attributeLength); // Copy data safely

            if (attributeID == 1) { // Title
                notificationTitle = std::string(value.data());
                Serial.print("Title: ");
                Serial.println(notificationTitle.c_str());
            } 
            else if (attributeID == 3) { // Message
                notificationMessage = std::string(value.data());
                Serial.print("Message: ");
                Serial.println(notificationMessage.c_str());
            }
        } else {
            Serial.println("Attribute length mismatch. Data might be truncated.");
        }

        // Move to the next attribute in the data packet
        index += 3 + attributeLength;
    }

    // Display notification as soon as either title or message is available
    if (!notificationTitle.empty() || !notificationMessage.empty()) {
        Serial.println("Displaying notification...");
        displayNotification(notificationTitle, notificationMessage);

        // Keep notification on screen for 5 seconds before clearing
        delay(5000);
        
        Serial.println("Clearing screen for next notification...");
        u8g2.clearDisplay();
        u8g2.display();
    }
}

// BLE Client Task to connect to ANCS Service
void MyClientTask(void* data) {
    BLEAddress* pAddress = (BLEAddress*)data;
    BLEClient* pClient = BLEDevice::createClient();
    pClient->connect(*pAddress);
    
    Serial.println("Searching for ANCS service...");

    BLERemoteService* pAncsService = pClient->getService(ancsServiceUUID);
    if (!pAncsService) {
        Serial.println("ANCS Service Not Found!");
        return;
    }
    Serial.println("ANCS Service Found!");

    BLERemoteCharacteristic* pNotificationSourceCharacteristic = pAncsService->getCharacteristic(notificationSourceCharacteristicUUID);
    BLERemoteCharacteristic* pControlPointCharacteristic = pAncsService->getCharacteristic(controlPointCharacteristicUUID);
    BLERemoteCharacteristic* pDataSourceCharacteristic = pAncsService->getCharacteristic(dataSourceCharacteristicUUID);

    if (pNotificationSourceCharacteristic) {
        Serial.println("Subscribing to Notification Source...");
        pNotificationSourceCharacteristic->registerForNotify(NotificationSourceNotifyCallback);
    } else {
        Serial.println("Notification Source Characteristic Not Found!");
    }

    if (pDataSourceCharacteristic) {
        Serial.println("Subscribing to Data Source...");
        pDataSourceCharacteristic->registerForNotify(dataSourceNotifyCallback);
        
        BLERemoteDescriptor* pDescriptor = pDataSourceCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
        if (pDescriptor) {
            uint8_t enableNotifications[] = {0x01, 0x00};  
            pDescriptor->writeValue(enableNotifications, 2, true);
            Serial.println("Data Source Notifications Enabled!");
        } else {
            Serial.println("Data Source Descriptor Not Found!");
        }
    } else {
        Serial.println("Data Source Characteristic Not Found!");
    }

    if (pControlPointCharacteristic) {
        Serial.println("Sending dummy request to force iOS notification...");
        uint8_t dummyRequest[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1F, 0x00};
        pControlPointCharacteristic->writeValue(dummyRequest, sizeof(dummyRequest), true);
    }

    while (1) {
        if (pendingNotification) {
            Serial.println("Fetching full details...");
            requestNotificationDetails(pControlPointCharacteristic, *(uint32_t*)latestMessageID);
            pendingNotification = false;
        }
        delay(500);
    }
}

// BLE Server Callbacks
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
        Serial.println("Device connected");

        // Force Secure Bonding + Encryption
        BLESecurity* pSecurity = new BLESecurity();
        pSecurity->setStaticPIN(123456); // Prevents loss of bonding on reboot
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
        pSecurity->setCapability(ESP_IO_CAP_NONE);
        pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        // Delay encryption request to allow bonding to complete
        Serial.println("Waiting for bonding to complete...");
        delay(500);

        Serial.println("Requesting encryption...");
        esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);

        BLEAddress* address = new BLEAddress(param->connect.remote_bda);
        xTaskCreate(MyClientTask, "ClientTask", 20000, (void*)address, 1, NULL);
    }

    void onDisconnect(BLEServer* pServer) {
        Serial.println("Device disconnected");
    }
};

// BLE Server Initialization
void setup() {
    Serial.begin(115200);

    u8g2.begin();

    u8g2.clearDisplay();
    u8g2.setCursor(0, 0);
    u8g2.print("Waiting for connection...");
    u8g2.display();

    Serial.println("🗑️ Clearing previous bonding data...");
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > 0) {
        esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t*) malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
        esp_ble_get_bond_device_list(&dev_num, dev_list);
        for (int i = 0; i < dev_num; i++) {
            esp_ble_remove_bond_device(dev_list[i].bd_addr);
            Serial.println("🗑️ Removed old bond data");
        }
        free(dev_list);
    }

    BLEDevice::init("ANCS");

    Serial.println("🔗 Forcing iOS bonding request...");

    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;  // Secure Bonding + MITM
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));

    uint8_t iocap = ESP_IO_CAP_NONE;  // No keyboard input required
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));

    uint8_t key_size = 16;  // Set encryption key size
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));

    uint8_t resp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &resp_key, sizeof(uint8_t));

    BLEServer* pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEAdvertising* pAdvertising = pServer->getAdvertising();
    pAdvertising->addServiceUUID(ancsServiceUUID);

    pAdvertising->setScanResponse(true);  // Send extended advertising data
    pAdvertising->setMinPreferred(0x06);  // Set preferred connection interval
    pAdvertising->setMaxPreferred(0x12);
    pAdvertising->setAppearance(384);  // Appearance value 384 = Generic Audio/Video Device
    pAdvertising->start();
}

void loop() {}