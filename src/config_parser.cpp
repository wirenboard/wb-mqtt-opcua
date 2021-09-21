#include "config_parser.h"

#include <iostream>
#include <fstream>

#include <sys/stat.h>

#include <wblib/wbmqtt.h>
#include <wblib/json_utils.h>

#include "log.h"

using namespace std;
using namespace WBMQTT;
using namespace WBMQTT::JSON;

#define LOG(logger) ::logger.Log() << "[config] "

namespace
{
    const auto GENERATOR_ID = "wb-mqtt-opcua-config_generator";

    std::string GetDeviceName(const std::string& topic)
    {
        return StringSplit(topic, '/')[0];
    }

    std::string GetControlName(const std::string& topic)
    {
        return StringSplit(topic, '/')[1];
    }

    bool IsValidTopic(const std::string& topic)
    {
        auto l = StringSplit(topic, '/');
        return (l.size() == 2);
    }

    OPCUA::TVariableNodesConfig LoadVariableNodes(const Json::Value& controls)
    {
        OPCUA::TVariableNodesConfig res;
        for (const auto& control: controls) {
            bool enabled = false;
            Get(control, "enabled", enabled);
            if (enabled) {
                OPCUA::TVariableNodeConfig n;
                n.DeviceControlPair = control["topic"].asString();
                if (IsValidTopic(n.DeviceControlPair)) {
                    res.push_back(n);
                } else {
                    LOG(Warn) << "Invalid topic: " << n.DeviceControlPair;
                }
            }
        }
        return res;
    }

    OPCUA::TObjectNodesConfig LoadNodes(const Json::Value& config)
    {
        OPCUA::TObjectNodesConfig res;
        for (const auto& group: config["groups"]) {
            bool enabled = false;
            Get(group, "enabled", enabled);
            if (enabled) {
                res.insert({group["name"].asString(), LoadVariableNodes(group["controls"])});
            }
        }
        return res;
    }

    Json::Value MakeControlConfig(const std::string& topic, const std::string& info)
    {
        Json::Value cnt(Json::objectValue);
        cnt["topic"] = topic;
        cnt["info"] = info;
        cnt["enabled"] = false;
        return cnt;
    }

    void AppendControl(Json::Value& root, PControl c)
    {
        std::string info(c->GetType());
        info += (c->IsReadonly() ? " (read only)" : " (setup is allowed)");
        std::string controlName(c->GetDevice()->GetId() + "/" + c->GetId());
        root.append(MakeControlConfig(controlName, info));
    }

    Json::Value MakeControlsConfig(std::map<std::string, PControl>& controls)
    {
        Json::Value res(Json::arrayValue);
        for (auto control: controls) {
            AppendControl(res, control.second);
        }
        return res;
    }

    Json::Value MakeGroupConfig(const std::string& name)
    {
        Json::Value dev(Json::objectValue);
        dev["name"] = name;
        dev["enabled"] = false;
        dev["controls"] = Json::Value(Json::arrayValue);
        return dev;
    }

    Json::Value& GetGroup(Json::Value& config, const std::string& name)
    {
        for (auto& group: config["groups"]) {
            if (group["name"].asString() == name) {
                return group;
            }
        }
        return config["groups"].append(MakeGroupConfig(name));
    }

    void LoadMqttConfig(WBMQTT::TMosquittoMqttConfig& cfg, const Json::Value& configRoot)
    {
        if (configRoot.isMember("mqtt")) {
            Get(configRoot["mqtt"], "host", cfg.Host);
            Get(configRoot["mqtt"], "port", cfg.Port);
            Get(configRoot["mqtt"], "keepalive", cfg.Keepalive);
            bool auth = false;
            Get(configRoot["mqtt"], "auth", auth);
            if (auth) {
                Get(configRoot["mqtt"], "username", cfg.User);
                Get(configRoot["mqtt"], "password", cfg.Password);
            }
        }
    }
}

void LoadConfig(TConfig& cfg, const std::string& configFileName, const std::string& configSchemaFileName)
{
    auto config = JSON::Parse(configFileName);
    JSON::Validate(config, JSON::Parse(configSchemaFileName));

    if (config.isMember("opcua")) {
        Get(config["opcua"], "host", cfg.OpcUa.BindIp);
        Get(config["opcua"], "port", cfg.OpcUa.BindPort);
    }
    LoadMqttConfig(cfg.Mqtt, config);
    cfg.OpcUa.ObjectNodes = LoadNodes(config);
    Get(config, "debug", cfg.Debug);
}

void UpdateConfig(const string& configFileName, const string& configSchemaFileName)
{
    auto config = JSON::Parse(configFileName);
    JSON::Validate(config, JSON::Parse(configSchemaFileName));

    WBMQTT::TMosquittoMqttConfig mqttConfig;
    mqttConfig.Id = GENERATOR_ID;
    LoadMqttConfig(mqttConfig, config);
    auto mqtt = NewMosquittoMqttClient(mqttConfig);
    auto backend = NewDriverBackend(mqtt);
    auto driver = NewDriver(TDriverArgs{}
        .SetId(GENERATOR_ID)
        .SetBackend(backend)
    );
    driver->StartLoop();
    UpdateConfig(driver, config);
    driver->StopLoop();

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "    ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    std::ofstream file(configFileName);
    writer->write(config, &file);
    file << std::endl;
}

void UpdateConfig(PDeviceDriver driver, Json::Value& oldConfig)
{
    driver->WaitForReady();
    driver->SetFilter(GetAllDevicesFilter());
    driver->WaitForReady();

    std::map<std::string, std::map<std::string, PControl>> mqttDevices;
    auto tx = driver->BeginTx();
    for (auto& device: tx->GetDevicesList()) {
        std::map<std::string, PControl> controls;
        for (auto& control: device->ControlsList()) {
            controls.insert({control->GetId(), control});
        }
        if (controls.size()) {
            mqttDevices.insert({device->GetId(), controls});
        }
    }

    for (auto& group: oldConfig["groups"]) {
        for (auto& control: group["controls"]) {
            auto topic = control["topic"].asString();
            if (IsValidTopic(topic)) {
                auto mqttDevice = mqttDevices.find(GetDeviceName(topic));
                if (mqttDevice != mqttDevices.end()) {
                    auto mqttControl = mqttDevice->second.find(GetControlName(topic));
                    if (mqttControl != mqttDevice->second.end()) {
                        mqttDevice->second.erase(mqttControl);
                    }
                }
                if (!mqttDevice->second.size()) {
                    mqttDevices.erase(mqttDevice);
                }
            }
        }
    }

    for (auto& mqttDevice: mqttDevices) {
        auto controls(MakeControlsConfig(mqttDevice.second));
        if (controls.size()) {
            auto& configGroup = GetGroup(oldConfig, mqttDevice.first);
            for (auto& v: controls) {
                configGroup["controls"].append(v);
            }
        }
    }
}