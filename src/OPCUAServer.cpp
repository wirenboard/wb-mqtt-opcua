#include "OPCUAServer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <open62541/plugin/create_certificate.h>

#include "log.h"
#include "opcua_exception.h"

#ifndef UA_ENABLE_ENCRYPTION
#error "open62541 must be built with encryption support"
#endif

#define LOG(logger) ::logger.Log() << "[OPCUA] "

namespace
{
    const auto APPLICATION_URI = "urn:wb-mqtt-opcua.server.application";
    const UA_UInt16 CERTIFICATE_KEY_SIZE_BITS = 2048;
    const UA_UInt16 CERTIFICATE_EXPIRES_IN_DAYS = 3650;

    const char* LogCategoryNames[UA_LOGCATEGORIES] = {"network",
                                                      "channel",
                                                      "session",
                                                      "server",
                                                      "client",
                                                      "userland",
                                                      "securitypolicy",
                                                      "eventloop",
                                                      "pubsub",
                                                      "discovery"};

    static_assert(UA_LOGCATEGORIES == 10, "Update LogCategoryNames for the current open62541 UA_LOGCATEGORIES");

    void PrintLogMessage(WBMQTT::TLogger& logger, UA_LogCategory category, const char* msg, va_list args)
    {
        va_list args2;
        va_copy(args2, args);
        auto bufSize = 1 + vsnprintf(nullptr, 0, msg, args);
        std::string str(bufSize, '\0');
        vsnprintf(&str[0], bufSize, msg, args2);
        va_end(args2);
        logger.Log() << "[OPCUA] " << LogCategoryNames[category] << ": " << str;
    }

    extern "C" {
    void Log(void* context, UA_LogLevel level, UA_LogCategory category, const char* msg, va_list args)
    {
        switch (level) {
            case UA_LOGLEVEL_TRACE:
            case UA_LOGLEVEL_DEBUG:
                PrintLogMessage(Debug, category, msg, args);
                break;
            case UA_LOGLEVEL_INFO:
                PrintLogMessage(Info, category, msg, args);
                break;
            case UA_LOGLEVEL_WARNING:
                PrintLogMessage(Warn, category, msg, args);
                break;
            case UA_LOGLEVEL_ERROR:
            case UA_LOGLEVEL_FATAL:
                PrintLogMessage(Error, category, msg, args);
                break;
        }
    }

    void LogClear(UA_Logger* logger)
    {}

    UA_StatusCode ReadVariableCallback(UA_Server* sserver,
                                       const UA_NodeId* ssessionId,
                                       void* ssessionContext,
                                       const UA_NodeId* snodeId,
                                       void* snodeContext,
                                       UA_Boolean ssourceTimeStamp,
                                       const UA_NumericRange* range,
                                       UA_DataValue* dataValue)
    {
        OPCUA::TServerImpl* server = (OPCUA::TServerImpl*)(snodeContext);
        return server->ReadVariable(snodeId, dataValue);
    }

    UA_StatusCode WriteVariableCallback(UA_Server* server,
                                        const UA_NodeId* sessionId,
                                        void* sessionContext,
                                        const UA_NodeId* nodeId,
                                        void* nodeContext,
                                        const UA_NumericRange* range,
                                        const UA_DataValue* data)
    {
        OPCUA::TServerImpl* s = (OPCUA::TServerImpl*)(nodeContext);
        return s->WriteVariable(nodeId, data);
    }
    }

    UA_Logger* GetLogger()
    {
        static UA_Logger logger = {Log, nullptr, LogClear};
        return &logger;
    }

    UA_Byte GetAccessLevel(const WBMQTT::PControl& control)
    {
        return control->IsReadonly() ? UA_ACCESSLEVELMASK_READ : UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    }

    UA_NodeId GetDataType(const WBMQTT::PControl& control)
    {
        try {
            auto v = control->GetValue();
            if (v.Is<bool>()) {
                return UA_NODEID_NUMERIC(0, UA_NS0ID_BOOLEAN);
            }
            if (v.Is<double>()) {
                return UA_NODEID_NUMERIC(0, UA_NS0ID_DOUBLE);
            }
        } catch (...) {
        }
        return UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
    }

    const char* GetDataTypeName(const UA_NodeId& dataType)
    {
        if (dataType.namespaceIndex == 0 && dataType.identifierType == UA_NODEIDTYPE_NUMERIC) {
            switch (dataType.identifier.numeric) {
                case UA_NS0ID_BOOLEAN:
                    return "Boolean";
                case UA_NS0ID_DOUBLE:
                    return "Double";
                case UA_NS0ID_BASEDATATYPE:
                    return "BaseDataType";
            }
        }
        return "unknown";
    }

    void SetVariableAttributes(UA_VariableAttributes& attr, WBMQTT::PControl control)
    {
        attr.accessLevel = GetAccessLevel(control);
        attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)control->GetId().c_str());
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.dataType = GetDataType(control);
    }

    void SetServerUrl(UA_ServerConfig* serverCfg, const std::string& url)
    {
        UA_Array_delete(serverCfg->serverUrls, serverCfg->serverUrlsSize, &UA_TYPES[UA_TYPES_STRING]);
        serverCfg->serverUrls = nullptr;
        serverCfg->serverUrlsSize = 0;

        auto urls = static_cast<UA_String*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_STRING]));
        if (!urls) {
            throw std::runtime_error("OPC UA server URL allocation failed");
        }
        urls[0] = UA_String_fromChars(url.c_str());
        serverCfg->serverUrls = urls;
        serverCfg->serverUrlsSize = 1;
    }

    void ThrowIfBad(UA_StatusCode res, const std::string& what)
    {
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error(what + ": " + UA_StatusCode_name(res));
        }
    }

    std::string ReadFile(const std::string& fileName)
    {
        std::ifstream file(fileName, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Can't open " + fileName);
        }
        return std::string(std::istreambuf_iterator<char>(file), {});
    }

    void WriteFile(const std::string& fileName, const UA_ByteString& data, mode_t mode)
    {
        int fd = open(fileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (fd < 0) {
            throw std::runtime_error("Can't create " + fileName + ": " + strerror(errno));
        }
        auto fail = [&](const std::string& what) {
            int err = errno; // close() may overwrite it
            close(fd);
            throw std::runtime_error(what + " " + fileName + ": " + strerror(err));
        };
        if (fchmod(fd, mode) != 0) {
            fail("Can't set permissions of");
        }
        for (size_t written = 0; written < data.length;) {
            auto res = write(fd, data.data + written, data.length - written);
            if (res < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("Can't write");
            }
            written += res;
        }
        if (fsync(fd) != 0) {
            fail("Can't sync");
        }
        if (close(fd) != 0) {
            throw std::runtime_error("Can't write " + fileName + ": " + strerror(errno));
        }
    }

    void SyncDirectory(const std::filesystem::path& dir)
    {
        int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            throw std::runtime_error("Can't open " + dir.string() + ": " + strerror(errno));
        }
        int res = fsync(fd);
        int err = errno; // close() may overwrite it
        close(fd);
        if (res != 0) {
            throw std::runtime_error("Can't sync " + dir.string() + ": " + strerror(err));
        }
    }

    std::string GetTempFileName(const std::string& fileName)
    {
        return fileName + ".tmp";
    }

    //! Owns a byte string allocated by open62541
    struct TByteString
    {
        UA_ByteString Data = UA_BYTESTRING_NULL;

        TByteString() = default;
        TByteString(const TByteString&) = delete;
        TByteString& operator=(const TByteString&) = delete;

        ~TByteString()
        {
            UA_ByteString_clear(&Data);
        }
    };

    //! The certificate and the private key are in the same directory
    std::filesystem::path GetCertificateDirectory(const OPCUA::TServerConfig& config)
    {
        return std::filesystem::path(config.PrivateKeyFile).parent_path();
    }

    std::string GetHostName()
    {
        char name[HOST_NAME_MAX + 1] = {};
        if (gethostname(name, HOST_NAME_MAX) != 0) {
            LOG(Warn) << "Can't get host name: " << strerror(errno);
            return "";
        }
        return name;
    }

    void CreateCertificate(const OPCUA::TServerConfig& config)
    {
        LOG(Info) << "Creating self-signed server certificate " << config.CertificateFile;

        std::string commonName = "CN=wb-mqtt-opcua";
        // No "IP:" SAN: open62541 1.4 mbedtls backend writes it from a dangling pointer
        std::vector<std::string> altNames = {std::string("URI:") + APPLICATION_URI};
        // An empty "DNS:" SAN would be malformed, the certificate is usable without it
        auto hostName = GetHostName();
        if (!hostName.empty()) {
            commonName += "@" + hostName;
            altNames.push_back("DNS:" + hostName);
        }

        UA_String subject = UA_STRING(commonName.data());
        std::vector<UA_String> subjectAltNames;
        for (auto& name: altNames) {
            subjectAltNames.push_back(UA_STRING(name.data()));
        }

        UA_UInt16 keySizeBits = CERTIFICATE_KEY_SIZE_BITS;
        UA_UInt16 expiresInDays = CERTIFICATE_EXPIRES_IN_DAYS;
        UA_KeyValuePair params[2];
        params[0].key = UA_QUALIFIEDNAME(0, (char*)"key-size-bits");
        UA_Variant_setScalar(&params[0].value, &keySizeBits, &UA_TYPES[UA_TYPES_UINT16]);
        params[1].key = UA_QUALIFIEDNAME(0, (char*)"expires-in-days");
        UA_Variant_setScalar(&params[1].value, &expiresInDays, &UA_TYPES[UA_TYPES_UINT16]);
        UA_KeyValueMap paramsMap = {2, params};

        // Check the directory before spending time on the key generation
        std::filesystem::create_directories(GetCertificateDirectory(config));

        TByteString key;
        TByteString cert;
        ThrowIfBad(UA_CreateCertificate(GetLogger(),
                                        &subject,
                                        1,
                                        subjectAltNames.data(),
                                        subjectAltNames.size(),
                                        UA_CERTIFICATEFORMAT_DER,
                                        &paramsMap,
                                        &key.Data,
                                        &cert.Data),
                   "OPC UA server certificate creation failed");

        // Files are written to temporary ones and renamed, so a power loss doesn't leave a truncated pair.
        // The certificate is renamed last, see FinishCertificateCreation
        auto keyTempFile = GetTempFileName(config.PrivateKeyFile);
        auto certTempFile = GetTempFileName(config.CertificateFile);
        try {
            WriteFile(keyTempFile, key.Data, S_IRUSR | S_IWUSR);
            WriteFile(certTempFile, cert.Data, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            std::filesystem::rename(keyTempFile, config.PrivateKeyFile);
            std::filesystem::rename(certTempFile, config.CertificateFile);
            SyncDirectory(GetCertificateDirectory(config));
        } catch (...) {
            // Don't leave a half-written pair, it would stop the next start
            std::error_code ec;
            for (const auto& fileName: {keyTempFile, certTempFile, config.PrivateKeyFile, config.CertificateFile}) {
                std::filesystem::remove(fileName, ec);
            }
            throw;
        }
    }

    //! Completes certificate creation interrupted between renames of the key and the certificate
    void FinishCertificateCreation(const OPCUA::TServerConfig& config)
    {
        auto certTempFile = GetTempFileName(config.CertificateFile);
        if (!std::filesystem::exists(config.CertificateFile) && std::filesystem::exists(config.PrivateKeyFile) &&
            !std::filesystem::exists(GetTempFileName(config.PrivateKeyFile)) && std::filesystem::exists(certTempFile))
        {
            LOG(Info) << "Completing interrupted creation of server certificate " << config.CertificateFile;
            std::filesystem::rename(certTempFile, config.CertificateFile);
            SyncDirectory(GetCertificateDirectory(config));
        }
    }

    void PrepareCertificate(const OPCUA::TServerConfig& config)
    {
        FinishCertificateCreation(config);
        bool hasCertificate = std::filesystem::exists(config.CertificateFile);
        bool hasPrivateKey = std::filesystem::exists(config.PrivateKeyFile);
        if (!hasCertificate && !hasPrivateKey) {
            CreateCertificate(config);
        } else if (hasCertificate != hasPrivateKey) {
            // Don't replace a user's certificate or key, the pair must be fixed manually
            throw TConfigException("Both server certificate " + config.CertificateFile + " and private key " +
                                   config.PrivateKeyFile + " must exist, delete both to create self-signed ones");
        }
    }

    //! Checks that the private key matches the certificate by signing a message and verifying the signature
    void CheckKeyPair(const UA_SecurityPolicy& policy)
    {
        void* channelContext = nullptr;
        // The own certificate is used as the remote one to verify the signature with its public key
        ThrowIfBad(policy.channelModule.newContext(&policy, &policy.localCertificate, &channelContext),
                   "Server certificate loading failed");
        const auto& signatureAlgorithm = policy.asymmetricModule.cryptoModule.signatureAlgorithm;
        UA_Byte messageData[] = "wb-mqtt-opcua";
        UA_ByteString message = {sizeof(messageData), messageData};
        std::vector<UA_Byte> signatureData(signatureAlgorithm.getLocalSignatureSize(channelContext));
        UA_ByteString signature = {signatureData.size(), signatureData.data()};
        auto res = signatureAlgorithm.sign(channelContext, &message, &signature);
        if (res == UA_STATUSCODE_GOOD) {
            res = signatureAlgorithm.verify(channelContext, &message, &signature);
        }
        policy.channelModule.deleteContext(channelContext);
        ThrowIfBad(res, "Server private key doesn't match the certificate");
    }

    //! Returns a DER certificate, a PEM one is decoded
    std::string GetDerCertificate(const std::string& certData)
    {
        const std::string pemBegin = "-----BEGIN CERTIFICATE-----";
        const std::string pemEnd = "-----END CERTIFICATE-----";
        auto begin = certData.find(pemBegin);
        if (begin == std::string::npos) {
            return certData;
        }
        begin += pemBegin.size();
        auto end = certData.find(pemEnd, begin);
        if (end == std::string::npos) {
            throw std::runtime_error("Malformed PEM server certificate");
        }
        // open62541 base64 decoder doesn't skip line breaks
        std::string base64;
        for (auto c: certData.substr(begin, end - begin)) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                base64 += c;
            }
        }
        UA_String encoded = {base64.size(), reinterpret_cast<UA_Byte*>(base64.data())};
        TByteString decoded;
        ThrowIfBad(UA_ByteString_fromBase64(&decoded.Data, &encoded), "PEM server certificate decoding failed");
        return std::string(reinterpret_cast<const char*>(decoded.Data.data), decoded.Data.length);
    }

    //! Clients reject a server certificate without the server's application URI in subjectAltName
    void CheckApplicationUri(const std::string& certData)
    {
        // Backends' checks aren't used: the default AcceptAll one accepts any URI,
        // the mbedtls one only searches the URI as a substring, so "<URI>2" passes it.
        // Exact match is a DER encoded URI general name: tag [6], short form length (URI is shorter than 128 bytes)
        const std::string uri = APPLICATION_URI;
        auto generalName = std::string{char(0x86), char(uri.size())} + uri;
        if (GetDerCertificate(certData).find(generalName) == std::string::npos) {
            throw std::runtime_error("Server certificate subjectAltName must contain URI:" + uri);
        }
    }

    void AddSecurityPolicies(UA_ServerConfig* serverCfg, const OPCUA::TServerConfig& config)
    {
        if (config.SecurityMode == OPCUA::TSecurityMode::None) {
            ThrowIfBad(UA_ServerConfig_addSecurityPolicyNone(serverCfg, nullptr),
                       "OPC UA security policy addition failed");
            return;
        }

        // Restarting the service won't help here, but would create a new key every second
        try {
            PrepareCertificate(config);
        } catch (const TConfigException&) {
            throw;
        } catch (const std::runtime_error& e) {
            throw TConfigException(std::string("Can't prepare server certificate: ") + e.what());
        }
        // An unreadable or malformed pair is a config error, restarting the service won't fix it
        try {
            auto certData = ReadFile(config.CertificateFile);
            auto keyData = ReadFile(config.PrivateKeyFile);
            UA_ByteString cert = {certData.size(), reinterpret_cast<UA_Byte*>(certData.data())};
            UA_ByteString key = {keyData.size(), reinterpret_cast<UA_Byte*>(keyData.data())};

            // SecurityPolicy#None is needed for endpoints discovery, the certificate is sent to clients in GetEndpoints
            ThrowIfBad(UA_ServerConfig_addSecurityPolicyNone(serverCfg, &cert),
                       "OPC UA security policy addition failed");
            ThrowIfBad(UA_ServerConfig_addSecurityPolicyBasic256Sha256(serverCfg, &cert, &key),
                       "OPC UA Basic256Sha256 security policy addition failed");
            ThrowIfBad(UA_ServerConfig_addSecurityPolicyAes128Sha256RsaOaep(serverCfg, &cert, &key),
                       "OPC UA Aes128Sha256RsaOaep security policy addition failed");
            ThrowIfBad(UA_ServerConfig_addSecurityPolicyAes256Sha256RsaPss(serverCfg, &cert, &key),
                       "OPC UA Aes256Sha256RsaPss security policy addition failed");

            // Otherwise a wrong pair is found out only by failing secure connections
            const auto& policy = serverCfg->securityPolicies[serverCfg->securityPoliciesSize - 1];
            CheckKeyPair(policy);
            CheckApplicationUri(certData);
        } catch (const std::runtime_error& e) {
            throw TConfigException(std::string(e.what()) + ", check server certificate " + config.CertificateFile +
                                   " and private key " + config.PrivateKeyFile +
                                   " (DER or PEM), delete both to create self-signed ones");
        }
    }

    void AddEndpoints(UA_ServerConfig* serverCfg, const OPCUA::TServerConfig& config)
    {
        if (config.SecurityMode == OPCUA::TSecurityMode::Required) {
            // Sign endpoints aren't added, they transmit data unencrypted
            for (size_t i = 0; i < serverCfg->securityPoliciesSize; ++i) {
                const auto& policyUri = serverCfg->securityPolicies[i].policyUri;
                if (!UA_String_equal(&policyUri, &UA_SECURITY_POLICY_NONE_URI)) {
                    ThrowIfBad(UA_ServerConfig_addEndpoint(serverCfg, policyUri, UA_MESSAGESECURITYMODE_SIGNANDENCRYPT),
                               "OPC UA server endpoint allocation failed");
                }
            }
            serverCfg->securityPolicyNoneDiscoveryOnly = true;
            return;
        }
        if (config.SecurityMode == OPCUA::TSecurityMode::Optional) {
            // Must be called first, it removes previously added endpoints.
            // Adds Sign and SignAndEncrypt endpoints for all not deprecated policies
            ThrowIfBad(UA_ServerConfig_addAllSecureEndpoints(serverCfg), "OPC UA server endpoint allocation failed");
        }
        ThrowIfBad(UA_ServerConfig_addEndpoint(serverCfg, UA_SECURITY_POLICY_NONE_URI, UA_MESSAGESECURITYMODE_NONE),
                   "OPC UA server endpoint allocation failed");
    }

    void ConfigureOpcUaServer(UA_ServerConfig* serverCfg, const OPCUA::TServerConfig& config)
    {
        serverCfg->logging = GetLogger();

        ThrowIfBad(UA_ServerConfig_setBasics_withPort(serverCfg, static_cast<UA_UInt16>(config.BindPort)),
                   "OPC UA server configuration failed");
        serverCfg->allowEmptyVariables = UA_RULEHANDLING_ACCEPT;

        UA_BuildInfo_clear(&serverCfg->buildInfo);
        UA_ApplicationDescription_clear(&serverCfg->applicationDescription);
        serverCfg->applicationDescription.applicationUri = UA_STRING_ALLOC(APPLICATION_URI);
        serverCfg->applicationDescription.productUri = UA_STRING_ALLOC("https://wirenboard.com");
        serverCfg->applicationDescription.applicationName =
            UA_LOCALIZEDTEXT_ALLOC("en", "Wiren Board MQTT to OPC UA gateway");
        serverCfg->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

        if (!config.BindIp.empty()) {
            SetServerUrl(serverCfg, "opc.tcp://" + config.BindIp + ":" + std::to_string(config.BindPort));
        }

        AddSecurityPolicies(serverCfg, config);

        if (config.Auth && config.SecurityMode == OPCUA::TSecurityMode::None) {
            LOG(Warn) << "Security is \"none\", OPC UA password will be transmitted in plain text";
            serverCfg->allowNonePolicyPassword = true;
        }

        // X.509 user tokens are not supported: default AcceptAll verification would accept any certificate.
        // Without verifyCertificate the access control neither offers nor accepts them
        serverCfg->sessionPKI.verifyCertificate = nullptr;

        // A token policy URI gives one token per type, without it tokens are duplicated for every security policy.
        // Its value isn't used: tokens are encrypted with the secure channel's policy,
        // on SecurityPolicy#None endpoints with Basic256Sha256 if there are encrypting policies.
        // The last added policy is an encrypting one if there are any, otherwise open62541 warns about plain passwords
        UA_UsernamePasswordLogin login = {UA_STRING((char*)config.User.c_str()),
                                          UA_STRING((char*)config.Password.c_str())};
        ThrowIfBad(UA_AccessControl_default(serverCfg,
                                            !config.Auth,
                                            &serverCfg->securityPolicies[serverCfg->securityPoliciesSize - 1].policyUri,
                                            config.Auth ? 1 : 0,
                                            &login),
                   "OPC UA access control configuration failed");

        AddEndpoints(serverCfg, config);
    }

    UA_Server* MakeOpcUaServer(const OPCUA::TServerConfig& config)
    {
        UA_ServerConfig serverCfg;
        memset(&serverCfg, 0, sizeof(serverCfg));
        try {
            ConfigureOpcUaServer(&serverCfg, config);
        } catch (...) {
            UA_ServerConfig_clean(&serverCfg);
            throw;
        }
        auto server = UA_Server_newWithConfig(&serverCfg);
        if (!server) {
            throw std::runtime_error("OPC UA server initilization failed");
        }
        return server;
    }

    struct TBrowsePathResult
    {
        UA_BrowsePathResult Result;

        explicit TBrowsePathResult(UA_BrowsePathResult result): Result(result)
        {}

        ~TBrowsePathResult()
        {
            UA_BrowsePathResult_clear(&Result);
        }

        TBrowsePathResult(const TBrowsePathResult&) = delete;
        TBrowsePathResult& operator=(const TBrowsePathResult&) = delete;
    };

}

namespace OPCUA
{
    TServerImpl::TServerImpl(const TServerConfig& config, WBMQTT::PDeviceDriver driver)
        : Server(MakeOpcUaServer(config)),
          IsRunning(true),
          Config(config),
          Driver(driver)
    {
        try {
            EventHandlers.push_back(Driver->On<WBMQTT::TControlValueEvent>(
                [this](const WBMQTT::TControlValueEvent& event) { ControlValueEventCallback(event); }));
            EventHandlers.push_back(Driver->On<WBMQTT::TNewExternalDeviceControlMetaEvent>(
                [this](const WBMQTT::TNewExternalDeviceControlMetaEvent& event) { ControlMetaEventCallback(event); }));

            // Load external controls
            std::vector<std::string> deviceIds;
            for (const auto& device: config.ObjectNodes) {
                LOG(Debug) << "'" << device.first << "' is added to filter";
                deviceIds.emplace_back(device.first);
            }
            Driver->SetFilter(WBMQTT::GetDeviceListFilter(deviceIds));
            Driver->WaitForReady();

            // Run OPC UA server
            ServerThread = std::thread([this]() {
                auto res = UA_Server_run(Server, &IsRunning);
                if (res != UA_STATUSCODE_GOOD) {
                    LOG(Error) << UA_StatusCode_name(res);
                    exit(1);
                }
            });
        } catch (...) {
            // The destructor isn't called for a partially constructed object
            RemoveEventHandlers();
            UA_Server_delete(Server);
            throw;
        }
    }

    TServerImpl::~TServerImpl()
    {
        RemoveEventHandlers();
        if (IsRunning) {
            IsRunning = false;
            if (ServerThread.joinable()) {
                ServerThread.join();
            }
        }
        if (Server) {
            UA_Server_delete(Server);
        }
    }

    void TServerImpl::RemoveEventHandlers()
    {
        for (const auto& handle: EventHandlers) {
            Driver->RemoveEventHandler(handle);
        }
        EventHandlers.clear();
    }

    void TServerImpl::AddControl(const std::string& nodeName, WBMQTT::PControl control)
    {
        std::unique_lock<std::mutex> lock(Mutex);
        ControlMap[nodeName] = control;
    }

    void TServerImpl::RemoveControl(const std::string& nodeName)
    {
        std::unique_lock<std::mutex> lock(Mutex);
        ControlMap.erase(nodeName);
        PendingAttributesUpdates.erase(nodeName);
    }

    WBMQTT::PControl TServerImpl::GetControl(const std::string& nodeName)
    {
        std::unique_lock<std::mutex> lock(Mutex);
        auto it = ControlMap.find(nodeName);
        return it != ControlMap.end() ? it->second : nullptr;
    }

    UA_StatusCode TServerImpl::WriteVariable(const UA_NodeId* snodeId, const UA_DataValue* dataValue)
    {
        std::string nodeIdName((const char*)snodeId->identifier.string.data, snodeId->identifier.string.length);
        auto ctrl = GetControl(nodeIdName);
        if (!ctrl || ctrl->IsReadonly()) {
            LOG(Error) << "Variable node '" + nodeIdName + "' writing failed. "
                       << (ctrl ? "It is read only" : "It is not presented in MQTT");
            return UA_STATUSCODE_BADDEVICEFAILURE;
        }
        // Without a type, range limits are unknown and the value can't be validated
        if (!ctrl->IsComplete()) {
            LOG(Error) << "Variable node '" + nodeIdName + "' writing failed. It is incomplete";
            return UA_STATUSCODE_BADNOCOMMUNICATION;
        }
        auto tx = Driver->BeginTx();
        try {
            if (dataValue->hasValue) {
                auto isDouble = UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_DOUBLE]);
                std::optional<double> min, max;
                if (ctrl->GetType() == "range") {
                    min = ctrl->GetMin();
                    max = ctrl->GetMax();
                }
                if ((min || max) && !isDouble) {
                    LOG(Error) << "Variable node '" + nodeIdName + "' writing failed. Value is not a double";
                    return UA_STATUSCODE_BADTYPEMISMATCH;
                }
                if (UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
                    auto value = *(UA_Boolean*)dataValue->value.data;
                    ctrl->SetValue(tx, value).Sync();
                    LOG(Info) << "Variable node '" + nodeIdName + "' = " << value;
                    return UA_STATUSCODE_GOOD;
                }
                if (isDouble) {
                    auto value = *(UA_Double*)dataValue->value.data;
                    if ((min && !(value >= *min)) || (max && !(value <= *max))) {
                        LOG(Error) << "Variable node '" + nodeIdName + "' writing failed. Value " << value
                                   << " is out of range";
                        return UA_STATUSCODE_BADOUTOFRANGE;
                    }
                    ctrl->SetValue(tx, value).Sync();
                    LOG(Info) << "Variable node '" + nodeIdName + "' = " << value;
                    return UA_STATUSCODE_GOOD;
                }
                if (UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_STRING])) {
                    // UA_String is not null-terminated, its length must be used explicitly.
                    // An empty string has data pointing to UA_EMPTY_ARRAY_SENTINEL, not to a buffer.
                    auto rawValue = (const UA_String*)dataValue->value.data;
                    std::string value;
                    if (rawValue->length) {
                        value.assign((const char*)rawValue->data, rawValue->length);
                    }
                    ctrl->SetRawValue(tx, value).Sync();
                    LOG(Info) << "Variable node '" + nodeIdName + "' = " << value;
                    return UA_STATUSCODE_GOOD;
                }
            }
            return UA_STATUSCODE_BADDATATYPEIDUNKNOWN;
        } catch (const std::exception& e) {
            LOG(Error) << "Variable node '" + nodeIdName + "' write error: " << e.what();
            return UA_STATUSCODE_BADDEVICEFAILURE;
        }
    }

    UA_StatusCode TServerImpl::ReadVariable(const UA_NodeId* snodeId, UA_DataValue* dataValue)
    {
        std::string nodeIdName((const char*)snodeId->identifier.string.data, snodeId->identifier.string.length);
        auto ctrl = GetControl(nodeIdName);
        if (!ctrl) {
            LOG(Debug) << "Control is not found '" + nodeIdName + "'";
            dataValue->hasStatus = true;
            dataValue->status = UA_STATUSCODE_BADNOCOMMUNICATION;
            return UA_STATUSCODE_GOOD;
        }
        if (!ctrl->IsComplete()) {
            LOG(Debug) << "Control '" + nodeIdName + "' is incomplete";
            dataValue->hasStatus = true;
            dataValue->status = UA_STATUSCODE_BADNOCOMMUNICATION;
            return UA_STATUSCODE_GOOD;
        }
        try {
            dataValue->hasStatus = true;
            if (ctrl->GetError().find("r") != std::string::npos) {
                dataValue->status = UA_STATUSCODE_BAD;
            } else {
                dataValue->status = UA_STATUSCODE_GOOD;
            }
            auto v = ctrl->GetValue();
            if (v.Is<bool>()) {
                auto value = v.As<bool>();
                UA_Variant_setScalarCopy(&dataValue->value, &value, &UA_TYPES[UA_TYPES_BOOLEAN]);
            } else if (v.Is<double>()) {
                auto value = v.As<double>();
                UA_Variant_setScalarCopy(&dataValue->value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
            } else {
                UA_String stringValue = UA_String_fromChars((char*)v.As<std::string>().c_str());
                UA_Variant_setScalarCopy(&dataValue->value, &stringValue, &UA_TYPES[UA_TYPES_STRING]);
                UA_String_clear(&stringValue);
            }
            dataValue->hasValue = true;
        } catch (const std::exception& e) {
            LOG(Error) << "Variable node '" + nodeIdName + "' read error: " << e.what();
            dataValue->hasStatus = true;
            dataValue->status = UA_STATUSCODE_BADNOCOMMUNICATION;
        }
        return UA_STATUSCODE_GOOD;
    }

    void TServerImpl::ControlValueEventCallback(const WBMQTT::TControlValueEvent& event)
    {
        if (event.RawValue.empty()) {
            return;
        }
        auto it = Config.ObjectNodes.find(event.Control->GetDevice()->GetId());
        if (it == Config.ObjectNodes.end()) {
            return;
        }
        std::string nodeName = it->first + "/" + event.Control->GetId();
        if (GetControl(nodeName) == event.Control) {
            if (IsAttributesUpdatePending(nodeName)) {
                UpdateVariableAttributes(nodeName, event.Control);
            }
            return;
        }
        auto isConfigured = [&](const auto& valueNode) { return valueNode.DeviceControlPair == nodeName; };
        if (std::none_of(it->second.begin(), it->second.end(), isConfigured)) {
            return;
        }
        try {
            auto browseName = UA_QUALIFIEDNAME(1, (char*)it->first.c_str());
            TBrowsePathResult parentBpr(
                UA_Server_browseSimplifiedBrowsePath(Server,
                                                     UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                                     1,
                                                     &browseName));
            auto parentNodeId = parentBpr.Result.statusCode == UA_STATUSCODE_GOOD
                                    ? parentBpr.Result.targets[0].targetId.nodeId
                                    : CreateObjectNode(it->first);
            browseName = UA_QUALIFIEDNAME(1, (char*)event.Control->GetId().c_str());
            TBrowsePathResult childBpr(UA_Server_browseSimplifiedBrowsePath(Server, parentNodeId, 1, &browseName));
            AddControl(nodeName, event.Control);
            if (childBpr.Result.statusCode == UA_STATUSCODE_GOOD) {
                LOG(Info) << "Control '" << nodeName << "' is published again, variable node is bound to it";
                UpdateVariableAttributes(nodeName, event.Control);
                return;
            }
            try {
                CreateVariableNode(parentNodeId, nodeName, event.Control);
            } catch (...) {
                RemoveControl(nodeName);
                throw;
            }
        } catch (const std::exception& e) {
            LOG(Error) << "Failed to add control '" << nodeName << "': " << e.what();
        }
    }

    void TServerImpl::ControlMetaEventCallback(const WBMQTT::TNewExternalDeviceControlMetaEvent& event)
    {
        auto device = event.Control->GetDevice();
        std::string nodeName = device->GetId() + "/" + event.Control->GetId();
        if (GetControl(nodeName) != event.Control) {
            return;
        }
        if (device->GetControl(event.Control->GetId()) != event.Control) {
            RemoveControl(nodeName);
            LOG(Warn) << "Control '" << nodeName << "' is removed from MQTT";
            return;
        }
        // An empty type is the JSON /meta topic, it may change both readonly and type
        if (event.Type.empty() || event.Type == "readonly" || event.Type == "type") {
            UpdateVariableAttributes(nodeName, event.Control);
        }
    }

    bool TServerImpl::IsAttributesUpdatePending(const std::string& nodeName)
    {
        std::unique_lock<std::mutex> lock(Mutex);
        return PendingAttributesUpdates.count(nodeName) != 0;
    }

    void TServerImpl::UpdateVariableAttributes(const std::string& nodeName, WBMQTT::PControl control)
    {
        {
            std::unique_lock<std::mutex> lock(Mutex);
            if (!control->IsComplete()) {
                PendingAttributesUpdates.insert(nodeName);
                return;
            }
            PendingAttributesUpdates.erase(nodeName);
        }

        auto nodeId = UA_NODEID_STRING(1, (char*)nodeName.c_str());

        UA_Byte accessLevel = 0;
        auto res = UA_Server_readAccessLevel(Server, nodeId, &accessLevel);
        auto newAccessLevel = GetAccessLevel(control);
        if (res == UA_STATUSCODE_GOOD && accessLevel != newAccessLevel) {
            res = UA_Server_writeAccessLevel(Server, nodeId, newAccessLevel);
            if (res == UA_STATUSCODE_GOOD) {
                LOG(Info) << "Variable node '" << nodeName << "' is "
                          << (control->IsReadonly() ? "read only" : "writable") << " now";
            }
        }
        if (res != UA_STATUSCODE_GOOD) {
            LOG(Error) << "Variable node '" << nodeName << "' access level update failed: " << UA_StatusCode_name(res);
        }

        UA_NodeId dataType;
        UA_NodeId_init(&dataType);
        res = UA_Server_readDataType(Server, nodeId, &dataType);
        auto newDataType = GetDataType(control);
        if (res == UA_STATUSCODE_GOOD && !UA_NodeId_equal(&dataType, &newDataType)) {
            res = UA_Server_writeDataType(Server, nodeId, newDataType);
            if (res == UA_STATUSCODE_GOOD) {
                LOG(Info) << "Variable node '" << nodeName << "' data type is " << GetDataTypeName(newDataType)
                          << " now";
            }
        }
        UA_NodeId_clear(&dataType);
        if (res != UA_STATUSCODE_GOOD) {
            LOG(Error) << "Variable node '" << nodeName << "' data type update failed: " << UA_StatusCode_name(res);
        }
    }

    UA_NodeId TServerImpl::CreateObjectNode(const std::string& nodeName)
    {
        UA_NodeId nodeId = UA_NODEID_STRING(1, (char*)nodeName.c_str());
        UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
        oAttr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)nodeName.c_str());
        auto res = UA_Server_addObjectNode(Server,
                                           nodeId,
                                           UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                           UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                           UA_QUALIFIEDNAME(1, (char*)nodeName.c_str()),
                                           UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
                                           oAttr,
                                           nullptr,
                                           nullptr);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error("Object node '" + nodeName + "' creation failed: " + UA_StatusCode_name(res));
        }
        return nodeId;
    }

    void TServerImpl::CreateVariableNode(const UA_NodeId& parentNodeId,
                                         const std::string& nodeName,
                                         WBMQTT::PControl control)
    {
        UA_VariableAttributes oAttr = UA_VariableAttributes_default;
        SetVariableAttributes(oAttr, control);

        UA_DataSource dataSource;
        dataSource.read = ReadVariableCallback;
        dataSource.write = WriteVariableCallback;

        auto res = UA_Server_addDataSourceVariableNode(Server,
                                                       UA_NODEID_STRING(1, (char*)nodeName.c_str()),
                                                       parentNodeId,
                                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                                       UA_QUALIFIEDNAME(1, (char*)control->GetId().c_str()),
                                                       UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                                       oAttr,
                                                       dataSource,
                                                       this,
                                                       nullptr);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error("Variable node '" + nodeName + "' creation failed: " + UA_StatusCode_name(res));
        }
    }

    std::unique_ptr<IServer> MakeServer(const TServerConfig& config, WBMQTT::PDeviceDriver driver)
    {
        return std::unique_ptr<IServer>(new TServerImpl(config, driver));
    }
}
