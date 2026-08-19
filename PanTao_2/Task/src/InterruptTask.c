#include "InterruptTask.h"
#include "CtrlTask.h"
#include "MesgTask.h"
#include "MainTask.h"
#include "LightTask.h"
#include "CommunicateTask.h"
#include "KeyTask.h"
#include "tim.h"
#include "usart.h"
#include "stdio.h"

#define Mesg_Head 0xAA
#define Mesg_Tail 0x55

extern Event_Handle_t Mesg_event;
extern Motor_Hoolle Motor_Hoolle1;
extern Motor_Card Card;
extern Switch_Valve Lock_Valve, Valve;
extern uint8_t LightBoard_Lightness;
extern uint8_t LightBelt_Lightness;
extern Scene_t Scene;
extern Light_t Light;
extern Event_Handle_t Key_event;
extern Rx_HandleTypeDef Rx1;
extern Rx_HandleTypeDef Rx3;

/* UART发生ORE/FE/NE错误时恢复中断接收，避免串口异常后永久停止接收 */
static void UART_RxErrorRecover(UART_HandleTypeDef *huart, Rx_HandleTypeDef *rx)
{
    uint32_t error_code;

    if (huart == NULL || rx == NULL)
        return;

    error_code = huart->ErrorCode;
    if ((error_code & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE | HAL_UART_ERROR_NE)) == 0U)
        return;

    /* STM32F4通过先读SR再读DR清除ORE/FE/NE错误标志 */
    if ((error_code & HAL_UART_ERROR_ORE) != 0U)
        __HAL_UART_CLEAR_OREFLAG(huart);
    if ((error_code & HAL_UART_ERROR_FE) != 0U)
        __HAL_UART_CLEAR_FEFLAG(huart);
    if ((error_code & HAL_UART_ERROR_NE) != 0U)
        __HAL_UART_CLEAR_NEFLAG(huart);

    /*
     * ORE会使HAL结束当前接收并把RxState恢复为READY；
     * FE/NE时接收可能仍在继续，因此只在READY状态重新启动接收，避免重复挂接。
     */
    if (huart->RxState == HAL_UART_STATE_READY)
    {
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        (void)HAL_UART_Receive_IT(huart, &rx->Handle.temp_data, 1);
    }
}

static void HoolleInput_IRQ(void)
{
    // EventGroupSetBits(&Mesg_event, MesgEvent_HoolleInput);
}

static void CoinInput_IRQ(void)
{
    // EventGroupSetBits(&Mesg_event, MesgEvent_CoinInput);
}

static void HoolleOutput_IRQ(void)
{
    if (HAL_GPIO_ReadPin(HoolleOutput_GPIO_Port, HoolleOutput_Pin) == GPIO_PIN_RESET)
    {
        /* 脉冲开始：重置计时器并重置运行时 */
        __HAL_TIM_SetCounter(&htim7, 0);
        Motor_Hoolle1.Motor.ResetRuntime(&Motor_Hoolle1.Motor);
        return;
    }
    else
    {
        if (__HAL_TIM_GetCounter(&htim7) > 100)
        {
            /* 有效脉冲：更新剩余数量并通知任务 */
            EventGroupSetBits(&Mesg_event, MesgEvent_RemainingHoolle);
            if (Motor_Hoolle1.Hoolle_num > 0)
            {
                Motor_Hoolle1.Hoolle_num--;
                Motor_Hoolle1.RetryCount = 0;
                if (Motor_Hoolle1.Hoolle_num == 0 && Motor_Hoolle1.Motor.state != DEVICE_STATE_IDLE)
                {
                    Motor_Hoolle1.Motor.state = DEVICE_STATE_STOP;
                }
            }
        }
    }
}

static void CardOutput_IRQ(void)
{
    Card.Switch.ResetRuntime(&Card.Switch);
    if (Card.Card_num > 0)
    {
        Card.Card_num--;
        EventGroupSetBits(&Mesg_event, MesgEvent_RemainingCard);
        if (Card.Card_num <= 0 && Card.Switch.state != DEVICE_STATE_IDLE)
        {
            EventGroupSetBits(&Mesg_event, MesgEvent_ClearRemainMesg);
            Card.Switch.state = DEVICE_STATE_STOP;
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    switch (GPIO_Pin)
    {
    case HoolleOutput_Pin:
        HoolleOutput_IRQ();
        break;
    case CardFeedback_Pin:
        CardOutput_IRQ();
        break;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == Rx1.Handle.huart)
    {
        Rx1.Handle.RingBuf.f_WriteByte(&Rx1.Handle.RingBuf, Rx1.Handle.temp_data);
        HAL_UART_Receive_IT(huart, &Rx1.Handle.temp_data, 1);
    }
    if (huart == Rx3.Handle.huart)
    {
        Rx3.Handle.RingBuf.f_WriteByte(&Rx3.Handle.RingBuf, Rx3.Handle.temp_data);
        HAL_UART_Receive_IT(huart, &Rx3.Handle.temp_data, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == Rx1.Handle.huart)
    {
        UART_RxErrorRecover(huart, &Rx1);
    }
    else if (huart == Rx3.Handle.huart)
    {
        UART_RxErrorRecover(huart, &Rx3);
    }
}
