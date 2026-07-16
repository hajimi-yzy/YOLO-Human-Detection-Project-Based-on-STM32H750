
#include "INS_interface.h"
#include "MPU_task.h"
extern INS_t INS;
INS_interface INS_I;

Position3 INS_interface::get_euler_angle()
{
    return Position3(INS.Pitch,INS.Roll,INS.Yaw);
}

uint8_t INS_interface::check_if_init_success()
{
    return INS.if_init_success;
}
