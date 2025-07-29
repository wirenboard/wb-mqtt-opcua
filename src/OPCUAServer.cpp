#include "OPCUAServer.h"

#include <open62541/plugin/accesscontrol_default.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/statuscodes.h>

#include <functional>
#include <stdexcept>
#include <vector>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[OPCUA] "

namespace
{
    const char* LogCategoryNames[7] =
        {"network", "channel", "session", "server", "client", "userland", "securitypolicy"};

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

    void LogClear(void* logContext)
    {}

    UA_StatusCode ReadVariableCallback(UA_Server* sserver,
                                       const UA_NodeId* ssessionId,
                                       void* ssessionContext,
                                       const UA_NodeId* snodeId,
                                       void* snodeContext,
                                       UA_Boolean ssourceTimeStamp,
                                       const UA_NumericRange* range,
                                       UA_DataValue* dataValue);

    UA_StatusCode WriteVariableCallback(UA_Server* server,
                                        const UA_NodeId* sessionId,
                                        void* sessionContext,
                                        const UA_NodeId* nodeId,
                                        void* nodeContext,
                                        const UA_NumericRange* range,
                                        const UA_DataValue* data);
    }

    UA_Logger MakeLogger()
    {
        UA_Logger logger = {Log, nullptr, LogClear};
        return logger;
    }

    void SetVariableAttributes(UA_VariableAttributes& attr, WBMQTT::PControl control)
    {
        attr.accessLevel =
            control->IsReadonly() ? UA_ACCESSLEVELMASK_READ : UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)control->GetId().c_str());
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
        try {
            auto v = control->GetValue();
            if (v.Is<bool>()) {
                attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BOOLEAN);
                return;
            }
            if (v.Is<double>()) {
                attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_DOUBLE);
                return;
            }
            return;
        } catch (...) {
        }
    }

    void ConfigureOpcUaServer(UA_ServerConfig* serverCfg, const OPCUA::TServerConfig& config)
    {
        serverCfg->logger = MakeLogger();

        UA_ServerConfig_setBasics(serverCfg);
        serverCfg->allowEmptyVariables = UA_RULEHANDLING_ACCEPT;

        UA_BuildInfo_clear(&serverCfg->buildInfo);
        UA_ApplicationDescription_clear(&serverCfg->applicationDescription);
        serverCfg->applicationDescription.applicationUri = UA_STRING_ALLOC("urn:wb-mqtt-opcua.server.application");
        serverCfg->applicationDescription.productUri = UA_STRING_ALLOC("https://wirenboard.com");
        serverCfg->applicationDescription.applicationName =
            UA_LOCALIZEDTEXT_ALLOC("en", "Wiren Board MQTT to OPC UA gateway");
        serverCfg->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

        if (!config.BindIp.empty()) {
            UA_String_clear(&serverCfg->customHostname);
            serverCfg->customHostname = UA_String_fromChars(config.BindIp.c_str());
        }

        auto res = UA_ServerConfig_addNetworkLayerTCP(serverCfg, config.BindPort, 0, 0);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error(std::string("OPC UA network layer configuration failed: ") +
                                     UA_StatusCode_name(res));
        }

        res = UA_ServerConfig_addSecurityPolicyNone(serverCfg, nullptr);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error(std::string("OPC UA security policy addition failed: ") + UA_StatusCode_name(res));
        }

        res = UA_AccessControl_default(serverCfg,
                                       true,
                                       &serverCfg->securityPolicies[serverCfg->securityPoliciesSize - 1].policyUri,
                                       0,
                                       nullptr);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error(std::string("OPC UA access control configuration failed: ") +
                                     UA_StatusCode_name(res));
        }

        res = UA_ServerConfig_addEndpoint(serverCfg, UA_SECURITY_POLICY_NONE_URI, UA_MESSAGESECURITYMODE_NONE);
        if (res != UA_STATUSCODE_GOOD) {
            throw std::runtime_error(std::string("OPC UA server endpoint allocation failed: ") +
                                     UA_StatusCode_name(res));
        }
    }

    /**! Basic gateway implementation.
     *   The server creates ObjectNodes for groups from config and VariableNodes for MQTT controls.
     *   OPC UA variable node id is DEVICE/CONTROL pair string.
     *   The server translates writes to VariableNodes to publishing into appropriate "on" topics.
     */
    class TServerImpl: public OPCUA::IServer
    {
        std::unordered_map<std::string, WBMQTT::PControl> ControlMap;

        UA_Server* Server;
        volatile UA_Boolean IsRunning;
        std::thread ServerThread;
        WBMQTT::PDeviceDriver Driver;

        UA_NodeId CreateObjectNode(const std::string& nodeName)
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

        void CreateVariableNode(const UA_NodeId& parentNodeId, const std::string& nodeName, WBMQTT::PControl control)
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
                throw std::runtime_error("Variable node '" + nodeName +
                                         "' creation failed: " + UA_StatusCode_name(res));
            }
        }

    public:
        TServerImpl(const OPCUA::TServerConfig& config, WBMQTT::PDeviceDriver driver)
            : Server(UA_Server_new()),
              IsRunning(true),
              Driver(driver)
        {
            if (!Server) {
                throw std::runtime_error("OPC UA server initilization failed");
            }

            Driver->On<WBMQTT::TControlValueEvent>([&](const WBMQTT::TControlValueEvent& event) {
                if (event.RawValue.empty()) {
                    return;
                }

                auto it = config.ObjectNodes.find(event.Control->GetDevice()->GetId());
                if (it == config.ObjectNodes.end()) {
                    return;
                }

                auto browseName = UA_QUALIFIEDNAME(1, (char*)it->first.c_str());
                auto res = UA_Server_browseSimplifiedBrowsePath(Server,
                                                                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                                                1,
                                                                &browseName);
                auto parentNodeId =
                    res.statusCode == UA_STATUSCODE_GOOD ? res.targets[0].targetId.nodeId : CreateObjectNode(it->first);
                std::string nodeName = it->first + "/" + event.Control->GetId();
                for (auto& valueNode: it->second) {
                    if (valueNode.DeviceControlPair != nodeName) {
                        continue;
                    }
                    browseName = UA_QUALIFIEDNAME(1, (char*)event.Control->GetId().c_str());
                    res = UA_Server_browseSimplifiedBrowsePath(Server, parentNodeId, 1, &browseName);
                    if (res.statusCode != UA_STATUSCODE_GOOD) {
                        ControlMap[nodeName] = event.Control;
                        CreateVariableNode(parentNodeId, nodeName, event.Control);
                        break;
                    }
                }
            });

            // Load external controls
            std::vector<std::string> deviceIds;
            for (const auto& device: config.ObjectNodes) {
                LOG(Debug) << "'" << device.first << "' is added to filter";
                deviceIds.emplace_back(device.first);
            }
            Driver->SetFilter(WBMQTT::GetDeviceListFilter(deviceIds));
            Driver->WaitForReady();

            // Setup and run OPC UA server
            ConfigureOpcUaServer(UA_Server_getConfig(Server), config);
            ServerThread = std::thread([this]() {
                auto res = UA_Server_run(Server, &IsRunning);
                if (res != UA_STATUSCODE_GOOD) {
                    LOG(Error) << UA_StatusCode_name(res);
                    exit(1);
                }
            });
        }

        ~TServerImpl()
        {
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

        UA_StatusCode writeVariable(const UA_NodeId* snodeId, const UA_DataValue* dataValue)
        {
            std::string nodeIdName((const char*)snodeId->identifier.string.data, snodeId->identifier.string.length);
            auto it = ControlMap.find(nodeIdName);
            if (it == ControlMap.end() || it->second->IsReadonly()) {
                LOG(Error) << "Variable node '" + nodeIdName + "' writing failed. It is "
                           << (it == ControlMap.end() ? "not presented in MQTT" : "read only");
                return UA_STATUSCODE_BADDEVICEFAILURE;
            }
            auto tx = Driver->BeginTx();
            try {
                if (dataValue->hasValue) {
                    if (UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
                        auto value = *(UA_Boolean*)dataValue->value.data;
                        it->second->SetValue(tx, value).Sync();
                        LOG(Info) << "Variable node '" + nodeIdName + "' = " << value;
                        return UA_STATUSCODE_GOOD;
                    }
                    if (UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_DOUBLE])) {
                        auto value = *(UA_Double*)dataValue->value.data;
                        it->second->SetValue(tx, value).Sync();
                        LOG(Info) << "Variable node '" + nodeIdName + "' = " << value;
                        return UA_STATUSCODE_GOOD;
                    }
                    if (UA_Variant_hasScalarType(&dataValue->value, &UA_TYPES[UA_TYPES_STRING])) {
                        auto value = (char*)((UA_String*)dataValue->value.data)->data;
                        it->second->SetRawValue(tx, value).Sync();
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

        UA_StatusCode readVariable(const UA_NodeId* snodeId, UA_DataValue* dataValue)
        {
            std::string nodeIdName((const char*)snodeId->identifier.string.data, snodeId->identifier.string.length);
            auto it = ControlMap.find(nodeIdName);
            if (it == ControlMap.end()) {
                LOG(Error) << "Control is not found '" + nodeIdName + "'";
                dataValue->hasStatus = true;
                dataValue->status = UA_STATUSCODE_BADNOCOMMUNICATION;
                return UA_STATUSCODE_GOOD;
            }
            try {
                dataValue->hasStatus = true;
                if (it->second->GetError().find("r") != std::string::npos) {
                    dataValue->status = UA_STATUSCODE_BAD;
                } else {
                    dataValue->status = UA_STATUSCODE_GOOD;
                }
                auto v = it->second->GetValue();
                if (v.Is<bool>()) {
                    auto value = v.As<bool>();
                    UA_Variant_setScalarCopy(&dataValue->value, &value, &UA_TYPES[UA_TYPES_BOOLEAN]);
                } else {
                    if (v.Is<double>()) {
                        auto value = v.As<double>();
                        UA_Variant_setScalarCopy(&dataValue->value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
                    } else {
                        UA_String stringValue = UA_String_fromChars((char*)v.As<std::string>().c_str());
                        UA_Variant_setScalarCopy(&dataValue->value, &stringValue, &UA_TYPES[UA_TYPES_STRING]);
                        UA_String_clear(&stringValue);
                    }
                }
                dataValue->hasValue = true;
            } catch (const std::exception& e) {
                LOG(Error) << "Variable node '" + nodeIdName + "' read error: " << e.what();
                dataValue->hasStatus = true;
                dataValue->status = UA_STATUSCODE_BADNOCOMMUNICATION;
            }
            return UA_STATUSCODE_GOOD;
        }
    };

    extern "C" {
    UA_StatusCode ReadVariableCallback(UA_Server* sserver,
                                       const UA_NodeId* ssessionId,
                                       void* ssessionContext,
                                       const UA_NodeId* snodeId,
                                       void* snodeContext,
                                       UA_Boolean ssourceTimeStamp,
                                       const UA_NumericRange* range,
                                       UA_DataValue* dataValue)
    {
        TServerImpl* server = (TServerImpl*)(snodeContext);
        return server->readVariable(snodeId, dataValue);
    }

    UA_StatusCode WriteVariableCallback(UA_Server* server,
                                        const UA_NodeId* sessionId,
                                        void* sessionContext,
                                        const UA_NodeId* nodeId,
                                        void* nodeContext,
                                        const UA_NumericRange* range,
                                        const UA_DataValue* data)
    {
        TServerImpl* s = (TServerImpl*)(nodeContext);
        return s->writeVariable(nodeId, data);
    }
    }
}

std::unique_ptr<OPCUA::IServer> OPCUA::MakeServer(const OPCUA::TServerConfig& config, WBMQTT::PDeviceDriver driver)
{
    return std::unique_ptr<OPCUA::IServer>(new TServerImpl(config, driver));
}
