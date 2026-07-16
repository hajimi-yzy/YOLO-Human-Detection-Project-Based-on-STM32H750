#ifndef INS_INTERFACE_H
#define INS_INTERFACE_H

#include "my_math.h"

class INS_interface
{
public:
    Position3 get_euler_angle();
    uint8_t check_if_init_success();
};

extern INS_interface INS_I;

#endif
