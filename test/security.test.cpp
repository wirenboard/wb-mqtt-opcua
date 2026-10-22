#include "OPCUAServer.h"
#include "opcua_exception.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/plugin/create_certificate.h>
#include <open62541/plugin/log_stdout.h>

#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

namespace
{
    std::string GetPolicyName(const UA_String& uri)
    {
        std::string s(reinterpret_cast<const char*>(uri.data), uri.length);
        return s.substr(s.find('#') + 1);
    }

    //! Describes endpoint as "<policy>/<mode>: <token type>[(<token encryption policy>)], ..."
    std::string DescribeEndpoint(const UA_EndpointDescription& endpoint)
    {
        const char* modes[] = {"Invalid", "None", "Sign", "SignAndEncrypt"};
        const char* tokenTypes[] = {"Anonymous", "UserName", "Certificate", "IssuedToken"};
        std::string res = GetPolicyName(endpoint.securityPolicyUri) + "/" + modes[endpoint.securityMode] + ":";
        for (size_t i = 0; i < endpoint.userIdentityTokensSize; ++i) {
            const auto& token = endpoint.userIdentityTokens[i];
            res += std::string(" ") + tokenTypes[token.tokenType];
            if (token.securityPolicyUri.length) {
                res += "(" + GetPolicyName(token.securityPolicyUri) + ")";
            }
        }
        return res;
    }

    std::vector<std::string> GetEndpoints(uint32_t port)
    {
        auto client = UA_Client_new();
        auto clientConfig = UA_Client_getConfig(client);
        UA_ClientConfig_setDefault(clientConfig);
        // Preset unencrypted endpoint, so the client doesn't look for an endpoint it supports
        clientConfig->endpoint.securityMode = UA_MESSAGESECURITYMODE_NONE;
        clientConfig->endpoint.securityPolicyUri = UA_STRING_ALLOC("http://opcfoundation.org/UA/SecurityPolicy#None");
        auto url = "opc.tcp://localhost:" + std::to_string(port);
        // The server is started in a separate thread, so wait for it a bit
        auto res = UA_STATUSCODE_BAD;
        for (int i = 0; i < 50 && res != UA_STATUSCODE_GOOD; ++i) {
            res = UA_Client_connectSecureChannel(client, url.c_str());
            if (res != UA_STATUSCODE_GOOD) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        EXPECT_EQ(UA_STATUSCODE_GOOD, res) << UA_StatusCode_name(res);

        // UA_Client_getEndpoints can't be used, it fails if there is no endpoint suitable for the client
        UA_GetEndpointsRequest request;
        UA_GetEndpointsRequest_init(&request);
        request.endpointUrl = UA_STRING(url.data());
        UA_GetEndpointsResponse response;
        __UA_Client_Service(client,
                            &request,
                            &UA_TYPES[UA_TYPES_GETENDPOINTSREQUEST],
                            &response,
                            &UA_TYPES[UA_TYPES_GETENDPOINTSRESPONSE]);
        EXPECT_EQ(UA_STATUSCODE_GOOD, response.responseHeader.serviceResult)
            << UA_StatusCode_name(response.responseHeader.serviceResult);
        std::vector<std::string> descriptions;
        for (size_t i = 0; i < response.endpointsSize; ++i) {
            descriptions.push_back(DescribeEndpoint(response.endpoints[i]));
        }
        UA_GetEndpointsResponse_clear(&response);
        UA_Client_delete(client);
        std::sort(descriptions.begin(), descriptions.end());
        return descriptions;
    }

    std::string ReadFile(const std::string& fileName)
    {
        std::ifstream file(fileName, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }

    //! Creates a user's certificate and private key
    void CreateCertificate(const std::string& certFile,
                           const std::string& keyFile,
                           const std::string& uri,
                           UA_CertificateFormat format = UA_CERTIFICATEFORMAT_DER)
    {
        UA_String subject = UA_STRING((char*)"CN=user");
        auto uriName = "URI:" + uri;
        UA_String altName = UA_STRING(uriName.data());
        // The default 4096 bits key is created too slowly under valgrind
        UA_UInt16 keySizeBits = 2048;
        UA_KeyValuePair param;
        param.key = UA_QUALIFIEDNAME(0, (char*)"key-size-bits");
        UA_Variant_setScalar(&param.value, &keySizeBits, &UA_TYPES[UA_TYPES_UINT16]);
        UA_KeyValueMap params = {1, &param};
        UA_ByteString key = UA_BYTESTRING_NULL;
        UA_ByteString cert = UA_BYTESTRING_NULL;
        auto res = UA_CreateCertificate(UA_Log_Stdout, &subject, 1, &altName, 1, format, &params, &key, &cert);
        ASSERT_EQ(UA_STATUSCODE_GOOD, res) << UA_StatusCode_name(res);
        std::ofstream(certFile, std::ios::binary).write(reinterpret_cast<char*>(cert.data), cert.length);
        std::ofstream(keyFile, std::ios::binary).write(reinterpret_cast<char*>(key.data), key.length);
        UA_ByteString_clear(&key);
        UA_ByteString_clear(&cert);
    }

    //! Returns a currently free port, a busy fixed one would make the server thread exit the test binary
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
}

class TServerSecurityTest: public Testing::TLoggedFixture
{
protected:
    std::string CertDir;
    PDeviceDriver Driver;
    Testing::PFakeMqttBroker MqttBroker;

    void SetUp()
    {
        char dirTemplate[] = "/tmp/wb-mqtt-opcua-test-XXXXXX";
        CertDir = mkdtemp(dirTemplate);

        MqttBroker = Testing::NewFakeMqttBroker(*this);
        auto backend = NewDriverBackend(MqttBroker->MakeClient("test"));
        Driver = NewDriver(TDriverArgs{}.SetId("test").SetBackend(backend));
        Driver->StartLoop();
        Driver->WaitForReady();
    }

    void TearDown()
    {
        Driver->StopLoop();
        std::filesystem::remove_all(CertDir);
        TLoggedFixture::TearDown();
    }

    OPCUA::TServerConfig MakeConfig(OPCUA::TSecurityMode mode)
    {
        OPCUA::TServerConfig config;
        config.BindPort = GetFreePort();
        config.SecurityMode = mode;
        config.CertificateFile = CertDir + "/pki/server_cert.der";
        config.PrivateKeyFile = CertDir + "/pki/server_key.der";
        return config;
    }

    std::vector<std::string> GetServerEndpoints(const OPCUA::TServerConfig& config)
    {
        OPCUA::TServerImpl server(config, Driver);
        return GetEndpoints(config.BindPort);
    }
};

// Old configs without "security" must give the same single unencrypted endpoint
TEST_F(TServerSecurityTest, none)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::None);
    EXPECT_EQ(std::vector<std::string>({"None/None: Anonymous"}), GetServerEndpoints(config));
    EXPECT_FALSE(std::filesystem::exists(config.CertificateFile));

    // The password is transmitted in plain text, but login is possible
    config.Auth = true;
    config.User = "user";
    config.Password = "password";
    EXPECT_EQ(std::vector<std::string>({"None/None: UserName"}), GetServerEndpoints(config));
}

TEST_F(TServerSecurityTest, encrypted)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Optional);
    config.Auth = true;
    config.User = "user";
    config.Password = "password";

    // The password is encrypted even on the unencrypted endpoint, no X.509 user tokens
    EXPECT_EQ(std::vector<std::string>({"Aes128_Sha256_RsaOaep/Sign: UserName",
                                        "Aes128_Sha256_RsaOaep/SignAndEncrypt: UserName",
                                        "Aes256_Sha256_RsaPss/Sign: UserName",
                                        "Aes256_Sha256_RsaPss/SignAndEncrypt: UserName",
                                        "Basic256Sha256/Sign: UserName",
                                        "Basic256Sha256/SignAndEncrypt: UserName",
                                        "None/None: UserName(Basic256Sha256)"}),
              GetServerEndpoints(config));

    // Self-signed certificate is created, the key is readable by owner only
    ASSERT_TRUE(std::filesystem::exists(config.CertificateFile));
    ASSERT_TRUE(std::filesystem::exists(config.PrivateKeyFile));
    EXPECT_EQ(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
              std::filesystem::status(config.PrivateKeyFile).permissions());
    auto cert = ReadFile(config.CertificateFile);

    // Existing certificate is reused, unencrypted and sign only endpoints are not offered
    config.SecurityMode = OPCUA::TSecurityMode::Required;
    config.Auth = false;
    EXPECT_EQ(std::vector<std::string>({"Aes128_Sha256_RsaOaep/SignAndEncrypt: Anonymous",
                                        "Aes256_Sha256_RsaPss/SignAndEncrypt: Anonymous",
                                        "Basic256Sha256/SignAndEncrypt: Anonymous"}),
              GetServerEndpoints(config));
    EXPECT_EQ(cert, ReadFile(config.CertificateFile));
}

// A certificate without a key and vice versa is an error, user's files are not replaced
TEST_F(TServerSecurityTest, incomplete_pair)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    std::filesystem::create_directories(CertDir + "/pki");
    for (const auto& fileName: {config.CertificateFile, config.PrivateKeyFile}) {
        std::ofstream(fileName) << "user data";
        EXPECT_THROW(OPCUA::TServerImpl(config, Driver), TConfigException);
        EXPECT_EQ("user data", ReadFile(fileName));
        std::filesystem::remove(fileName);
    }
}

// A malformed pair is a config error, so systemd doesn't restart the service endlessly
TEST_F(TServerSecurityTest, malformed_pair)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    std::filesystem::create_directories(CertDir + "/pki");
    for (const auto& fileName: {config.CertificateFile, config.PrivateKeyFile}) {
        std::ofstream(fileName) << "user data";
    }
    EXPECT_THROW(OPCUA::TServerImpl(config, Driver), TConfigException);
    EXPECT_EQ("user data", ReadFile(config.CertificateFile));
    EXPECT_EQ("user data", ReadFile(config.PrivateKeyFile));
}

// The certificate is renamed last on creation, a power loss before it must not stop the service
TEST_F(TServerSecurityTest, interrupted_creation)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    EXPECT_EQ(3, GetServerEndpoints(config).size());
    auto cert = ReadFile(config.CertificateFile);
    std::filesystem::rename(config.CertificateFile, config.CertificateFile + ".tmp");

    EXPECT_EQ(3, GetServerEndpoints(config).size());
    EXPECT_EQ(cert, ReadFile(config.CertificateFile));
    EXPECT_FALSE(std::filesystem::exists(config.CertificateFile + ".tmp"));
}

// A failed creation is a config error, so systemd doesn't create a new key every second
TEST_F(TServerSecurityTest, creation_failure)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    std::ofstream(CertDir + "/pki") << "not a directory";
    EXPECT_THROW(OPCUA::TServerImpl(config, Driver), TConfigException);
    EXPECT_EQ("not a directory", ReadFile(CertDir + "/pki"));
}

// A wrong user's pair must stop the service on start, not fail every secure connection later
TEST_F(TServerSecurityTest, wrong_user_pair)
{
    const std::string uri = "urn:wb-mqtt-opcua.server.application";
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    std::filesystem::create_directories(CertDir + "/pki");

    // Certificate of another application, URI must match exactly
    for (const auto& otherUri: {std::string("urn:other.application"), uri + "2"}) {
        CreateCertificate(config.CertificateFile, config.PrivateKeyFile, otherUri);
        EXPECT_THROW(OPCUA::TServerImpl(config, Driver), TConfigException) << otherUri;
    }

    // The key doesn't match the certificate
    CreateCertificate(config.CertificateFile, config.PrivateKeyFile, uri);
    CreateCertificate(CertDir + "/other_cert.der", config.PrivateKeyFile, uri);
    EXPECT_THROW(OPCUA::TServerImpl(config, Driver), TConfigException);
}

TEST_F(TServerSecurityTest, user_pem_pair)
{
    auto config = MakeConfig(OPCUA::TSecurityMode::Required);
    config.CertificateFile = CertDir + "/server_cert.pem";
    config.PrivateKeyFile = CertDir + "/server_key.pem";
    CreateCertificate(config.CertificateFile,
                      config.PrivateKeyFile,
                      "urn:wb-mqtt-opcua.server.application",
                      UA_CERTIFICATEFORMAT_PEM);
    EXPECT_EQ(3, GetServerEndpoints(config).size());
}
