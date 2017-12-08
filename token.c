/* Includes */
#include "includes.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Define */
#define TASK_STK_SIZE    OS_TASK_DEF_STK_SIZE
#define N_TASKS          5      // [1]Watchdog, [2]Cds, [3]Led, [4]Fnd, [5]Pause task
#define MSG_QUEUE_SIZE   6      // To transfer string "LIFE %d"

#define CDS_VALUE        871

#define DOT              0x80   // Display .(dot) on FND
#define BLANK            0x00   // Display blank on FND

#define BUF_SIZE         16     // Temporary size of silde buffer
#define PADDING          3      // Padding size of slide buffer

#define ON               1      // Switch on
#define OFF              0      // Switch off

#define INITIAL_LEVEL    1      // Initail value of var level
#define CLEAR_LEVEL      10     // Last level (to judge game clear)

#define BRIGHT           1      // CDS brightness :: bright
#define DARK             0      // CDS brightness :: dark

#define DEADLINE         3      // Max cycle (if exceed it, -1 life)

#define SLIDE_FAST       20     // FND slide speed :: fast
#define SLIDE_SLOW       40     // FND slide speed :: slow

#define START_LEVEL      1      // Start level
#define INITIAL_LIFE     5      // Start life
#define INITIAL_SPEED    (11 - START_LEVEL) * 2    // level 1 speed
#define CYCLE_BEGIN      0      // LED cycle begin
#define CYCLE_END        15     // LED cycle end
#define ROUND_INTERVAL   300    // Time interval of each round

#define PAUSE_INTERVAL   10     // Time interval to check pausing

/* Variables */
OS_STK TaskStk[N_TASKS][TASK_STK_SIZE];

OS_EVENT *Mbox;                        // Mail box
OS_EVENT *MQueue;                      // Message queue
OS_EVENT *Sem;                         // Semaphore
OS_FLAG_GRP *FlagGrp;                  // Event flag

void *MQueueBuffer[MSG_QUEUE_SIZE];    // buffer of Message queue

INT8U Level = 1;                       // Shared variable (have to guarantee the mutual exclusion)

volatile INT8U Sw1, Sw2;               // Switch var

// LED order (7 -> 6 -> ... -> 1 -> 0 -> 1 -> ... -> 6 -> 7)
const INT8U Order[] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };

INT8U LevelDisp[] = { 0x38, 0x1C + DOT, 0x3f, 0x3f };          // "Lv.00"
const INT8U LifeDisp[] = { 0x38, 0x06, 0x71, 0x79 };           // "LIFE"
const INT8U ClearDisp[] = { 0x39, 0x38, 0x79, 0x77, 0x50 };    // "CLEAr"
const INT8U OverDisp[] = { 0x5C, 0x1C, 0x79, 0x50 };           // "ovEr"
const INT8U PauseDisp[] = { 0x73, 0x77, 0x3E, 0x6D, 0x79 };    // "PAUSE"
const INT8U Digit[] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x27, 0x7f, 0x6f }; // 0 ~ 9

const INT8U Fnd_sel[4] = { 0x08, 0x04, 0x02, 0x01 };  // 왼쪽부터 0, 1, 2, 3번째

/* Interrupt Service Routine */
ISR(INT4_vect) { /* Switch 1 :: Gaming Button */
	Sw1 = ON;
	_delay_ms(10);
	OSTimeDly(1);
}
ISR(INT5_vect) { /* Switch 2 :: Pause */
	Sw2 ^= 0x01;
	_delay_ms(10);
	OSTimeDly(1);
}

/* Function Dec */
void regInit();                         // Initiate registers
void eventInit();                       // Initiate events
inline void displayFnd(INT8U fnd[]);    // Display the fnd[] to FND
void slideFnd(INT8U str[], INT8U len, INT8U time);    // Slide str[] to FND
inline INT8U getRandomToken();          // Generate random token
INT16U read_adc();                      // read ADC converter

/* Task Dec */
void WatchdogTask(void *data);          // Manage the whole program (by life)
void CdsTask(void *data);               // Read ADC value in CDS sensor
void LedTask(void *data);               // Progress the game (hit the token)
void FndTask(void *data);               // Display the status to FND
void PauseTask(void *data);             // Pause the game


/* Main */
int main() {
	/* OS Initialize */
	OSInit();
	OS_ENTER_CRITICAL();
	TCCR0 = 0x07;
	TIMSK = _BV(TOIE0);
	TCNT0 = 256 - (CPU_CLOCK_HZ / OS_TICKS_PER_SEC / 1024);
	OS_EXIT_CRITICAL();

	/* Init Registers */
	regInit();

	/* Init Events */
	eventInit();

	/* Task Create */
	OSTaskCreate(WatchdogTask, (void *)0, (void *)&TaskStk[0][TASK_STK_SIZE - 1], 1);
	OSTaskCreate(CdsTask, (void *)0, (void *)&TaskStk[1][TASK_STK_SIZE - 1], 2);
	OSTaskCreate(LedTask, (void *)0, (void *)&TaskStk[2][TASK_STK_SIZE - 1], 3);
	OSTaskCreate(FndTask, (void *)0, (void *)&TaskStk[3][TASK_STK_SIZE - 1], 4);
	OSTaskCreate(PauseTask, (void *)0, (void *)&TaskStk[4][TASK_STK_SIZE - 1], 5);

	/* OS Start */
	OSStart();

	return 0;
}



/* Functions */
void regInit() {
	/* LED */
	DDRA = 0xff;    // LED output

	/* FND */
	DDRC = 0xff;    // FND output
	DDRG = 0x0f;    // FND select output

	/* Switch */
	DDRE = 0xcf;    // Switch input
	EICRB = 0x0A;   // Set external interrupt 4, 5's trigger as falling edge
	EIMSK = 0x30;   // External interrupt 4, 5 enable
	sei();          // Set global Interrupt

	/* ADC */
	ADMUX = 0x00;
		// 00000000
		// REFS(1:0) = "00" AREF(+5V) 기준전압 사용
		// ADLAR = '0' 디폴트 오른쪽 정렬
		// MUX(4:0) = "00000" ADC0 사용, 단극 입력
	ADCSRA = 0x87;
		// 10000111
		// ADEN = '1' ADC를 Enable
		// ADFR = '0' single conversion 모드
		// ADPS(2:0) = "111" 프리스케일러 128분주	
}

void eventInit() {
	INT8U err;

	Mbox = OSMboxCreate((void *)0);
	MQueue = OSQCreate(MQueueBuffer, MSG_QUEUE_SIZE);
	Sem = OSSemCreate(1);
	FlagGrp = OSFlagCreate(0x00, &err);
}

inline void displayFnd(INT8U fnd[]) {
	INT8U pos;
	for (pos = 0; pos < 4; pos++) {
		PORTC = fnd[pos];
		PORTG = Fnd_sel[pos];
		_delay_us(2500);
	}
}

void slideFnd(INT8U str[], INT8U len, INT8U time) {
	INT8U i, j;
	INT8U slide[BUF_SIZE];
	INT8U newlen;

	newlen = len + (PADDING * 2);
	if (newlen > BUF_SIZE) return;

	memset(slide, 0, newlen * sizeof(INT8U));

	for (i = 0; i < len; i++) {
		slide[i + PADDING] = str[i];
	}
	
	for (i = 0; i < newlen - PADDING; i++) {
		PORTA = 0x01 << i;
		for (j = 0; j < time; j++) {
			displayFnd(&slide[i]);
		}
	}
}

inline INT8U getRandomToken() { return (0x01 << (rand() % 8)); }

INT16U read_adc() {
	INT8U low, high;
	INT16U value;

	// ADC read start
	ADCSRA |= 0x40;

	// Wait for the reading to finish
	while ((ADCSRA & 0x10) != 0x10);

	// Get ADC data
	low = ADCL;
	high = ADCH;

	// Add up the data
	value = (high << 8) | low;

	// return
	return value;
}



/* Task body */
void WatchdogTask(void *data) {
	INT8U err;
	INT8U life;                  // To receive message from LedTask
	INT8U send[MSG_QUEUE_SIZE];  // To be transferred to FndTask
	INT8U lev;                   // To check the player clear the game

	// To avoid complier warning
	data = data;

	// Fill in the message to send (default)
	send[0] = LifeDisp[0];
	send[1] = LifeDisp[1];
	send[2] = LifeDisp[2];
	send[3] = LifeDisp[3];
	send[4] = BLANK;

	while (1) {
		// This task would be running, when it takes message that life is decreasing

		// Get message (Mbox) <-- LedTask
		life = *(INT8U *)OSMboxPend(Mbox, 0, &err);

		// Get current level from shared memory
		OSSemPend(Sem, 0, &err);
		lev = Level;
		OSSemPost(Sem);

		if (lev > CLEAR_LEVEL) { // level is greater than CLEAR_LEVEL(10)
			while (1) { // Dislay "clear" to FND
				slideFnd(ClearDisp, sizeof(ClearDisp) / sizeof(INT8U), SLIDE_SLOW);
			}
		} else if (life == 0) {
			// Game Over
			while (1) {
				// On this page, all the task would be stopped
				// You have to push reset button
				displayFnd(OverDisp);
			}
		} else {
			// Fill in the message to send
			send[5] = Digit[life];
			
			// Message(send[]) :: "LIFE %d"

			// Send message (Message Queue) --> FndTask
			OSQPost(MQueue, (void *)send);
		}
	}
}

void CdsTask(void *data) {
	INT8U err;
	INT16U value;
	INT8U brightness;

	// To avoid complier warning
	data = data;
	
	// Init value
	brightness = -1;
	
	while (1) {
		// Get adc value
		value = read_adc();

		// Decide brightness
		if (value < CDS_VALUE) { // Dark
			if (brightness != DARK) {
				// Post to LED task (the brightness is changed)
				OSFlagPost(FlagGrp, 0x01, OS_FLAG_SET, &err);
				brightness = DARK;
			}
		} else { // Bright
			if (brightness != BRIGHT) {
				// Post to LED task (the brightness is changed)
				OSFlagPost(FlagGrp, 0x10, OS_FLAG_SET, &err);
				brightness = BRIGHT;
			}
		}

		OSTimeDly(10);
	}
}

void LedTask(void *data) {
	INT8U i;
	INT8U err;

	INT8U orderIdx;      // LED progressing order (index of Order[])
	INT8U token;         // Position of token have to hit right timing
	INT8U left_time;     // Left time of cycles (if it is 0, -1 life)
	INT8U life;          // Life of the game
	INT8U speed;         // LED speed depending on the level
	INT8U brightness;    // Brightness (get the information from CdsTask)

	// To avoid complier warning
	data = data;

	// Init conditions
	orderIdx = CYCLE_BEGIN;
	token = getRandomToken();
	left_time = DEADLINE;
	life = INITIAL_LIFE;
	speed = INITIAL_SPEED;
	brightness = BRIGHT;

	while (1) {
		// Get the brightness information from CdsTask
		if (OSFlagAccept(FlagGrp, 0x01, OS_FLAG_WAIT_SET_ALL, &err) != (OS_FLAGS)0) {
			brightness = DARK;
			
			// Consume the flag
			OSFlagAccept(FlagGrp, 0x01, OS_FLAG_WAIT_SET_ALL + OS_FLAG_CONSUME, &err);
		}

		if (OSFlagAccept(FlagGrp, 0x10, OS_FLAG_WAIT_SET_ALL, &err) != (OS_FLAGS)0) {
			brightness = BRIGHT;

			// Consume the flag
			OSFlagAccept(FlagGrp, 0x10, OS_FLAG_WAIT_SET_ALL + OS_FLAG_CONSUME, &err);
		}

		if (Sw2 == OFF) {
			if (Sw1 == OFF) { // Default :: progress the game normally
				PORTA = (Order[orderIdx++] | token); // Update the LED
				if (brightness == DARK) { // If outside is dark, toggle the LED
					PORTA ^= 0xFF;
				}
				
				if (orderIdx == CYCLE_END) { // LED is on the end point of each cycle
					orderIdx = CYCLE_BEGIN + 1;
					if (--left_time == 0) { // Time over -> -1 life
						--life;

						// Send to WatchdogTask by Mbox
						OSMboxPost(Mbox, (void *)&life);

						// Reset
						left_time = DEADLINE;
						orderIdx = CYCLE_BEGIN;
						OSTimeDly(ROUND_INTERVAL);

						// Generate random token 
						token = getRandomToken();
					}
				}
			} else { // Sw1 Clicked
				if (PORTA == token || PORTA == (token ^ 0xFF)) { // Hit! -> next level
					// Level up (-> speed up)
					// Change level on shared memory
					OSSemPend(Sem, 0, &err);
					Level++;
					speed = (11 - Level) * 2;
					OSSemPost(Sem);

					// Reset
					orderIdx = CYCLE_BEGIN;
					left_time = DEADLINE;

					// Blink 3 times
					for (i = 0; i < 3; i++) {
						PORTA = 0xFF;
						OSTimeDly(ROUND_INTERVAL / 6);
						PORTA = 0x00;
						OSTimeDly(ROUND_INTERVAL / 6);
					}
				} else { // Miss! -> -1 life
					--life;

					// Send to WatchdogTask
					OSMboxPost(Mbox, (void *)&life);

					// Reset
					orderIdx = CYCLE_BEGIN;
					left_time = DEADLINE;
					//PORTA = 0x00;
					OSTimeDly(ROUND_INTERVAL);
				}

				// Generate random token 
				token = getRandomToken();

				// Switch off
				Sw1 = OFF;
			}
			OSTimeDly(speed);
		} else { // Pause
			OSTimeDly(PAUSE_INTERVAL);
		}
	}
}

void FndTask(void *data) {
	INT8U err;
	INT8U *recv;
	INT8U lev;

	// To avoid complier warning
	data = data;

	while (1) {
		if (Sw2 == OFF) {
			// Get a message from MQueue by non-block pending
			recv = (INT8U *)OSQAccept(MQueue);
			if (recv == (void *)0) { // There is no message in queue

				// Get level from shared memory
				OSSemPend(Sem, 0, &err);
				lev = Level;
				OSSemPost(Sem);

				// Change displaying level
				LevelDisp[2] = Digit[lev / 10];
				LevelDisp[3] = Digit[lev % 10];

				if (lev > CLEAR_LEVEL) {
					LevelDisp[2] = Digit[0];
					LevelDisp[3] = Digit[0];
				}

				// Display level
				displayFnd(LevelDisp);

			} else { // Queue has an element (life information)
				// Display life left
				slideFnd(recv, MSG_QUEUE_SIZE, SLIDE_FAST);

				// Flush LED
				PORTA = 0xFF;
				_delay_ms(500);
				PORTA = 0x00;
			}
		} else { // Pause
			OSTimeDly(PAUSE_INTERVAL);
		}
	}
}

void PauseTask(void *data) {
	// To avoid complier warning
	data = data;

	while (1) {
		// Display sliding string "PAUSE"
		slideFnd(PauseDisp, sizeof(PauseDisp) / sizeof(INT8U), SLIDE_SLOW);
	}
}
