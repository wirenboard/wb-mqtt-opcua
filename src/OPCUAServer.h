#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

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

    //! OPC UA server configuration parameters
    struct TServerConfig
    {
        //! Local IP to bind the server. If empty, the server will listen to all available local IP's
        std::string BindIp;

        //! Port to listen
        uint32_t BindPort = 4840;

        TObjectNodesConfig ObjectNodes;
    };

    //! Interface of OPCUA server.
    class IServer
    {
    public:
        virtual ~IServer() = default;
    };

    //! Make a new instance of server
    std::unique_ptr<IServer> MakeServer(const TServerConfig& config, WBMQTT::PDeviceDriver driver);
}
