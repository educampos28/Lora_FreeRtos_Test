/*
 * This file is part of the µOS++ distribution.
 *   (https://github.com/micro-os-plus)
 * Copyright (c) 2014 Liviu Ionescu.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
// ----------------------------------------------------------------------------
//
// Semihosting STM32F1  (trace via DEBUG).
//
// ----------------------------------------------------------------------------


/*******************************************************************************
 * File Name      : main.cpp
 * description    : That program manages the packages received from I2C, two
 * 					Lora modules working at 915MHz, and one Lora module working at
 * 					2.4GHz by using FreeRtos. The tasks are organized to do:
 *
 * 					Any data received from I2C will be placed into a queue for the Lora@915MHz.
 *					One module Lora@915MHz will receive any package, and it will be placed into the
 *					queue for sending back throughout Lora@915MHz and Lora@2.4GHz.
 *
 *
 * Author	      : Eduardo Lacerda Campos
 * Date		      : Feb 17th 2020
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"

#include "timers.h"
#include "cmsis_device.h"

//#include "Timer.h"
#include "BlinkLed.h"

#include "Hard/USART.h"
#include "Hard/Timer.h"
#include "Hard/I2CSlave.h"
#include "Hard/SPI.h"
#include "LORA.h"
#include "Lora_E28.h"
#include "My_Macros.h"


// ----------------------------------------------------------------------------
// Program define
// ----------------------------------------------------------------------------

//High number means high priority
#define I2CSLAVE_TASK_PRIORITY  ( tskIDLE_PRIORITY + 3)
#define LORA_TASK_PRIORITY		( tskIDLE_PRIORITY + 2)
#define LORA_2400_TASK_PRIORITY	( tskIDLE_PRIORITY + 1 )
#define LED_TASK_PRIORITY		( tskIDLE_PRIORITY)

//Used to debug throughout GDB
volatile int __attribute__((used)) uxTopUsedPriority  = configMAX_PRIORITIES-1;
#define configTASK_RETURN_ADDRESS   0  /* return address of task is zero */
volatile __attribute__((used)) unsigned long ulHighFrequencyTimerTicks = 0;

//Local defines
#define LORA_TASK_OK 1
#define I2C_TASK_OK 2


// ----------------------------------------------------------------------------
//	Global variables
// ----------------------------------------------------------------------------

BlinkLed 	blinkLed;
USART 		USART_COM1;
I2CSlave 	I2C;
Timer		Timer2;
SPI 		spi;

//to distinguish between different lora modules
//the mean frequency was used in their names
SemtechSX1280::RadioCallbacks_t CallBack2450;
SemtechSX1280::LORAE28	Lora2450(&CallBack2450);

SemtechSX126X::RadioCallbacks_t CallBack;
SemtechSX126X::LORA	Lora915M1(&CallBack);
SemtechSX126X::LORA	Lora915M2(&CallBack);

EventGroupHandle_t xBootEventGroup= NULL;
QueueHandle_t xQueue_Lora915;
QueueHandle_t xQueue_Lora2450;
SemaphoreHandle_t xSemaphoreLora2450 = NULL;
SemaphoreHandle_t xSPI_Semaphore = NULL;
SemaphoreHandle_t xRADIO_TX_Semaphore = NULL;

#define MAX_LORA_MSG_LEN 20
typedef struct {
	uint8_t uMessageID;
	uint8_t uData[ MAX_LORA_MSG_LEN ];
	uint8_t uLength;
}Message;



// ----------------------------------------------------------------------------
//		Interrupts handle
// ----------------------------------------------------------------------------

extern "C" void USART1_IRQHandler(void)
{
	USART_COM1.USART1_Interrupt();
}

extern "C" void TIM2_IRQHandler(void) {
    if((TIM2->SR & TIM_SR_UIF) != 0)
    {// If update flag is set
    	Timer2.SysTime++;
    	ulHighFrequencyTimerTicks++;
    }

    TIM2->SR &= ~TIM_SR_UIF; //clear interrupt flag

}

extern "C" void I2C1_ER_IRQHandler(void) {
  if (I2C_GetITStatus(I2C1, I2C_IT_AF)) {
    I2C_ClearITPendingBit(I2C1, I2C_IT_AF);
  }

  if (I2C_GetITStatus(I2C1, I2C_IT_TIMEOUT)) {
    I2C_ClearITPendingBit(I2C1, I2C_IT_TIMEOUT);
  }

  if (I2C_GetITStatus(I2C1, I2C_IT_BERR)) {
    I2C_ClearITPendingBit(I2C1, I2C_IT_BERR);
  }

  if (I2C_GetITStatus(I2C1, I2C_IT_ARLO)) {
    I2C_ClearITPendingBit(I2C1, I2C_IT_ARLO);
  }

  if (I2C_GetITStatus(I2C1, I2C_IT_OVR)) {
    I2C_ClearITPendingBit(I2C1, I2C_IT_OVR);
  }
}

extern "C" void I2C1_EV_IRQHandler(void) {

	I2C.Wait_Master();
}

// ----------------------------------------------------------------------------
//		POSIX
// ----------------------------------------------------------------------------
//  Define the time sleep function
int usleep(useconds_t __useconds){

	//the FreeRtos is not capable to generate time interval in microseconds
	if(__useconds>=1000)
		vTaskDelay( pdMS_TO_TICKS( (TickType_t)__useconds/1000 ) );
	else
		vTaskDelay(1);

	return 0;
}

//  Define how the print function will send it values
int _write(int file, char *data, int len){

	ssize_t bytes_written;

	bytes_written = trace_write(data,len);

	return len;
 }

// ----------------------------------------------------------------------------
//		EXTERNAL CALL
// ----------------------------------------------------------------------------
extern "C" {

void vApplicationMallocFailedHook( void );

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName );

void vApplicationIdleHook( void );

void vMyMallocFailedHookFunction(void);

void vMyApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName );

}


// ----------------------------------------------------------------------------
//		TASKS
// ----------------------------------------------------------------------------
/******************************************************
* Receive the package though I2C and decide what to do
******************************************************/
static void prvI2C_Slave_Task( void *pvParameters )
{

	//emulate a message receive though I2C and sent by Lora
	Message msg;
	msg.uMessageID = 0;
	msg.uLength = 10;
	const uint8_t temp[] = {0xFF,0xAA,0,1,2,3,4,5,6,7};
	memcpy(msg.uData, temp ,10);
	EventBits_t uxBits;

	//this function has started, release the semaphore to unblock the led task
	uxBits = xEventGroupSetBits(xBootEventGroup,I2C_TASK_OK);
	assert((uxBits & I2C_TASK_OK) == I2C_TASK_OK);

	while(true)
	{
//		vTaskSuspend(NULL);
		vTaskDelay( pdMS_TO_TICKS( 3000 ) );
		(xQueueSend( xQueue_Lora915, &msg, 10 ) == pdPASS );
		msg.uMessageID++;
	}
}

/******************************************************
* Blink the LED to know that the system is working
******************************************************/
static void prvLED_Task( void *pvParameters ){

	static bool status=false;

	//Wait until all tasks have started
	EventBits_t uxBits;
	do{
		uxBits = xEventGroupWaitBits(xBootEventGroup, LORA_TASK_OK | I2C_TASK_OK, pdFALSE , pdTRUE,pdMS_TO_TICKS( 1000 )) ;

	}while(!((uxBits & (LORA_TASK_OK | I2C_TASK_OK)) == (LORA_TASK_OK | I2C_TASK_OK)));

	for(;;){
		status = !(status);
		if (status)
			blinkLed.turnOff();
		else
			blinkLed.turnOn();

		vTaskDelay( pdMS_TO_TICKS( 1000 ) );
	}
}

typedef enum{
	Standby,
	FC,
	LORA_RX,
	LORA_TX
}Lora_status;

/******************************************************
* Task to receive some msg through 2.4GHz
******************************************************/
static void prvLora2G4_Task_Tx( void *pvParameters ){

	Message xMessage;
	SemtechSX1280::TickTime_t time;

	while(true){

		//Setting xTicksToWait to portMAX_DELAY will cause the task to wait indefinitely
		xQueueReceive( xQueue_Lora2450, &xMessage, portMAX_DELAY );

		//waiting in case the module Lora2450 is been used to receive some data
		if( xSemaphoreTake( xSemaphoreLora2450, portMAX_DELAY ) == pdFALSE )
			assert(false);

		//manager the hardware access for multiple thread program
		xSemaphoreTake(xRADIO_TX_Semaphore, portMAX_DELAY);

		Lora2450.EnableTX();
		time.PeriodBase=SemtechSX1280::RADIO_TICK_SIZE_4000_US;// time to successfully transmit the package
		time.PeriodBaseCount = 0;
		MUTEX_SPI(Lora2450.SendPayload((uint8_t*)&xMessage,xMessage.uLength+1,time));

		//number of trying before giving up
		uint16_t counter = 500;
		bool txstaus;
		do{
			counter--;
			vTaskDelay( pdMS_TO_TICKS( 200 ));

			MUTEX_SPI(txstaus=Lora2450.TXDone());

		}while(!txstaus && (counter!=0));

		//if the counter has reached zero, it means that the communication has failed
		if(counter==0)
			trace_printf("2.4GHz Package not transmitted\r\n");
		else
			trace_printf("2.4GHz Package transmitted\r\n");

		Lora2450.AntSwOff();

		//release to other devices to transmit
		xSemaphoreGive(xRADIO_TX_Semaphore);

		xSemaphoreGive( xSemaphoreLora2450 );

	}

}

/******************************************************
* Task to send some msg through 2.4GHz
******************************************************/
static void prvLora2G4_Task_Rx( void *pvParameters){

	Message RxMessage;
	SemtechSX1280::TickTime_t time;

	while(true){

		//waiting in case the module Lora2450 is been used to transmit some data
		if( xSemaphoreTake( xSemaphoreLora2450, portMAX_DELAY ) == pdFALSE )
			assert(false);

		Lora2450.EnableRX();
		//The SX1280 will produce steps with 4ms
		time.PeriodBase=SemtechSX1280::RADIO_TICK_SIZE_4000_US;
		time.PeriodBaseCount = 0;
		MUTEX_SPI(Lora2450.SetRx(time));

		uint8_t length;
		volatile SemtechSX1280::RadioStatus_t status;
		//this loop will constant verify if there is a new msg
		do{

			vTaskDelay( pdMS_TO_TICKS( 200 ));
			//retrieve the status
			MUTEX_SPI(status.Value = Lora2450.GetStatus().Value);

			//something wrong has happened, set RX again
			if (status.Fields.ChipMode != SX1280_STATUS_MODE_RX)
				MUTEX_SPI(Lora2450.SetRx(time));

		}while(status.Fields.CmdStatus != SX1280_STATUS_DATA_AVAILABLE);


		MUTEX_SPI(Lora2450.GetPayload((uint8_t*)&RxMessage,&length,MAX_LORA_MSG_LEN));
		trace_printf("Package received\r\n");
		Lora2450.AntSwOff();

		//release the semaphore
		xSemaphoreGive( xSemaphoreLora2450 );

	}

}


/******************************************************
* Task to receive a msg through 900MHz
******************************************************/
static void prvLora915M1_Task_Rx( void *pvParameters){

	//Message received
	Message RxMessage;

	//number of received messages
	uint32_t uCounter = 0;

	//enable the EBETY antenna module
	Lora915M1.EnableRX();
	//place the module in RX
	MUTEX_SPI(Lora915M1.SetRx(0));

	SemtechSX126X::RadioStatus_t status = {0};
	while(true){

		//this loop will constant verify if there is a new msg
		do{

			vTaskDelay( pdMS_TO_TICKS( 200 ));
			MUTEX_SPI(status.Value = Lora915M1.GetStatus().Value);

			//something wrong has happened, set RX again
			if (status.Fields.ChipMode != SX126X_STATUS_MODE_RX)
				MUTEX_SPI(Lora915M1.SetRx(0));

		}while(status.Fields.CmdStatus != SX126X_STATUS_DATA_AVAILABLE);

		uint8_t length;
		MUTEX_SPI(Lora915M1.GetPayload((uint8_t*)&RxMessage,&length,MAX_LORA_MSG_LEN));
		RxMessage.uLength = length -1;

		trace_printf("Package ID %i received from Base\r\n", RxMessage.uMessageID);

		uCounter++;

		//Send back the information
		//send though 900MHz
		if(xQueueSend( xQueue_Lora915, &RxMessage, pdMS_TO_TICKS(100))!= pdPASS)
			trace_printf("xQueue_Lora915 fail");
		//send though 2.4GHz
		xQueueSend( xQueue_Lora2450, &RxMessage, pdMS_TO_TICKS(100));


	}

}

/******************************************************
* Task to send a msg through 900MHz
******************************************************/
static void prvLora915M2_Task_Tx( void *pvParameters){

	Message xMessage;

	EventBits_t uxBits;
	uxBits = xEventGroupSetBits(xBootEventGroup,LORA_TASK_OK);
	assert((uxBits&LORA_TASK_OK)==LORA_TASK_OK);

	uint32_t pkgCounter =0;

	while(true){

		//Setting xTicksToWait to portMAX_DELAY will cause the task to wait indefinitely
		xQueueReceive( xQueue_Lora915, &xMessage, portMAX_DELAY );

		//Manage the access to the lora module
		xSemaphoreTake(xRADIO_TX_Semaphore, portMAX_DELAY);

		//SPI is a shared resource.
		Lora915M2.EnableTX();
		MUTEX_SPI(Lora915M2.SendPayload((uint8_t*)&xMessage,xMessage.uLength+1,0));

		//number of trying before give up
		uint16_t counter = 500;
		bool txstaus;
		do{
			counter--;
			vTaskDelay( pdMS_TO_TICKS( 200 ));
			//SPI is a shared resource.
			MUTEX_SPI(txstaus =  Lora915M2.TXDone());

			//TODO assess the SX1280_STATUS_CMD_TIMEOUT
		}while(!txstaus && (counter!=0));

		if(counter==0)
			trace_printf("Package ID %i not transmitted\r\n", xMessage.uMessageID);
		else
			trace_printf("Package ID %i transmitted\r\n", xMessage.uMessageID);

		pkgCounter++;

		Lora915M2.AntSwOff();

		xSemaphoreGive(xRADIO_TX_Semaphore);
		vTaskDelay( pdMS_TO_TICKS( 200 ));

	}

}

/******************************************************
* Task to control the 2.4GHz module state
*****************************************************/
static void prvLora_Task( void *pvParameters )
{
	static Lora_status Status = Standby;

	EventBits_t uxBits;
	uxBits = xEventGroupSetBits(xBootEventGroup,LORA_TASK_OK);
	assert((uxBits&LORA_TASK_OK)==LORA_TASK_OK);

	while(true)
	{
		switch(Status){
		case Standby:
			vTaskDelay( pdMS_TO_TICKS( 10000 ));
			break;
		case LORA_TX:

			Status = Standby;
			break;
		case LORA_RX:

			Status = Standby;
			break;
		default:
			assert(false);
		}

	}
}

// ----------------------------------------------------------------------------
//		SETUP
// ----------------------------------------------------------------------------
static void prvSetup_Task( void *pvParameters ){

	blinkLed.powerUp();

	//Init Serial Communication
	USART_COM1.Setup();
	USART_COM1.write("Serial Started\r\n");

	//Verify the STM32 ID
	volatile uint32_t *STM32_UUID = (uint32_t *)0x1FFFF7E8;
	if ((STM32_UUID[0] == 16985888) && (STM32_UUID[1] == 1295454770) && (STM32_UUID[2] == 5131075))
	{
		trace_printf("Valid Chip ID\n");
	}
	else
	{
		assert(false);
	}


	//-------------------------------------------------------------------
	// Initialize the i2c hardware
	//-------------------------------------------------------------------
	if(!I2C.Init(TWI_FREQ,10))
	{
		  trace_printf("I2C fail!\n");
		  assert(false);
	}
	puts("I2C - Channel 1 Started");

	//-------------------------------------------------------------------
	// Initialize the SPI hardware
	//-------------------------------------------------------------------
	if(!spi.Init(SPI2))
	{
	  trace_printf("SPI fail!\n");
	  assert(false);
	}
	trace_printf("SPI ok!\n");

	//-------------------------------------------------------------------
	// Start the SX126X modules
	//-------------------------------------------------------------------
	SemtechSX126X::pinAssignment pinTable1 = {0,1,2,0xFF,3,4,5,6,0,0,0,0xFF,0,0,0,0};
	if(!Lora915M1.Initialize(&spi,925,pinTable1)) //CubeSat RX module
	{
	  trace_printf("Lora 915 module 1 fail!\n");
	  assert(false);
	}
	 trace_printf("Lora 915 module 1 OK!\n");

	SemtechSX126X::pinAssignment pinTable2 = {7,8,9,0xFF,10,11,12,10,0,0,0,0xFF,0,0,0,1};
	if(!Lora915M2.Initialize(&spi,905,pinTable2))//CubeSat TX module
	{
	  trace_printf("Lora 915 module 2 fail!\n");
	  assert(false);
	}
	 trace_printf("Lora 915 module 2 OK!\n");

	//-------------------------------------------------------------------
	// Start the SX1280 module
	//-------------------------------------------------------------------
	SemtechSX1280::pinAssignment pinTable3 = {0,1,3,0xFF,9,4,5,8,1,1,1,0xFF,1,1,1,1};
	if(!Lora2450.Initialize(&spi,2450,pinTable3))
	{
	  trace_printf("Lora 2.4G fail!\n");
	  assert(false);
	}
	trace_printf("Lora 2.4G ok!\n");
	//-------------------------------------------------------------------
	// Start the tasks
	//-------------------------------------------------------------------

	assert(xTaskCreate( prvLora_Task ,"LoRa", configMINIMAL_STACK_SIZE*2, NULL, LORA_TASK_PRIORITY, NULL) == pdPASS);
	assert(xTaskCreate( prvI2C_Slave_Task , "I2C", configMINIMAL_STACK_SIZE, NULL, I2CSLAVE_TASK_PRIORITY, NULL) == pdPASS);
	assert(xTaskCreate( prvLED_Task , "LED", configMINIMAL_STACK_SIZE, NULL, LED_TASK_PRIORITY, NULL ) == pdPASS);
	assert(xTaskCreate( prvLora915M2_Task_Tx , "LoraTx", configMINIMAL_STACK_SIZE*2, NULL, LORA_TASK_PRIORITY, NULL ) == pdPASS);
	assert(xTaskCreate( prvLora915M1_Task_Rx , "LoraRx", configMINIMAL_STACK_SIZE*2, NULL, LORA_TASK_PRIORITY, NULL ) == pdPASS);
	assert(xTaskCreate( prvLora2G4_Task_Tx , "24LoraRx", configMINIMAL_STACK_SIZE*2, NULL, LORA_2400_TASK_PRIORITY, NULL ) == pdPASS);



	//finish this task
	vTaskDelete( NULL );



}

static void prvSetupHardware( void )
{
	/* RCC system reset(for debug purpose). */
	RCC_DeInit ();

    /* Enable HSE. */
	RCC_HSEConfig( RCC_HSE_ON );

	/* Wait till HSE is ready. */
	while (RCC_GetFlagStatus(RCC_FLAG_HSERDY) == RESET);

    /* HCLK = SYSCLK. */
	RCC_HCLKConfig( RCC_SYSCLK_Div1 );

    /* PCLK2  = HCLK. */
	RCC_PCLK2Config( RCC_HCLK_Div1 );

    /* PCLK1  = HCLK/2. */
	RCC_PCLK1Config( RCC_HCLK_Div2 );

	/* ADCCLK = PCLK2/4. */
	RCC_ADCCLKConfig( RCC_PCLK2_Div4 );

    /* Flash 2 wait state. */
	*( volatile unsigned long  * )0x40022000 = 0x01;

	/* PLLCLK = 8MHz * 9 = 72 MHz */
	RCC_PLLConfig( RCC_PLLSource_HSE_Div1, RCC_PLLMul_9 );

    /* Enable PLL. */
	RCC_PLLCmd( ENABLE );

	/* Wait till PLL is ready. */
	while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET);

	/* Select PLL as system clock source. */
	RCC_SYSCLKConfig (RCC_SYSCLKSource_PLLCLK);

	/* Wait till PLL is used as system clock source. */
	while (RCC_GetSYSCLKSource() != 0x08)

	/* Enable GPIOA, GPIOB, GPIOC, GPIOD, GPIOE and AFIO clocks */
	RCC_APB2PeriphClockCmd(	RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |RCC_APB2Periph_GPIOC
							| RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE | RCC_APB2Periph_AFIO, ENABLE );

	/* Set the Vector Table base address at 0x08000000. */
	NVIC_SetVectorTable( NVIC_VectTab_FLASH, 0x0 );

	NVIC_PriorityGroupConfig( NVIC_PriorityGroup_4 );

	/* Configure HCLK clock as SysTick clock source. */
	SysTick_CLKSourceConfig( SysTick_CLKSource_HCLK );

}

// ----- main() ---------------------------------------------------------------

// Sample pragmas to cope with warnings. Please note the related line at
// the end of this function, used to pop the compiler diagnostics status.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wreturn-type"

int
main(int argc, char* argv[])
{
	// Show the program parameters (passed via semihosting).
	// Output is via the semihosting output channel.
	trace_dump_args(argc, argv);

	// Send a greeting to the trace device (skipped on Release).
	trace_puts("Hello ARM World - FreeRtos is Running!");

	// Send a message to the standard output.
	puts("Standard output message.");

	// Send a message to the standard error.
//	fprintf(stderr, "Standard error message.\n");

	// Send a message to the standard output.
	printf("Standard message.\n");

	// At this stage the system clock should have already been configured
	// at high speed.
	trace_printf("System clock: %u Hz\n", SystemCoreClock);

	uxTopUsedPriority = configMAX_PRIORITIES-1;
	ulHighFrequencyTimerTicks =0;

	//I dont know the reason, but depending of the computer used, the SetupHardware fail to init
	SystemInit();
//	prvSetupHardware();

	Timer2.start();

	//creat the setup tasks
	assert(xTaskCreate( prvSetup_Task, "Setup", configMINIMAL_STACK_SIZE*2, NULL, LED_TASK_PRIORITY, NULL ) == pdPASS);


	/* Attempt to create the event group. */
	xBootEventGroup = xEventGroupCreate();
	assert(xBootEventGroup != NULL);

	//Semaphore to control the access to the Lora module
	xSemaphoreLora2450 = xSemaphoreCreateMutex();

	//To control the access to SPI port
	xSPI_Semaphore = xSemaphoreCreateMutex();

	//There is a issue that is no possible to send
	//any message using different Lora modules at the same time
	//
	xRADIO_TX_Semaphore = xSemaphoreCreateMutex();

	//Send a Queue to send msg throughout LoRa
	xQueue_Lora915 = xQueueCreate( 10, sizeof(Message));
	xQueue_Lora2450 = xQueueCreate( 10, sizeof(Message));
	assert(xQueue_Lora915!=NULL);
	assert(xQueue_Lora2450!=NULL);
	vQueueAddToRegistry(xQueue_Lora915,"Q_LoRa");
	vQueueAddToRegistry(xQueue_Lora2450,"Q_LoRa2");

	/* Start the tasks and timer running. */
	vTaskStartScheduler();

	/* Will only get here if there was insufficient memory to create the idle
	task.  The idle task is created within vTaskStartScheduler(). */
	assert(false);
	for( ;; );

}

void vApplicationMallocFailedHook( void )
{
	/* Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	trace_printf("Malloc Fail");
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	trace_printf("Stack Over Flow");
	/* Run time stack overflow checking is performed if
	configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

	/* This function is called on each cycle of the idle task.  In this case it
	does nothing useful, other than report the amout of FreeRTOS heap that
	remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if( xFreeStackSpace > 100 )
	{
		/* By now, the kernel has allocated everything it is going to, so
		if there is a lot of heap remaining unallocated then
		the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		reduced accordingly. */
	}
}

void prvTaskExitError(void) {
  /* A function that implements a task must not exit or attempt to return to
  its caller as there is nothing to return to.  If a task wants to exit it
  should instead call vTaskDelete( NULL ).

  Artificially force an assert() to be triggered if configASSERT() is
  defined, then stop here so application writers can catch the error. */
  assert(false);
  portDISABLE_INTERRUPTS();
  for(;;) {
    /* wait here */
  }
}

// ----------------------------------------------------------------------------
