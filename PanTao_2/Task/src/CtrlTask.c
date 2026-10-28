#include "CtrlTask.h"
#include "MainTask.h"
#include "MesgTask.h"
#include "KeyTask.h"
#include "tim.h"
#include "port_device.h"
#include "port_event.h"
#include "string.h"

volatile Motor_Hoolle Motor_Hoolle1;
volatile Motor_Card Card;
Switch_Valve Lock_Valve, Valve;
servo_t Servo;

#define SERVO_RELEASE_TIME_MS 200U
#define SERVO_MANUAL_WAIT_MS 10000U
/* 中文注释：自动摆动独立功能开关，0关闭、1开启；不与版本号绑定。 */
#define SERVO_AUTO_SWING_ENABLE 0U
#define SERVO_AUTO_INTERVAL_MS 75U

/* 中文注释：等待安卓实际下发场景，避免默认游戏场景导致开机误动作。 */
static uint8_t ServoSceneReady = 0U;
static uint8_t ServoManualPending = 0U;
static uint8_t ServoPwmRunning = 0U;
static uint32_t ServoManualTick = 0U;
static uint32_t ServoLastControlTick = 0U;
extern Scene_t Scene;

extern Event_Handle_t Mesg_event;
extern Event_Handle_t Key_event;
extern uint8_t KeyState[4];
static inline uint32_t Get_SysTime(void)
{
    return HAL_GetTick();
}
/* 中文注释：所有有效角度动作使用同一PWM输出，空闲200ms后释放。 */
static void Servo_StartPwm(void)
{
    if (ServoPwmRunning == 0U)
    {
        HAL_TIM_PWM_Start(Servo.htim, Servo.channel);
        ServoPwmRunning = 1U;
    }
}

static void Servo_ReleasePwm(void)
{
    if (ServoPwmRunning != 0U &&
        (uint32_t)(Get_SysTime() - ServoLastControlTick) >= SERVO_RELEASE_TIME_MS)
    {
        HAL_TIM_PWM_Stop(Servo.htim, Servo.channel);
        ServoPwmRunning = 0U;
    }
}

/* 中文注释：只有收到安卓场景命令后，游戏场景才能启动自动摆动。 */
void Servo_SceneChanged(void)
{
    ServoSceneReady = 1U;
}

/* 中文注释：限位时旋钮继续旋转也要刷新手动优先10秒计时。 */
void Servo_ManualTurn(uint8_t direction)
{
    ServoManualTick = Get_SysTime();
    ServoManualPending = 1U;
    if (direction == 0U)
    {
        if (Servo.angle <= Servo.min_angle)
            return;
        Servo_StartPwm();
        Servo.DecreaseAngle(&Servo, 1U);
    }
    else
    {
        if (Servo.angle >= Servo.max_angle)
            return;
        Servo_StartPwm();
        Servo.IncreaseAngle(&Servo, 1U);
    }
    ServoLastControlTick = Get_SysTime();
}

/* 中文注释：安卓归零和编码器按下归零均在200ms后释放PWM。 */
void Servo_Reset(void)
{
    Servo_StartPwm();
    Servo.SetAngle(&Servo, 90U);
    ServoLastControlTick = Get_SysTime();
}

static void Ctrl_HoolleMotor(Motor_Hoolle *Motor, uint16_t speed, uint8_t dir, uint32_t timeout, uint32_t reverse_time, uint8_t retry_times, void (*Timeout_callbcak)(void))
{
    // 开机吐珠电机
    if (Motor->Motor.state == DEVICE_STATE_START)
    {
        Motor->Motor.SetSpeed(&Motor->Motor, speed, dir);
        Motor->Motor.state = DEVICE_STATE_BUSY;
        // Motor->RetryCount = 0;
    }
    // 停止吐珠电机
    if (Motor->Motor.state == DEVICE_STATE_STOP)
    {
        Motor->Motor.Stop(&Motor->Motor);
        Motor->Motor.state = DEVICE_STATE_IDLE;
        Motor->Hoolle_num = 0;
    }
    // 吐珠电机超时
    if (Motor->Motor.state == DEVICE_STATE_TIMEOUT)
    {
        // 反转时间到
        if (Motor->Motor.GetRuntime(&Motor->Motor) > HoolleMotorReverse_Time)
        {
            // 翻转次数不够，重新吐出
            if (Motor->RetryCount < retry_times)
            {
                Motor->Motor.state = DEVICE_STATE_START;
                Motor->RetryCount++;
            }
            else
            {
                Motor->Motor.state = DEVICE_STATE_IDLE;
                Motor->Motor.Stop(&Motor->Motor);
                // 超时停转后的反应
                Timeout_callbcak();
            }
        }
    }
    // 吐珠电机暂停
    if (Motor->Motor.state == DEVICE_STATE_PAUSE)
    {
        Motor->Motor.LosePower(&Motor->Motor);
        Motor->Motor.ResetRuntime(&Motor->Motor);
    }
    // 吐珠电机超时
    if (Motor->Motor.GetRuntime(&Motor->Motor) > timeout && Motor->Motor.state != DEVICE_STATE_IDLE)
    {
        Motor->Motor.state = DEVICE_STATE_TIMEOUT;
        Motor->Motor.LosePower(&Motor->Motor);
        HAL_Delay(1);
        // 反转
        Motor->Motor.SetSpeed(&Motor->Motor, speed, !dir);
    }
}

static void Ctrl_CardMotor(Motor_Card *Card, uint32_t timeout, void (*Timeout_callbcak)(void))
{

    /*==============卡片机控制===============*/
    // 开机吐卡
    if (Card->Switch.state == DEVICE_STATE_START)
    {
        Card->Switch.on(&Card->Switch);
        Card->Switch.state = DEVICE_STATE_BUSY;
    }
    // 停止吐卡
    if (Card->Switch.state == DEVICE_STATE_STOP)
    {
        Card->Switch.off(&Card->Switch);
        Card->Switch.state = DEVICE_STATE_IDLE;
        Card->Card_num = 0;
    }
    // 吐卡超时
    if (Card->Switch.state == DEVICE_STATE_TIMEOUT)
    {
        Card->Switch.off(&Card->Switch);
        Card->Switch.state = DEVICE_STATE_IDLE;
        // 吐卡超时反应
        Timeout_callbcak();
    }
    // 吐卡超时判断
    if (Card->Switch.GetRuntime(&Card->Switch) > CardMotorTimeout_time && Card->Switch.state != DEVICE_STATE_IDLE)
    {
        Card->Switch.state = DEVICE_STATE_TIMEOUT;
    }
}

/*==============电磁阀控制===============*/
static void Ctrl_Valve(Switch_Valve *Valve, uint32_t timeout, void (*Timeout_callbcak)(void))
{
    // 电磁阀启动
    if (Valve->Switch.state == DEVICE_STATE_START)
    {
        Valve->Switch.on(&Valve->Switch);
        Valve->Switch.state = DEVICE_STATE_BUSY;
    }
    // 电磁阀停止
    if (Valve->Switch.state == DEVICE_STATE_STOP)
    {
        Valve->Switch.off(&Valve->Switch);
        Valve->Switch.state = DEVICE_STATE_IDLE;
    }
    // 电磁阀超时
    if (Valve->Switch.state == DEVICE_STATE_TIMEOUT)
    {
        Valve->Switch.state = DEVICE_STATE_IDLE;
        Valve->Switch.off(&Valve->Switch);
        Timeout_callbcak();
    }
    // 电磁阀超时判断
    if (Valve->Switch.GetRuntime(&Valve->Switch) > timeout && Valve->Switch.state != DEVICE_STATE_IDLE)
    {
        Valve->Switch.state = DEVICE_STATE_TIMEOUT;
    }
}

static void HoolleMotorTimeout_callback(void)
{
    EventGroupSetBits(&Mesg_event, MesgEvent_HoolleOutputTimeout);
}

static void CardMotorTimeout_callback(void)
{
    EventGroupSetBits(&Mesg_event, MesgEvent_CardOutputTimeout);
}

static void ValveTimeout_callback(void)
{
    EventGroupClearBits(&Key_event, Event_AllHoleSwitchTrigger);
    Hoolle_Output(&Motor_Hoolle1, 0);
    /* 中文注释：四路完成后的状态复位不再操作孔洞RGB灯。 */
    memset(KeyState, 0, sizeof(KeyState));
}

static void Nothing_callback(void)
{
}

void Hoolle_Output(Motor_Hoolle *Motor, uint16_t num)
{
    Motor->Hoolle_num += num;
    if (Motor->Hoolle_num != 0)
    {
        Motor->Motor.state = DEVICE_STATE_START;
        Motor->Motor.runtick = Get_SysTime();
        Motor->RetryCount = 0;
    }
}

void Card_Output(Motor_Card *Switch, uint16_t num)
{
    Switch->Card_num += num;
    if (Switch->Card_num != 0)
    {
        Switch->Switch.state = DEVICE_STATE_START;
        Switch->Switch.runtick = Get_SysTime();
    }
}

void Device_Init(void)
{
    Device_Motor_Init(&Motor_Hoolle1.Motor, &htim1, TIM_CHANNEL_1, &htim1, TIM_CHANNEL_2);
    Device_Switch_Init(&Card.Switch, CardOutput_GPIO_Port, CardOutput_Pin, GPIO_PIN_SET);
    Device_Switch_Init(&Lock_Valve.Switch, Lock_Valve_GPIO_Port, Lock_Valve_Pin, GPIO_PIN_SET);
    Device_Switch_Init(&Valve.Switch, Valve_GPIO_Port, Valve_Pin, GPIO_PIN_SET);
    Device_Servo_Init(&Servo, &htim2, TIM_CHANNEL_1, 45, 135, 90);
    /* 中文注释：初始化时舵机PWM已启用，默认角度也适用200ms释放。 */
    ServoPwmRunning = 1U;
    ServoLastControlTick = Get_SysTime();

    HAL_TIM_Base_Start(&htim7);
    Motor_Hoolle1.Hoolle_num = 0;
    Motor_Hoolle1.RetryCount = 0;
    Card.Card_num = 0;
    Valve.Switch.state = DEVICE_STATE_START;
}

void Device_StopAllImmediately(void)
{
    /* 立即断开输出，不能只改state后马上复位。 */
    if (Motor_Hoolle1.Motor.LosePower != NULL)
        Motor_Hoolle1.Motor.LosePower((void *)&Motor_Hoolle1.Motor);
    Motor_Hoolle1.Motor.state = DEVICE_STATE_IDLE;
    Motor_Hoolle1.Hoolle_num = 0;
    Motor_Hoolle1.RetryCount = 0;

    if (Card.Switch.off != NULL)
        Card.Switch.off((void *)&Card.Switch);
    Card.Switch.state = DEVICE_STATE_IDLE;
    Card.Card_num = 0;
    Card.RetryCount = 0;

    if (Valve.Switch.off != NULL)
        Valve.Switch.off((void *)&Valve.Switch);
    Valve.Switch.state = DEVICE_STATE_IDLE;

    if (Lock_Valve.Switch.off != NULL)
        Lock_Valve.Switch.off((void *)&Lock_Valve.Switch);
    Lock_Valve.Switch.state = DEVICE_STATE_IDLE;

    if (Servo.htim != NULL)
        HAL_TIM_PWM_Stop(Servo.htim, Servo.channel);
    ServoPwmRunning = 0U;
}

void Servo_AutoRun(servo_t *Servo, uint32_t time)
{
    static uint32_t Time = 0U;
    static uint8_t dir = 0U;
    uint32_t time_now = Get_SysTime();

    if ((uint32_t)(time_now - Time) >= time)
    {
        Time = time_now;
        if (dir == 0U)
        {
            /* 中文注释：每次增加2度，到达上限时钳位并反向。 */
            if (Servo->angle + 2U >= Servo->max_angle)
            {
                Servo->angle = Servo->max_angle;
                dir = 1U;
            }
            else
                Servo->angle += 2U;
        }
        else
        {
            /* 中文注释：每次减少2度，到达下限时钳位并反向。 */
            if (Servo->angle <= Servo->min_angle + 2U)
            {
                Servo->angle = Servo->min_angle;
                dir = 0U;
            }
            else
                Servo->angle -= 2U;
        }

        /* 中文注释：自动摆动每75ms步进2度，并沿用原有PWM控制。 */
        Servo_StartPwm();
        Servo->SetAngle(Servo, Servo->angle);
        ServoLastControlTick = time_now;
    }
}

/* 中文注释：场景2、3、4自动摆动；玩家最后一次旋转后等待10秒。 */
static void Servo_AutoTask(void)
{
    if (ServoSceneReady == 0U ||
        ((uint8_t)Scene != 2U && (uint8_t)Scene != 3U && (uint8_t)Scene != 4U))
    {
        ServoManualPending = 0U;
        return;
    }

    if (ServoManualPending != 0U)
    {
        if ((uint32_t)(Get_SysTime() - ServoManualTick) < SERVO_MANUAL_WAIT_MS)
            return;
        ServoManualPending = 0U;
    }

    Servo_AutoRun(&Servo, SERVO_AUTO_INTERVAL_MS);
}
void CtrlTask(void)
{
    /*==============吐珠电机控制===============*/
    Ctrl_HoolleMotor(&Motor_Hoolle1, HoolleMotor_Speed, HoolleMotor_Dir, HoolleMotorTimeout_time, HoolleMotorReverse_Time, HoolleMotorRetry_Times, HoolleMotorTimeout_callback);
    /*==============卡片机控制===============*/
    Ctrl_CardMotor(&Card, CardMotorTimeout_time, CardMotorTimeout_callback);
    /*==============电磁阀控制===============*/
    Ctrl_Valve(&Valve, 2500, ValveTimeout_callback);
    Ctrl_Valve(&Lock_Valve, ValveTimeout_time, Nothing_callback);

    /* 中文注释：当前正式版关闭自动摆动，后续定制版本可通过开关启用。 */
    if (SERVO_AUTO_SWING_ENABLE != 0U)
        Servo_AutoTask();
    Servo_ReleasePwm();
}