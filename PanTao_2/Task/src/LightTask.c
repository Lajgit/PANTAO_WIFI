#include "LightTask.h"
#include "MainTask.h"
#include "FlashTask.h"
#include "MesgTask.h"
#include "CommunicateTask.h"
#include "tim.h"

RGB_t Light_RGBbuffer[Light_RGBbuffer_SIZE];
uint16_t Light_CRRbuffer[Light_CRRbuffer_SIZE];
Semaphore_t Light_Semaphore = {1};
Light_t Light;
BreathLight_t J1, J2, J3, J4, J5, J6;
Light_Handle_t Hole1, Hole2, Hole3, Hole4;
Light_Handle_t *HoleLightList[4] = {&Hole1, &Hole2, &Hole3, &Hole4};
BreathLight_t *BreathList[] = {&J1, &J2, &J3, &J4, &J5, &J6};
extern Setting_TypeDef Setting;
extern uint8_t sm16306s_data[2];
extern Scene_t Scene;
extern Tx_HandleTypeDef Tx3;

#define HOLE_BOOT_TEST_LIGHTNESS 5

void Light_Init(void)
{
    RGB_Init(&Light, &htim3, TIM_CHANNEL_2, Light_RGBbuffer_SIZE, Light_RGBbuffer, Light_CRRbuffer, &Light_Semaphore, GRB);
    BreathLight_Init(&J1, &htim5, TIM_CHANNEL_2, J1_GPIO_Port, J1_Pin);
    BreathLight_Init(&J2, &htim5, TIM_CHANNEL_3, J2_GPIO_Port, J2_Pin);
    BreathLight_Init(&J3, &htim5, TIM_CHANNEL_4, J3_GPIO_Port, J3_Pin);
    BreathLight_Init(&J4, &htim9, TIM_CHANNEL_1, J4_GPIO_Port, J4_Pin);
    BreathLight_Init(&J5, &htim9, TIM_CHANNEL_2, J5_GPIO_Port, J5_Pin);
    BreathLight_Init(&J6, &htim10, TIM_CHANNEL_1, J6_GPIO_Port, J6_Pin);
    RegisterLight(ColorLight, &Light);
    RegisterLight(BreathLight, &J1);
    RegisterLight(BreathLight, &J2);
    RegisterLight(BreathLight, &J3);
    RegisterLight(BreathLight, &J4);
    RegisterLight(BreathLight, &J5);
    RegisterLight(BreathLight, &J6);

    LightDerive_Init(&Hole1, &Light, 0, 7, (uint8_t *)&Setting.Board_Lightness);
    LightDerive_Init(&Hole2, &Light, 8, 15, (uint8_t *)&Setting.Board_Lightness);
    LightDerive_Init(&Hole3, &Light, 16, 23, (uint8_t *)&Setting.Board_Lightness);
    LightDerive_Init(&Hole4, &Light, 24, 31, (uint8_t *)&Setting.Board_Lightness);

    //  /* 开机时四个孔洞灯亮天蓝色 */
    // Light.Init = true;

    // LightEffect_Unblock_SetColor(
    //     &Light,
    //     0,
    //     Light_RGBbuffer_SIZE - 1,
    //     SKYBLUE,
    //     HOLE_BOOT_TEST_LIGHTNESS,
    //     255,
    //     false
    // );

    // /* 保持2秒 */
    // HAL_Delay(2000);

    // /* 熄灭孔洞灯 */
    // Light.Init = true;

    // LightEffect_Unblock_SetColor(
    //     &Light,
    //     0,
    //     Light_RGBbuffer_SIZE - 1,
    //     NONE,
    //     0,
    //     0,
    //     false
    // );

    // /* 等待最后一次WS2812数据发送完成 */
    // HAL_Delay(10);
}

/* 中文注释：安卓0x15只设置四路目标开关状态，统一由灯光任务刷新RGB。 */
static uint8_t HoleLightRefreshPending = 0U;

void HoleLight_Set(uint8_t light_number, uint8_t enabled)
{
    if (light_number < 1U || light_number > 4U || enabled > 1U)
        return;

    HoleLightList[light_number - 1U]->state = enabled;
    HoleLightRefreshPending = 1U;
}

/* 使用包含式end索引的灯光接口时，32颗灯的合法末尾索引为31 */
static void SettingSceneLight(void)
{
    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
}

static void IdleSceneLight(void)
{
    static uint8_t LightBoard_dir = 0;
    if (LightBoard_dir == 0)
    {
        LightEffect_Unblock_Flow(&Light, 0, Light_RGBbuffer_SIZE, NONE, WHITE, Setting.Board_Lightness, 255, 50, 6000, 0);
        if (Light.Finish == true)
            LightBoard_dir = 1;
    }
    if (LightBoard_dir == 1)
    {
        LightEffect_Unblock_Flow(&Light, 0, Light_RGBbuffer_SIZE, WHITE, NONE, Setting.Board_Lightness, 255, 50, 0, 0);
        if (Light.Finish == true)
            LightBoard_dir = 0;
    }
    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
}

static void HoolleInputSceneLight(void)
{
}

static void PublicRatioLight(void)
{
    LightEffect_Unblock_SetRand(&Light, 0, Light_RGBbuffer_SIZE - 1U, Setting.Board_Lightness, 255, 30);
    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
}

static void PlayingSceneLight(void)
{
    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
}

/* 中文注释：四组孔洞灯仅依据安卓指令状态统一刷新；DMA忙时保留请求等待下次任务。 */
static bool RefreshHoleLight(void)
{
    if (SemaphoreTake(Light.Semaphore) == false)
        return false;

    for (uint8_t i = 0; i < 4; i++)
    {
        if (HoleLightList[i]->state != 0)
        {
            RGB_SetMoreColor(&Light,
                             HoleLightList[i]->start,
                             HoleLightList[i]->end,
                             GREEN,
                             Setting.Board_Lightness,
                             255);
        }
        else
        {
            RGB_SetMoreColor(&Light,
                             HoleLightList[i]->start,
                             HoleLightList[i]->end,
                             NONE,
                             0,
                             0);
        }
    }

    RGB_Flush(&Light);
    Light.Init = false;
    HoleLightRefreshPending = 0U;
    return true;
}

static void VictorySceneLight(void)
{

    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
    LightEffect_Unblock_Blink(&Light, 0, Light_RGBbuffer_SIZE - 1U, YELLOW, Setting.Board_Lightness, 255, 100);
}

static void DefeatSceneLight(void)
{

    LightEffect_Unblock_Breath(&Light, 0, Light_RGBbuffer_SIZE - 1U, RED, Setting.Board_Lightness, 15, 2, Breath_LinearlyDiminish, false);
    for (uint8_t i = 0; i < 6; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, Setting.LightBelt_Lightness, 255);
}
void Light_Task(void)
{
    if (Scene == SCENE_SETTING)
        SettingSceneLight();
    // else if (Scene == SCENE_IDLE)
    //     IdleSceneLight();
    else if (Scene == SCENE_PLAYING)
        PlayingSceneLight();

    /* 中文注释：安卓灯光命令及亮度变更触发刷新，光眼和电磁阀不再改变灯状态。 */
    if (HoleLightRefreshPending != 0U || Light.Init == true)
        RefreshHoleLight();
}
