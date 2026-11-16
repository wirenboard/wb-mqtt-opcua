#include "OPCUAServer.h"
#include "config_parser.h"

#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>

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

    uint16_t GetFreePort()
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_LE(0, fd);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        socklen_t addrLen = sizeof(addr);
        EXPECT_EQ(0, bind(fd, reinterpret_cast<sockaddr*>(&addr), addrLen));
        EXPECT_EQ(0, getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen));
        close(fd);
        return ntohs(addr.sin_port);
    }

    using PClient = std::unique_ptr<UA_Client, decltype(&UA_Client_delete)>;

    PClient ConnectClient(uint16_t port)
    {
        PClient client(UA_Client_new(), &UA_Client_delete);
        UA_ClientConfig_setDefault(UA_Client_getConfig(client.get()));
        auto url = "opc.tcp://localhost:" + std::to_string(port);
        // The server is started in a separate thread, so wait for it a bit
        auto res = UA_STATUSCODE_BAD;
        for (int i = 0; i < 100 && res != UA_STATUSCODE_GOOD; ++i) {
            res = UA_Client_connect(client.get(), url.c_str());
            if (res != UA_STATUSCODE_GOOD) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        EXPECT_EQ(UA_STATUSCODE_GOOD, res) << UA_StatusCode_name(res);
        return client;
    }
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

// A control removed from MQTT is unbound from the variable node,
// the same control published again is a new object and the node is bound to it
TEST_F(TServerTest, control_recreated)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);
    config.OpcUa.BindPort = GetFreePort();

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    tx->End();
    auto createControl = [&](const std::string& value) {
        auto tx = driver->BeginTx();
        auto control =
            device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("value").SetRawValue(value)).GetValue();
        tx->End();
        return control;
    };
    auto removeControl = [&]() {
        auto tx = driver->BeginTx();
        device->RemoveControl(tx, "test").Wait();
        tx->End();
    };

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);

    char nodeIdStr[] = "test/test";
    auto nodeId = UA_NODEID_STRING(1, nodeIdStr);
    auto read = [&](UA_StatusCode expectedStatus, std::optional<double> expectedValue = std::nullopt) {
        UA_DataValue dataValue;
        UA_DataValue_init(&dataValue);
        ASSERT_EQ(UA_STATUSCODE_GOOD, server->ReadVariable(&nodeId, &dataValue));
        ASSERT_TRUE(dataValue.hasStatus);
        ASSERT_EQ(expectedStatus, dataValue.status);
        if (expectedValue) {
            ASSERT_TRUE(dataValue.hasValue);
            ASSERT_TRUE(UA_Variant_hasScalarType(&dataValue.value, &UA_TYPES[UA_TYPES_DOUBLE]));
            ASSERT_EQ(*expectedValue, *(UA_Double*)dataValue.value.data);
        }
        UA_DataValue_clear(&dataValue);
    };

    auto control = createControl("1");
    server->ControlValueEventCallback(TControlValueEvent(control, "1"));
    ASSERT_EQ(control, server->GetControl("test/test"));
    read(UA_STATUSCODE_GOOD, 1);

    // Clearing a single meta topic doesn't remove the control
    server->ControlMetaEventCallback(TNewExternalDeviceControlMetaEvent(control, "units", ""));
    ASSERT_EQ(control, server->GetControl("test/test"));

    removeControl();
    server->ControlMetaEventCallback(TNewExternalDeviceControlMetaEvent(control, "type", ""));
    ASSERT_EQ(nullptr, server->GetControl("test/test"));
    read(UA_STATUSCODE_BADNOCOMMUNICATION);

    auto newControl = createControl("2");
    server->ControlValueEventCallback(TControlValueEvent(newControl, "2"));
    ASSERT_EQ(newControl, server->GetControl("test/test"));
    read(UA_STATUSCODE_GOOD, 2);

    // A removal event of the old control doesn't unbind the new one
    server->ControlMetaEventCallback(TNewExternalDeviceControlMetaEvent(control, "type", ""));
    ASSERT_EQ(newControl, server->GetControl("test/test"));

    // The node is bound to a new control even if the removal wasn't seen
    removeControl();
    auto thirdControl = createControl("3");
    server->ControlValueEventCallback(TControlValueEvent(thirdControl, "3"));
    ASSERT_EQ(thirdControl, server->GetControl("test/test"));
    read(UA_STATUSCODE_GOOD, 3);
}

// Attributes of a variable node follow the control's meta changed after the node creation
TEST_F(TServerTest, variable_attributes_update)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);
    config.OpcUa.BindPort = GetFreePort();

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}.SetId("test")).GetValue();
    auto control =
        device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("value").SetReadonly(false).SetRawValue("1"))
            .GetValue();
    tx->End();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server->ControlValueEventCallback(TControlValueEvent(control, "1"));
    auto client = ConnectClient(config.OpcUa.BindPort);

    char nodeIdStr[] = "test/test";
    auto nodeId = UA_NODEID_STRING(1, nodeIdStr);
    auto readAccessLevel = [&]() {
        UA_Byte accessLevel = 0;
        EXPECT_EQ(UA_STATUSCODE_GOOD, UA_Client_readAccessLevelAttribute(client.get(), nodeId, &accessLevel));
        return accessLevel;
    };
    auto readDataType = [&]() {
        UA_NodeId dataType;
        UA_NodeId_init(&dataType);
        EXPECT_EQ(UA_STATUSCODE_GOOD, UA_Client_readDataTypeAttribute(client.get(), nodeId, &dataType));
        EXPECT_EQ(UA_NODEIDTYPE_NUMERIC, dataType.identifierType);
        return dataType.identifier.numeric;
    };
    auto writeDouble = [&](double value) {
        UA_Variant variant;
        UA_Variant_setScalar(&variant, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        return UA_Client_writeValueAttribute(client.get(), nodeId, &variant);
    };
    auto writeString = [&](std::string value) {
        UA_String rawValue = {value.size(), (UA_Byte*)value.data()};
        UA_Variant variant;
        UA_Variant_setScalar(&variant, &rawValue, &UA_TYPES[UA_TYPES_STRING]);
        return UA_Client_writeValueAttribute(client.get(), nodeId, &variant);
    };
    auto updateMeta = [&](const PControl& control, const std::string& meta, const std::string& value) {
        control->SetSingleMeta(meta, value);
        server->ControlMetaEventCallback(TNewExternalDeviceControlMetaEvent(control, meta, value));
    };
    const UA_Byte readWrite = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    ASSERT_EQ(readWrite, readAccessLevel());
    ASSERT_EQ(UA_NS0ID_DOUBLE, readDataType());

    updateMeta(control, "readonly", "1");
    ASSERT_EQ(UA_ACCESSLEVELMASK_READ, readAccessLevel());
    ASSERT_EQ(UA_STATUSCODE_BADNOTWRITABLE, writeDouble(2));
    ASSERT_EQ("1", control->GetRawValue());

    updateMeta(control, "readonly", "0");
    ASSERT_EQ(readWrite, readAccessLevel());
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeDouble(2));
    ASSERT_EQ("2", control->GetRawValue());

    // A cleared readonly topic means a writable control
    updateMeta(control, "readonly", "1");
    ASSERT_EQ(UA_ACCESSLEVELMASK_READ, readAccessLevel());
    updateMeta(control, "readonly", "");
    ASSERT_EQ(readWrite, readAccessLevel());

    // A type cleared while a publisher restarts doesn't change the data type
    updateMeta(control, "type", "");
    ASSERT_EQ(UA_NS0ID_DOUBLE, readDataType());
    updateMeta(control, "type", "value");

    // A text control accepts strings, unlike the Double variable node created for a value one
    updateMeta(control, "type", "text");
    ASSERT_EQ(UA_NS0ID_BASEDATATYPE, readDataType());
    ASSERT_EQ(UA_STATUSCODE_GOOD, writeString("text"));
    ASSERT_EQ("text", control->GetRawValue());

    // Attributes of a control published again are taken from its meta
    tx = driver->BeginTx();
    device->RemoveControl(tx, "test").Wait();
    tx->End();
    server->ControlMetaEventCallback(TNewExternalDeviceControlMetaEvent(control, "type", ""));
    tx = driver->BeginTx();
    auto newControl =
        device->CreateControl(tx, TControlArgs{}.SetId("test").SetType("switch").SetReadonly(true).SetRawValue("1"))
            .GetValue();
    tx->End();
    server->ControlValueEventCallback(TControlValueEvent(newControl, "1"));
    ASSERT_EQ(UA_ACCESSLEVELMASK_READ, readAccessLevel());
    ASSERT_EQ(UA_NS0ID_BOOLEAN, readDataType());
}

// Driver events coming after the server destruction must not reach it
TEST_F(TServerTest, no_events_after_destruction)
{
    TConfig config;
    LoadConfig(config, testRootDir + "/bad/wb-mqtt-opcua.conf", schemaFile);

    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
    driver->StartLoop();
    driver->WaitForReady();

    auto server = std::make_unique<OPCUA::TServerImpl>(config.OpcUa, driver);
    server.reset();

    TPromise<void> received;
    auto handle = driver->On<TControlValueEvent>([&](const TControlValueEvent&) {
        if (!received.IsFulfilled()) {
            received.Complete();
        }
    });
    mqttBroker->Publish("ext", {TMqttMessage{"/devices/test/controls/test/meta/type", "value", 1, true}});
    mqttBroker->Publish("ext", {TMqttMessage{"/devices/test/controls/test", "1", 1, true}});
    ASSERT_TRUE(received.GetFuture().WaitFor(std::chrono::seconds(5)));
    driver->RemoveEventHandler(handle);
    driver->StopLoop();
}
