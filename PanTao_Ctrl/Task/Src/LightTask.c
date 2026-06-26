#include "LightTask.h"
#include "MainTask.h"
#include "FlashTask.h"
#include "app_list.h"
#include "CommTask.h"
#include "tim.h"
#include "stdlib.h"

static RGB_t Light1_RGBbuffer[Light1_RGBbuffer_SIZE];
static uint8_t Light1_CRRbuffer[Light1_CRRbuffer_SIZE];
static RGB_t Light2_RGBbuffer[Light2_RGBbuffer_SIZE];
static uint8_t Light2_CRRbuffer[Light2_CRRbuffer_SIZE];

uint8_t LightBelt_Lightness = 5;
uint8_t LightBoard_Lightness = 5;
Semaphore_t Light1_Semaphore, Light2_Semaphore = {1};
Light_t Light1, Light2;
BreathLight_t J1, J2;
BreathLight_t *BreathList[] = {&J1, &J2};

extern Tx_HandleTypeDef Tx1;
extern Scene_t Scene;

typedef struct
{
    Light_t *Light;
    uint8_t Start;
    uint8_t End;
} CtrlLightGroupMap_t;

typedef struct
{
    uint8_t Color;
    uint8_t Mode;
    uint8_t BlinkOn;
    uint8_t Dirty;
    uint8_t Enabled;
    uint32_t LastToggleTime;
} CtrlLightGroupState_t;

/*
 * 位置编号按控台功能区域划分：
 * 0x00：孙悟空升级按键；用户当前协议同时将游戏说明按键定义为0x00；
 * 0x01：猪八戒升级按键；0x02：沙悟净升级按键；
 * 0x03：孙悟空发射按键；0x04：猪八戒发射按键；
 * 0x05：沙悟净发射按键；0x06：右上四联按键组（业务名称未指定）；
 * 0x07：旋钮按键；0x08：数码管按键。
 */
static const CtrlLightGroupMap_t CtrlLightGroupMap[CTRL_LIGHT_GROUP_NUM] =
{
    {&Light1, 18, 25}, /* 0x00：孙悟空升级 / 游戏说明（当前协议共用） */
    {&Light1, 26, 33}, /* 0x01：猪八戒升级 */
    {&Light1, 34, 41}, /* 0x02：沙悟净升级 */
    {&Light1, 42, 49}, /* 0x03：孙悟空发射 */
    {&Light1, 50, 57}, /* 0x04：猪八戒发射 */
    {&Light1, 58, 65}, /* 0x05：沙悟净发射 */
    {&Light1, 0, 17},  /* 0x06：右上四联按键组（业务名称未指定） */
    {&Light2, 8, 15},  /* 0x07：旋钮按键 */
    {&Light2, 0, 7},   /* 0x08：数码管按键 */
};

static CtrlLightGroupState_t CtrlLightGroupState[CTRL_LIGHT_GROUP_NUM];
static uint8_t Light1RefreshPending = 0U;
static uint8_t Light2RefreshPending = 0U;
static uint8_t GlobalSceneForceRefresh = 1U;
static uint8_t GlobalBreathColor = 0U;
static uint8_t GlobalBreathDirection = 0U;
static uint16_t GlobalBreathLevel = 0U;
static uint32_t GlobalBreathLastTime = 0U;

static void SetAllWs2812Color(RGB_t color, uint8_t relative_lightness)
{
    RGB_SetMoreColor(
        &Light1,
        0,
        Light1_RGBbuffer_SIZE - 1,
        color,
        LightBoard_Lightness,
        relative_lightness
    );

    RGB_SetMoreColor(
        &Light2,
        0,
        Light2_RGBbuffer_SIZE - 1,
        color,
        LightBoard_Lightness,
        relative_lightness
    );

    Light1RefreshPending = 1U;
    Light2RefreshPending = 1U;
}

static void ResetGlobalBreath(void)
{
    GlobalBreathColor = 0U;
    GlobalBreathDirection = 0U;
    GlobalBreathLevel = 0U;
    GlobalBreathLastTime = HAL_GetTick() - GLOBAL_BREATH_STEP_TIME;
}

void CtrlLightGroup_ClearAll(void)
{
    for (uint8_t i = 0; i < CTRL_LIGHT_GROUP_NUM; i++)
    {
        CtrlLightGroupState[i].Enabled = 0U;
        CtrlLightGroupState[i].Dirty = 0U;
        CtrlLightGroupState[i].BlinkOn = 0U;
    }

    /* 全局场景指令优先：即使场景编号没有变化，也重新覆盖全部灯珠。 */
    GlobalSceneForceRefresh = 1U;

    if (Scene == SCENE_LESSLIGHT)
        ResetGlobalBreath();
}

bool CtrlLightGroup_Set(uint8_t position, uint8_t color, uint8_t mode)
{
    CtrlLightGroupState_t *state;

    if (position >= CTRL_LIGHT_GROUP_NUM || color >= CTRL_LIGHT_COLOR_NUM)
        return false;

    if (mode != CTRL_LIGHT_MODE_ON &&
        mode != CTRL_LIGHT_MODE_OFF &&
        mode != CTRL_LIGHT_MODE_BLINK)
        return false;

    state = &CtrlLightGroupState[position];
    state->Color = color;
    state->Mode = mode;
    state->BlinkOn = (mode == CTRL_LIGHT_MODE_OFF) ? 0U : 1U;
    state->Dirty = 1U;
    state->Enabled = 1U;
    state->LastToggleTime = HAL_GetTick();

    return true;
}

static void CtrlLightGroup_Task(void)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t i = 0; i < CTRL_LIGHT_GROUP_NUM; i++)
    {
        CtrlLightGroupState_t *state = &CtrlLightGroupState[i];
        const CtrlLightGroupMap_t *map = &CtrlLightGroupMap[i];
        uint8_t global_refresh;
        RGB_t color;

        if (state->Enabled == 0U)
            continue;

        if (state->Mode == CTRL_LIGHT_MODE_BLINK &&
            (uint32_t)(now - state->LastToggleTime) >= CTRL_LIGHT_BLINK_HALF_PERIOD)
        {
            state->LastToggleTime = now;
            state->BlinkOn = !state->BlinkOn;
            state->Dirty = 1U;
        }

        global_refresh = (map->Light == &Light1) ? Light1RefreshPending : Light2RefreshPending;
        if (state->Dirty == 0U && global_refresh == 0U)
            continue;

        if (state->Mode == CTRL_LIGHT_MODE_OFF ||
            (state->Mode == CTRL_LIGHT_MODE_BLINK && state->BlinkOn == 0U))
            color = NONE;
        else
            color = Color_table[state->Color];

        RGB_SetMoreColor(
            map->Light,
            map->Start,
            map->End,
            color,
            LightBoard_Lightness,
            255
        );

        if (map->Light == &Light1)
            Light1RefreshPending = 1U;
        else
            Light2RefreshPending = 1U;

        state->Dirty = 0U;
    }
}

void Light_Init(void)
{
    RGB_Init(&Light1, &htim3, TIM_CHANNEL_1, Light1_RGBbuffer_SIZE, Light1_RGBbuffer, Light1_CRRbuffer, &Light1_Semaphore, RGB);
    RGB_Init(&Light2, &htim3, TIM_CHANNEL_4, Light2_RGBbuffer_SIZE, Light2_RGBbuffer, Light2_CRRbuffer, &Light2_Semaphore, RGB);
    BreathLight_Init(&J1, &htim1, TIM_CHANNEL_1, GPIOA, GPIO_PIN_8);
    BreathLight_Init(&J2, &htim1, TIM_CHANNEL_2, GPIOA, GPIO_PIN_9);
    RegisterLight(ColorLight, &Light1);
    RegisterLight(ColorLight, &Light2);
    RegisterLight(BreathLight, &J1);
    RegisterLight(BreathLight, &J2);

    RGB_SetMoreColor(&Light1, 0, Light1_RGBbuffer_SIZE - 1, NONE, 0, 0);
    RGB_SetMoreColor(&Light2, 0, Light2_RGBbuffer_SIZE - 1, NONE, 0, 0);
    RGB_Flush(&Light1);
    RGB_Flush(&Light2);
}

/* 场景0：全部WS2812依次使用九种颜色进行呼吸亮灭。 */
static void LesslightSceneLight(void)
{
    uint32_t now = HAL_GetTick();

    if (GlobalSceneForceRefresh == 0U &&
        (uint32_t)(now - GlobalBreathLastTime) < GLOBAL_BREATH_STEP_TIME)
        return;

    GlobalSceneForceRefresh = 0U;
    GlobalBreathLastTime = now;

    SetAllWs2812Color(
        Color_table[GlobalBreathColor],
        (uint8_t)GlobalBreathLevel
    );

    if (GlobalBreathDirection == 0U)
    {
        if (GlobalBreathLevel < 255U)
            GlobalBreathLevel++;
        else
            GlobalBreathDirection = 1U;
    }
    else
    {
        if (GlobalBreathLevel > 0U)
            GlobalBreathLevel--;
        else
        {
            GlobalBreathDirection = 0U;
            GlobalBreathColor++;
            if (GlobalBreathColor >= CTRL_LIGHT_COLOR_NUM)
                GlobalBreathColor = 0U;
        }
    }

    for (uint8_t i = 0; i < 2; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, LightBelt_Lightness, 255);
}

/* 场景1：全部WS2812常亮白色。 */
static void IdleSceneLight(void)
{
    if (GlobalSceneForceRefresh != 0U)
    {
        SetAllWs2812Color(WHITE, 255);
        GlobalSceneForceRefresh = 0U;
    }

    for (uint8_t i = 0; i < 2; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, LightBelt_Lightness, 255);
}

/* 场景2：全部WS2812常亮红色。 */
static void PlayingSceneLight(void)
{
    if (GlobalSceneForceRefresh != 0U)
    {
        SetAllWs2812Color(RED, 255);
        GlobalSceneForceRefresh = 0U;
    }

    for (uint8_t i = 0; i < 2; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, LightBelt_Lightness, 255);
}

/* 场景3：全部WS2812常灭。 */
static void VictorySceneLight(void)
{
    if (GlobalSceneForceRefresh != 0U)
    {
        SetAllWs2812Color(NONE, 255);
        GlobalSceneForceRefresh = 0U;
    }

    for (uint8_t i = 0; i < 2; i++)
        BreathLight_SetLightKeep(BreathList[i], 0, LightBelt_Lightness, 255);
}

void Light_Task(void)
{
    static Scene_t last_scene = (Scene_t)0xFF;
    static uint8_t last_lightness = 0xFFU;

    if (Scene != last_scene)
    {
        last_scene = Scene;
        GlobalSceneForceRefresh = 1U;

        if (Scene == SCENE_LESSLIGHT)
            ResetGlobalBreath();
    }

    if (LightBoard_Lightness != last_lightness)
    {
        last_lightness = LightBoard_Lightness;
        GlobalSceneForceRefresh = 1U;
    }

    if (Scene == SCENE_LESSLIGHT)
        LesslightSceneLight();
    else if (Scene == SCENE_IDLE)
        IdleSceneLight();
    else if (Scene == SCENE_PLAYING)
        PlayingSceneLight();
    else if (Scene == SCENE_VICTORY)
        VictorySceneLight();

    /* 分组灯覆盖全局场景；未启用的分组继续保留全局灯效。 */
    CtrlLightGroup_Task();

    if (Light1RefreshPending != 0U)
    {
        RGB_Flush(&Light1);
        Light1RefreshPending = 0U;
    }

    if (Light2RefreshPending != 0U)
    {
        RGB_Flush(&Light2);
        Light2RefreshPending = 0U;
    }
}
