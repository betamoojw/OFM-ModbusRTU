#include <OpenKNX.h>
#include <Logic.h>
#include "HardwareDevices.h"
#include "hardware.h"
#include "Device.h"
#include "Wire.h"

void appSetup();
void appLoop();

void setup()
{
#ifdef ARDUINO_ARCH_RP2040
    Serial1.setRX(KNX_UART_RX_PIN);
    Serial1.setTX(KNX_UART_TX_PIN);
#endif
    SERIAL_DEBUG.begin(115200);
    pinMode(PROG_LED_PIN, OUTPUT);
    digitalWrite(PROG_LED_PIN, HIGH);
    delay(DEBUG_DELAY);
    digitalWrite(PROG_LED_PIN, LOW);
#ifdef HF_POWER_PIN
    Serial2.setRX(HF_UART_RX_PIN);
    Serial2.setTX(HF_UART_TX_PIN);
    Wire1.setSDA(I2C_SDA_PIN);
    Wire1.setSCL(I2C_SCL_PIN);
    Sensor::SetWire(Wire1);
    pinMode(PRESENCE_LED_PIN, OUTPUT);
    pinMode(MOVE_LED_PIN, OUTPUT);
    pinMode(HF_S1_PIN, INPUT);
    pinMode(HF_S2_PIN, INPUT);
    pinMode(HF_POWER_PIN, OUTPUT);
#endif
    SERIAL_DEBUG.println("Startup called...");
    ArduinoPlatform::SerialDebug = &SERIAL_DEBUG;

#ifdef INFO_LED_PIN
    pinMode(INFO_LED_PIN, OUTPUT);
    ledInfo(true);
#endif

    //I2C Init
    Wire.begin();
    initHW(get_HW_ID());

    // pin or GPIO the programming led is connected to. Default is LED_BUILDIN
    knx.ledPin(get_PROG_LED_PIN(get_HW_ID()));
    // is the led active on HIGH or low? Default is LOW
    knx.ledPinActiveOn(get_PROG_LED_PIN_ACTIVE_ON(get_HW_ID()));
    // pin or GPIO programming button is connected to. Default is 0
    knx.buttonPin(get_PROG_BUTTON_PIN(get_HW_ID()));

#ifdef USERDATA_SAVE_SIZE
  // utilize SaveRestore framework from knx-stack, this has to happen BEFORE knx.read()
  knx.setSaveCallback(Logic::onSaveToFlashHandler);
  knx.setRestoreCallback(Logic::onLoadFromFlashHandler);
#endif

  // all MAIN_* parameters are generated by OpenKNXproducer for correct version checking by ETS
  // If you want just a bugfix firmware update without ETS-Application dependency, just increase firmwareRevision.
  // As soon, as you want again a sync between ETS-Application and firmware, set firmwareRevision to 0.
  const uint8_t firmwareRevision = 0;
  OpenKNX::knxRead(MAIN_OpenKnxId, MAIN_ApplicationNumber, MAIN_ApplicationVersion, firmwareRevision);

  appSetup();

  // start the framework.
  knx.start();
  ledInfo(false);
}

void loop()
{
    // don't delay here to much. Otherwise you might lose packages or mess up the timing with ETS
    knx.loop();

    // only run the application code if the device was configured with ETS
    if (knx.configured())
    {
        appLoop();
    }
}
