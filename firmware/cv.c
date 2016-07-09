//////////////////////////////////////////////////////////////
//
//       ///// //   //          /////    /////  /////
//     //     //   //         //    // //      //   //
//    //      // //    //    //    // //      //   //
//   //      // //   ////   //    // //      //   //
//   /////   ///     //     //////   /////  //////
//
// CV.OCD MIDI-TO-CV CONVERTER
// hotchk155/2016
// Sixty Four Pixels Limited
//
// This work is distibuted under terms of Creative Commons 
// License BY-NC-SA (Attribution, Non-commercial, Share-Alike)
// https://creativecommons.org/licenses/by-nc-sa/4.0/
//
//////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////
//
// CV OUTPUT MODULE
//
//////////////////////////////////////////////////////////////

//
// INCLUDE FILES
//
#include <system.h>
#include <memory.h>
#include "cv-strip.h"

//
// MACRO DEFS
//
/*

	FSD = 5V
	
	6553.5
*/
//
// TYPE DEFS
//

// different modes CV output can operate in 
enum {
	CV_DISABLE = 0,	// disabled
	CV_NOTE,	// mapped to note input
	CV_VEL,		// mapped to note input velocity
	CV_MIDI_BEND,	// mapped to MIDI pitch bend
	CV_MIDI_TOUCH, // mapped to aftertouch
	CV_MIDI_CC,	// mapped to midi CC
	CV_MIDI_BPM, // mapped to midi CC
	CV_TEST			// mapped to test voltage	
};

typedef struct {
	byte mode;	// CV_xxx enum
	byte volts;	
	byte stack_id;
	byte out;
	char transpose;	// note offset 
} T_CV_EVENT;

typedef struct {
	byte mode;	// CV_xxx enum
	byte volts;	
	byte chan;
	byte cc;
} T_CV_MIDI;

typedef union {
	T_CV_EVENT 				event;
	T_CV_MIDI 				midi;
} CV_OUT;

//
// LOCAL DATA
//

// cache of raw DAC data
unsigned int l_dac[CV_MAX] = {0};

// CV config 
CV_OUT l_cv[CV_MAX];

// cache of the notes playing on each output
int l_note[CV_MAX];

//
// LOCAL FUNCTIONS
//

////////////////////////////////////////////////////////////
// CONFIGURE THE DAC
//static void cv_config_dac() {
//	i2c_begin_write(I2C_ADDRESS);
//	i2c_send(0b10001111); // set each channel to use internal vref
//	i2c_end();	
//
//	i2c_begin_write(I2C_ADDRESS);
//	i2c_send(0b11001111); // set x2 gain on each channel
//	i2c_end();	
//}

////////////////////////////////////////////////////////////
// STORE AN OUTPUT VALUE READY TO SEND TO DAC
// value is in volts * 65536
static void cv_update(byte which, long value) {

	// divide by full 10V output range
	value/=10;
	
	//TODO - offset/scale
	
	// limit the value
	if(value < 0) 
		value = 0;
	if(value > 65535) 
		value = 65535;
		
	// check the value has actually changed
	if(value != l_dac[which]) {
		l_dac[which] = value;
		
		// flag new data to go to DAC
		g_pending_txn->dac[which] = value;
		g_pending_txn->flags |= (1<<which);
	}
}	

////////////////////////////////////////////////////////////
// WRITE A NOTE VALUE TO A CV OUTPUT
// pitch_bend units = MIDI note * 256
static void cv_write_note(byte which, long note, int pitch_bend) {

	note <<= 8;				// get MIDI note into same units as bend
	note += pitch_bend; 	// add MIDI note and pitch bend	
	note <<=8;				// upscale to midi note * 65536
	note/=12;				// scale to volts (1V/octave)
	cv_update(which, note);
}



////////////////////////////////////////////////////////////
// WRITE A 7-BIT CC VALUE TO A CV OUTPUT
static void cv_write_7bit(byte which, byte value, byte volts) {

	// get a value which is 65536 * desired voltage
	// = 65536 * volts * (value/128)
	// = volts * value * 512
	cv_update(which, ((long)value * volts)<<9);
}

////////////////////////////////////////////////////////////
// WRITE PITCH BEND VALUE TO A CV OUTPUT
// receive raw 14bit value 
static void cv_write_bend(byte which, int value, byte volts) {	
	
	// get a value which is 65536 * desired voltage
	// = 65536 * volts * (value/16384)
	// = volts * value * 4
	cv_update(which, ((long)value * volts)<<2);
}

////////////////////////////////////////////////////////////
// WRITE VOLTS
static void cv_write_volts(byte which, byte value) {
	
	cv_update(which, ((long)value)<<16);
}

//
// GLOBAL FUNCTIONS
//

/*
////////////////////////////////////////////////////////////
// COPY CURRENT OUTPUT VALUES TO TRANSMIT BUFFER FOR DAC
void cv_dac_prepare() {	
	g_i2c_tx_buf[0] = I2C_ADDRESS<<1;
	g_i2c_tx_buf[1] = ((l_dac[1]>>8) & 0xF);
	g_i2c_tx_buf[2] = (l_dac[1] & 0xFF);
	g_i2c_tx_buf[3] = ((l_dac[3]>>8) & 0xF);
	g_i2c_tx_buf[4] = (l_dac[3] & 0xFF);
	g_i2c_tx_buf[5] = ((l_dac[2]>>8) & 0xF);
	g_i2c_tx_buf[6] = (l_dac[2] & 0xFF);
	g_i2c_tx_buf[7] = ((l_dac[0]>>8) & 0xF);
	g_i2c_tx_buf[8] = (l_dac[0] & 0xFF);
	g_i2c_tx_buf_len = 9;
	g_i2c_tx_buf_index = 0;
}*/

////////////////////////////////////////////////////////////
// HANDLE AN EVENT FROM A NOTE STACK
void cv_event(byte event, byte stack_id) {
	byte output_id;
	int note;
	NOTE_STACK *pstack;
	
	// for each CV output
	for(byte which_cv=0; which_cv<CV_MAX; ++which_cv) {
		CV_OUT *pcv = &l_cv[which_cv];
		if(pcv->event.mode == CV_DISABLE)
			continue;
		
		// is it listening to the stack sending the event?
		if(pcv->event.stack_id != stack_id)
			continue;

		// get pointer to note stack
		pstack = &g_stack[stack_id];
		
		// check the mode			
		switch(pcv->event.mode) {

		/////////////////////////////////////////////
		// CV OUTPUT TIED TO INPUT NOTE
		case CV_NOTE:	
			switch(event) {
				case EV_NOTE_A:
				case EV_NOTE_B:
				case EV_NOTE_C:
				case EV_NOTE_D:
					output_id = event - EV_NOTE_A;
					if(pcv->event.out == output_id) {			
						note = pstack->out[output_id] + pcv->event.transpose - 24;
						while(note < 0) note += 12; 	
						while(note > 96) note -= 12; 	
						l_note[which_cv] = note;
						cv_write_note(which_cv, l_note[which_cv], pstack->bend);
					}
					break;
				case EV_BEND:
					cv_write_note(which_cv, l_note[which_cv], pstack->bend);
					break;
			}
			break;
		/////////////////////////////////////////////
		// CV OUTPUT TIED TO INPUT VELOCITY
		case CV_VEL:	
			switch(event) {
				case EV_NOTE_A:
				case EV_NOTE_B:
				case EV_NOTE_C:
				case EV_NOTE_D:
					cv_write_7bit(which_cv, pstack->vel, pcv->event.volts);
					break;
			}
			break;
		}
	}
}

////////////////////////////////////////////////////////////
// HANDLE A MIDI CC
void cv_midi_cc(byte chan, byte cc, byte value) {
	for(byte which_cv=0; which_cv<CV_MAX; ++which_cv) {
		CV_OUT *pcv = &l_cv[which_cv];
		
		// is this CV output configured for CC?
		if(pcv->event.mode != CV_MIDI_CC) {
			continue;
		}		
		// does the CC number match?
		if(cc != pcv->midi.cc) {
			continue;
		}
		// does MIDI channel match
		if(!IS_CHAN(pcv->midi.chan,chan)) {
			continue;
		}		
		// OK update the output
		cv_write_7bit(which_cv, value, pcv->event.volts);
	}
}

////////////////////////////////////////////////////////////
// HANDLE MIDI AFTERTOUCH
void cv_midi_touch(byte chan, byte value) {
	for(byte which_cv=0; which_cv<CV_MAX; ++which_cv) {
		CV_OUT *pcv = &l_cv[which_cv];
		
		// is this CV output configured for CC?
		if(pcv->event.mode != CV_MIDI_TOUCH) {
			continue;
		}		
		// does MIDI channel match
		if(!IS_CHAN(pcv->midi.chan,chan)) {
			continue;
		}		
		// OK update the output
		cv_write_7bit(which_cv, value, pcv->event.volts);
	}
}

////////////////////////////////////////////////////////////
// HANDLE PITCH BEND
void cv_midi_bend(byte chan, int value)
{
	for(byte which_cv=0; which_cv<CV_MAX; ++which_cv) {
		CV_OUT *pcv = &l_cv[which_cv];
		if(pcv->event.mode != CV_MIDI_BEND) {
			continue;
		}		
		// does MIDI channel match
		if(!IS_CHAN(pcv->midi.chan,chan)) {
			continue;
		}		
		cv_write_bend(which_cv, value, pcv->event.volts);		
	}
}					

////////////////////////////////////////////////////////////
// HANDLE BPM
// BPM is upscaled by 256
void cv_midi_bpm(long value) {
	for(byte which_cv=0; which_cv<CV_MAX; ++which_cv) {
		CV_OUT *pcv = &l_cv[which_cv];
		if(pcv->event.mode != CV_MIDI_BPM) {
			continue;
		}		
		value *= (500 * pcv->event.volts);
		cv_update(which_cv, value>>16);
	}
}					
 
////////////////////////////////////////////////////////////
// CONFIGURE A CV OUTPUT
// return nonzero if any change was made
byte cv_nrpn(byte which_cv, byte param_lo, byte value_hi, byte value_lo) 
{
	if(which_cv>CV_MAX)
		return 0;
	CV_OUT *pcv = &l_cv[which_cv];
	
	switch(param_lo) {
	// SELECT SOURCE
	case NRPNL_SRC:
		switch(value_hi) {				
		case NRPVH_SRC_DISABLE:	// DISABLE
			cv_write_volts(which_cv, 0); 
			pcv->event.mode = CV_DISABLE;
			return 1;
		case NRPVH_SRC_TESTVOLTAGE:	// REFERENCE VOLTAGE
			pcv->event.mode = CV_TEST;
			pcv->event.volts = DEFAULT_CV_TEST_VOLTS;
			return 1;
		case NRPVH_SRC_MIDITICK: // BPM
			pcv->event.mode = CV_MIDI_BPM;
			pcv->event.volts = DEFAULT_CV_BPM_MAX_VOLTS;
			return 1;
		case NRPVH_SRC_MIDICC: // CC
			pcv->event.mode = CV_MIDI_CC;
			pcv->midi.chan = CHAN_GLOBAL;
			pcv->midi.cc = value_lo;
			pcv->midi.volts = DEFAULT_CV_CC_MAX_VOLTS;
			return 1;					
		case NRPVH_SRC_MIDITOUCH: // AFTERTOUCH
			pcv->event.mode = CV_MIDI_TOUCH;
			pcv->midi.chan = CHAN_GLOBAL;
			pcv->midi.volts = DEFAULT_CV_TOUCH_MAX_VOLTS;
			return 1;					
		case NRPVH_SRC_MIDIBEND: // PITCHBEND
			pcv->event.mode = CV_MIDI_BEND;
			pcv->midi.chan = CHAN_GLOBAL;
			pcv->midi.volts = DEFAULT_CV_PB_MAX_VOLTS;
			return 1;					
		case NRPVH_SRC_STACK1: // NOTE STACK 
		case NRPVH_SRC_STACK2:
		case NRPVH_SRC_STACK3:
		case NRPVH_SRC_STACK4:
			pcv->event.stack_id = value_hi - NRPVH_SRC_STACK1;		
			switch(value_lo) {
			case NRPVL_SRC_NOTE1:	// NOTE PITCH
			case NRPVL_SRC_NOTE2:
			case NRPVL_SRC_NOTE3:
			case NRPVL_SRC_NOTE4:
				pcv->event.mode = CV_NOTE;
				pcv->event.out = value_lo - NRPVL_SRC_NOTE1;
				pcv->event.transpose = 0;
				return 1;				
			case NRPVL_SRC_VEL:		// NOTE VELOCITY
				pcv->event.mode = CV_VEL;
				pcv->event.volts = DEFAULT_CV_VEL_MAX_VOLTS;
				return 1;
			}
		}
		break;
	// SELECT TRANSPOSE AMOUNT
	case NRPNL_TRANSPOSE:
		if(CV_NOTE == pcv->event.mode) {
			pcv->event.transpose = value_lo - 64;
			return 1;
		}
		break;

	// SELECT VOLTAGE RANGE
	case NRPNL_VOLTS:
		if(value_lo >= 0 && value_lo <= 8) {
			pcv->event.volts = value_lo;
			return 1;
		}
		break;	
	}
	return 0;
}

////////////////////////////////////////////////////////////
// GET CV CONFIG
byte *cv_storage(int *len) {
	*len = sizeof(l_cv);
	return (byte*)&l_cv;
}

////////////////////////////////////////////////////////////
// INITIALISE CV MODULE
void cv_init() {
	memset(l_cv, 0, sizeof(l_cv));
	memset(l_dac, 0, sizeof(l_dac));
	memset(l_note, 0, sizeof(l_note));
	//cv_config_dac();
	
	l_cv[0].event.mode = CV_NOTE;
	l_cv[0].event.out = 0;	
	l_cv[1].event.mode = CV_NOTE;
	l_cv[1].event.out = 1;	
	l_cv[2].event.mode = CV_NOTE;
	l_cv[2].event.out = 2;	
	l_cv[3].event.mode = CV_NOTE;
	l_cv[3].event.out = 3;		
}

////////////////////////////////////////////////////////////
void cv_reset() {
	for(byte which=0; which < CV_MAX; ++which) {
		switch(l_cv[which].event.mode) {				
		case CV_TEST:	
			cv_write_volts(which, l_cv[which].event.volts); // set test volts
			break;
		case CV_MIDI_BEND: 
			cv_write_bend(which, 8192, l_cv[which].event.volts); // set half full volts
			break;
		default:
			cv_write_volts(which, 0); 
			break;
		}
	}
	//g_cv_dac_pending = 1;
}

//
// END
//