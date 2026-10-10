#pragma once
#include "sdkconfig.h"

enum command_id_t : uint16_t{
#define COMMAND(name,val) name = val,
#include "commands_list.h"
#undef COMMAND
};


