#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <open62541/plugin/accesscontrol_default.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/statuscodes.h>

#include <wblib/wbmqtt.h>

namespace OPCUA
{
    struct TVariableNodeConfig
    {
        std::string
            DeviceControlPair; //! DEVICE_NAME/CONTROL_NAME from MQTT (/devices/DEVICE_NAME/controls/CONTROL_NAME)
    };

    typedef std::vector<TVariableNodeConfig> TVariableNodesConfig;

    //! map with object nodes name as keys
    typedef std::map<std::string, TVariableNodesConfig> TObjectNodesConfig;

    //! Message security of OPC UA endpoints
    enum class TSecurityMode
    {
        None,     //! Unencrypted endpoint only
        Optional, //! Unencrypted and encrypted endpoints
        Required  //! Encrypted endpoints only
    };

    //! OPC UA server configuration parameters
    struct TServerConfig
    {
        //! Local IP to bind the server. If empty, the server will listen to all available local IP's
        std::string BindIp;

        //! Port to listen
        uint32_t BindPort = 4840;

        //! Authentication. If disabled, anonymous access is allowed
        bool Auth = false;
        std::string User = "";
        std::string Password = "";

        TSecurityMode SecurityMode = TSecurityMode::None;

        //! Server certificate and private key (DER or PEM) in the same directory.
        //! Self-signed ones are created if both are missing
        std::string CertificateFile = "/var/lib/wb-mqtt-opcua/server_cert.der";
        std::string PrivateKeyFile = "/var/lib/wb-mqtt-opcua/server_key.der";

        TObjectNodesConfig ObjectNodes;
    };

    //! Interface of OPCUA server.
    class IServer
    {
    public:
        virtual ~IServer() = default;
    };

    /**! Basic gateway implementation.
     *   The server creates ObjectNodes for groups from config and VariableNodes for MQTT controls.
     *   OPC UA variable node id is DEVICE/CONTROL pair string.
     *   The server translates writes to VariableNodes to publishing into appropriate "on" topics.
     */
    class TServerImpl: public OPCUA::IServer
    {
    public:
        TServerImpl(const TServerConfig& config, WBMQTT::PDeviceDriver driver);
        ~TServerImpl();

        bool ControlExists(const std::string& nodeName);
        void AddControl(const std::string& nodeName, WBMQTT::PControl control);
        void RemoveControl(const std::string& nodeName);
        WBMQTT::PControl GetControl(const std::string& nodeName);

        UA_StatusCode WriteVariable(const UA_NodeId* snodeId, const UA_DataValue* dataValue);
        UA_StatusCode ReadVariable(const UA_NodeId* snodeId, UA_DataValue* dataValue);

        void ControlValueEventCallback(const WBMQTT::TControlValueEvent& event);

    private:
        std::mutex Mutex;
        std::unordered_map<std::string, WBMQTT::PControl> ControlMap;

        UA_Server* Server;
        volatile UA_Boolean IsRunning;
        std::thread ServerThread;

        const TServerConfig& Config;
        WBMQTT::PDeviceDriver Driver;

    protected:
        virtual UA_NodeId CreateObjectNode(const std::string& nodeName);
        virtual void CreateVariableNode(const UA_NodeId& parentNodeId,
                                        const std::string& nodeName,
                                        WBMQTT::PControl control);
    };

    //! Make a new instance of server
    std::unique_ptr<IServer> MakeServer(const TServerConfig& config, WBMQTT::PDeviceDriver driver);
}
