#include "cmd_system.h"
#include "sdkconfig.h"

void register_system(void)
{
    register_system_common();

#if SOC_LIGHT_SLEEP_SUPPORTED
    register_system_light_sleep();
#endif

#if SOC_DEEP_SLEEP_SUPPORTED
    register_system_deep_sleep();
#endif
}
