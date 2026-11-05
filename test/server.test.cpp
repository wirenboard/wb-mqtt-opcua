#include "OPCUAServer.h"
#include "config_parser.h"

#include <gtest/gtest.h>
#include <limits>

#include <wblib/json_utils.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

namespace
{
    class TFailingServer: public OPCUA::TServerImpl
    {
    public:
        TFailingServer(const OPCUA::TServerConfig& config, WBMQTT::PDeviceDriver driver): TServerImpl(config, driver)
        {}

    protected:
        void CreateVariableNode(const UA_NodeId& parentNodeId,
                                const std::string& nodeName,
                                WBMQTT::PControl control) override
        {
            throw std::runtime_error("forced CreateVariableNode failure");
        }
    };
}

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

// Check that server control map contains the control after MQTT driver callback.
// Control added to the map when OPC UA nodes are created.
TEST_F(TServerTest, control)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control = device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("value")).GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server->ControlValueEventCallback(TControlValueEvent(control, std::to_string(0)));
    ASSERT_EQ(control, server->GetControl("test/test"));
}

TEST_F(TServerTest, control_rollback_on_create_variable_node_failure)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control = device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("value")).GetValue();
    tx->End();

    auto server = std::make_unique<TFailingServer>(config.OpcUa, driver);

    ASSERT_NO_THROW(server->ControlValueEventCallback(TControlValueEvent(control, std::to_string(0))));
    ASSERT_EQ(nullptr, server->GetControl("test/test"));
}

TEST_F(TServerTest, browse_path_result_released_for_unmatched_control)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control = device->CreateControl(tx, TControlArgs{}.SetId("unmatched").SetType("value")).GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);

    for (auto i = 0; i < 5; ++i) {
        server->ControlValueEventCallback(TControlValueEvent(control, std::to_string(i)));
    }

    ASSERT_EQ(nullptr, server->GetControl("test/unmatched"));
}

TEST_F(TServerTest, write_string_value)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control =
        device
            ->CreateControl(tx, TControlArgs{}.SetId("test").SetType("text").SetReadonly(false).SetRawValue("initial"))
            .GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server->ControlValueEventCallback(TControlValueEvent(control, "initial"));
    ASSERT_EQ(control, server->GetControl("test/test"));

    char nodeIdStr[] = "test/test";
    auto nodeId = UA_NODEID_STRING(1, nodeIdStr);

    {
        // The bytes after the value are deliberately non-zero, so reading it as a
        // C string cannot accidentally stop in the right place.
        char buffer[] = {'O', 'N', 'X', 'Y', 'Z'};
        UA_String rawValue;
        rawValue.length = 2;
        rawValue.data = (UA_Byte*)buffer;

        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        UA_Variant_setScalar(&dataValue.value, &rawValue, &UA_TYPES[UA_TYPES_STRING]);
        dataValue.hasValue = true;

        ASSERT_EQ(UA_STATUSCODE_GOOD, server->WriteVariable(&nodeId, &dataValue));
        ASSERT_EQ("ON", control->GetRawValue());
    }

    {
        // Zero-length strings may have a non-null data pointer (e.g. empty/sentinel buffer).
        char buffer[] = {'X', 'Y', 'Z', '\0'};
        UA_String rawValue;
        rawValue.length = 0;
        rawValue.data = (UA_Byte*)buffer;
        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        UA_Variant_setScalar(&dataValue.value, &rawValue, &UA_TYPES[UA_TYPES_STRING]);
        dataValue.hasValue = true;
        ASSERT_EQ(UA_STATUSCODE_GOOD, server->WriteVariable(&nodeId, &dataValue));
        ASSERT_EQ("", control->GetRawValue());
    }

    {
        UA_String rawValue = UA_STRING_NULL;

        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        UA_Variant_setScalar(&dataValue.value, &rawValue, &UA_TYPES[UA_TYPES_STRING]);
        dataValue.hasValue = true;

        ASSERT_EQ(UA_STATUSCODE_GOOD, server->WriteVariable(&nodeId, &dataValue));
        ASSERT_EQ("", control->GetRawValue());
    }
}

// Writes of values out of the control's min and max are rejected
TEST_F(TServerTest, value_limits)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control =
        device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("range").SetReadonly(false).SetRawValue("0"))
            .GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server->ControlValueEventCallback(TControlValueEvent(control, "0"));

    char nodeIdStr[] = "test/test";
    auto nodeId = UA_NODEID_STRING(1, nodeIdStr);
    auto writeDouble = [&](double value) {
        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        UA_Variant_setScalar(&dataValue.value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        dataValue.hasValue = true;
        return server->WriteVariable(&nodeId, &dataValue);
    };
    auto writeString = [&](std::string value) {
        UA_String rawValue = {value.size(), (UA_Byte*)value.data()};
        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        UA_Variant_setScalar(&dataValue.value, &rawValue, &UA_TYPES[UA_TYPES_STRING]);
        dataValue.hasValue = true;
        return server->WriteVariable(&nodeId, &dataValue);
    };

    // No limits
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(1000));
    ASSERT_EQ("1000", control->GetRawValue());

    // wblib sets limits from /meta/min and /meta/max topics
    control->SetSingleMeta("min", "0");
    control->SetSingleMeta("max", "100");
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(0));
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(100));
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(50));
    ASSERT_EQ("50", control->GetRawValue());
    ASSERT_EQ(UA_STATUSCODE_BADOUTOFRANGE, writeDouble(150));
    ASSERT_EQ(UA_STATUSCODE_BADOUTOFRANGE, writeDouble(-1));
    ASSERT_EQ(UA_STATUSCODE_BADOUTOFRANGE, writeDouble(std::numeric_limits<double>::quiet_NaN()));
    ASSERT_EQ(UA_STATUSCODE_BADTYPEMISMATCH, writeString("50"));
    ASSERT_EQ(UA_STATUSCODE_BADTYPEMISMATCH, writeString("150"));
    ASSERT_EQ("50", control->GetRawValue());
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(70));
    ASSERT_EQ("70", control->GetRawValue());

    // Only max is left
    control->SetSingleMeta("min", "");
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(-5));
    ASSERT_EQ(UA_STATUSCODE_BADOUTOFRANGE, writeDouble(150));

    control->SetSingleMeta("max", "");
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(150));
    ASSERT_EQ("150", control->GetRawValue());
}
