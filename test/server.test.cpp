#include "OPCUAServer.h"
#include "config_parser.h"

#include <gtest/gtest.h>

#include <wblib/json_utils.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

class TServerTest: public Testing::TLoggedFixture
{
protected:
    std::string testRootDir;
    std::string schemaFile;

    void SetUp()
    {
        testRootDir = GetDataFilePath("config_test_data");
        schemaFile = testRootDir + "/../../wb-mqtt-opcua.schema.json";
    }
};

TEST_F(TServerTest, control)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control = device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("value")).GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server->ControlValueEventCallback(TControlValueEvent(control, std::to_string(0)));
    ASSERT_EQ(control, server->GetControl("test/test"));
}
