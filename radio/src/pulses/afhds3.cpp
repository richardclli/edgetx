/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "afhds3.h"
#include "afhds3_transport.h"
#include "afhds3_config.h"

#include "pulses.h"

#include "../debug.h"
#include "../definitions.h"

#include "telemetry/telemetry.h"
#include "mixer_scheduler.h"
#include "hal/module_driver.h"
#include "hal/module_port.h"

#define FAILSAFE_HOLD 1
#define FAILSAFE_CUSTOM 2

#define FAILSAFE_HOLD_VALUE         0x8000
#define FAILSAFE_NOPULSES_VALUE     0x8001
//get channel value outside of afhds3 namespace
int32_t getChannelValue(uint8_t channel);

void processFlySkySensor(const uint8_t * packet, uint8_t type);

namespace afhds3
{

static uint8_t _phyMode_channels[] = {
  18, // CLASSIC_FLCR1_18CH
  10, // CLASSIC_FLCR6_10CH
  18, // ROUTINE_FLCR1_18CH
  8,  // ROUTINE_FLCR6_8CH
  12, // ROUTINE_LORA_12CH
};

// enum COMMAND_DIRECTION
// {
//   RADIO_TO_MODULE = 0,
//   MODULE_TO_RADIO = 1
// };

// enum DATA_TYPE
// {
//   READY_DT,  // 8 bytes 0x01 Not ready 0x02 Ready
//   STATE_DT,  // See MODULE_STATE
//   MODE_DT,
//   MOD_CONFIG_DT,
//   CHANNELS_DT,
//   TELEMETRY_DT,
//   MODULE_POWER_DT,
//   MODULE_VERSION_DT,
//   EMPTY_DT,
// };

// enum used by command response -> translate to ModuleState
enum MODULE_READY_E {
  MODULE_STATUS_UNKNOWN = 0x00,
  MODULE_STATUS_NOT_READY = 0x01,
  MODULE_STATUS_READY = 0x02
};

enum ModuleState {
  STATE_NOT_READY = 0x00,  // virtual
  STATE_HW_ERROR = 0x01,
  STATE_BINDING = 0x02,
  STATE_SYNC_RUNNING = 0x03,
  STATE_SYNC_DONE = 0x04,
  STATE_STANDBY = 0x05,
  STATE_UPDATING_WAIT = 0x06,
  STATE_UPDATING_MOD = 0x07,
  STATE_UPDATING_RX = 0x08,
  STATE_UPDATING_RX_FAILED = 0x09,
  STATE_RF_TESTING = 0x0a,
  STATE_READY = 0x0b,  // virtual
  STATE_HW_TEST = 0xff,
};

// used for set command
enum MODULE_MODE_E {
  STANDBY = 0x01,
  BIND = 0x02,  // after bind module will enter run mode
  RUN = 0x03,
  RX_UPDATE = 0x04,  // after successful update module will enter standby mode,
                     // otherwise hw error will be raised
  MODULE_MODE_UNKNOWN = 0xFF
};

enum CMD_RESULT {
  FAILURE = 0x01,
  SUCCESS = 0x02,
};

enum CHANNELS_DATA_MODE {
  CHANNELS = 0x01,
  FAIL_SAFE = 0x02,
};

PACK(struct ChannelsData {
  uint8_t mode;
  uint8_t channelsNumber;
  int16_t data[AFHDS3_MAX_CHANNELS];
});

union ChannelsData_u {
  ChannelsData data;
  uint8_t buffer[sizeof(ChannelsData)];
};

PACK(struct TelemetryData {
  uint8_t sensorType;
  uint8_t length;
  uint8_t type;
  uint8_t semsorID;
  uint8_t data[8];
});

enum MODULE_POWER_SOURCE {
  INTERNAL = 0x01,
  EXTERNAL = 0x02,
};

enum DeviceAddress {
  TRANSMITTER = 0x01,
  FRM303 = 0x04,
  IRM301 = 0x05,
};

PACK(struct ModuleVersion {
  uint32_t productNumber;
  uint32_t hardwereVersion;
  uint32_t bootloaderVersion;
  uint32_t firmwareVersion;
  uint32_t rfVersion;
});

PACK(struct CommandResult_s {
  uint16_t command;
  uint8_t result;
  uint8_t respLen;
});

union AfhdsFrameData {
  uint8_t value;
  // Config_s Config;
  ChannelsData Channels;
  TelemetryData Telemetry;
  ModuleVersion Version;
  CommandResult_s CommandResult;
};

#define FRM302_STATUS 0x56

class ProtoState
{
  public:
    /**
    * Initialize class for operation
    * @param moduleIndex index of module one of INTERNAL_MODULE, EXTERNAL_MODULE
    * @param resetFrameCount flag if current frame count should be reseted
    */
   void init(uint8_t moduleIndex, void* buffer, etx_module_state_t* mod_st,
             uint8_t fAddr);

   /**
    * Fills DMA buffers with frame to be send depending on actual state
    */
   void setupFrame();

   /**
    * Sends prepared buffers
    */
   void sendFrame() { trsp.sendBuffer(); }

   /**
    * Gets actual module status into provided buffer
    * @param statusText target buffer for status
    */
   void getStatusString(char* statusText) const;

   /**
    * Sends stop command to prevent any further module operations
    */
   void stop();

   Config_u* getConfig() { return &cfg; }

   void applyConfigFromModel();

  protected:

    void resetConfig(uint8_t version);

  private:
    //friendship declaration - use for passing telemetry
    friend void processTelemetryData(void* ctx, uint8_t data, uint8_t* buffer, uint8_t* len);

    void processTelemetryData(uint8_t data, uint8_t* buffer, uint8_t* len);

    void parseData(uint8_t* rxBuffer, uint8_t rxBufferCount);

    void setState(ModuleState state);

    bool syncSettings();

    void requestInfoAndRun(bool send = false);

    uint8_t setFailSafe(int16_t* target, uint8_t rfchannelcount=AFHDS3_MAX_CHANNELS);

    inline int16_t convert(int channelValue);

    void sendChannelsData();

    void clearFrameData();

    bool isConnected();
    bool hasTelemetry();

    Transport trsp;

    /**
     * Index of the module
     */
    uint8_t module_index;

    /**
     * Reported state of the HF module
     */
    ModuleState state;

    bool modelIDSet;
    bool modelcfgGet;
    uint8_t modelID;
    /**
     * Command count used for counting actual number of commands sent in run mode
     */
    uint32_t cmdCount;

    /**
     * Command index of command to be send when cmdCount reached necessary value
     */
    uint32_t cmdIndex;

    /**
     * Pointer to module config - it is making operations easier and faster
     */
    ModuleData* moduleData;

    /**
     * Actual module configuration - must be requested from module
     */
    Config_u cfg;

    /**
     * Actual module version - must be requested from module
     */
    ModuleVersion version;
};

static const char* const moduleStateText[] =
{
  "Not ready",
  "HW Error",
  "Binding",
  "Disconnected",
  "Connected",
  "Standby",
  "Waiting for update",
  "Updating",
  "Updating RX",
  "Updating RX failed",
  "Testing",
  "Ready",
  "HW test"
};

static const COMMAND periodicRequestCommands[] =
{
  COMMAND::MODULE_STATE,
  // COMMAND::MODULE_GET_CONFIG,
  COMMAND::VIRTUAL_FAILSAFE
};

//Static collection of afhds3 object instances by module
static ProtoState protoState[MAX_MODULES];

void getStatusString(uint8_t module, char* buffer)
{
  return protoState[module].getStatusString(buffer);
}

//friends function that can access telemetry parsing method
void processTelemetryData(void* ctx, uint8_t data, uint8_t* buffer, uint8_t* len)
{
  auto mod_st = (etx_module_state_t*)ctx;
  auto p_state = (ProtoState*)mod_st->user_data;
  p_state->processTelemetryData(data, buffer, len);
}

void ProtoState::getStatusString(char* buffer) const
{
  strcpy(buffer, state <= ModuleState::STATE_READY ? moduleStateText[state]
                                                   : "Unknown");
}

void ProtoState::processTelemetryData(uint8_t byte, uint8_t* buffer, uint8_t* len)
{
  uint8_t maxSize = TELEMETRY_RX_PACKET_SIZE;
  if (!trsp.processTelemetryData(byte, buffer, *len, maxSize))
    return;

  parseData(buffer, *len);
  *len = 0;
}

bool ProtoState::isConnected()
{
  return this->state == ModuleState::STATE_SYNC_DONE;
}

bool ProtoState::hasTelemetry()
{
  if (cfg.version == 0)
    return cfg.v0.IsTwoWay;
  else
    return cfg.v1.IsTwoWay;
}

void ProtoState::setupFrame()
{
  bool trsp_error = false;
  if (trsp.handleRetransmissions(trsp_error)) return;

  if (trsp_error) {
    this->state = ModuleState::STATE_NOT_READY;
    clearFrameData();
  }

  if (this->state == ModuleState::STATE_NOT_READY) {
    TRACE("AFHDS3 [GET MODULE READY]");
    trsp.sendFrame(COMMAND::MODULE_READY, FRAME_TYPE::REQUEST_GET_DATA);
    return;
  }

  // process backlog
  if (trsp.processQueue()) return;

  // config should be loaded already
  if (syncSettings()) { return; }

  ::ModuleSettingsMode moduleMode = getModuleMode(module_index);

  if (moduleMode == ::ModuleSettingsMode::MODULE_MODE_BIND) {
    if (state != STATE_BINDING) {
      TRACE("AFHDS3 [BIND]");
      applyConfigFromModel();

      trsp.sendFrame(COMMAND::MODULE_SET_CONFIG,
                     FRAME_TYPE::REQUEST_SET_EXPECT_DATA, cfg.buffer,
                     cfg.version == 0 ? sizeof(cfg.v0) : sizeof(cfg.v1));

      trsp.enqueue(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, true,
                   (uint8_t)MODULE_MODE_E::BIND);
      return;
    }
  }
  else if (moduleMode == ::ModuleSettingsMode::MODULE_MODE_RANGECHECK) {
    TRACE("AFHDS3 [RANGE CHECK] not supported");
  }
  else if (moduleMode == ::ModuleSettingsMode::MODULE_MODE_NORMAL) {

    // if module is ready but not started
    if (this->state == ModuleState::STATE_READY) {
      trsp.sendFrame(MODULE_STATE, FRAME_TYPE::REQUEST_GET_DATA);
      return;
    }

    if (!modelIDSet) {
      if (this->state != ModuleState::STATE_STANDBY) {
        auto mode = (uint8_t)MODULE_MODE_E::STANDBY;
        trsp.sendFrame(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, &mode, 1);
        return;
      } else {
        modelIDSet = true;
        modelID = g_model.header.modelId[module_index];
        trsp.sendFrame(COMMAND::MODEL_ID, FRAME_TYPE::REQUEST_SET_EXPECT_DATA,
                       &g_model.header.modelId[module_index], 1);
        return;
      }
    } else if (modelID != g_model.header.modelId[module_index]) {
      modelIDSet = false;
      auto mode = (uint8_t)MODULE_MODE_E::STANDBY;
      trsp.sendFrame(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, &mode, 1);
      return;
    }

    if (this->state == ModuleState::STATE_STANDBY) {
      cmdCount = 0;
      requestInfoAndRun(true);
      return;
    }

    // exit bind
    if (this->state == STATE_BINDING) {
      TRACE("AFHDS3 [EXIT BIND]");
      modelcfgGet = true;
      auto mode = (uint8_t)MODULE_MODE_E::RUN;
      trsp.sendFrame(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, &mode, 1);
      return;
    }
  }

  if (modelcfgGet){
    trsp.enqueue(COMMAND::MODULE_GET_CONFIG, FRAME_TYPE::REQUEST_GET_DATA);
    return;
  }

  if (cmdCount++ >= 150) {

    cmdCount = 0;
    if (cmdIndex >= sizeof(periodicRequestCommands)) {
      cmdIndex = 0;
    }
    COMMAND cmd = periodicRequestCommands[cmdIndex++];

    if (cmd == COMMAND::VIRTUAL_FAILSAFE) {
      Config_u* cfg = this->getConfig();
      uint8_t len =_phyMode_channels[cfg->v0.PhyMode];
      if (!hasTelemetry()) {
          uint16_t failSafe[AFHDS3_MAX_CHANNELS + 1] = {
          ((AFHDS3_MAX_CHANNELS << 8) | CHANNELS_DATA_MODE::FAIL_SAFE), 0};
          setFailSafe((int16_t*)(&failSafe[1]), len);
          TRACE("AFHDS ONE WAY FAILSAFE");
          trsp.sendFrame(COMMAND::CHANNELS_FAILSAFE_DATA,
                   FRAME_TYPE::REQUEST_SET_NO_RESP, (uint8_t*)failSafe,
                   AFHDS3_MAX_CHANNELS * 2 + 2);
          return;
      }
      else if( isConnected() ){
          uint8_t data[AFHDS3_MAX_CHANNELS*2 + 3] = { (uint8_t)(RX_CMD_FAILSAFE_VALUE&0xFF), (uint8_t)((RX_CMD_FAILSAFE_VALUE>>8)&0xFF), (uint8_t)(2*len)};
          int16_t failSafe[18];
          setFailSafe(&failSafe[0], len);
          std::memcpy( &data[3], failSafe, 2*len );
          trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data, 2*len+3);
          return;
      }
    } else {
      trsp.sendFrame(cmd, FRAME_TYPE::REQUEST_GET_DATA);
      return;
    }
  }

  if (isConnected()) {
    sendChannelsData();
  } else {
    //default frame - request state
    trsp.sendFrame(MODULE_STATE, FRAME_TYPE::REQUEST_GET_DATA);
  }
}

void ProtoState::init(uint8_t moduleIndex, void* buffer,
                      etx_module_state_t* mod_st, uint8_t fAddr)
{
  module_index = moduleIndex;
  trsp.init(buffer, mod_st, fAddr);

  //clear local vars because it is member of union
  moduleData = &g_model.moduleData[module_index];
  state = ModuleState::STATE_NOT_READY;
  modelIDSet = false;
  clearFrameData();
}

void ProtoState::clearFrameData()
{
  TRACE("AFHDS3 clearFrameData");
  trsp.clear();

  cmdCount = 0;
  cmdIndex = 0;
}

bool containsData(enum FRAME_TYPE frameType)
{
  return (frameType == FRAME_TYPE::RESPONSE_DATA ||
      frameType == FRAME_TYPE::REQUEST_SET_EXPECT_DATA ||
      frameType == FRAME_TYPE::REQUEST_SET_EXPECT_ACK ||
      frameType == FRAME_TYPE::REQUEST_SET_EXPECT_DATA ||
      frameType == FRAME_TYPE::REQUEST_SET_NO_RESP);
}

void ProtoState::setState(ModuleState state)
{
  if (state == this->state) {
    return;
  }
  uint8_t oldState = this->state;
  this->state = state;
  if (oldState == ModuleState::STATE_BINDING) {
    setModuleMode(module_index, ::ModuleSettingsMode::MODULE_MODE_NORMAL);
  }
  if (state == ModuleState::STATE_NOT_READY) {
    trsp.clear();
  }
}

void ProtoState::requestInfoAndRun(bool send)
{
  // set model ID
  // trsp.enqueue(COMMAND::MODEL_ID, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, true,
  //              g_model.header.modelId[module_index]);

  // RUN
  trsp.enqueue(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, true,
               (uint8_t)MODULE_MODE_E::RUN);

  if (send) { trsp.processQueue(); }
}

void ProtoState::parseData(uint8_t* rxBuffer, uint8_t rxBufferCount)
{
  AfhdsFrame* responseFrame = reinterpret_cast<AfhdsFrame*>(rxBuffer);
  if (containsData((enum FRAME_TYPE) responseFrame->frameType)) {
    switch (responseFrame->command) {
      case COMMAND::MODULE_READY:
        TRACE("AFHDS3 [MODULE_READY] %02X", responseFrame->value);
        if (responseFrame->value == MODULE_STATUS_READY) {
          setState(ModuleState::STATE_READY);
          // requestInfoAndRun();
        }
        else {
          setState(ModuleState::STATE_NOT_READY);
        }
        break;
      case COMMAND::MODULE_GET_CONFIG: {
        modelcfgGet = false;
        size_t len = min<size_t>(sizeof(cfg.buffer), rxBufferCount);
        std::memcpy((void*) cfg.buffer, &responseFrame->value, len);
        moduleData->afhds3.emi = cfg.v0.EMIStandard;
        moduleData->afhds3.telemetry = cfg.v0.IsTwoWay;
        moduleData->afhds3.phyMode = cfg.v0.PhyMode;
      } break;
      case COMMAND::MODULE_VERSION:
        std::memcpy((void*) &version, &responseFrame->value, sizeof(version));
        TRACE("AFHDS3 [MODULE_VERSION] Product %d, HW %d, BOOT %d, FW %d",
              version.productNumber, version.hardwereVersion,
              version.bootloaderVersion, version.firmwareVersion);
        break;
      case COMMAND::MODULE_STATE:
        TRACE("AFHDS3 [MODULE_STATE] %02X", responseFrame->value);
        setState((ModuleState)responseFrame->value);
        break;
      case COMMAND::MODULE_MODE:
        TRACE("AFHDS3 [MODULE_MODE] %02X", responseFrame->value);
        if (responseFrame->value != CMD_RESULT::SUCCESS) {
          setState(ModuleState::STATE_NOT_READY);
        }
        break;
      case COMMAND::MODULE_SET_CONFIG:
        if (responseFrame->value != CMD_RESULT::SUCCESS) {
          setState(ModuleState::STATE_NOT_READY);
        }
        TRACE("AFHDS3 [MODULE_SET_CONFIG], %02X", responseFrame->value);
        break;
      case COMMAND::MODEL_ID:
        if (responseFrame->value == CMD_RESULT::SUCCESS) {
          modelcfgGet = true;
        }
        break;
      case COMMAND::TELEMETRY_DATA:
        {
        uint8_t* telemetry = &responseFrame->value;

        if (telemetry[0] == 0x22) {
          telemetry++;
          while (telemetry < rxBuffer + rxBufferCount) {
            uint8_t length = telemetry[0];
            uint8_t id = telemetry[1];
            if (id == 0xFE) {
              id = 0xF7;  //use new id because format is different
            }
            if (length == 0 || telemetry + length > rxBuffer + rxBufferCount) {
              break;
            }
            if (length == 4) {
              //one byte value fill missing byte
              uint8_t data[] = { id, telemetry[2], telemetry[3], 0 };
              ::processFlySkySensor(data, 0xAA);
            }
            if (length == 5) {
              if (id == 0xFA) {
                telemetry[1] = 0xF8; //remap to afhds3 snr
              }
              ::processFlySkySensor(telemetry + 1, 0xAA);
            }
            else if (length == 6 && id == FRM302_STATUS) {
              //convert to ibus
              uint16_t t = (uint16_t) (((int16_t) telemetry[3] * 10) + 400);
              uint8_t dataTemp[] = { ++id, telemetry[2], (uint8_t) (t & 0xFF), (uint8_t) (t >> 8) };
              ::processFlySkySensor(dataTemp, 0xAA);
              uint8_t dataVoltage[] = { ++id, telemetry[2], telemetry[4], telemetry[5] };
              ::processFlySkySensor(dataVoltage, 0xAA);
            }
            else if (length == 7) {
              ::processFlySkySensor(telemetry + 1, 0xAC);
            }
            telemetry += length;
          }
        }
      }
        break;
      case COMMAND::COMMAND_RESULT: {
        // AfhdsFrameData* respData = responseFrame->GetData();
        // TRACE("COMMAND RESULT %02X result %d datalen %d",
        // respData->CommandResult.command, respData->CommandResult.result,
        // respData->CommandResult.respLen);
      } break;
    }
  }

  if (responseFrame->frameType == FRAME_TYPE::REQUEST_GET_DATA ||
      responseFrame->frameType == FRAME_TYPE::REQUEST_SET_EXPECT_DATA) {
    TRACE("Command %02X NOT IMPLEMENTED!", responseFrame->command);
  }
}

inline bool isSbus(uint8_t mode)
{
  return (mode & 1);
}

inline bool isPWM(uint8_t mode)
{
  return !(mode & 2);
}

bool ProtoState::syncSettings()
{
  // RUN_POWER targetPower = getRunPower();

  // /*not sure if we need to prevent them in bind mode*/
  // if (getModuleMode(module_index) != ::ModuleSettingsMode::MODULE_MODE_BIND &&
  //     targetPower != cfg.config.runPower) {
  //   cfg.config.runPower = moduleData->afhds3.runPower;
  //   uint8_t data[] = {0x13, 0x20, 0x02, moduleData->afhds3.runPower, 0};
  //   TRACE("AFHDS3 SET TX POWER %d", moduleData->afhds3.runPower);
  //   trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data,
  //            sizeof(data));
  //   return true;
  // }

  // // other settings only in 2 way mode (state must be synchronized)
  // if (this->state != ModuleState::STATE_SYNC_DONE) {
  //   return false;
  // }

  // if (moduleData->afhds3.rxFreq() != cfg.config.pwmFreq) {
  //   cfg.config.pwmFreq = moduleData->afhds3.rxFreq();
  //   uint8_t data[] = {0x17, 0x70, 0x02,
  //                     (uint8_t)(moduleData->afhds3.rxFreq() & 0xFF),
  //                     (uint8_t)(moduleData->afhds3.rxFreq() >> 8)};
  //   TRACE("AFHDS3 SET RX FREQ");
  //   trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data,
  //            sizeof(data));
  //   return true;
  // }

  // PULSE_MODE modelPulseMode = isPWM(moduleData->afhds3.mode)
  //                                 ? PULSE_MODE::PWM_MODE
  //                                 : PULSE_MODE::PPM_MODE;
  // if (modelPulseMode != cfg.config.pulseMode) {
  //   cfg.config.pulseMode = modelPulseMode;
  //   TRACE("AFHDS3 PWM/PPM %d", modelPulseMode);
  //   uint8_t data[] = {0x16, 0x70, 0x01, (uint8_t)(modelPulseMode)};
  //   trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data,
  //            sizeof(data));
  //   return true;
  // }

  // SERIAL_MODE modelSerialMode = isSbus(moduleData->afhds3.mode)
  //                                   ? SERIAL_MODE::SBUS_MODE
  //                                   : SERIAL_MODE::IBUS;
  // if (modelSerialMode != cfg.config.serialMode) {
  //   cfg.config.serialMode = modelSerialMode;
  //   TRACE("AFHDS3 IBUS/SBUS %d", modelSerialMode);
  //   uint8_t data[] = {0x18, 0x70, 0x01, (uint8_t)(modelSerialMode)};
  //   trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data,
  //            sizeof(data));
  //   return true;
  // }

  // if (moduleData->afhds3.failsafeTimeout != cfg.config.failSafeTimout) {
  //   moduleData->afhds3.failsafeTimeout = cfg.config.failSafeTimout;
  //   uint8_t data[] = {0x12, 0x60, 0x02,
  //                     (uint8_t)(moduleData->afhds3.failsafeTimeout & 0xFF),
  //                     (uint8_t)(moduleData->afhds3.failsafeTimeout >> 8)};
  //   trsp.sendFrame(COMMAND::SEND_COMMAND, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, data,
  //            sizeof(data));
  //   TRACE("AFHDS3 FAILSAFE TMEOUT, %d", moduleData->afhds3.failsafeTimeout);
  //   return true;
  // }

  return false;
}

void ProtoState::sendChannelsData()
{
  uint8_t channels_start = moduleData->channelsStart;
  uint8_t channelsCount = 8 + moduleData->channelsCount;
  uint8_t channels_last = channels_start + channelsCount;

  int16_t buffer[AFHDS3_MAX_CHANNELS + 1] = {0};

  uint8_t* header = (uint8_t*)buffer;
  header[0] = CHANNELS_DATA_MODE::CHANNELS;

  uint8_t channels = _phyMode_channels[cfg.v0.PhyMode];
  header[1] = channels;

  for (uint8_t channel = channels_start, index = 1; channel < channels_last;
       channel++, index++) {
    int16_t channelValue = convert(::getChannelValue(channel));
    buffer[index] = channelValue;
  }

  trsp.sendFrame(COMMAND::CHANNELS_FAILSAFE_DATA, FRAME_TYPE::REQUEST_SET_NO_RESP,
           (uint8_t*)buffer, (channels + 1) * 2);
}

void ProtoState::stop()
{
  TRACE("AFHDS3 STOP");
  auto mode = (uint8_t)MODULE_MODE_E::STANDBY;
  trsp.sendFrame(COMMAND::MODULE_MODE, FRAME_TYPE::REQUEST_SET_EXPECT_DATA, &mode, 1);
}

void ProtoState::resetConfig(uint8_t version)
{
  memclear(&cfg, sizeof(cfg));
  cfg.version = version;

  if (cfg.version == 1) {
    cfg.v1.SignalStrengthRCChannelNb = 0xFF;
    cfg.v1.FailsafeTimeout = 500;
    for (int i = 0; i < SES_NB_MAX_CHANNELS; i++)
      cfg.v1.PWMFrequenciesV1.PWMFrequencies[i] = 50;
  } else {
    cfg.v0.SignalStrengthRCChannelNb = 0xFF;
    cfg.v0.FailsafeTimeout = 500;
    cfg.v0.PWMFrequency.Frequency = 50;
  }
}

void ProtoState::applyConfigFromModel()
{
  uint8_t version = 0;
  if (moduleData->afhds3.phyMode >= ROUTINE_FLCR1_18CH) {
    version = 1;
  }

  if (version != cfg.version) {
    resetConfig(version);
  }

  if (cfg.version == 1) {
    cfg.v1.EMIStandard = moduleData->afhds3.emi;
    cfg.v1.IsTwoWay = moduleData->afhds3.telemetry;
    cfg.v1.PhyMode = moduleData->afhds3.phyMode;

    // Failsafe
    setFailSafe(cfg.v1.FailSafe);
    if (moduleData->failsafeMode != FAILSAFE_NOPULSES) {
      cfg.v1.FailsafeOutputMode = true;
    } else {
      cfg.v1.FailsafeOutputMode = false;
    }
  } else {
    cfg.v0.EMIStandard = moduleData->afhds3.emi;
    cfg.v0.IsTwoWay = moduleData->afhds3.telemetry;
    cfg.v0.PhyMode = moduleData->afhds3.phyMode;

    // Failsafe
    setFailSafe(cfg.v0.FailSafe);
    if (moduleData->failsafeMode != FAILSAFE_NOPULSES) {
      cfg.v0.FailsafeOutputMode = true;
    } else {
      cfg.v0.FailsafeOutputMode = false;
    }
  }
}

inline int16_t ProtoState::convert(int channelValue)
{
  return ::limit<int16_t>(AFHDS3_FAILSAFE_MIN, channelValue * 10, AFHDS3_FAILSAFE_MAX);
}

uint8_t ProtoState::setFailSafe(int16_t* target, uint8_t rfchannelsCount )
{
  int16_t pulseValue = 0;
  uint8_t channels_start = moduleData->channelsStart;
  uint8_t channelsCount = 8 + moduleData->channelsCount;
  uint8_t channels_last = channels_start + channelsCount;
  std::memset(target, 0, 2*rfchannelsCount );
  for (uint8_t channel = channels_start, i=0; i<rfchannelsCount && channel < channels_last; channel++, i++) {
    if (moduleData->failsafeMode == FAILSAFE_CUSTOM) {
      if(FAILSAFE_CHANNEL_HOLD==g_model.failsafeChannels[channel]){
        pulseValue = FAILSAFE_HOLD_VALUE;
      }else if(FAILSAFE_CHANNEL_NOPULSE==g_model.failsafeChannels[channel]){
        pulseValue = FAILSAFE_NOPULSES_VALUE;
      }
      else{
        pulseValue = convert(g_model.failsafeChannels[channel]);
      }
    }
    else if (moduleData->failsafeMode == FAILSAFE_HOLD) {
      pulseValue = FAILSAFE_HOLD_VALUE;
    }
    else if (moduleData->failsafeMode == FAILSAFE_NOPULSES) {
      pulseValue = FAILSAFE_NOPULSES_VALUE;
    }
    else {
      pulseValue = FAILSAFE_NOPULSES_VALUE;
    }
    target[i] = pulseValue;
  }
  //return max channels because channel count can not be change after bind
  return (uint8_t) (AFHDS3_MAX_CHANNELS);
}

Config_u* getConfig(uint8_t module)
{
  auto p_state = &protoState[module];
  return p_state->getConfig();
}

void applyModelConfig(uint8_t module)
{
  auto p_state = &protoState[module];
  p_state->applyConfigFromModel();
}

static const etx_serial_init _uartParams = {
  .baudrate = 0, //AFHDS3_UART_BAUDRATE,
  .encoding = ETX_Encoding_8N1,
  .direction = ETX_Dir_TX_RX,
  .polarity = ETX_Pol_Normal,
};

static void* initModule(uint8_t module)
{
  etx_module_state_t* mod_st = nullptr;
  etx_serial_init params(_uartParams);
  uint16_t period = AFHDS3_UART_COMMAND_TIMEOUT * 1000;
  uint8_t fAddr = (module == INTERNAL_MODULE ? DeviceAddress::IRM301
                                             : DeviceAddress::FRM303)
                      << 4 |
                  DeviceAddress::TRANSMITTER;

  params.baudrate = AFHDS3_UART_BAUDRATE;
  params.polarity =
    module == INTERNAL_MODULE ? ETX_Pol_Normal : ETX_Pol_Inverted;
  mod_st = modulePortInitSerial(module, ETX_MOD_PORT_UART, &params);

#if defined(CONFIGURABLE_MODULE_PORT)
  if (!mod_st && module == EXTERNAL_MODULE) {
    // Try Connect using aux serial mod
    params.polarity = ETX_Pol_Normal;
    mod_st = modulePortInitSerial(module, ETX_MOD_PORT_UART, &params);
  }
#endif

  if (!mod_st && module == EXTERNAL_MODULE) {
    // soft-serial fallback
    params.baudrate = AFHDS3_SOFTSERIAL_BAUDRATE;
    params.direction = ETX_Dir_TX;
    period = AFHDS3_SOFTSERIAL_COMMAND_TIMEOUT * 1000 /* us */;
    mod_st = modulePortInitSerial(module, ETX_MOD_PORT_SOFT_INV, &params);
    // TODO: telemetry RX ???
  }

  if (!mod_st) return nullptr;

  auto p_state = &protoState[module];
  p_state->init(module, pulsesGetModuleBuffer(module), mod_st, fAddr);
  mod_st->user_data = (void*)p_state;

  mixerSchedulerSetPeriod(module, period);

  return mod_st;
}

static void deinitModule(void* ctx)
{
  auto mod_st = (etx_module_state_t*)ctx;
  modulePortDeInit(mod_st);
}

static void sendPulses(void* ctx, uint8_t* buffer, int16_t* channels,
                       uint8_t nChannels)
{
  (void)buffer;
  (void)channels;
  (void)nChannels;

  auto mod_st = (etx_module_state_t*)ctx;
  auto p_state = (ProtoState*)mod_st->user_data;
  p_state->setupFrame();
  p_state->sendFrame();
}

etx_proto_driver_t ProtoDriver = {
    .protocol = PROTOCOL_CHANNELS_AFHDS3,
    .init = initModule,
    .deinit = deinitModule,
    .sendPulses = sendPulses,
    .processData = processTelemetryData,
};

}  // namespace afhds3
