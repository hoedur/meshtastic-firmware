#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
#include "BluetoothCommon.h"
#include "NimbleBluetooth.h"
#include "PowerFSM.h"

#include "main.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"
#include "sleep.h"
#include <NimBLEDevice.h>
#include <mutex>

NimBLECharacteristic *fromNumCharacteristic;
NimBLECharacteristic *BatteryCharacteristic;
NimBLECharacteristic *logRadioCharacteristic;
NimBLEServer *bleServer;

static bool passkeyShowing;

class BluetoothPhoneAPI : public PhoneAPI, public concurrency::OSThread
{
  public:
    BluetoothPhoneAPI() : concurrency::OSThread("NimbleBluetooth") { nimble_queue.resize(3); }
    std::vector<NimBLEAttValue> nimble_queue;
    std::mutex nimble_mutex;
    uint8_t queue_size = 0;
    bool has_fromRadio = false;
    uint8_t fromRadioBytes[meshtastic_FromRadio_size] = {0};
    size_t numBytes = 0;
    bool hasChecked = false;
    bool phoneWants = false;

  protected:
    virtual int32_t runOnce() override
    {
        std::lock_guard<std::mutex> guard(nimble_mutex);
        if (queue_size > 0) {
            for (uint8_t i = 0; i < queue_size; i++) {
                handleToRadio(nimble_queue.at(i).data(), nimble_queue.at(i).length());
            }
            LOG_DEBUG("Queue_size %u", queue_size);
            queue_size = 0;
        }
        if (hasChecked == false && phoneWants == true) {
            numBytes = getFromRadio(fromRadioBytes);
            hasChecked = true;
        }

        return 100;
    }
    /**
     * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
     */
    virtual void onNowHasData(uint32_t fromRadioNum)
    {
        PhoneAPI::onNowHasData(fromRadioNum);

        LOG_DEBUG("BLE notify fromNum");

        uint8_t val[4];
        put_le32(val, fromRadioNum);

        fromNumCharacteristic->setValue(val, sizeof(val));
        fromNumCharacteristic->notify();
    }

    /// Check the current underlying physical link to see if the client is currently connected
    virtual bool checkIsConnected() { return bleServer && bleServer->getConnectedCount() > 0; }
};

static BluetoothPhoneAPI *bluetoothPhoneAPI;
/**
 * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
 */

// Last ToRadio value received from the phone
static uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE];

class NimbleBluetoothToRadioCallback : public NimBLECharacteristicCallbacks
{
#if defined(M5STACK_UNITC6L)
    virtual void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("NimbleBluetoothToRadioCallback onWrite\n");
        auto val = pCharacteristic->getValue();

        bluetoothPhoneAPI->handleToRadio(val.data(), val.length());
    }

    virtual void onNotify(NimBLECharacteristic *pCharacteristic) { LOG_INFO("NimbleBluetoothToRadioCallback onNotify\n"); }

    virtual void onStatus(NimBLECharacteristic *pCharacteristic, int code)
    {
        LOG_INFO("NimbleBluetoothToRadioCallback onStatus\n");
    }

    virtual void onSubscribe(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo, uint16_t subValue)
    {
        LOG_INFO("NimbleBluetoothToRadioCallback onSubscribe\n");
    }
#else
    virtual void onWrite(NimBLECharacteristic *pCharacteristic)
    {
        auto val = pCharacteristic->getValue();

        if (memcmp(lastToRadio, val.data(), val.length()) != 0) {
            if (bluetoothPhoneAPI->queue_size < 3) {
                memcpy(lastToRadio, val.data(), val.length());
                std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
                bluetoothPhoneAPI->nimble_queue.at(bluetoothPhoneAPI->queue_size) = val;
                bluetoothPhoneAPI->queue_size++;
                bluetoothPhoneAPI->setIntervalFromNow(0);
            }
        }
    }
#endif
};

class NimbleBluetoothFromRadioCallback : public NimBLECharacteristicCallbacks
{
#if defined(M5STACK_UNITC6L)
    virtual void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("NimbleBluetoothFromRadioCallback onRead\n");
        uint8_t fromRadioBytes[meshtastic_FromRadio_size];
        size_t numBytes = bluetoothPhoneAPI->getFromRadio(fromRadioBytes);

        std::string fromRadioByteString(fromRadioBytes, fromRadioBytes + numBytes);

        pCharacteristic->setValue(fromRadioByteString);
    }

    virtual void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("NimbleBluetoothFromRadioCallback onWrite\n");
    }

    virtual void onNotify(NimBLECharacteristic *pCharacteristic) { LOG_INFO("NimbleBluetoothFromRadioCallback onNotify\n"); }

    virtual void onStatus(NimBLECharacteristic *pCharacteristic, int code)
    {
        LOG_INFO("NimbleBluetoothFromRadioCallback onStatus\n");
    }

    virtual void onSubscribe(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo, uint16_t subValue)
    {
        LOG_INFO("NimbleBluetoothFromRadioCallback onSubscribe\n");
    }
#else
    virtual void onRead(NimBLECharacteristic *pCharacteristic)
    {
        int tries = 0;
        bluetoothPhoneAPI->phoneWants = true;
        while (!bluetoothPhoneAPI->hasChecked && tries < 100) {
            bluetoothPhoneAPI->setIntervalFromNow(0);
            delay(20);
            tries++;
        }
        std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
        std::string fromRadioByteString(bluetoothPhoneAPI->fromRadioBytes,
                                        bluetoothPhoneAPI->fromRadioBytes + bluetoothPhoneAPI->numBytes);
        pCharacteristic->setValue(fromRadioByteString);

        if (bluetoothPhoneAPI->numBytes != 0) // if we did send something, queue it up right away to reload
            bluetoothPhoneAPI->setIntervalFromNow(0);
        bluetoothPhoneAPI->numBytes = 0;
        bluetoothPhoneAPI->hasChecked = false;
        bluetoothPhoneAPI->phoneWants = false;
    }
#endif
};

class NimbleBluetoothServerCallback : public NimBLEServerCallbacks
{
#if defined(M5STACK_UNITC6L)
    virtual void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("NimbleBluetoothServerCallback onConnect address: %s\n", connInfo.getAddress().toString().c_str());
        bleServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 18);
    }

    virtual void onMTUChange(uint16_t MTU, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("NimbleBluetoothServerCallback MTU updated: %u for connection ID: %u\n", MTU, connInfo.getConnHandle());
        bleServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 60);
    }
    virtual uint32_t onPassKeyDisplay()
    {
#else
    virtual uint32_t onPassKeyRequest()
    {
#endif
        uint32_t passkey = config.bluetooth.fixed_pin;

        if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN) {
            LOG_INFO("Use random passkey");
            // This is the passkey to be entered on peer - we pick a number >100,000 to ensure 6 digits
            passkey = random(100000, 999999);
        }
        LOG_INFO("*** Enter passkey %d on the peer side ***", passkey);

        powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
        bluetoothStatus->updateStatus(new meshtastic::BluetoothStatus(std::to_string(passkey)));

#if HAS_SCREEN // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
        if (screen) {
#if defined(M5STACK_UNITC6L)
            screen->startAlert([passkey](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
                char btPIN[16] = "888888";
                snprintf(btPIN, sizeof(btPIN), "%06u", passkey);
                int x_offset = display->width() / 2;
                int y_offset = display->height() <= 80 ? 0 : 12;
                display->setTextAlignment(TEXT_ALIGN_CENTER);
                display->setFont(FONT_MEDIUM);
                display->drawString(x_offset + x, y_offset + y, "Bluetooth");
                display->setFont(FONT_SMALL);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
                display->drawString(x_offset + x, y_offset + y - 8, "Enter this code");
                display->setFont(FONT_LARGE);
                char pin[8];
                snprintf(pin, sizeof(pin), "%.3s %.3s", btPIN, btPIN + 3);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
                display->drawString(x_offset + x, y_offset + y - 13, pin);
                display->setFont(FONT_SMALL);
                char deviceName[64];
                snprintf(deviceName, sizeof(deviceName), "Name: %s", owner.short_name);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
                display->drawString(x_offset + x, y_offset + y - 20, deviceName);
            });
#else
            screen->startAlert([passkey](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
                char btPIN[16] = "888888";
                snprintf(btPIN, sizeof(btPIN), "%06u", passkey);
                int x_offset = display->width() / 2;
                int y_offset = display->height() <= 80 ? 0 : 12;
                display->setTextAlignment(TEXT_ALIGN_CENTER);
                display->setFont(FONT_MEDIUM);
                display->drawString(x_offset + x, y_offset + y, "Bluetooth");

                display->setFont(FONT_SMALL);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
                display->drawString(x_offset + x, y_offset + y, "Enter this code");

                display->setFont(FONT_LARGE);
                char pin[8];
                snprintf(pin, sizeof(pin), "%.3s %.3s", btPIN, btPIN + 3);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
                display->drawString(x_offset + x, y_offset + y, pin);

                display->setFont(FONT_SMALL);
                char deviceName[64];
                snprintf(deviceName, sizeof(deviceName), "Name: %s", getDeviceName());
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
                display->drawString(x_offset + x, y_offset + y, deviceName);
            });
#endif
        }
#endif
        passkeyShowing = true;

        return passkey;
    }
#if defined(M5STACK_UNITC6L)
    virtual void onAuthenticationComplete(NimBLEConnInfo &connInfo)
#else
    virtual void onAuthenticationComplete(ble_gap_conn_desc *desc)
#endif
    {
        LOG_INFO("BLE authentication complete");

        bluetoothStatus->updateStatus(new meshtastic::BluetoothStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED));

        // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
        if (passkeyShowing) {
            passkeyShowing = false;
            if (screen)
                screen->endAlert();
        }
    }

    virtual void onDisconnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
    {
        LOG_INFO("BLE disconnect");

        bluetoothStatus->updateStatus(
            new meshtastic::BluetoothStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED));

        if (bluetoothPhoneAPI) {
            std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
            bluetoothPhoneAPI->close();
            bluetoothPhoneAPI->hasChecked = false;
            bluetoothPhoneAPI->phoneWants = false;
            bluetoothPhoneAPI->numBytes = 0;
            bluetoothPhoneAPI->queue_size = 0;
        }
    }
};

static NimbleBluetoothToRadioCallback *toRadioCallbacks;
static NimbleBluetoothFromRadioCallback *fromRadioCallbacks;

void NimbleBluetooth::shutdown()
{
    // No measurable power saving for ESP32 during light-sleep(?)
#ifndef ARCH_ESP32
    // Shutdown bluetooth for minimum power draw
    LOG_INFO("Disable bluetooth");
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->reset();
    pAdvertising->stop();
#endif
}

// Proper shutdown for ESP32. Needs reboot to reverse.
void NimbleBluetooth::deinit()
{
#ifdef ARCH_ESP32
    LOG_INFO("Disable bluetooth until reboot");

#ifdef BLE_LED
#ifdef BLE_LED_INVERTED
    digitalWrite(BLE_LED, HIGH);
#else
    digitalWrite(BLE_LED, LOW);
#endif
#endif
    NimBLEDevice::deinit();
#endif
}

// Has initial setup been completed
bool NimbleBluetooth::isActive()
{
    return bleServer;
}

bool NimbleBluetooth::isConnected()
{
    return bleServer->getConnectedCount() > 0;
}

int NimbleBluetooth::getRssi()
{
    if (bleServer && isConnected()) {
        auto service = bleServer->getServiceByUUID(MESH_SERVICE_UUID);
        uint16_t handle = service->getHandle();
#if defined(M5STACK_UNITC6L)
        return NimBLEDevice::getClientByHandle(handle)->getRssi();
#else
        return NimBLEDevice::getClientByID(handle)->getRssi();
#endif
    }
    return 0; // FIXME figure out where to source this
}

void NimbleBluetooth::setup()
{
    // Uncomment for testing
    // NimbleBluetooth::clearBonds();

    LOG_INFO("Init the NimBLE bluetooth module");

    NimBLEDevice::init(getDeviceName());
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM | BLE_SM_PAIR_AUTHREQ_SC);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    }
    bleServer = NimBLEDevice::createServer();

    NimbleBluetoothServerCallback *serverCallbacks = new NimbleBluetoothServerCallback();
    bleServer->setCallbacks(serverCallbacks, true);
#if defined(M5STACK_UNITC6L)
    bleServer->advertiseOnDisconnect(true);
#else
#endif
    setupService();
    startAdvertising();
}

void NimbleBluetooth::setupService()
{
    NimBLEService *bleService = bleServer->createService(MESH_SERVICE_UUID);
    NimBLECharacteristic *ToRadioCharacteristic;
    NimBLECharacteristic *FromRadioCharacteristic;
    // Define the characteristics that the app is looking for
    if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        ToRadioCharacteristic = bleService->createCharacteristic(TORADIO_UUID, NIMBLE_PROPERTY::WRITE);
        FromRadioCharacteristic = bleService->createCharacteristic(FROMRADIO_UUID, NIMBLE_PROPERTY::READ);
        fromNumCharacteristic = bleService->createCharacteristic(FROMNUM_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
        logRadioCharacteristic =
            bleService->createCharacteristic(LOGRADIO_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 512U);
    } else {
        ToRadioCharacteristic = bleService->createCharacteristic(
            TORADIO_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN | NIMBLE_PROPERTY::WRITE_ENC);
        FromRadioCharacteristic = bleService->createCharacteristic(
            FROMRADIO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC);
        fromNumCharacteristic =
            bleService->createCharacteristic(FROMNUM_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                               NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC);
        logRadioCharacteristic = bleService->createCharacteristic(
            LOGRADIO_UUID,
            NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC, 512U);
    }
    bluetoothPhoneAPI = new BluetoothPhoneAPI();

    toRadioCallbacks = new NimbleBluetoothToRadioCallback();
    ToRadioCharacteristic->setCallbacks(toRadioCallbacks);

    fromRadioCallbacks = new NimbleBluetoothFromRadioCallback();
    FromRadioCharacteristic->setCallbacks(fromRadioCallbacks);

    bleService->start();

    // Setup the battery service
    NimBLEService *batteryService = bleServer->createService(NimBLEUUID((uint16_t)0x180f)); // 0x180F is the Battery Service
    BatteryCharacteristic = batteryService->createCharacteristic( // 0x2A19 is the Battery Level characteristic)
        (uint16_t)0x2a19, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);

    NimBLE2904 *batteryLevelDescriptor = (NimBLE2904 *)BatteryCharacteristic->createDescriptor((uint16_t)0x2904);
    batteryLevelDescriptor->setFormat(NimBLE2904::FORMAT_UINT8);
    batteryLevelDescriptor->setNamespace(1);
    batteryLevelDescriptor->setUnit(0x27ad);

    batteryService->start();
}

void NimbleBluetooth::startAdvertising()
{
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->reset();
#if defined(M5STACK_UNITC6L)
    // Create a broadcast data object
    NimBLEAdvertisementData advertisementData;
    // Set advertising Flags
    uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertisementData.setFlags(flags); // Type 0x01
    // Device name
    advertisementData.setName(getDeviceName()); // Type 0x09
    NimBLEAdvertisementData scanResponseData;
    // Add a master service UUID (Type 0x07)
    scanResponseData.addServiceUUID(NimBLEUUID(MESH_SERVICE_UUID)); // Type 0x07
    // Add the battery service UUID to the scan response
    scanResponseData.addServiceUUID(NimBLEUUID((uint16_t)0x1812)); // Type 0x03
    // Apply broadcast and scan response data
    pAdvertising->setAdvertisementData(advertisementData);
    pAdvertising->setScanResponseData(scanResponseData);
#else
    pAdvertising->addServiceUUID(MESH_SERVICE_UUID);
    pAdvertising->addServiceUUID(NimBLEUUID((uint16_t)0x180f)); // 0x180F is the Battery Service
#endif
    pAdvertising->start(0);
}

/// Given a level between 0-100, update the BLE attribute
void updateBatteryLevel(uint8_t level)
{
    if ((config.bluetooth.enabled == true) && bleServer && nimbleBluetooth->isConnected()) {
        BatteryCharacteristic->setValue(&level, 1);
        BatteryCharacteristic->notify();
    }
}

void NimbleBluetooth::clearBonds()
{
    LOG_INFO("Clearing bluetooth bonds!");
    NimBLEDevice::deleteAllBonds();
}

void NimbleBluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    if (!bleServer || !isConnected() || length > 512) {
        return;
    }
    logRadioCharacteristic->notify(logMessage, length, true);
}

void clearNVS()
{
    NimBLEDevice::deleteAllBonds();
#ifdef ARCH_ESP32
    ESP.restart();
#endif
}
#endif
