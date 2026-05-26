/* main.c - GSM SMS Relay Controller - STM32L412 */

#include "main.h"
#include "usart.h"
#include "gpio.h"
#include <string.h>
#include <stdio.h>

/* ====================== Defines ====================== */
#define GSM_UART          (&huart3)
#define RS485_UART        (&huart1)
#define GSM_TIMEOUT       2000
#define MY_NUMBER         "+918102037997"

/* ====================== Forward Declarations ====================== */
void RS485_Send(const char *msg);
void GSM_Flush(void);
void GSM_SendCmd(const char *cmd, char *resp, uint16_t resp_size, uint32_t wait_ms);
void GSM_SendSMS(const char *number, const char *message);
void GSM_PowerOn(void);
void Activate_Relay(uint8_t relay_num);
void Process_Incoming_SMS(void);
void SystemClock_Config(void);
void Error_Handler(void);

/* ====================== RS485 ====================== */
void RS485_Send(const char *msg)
{
    HAL_GPIO_WritePin(RS485_TX_Pin_GPIO_Port, RS485_TX_Pin_Pin, GPIO_PIN_SET);
    HAL_Delay(5);
    HAL_UART_Transmit(RS485_UART, (uint8_t*)msg, strlen(msg), 300);
    HAL_Delay(5);
    HAL_GPIO_WritePin(RS485_TX_Pin_GPIO_Port, RS485_TX_Pin_Pin, GPIO_PIN_RESET);
}

/* ====================== GSM Flush (CRITICAL FIX) ====================== */
/*
 * Clears the UART Overrun Error (ORE) flag and discards any leftover
 * bytes sitting in the UART receive register.
 * Must be called before any HAL_UART_Receive, otherwise ORE causes
 * the receive to return HAL_ERROR immediately with empty buffer.
 */
void GSM_Flush(void)
{
    uint8_t dummy;
    /* Clear all UART error flags */
    __HAL_UART_CLEAR_OREFLAG(GSM_UART);
    __HAL_UART_CLEAR_FEFLAG(GSM_UART);
    __HAL_UART_CLEAR_NEFLAG(GSM_UART);
    /* Drain any bytes already waiting in the hardware register */
    while (HAL_UART_Receive(GSM_UART, &dummy, 1, 10) == HAL_OK);
}

/* ====================== GSM Send + Receive ====================== */
void GSM_SendCmd(const char *cmd, char *resp, uint16_t resp_size, uint32_t wait_ms)
{
    GSM_Flush();  /* Always flush before sending â€” clears ORE flag */

    if (resp && resp_size > 0)
        memset(resp, 0, resp_size);

    HAL_UART_Transmit(GSM_UART, (uint8_t*)cmd, strlen(cmd), GSM_TIMEOUT);

    if (resp && resp_size > 0)
        HAL_UART_Receive(GSM_UART, (uint8_t*)resp, resp_size - 1, wait_ms);
    else
        HAL_Delay(wait_ms);
}

/* ====================== GSM Send SMS ====================== */
void GSM_SendSMS(const char *number, const char *message)
{
    char cmd[64];
    char tmp[64];

    GSM_SendCmd("AT+CMGF=1\r\n", tmp, sizeof(tmp), 500);
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", number);
    GSM_SendCmd(cmd, tmp, sizeof(tmp), 500);

    HAL_UART_Transmit(GSM_UART, (uint8_t*)message, strlen(message), GSM_TIMEOUT);
    HAL_Delay(200);

    uint8_t ctrlz = 0x1A;
    HAL_UART_Transmit(GSM_UART, &ctrlz, 1, GSM_TIMEOUT);
    HAL_Delay(5000);  /* Wait for SMS to send */
}

/* ====================== GSM Power On ====================== */
/*
 * NOTE: If "System Starting..." appears twice on TeraTerm, the STM32
 * is brown-out resetting due to GSM module peak current (~2A).
 * Hardware fix: add a 1000uF + 100nF capacitor on the GSM module VCC rail.
 * Software workaround: we check if the module is already alive before
 * doing the full power-on sequence.
 */
void GSM_PowerOn(void)
{
    char resp[64];

    /* Check if module is already ON (e.g. after a brownout reset) */
    GSM_SendCmd("AT\r\n", resp, sizeof(resp), 1000);
    if (strstr(resp, "OK"))
    {
        RS485_Send("GSM already ON - skipping power cycle\r\n");
        return;
    }

    /* Module not responding â€” do full power-on sequence */
    RS485_Send("GSM Power On...\r\n");
    HAL_GPIO_WritePin(GSM_RESET_GPIO_Port, GSM_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GSM_PWRKEY_GPIO_Port, GSM_PWRKEY_Pin, GPIO_PIN_SET);
    HAL_Delay(1500);
    HAL_GPIO_WritePin(GSM_PWRKEY_GPIO_Port, GSM_PWRKEY_Pin, GPIO_PIN_RESET);
    HAL_Delay(1500);
    HAL_GPIO_WritePin(GSM_PWRKEY_GPIO_Port, GSM_PWRKEY_Pin, GPIO_PIN_SET);
    HAL_Delay(20000);  /* Wait for network registration */
}

/* ====================== Relay Activation ====================== */
void Activate_Relay(uint8_t relay_num)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0;

    switch (relay_num)
    {
        case 1: port = Relay_1_GPIO_Port; pin = Relay_1_Pin; break;
        case 2: port = Relay_2_GPIO_Port; pin = Relay_2_Pin; break;
        case 3: port = Relay_3_GPIO_Port; pin = Relay_3_Pin; break;
        case 4: port = Relay_4_GPIO_Port; pin = Relay_4_Pin; break;
        default: return;
    }

    char msg[60];
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    snprintf(msg, sizeof(msg), "Relay %d ACTIVATED for 3 seconds\r\n", relay_num);
    RS485_Send(msg);

    HAL_Delay(3000);

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    snprintf(msg, sizeof(msg), "Relay %d DEACTIVATED\r\n", relay_num);
    RS485_Send(msg);
}

/* ====================== SMS Processing ====================== */
void Process_Incoming_SMS(void)
{
    char buffer[512] = {0};

    /* Send CMGL and immediately read the response */
    GSM_SendCmd("AT+CMGL=\"ALL\"\r\n", buffer, sizeof(buffer), 3000);

    /* No messages found â€” exit early */
    if (strstr(buffer, "+CMGL:") == NULL)
        return;

    RS485_Send("=== SMS FOUND ===\r\n");
    RS485_Send(buffer);
    RS485_Send("\r\n");

    /* Detect relay command (case-insensitive) */
    uint8_t relay = 0;
    if      (strstr(buffer, "relay 1") || strstr(buffer, "Relay 1") || strstr(buffer, "RELAY 1")) relay = 1;
    else if (strstr(buffer, "relay 2") || strstr(buffer, "Relay 2") || strstr(buffer, "RELAY 2")) relay = 2;
    else if (strstr(buffer, "relay 3") || strstr(buffer, "Relay 3") || strstr(buffer, "RELAY 3")) relay = 3;
    else if (strstr(buffer, "relay 4") || strstr(buffer, "Relay 4") || strstr(buffer, "RELAY 4")) relay = 4;

    if (relay > 0)
    {
        char log[48];
        char sms_reply[48];
        snprintf(sms_reply, sizeof(sms_reply), "Relay %d activated for 3 seconds", relay);
        snprintf(log, sizeof(log), "Relay %d Command Executed\r\n", relay);

        Activate_Relay(relay);
        GSM_SendSMS(MY_NUMBER, sms_reply);
        RS485_Send(log);
    }
    else
    {
        RS485_Send("Unknown SMS command received\r\n");
        GSM_SendSMS(MY_NUMBER, "Unknown command. Send: relay 1 / relay 2 / relay 3 / relay 4");
    }

    /* Delete all messages to prevent re-triggering on next poll */
    GSM_SendCmd("AT+CMGD=1,4\r\n", NULL, 0, 1000);
}

/* ====================== Main ====================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART3_UART_Init();
    MX_USART1_UART_Init();

    HAL_GPIO_WritePin(RS485_TX_Pin_GPIO_Port, RS485_TX_Pin_Pin, GPIO_PIN_RESET);

    RS485_Send("System Starting...\r\n");

    GSM_PowerOn();

    /* Init AT commands â€” use tmp buffer to consume responses and avoid ORE */
    char tmp[64];
    GSM_SendCmd("ATE0\r\n",         tmp, sizeof(tmp), 500);
    GSM_SendCmd("AT+CMGF=1\r\n",   tmp, sizeof(tmp), 500);
    GSM_SendCmd("AT+CMGD=1,4\r\n", tmp, sizeof(tmp), 1000);  /* Clear old messages */

    RS485_Send("GSM Ready - Send 'relay 1/2/3/4'\r\n");

    /* Notify user that the system is ready */
    GSM_SendSMS(MY_NUMBER, "System Ready! Send: relay 1 / relay 2 / relay 3 / relay 4");
    RS485_Send("Startup SMS sent\r\n");

    while (1)
    {
        Process_Incoming_SMS();
        HAL_Delay(3000);
    }
}

/* ====================== System Clock ====================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_MSI;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
        Error_Handler();
}

/* ====================== Error Handler ====================== */
void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}
#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
Â Â /* User can add his own implementation to report the file name and line number,
Â Â Â Â Â ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
