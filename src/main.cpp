#include <getopt.h>

#include <wblib/signal_handling.h>
#include <wblib/wbmqtt.h>

#include "OPCUAServer.h"
#include "log.h"

#include "config_parser.h"
#include "opcua_exception.h"

#define LOG(logger) ::logger.Log() << "[main] "

#define STR(x) #x
#define XSTR(x) STR(x)

using namespace std;
using namespace WBMQTT;

const auto APP_NAME = "wb-mqtt-opcua";
const auto CONFIG_FULL_FILE_PATH = "/etc/wb-mqtt-opcua.conf";
const auto CONFIG_JSON_SCHEMA_FULL_FILE_PATH = "/usr/share/wb-mqtt-confed/schemas/wb-mqtt-opcua.schema.json";

const auto DRIVER_STOP_TIMEOUT_S = chrono::seconds(10);

//! Maximun time to start application. Exceded timeout will case application termination.
const auto DRIVER_INIT_TIMEOUT_S = chrono::seconds(60);

const auto EXIT_NOTCONFIGURED = 6; // Is not configured properly; do not auto-restart by systemd

namespace
{
    void PrintStartupInfo()
    {
        std::string commit(XSTR(WBMQTT_COMMIT));
        cout << APP_NAME << " " << XSTR(WBMQTT_VERSION);
        if (!commit.empty()) {
            cout << " git " << commit;
        }
        cout << endl;
    }

    void PrintUsage()
    {
        PrintStartupInfo();
        cout << "Usage:" << endl
             << " " << APP_NAME << " [options]" << endl
             << "Options:" << endl
             << "  -d  level     enable debuging output:" << endl
             << "                  1 - " << APP_NAME << " only;" << endl
             << "                  2 - MQTT only;" << endl
             << "                  3 - both;" << endl
             << "                  negative values - silent mode (-1, -2, -3))" << endl
             << "  -c  config    config file (default " << CONFIG_FULL_FILE_PATH << ")" << endl
             << "  -g  config    update config file with information about active MQTT publications" << endl
             << "  -p  port      MQTT broker port (default: 1883)" << endl
             << "  -h  IP        MQTT broker IP (default: localhost)" << endl
             << "  -u  user      MQTT user (optional)" << endl
             << "  -P  password  MQTT user password (optional)" << endl
             << "  -T  prefix    MQTT topic prefix (optional)" << endl;
    }

    void ParseCommadLine(int argc, char* argv[], WBMQTT::TMosquittoMqttConfig& mqttConfig, string& configFile)
    {
        int debugLevel = 0;
        int c;

        while ((c = getopt(argc, argv, "d:c:g:p:h:T:u:P:")) != -1) {
            switch (c) {
                case 'd':
                    debugLevel = stoi(optarg);
                    break;
                case 'c':
                    configFile = optarg;
                    break;
                case 'g':
                    try {
                        UpdateConfig(optarg, CONFIG_JSON_SCHEMA_FULL_FILE_PATH);
                    } catch (const exception& e) {
                        std::cerr << "FATAL: " << e.what();
                        exit(1);
                    }
                    exit(0);
                case 'p':
                    mqttConfig.Port = stoi(optarg);
                    break;
                case 'h':
                    mqttConfig.Host = optarg;
                    break;
                case 'T':
                    mqttConfig.Prefix = optarg;
                    break;
                case 'u':
                    mqttConfig.User = optarg;
                    break;
                case 'P':
                    mqttConfig.Password = optarg;
                    break;
                default:
                    PrintUsage();
                    exit(2);
            }
        }

        switch (debugLevel) {
            case 0:
                break;
            case -1:
                ::Info.SetEnabled(false);
                break;
            case -2:
                WBMQTT::Info.SetEnabled(false);
                break;
            case -3:
                WBMQTT::Info.SetEnabled(false);
                ::Info.SetEnabled(false);
                break;
            case 1:
                ::Debug.SetEnabled(true);
                break;
            case 2:
                WBMQTT::Debug.SetEnabled(true);
                break;
            case 3:
                WBMQTT::Debug.SetEnabled(true);
                ::Debug.SetEnabled(true);
                break;
            default:
                cout << "Invalid -d parameter value " << debugLevel << endl;
                PrintUsage();
                exit(2);
        }
    }
}

int main(int argc, char* argv[])
{
    TConfig config;
    string configFile(CONFIG_FULL_FILE_PATH);

    TPromise<void> initialized;
    SignalHandling::Handle({SIGINT, SIGTERM});
    SignalHandling::OnSignals({SIGINT, SIGTERM}, [&] { SignalHandling::Stop(); });
    SetThreadName(APP_NAME);

    ParseCommadLine(argc, argv, config.Mqtt, configFile);

    PrintStartupInfo();

    SignalHandling::SetWaitFor(DRIVER_INIT_TIMEOUT_S, initialized.GetFuture(), [&] {
        LOG(Error) << "Driver takes too long to initialize. Exiting.";
        exit(1);
    });

    SignalHandling::SetOnTimeout(DRIVER_STOP_TIMEOUT_S, [&] {
        LOG(Error) << "Driver takes too long to stop. Exiting.";
        exit(1);
    });

    try {
        LoadConfig(config, configFile, CONFIG_JSON_SCHEMA_FULL_FILE_PATH);
        if (config.Debug) {
            ::Debug.SetEnabled(true);
        }

        if (config.Mqtt.Id.empty())
            config.Mqtt.Id = APP_NAME;

        SignalHandling::Start();

        auto mqtt = NewMosquittoMqttClient(config.Mqtt);
        auto backend = NewDriverBackend(mqtt);
        auto driver = NewDriver(TDriverArgs{}.SetId(APP_NAME).SetBackend(backend));

        driver->StartLoop();
        driver->WaitForReady();

        auto IecServer(OPCUA::MakeServer(config.OpcUa, driver));

        initialized.Complete();
        SignalHandling::Wait();
    } catch (const TConfigException& e) {
        LOG(Error) << "FATAL: " << e.what();
        return EXIT_NOTCONFIGURED;
    } catch (const exception& e) {
        LOG(Error) << "FATAL: " << e.what();
        return 1;
    }

    return 0;
}
