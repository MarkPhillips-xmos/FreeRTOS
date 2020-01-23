// Copyright (c) 2019, XMOS Ltd, All rights reserved

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include <xs1.h>

// For hwtimer_t
#include "xcore/hwtimer.h"


static hwtimer_t xKernelTimer;

uint32_t ulPortYieldRequired[ portMAX_CORE_COUNT ] = { pdFALSE };

/*-----------------------------------------------------------*/

__attribute__((fptrgroup("rtos_isr")))
void pxKernelCallHandler( uint32_t ulData )
{
	int xCoreID;

//	debug_printf( "In KCALL: %u\n", ulData );
	xCoreID = rtos_core_id_get();
	ulPortYieldRequired[ xCoreID ] = pdTRUE;
}
/*-----------------------------------------------------------*/

void vIntercoreInterruptISR( void )
{
	int xCoreID;

//	debug_printf( "In KCALL: %u\n", ulData );
	xCoreID = rtos_core_id_get();
	ulPortYieldRequired[ xCoreID ] = pdTRUE;
}
/*-----------------------------------------------------------*/

void vPortSwitchContext( void )
{
	int xCoreID = rtos_core_id_get();
	if( ulPortYieldRequired[ xCoreID ] == pdTRUE )
	{
		ulPortYieldRequired[ xCoreID ] = pdFALSE;
        vTaskSwitchContext( xCoreID );
	}
}
/*-----------------------------------------------------------*/

#if 0
/***** TODO: These should be added to lib_xcore_c *****/
static void _hwtimer_get_trigger_time( hwtimer_t t, uint32_t *time )
{
	asm volatile("getd %0, res[%1]" : "=r" (*time): "r" (t));
}

static xcore_c_error_t hwtimer_get_trigger_time( hwtimer_t t, uint32_t *time )
{
	RETURN_EXCEPTION_OR_ERROR( _hwtimer_get_trigger_time( t, time ) );
}
/******************************************************/
#endif

DEFINE_RTOS_INTERRUPT_CALLBACK( pxKernelTimerISR, pvData )
{
	uint32_t ulNow;
	int xCoreID;

	xCoreID = 0;

	configASSERT( xCoreID == rtos_core_id_get() );

	/* must call hwtimer_get_time to clear the interrupt */
	ulNow = hwtimer_get_time( xKernelTimer );
	/* but we want the next interrupt to be scheduled
	 * relative to the previous interrupt time, not the
	 * current time which is some random amount of time
	 * later. */
	//hwtimer_get_trigger_time( xKernelTimer, &ulNow );
	ulNow = hwtimer_get_trigger_time( xKernelTimer );
	ulNow += configCPU_CLOCK_HZ / configTICK_RATE_HZ;
	hwtimer_change_trigger_time( xKernelTimer, ulNow );

	//	debug_printf("KISR from %d\n", xCoreID);

	if( xTaskIncrementTick() != pdFALSE )
	{
		ulPortYieldRequired[ xCoreID ] = pdTRUE;
	}
}
/*-----------------------------------------------------------*/

void vPortYieldOtherCore( int xOtherCoreID )
{
	int xCoreID;

	/*
	 * This function must be called from within a critical section.
	 */

	xCoreID = rtos_core_id_get();

//	debug_printf("%d->%d\n", xCoreID, xOtherCoreID);

//	debug_printf("Yield core %d from %d\n", xOtherCoreID, xCoreID );

	rtos_irq( xOtherCoreID, xCoreID );
}
/*-----------------------------------------------------------*/

static int prvCoreInit( void )
{
	int xCoreID;

	xCoreID = rtos_core_register();
	debug_printf( "Logical Core %d initializing as FreeRTOS Core %d\n", get_logical_core_id(), xCoreID );

	asm volatile (
			"ldap r11, kexcept\n\t"
			"set kep, r11\n\t"
			:
			:
			: "r11"
	);

	rtos_irq_enable( configNUM_CORES );

	/*
	 * All threads wait here until all have enabled IRQs
	 */
	while( rtos_irq_ready() == pdFALSE );

	if( xCoreID == 0 )
	{
		uint32_t ulNow;
		ulNow = hwtimer_get_time( xKernelTimer );
//		debug_printf( "The time is now (%u)\n", ulNow );

		ulNow += configCPU_CLOCK_HZ / configTICK_RATE_HZ;

		hwtimer_setup_interrupt_callback( xKernelTimer, ulNow, NULL, RTOS_INTERRUPT_CALLBACK( pxKernelTimerISR ) );
		hwtimer_enable_trigger( xKernelTimer );
	}

	return xCoreID;
}
/*-----------------------------------------------------------*/

DEFINE_RTOS_INTERRUPT_PERMITTED( void, vPortStartSchedulerOnCore, void )
{
	int xCoreID;

	xCoreID = prvCoreInit();

	debug_printf( "FreeRTOS Core %d initialized\n", xCoreID );

	/*
	 * Restore the context of the first thread
	 * to run and jump into it.
	 */
	asm volatile (
			"mov r5, %0\n\t" /* R5 must be the FreeRTOS core ID*/
			"ldaw r4, dp[pxCurrentTCBs]\n\t" /* R4 must be the TCB list which is indexed by R5 */
			"bu _freertos_restore_ctx\n\t"
			: /* no outputs */
			: "r"(xCoreID)
			: "r4", "r5"
	);
}
/*-----------------------------------------------------------*/

/*-----------------------------------------------------------*/
/* Public functions required by all ports below:             */
/*-----------------------------------------------------------*/

/*
 * See header file for description.
 */
StackType_t *pxPortInitialiseStack( StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters )
{
	//debug_printf( "Top of stack was %p for task %p\n", pxTopOfStack, pxCode );
	/*
	 * Grow the thread's stack by portTHREAD_CONTEXT_STACK_GROWTH
	 * so we can push the context onto it.
	 */
	pxTopOfStack -= portTHREAD_CONTEXT_STACK_GROWTH;

	uint32_t dp;
	uint32_t cp;

	/*
	 * We need to get the current CP and DP pointers.
	 */
	asm volatile (
			"ldaw r11, cp[0]\n\t" /* get CP into R11 */
			"mov %0, r11\n\t" /* get R11 (CP) into cp */
			"ldaw r11, dp[0]\n\t" /* get DP into R11 */
			"mov %1, r11\n\t" /* get R11 (DP) into dp */
			: "=r"(cp), "=r"(dp) /* output 0 is cp, output 1 is dp */
			: /* there are no inputs */
			: "r11" /* R11 gets clobbered */
	);

	/*
	 * Push the thread context onto the stack.
	 * Saved PC will point to the new thread's
	 * entry pointer.
	 * Interrupts will default to enabled.
	 */
	pxTopOfStack[ 1 ] = ( StackType_t ) pxCode;       /* SP[1]  := SPC */
	pxTopOfStack[ 2 ] = XS1_SR_IEBLE_MASK;            /* SP[2]  := SSR */
	pxTopOfStack[ 3 ] = 0x00000000;                   /* SP[3]  := SED */
	pxTopOfStack[ 4 ] = 0x00000000;                   /* SP[4]  := ET */
	pxTopOfStack[ 5 ] = dp;                           /* SP[5]  := DP */
	pxTopOfStack[ 6 ] = cp;                           /* SP[6]  := CP */
	pxTopOfStack[ 7 ] = 0x00000000;                   /* SP[7]  := LR */
	pxTopOfStack[ 8 ] = ( StackType_t ) pvParameters; /* SP[8]  := R0 */
	pxTopOfStack[ 9 ] = 0x01010101;                   /* SP[9]  := R1 */
	pxTopOfStack[ 10 ] = 0x02020202;                  /* SP[10] := R2 */
	pxTopOfStack[ 11 ] = 0x03030303;                  /* SP[11] := R3 */
	pxTopOfStack[ 12 ] = 0x04040404;                  /* SP[12] := R4 */
	pxTopOfStack[ 13 ] = 0x05050505;                  /* SP[13] := R5 */
	pxTopOfStack[ 14 ] = 0x06060606;                  /* SP[14] := R6 */
	pxTopOfStack[ 15 ] = 0x07070707;                  /* SP[15] := R7 */
	pxTopOfStack[ 16 ] = 0x08080808;                  /* SP[16] := R8 */
	pxTopOfStack[ 17 ] = 0x09090909;                  /* SP[17] := R9 */
	pxTopOfStack[ 18 ] = 0x10101010;                  /* SP[18] := R10 */
	pxTopOfStack[ 19 ] = 0x11111111;                  /* SP[19] := R11 */

	//debug_printf( "Top of stack is now %p for task %p\n", pxTopOfStack, pxCode );

	/*
	 * Returns the new top of the stack
	 */
	return pxTopOfStack;
}
/*-----------------------------------------------------------*/

void vPortStartSMPScheduler( void );

/*
 * See header file for description.
 */
BaseType_t xPortStartScheduler( void )
{
	if( ( configNUM_CORES > portMAX_CORE_COUNT ) || ( configNUM_CORES <= 0 ) )
	{
		return pdFAIL;
	}

	rtos_locks_initialize();
	xKernelTimer = hwtimer_alloc();

	vPortStartSMPScheduler();

	return pdPASS;
}
/*-----------------------------------------------------------*/

void vPortEndScheduler( void )
{
	/* Do not implement. */
}
/*-----------------------------------------------------------*/
