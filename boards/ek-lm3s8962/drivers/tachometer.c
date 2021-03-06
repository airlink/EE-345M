//*****************************************************************************
//
// Filename: tachometer.c
// Authors: Dan Cleary   
// Initial Creation Date: March 29, 2011 
// Description: This file includes functions for interfacing with the 
//   QRB1134 optical reflectance sensor.    
// Lab Number: 6    
// TA: Raffaele Cetrulo      
// Date of last revision: March 29, 2011      
// Hardware Configuration:
//   PB0 - Tachometer A Input
//   PB1 - Tachometer B Input
//
//*****************************************************************************

#include <string.h>
#include "tachometer.h"
#include "motor.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "driverlib/can.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "drivers/OS.h"
#include "timer.h"
#include "inc/hw_timer.h"
#include "driverlib/sysctl.h"
#include "can_device_fifo/can_device_fifo.h"
#include "math.h"
//#include "drivers/can_fifo.h"

long SRSave (void);
void SRRestore(long sr);

#define OS_ENTERCRITICAL(){sr = SRSave();}
#define OS_EXITCRITICAL(){SRRestore(sr);}
#define CAN_FIFO_SIZE           (8 * 8)

#define _TACH_STATS	0

//***********************************************************************
// Tach_Fifo variables, this code segment copied from Valvano, lecture1
//***********************************************************************
#define NUM_TACHS 2
typedef unsigned long dataType;
unsigned long Tach_FifoSize;

dataType volatile (*Tach_PutPt[NUM_TACHS]); // put next
dataType volatile (*Tach_GetPt[NUM_TACHS]); // get next
dataType Tach_Fifo[NUM_TACHS][MAX_TACH_FIFOSIZE];
Sema4Type Tach_FifoDataReady[NUM_TACHS];
unsigned long Tach_NumSamples[NUM_TACHS];
unsigned long Tach_DataLost[NUM_TACHS];


//***********************************************************************
//
// Tach_Fifo_Init
//
//***********************************************************************
static
void Tach_Fifo_Init(unsigned int size)
{
  Tach_PutPt[0] = Tach_GetPt[0] = &Tach_Fifo[0][0];
  Tach_PutPt[1] = Tach_GetPt[1] = &Tach_Fifo[1][0];
  Tach_FifoSize = size;
}

//***********************************************************************
//
// Tach_Fifo_Get
//
//***********************************************************************
static
unsigned int
Tach_Fifo_Get(unsigned char tach_id, unsigned long * dataPtr)
{
  if(Tach_PutPt[tach_id] == Tach_GetPt[tach_id]){
    return FAIL;// Empty if Tach_PutPt=GetPt
  }
  else{
    *dataPtr = *(Tach_GetPt[tach_id]++);
    if(Tach_GetPt[tach_id] == &Tach_Fifo[tach_id][Tach_FifoSize]){
      Tach_GetPt[tach_id] = &Tach_Fifo[tach_id][0]; // wrap
    }
    return SUCCESS;
  }
}

//***********************************************************************
//
// Tach_Fifo_Put
//
//***********************************************************************
unsigned int
Tach_Fifo_Put(unsigned char tach_id, unsigned long data)
{
  dataType volatile *nextPutPt;
  nextPutPt = Tach_PutPt[tach_id]+1;
  if(nextPutPt ==&Tach_Fifo[tach_id][Tach_FifoSize]){
    nextPutPt = &Tach_Fifo[tach_id][0]; // wrap
  }
  if(nextPutPt == Tach_GetPt[tach_id]){
    return FAIL; // Failed, fifo full
  }
  else{
    *(Tach_PutPt[tach_id]) = data; // Put
    Tach_PutPt[tach_id] = nextPutPt; // Success, update
    return SUCCESS;
  }
}

// ********** Tach_Filter ***********
// Peforms IIR filter calculations, stores
//   filtered data in y buffer
// (Based on Filter from Lab2.c	by Jonathan Valvano)
// Inputs:
// 	 data - data to be filtered
// Outputs: filtered data
static
unsigned long Tach_Filter(unsigned long data){
	static unsigned long x[4];
	static unsigned long y[4];
	static unsigned int n=2;

	n++;
	if(n==4) n=2;
	x[n] = x[n-2] = data; // two copies of new data
	y[n] = (y[n-1] + x[n])/2;
	y[n-2] = y[n]; // two copies of filter outputs too
	return y[n];
} 

// *********** Tach_Init ************
// Initializes tachometer I/O pins and interrupts
// Inputs: none
// Outputs: none
void Tach_Init(unsigned long priority){

	IntMasterDisable();

  	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);

	//Configure port pin for digital input
	GPIODirModeSet(GPIO_PORTB_BASE, (GPIO_PIN_0), GPIO_DIR_MODE_HW);
	GPIOPadConfigSet(GPIO_PORTB_BASE, (GPIO_PIN_0), GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
	GPIODirModeSet(GPIO_PORTB_BASE, (GPIO_PIN_1), GPIO_DIR_MODE_HW);
	GPIOPadConfigSet(GPIO_PORTB_BASE, (GPIO_PIN_1), GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

	//Configure alternate function 
	GPIOPinConfigure(GPIO_PB0_CCP0);
	GPIOPinConfigure(GPIO_PB1_CCP2);

	// Configure GPTimerModule to generate triggering events
  	// at the specified sampling rate.
  	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
  	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER1);
									  
	TimerDisable(TIMER0_BASE, TIMER_A);
	TimerDisable(TIMER1_BASE, TIMER_A);
	//TimerDisable(TIMER1_BASE, TIMER_A);


	// Configure for Subtimer B Input Edge Timing Mode
	HWREG(TIMER0_BASE + TIMER_O_CFG) = 0x04;
	HWREG(TIMER0_BASE + TIMER_O_TAMR) |= 0x07; 
	HWREG(TIMER0_BASE + TIMER_O_CTL) &= ~(0x0C); //0x0C?
	//debugging
	HWREG(TIMER0_BASE + TIMER_O_CTL) |= 0x02;
	HWREG(TIMER0_BASE + TIMER_O_TAILR) |= 0xFFFF;


	// Configure for Subtimer A Input Edge Timing Mode
	HWREG(TIMER1_BASE + TIMER_O_CFG) = 0x04;
	HWREG(TIMER1_BASE + TIMER_O_TAMR) |= 0x07;
	//TimerConfigure(TIMER0_BASE, TIMER_CFG_A_CAP_TIME);
	HWREG(TIMER1_BASE + TIMER_O_CTL) &= ~(0x0C); //0x06?
	//debugging
	HWREG(TIMER1_BASE + TIMER_O_CTL) |= 0x02;
	HWREG(TIMER1_BASE + TIMER_O_TAILR) |= 0xFFFF; 

	TimerIntEnable(TIMER0_BASE, (TIMER_CAPA_EVENT | TIMER_TIMA_TIMEOUT));
	TimerIntEnable(TIMER1_BASE, (TIMER_CAPA_EVENT | TIMER_TIMA_TIMEOUT));
	TimerEnable(TIMER0_BASE, TIMER_A);
	TimerEnable(TIMER1_BASE, TIMER_A);

	//Enable port interrupt in NVIC
	IntEnable(INT_TIMER0A);
	IntEnable(INT_TIMER1A);				 
	IntPrioritySet(INT_TIMER1B, priority << 5);
	IntPrioritySet(INT_TIMER1A, priority << 5);
	//

	//Initialize FIFO
	Tach_Fifo_Init(128);

	IntMasterEnable();
}

// ********** Tach_InputCapture0A ***********
// Input capture exception handler for tachometer.
//   Interrupts on rising edge. Obtains time since
//   previous interrupts, sends time to foreground
//   via FIFO.
// Inputs: none
// Outputs: none
#define STOP_TIMEOUT	500
unsigned long tach_0A_timeout_count = 0;
unsigned long tach_0A_stop_detect = 0;
unsigned long SeePeriod1 = 0;   
unsigned long SeeRPM1 = 0;
unsigned long SeePeriod2 = 0;   
unsigned long SeeRPM2 = 0;
void Tach_InputCapture0A(void){
	unsigned long period;
	//long time_debug;

	// if timeout
 	if (HWREG(TIMER0_BASE + TIMER_O_MIS) & TIMER_TIMA_TIMEOUT){
		tach_0A_timeout_count++;
        //tach_0A_stop_detect++;
		 // Stop Detection
		if (tach_0A_timeout_count >= STOP_TIMEOUT){
			tach_0A_timeout_count = 0;
			Tach_Fifo_Put(MOTOR_LEFT_ID, 3750000000);
		}
//        tach_0A_stop_detect++;
//		 // Stop Detection
//		if (tach_0A_stop_detect >= STOP_TIMEOUT){
//			tach_0A_stop_detect = 0;
//			Tach_Fifo_Put(0, 3750000000);
//		}
	}
	// if input capture
	if (HWREG(TIMER0_BASE + TIMER_O_MIS) & TIMER_CAPA_EVENT){
    // Get time automatically from hardware
		period = (0xFFFF - HWREG(TIMER0_BASE + TIMER_O_TAR))  // time remaining from countdown
				  + (tach_0A_timeout_count * 0xFFFF);
		SeePeriod1 = period;
		SeeRPM1 = (375000000/period)*10;			  // number of timeouts
		tach_0A_timeout_count = 0;
		//time_debug = Tach_TimeDifference(time2, time1);
		//Tach_Fifo_Put(time_debug);
		//if((SeeRPM1 < (FULL_SPEED + 200)) && Tach_Fifo_Put(0, period))
		if((SeeRPM1 < (FULL_SPEED + 100)) && Tach_Fifo_Put(MOTOR_LEFT_ID, period))	
            Tach_NumSamples[MOTOR_LEFT_ID]++;
		else
			Tach_DataLost[MOTOR_LEFT_ID]++;
	}
	TimerIntClear(TIMER0_BASE, (TIMER_CAPA_EVENT | TIMER_TIMA_TIMEOUT));
}

// ********** Tach_InputCapture ***********
// Input capture exception handler for tachometer.
//   Interrupts on rising edge. Obtains time since
//   previous interrupts, sends time to foreground
//   via FIFO.
// Inputs: none
// Outputs: none
unsigned long tach_1A_timeout_count = 0;
unsigned long tach_1A_stop_detect = 0;
void Tach_InputCapture1A(void){
	unsigned long period;
	//long time_debug;

	// if timeout
 	if (HWREG(TIMER1_BASE + TIMER_O_MIS) & TIMER_TIMA_TIMEOUT){
		tach_1A_timeout_count++;
        //tach_1A_stop_detect++;
		// Stop Detection
		if (tach_1A_timeout_count >= STOP_TIMEOUT){
			tach_1A_timeout_count = 0;
		 	Tach_Fifo_Put(MOTOR_RIGHT_ID, 3750000000);
		}
//        tach_1A_stop_detect++;
//		// Stop Detection
//		if (tach_1A_stop_detect >= STOP_TIMEOUT){
//			tach_1A_stop_detect = 0;
//		 	Tach_Fifo_Put(0, 3750000000);
//		}
	}
	// if input capture
	if ((HWREG(TIMER1_BASE + TIMER_O_MIS) & TIMER_CAPA_EVENT)){
    // Get time automatically from hardware
		period = (0xFFFF - HWREG(TIMER1_BASE + TIMER_O_TAR))  // time remaining from countdown
				  + (tach_1A_timeout_count * 0xFFFF);			  // number of timeouts
		SeePeriod2 = period;
		SeeRPM2 = (375000000/period)*10;
        if (SeeRPM2 > 2500){
            period++;
        }
		tach_1A_timeout_count = 0;
		//time_debug = Tach_TimeDifference(time2, time1);
		//Tach_Fifo_Put(time_debug);
        if (period == 0){
            period++;
        }
        
		if((SeeRPM2 < (FULL_SPEED + 100)) && Tach_Fifo_Put(MOTOR_RIGHT_ID, period))
			Tach_NumSamples[MOTOR_RIGHT_ID]++;
		else
			Tach_DataLost[MOTOR_RIGHT_ID]++;
	}
	TimerIntClear(TIMER1_BASE, (TIMER_CAPA_EVENT | TIMER_TIMA_TIMEOUT));
}


// ********** Tach_SendData ***********
// Analyzes tachometer data, passes to
//   big board via CAN.
// Inputs: none
// Outputs: none
#define TACH_STATS_SIZE 350
unsigned long SeeTach1;
unsigned long SeeTach2;
unsigned long SeeTach3;
unsigned long SeeTach4;
unsigned long speed;
//int CANTransmitFIFO(unsigned char *pucData, unsigned long ulSize);
unsigned char speedBuffer[CAN_FIFO_SIZE];
unsigned long stats[TACH_STATS_SIZE];
struct TACH_STATS{
  short average;
  short stdev;
  short maxdev;
};
int speed_i = 0;
int speed2_i = 0;
struct TACH_STATS Tach_Stats;
unsigned long SpeedArr[100] = {0, }; 
unsigned long SpeedArr2[100] = {0, };
unsigned long data = 0;
unsigned long NumReceived[2] = {0, 0};
void Tach_SendData(unsigned char tach_id){
	static unsigned int total_time = 0;

	#ifdef _TACH_STATS
	unsigned short i;
	static unsigned int num_samples = 0;
	static unsigned char stat_done = 0;
	long sum;
	unsigned long max,min;
	#endif

	if(Tach_Fifo_Get(tach_id, &data)){
        NumReceived[tach_id]++;
        //if (NumReceived[tach_id] > 2){
    		total_time += data;
    		//data = Tach_Filter(data);
    		SeeTach1 = data;
            SeeTach2 = (375000000/data)*10;
            data = SeeTach2;
    		//data = (375000000/data)*10; //convert to .1 RPM	-> (60 s)*(10^9ns)/4*(T*40 ns) * 10 .1RPM
    		if (tach_id == 0){
    		    SpeedArr[speed_i++] = data;
                if (speed_i == 100){
                    speed_i = 0;
                }
    		}
    		else {
    		    SpeedArr2[speed2_i++] = data;
                if (speed2_i == 100){
                    speed2_i = 0;
                }
    		}
            //if (NumReceived[tach_id] > 2){
                if (data < FULL_SPEED+200)
    		        Motor_PID(tach_id, data);
            //}
    	
    		#ifdef _TACH_STATS
    		if((tach_id == 0) && (!stat_done)){
    			if((num_samples < TACH_STATS_SIZE) && (total_time < 250000000)){	 //250mil * 4ns = 10 s
    				stats[num_samples] = data;
    				num_samples++;
    			}
    			else {
    				//Average = sum of all values/number of values
    				//Maximum deviation = max value - min value
    				max = 0;
    				min = 0xFFFFFFFF;
    				sum = 0;
    				for(i = 1; i < num_samples; i++){	 // first sample is junk
    			      sum += (long)stats[i];
    				  if(stats[i] < min) min = stats[i];
    				  if(stats[i] > max) max = stats[i];	  
    				}
    			    Tach_Stats.average = (short)(sum/(num_samples - 1));
    				Tach_Stats.maxdev = (short)(max - min);
    			
    				// Standard deviation = sqrt(sum of (each value - average)^2 / number of values)
    				sum = 0;
    				for(i = 1; i < num_samples; i++){
    			      sum +=(long)((short)stats[i]-(short)Tach_Stats.average)*((short)stats[i]-(short)Tach_Stats.average);	  
    				}
    				sum = sum/(num_samples - 1);
    				Tach_Stats.stdev = (short)sqrt(sum);
    				stat_done = 1;
    			}
    		}
    		#endif
    
			speedBuffer[0] = 't';
			memcpy(&speedBuffer[1], &data, 4); 
			CAN_Send(speedBuffer);	
        //}
	}
}
