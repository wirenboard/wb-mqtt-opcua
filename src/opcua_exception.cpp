#include "opcua_exception.h"

TEmptyConfigException::TEmptyConfigException(): std::runtime_error("Empty configuration")
{}

TConfigException::TConfigException(const std::string& message): std::runtime_error("Config error:" + message)
{}
