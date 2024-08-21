#include "opcua_exception.h"

TEmptyConfigException::TEmptyConfigException(): std::runtime_error("Empty configuration")
{}
