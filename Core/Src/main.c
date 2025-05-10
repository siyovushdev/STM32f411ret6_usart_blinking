// Target: STM32F411RE (Nucleo-F411RE)
// Toolchain: CMSIS v1.28.1, CMSIS-RTOS2

#include "stm32f411xe.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdbool.h>

#define UART_RX_QUEUE_LEN 64
#define UART_TX_BUFFER_LEN 128

// RTOS objects
osThreadId_t UartTaskHandle;
osThreadId_t BlinkingTaskHandle;
osMessageQueueId_t UartRxQueue;
osMessageQueueId_t BlinkingQueue;
osEventFlagsId_t UartTxEventFlags;
#define TX_DONE_FLAG (1 << 0)

char txBuffer[UART_TX_BUFFER_LEN];
volatile uint16_t txIndex = 0;
volatile uint16_t txLength = 0;

void SystemClock_Config(void);

void GPIO_Init(void);

void USART2_Init(void);

void UartTask(void *argument);

void BlinkingTask(void *argument);

void USART2_SendStringAsync(const char *str);

int main(void) {
    SystemClock_Config();
    GPIO_Init();
    USART2_Init();

    osKernelInitialize();

    UartRxQueue = osMessageQueueNew(UART_RX_QUEUE_LEN, sizeof(uint8_t), NULL);
    BlinkingQueue = osMessageQueueNew(16, sizeof(uint8_t), NULL);
    UartTxEventFlags = osEventFlagsNew(NULL);
    UartTaskHandle = osThreadNew(UartTask, NULL, NULL);
    BlinkingTaskHandle = osThreadNew(BlinkingTask, NULL, NULL);

    osKernelStart();
    while (1) {
        GPIOA->ODR ^= GPIO_ODR_OD5;
        for (int i = 0; i < 1000000; i++);
    }
}

void GPIO_Init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN;

    // PA2 (USART2_TX), PA3 (USART2_RX)
    GPIOA->MODER &= ~((3U << GPIO_MODER_MODER2_Pos) | (3U << GPIO_MODER_MODER3_Pos));
    GPIOA->MODER |= (2U << GPIO_MODER_MODER2_Pos) | (2U << GPIO_MODER_MODER3_Pos); // Alternate function
    GPIOA->AFR[0] &= ~((0xF << GPIO_AFRL_AFSEL2_Pos) | (0xF << GPIO_AFRL_AFSEL3_Pos));
    GPIOA->AFR[0] |= (7U << GPIO_AFRL_AFSEL2_Pos) | (7U << GPIO_AFRL_AFSEL3_Pos); // AF7 (USART2)

    // PC13 — LED
    GPIOA->MODER &= ~(3U << GPIO_MODER_MODER5_Pos);
    GPIOA->MODER |= (1U << GPIO_MODER_MODER5_Pos); // Output
    GPIOA->ODR |= GPIO_ODR_OD5; // LED OFF (active low)
}

void USART2_Init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    USART2->BRR = SystemCoreClock / 115200;
    USART2->CR1 |= USART_CR1_TE | USART_CR1_RE;
    USART2->CR1 |= USART_CR1_RXNEIE;
    USART2->CR1 |= USART_CR1_UE;

    NVIC_SetPriority(USART2_IRQn, 6);
    NVIC_EnableIRQ(USART2_IRQn);
}

void BlinkingTask(void *argument) {
    uint8_t cmd = 0;
    for (;;) {
        GPIOA->ODR ^= GPIO_ODR_OD5;
        osDelay(100);
        if (osMessageQueueGet(BlinkingQueue, &cmd, NULL, osWaitForever) == osOK) {
            if (cmd == 0xFF) {
                while (1) {
                    GPIOA->ODR ^= GPIO_ODR_OD5;
                    osDelay(100);
                    if (osMessageQueueGet(BlinkingQueue, &cmd, NULL, 0) == osOK && cmd == 0xFE)
                        break;
                }
                GPIOA->ODR |= GPIO_ODR_OD5; // LED OFF
            }
        }
    }
}

void UartTask(void *argument) {
    uint8_t cmd;
    while (1) {
        if (osMessageQueueGet(UartRxQueue, &cmd, NULL, osWaitForever) == osOK) {
            osMessageQueuePut(BlinkingQueue, (uint8_t[]){0xFF}, 0, 0);

            const char *msg = NULL;
            if (cmd == '1') msg = "Hello, STM32F411!\r\n";
            else if (cmd == '0') msg = "Goodbye from STM32F411!\r\n";
            else msg = "Unknown command!\r\n";

            USART2_SendStringAsync(msg);
            osEventFlagsWait(UartTxEventFlags, TX_DONE_FLAG, osFlagsWaitAny, osWaitForever);
            osMessageQueuePut(BlinkingQueue, (uint8_t[]){0xFE}, 0, 0);
        }
    }
}

void USART2_SendStringAsync(const char *str) {
    while (txLength);
    txLength = strlen(str);
    memcpy((char *) txBuffer, str, txLength);
    txIndex = 0;
    USART2->CR1 |= USART_CR1_TXEIE;
}

void USART2_IRQHandler(void) {
    if ((USART2->SR & USART_SR_RXNE) && (USART2->CR1 & USART_CR1_RXNEIE)) {
        uint8_t byte = USART2->DR;
        osMessageQueuePut(UartRxQueue, &byte, 0, 0);
    }
    if ((USART2->SR & USART_SR_TXE) && (USART2->CR1 & USART_CR1_TXEIE)) {
        if (txIndex < txLength) {
            USART2->DR = txBuffer[txIndex++];
        } else {
            USART2->CR1 &= ~USART_CR1_TXEIE;
            USART2->CR1 |= USART_CR1_TCIE;
        }
    }
    if ((USART2->SR & USART_SR_TC) && (USART2->CR1 & USART_CR1_TCIE)) {
        USART2->CR1 &= ~USART_CR1_TCIE;
        txLength = 0;
        osEventFlagsSet(UartTxEventFlags, TX_DONE_FLAG);
    }
}

void SystemClock_Config(void) {
    // RCC->CR |= RCC_CR_HSEON;
    // while (!(RCC->CR & RCC_CR_HSERDY));
    //
    // RCC->PLLCFGR = (RCC_PLLCFGR_PLLSRC_HSE | (8 << RCC_PLLCFGR_PLLM_Pos) |
    //                 (336 << RCC_PLLCFGR_PLLN_Pos) | (0 << RCC_PLLCFGR_PLLP_Pos) |
    //                 (7 << RCC_PLLCFGR_PLLQ_Pos));
    //
    // RCC->CR |= RCC_CR_PLLON;
    // while (!(RCC->CR & RCC_CR_PLLRDY));
    //
    // FLASH->ACR |= FLASH_ACR_LATENCY_5WS;
    // RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    // RCC->CFGR |= RCC_CFGR_SW_PLL;
    // while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

    SystemCoreClockUpdate();
}
