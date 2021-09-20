//
// m68kinterface.c: Code interface to the UAE 68000 core and support code
//
// by James Hammons
// (C) 2011 Underground Software
//
// JLH = James Hammons <jlhamm@acm.org>
//
// Who  When        What
// ---  ----------  -------------------------------------------------------------
// JLH  10/28/2011  Created this file ;-)
//

#include "m68kinterface.h"
//#include <pthread.h>
#include "cpudefs.h"
#include "inlines.h"
#include "cpuextra.h"
#include "readcpu.h"

// Exception Vectors handled by emulation
#define EXCEPTION_BUS_ERROR                2 /* This one is not emulated! */
#define EXCEPTION_ADDRESS_ERROR            3 /* This one is partially emulated (doesn't stack a proper frame yet) */
#define EXCEPTION_ILLEGAL_INSTRUCTION      4
#define EXCEPTION_ZERO_DIVIDE              5
#define EXCEPTION_CHK                      6
#define EXCEPTION_TRAPV                    7
#define EXCEPTION_PRIVILEGE_VIOLATION      8
#define EXCEPTION_TRACE                    9
#define EXCEPTION_1010                    10
#define EXCEPTION_1111                    11
#define EXCEPTION_FORMAT_ERROR            14
#define EXCEPTION_UNINITIALIZED_INTERRUPT 15
#define EXCEPTION_SPURIOUS_INTERRUPT      24
#define EXCEPTION_INTERRUPT_AUTOVECTOR    24
#define EXCEPTION_TRAP_BASE               32

// These are found in obj/cpustbl.c (generated by gencpu)

//extern const struct cputbl op_smalltbl_0_ff[];	/* 68040 */
//extern const struct cputbl op_smalltbl_1_ff[];	/* 68020 + 68881 */
//extern const struct cputbl op_smalltbl_2_ff[];	/* 68020 */
//extern const struct cputbl op_smalltbl_3_ff[];	/* 68010 */
extern const struct cputbl op_smalltbl_4_ff[];	/* 68000 */
extern const struct cputbl op_smalltbl_5_ff[];	/* 68000 slow but compatible.  */

// Externs, supplied by the user...
//extern int irq_ack_handler(int);

// Function prototypes...
static INLINE void m68ki_check_interrupts(void);
void m68ki_exception_interrupt(uint32_t intLevel);
static INLINE uint32_t m68ki_init_exception(void);
static INLINE void m68ki_stack_frame_3word(uint32_t pc, uint32_t sr);
unsigned long IllegalOpcode(uint32_t opcode);
void BuildCPUFunctionTable(void);
void m68k_set_irq2(unsigned int intLevel);

// Local "Global" vars
static int32_t initialCycles;
cpuop_func * cpuFunctionTable[65536];

// By virtue of the fact that m68k_set_irq() can be called asychronously by
// another thread, we need something along the lines of this:
static int checkForIRQToHandle = 0;
//static pthread_mutex_t executionLock = PTHREAD_MUTEX_INITIALIZER;
static int IRQLevelToHandle = 0;

void Dasm(uint32_t offset, uint32_t qt)
{
}


void M68KDebugHalt(void)
{
	regs.spcflags |= SPCFLAG_DEBUGGER;
}


void M68KDebugResume(void)
{
	regs.spcflags &= ~SPCFLAG_DEBUGGER;
}


void m68k_set_cpu_type(unsigned int type)
{
}


// Pulse the RESET line on the CPU
void m68k_pulse_reset(void)
{
	static uint32_t emulation_initialized = 0;

	// The first call to this function initializes the opcode handler jump table
	if (!emulation_initialized)
	{
		// Build opcode handler table here...
		read_table68k();
		do_merges();
		BuildCPUFunctionTable();
		emulation_initialized = 1;
	}

//	if (CPU_TYPE == 0)	/* KW 990319 */
//		m68k_set_cpu_type(M68K_CPU_TYPE_68000);

	regs.spcflags = 0;
	regs.stopped = 0;
	regs.remainingCycles = 0;
	
	regs.intmask = 0x07;
	regs.s = 1;								// Supervisor mode ON

	// Read initial SP and PC
	m68k_areg(regs, 7) = m68k_read_memory_32(0);
	m68k_setpc(m68k_read_memory_32(4));
	refill_prefetch(m68k_getpc(), 0);
}


int m68k_execute(int num_cycles)
{
	if (regs.stopped)
	{
		regs.remainingCycles = 0;	// int32_t
		regs.interruptCycles = 0;	// uint32_t

		return num_cycles;
	}

	regs.remainingCycles = num_cycles;
	/*int32_t*/ initialCycles = num_cycles;
	
	regs.remainingCycles -= regs.interruptCycles;
	regs.interruptCycles = 0;

	/* Main loop.  Keep going until we run out of clock cycles */
	do
	{
      uint32_t opcode;
      int32_t cycles;

		// This is so our debugging code can break in on a dime.
		// Otherwise, this is just extra slow down :-P
		if (regs.spcflags & SPCFLAG_DEBUGGER)
		{
			// Not sure this is correct... :-P
			num_cycles = initialCycles - regs.remainingCycles;
			regs.remainingCycles = 0;	// int32_t
			regs.interruptCycles = 0;	// uint32_t

			return num_cycles;
		}
		if (checkForIRQToHandle)
		{
			checkForIRQToHandle = 0;
			m68k_set_irq2(IRQLevelToHandle);
		}

#ifdef M68K_HOOK_FUNCTION
		M68KInstructionHook();
#endif
		opcode = get_iword(0);
		cycles = (int32_t)(*cpuFunctionTable[opcode])(opcode);
		regs.remainingCycles -= cycles;

      //printf("Executed opcode $%04X (%i cycles)...\n", opcode, cycles);
	}
	while (regs.remainingCycles > 0);

	regs.remainingCycles -= regs.interruptCycles;
	regs.interruptCycles = 0;

	// Return # of clock cycles used
	return initialCycles - regs.remainingCycles;
}


void m68k_set_irq(unsigned int intLevel)
{
	// We need to check for stopped state as well...
	if (regs.stopped)
	{
		m68k_set_irq2(intLevel);
		return;
	}

	// Since this can be called asynchronously, we need to fix it so that it
	// doesn't fuck up the main execution loop.
	IRQLevelToHandle = intLevel;
	checkForIRQToHandle = 1;
}


/* ASG: rewrote so that the int_level is a mask of the IPL0/IPL1/IPL2 bits */
void m68k_set_irq2(unsigned int intLevel)
{
//	pthread_mutex_lock(&executionLock);
//		printf("m68k_set_irq: Could not get the lock!!!\n");

	int oldLevel = regs.intLevel;
	regs.intLevel = intLevel;

	// A transition from < 7 to 7 always interrupts (NMI)
	// Note: Level 7 can also level trigger like a normal IRQ
	if (oldLevel != 0x07 && regs.intLevel == 0x07)
		m68ki_exception_interrupt(7);		// Edge triggered level 7 (NMI)
	else
		m68ki_check_interrupts();			// Level triggered (IRQ)

//	pthread_mutex_unlock(&executionLock);
}


// Check for interrupts
static INLINE void m68ki_check_interrupts(void)
{
	if (regs.intLevel > regs.intmask)
		m68ki_exception_interrupt(regs.intLevel);
}


// Service an interrupt request and start exception processing
void m68ki_exception_interrupt(uint32_t intLevel)
{
   uint32_t vector, sr, newPC;

	// Turn off the stopped state (N.B.: normal 68K behavior!)
	regs.stopped = 0;

//JLH: need to add halt state?
// prolly, for debugging/alpine mode... :-/
// but then again, this should be handled already by the main execution loop :-P
	// If we are halted, don't do anything
//	if (regs.stopped)
//		return;

	// Acknowledge the interrupt (NOTE: This is a user supplied function!)
	vector = irq_ack_handler(intLevel);

	// Get the interrupt vector
	if (vector == M68K_INT_ACK_AUTOVECTOR)
		// Use the autovectors.  This is the most commonly used implementation
		vector = EXCEPTION_INTERRUPT_AUTOVECTOR + intLevel;
	else if (vector == M68K_INT_ACK_SPURIOUS)
		// Called if no devices respond to the interrupt acknowledge
		vector = EXCEPTION_SPURIOUS_INTERRUPT;
	else if (vector > 255)
	{
//		M68K_DO_LOG_EMU((M68K_LOG_FILEHANDLE "%s at %08x: Interrupt acknowledge returned invalid vector $%x\n",
//			 m68ki_cpu_names[CPU_TYPE], ADDRESS_68K(REG_PC), vector));
		return;
	}

	// Start exception processing
	sr = m68ki_init_exception();

	// Set the interrupt mask to the level of the one being serviced
	regs.intmask = intLevel;

	// Get the new PC
	newPC = m68k_read_memory_32(vector << 2);

	// If vector is uninitialized, call the uninitialized interrupt vector
	if (newPC == 0)
		newPC = m68k_read_memory_32(EXCEPTION_UNINITIALIZED_INTERRUPT << 2);

	// Generate a stack frame
	m68ki_stack_frame_3word(regs.pc, sr);

	m68k_setpc(newPC);

	// Defer cycle counting until later
	regs.interruptCycles += 56;	// NOT ACCURATE-- !!! FIX !!!
//	CPU_INT_CYCLES += CYC_EXCEPTION[vector];
}


// Initiate exception processing
static INLINE uint32_t m68ki_init_exception(void)
{
   uint32_t sr;

	MakeSR();
	sr = regs.sr;					// Save old status register
	regs.s = 1;								// Set supervisor mode

	return sr;
}


// 3 word stack frame (68000 only)
static INLINE void m68ki_stack_frame_3word(uint32_t pc, uint32_t sr)
{
	// Push PC on stack:
	m68k_areg(regs, 7) -= 4;
	m68k_write_memory_32(m68k_areg(regs, 7), pc);
	// Push SR on stack:
	m68k_areg(regs, 7) -= 2;
	m68k_write_memory_16(m68k_areg(regs, 7), sr);
}


unsigned int m68k_get_reg(void * context, m68k_register_t reg)
{
	if (reg <= M68K_REG_A7)
		return regs.regs[reg];
	else if (reg == M68K_REG_PC)
		return regs.pc;
	else if (reg == M68K_REG_SR)
	{
		MakeSR();
		return regs.sr;
	}
	else if (reg == M68K_REG_SP)
		return regs.regs[15];

	return 0;
}


void m68k_set_reg(m68k_register_t reg, unsigned int value)
{
	if (reg <= M68K_REG_A7)
		regs.regs[reg] = value;
	else if (reg == M68K_REG_PC)
		regs.pc = value;
	else if (reg == M68K_REG_SR)
	{
		regs.sr = value;
		MakeFromSR();
	}
	else if (reg == M68K_REG_SP)
		regs.regs[15] = value;
}


//
// Check if the instruction is a valid one
//
unsigned int m68k_is_valid_instruction(unsigned int instruction, unsigned int cpu_type)
{
	instruction &= 0xFFFF;

	if (cpuFunctionTable[instruction] == IllegalOpcode)
		return 0;

	return 1;
}


// Dummy functions, for now, until we prove the concept here. :-)

int m68k_cycles_run(void) { return 0; }              /* Number of cycles run so far */
int m68k_cycles_remaining(void) { return 0; }        /* Number of cycles left */

void m68k_modify_timeslice(int cycles)
{
	regs.remainingCycles = cycles;
}


void m68k_end_timeslice(void)
{
	initialCycles = regs.remainingCycles;
	regs.remainingCycles = 0;
}


unsigned long IllegalOpcode(uint32_t opcode)
{
	if ((opcode & 0xF000) == 0xF000)
	{
		Exception(0x0B, 0, M68000_EXC_SRC_CPU);	// LineF exception...
		return 4;
	}
	else if ((opcode & 0xF000) == 0xA000)
	{
		Exception(0x0A, 0, M68000_EXC_SRC_CPU);	// LineA exception...
		return 4;
	}

	Exception(0x04, 0, M68000_EXC_SRC_CPU);		// Illegal opcode exception...
	return 4;
}


void BuildCPUFunctionTable(void)
{
	int i;
	unsigned long opcode;

	// We're only using the "fast" 68000 emulation here, not the "compatible"
	// ("fast" doesn't throw exceptions, so we're using "compatible" now :-P)
   //let's try "compatible" and see what happens here...
	const struct cputbl * tbl = op_smalltbl_5_ff;

	// Set all instructions to Illegal...
	for(opcode=0; opcode<65536; opcode++)
		cpuFunctionTable[opcode] = IllegalOpcode;

	// Move functions from compact table into our full function table...
	for(i=0; tbl[i].handler!=NULL; i++)
		cpuFunctionTable[tbl[i].opcode] = tbl[i].handler;

//JLH: According to readcpu.c, handler is set to -1 and never changes.
// Actually, it does read this crap in readcpu.c, do_merges() does it... :-P
// Again, seems like a build time thing could be done here...
	for(opcode=0; opcode<65536; opcode++)
	{
		if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > 0)
			continue;

		if (table68k[opcode].handler != -1)
		{
			cpuop_func * f = cpuFunctionTable[table68k[opcode].handler];

			if (f == IllegalOpcode)
				abort();

			cpuFunctionTable[opcode] = f;
		}
	}
}
