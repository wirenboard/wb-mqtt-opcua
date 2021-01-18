#pragma once

#include <jsoncpp/json/json.h>
#include <wblib/wbmqtt.h>
#include "OPCUAServer.h"

struct TConfig
{
    OPCUA::TServerConfig         OpcUa;
    WBMQTT::TMosquittoMqttConfig Mqtt;
    bool                         Debug = false;
};

void LoadConfig(TConfig& cfg, const std::string& configFileName, const std::string& configSchemaFileName);

/**
 * @brief Updates config.
 *        Takes MQTT brocker params from old config and creates new instance of device driver.
 *        Writes resulting config instead of old one.
 * 
 * @param configFileName full path and file name of config to update
 * @param configSchemaFileName full path and file name of config's JSON schema
 */
void UpdateConfig(const std::string& configFileName, const std::string& configSchemaFileName);

/**
 * @brief Updates oldConfig with new controls from driver.
 * 
 * @param driver active instance of device driver
 * @param oldConfig JSON object with config to update
 */
void UpdateConfig(WBMQTT::PDeviceDriver driver, Json::Value& oldConfig);
