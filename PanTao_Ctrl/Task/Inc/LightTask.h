#ifndef __LIGHTTASK_H__
#define __LIGHTTASK_H__

#include "port_lighteffect.h"
#include "port_light.h"

#define Light1_RGBbuffer_SIZE 66
#define Light1_CRRbuffer_SIZE ((Light1_RGBbuffer_SIZE + 7) * 24)

#define Light2_RGBbuffer_SIZE 16
#define Light2_CRRbuffer_SIZE ((Light2_RGBbuffer_SIZE + 7) * 24)

#define LightTime 300

#define CTRL_LIGHT_GROUP_NUM 9U
#define CTRL_LIGHT_COLOR_NUM 9U

#define CTRL_LIGHT_MODE_ON 0x00U
#define CTRL_LIGHT_MODE_OFF 0x01U
#define CTRL_LIGHT_MODE_BLINK 0x02U
#define CTRL_LIGHT_BLINK_HALF_PERIOD 200U

#define GLOBAL_BREATH_STEP_TIME 10U

bool CtrlLightGroup_Set(uint8_t position, uint8_t color, uint8_t mode);
void CtrlLightGroup_ClearAll(void);

void Light_Init(void);
void Light_Task(void);

#endif
