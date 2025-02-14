#include "Modbus.h"
#include "Device.h"
#include "ModbusGateway.h"
#include <knx.h>
// #include "hardware.h"
#include "KnxHelper.h"
#include "LED_Statusanzeige.h"
#include "ModbusMaster.h"

uint32_t Modbus::sendDelay[MOD_ChannelCount];
// Flag that value is valid
bool Modbus::valueValid[MOD_ChannelCount];
bool Modbus::errorState[MOD_ChannelCount][2];
bool Modbus::_readyToSend[MOD_ChannelCount];
uint8_t Modbus::_skipCounter[MOD_ChannelCount];

Modbus::values_t Modbus::lastSentValue[MOD_ChannelCount];

float Modbus::lastSentPowerZ[] = {0.67};
float Modbus::lastSentCounterZ[] = {0.056};
float Modbus::powerZ[maxMeters];
float Modbus::counterZ[maxMeters];
float Modbus::powerZ1;
float Modbus::powerZ2;
float Modbus::powerZ3;
float Modbus::powerZ4;
float Modbus::counterZ1;
float Modbus::counterZ2;
float Modbus::counterZ3;
float Modbus::counterZ4;

Modbus::Modbus(void)
{
}

void Modbus::initSlave(uint8_t slaveID, Stream &serialModbus, uint8_t RegisterPos, uint8_t RegisterStart)
{
    begin(slaveID, serialModbus);
    _RegisterPos = RegisterPos;
    _RegisterStart = RegisterStart;
    _slaveID = slaveID;

    // set Zäherstand Offset
    counterZ1 = knx.paramInt(MOD_ModbusZaehler1Offset);

    // set Skipcounter
    for (int i = 0; i < MOD_ChannelCount; i++)
        _skipCounter[i] = 2;
}

uint8_t Modbus::getRegisterstart()
{
    return _RegisterStart;
}

uint8_t Modbus::getSlaveID()
{
    return _slaveID;
}

inline uint16_t Modbus::adjustRegisterAddress(uint16_t u16ReadAddress)
{
    return u16ReadAddress && _RegisterStart ? u16ReadAddress - 1 : u16ReadAddress;
}

uint32_t Modbus::convertFloatTo32Bit(float num)
{
    union
    {
        uint32_t data32;
        float num;
    } intfloat = {.num = num};
    return intfloat.data32;
}

bool Modbus::getErrorState1(uint8_t channel)
{
    return errorState[channel][0];
}

bool Modbus::getErrorState2(uint8_t channel)
{
    return errorState[channel][1];
}

void Modbus::setSkipCounter(uint8_t channel, uint8_t value)
{
    _skipCounter[channel] = value;
}

uint8_t Modbus::decreaseSkipCounter(uint8_t channel)
{
    return _skipCounter[channel]--;
}

uint8_t Modbus::getSkipCounter(uint8_t channel)
{
    return _skipCounter[channel];
}

void Modbus::debugMsgClear(uint8_t channel)
{
    uint8_t debugInfo = channel;
    debugInfo = debugInfo & ~(1 << 7);
    knx.getGroupObject(MOD_KoDebugModbus).value(debugInfo, getDPT(VAL_DPT_5));
}

void Modbus::debugMsgSet(uint8_t channel)
{
    uint8_t debugInfo = channel;
    debugInfo = debugInfo | (1 << 7);
    knx.getGroupObject(MOD_KoDebugModbus).value(debugInfo, getDPT(VAL_DPT_5));
}

void Modbus::processMeter(uint8_t channel, float newValue)
{
    if (knx.paramByte(getPar(MOD_CHModBusSelectionVirtualZaehler1, channel))) // Zähler aktiv
    {
        //// SERIAL_DEBUG.print("vZ1 = Aktiv |");
        switch (knx.paramByte(getPar(MOD_CHModBusTypZaehler1, channel)))
        {
            // Leistung
            case 0:
                //// SERIAL_DEBUG.print(" Pwr |");
                if (knx.paramByte(getPar(MOD_CHModBusMathOperationVirtualZaehler1, channel))) // Vorzeichen = "-"
                {
                    powerZ1 = powerZ1 - newValue;
                    //// SERIAL_DEBUG.print(" - | ");
                    //// SERIAL_DEBUG.println(powerZ1);
                }
                else
                {
                    powerZ1 = powerZ1 + newValue;
                    //// SERIAL_DEBUG.print(" + | ");
                    //// SERIAL_DEBUG.println(powerZ1);
                }
                break;
            // Zählerstand
            case 1:
                //// SERIAL_DEBUG.print(" Count |");
                if (knx.paramByte(getPar(MOD_CHModBusMathOperationVirtualZaehler1, channel))) // Vorzeichen = "-"
                {
                    counterZ1 = counterZ1 - newValue;
                    //// SERIAL_DEBUG.print(" - | ");
                    //// SERIAL_DEBUG.println(counterZ1);
                }
                else
                {
                    counterZ1 = counterZ1 + newValue;
                    //// SERIAL_DEBUG.print(" + | ");
                    //// SERIAL_DEBUG.println(counterZ1);
                }
                break;

            default:
                break;
        }
    }
}

void Modbus::ErrorHandling(uint8_t channel)
{
    if (errorState[channel][0] == false && errorState[channel][1] == false)
    {
        errorState[channel][0] = true;
    }
    // set Fehler Stufe 2
    else if (errorState[channel][0] == true && errorState[channel][1] == false)
    {
        errorState[channel][1] = true;
    }
}

void Modbus::ErrorHandlingLED()
{
    bool error = false;
    for (int i = 0; i < MOD_ChannelCount; i++)
    {
        if (errorState[i][0])
        {
            error = true;
        }
    }
    if (error)
        setLED(MODBUS_ERROR, HIGH);
    else
        setLED(MODBUS_ERROR, LOW);
}

void Modbus::ReadyToSendModbus(uint8_t channel)
{
    _readyToSend[channel] = true;
}

bool Modbus::sendModbus(uint8_t channel)
{
    uint8_t dpt = knx.paramByte(getPar(MOD_CHModBusDptSelection, channel));
    if (dpt == 0) // Kein DPT ausgewählt, daher abbruch
        return false;
    else if (dpt > 14) // >14 dann ist der dpt nicht in Spec, damit abbruch
        return false;

    return knxToModbus(dpt, channel, true);
}

bool Modbus::readModbus(uint8_t channel, bool readRequest)
{
    // 1. DPT auslesen: bei 0 abbrechen
    // 2. Richtung bestimmen: KNX - Modbus / Modbus - KNX
    // 3. Funktion bestimmen: 0x03 Lese holding Reg, ...
    // 4. Register bestimmen
    // 5. Registertyp wenn notwendig
    // 6. Register Position wenn notwendig
    // 7. Modbus Wert abfragen
    // 8. KNX Botschaft senden

    uint8_t dpt = knx.paramByte(getPar(MOD_CHModBusDptSelection, channel));
    if (dpt == 0) // Kein DPT ausgewählt, daher abbruch
        return false;
    else if (dpt > 14) // >14 dann ist der dpt nicht in Spec, damit abbruch
        return false;

    // ERROR LED
    ErrorHandlingLED();

    // Richtungsauswahl: KNX - Modbus oder Modbus - KNX
    switch (knx.paramByte(getPar(MOD_CHModBusBusDirection, channel)))
    {
        case 0: // KNX -> modbus
            // if (readRequest)
            //{
            ////     SERIAL_DEBUG.println("KNX->Modbus ");
            // }
            if (_readyToSend[channel])
            {
                _readyToSend[channel] = false;
                sendModbus(channel);
            }
            return false;
            break;

        case 1: // modbus -> KNX
            return modbusToKnx(dpt, channel, readRequest);
            break;

        default:
            return false;
            break;
    }
}

uint16_t flipMsbLsb(uint16_t value)
{
    uint8_t MSB; // Arduino MSB = Modbus LSB
    uint8_t LSB; // Arduino LSB = Modbus MSB

    LSB = (uint8_t)value;
    MSB = (uint8_t)(value >> 8);

    return (((uint16_t)LSB) << 8 | MSB); // new setup with MSB first !!!
}

uint8_t Modbus::sendProtocol(uint8_t channel, uint16_t registerAddr, uint16_t u16value)
{
    if (0x06 == knx.paramByte(getPar(MOD_CHModBusWriteWordFunktion, channel)))
    {
        //// SERIAL_DEBUG.print(" 0x06 ");
        return writeSingleRegister(registerAddr, u16value);
    }
    else if (0x10 == knx.paramByte(getPar(MOD_CHModBusWriteWordFunktion, channel)))
    {
        //// SERIAL_DEBUG.print(" 0x10 ");
        setTransmitBuffer(0, u16value);
        return writeMultipleRegisters(registerAddr, 1);
    }
    return ku8MBIllegalFunction;
}

void Modbus::printDebugResult(const char *dpt, uint16_t registerAddr, uint8_t result)
{
    //// SERIAL_DEBUG.print(" DPT");
    //// SERIAL_DEBUG.print(dpt);
    //// SERIAL_DEBUG.print(" ");
    //// SERIAL_DEBUG.print(registerAddr);
    //// SERIAL_DEBUG.print(" ");
    switch (result)
    {
        case ku8MBSuccess:
            //// SERIAL_DEBUG.println("DONE");
            break;
        case ku8MBInvalidSlaveID:
            //// SERIAL_DEBUG.println("ERROR: Invalid Slave ID");
            break;
        case ku8MBInvalidFunction:
            //// SERIAL_DEBUG.println("ERROR: Invalid Function");
            break;
        case ku8MBResponseTimedOut:
            //// SERIAL_DEBUG.println("ERROR: Response Timed Out");
            break;
        case ku8MBInvalidCRC:
            //// SERIAL_DEBUG.println("ERROR: Invalid CRC");
            break;
        default:
            //// SERIAL_DEBUG.println("ERROR");
            break;
    }
}

bool Modbus::knxToModbus(uint8_t dpt, uint8_t channel, bool readRequest)
{
    uint8_t result;
    uint16_t registerAddr = adjustRegisterAddress(knx.paramInt(getPar(MOD_CHModbusRegister, channel)));
    GroupObject iKo = knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel));

    ////SERIAL_DEBUG.print("KNX: CH");
    ////SERIAL_DEBUG.print(channel + 1);

    //*****************************************************************************************************************************************
    //*****************************************  DPT 1.001 ************************************************************************************
    //*****************************************************************************************************************************************
    if (dpt == 1)
    {
        if (readRequest)
        {
            bool v = (bool)iKo.value(getDPT(VAL_DPT_1)) ^ (knx.paramByte(getPar(MOD_CHModBusInputTypInvDpt1, channel)));

            // Bit Register
            if (knx.paramByte(getPar(MOD_CHModBusInputTypDpt1, channel)) == 0)
            {
                ////SERIAL_DEBUG.print(" 0x05 ");
                result = writeSingleCoil(registerAddr, v);
            }
            // Bit in Word
            else if (knx.paramByte(getPar(MOD_CHModBusInputTypDpt1, channel)) == 1)
            {
                uint16_t value = v << knx.paramByte(getPar(MOD_CHModBusBitPosDpt1, channel));
                result = sendProtocol(channel, registerAddr, value);
            }
            else
            {
                return false;
            }
            printDebugResult("1.001", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 5.004 ************************************************************************************
    //*****************************************************************************************************************************************
    else if (4 == dpt)
    {
        if (readRequest)
        {
            result = sendProtocol(channel, registerAddr, iKo.value(getDPT(VAL_DPT_5001)));
            printDebugResult("5.004", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 5.010 ************************************************************************************
    //*****************************************************************************************************************************************
    else if (5 == dpt)
    {
        if (readRequest)
        {
            uint16_t v = iKo.value(getDPT(VAL_DPT_5001));

            switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT5, channel)))
            {
                case 1: // High Byte
                    v <<= 8;
                    break;
                case 2: // Low Byte
                    // already at correct position
                    break;
                case 3: // frei Wählbar
                    v &= knx.paramByte(getPar(MOD_CHModbusCountBitsDPT56, channel));
                    v <<= knx.paramByte(getPar(MOD_CHModBusOffsetRight5, channel));
                    break;
                default:
                    return false;
            } // Ende Register Pos

            result = sendProtocol(channel, registerAddr, v);
            printDebugResult("5.001", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 7 ***************************************************************************************
    //*****************************************************************************************************************************************
    else if (7 == dpt)
    {
        if (readRequest)
        {
            uint16_t v = iKo.value(getDPT(VAL_DPT_7));

            switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT7, channel)))
            {
                case 1: // High/LOW Byte
                    // already at correct position
                    break;
                case 2: // frei Wählbar
                    v &= ((1 << knx.paramByte(getPar(MOD_CHModbusCountBitsDPT7, channel))) - 1);
                    v <<= (knx.paramByte(getPar(MOD_CHModBusOffsetRight7, channel)));
                    break;
                default:
                    return false;
            } // Ende Register Pos

            result = sendProtocol(channel, registerAddr, v);
            printDebugResult("5", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 8 signed 16Bit ***************************************************************************
    //*****************************************************************************************************************************************
    else if (8 == dpt)
    {
        if (readRequest)
        {
            result = sendProtocol(channel, registerAddr, iKo.value(getDPT(VAL_DPT_8)));
            printDebugResult("8", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 9 ***************************************************************************************
    //*****************************************************************************************************************************************
    else if (9 == dpt)
    {
        if (readRequest)
        {
            float raw = iKo.value(getDPT(VAL_DPT_9));
            uint16_t v;
            // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
            switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT9, channel)))
            {
                case 1: // Low Byte unsigned
                    v = ((uint16_t)roundf(raw)) & 0xff;
                    break;
                case 2: // High Byte unsigned
                    v = (((uint16_t)roundf(raw)) >> 8) & 0xff;
                    break;
                case 3: // High/Low Byte unsigned
                    v = (uint16_t)roundf(raw);
                    break;
                case 4: // Low Byte signed
                    v = ((int)roundf(raw)) & 0xff;
                    break;
                case 5: // High Byte signed
                    v = (((int)roundf(raw)) >> 8) & 0xff;
                    break;
                case 6: // High/Low Byte signed
                    v = (int)roundf(raw);
                    break;
                default:
                    return false;
            }
            result = sendProtocol(channel, registerAddr, v);
            printDebugResult("9", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 12 ***************************************************************************************
    //*****************************************************************************************************************************************
    else if (12 == dpt)
    {
        if (readRequest)
        {
            uint32_t v = iKo.value(getDPT(VAL_DPT_12));
            setTransmitBuffer(0, v & 0xffff);
            setTransmitBuffer(1, v >> 16);
            result = writeMultipleRegisters(registerAddr, 2);
            printDebugResult("12 0x10", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 13 ***************************************************************************************
    //*****************************************************************************************************************************************
    else if (13 == dpt)
    {
        if (readRequest)
        {
            int32_t v = iKo.value(getDPT(VAL_DPT_13));
            setTransmitBuffer(0, v);
            setTransmitBuffer(1, v >> 16);
            result = writeMultipleRegisters(registerAddr, 2);
            printDebugResult("13 0x10", registerAddr, result);
        }
    }
    //*****************************************************************************************************************************************
    //*****************************************  DPT 14 ***************************************************************************************
    //*****************************************************************************************************************************************
    else if (14 == dpt)
    {
        if (readRequest)
        {
            float raw = iKo.value(getDPT(VAL_DPT_14));
            union floatint
            {
                float floatVal;
                uint32_t intVal;
            };
            uint32_t v = ((floatint *)&raw)->intVal;
            // HI / LO   OR   LO / Hi  order
            if (knx.paramByte(getPar(MOD_CHModBusWordPosDpt14, channel)) == 0)
            { // HI / LO
                setTransmitBuffer(0, v >> 16);
                setTransmitBuffer(1, v);
            }
            else
            { // LO / HI
                setTransmitBuffer(0, v);
                setTransmitBuffer(1, v >> 16);
            }
            result = writeMultipleRegisters(registerAddr, 2);
            printDebugResult("14 0x10", registerAddr, result);
        }
    }

    return true;
}

/**********************************************************************************************************
 **********************************************************************************************************
 *  Modbus to KNX
 *
 *
 ***********************************************************************************************************
 **********************************************************************************************************/

// Routine zum Einlesen des ModBus-Register mit senden auf KNX-Bus
bool Modbus::modbusToKnx(uint8_t dpt, uint8_t channel, bool readRequest)
{

    bool lSend = !valueValid[channel] && readRequest; // Flag if value should be send on KNX
    uint8_t result;
    uint16_t registerAddr = adjustRegisterAddress(knx.paramInt(getPar(MOD_CHModbusRegister, channel)));

    if (readRequest)
    {
        // SERIAL_DEBUG.print("Modbus->KNX ");
        // SERIAL_DEBUG.print(registerAddr);
        // SERIAL_DEBUG.print(" ");
    }

    {
        uint32_t lCycle = knx.paramInt(getPar(MOD_CHModBusSendDelay, channel)) * 1000;

        // if cyclic sending is requested, send the last value if one is available
        lSend |= (lCycle && delayCheck(sendDelay[channel], lCycle) && valueValid[channel]);
    }

    // wählt den passenden DPT
    switch (dpt)
    {
        //*****************************************************************************************************************************************
        //*****************************************  DPT 1.001 ************************************************************************************
        //*****************************************************************************************************************************************
        case 1:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT1 |");
                //  clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                bool v = false;

                // Bit Register
                if (knx.paramByte(getPar(MOD_CHModBusInputTypDpt1, channel)) == 0)
                {
                    switch (knx.paramByte(getPar(MOD_CHModBusReadBitFunktion, channel))) // Choose Modbus Funktion (0x01 readHoldingRegisters ODER 0x02 readInputRegisters)
                    {
                        case 1: // 0x01 Lese Coils
                            // SERIAL_DEBUG.print(" 0x01 ");
                            result = readCoils(registerAddr, 1);
                            break;

                        case 2:
                            // SERIAL_DEBUG.print(" 0x02 ");
                            result = readDiscreteInputs(registerAddr, 1);
                            break;
                        default: // default Switch(ModBusReadBitFunktion)
                            return false;
                    }
                    if (result == ku8MBSuccess)
                    {
                        v = getResponseBuffer(0);
                    }
                }
                // Bit in Word
                else if (knx.paramByte(getPar(MOD_CHModBusInputTypDpt1, channel)) == 1)
                {
                    switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                    {
                        case 3: // 0x03 Lese holding registers
                            // SERIAL_DEBUG.print(" 0x03 ");
                            result = readHoldingRegisters(registerAddr, 1);
                            break; // Ende 0x03

                        case 4:
                            // SERIAL_DEBUG.print(" 0x04 ");
                            result = readInputRegisters(registerAddr, 1);
                            break;
                        default: // Error Switch (0x03 & 0x04)
                            return false;
                    } // Ende Switch (0x03 & 0x04)
                    if (result == ku8MBSuccess)
                    {
                        v = (getResponseBuffer(0) >> knx.paramByte(getPar(MOD_CHModBusBitPosDpt1, channel)) & 1);
                    }
                }
                else
                {
                    return false;
                }

                if (result == ku8MBSuccess)
                {
                    // Invertiert
                    if (knx.paramByte(getPar(MOD_CHModBusInputTypInvDpt1, channel)) > 0)
                    {
                        v = !v;
                    }
                    // senden bei Wertänderung
                    bool lAbsoluteBool = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsoluteBool && v != lastSentValue[channel].lValueBool);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_1));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueBool = v;
                    }

                    // SERIAL_DEBUG.println(v);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler bei der Übertragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);
                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 5.001 ************************************************************************************
        //*****************************************************************************************************************************************
        case 4:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT5.001 |");

                // clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                uint8_t v;

                // Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                {
                    case 3: // 0x03 Lese holding registers
                        // SERIAL_DEBUG.print(" 0x03 ");
                        result = readHoldingRegisters(registerAddr, 1);
                        break; // Ende 0x03

                    case 4:
                        // SERIAL_DEBUG.print(" 0x04 ");
                        result = readInputRegisters(registerAddr, 1);
                        break;
                    default: // Error Switch (0x03 & 0x04)
                        return false;
                } // Ende Switch (0x03 & 0x04)

                if (result == ku8MBSuccess)
                {
                    switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT5, channel)))
                    {
                        case 1: // High Byte
                            v = (getResponseBuffer(0) >> 8);
                            break;
                        case 2: // Low Byte
                            v = getResponseBuffer(0);
                            break;
                        case 3: // frei Wählbar
                            v = (getResponseBuffer(0) >> (knx.paramByte(getPar(MOD_CHModBusOffsetRight5, channel))));
                            v = v & (knx.paramByte(getPar(MOD_CHModbusCountBitsDPT56, channel)));
                            // SERIAL_DEBUG.print(v, BIN);
                            // SERIAL_DEBUG.print(" ");
                            break;
                        default:
                            return false;
                    } // Ende Register Pos

                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && (uint32_t)abs(v - lastSentValue[channel].lValueUint8_t) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_5001));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueUint8_t = v;
                    }

                    // SERIAL_DEBUG.println(v);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler in der Übtragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);
                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 5.010 ************************************************************************************
        //*****************************************************************************************************************************************
        case 5:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT5 |");

                // clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                uint8_t v;

                // Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                {
                    case 3: // 0x03 Lese holding registers
                        // SERIAL_DEBUG.print(" 0x03 ");
                        result = readHoldingRegisters(registerAddr, 1);
                        break; // Ende 0x03

                    case 4:
                        // SERIAL_DEBUG.print(" 0x04 ");
                        result = readInputRegisters(registerAddr, 1);
                        break;
                    default: // Error Switch (0x03 & 0x04)
                        return false;
                } // Ende Switch (0x03 & 0x04)

                if (result == ku8MBSuccess)
                {
                    switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT5, channel)))
                    {
                        case 1: // High Byte
                            v = (getResponseBuffer(0) >> 8);
                            break;
                        case 2: // Low Byte
                            v = getResponseBuffer(0);
                            break;
                        case 3: // frei Wählbar
                            v = (getResponseBuffer(0) >> (knx.paramByte(getPar(MOD_CHModBusOffsetRight5, channel))));
                            v = v & (knx.paramByte(getPar(MOD_CHModbusCountBitsDPT56, channel)));
                            // SERIAL_DEBUG.print(v, BIN);
                            // SERIAL_DEBUG.print(" ");
                            break;
                        default:
                            return false;
                    } // Ende Register Pos

                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && (uint32_t)abs(v - lastSentValue[channel].lValueUint8_t) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_5));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueUint8_t = v;
                    }

                    // SERIAL_DEBUG.println(v);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler in der Übtragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);

                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 7 ***************************************************************************************
        //*****************************************************************************************************************************************
        case 7:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT7 |");
                //  clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                uint16_t v;

                // Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                {
                    case 3: // 0x03 Lese holding registers
                        // SERIAL_DEBUG.print(" 0x03 ");
                        result = readHoldingRegisters(registerAddr, 1);
                        break; // Ende 0x03

                    case 4:
                        // SERIAL_DEBUG.print(" 0x04 ");
                        result = readInputRegisters(registerAddr, 1);
                        break;
                    default: // Error Switch (0x03 & 0x04)
                        return false;
                } // Ende Switch (0x03 & 0x04)

                if (result == ku8MBSuccess)
                {

                    switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT7, channel)))
                    {
                        case 1: // High/LOW Byte
                            v = getResponseBuffer(0);
                            break;
                        case 2: // frei Wählbar
                            v = (getResponseBuffer(0) >> (knx.paramByte(getPar(MOD_CHModBusOffsetRight7, channel))));
                            v = v & ((1 << knx.paramByte(getPar(MOD_CHModbusCountBitsDPT7, channel))) - 1);
                            break;
                        default:
                            return false;
                    } // Ende Register Pos

                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && (uint32_t)abs(v - lastSentValue[channel].lValueUint16_t) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_7));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueUint16_t = v;
                    }

                    // SERIAL_DEBUG.println(v);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler in der Übtragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);
                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 8 signed 16Bit ***************************************************************************
        //*****************************************************************************************************************************************
        case 8:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT8 |");
                //  clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                int16_t v;

                // Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                {
                    case 3: // 0x03 Lese holding registers
                        // SERIAL_DEBUG.print(" 0x03 ");
                        result = readHoldingRegisters(registerAddr, 1);
                        break; // Ende 0x03

                    case 4:
                        // SERIAL_DEBUG.print(" 0x04 ");
                        result = readInputRegisters(registerAddr, 1);
                        break;
                    default: // Error Switch (0x03 & 0x04)
                        return false;
                } // Ende Switch (0x03 & 0x04)

                if (result == ku8MBSuccess)
                {

                    v = (int16_t)getResponseBuffer(0);

                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && abs(v - lastSentValue[channel].lValueInt16_t) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_8));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueInt16_t = v;
                    }

                    // Serial Output
                    // SERIAL_DEBUG.println(v);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler in der Übtragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);
                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 9 ***************************************************************************************
        //*****************************************************************************************************************************************
        case 9:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT9 |");

                // clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                float v;

                // Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                {
                    case 3: // 0x03 Lese holding registers
                        // SERIAL_DEBUG.print(" 0x03 ");
                        result = readHoldingRegisters(registerAddr, 1);
                        break; // Ende 0x03

                    case 4:
                        // SERIAL_DEBUG.print(" 0x04 ");
                        result = readInputRegisters(registerAddr, 1);
                        break;
                    default: // Error Switch (0x03 & 0x04)
                        return false;
                } // Ende Switch (0x03 & 0x04)

                if (result == ku8MBSuccess)
                {
                    if (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT9, channel)) <= 3)
                    {
                        uint16_t uraw;
                        // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                        switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT9, channel)))
                        {
                            case 1: // Low Byte unsigned
                                uraw = (uint8_t)(getResponseBuffer(0) & 0xff);
                                break;
                            case 2: // High Byte unsigned
                                uraw = (uint8_t)((getResponseBuffer(0) >> 8) & 0xff);
                                break;
                            case 3: // High/Low Byte unsigned
                                uraw = getResponseBuffer(0);
                                break;
                            default:
                                return false;
                        }

                        // SERIAL_DEBUG.print(uraw);
                        // SERIAL_DEBUG.print(" ");
                        //   ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                        v = uraw / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                        v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                    }
                    else
                    {
                        int16_t sraw;
                        // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                        switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT9, channel)))
                        {
                            case 4:                                           // Low Byte signed
                                sraw = (int8_t)(getResponseBuffer(0) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                break;
                            case 5:                                                  // High Byte signed
                                sraw = (int8_t)((getResponseBuffer(0) >> 8) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                break;
                            case 6:                                   // High/Low Byte signed
                                sraw = (int16_t)getResponseBuffer(0); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                break;
                            default:
                                return false;
                        }
                        // SERIAL_DEBUG.print(sraw);
                        // SERIAL_DEBUG.print(" ");
                        //   ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                        v = sraw / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                        v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                    }

                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && abs(v - lastSentValue[channel].lValue) * 10.0 >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_9)); //  ************************ MUSS NOCH GEPRÜFT WERDEN Float mit 2 Bytes !!!!!!!!!!!!!!!!!!!!!!!!!!!
                    if (lSend)
                    {
                        lastSentValue[channel].lValue = v;
                    }

                    // Serial Output
                    // SERIAL_DEBUG.println(v, 2);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;
                }
                else // Fehler in der Übtragung
                {
                    // SERIAL_DEBUG.print("ERROR: ");
                    // SERIAL_DEBUG.println(result, HEX);
                    ErrorHandling(channel);

                    return false;
                }
            }
            break;
        //*****************************************************************************************************************************************
        //*****************************************  DPT 12 ***************************************************************************************
        //*****************************************************************************************************************************************
        case 12:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT12 ");

                // clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                uint32_t v;

                // Bestimmt ob Register-Typ: Word oder Double Word
                switch (knx.paramByte(getPar(MOD_CHModBusWordTyp12, channel))) // Choose Word Register OR Double Word Register
                {
                    case 0: // Word Register
                        // SERIAL_DEBUG.print("| Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 1);
                                break;

                            case 4:

                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 1);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                            switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT12, channel)))
                            {
                                case 1:                                         // Low Byte signed
                                    v = (uint8_t)(getResponseBuffer(0) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                case 2:                                                // High Byte signed
                                    v = (uint8_t)((getResponseBuffer(0) >> 8) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                case 3:                                 // High/Low Byte signed
                                    v = (uint16_t)getResponseBuffer(0); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                default:
                                    return false;
                            }

                            // Löscht Fehlerspeicher
                            errorState[channel][0] = false;
                            errorState[channel][1] = false;
                        }
                        else // Fehler
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);
                            ErrorHandling(channel);

                            return false;
                        }

                        break;
                    case 1: // Double Word Register
                        // SERIAL_DEBUG.print("| Double Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 2);
                                break;

                            case 4:
                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 2);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            // check HI / LO   OR   LO / Hi  order
                            switch (knx.paramByte(getPar(MOD_CHModBusWordPosDpt12, channel)))
                            {
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                case 0: // HI Word / LO Word
                                    v = (int32_t)(getResponseBuffer(0) << 16 | getResponseBuffer(1));
                                    break;
                                case 1: // LO Word / HI Word
                                    v = (int32_t)(getResponseBuffer(1) << 16 | getResponseBuffer(0));
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                default:
                                    return false;
                            } // Ende // HI / LO Word
                        }
                        else // Fehler
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);

                            ErrorHandling(channel);

                            return false;
                        }
                        break; // Ende Case 1 Double Register
                    default:
                        return false;
                } // ENDE ENDE Word / Double Word Register

                if (result == ku8MBSuccess)
                {
                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    int32_t lDiff = v - lastSentValue[channel].lValueUint32_t;
                    lSend |= (lAbsolute && (uint32_t)abs(lDiff) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_13));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueUint32_t = v;
                    }

                    // SERIAL_DEBUG.println(v, 2);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;

                    // virtueller Zähler
                    processMeter(channel, v);
                }

            } // ENDE
            break; // Ende PDT12
        //*****************************************************************************************************************************************
        //*****************************************  DPT 13 ***************************************************************************************
        //*****************************************************************************************************************************************
        case 13:
            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT13 ");

                // clear Responsebuffer before revicing a new message
                clearResponseBuffer();

                int32_t v;

                // Bestimmt ob Register-Typ: Word oder Double Word
                switch (knx.paramByte(getPar(MOD_CHModBusWordTyp13, channel))) // Choose Word Register OR Double Word Register
                {
                    case 0: // Word Register
                        // SERIAL_DEBUG.print("| Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 1);
                                break;

                            case 4:

                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 1);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                            switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT13, channel)))
                            {
                                case 1:                                        // Low Byte signed
                                    v = (int8_t)(getResponseBuffer(0) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                case 2:                                               // High Byte signed
                                    v = (int8_t)((getResponseBuffer(0) >> 8) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                case 3:                                // High/Low Byte signed
                                    v = (int16_t)getResponseBuffer(0); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                default:
                                    return false;
                            }
                        }
                        else
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);
                            ErrorHandling(channel);

                            return false;
                        }

                        break;
                    case 1: // Double Word Register
                        // SERIAL_DEBUG.print("| Double Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 2);
                                break;

                            case 4:
                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 2);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            // check HI / LO   OR   LO / Hi  order
                            switch (knx.paramByte(getPar(MOD_CHModBusWordPosDpt13, channel)))
                            {
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                case 0: // HI Word / LO Word
                                    v = (int32_t)(getResponseBuffer(0) << 16 | getResponseBuffer(1));
                                    break;
                                case 1: // LO Word / HI Word
                                    v = (int32_t)(getResponseBuffer(1) << 16 | getResponseBuffer(0));
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                default:
                                    return false;
                            } // Ende // HI / LO Word
                        }
                        else
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);

                            ErrorHandling(channel);

                            return false;
                        }
                        break; // Ende Case 1 Double Register
                    default:
                        return false;
                } // ENDE ENDE Word / Double Word Register

                if (result == ku8MBSuccess)
                {
                    // senden bei Wertänderung
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && (uint32_t)abs(v - lastSentValue[channel].lValueInt32_t) >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_13));
                    if (lSend)
                    {
                        lastSentValue[channel].lValueInt32_t = v;
                    }

                    // SERIAL_DEBUG.println(v, 2);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;

                    // virtueller Zähler
                    processMeter(channel, v);
                }

            } // ENDE
            break; // Ende PDT13

        //*****************************************************************************************************************************************
        //*****************************************  DPT 14 ***************************************************************************************
        //*****************************************************************************************************************************************
        case 14:

            if (readRequest)
            {
                // SERIAL_DEBUG.print("DPT14 ");

                // clear Responsebuffer before receiving a new message
                clearResponseBuffer();

                float v;

                // Bestimmt ob Register-Typ: Word oder Double Word
                switch (knx.paramByte(getPar(MOD_CHModBusWordTyp14, channel))) // Choose Word Register OR Double Word Register
                {
                    case 0: // Word Register
                        // SERIAL_DEBUG.print("| Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 1);
                                break;

                            case 4:

                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 1);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            if (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT14, channel)) <= 3)
                            {
                                uint16_t uraw;
                                // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                                switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT14, channel)))
                                {
                                    case 1: // Low Byte unsigned
                                        uraw = (uint8_t)(getResponseBuffer(0) & 0xff);
                                        break;
                                    case 2: // High Byte unsigned
                                        uraw = (uint8_t)((getResponseBuffer(0) >> 8) & 0xff);
                                        break;
                                    case 3: // High/Low Byte unsigned
                                        uraw = getResponseBuffer(0);
                                        break;
                                    default:
                                        return false;
                                }
                                // SERIAL_DEBUG.print(uraw);
                                // SERIAL_DEBUG.print(" ");
                                //   ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                v = uraw / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                                v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                            }
                            else
                            {
                                int16_t sraw;
                                // adapt input value (Low Byte / High Byte / High&Low Byte / .... )
                                switch (knx.paramByte(getPar(MOD_CHModBusRegisterPosDPT14, channel)))
                                {
                                    case 4:                                           // Low Byte signed
                                        sraw = (int8_t)(getResponseBuffer(0) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                        break;
                                    case 5:                                                  // High Byte signed
                                        sraw = (int8_t)((getResponseBuffer(0) >> 8) & 0xff); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                        break;
                                    case 6:                                   // High/Low Byte signed
                                        sraw = (int16_t)getResponseBuffer(0); // muss noch bearbeitet werden !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                                        break;
                                    default:
                                        return false;
                                }
                                // SERIAL_DEBUG.print(sraw);
                                //   ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                v = sraw / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                                v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                            }
                        }
                        else
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);
                            ErrorHandling(channel);

                            return false;
                        }

                        break;
                    case 1: // Double Word Register
                        // SERIAL_DEBUG.print("| Double Word ");
                        //  Choose Modbus Funktion (0x03 readHoldingRegisters ODER 0x04 readInputRegisters)
                        switch (knx.paramByte(getPar(MOD_CHModBusReadWordFunktion, channel)))
                        {
                            case 3: // 0x03 Lese holding registers
                                // SERIAL_DEBUG.print(" 0x03 ");
                                result = readHoldingRegisters(registerAddr, 2);
                                break;

                            case 4:
                                // SERIAL_DEBUG.print(" 0x04 ");
                                result = readInputRegisters(registerAddr, 2);
                                break;
                            default:
                                return false;
                        }

                        if (result == ku8MBSuccess)
                        {
                            uint32_t raw;

                            // check HI / LO   OR   LO / Hi  order
                            switch (knx.paramByte(getPar(MOD_CHModBusWordPosDpt14, channel)))
                            {
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                case 0: // HI Word / LO Word
                                    raw = getResponseBuffer(0) << 16 | getResponseBuffer(1);
                                    break;
                                case 1: // LO Word / HI Word
                                    raw = getResponseBuffer(0) | getResponseBuffer(1) << 16;
                                    //  ************************************************************************** MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    break;
                                default:
                                    return false;
                            } // Ende // HI / LO Word
                            // check receive input datatype ( signed / unsgined / Float)
                            switch (knx.paramByte(getPar(MOD_CHModBusRegisterValueTypDpt14, channel)))
                            {
                                case 1: // unsigned
                                {
                                    uint32_t lValueu32bit = raw;
                                    //                                                         ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    v = lValueu32bit / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                                    v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                                }
                                break;
                                case 2: // signed
                                {
                                    int32_t lValuei32bit = (int32_t)raw;
                                    //                                                         ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    v = lValuei32bit / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                                    v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                                }
                                break;
                                case 3: // float
                                {
                                    // going via union allows the compiler to be sure about alignment
                                    union intfloat
                                    {
                                        uint32_t intVal;
                                        float floatVal;
                                    };
                                    float lValueFloat = ((intfloat *)&raw)->floatVal;
                                    //                                                         ************************ MUSS NOCH GEPRÜFT WERDEN !!!!!!!!!!!!!!!!!!!!!!!!!!!
                                    v = lValueFloat / (float)knx.paramInt(getPar(MOD_CHModBuscalculationValueDiff, channel));
                                    v = v + (int16_t)knx.paramInt(getPar(MOD_CHModBuscalculationValueAdd, channel));
                                }
                                break;
                                default:
                                    return false;
                            }
                        }
                        else
                        {
                            // SERIAL_DEBUG.print("ERROR: ");
                            // SERIAL_DEBUG.println(result, HEX);

                            ErrorHandling(channel);

                            return false;
                        }
                        break; // Ende Case 1 Double Register
                    default:
                        return false;
                } // ENDE ENDE Word / Double Word Register

                if (result == ku8MBSuccess)
                {
                    // send on first value or value change
                    uint32_t lAbsolute = knx.paramInt(getPar(MOD_CHModBusValueChange, channel));
                    lSend |= (lAbsolute && abs(v - lastSentValue[channel].lValue) * 10.0 >= lAbsolute);

                    // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                    knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).valueNoSend(v, getDPT(VAL_DPT_14));
                    if (lSend)
                    {
                        lastSentValue[channel].lValue = v;
                    }

                    // SERIAL_DEBUG.println(v, 2);

                    // Löscht Fehlerspeicher
                    errorState[channel][0] = false;
                    errorState[channel][1] = false;

                    // virtueller Zähler
                    processMeter(channel, v);
                }

            } // ENDE
            break; // Ende PDT14

        default: // all other dpts
            break;
    } // Ende DPT Wahl Wahl

    if (lSend && !errorState[channel][0] && !errorState[channel][1])
    {
        knx.getGroupObject(getCom(MOD_KoGO_BASE_, channel)).objectWritten();
        valueValid[channel] = true;
        sendDelay[channel] = millis();
    }

    return true;
}

// number = 1 - 4 !!!!
void Modbus::handleMeters(uint8_t number)
{
    int lAbsolute = 0;
    // senden bei Wertänderung
    switch (number)
    {
        case 1:
            lAbsolute = knx.paramInt(MOD_ModBusZaehler1ValueChangeWatt);
            break;
        case 2:
            // lAbsolute = knx.paramInt(MOD_ModBusZaehler2ValueChangeWatt);
            break;
        case 3:
            // lAbsolute = knx.paramInt(MOD_ModBusZaehler3ValueChangeWatt);
            break;
        case 4:
            // lAbsolute = knx.paramInt(MOD_ModBusZaehler4ValueChangeWatt);
            break;

        default:
            break;
    }

    if (lAbsolute > 0.0f && roundf(abs(powerZ[number - 1] - lastSentPowerZ[number - 1])) >= lAbsolute)
    {
        knx.getGroupObject(MOD_KoPower1_base + number - 1).value(powerZ[number - 1], getDPT(VAL_DPT_14));
        lastSentPowerZ[number - 1] = powerZ[number - 1];
    }
}
