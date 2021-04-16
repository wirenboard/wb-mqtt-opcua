#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/testing/testlog.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/json_utils.h>

using namespace WBMQTT;

class TLoadConfigTest : public testing::Test
{
protected:
    std::string TestRootDir;
    std::string SchemaFile;

    void SetUp()
    {
        TestRootDir = Testing::TLoggedFixture::GetDataFilePath("config_test_data");
        SchemaFile = TestRootDir + "/../../wb-mqtt-opcua.schema.json";
    }
};

TEST_F(TLoadConfigTest, no_file)
{
    TConfig cfg;
    ASSERT_THROW(LoadConfig(cfg, "", SchemaFile), std::runtime_error);
    ASSERT_THROW(LoadConfig(cfg, TestRootDir + "/bad/wb-mqtt-opcua.conf", ""), std::runtime_error);
}

TEST_F(TLoadConfigTest, bad_config)
{
    // missing fields
    for (size_t i = 1; i <= 3; ++i) {
        TConfig cfg;
        ASSERT_THROW(LoadConfig(cfg, TestRootDir + "/bad/bad" + std::to_string(i) + ".conf", SchemaFile), std::runtime_error) << i;
    }

    // bad topic name
    TConfig cfg;
    LoadConfig(cfg, TestRootDir + "/bad/bad_topic_name.conf", SchemaFile);
    ASSERT_EQ(cfg.OpcUa.ObjectNodes.size(), 1);
    ASSERT_EQ(cfg.OpcUa.ObjectNodes["test"].size(), 1);
}

TEST_F(TLoadConfigTest, good)
{
    TConfig cfg;
    LoadConfig(cfg, TestRootDir + "/bad/wb-mqtt-opcua.conf", SchemaFile);
    ASSERT_EQ(cfg.OpcUa.ObjectNodes.size(), 1);
    ASSERT_EQ(cfg.OpcUa.ObjectNodes["test"].size(), 1);
    ASSERT_STREQ(cfg.OpcUa.ObjectNodes["test"].begin()->DeviceControlPair.c_str(), "test/test");
}

class TUpdateConfigTest: public Testing::TLoggedFixture
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

TEST_F(TUpdateConfigTest, update)
{
    auto mqttBroker = Testing::NewFakeMqttBroker(*this);
    auto mqttClient = mqttBroker->MakeClient("test");
    auto backend = NewDriverBackend(mqttClient);
    auto driver = NewDriver(TDriverArgs{}
        .SetId("test")
        .SetBackend(backend)
    );

    driver->StartLoop();

    auto tx = driver->BeginTx();
    auto device = tx->CreateDevice(TLocalDeviceArgs{}
        .SetId("test")
    ).GetValue();

    device->CreateControl(tx, TControlArgs{}
        .SetId("test")
        .SetType("value")
    ).GetValue();

    device->CreateControl(tx, TControlArgs{}
        .SetId("test4")
        .SetType("rgb")
    ).GetValue();

    auto device2 = tx->CreateDevice(TLocalDeviceArgs{}
        .SetId("test2")
    ).GetValue();

    device2->CreateControl(tx, TControlArgs{}
        .SetId("test2")
        .SetType("value")
        .SetReadonly(true)
    ).GetValue();

    tx->End();

    auto c = JSON::Parse(testRootDir + "/bad/wb-mqtt-opcua.conf");
    JSON::Validate(c, JSON::Parse(schemaFile));

    UpdateConfig(driver, c);

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "    ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    std::stringstream ss;
    writer->write(c, &ss);
    Emit() << ss.str();
}