/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *  
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include "fluidsynth_priv.h"
#include "fluid_voice.h"
#include "fluid_mod.h"
#include "fluid_chan.h"
#include "fluid_conv.h"
#include "fluid_synth.h"
#include "fluid_sys.h"
#include "fluid_sfont.h"

#ifndef WITH_FLOAT
#ifdef ENABLE_SSE
#error "Can't use SSE extensions with other than float type!"
#endif
#endif

#ifdef ENABLE_SSE
#include "fluid_sse.h"
float interp_coeff_sse_mem[FLUID_INTERP_MAX*4+4];
sse_t* interp_coeff_sse;
#endif

/* used for filter turn off optimization - if filter cutoff is above the
   specified value and filter q is below the other value, turn filter off */
#define FLUID_MAX_AUDIBLE_FILTER_FC 16000.0f
#define FLUID_MIN_AUDIBLE_FILTER_Q 1.2f
 
/* Smallest amplitude that can be perceived (full scale is +/- 0.5)
 * 16 bits => 96+4=100 dB dynamic range => 0.00001
 * 0.00001 * 2 is approximately 0.00003 :)
 */
#define FLUID_NOISE_FLOOR 0.00003

/* these should be the absolute minimum that FluidSynth can deal with */
#define FLUID_MIN_LOOP_SIZE 2
#define FLUID_MIN_LOOP_PAD 1

fluid_interp_coeff_t interp_coeff[FLUID_INTERP_MAX];
fluid_interp_coeff_t interp_coeff_linear[FLUID_INTERP_MAX];
fluid_real_t sinc_table7[7][FLUID_INTERP_MAX];

/*
 * fluid_voice_config
 */
void fluid_voice_config()
{
  int i;
  double x;

#ifdef ENABLE_SSE
  sse_t* sse_a;
  interp_coeff_sse=(sse_t*)FLUID_ALIGN16BYTE(&interp_coeff_sse_mem);
#endif

  /* Initialize the coefficients for the interpolation. The math comes
   * from a mail, posted by Olli Niemitalo to the music-dsp mailing
   * list (I found it in the music-dsp archives
   * http://www.smartelectronix.com/musicdsp/).  */

  for (i = 0; i < FLUID_INTERP_MAX; i++) {

    x = (double) i / (double) FLUID_INTERP_MAX;

    interp_coeff[i].a0 = (fluid_real_t) (x * (-0.5 + x * (1 - 0.5 * x)));
    interp_coeff[i].a1 = (fluid_real_t) (1.0 + x * x * (1.5 * x - 2.5));
    interp_coeff[i].a2 = (fluid_real_t) (x * (0.5 + x * (2.0 - 1.5 * x)));
    interp_coeff[i].a3 = (fluid_real_t) (0.5 * x * x * (x - 1.0));

    interp_coeff_linear[i].a0 = (fluid_real_t) (1. -x);
    interp_coeff_linear[i].a1 = (fluid_real_t) x;

#if 0
    interp_coeff[i].c0 = (signed int) (65536.0 * x * (-0.5 + x * (1 - 0.5 * x)));
    interp_coeff[i].c1 = (signed int) (65536.0 * (1.0 + x * x * (1.5 * x - 2.5)));
    interp_coeff[i].c2 = (signed int) (65536.0 * x * (0.5 + x * (2.0 - 1.5 * x)));
    interp_coeff[i].c3 = (signed int) (65536.0 * 0.5 * x * x * (x - 1.0));
#endif
#ifdef ENABLE_SSE
    sse_a=&interp_coeff_sse[i];
    sse_a->sf[0]=interp_coeff[i].a0;
    sse_a->sf[1]=interp_coeff[i].a1;
    sse_a->sf[2]=interp_coeff[i].a2;
    sse_a->sf[3]=interp_coeff[i].a3;
#endif
}

#define sinc_interp_order 7
  /* i: Offset in terms of whole samples */
  for (i = 0; i < sinc_interp_order; i++){
      int ii;
      /* ii: Offset in terms of fractional samples ('subsamples') */
      for (ii = 0; ii < FLUID_INTERP_MAX; ii++){
	  /* Move the origin into the center of the table */
      double i_shifted=(double)i-((double)sinc_interp_order)/2.
	+ (double)ii/(double)FLUID_INTERP_MAX;
      double v=1.;

	/* sinc(0) cannot be calculated straightforward (limit needed for 0/0) */
      if (fabs(i_shifted) > 0.000001) {
	v = (fluid_real_t)sin(i_shifted * M_PI) / (M_PI * i_shifted);
	/* Hamming window */
	v *= (fluid_real_t)0.5 * (1.0 + cos(2.0 * M_PI * i_shifted / (fluid_real_t)sinc_interp_order));
      }
      sinc_table7[i][FLUID_INTERP_MAX-ii-1]=v;
    }
  }
  fluid_check_fpe("interpolation table calculation");
}

/*
 * new_fluid_voice
 */ 
fluid_voice_t* 
new_fluid_voice(fluid_real_t sample_rate)
{
  fluid_voice_t* voice;
  voice = FLUID_NEW(fluid_voice_t);
  if (voice == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }
  voice->status = FLUID_VOICE_CLEAN;
  voice->chan = NO_CHANNEL;
  voice->key = 0;
  voice->vel = 0;
  voice->channel = NULL;
  voice->sample = NULL;
  voice->sample_rate = sample_rate;

  /* The 'sustain' and 'finished' segments of the volume / modulation
   * envelope are constant. They are never affected by any modulator 
   * or generator. Therefore it is enough to initialize them once
   * during the lifetime of the synth.
   */
  voice->volenv_data[FLUID_VOICE_ENVSUSTAIN].count = 0xffffffff;
  voice->volenv_data[FLUID_VOICE_ENVSUSTAIN].coeff = 1.0f;
  voice->volenv_data[FLUID_VOICE_ENVSUSTAIN].incr = 0.0f;
  voice->volenv_data[FLUID_VOICE_ENVSUSTAIN].min = -1.0f;
  voice->volenv_data[FLUID_VOICE_ENVSUSTAIN].max = 2.0f;

  voice->volenv_data[FLUID_VOICE_ENVFINISHED].count = 0xffffffff;
  voice->volenv_data[FLUID_VOICE_ENVFINISHED].coeff = 0.0f;
  voice->volenv_data[FLUID_VOICE_ENVFINISHED].incr = 0.0f;
  voice->volenv_data[FLUID_VOICE_ENVFINISHED].min = -1.0f;
  voice->volenv_data[FLUID_VOICE_ENVFINISHED].max = 1.0f;

  voice->modenv_data[FLUID_VOICE_ENVSUSTAIN].count = 0xffffffff;
  voice->modenv_data[FLUID_VOICE_ENVSUSTAIN].coeff = 1.0f;
  voice->modenv_data[FLUID_VOICE_ENVSUSTAIN].incr = 0.0f;
  voice->modenv_data[FLUID_VOICE_ENVSUSTAIN].min = -1.0f;
  voice->modenv_data[FLUID_VOICE_ENVSUSTAIN].max = 2.0f;

  voice->modenv_data[FLUID_VOICE_ENVFINISHED].count = 0xffffffff;
  voice->modenv_data[FLUID_VOICE_ENVFINISHED].coeff = 0.0f;
  voice->modenv_data[FLUID_VOICE_ENVFINISHED].incr = 0.0f;
  voice->modenv_data[FLUID_VOICE_ENVFINISHED].min = -1.0f;
  voice->modenv_data[FLUID_VOICE_ENVFINISHED].max = 1.0f;

  return voice;
}

/*
 * delete_fluid_voice
 */
int 
delete_fluid_voice(fluid_voice_t* voice)
{
  if (voice == NULL) {
    return FLUID_OK;
  }
  FLUID_FREE(voice);
  return FLUID_OK;
}

/* fluid_voice_init
 *
 * Initialize the synthesis process
 */
int 
fluid_voice_init(fluid_voice_t* voice, fluid_sample_t* sample, 
		fluid_channel_t* channel, int key, int vel, 
		unsigned int id, unsigned int start,fluid_real_t gain)
{
  /* Note: The voice parameters will be initialized later, when the
   * generators have been retrieved from the sound font. Here, only
   * the 'working memory' of the voice (position in envelopes, history
   * of IIR filters, position in sample etc) is initialized. */
  
  voice->id = id;
  voice->chan = fluid_channel_get_num(channel);
  voice->key = (unsigned char) key;
  voice->vel = (unsigned char) vel;
  voice->channel = channel;
  voice->preset = fluid_channel_get_preset(channel);
  voice->mod_count = 0;
  voice->sample = sample;
  voice->start = start;
  voice->ticks = 0;
  voice->debug = 0;
  voice->has_looped = 0; /* Will be set during voice_write when the 2nd loop point is reached */
  voice->last_fres = -1; /* The filter coefficients have to be calculated later in the DSP loop. */
  voice->filter_startup = 1; /* Set the filter immediately, don't fade between old and new settings */
  voice->interp_method = fluid_channel_get_interp_method(voice->channel);

  /* vol env initialization */
  voice->volenv_count = 0;
  voice->volenv_section = 0;
  voice->volenv_val = 0.0f;
  voice->amp = 0.0f; /* The last value of the volume envelope, used to
                        calculate the volume increment during
                        processing */  
  
  /* mod env initialization*/
  voice->modenv_count = 0;
  voice->modenv_section = 0;
  voice->modenv_val = 0.0f;

  /* mod lfo */
  voice->modlfo_val = 0.0;/* Fixme: Retrieve from any other existing
                             voice on this channel to keep LFOs in
                             unison? */

  /* vib lfo */
  voice->viblfo_val = 0.0f; /* Fixme: See mod lfo */
  
  /* Clear sample history in filter */
  voice->hist1 = 0;
  voice->hist2 = 0;

  /* Set all the generators to their default value, according to SF
   * 2.01 section 8.1.3 (page 48). The value of NRPN messages are
   * copied from the channel to the voice's generators. The sound font
   * loader overwrites them. The generator values are later converted
   * into voice parameters in
   * fluid_voice_calculate_runtime_synthesis_parameters.  */
  fluid_gen_init(&voice->gen[0], channel);
  
  voice->synth_gain = gain;
  /* avoid division by zero later*/
  if (voice->synth_gain < 0.0000001){
    voice->synth_gain = 0.0000001;
  }

  /* For a looped sample, this value will be overwritten as soon as the
   * loop parameters are initialized (they may depend on modulators). 
   * This value can be kept, it is a worst-case estimate.
   */

  voice->amplitude_that_reaches_noise_floor_nonloop = FLUID_NOISE_FLOOR / voice->synth_gain; 
  voice->amplitude_that_reaches_noise_floor_loop = FLUID_NOISE_FLOOR / voice->synth_gain; 

  /* Increment the reference count of the sample to prevent the
     unloading of the soundfont while this voice is playing. */
  fluid_sample_incr_ref(voice->sample);
  
  return FLUID_OK;
}

void fluid_voice_gen_set(fluid_voice_t* voice, int i, float val)
{
  voice->gen[i].val = val;
  voice->gen[i].flags = GEN_SET;
}

void fluid_voice_gen_incr(fluid_voice_t* voice, int i, float val)
{
  voice->gen[i].val += val;
  voice->gen[i].flags = GEN_SET;
}

float fluid_voice_gen_get(fluid_voice_t* voice, int gen)
{
  return voice->gen[gen].val;
}


/*
 * fluid_voice_write
 *
 * This is where it all happens. This function is called by the
 * synthesizer to generate the sound samples. The synthesizer passes
 * four audio buffers: left, right, reverb out, and chorus out.
 *
 * The biggest part of this function sets the correct values for all
 * the dsp parameters (all the control data boil down to only a few
 * dsp parameters). The dsp routine is #included in several places (fluid_dsp_core.c).
 */
int 
fluid_voice_write(fluid_voice_t* voice,
		 fluid_real_t* dsp_left_buf, fluid_real_t* dsp_right_buf, 
		 fluid_real_t* dsp_reverb_buf, fluid_real_t* dsp_chorus_buf)
{
  unsigned int i, start, end_in_buffer;
  fluid_real_t incr;
  fluid_real_t fres; 

  /* All variables starting with dsp_ are used by the DSP
     loop. Documented in fluid_dsp_core.c */

  int dsp_phase_index;
  unsigned int dsp_i;
  fluid_phase_t dsp_phase, dsp_phase_incr;
  fluid_real_t dsp_incr; 
  fluid_real_t dsp_amp, dsp_amp_incr, dsp_centernode, dsp_hist1, dsp_hist2;
  fluid_real_t dsp_b02, dsp_b1, dsp_a1, dsp_a2;
  fluid_real_t dsp_a1_incr;
  fluid_real_t dsp_a2_incr;
  fluid_real_t dsp_b02_incr;
  fluid_real_t dsp_b1_incr;
  fluid_interp_coeff_t* dsp_coeff;
  unsigned int dsp_start, dsp_end;
  int dsp_filter_coeff_incr_count;
  int dsp_use_filter_flag = 1;
  short* dsp_data;
  int dsp_interp_method = voice->interp_method;


#ifdef ENABLE_SSE
  float mem_for_sse_interface[5*4+4];             /* Reserve memory */
                                                  /* +4: add four floats for 16 added bytes */

  /* Align the first element */
  sse_t* sse_n = (sse_t*) FLUID_ALIGN16BYTE(&mem_for_sse_interface);   
  sse_t* sse_a = sse_n++;                         /* The ++ operator increases
						   * by the size of the structure! */
  sse_t* sse_b = sse_n++;
  sse_t* sse_c = sse_n++;
  sse_t* sse_d = sse_n++;
  sse_t* sse_e = sse_n++;
  sse_t* sse_coeff;

  sse_t* sse_src;
  sse_t* sse_dest_left;
  sse_t* sse_dest_right;
  sse_t* sse_dest;
#endif

  /* +4: add four floats for 16 added bytes */
  fluid_real_t dsp_buf_unaligned[FLUID_BUFSIZE+4];  
  fluid_real_t* dsp_buf = (fluid_real_t*) FLUID_ALIGN16BYTE(&dsp_buf_unaligned);
  fluid_env_data_t* env_data;
  fluid_real_t x;


  /* make sure we're playing and that we have sample data */
  if (!_PLAYING(voice)) {
    return FLUID_OK;
  }

  /******************* sample **********************/

  if (voice->sample == NULL) {
    fluid_voice_off(voice);
    return FLUID_OK;
  }
    
  fluid_check_fpe("voice_write startup");

  /* Range checking for sample- and loop-related parameters 
   * Initial phase is calculated here*/
  fluid_voice_check_sample_sanity(voice);

  /******************* vol env **********************/

  env_data = &voice->volenv_data[voice->volenv_section];

  /* skip to the next section of the envelope if necessary */
  while (voice->volenv_count >= env_data->count) {
    env_data = &voice->volenv_data[++voice->volenv_section];
    voice->volenv_count = 0;
  }

  /* calculate the envelope value and check for valid range */
  x = env_data->coeff * voice->volenv_val + env_data->incr;
  if (x < env_data->min) {
    x = env_data->min; 
    voice->volenv_section++;
    voice->volenv_count = 0;
  } else if (x > env_data->max) {
    x = env_data->max; 
    voice->volenv_section++;
    voice->volenv_count = 0;
  }

  voice->volenv_val = x;
  voice->volenv_count++;

  if (voice->volenv_section == FLUID_VOICE_ENVFINISHED) {
    fluid_profile(FLUID_PROF_VOICE_RELEASE, voice->ref);
    fluid_voice_off(voice);
    return FLUID_OK;
  }
  fluid_check_fpe("voice_write vol env");

  /******************* mod env **********************/

  env_data = &voice->modenv_data[voice->modenv_section];

  /* skip to the next section of the envelope if necessary */
  while (voice->modenv_count >= env_data->count) {
    env_data = &voice->modenv_data[++voice->modenv_section];
    voice->modenv_count = 0;
  }

  /* calculate the envelope value and check for valid range */
  x = env_data->coeff * voice->modenv_val + env_data->incr;

  if (x < env_data->min) {
    x = env_data->min; 
    voice->modenv_section++;
    voice->modenv_count = 0;
  } else if (x > env_data->max) {
    x = env_data->max; 
    voice->modenv_section++;
    voice->modenv_count = 0;
  }

  voice->modenv_val = x;
  voice->modenv_count++;
  fluid_check_fpe("voice_write mod env");

  /******************* mod lfo **********************/

  if (voice->ticks >= voice->modlfo_delay) {
    voice->modlfo_val += voice->modlfo_incr;
    if (voice->modlfo_val > 1.0) {
      voice->modlfo_incr = -voice->modlfo_incr;
      voice->modlfo_val = (fluid_real_t) 2.0 - voice->modlfo_val;
    } else if (voice->modlfo_val < -1.0) {
      voice->modlfo_incr = -voice->modlfo_incr;
      voice->modlfo_val = (fluid_real_t) -2.0 - voice->modlfo_val;
    }
  }
  fluid_check_fpe("voice_write mod LFO");

  /******************* vib lfo **********************/

  if (voice->ticks >= voice->viblfo_delay) {
    voice->viblfo_val += voice->viblfo_incr;
    if (voice->viblfo_val > (fluid_real_t) 1.0) {
      voice->viblfo_incr = -voice->viblfo_incr;
      voice->viblfo_val = (fluid_real_t) 2.0 - voice->viblfo_val;
    } else if (voice->viblfo_val < -1.0) {
      voice->viblfo_incr = -voice->viblfo_incr;
      voice->viblfo_val = (fluid_real_t) -2.0 - voice->viblfo_val;
    }
  }
  fluid_check_fpe("voice_write Vib LFO");
  
  /******************* amplitude **********************/

  /* calculate final amplitude 
   * - initial gain
   * - amplitude envelope
   */

  if (voice->volenv_section == FLUID_VOICE_ENVDELAY) {
    /* The volume amplitude is in hold phase. No sound is produced. */
    goto post_process;
  } else if (voice->volenv_section == FLUID_VOICE_ENVATTACK) {
    /* the envelope is in the attack section: ramp linearly to max value. */
    dsp_amp = (fluid_cb2amp(voice->attenuation + voice->modlfo_val * voice->modlfo_to_vol) 
	       * voice->volenv_val);
  } else {
    fluid_real_t amplitude_that_reaches_noise_floor;
    fluid_real_t amp_max;
    dsp_amp = fluid_cb2amp(voice->attenuation
			  + 960.0f * (1.0f - voice->volenv_val)
			  + voice->modlfo_val * voice->modlfo_to_vol);
    
    /* Here we are trying to turn off a voice, if the volume has dropped 
     * low enough.
     * Motivation:
     *   If voices are not turned off as soon as possible, the 
     *   DSP loop burns a lot of CPU time producing sounds that no one
     *   can hear
     * Problem:
     *   It would be tempting to just look at the attenuation, but then
     *   a voice terminates as soon as it is brought to 0 with a volume
     *   pedal (MIDI CC 7 or 11).
     */
    
    /* A voice can be turned off, when an estimate for the volume
     * (upper bound) falls below that volume, that will drop the
     * sample below the noise floor.
     */

    /* If the loop amplitude is known, we can use it if the voice loop is within
     * the sample loop
     */
#if 0
    printf("%i %i %i %i %i %i\n", voice->has_looped, voice->sample->amplitude_that_reaches_noise_floor_is_valid, voice->loop_start, voice->loop_end, voice->sample->loopstart, voice->sample->loopend  );
#endif
    
    /* Is the playing pointer already in the loop? */
    if (voice->has_looped){
      amplitude_that_reaches_noise_floor = voice->amplitude_that_reaches_noise_floor_loop;    
    } else {
      amplitude_that_reaches_noise_floor = voice->amplitude_that_reaches_noise_floor_nonloop;
    };
#if 0
    printf("Retrieving %f\n", amplitude_that_reaches_noise_floor);
#endif
    
    /* voice->attenuation_min is a lower boundary for the attenuation
     * now and in the future (possibly 0 in the worst case).  Now the
     * amplitude of sample and volenv cannot exceed amp_max (since
     * volenv_val can only drop):
     */
    amp_max = fluid_cb2amp(voice->min_attenuation_cB
			  + 960.0f * (1.0f - voice->volenv_val));

    /*    printf("Att min: %f Amp max: %f Limit: %f\n",voice->min_attenuation_cB,amp_max, amplitude_that_reaches_noise_floor); */
    
//    printf("Amp max: %f\n",amp_max);

    /* And if amp_max is already smaller than the known amplitude,
     * which will attenuate the sample below the noise floor, then we
     * can safely turn off the voice. Duh. */
    if (amp_max < amplitude_that_reaches_noise_floor){
      fluid_profile(FLUID_PROF_VOICE_RELEASE, voice->ref);
#if 0
      printf("Voice turned off! Amp is %f\n", amp_max);
#endif
      fluid_voice_off(voice);
      goto post_process;    
    }
  }
  
  /* At this point, dsp_amp is the desired amplitude for the voice.
   * This value will be reached at the end of the next buffer.  So we
   * raise the amplitude smoothly from the current voice amplitude
   * (voice->amp) to the wanted amplitude dsp_amp.
   *
   * By how much do we have to increase voice->amp, so that it reaches
   * dsp_amp, when the increment is added FLUID_BUFSIZE times? */
  dsp_amp_incr = (dsp_amp - voice->amp) / FLUID_BUFSIZE;

  /* dsp_amp will now be fed into the DSP loop.  Use the amplitude,
   * that came out of the last DSP loop run. */
  dsp_amp = voice->amp;
  
  fluid_check_fpe("voice_write amplitude calculation");
  if ((dsp_amp == 0.0f) && (dsp_amp_incr == 0.0f)) {
    goto post_process;    
  }

  /* Calculate the number of samples, that the DSP loop advances
   * through the original waveform with each step in the output
   * buffer. It is the ratio between the frequencies of original
   * waveform and output waveform.*/
  incr = fluid_ct2hz(voice->pitch
		    + voice->modlfo_val * voice->modlfo_to_pitch
		    + voice->viblfo_val * voice->viblfo_to_pitch
		    + voice->modenv_val * voice->modenv_to_pitch) / voice->root_pitch;

  /* Transfer the phase from the voice into the dsp loop parameter
     dsp_phase */
  fluid_phase_set(dsp_phase, voice->phase);

  /* Convert the 'speed' through the original waveform to a
   * representation 'integer part' and 'fractional part' */
  fluid_phase_set_float(dsp_phase_incr, incr);
  incr *= 1.00000000000001; /* gcc optimization problem. Quick fix. */
  fluid_check_fpe("voice_write phase calculation");

  /* Check, if we are really making progress through the original
   * sample.  If the step size is rounded to 0, the DSP loop would get
   * stuck. */
  if (fluid_phase_index(dsp_phase) == 0 && fluid_phase_fract(dsp_phase) == 0){
      fluid_phase_fract(dsp_phase)=1;
  }

  /*************** resonant filter ******************/

  /* calculate the frequency of the resonant filter in Hz */
  fres = fluid_ct2hz(voice->fres 
		    + voice->modlfo_val * voice->modlfo_to_fc 
		    + voice->modenv_val * voice->modenv_to_fc);

  /* Testcase for filter resonance: Use the following line, and press
   * A3. There should be a _very_ pronounced resonant peak, which
   * drops down, when moving the pitch bend wheel only slightly. */
  //fres=440;voice->q_lin=100;

  if (fres < 5) {
      fres = 5;
  } else if ((fres > FLUID_MAX_AUDIBLE_FILTER_FC)
	     && (voice->q_lin < FLUID_MIN_AUDIBLE_FILTER_Q)) {
    /* Beyond a certain point the filter does not change the sound
     * much.  Turn it off to save CPU cycles. */
    /* Fixme: When the filter has been on once for a voice, keep it on forever. */
    dsp_use_filter_flag = 0;
  }
   
   if (dsp_use_filter_flag && (abs(fres - voice->last_fres) > 0.01)) {
     
     /* The filter coefficients have to be recalculated (filter
      * parameters have changed). Recalculation for various reasons is
      * forced by setting last_fres to -1.  The flag filter_startup
      * indicates, that the DSP loop runs for the first time, in this
      * case, the filter is set directly, instead of smoothly fading
      * between old and new settings.
      *
      * Those equations from Robert Bristow-Johnson's `Cookbook
      * formulae for audio EQ biquad filter coefficients', obtained
      * from Harmony-central.com / Computer / Programming. They are
      * the result of the bilinear transform on an analogue filter
      * prototype. To quote, `BLT frequency warping has been taken
      * into account for both significant frequency relocation and for
      * bandwidth readjustment'. */
     
     fluid_real_t omega = (fluid_real_t) (2.0 * M_PI * (fres / 44100.0f));
     fluid_real_t sin_coeff = (fluid_real_t) sin(omega);
     fluid_real_t cos_coeff = (fluid_real_t) cos(omega);
     fluid_real_t alpha_coeff = sin_coeff / (2.0f * voice->q_lin);
     fluid_real_t a0_inv = 1.0f / (1.0f + alpha_coeff); 
     
     /* Calculate the filter coefficients. All coefficients are
      * normalized by a0. Think of `a1' as `a1/a0'.
      *
      * Here a couple of multiplications are saved by reusing common expressions. 
      * The original equations should be:
      *  voice->b0=(1.-cos_coeff)*a0_inv*0.5*voice->filter_gain;
      *  voice->b1=(1.-cos_coeff)*a0_inv*voice->filter_gain;
      *  voice->b2=(1.-cos_coeff)*a0_inv*0.5*voice->filter_gain; */
     
     fluid_real_t a1_temp = -2.0f * cos_coeff * a0_inv;
     fluid_real_t a2_temp = (1.0f - alpha_coeff) * a0_inv;
     fluid_real_t b1_temp = (1.0f - cos_coeff) * a0_inv * voice->filter_gain;
     /* both b0 -and- b2 */
     fluid_real_t b02_temp = b1_temp * 0.5f;

     if (voice->filter_startup){
       /* The filter is calculated, because the voice was started up.
	* In this case set the filter coefficients without delay.
	*/
       voice->a1 = a1_temp;
       voice->a2 = a2_temp;
       voice->b02 = b02_temp;
       voice->b1 = b1_temp; 
       voice->filter_coeff_incr_count = 0;
       voice->filter_startup = 0;
//       printf("Setting initial filter coefficients.\n");
     } else {

       /* The filter frequency is changed.  Calculate an increment
	* factor, so that the new setting is reached after one buffer
	* length. x_incr is added to the current value FLUID_BUFSIZE
	* times. The length is arbitrarily chosen. Longer than one
	* buffer will sacrifice some performance, though.  Note: If
	* the filter is still too 'grainy', then increase this number
	* at will.
	*/

#define FILTER_TRANSITION_SAMPLES (FLUID_BUFSIZE)

       voice->a1_incr = (a1_temp - voice->a1) / FILTER_TRANSITION_SAMPLES;
       voice->a2_incr = (a2_temp - voice->a2) / FILTER_TRANSITION_SAMPLES;
       voice->b02_incr = (b02_temp - voice->b02) / FILTER_TRANSITION_SAMPLES;
       voice->b1_incr = (b1_temp - voice->b1) / FILTER_TRANSITION_SAMPLES;
       /* Have to add the increments filter_coeff_incr_count times. */
       voice->filter_coeff_incr_count = FILTER_TRANSITION_SAMPLES;
     }
     voice->last_fres = fres;
     fluid_check_fpe("voice_write filter calculation");
   }

   /* Now we set up the variables, that go into the DSP routine. For documentation,
    * see fluid_dsp_core.c
    */
   /* Sample waveform data */
   dsp_data = voice->sample->data;
   
   /* IIR filter sample history */
   dsp_hist1 = voice->hist1;
   dsp_hist2 = voice->hist2;

   /* IIR filter coefficients */
   dsp_a1 = voice->a1;
   dsp_a2 = voice->a2;
   dsp_b02 = voice->b02;
   dsp_b1 = voice->b1;
   dsp_a1_incr = voice->a1_incr;
   dsp_a2_incr = voice->a2_incr;
   dsp_b02_incr = voice->b02_incr;
   dsp_b1_incr = voice->b1_incr;
   dsp_filter_coeff_incr_count = voice->filter_coeff_incr_count;
   
   fluid_check_fpe("voice_write DSP coefficients");

  /*********************** run the dsp chain ************************
   * The sample is mixed with the output buffer. 
   * The buffer has to be filled from 0 to FLUID_BUFSIZE-1.
   * Depending on the position in the loop and the loop size, this 
   * may require several runs. */
  fluid_check_fpe("voice_write DSP processing");

  if (((_SAMPLEMODE(voice) == FLUID_LOOP) && (voice->volenv_section < FLUID_VOICE_ENVRELEASE)) 
      || (_SAMPLEMODE(voice) == FLUID_LOOP_DURING_RELEASE)) {

    /* At which index does the loop point occur in the output buffer?
     * This calculates the first index in the buffer, which uses
     * sample data taken after the looparound. */
    end_in_buffer = fluid_phase_steps(dsp_phase, voice->loop_end, incr);
    
    if (end_in_buffer >= FLUID_BUFSIZE) {
      /* The loop occurs after the end of the buffer. 
       * Process the whole buffer in one run. */
      dsp_start = 0;
      dsp_end = FLUID_BUFSIZE;
#include "fluid_dsp_core.c"

    } else {
      /* The loop occurs during the current buffer length. Note, that
       * this method is unable to cope with a 'skipped' loop point.  *
       * If, by means of witchcraft, evil magic or modulators, we end
       * up beyond * the loop point, a SEGV is unavoidable.  * We
       * can't even detect this here, because the calculations are
       * done using * unsigned ints (only positive), and end_in_buffer
       * would be negative * for a skipped loop point...*/ 
      start = 0;

      while (end_in_buffer < FLUID_BUFSIZE) {

	dsp_start = start;
	dsp_end = end_in_buffer;
#include "fluid_dsp_core.c"
      
	/* loop */
	fluid_phase_sub_int(dsp_phase, voice->loop_end - voice->loop_start); 
	start = end_in_buffer;
	end_in_buffer += fluid_phase_steps(dsp_phase, voice->loop_end, incr);
      }
      voice->has_looped=1;
      dsp_start = start;
      dsp_end = FLUID_BUFSIZE;
#include "fluid_dsp_core.c"
    }
    
  } else {
    /* Not looping right now. */
    
    dsp_start = 0;
    end_in_buffer = fluid_phase_steps(dsp_phase, voice->sample_end, incr);

    if (end_in_buffer >= FLUID_BUFSIZE) {
      /* Run the whole buffer at once */
      dsp_end = FLUID_BUFSIZE;
#include "fluid_dsp_core.c"
    } else {
      /* The sample ends in the middle of the buffer length. 
       * Process that far, and turn the voice off. 
       */
      dsp_end = end_in_buffer;
#include "fluid_dsp_core.c"
      fluid_profile(FLUID_PROF_VOICE_RELEASE, voice->ref);
      fluid_voice_off(voice);
      goto post_process;
    }
  }

  /*************** finishing ******************/
  /* copy back the state for the next cycle */

  voice->hist1 = dsp_hist1;
  voice->hist2 = dsp_hist2;
  voice->phase = dsp_phase;
  voice->amp = dsp_amp;
  voice->a1 = dsp_a1;
  voice->a2 = dsp_a2;
  voice->b02 = dsp_b02;
  voice->b1 = dsp_b1;
  voice->filter_coeff_incr_count = dsp_filter_coeff_incr_count;

/*   if (dsp_filter_coeff_incr_count) { */
/*     printf("ticks = %d, dsp_filter_coeff_incr_count = %d\n",  */
/* 	   voice->ticks, dsp_filter_coeff_incr_count); */
/*   } */

 post_process:
  voice->ticks += FLUID_BUFSIZE;
  fluid_check_fpe("voice_write postprocess");
  return FLUID_OK;
}

/*
 * fluid_voice_get_channel
 */
fluid_channel_t* 
fluid_voice_get_channel(fluid_voice_t* voice)
{
  return voice->channel;
}

/*
 * fluid_voice_start
 */
void fluid_voice_start(fluid_voice_t* voice)
{
  /* The maximum volume of the loop is calculated and cached once for each 
   * sample with its nominal loop settings. This happens, when the sample is used 
   * for the first time.*/ 
    
  fluid_voice_calculate_runtime_synthesis_parameters(voice);
  
  /* Force setting of the phase at the first DSP loop run
   * This cannot be done earlier, because it depends on modulators.*/
  voice->check_sample_sanity_flag=FLUID_SAMPLESANITY_STARTUP;

  voice->ref = fluid_profile_ref();

  voice->status = FLUID_VOICE_ON;
}

/*
 * fluid_voice_calculate_runtime_synthesis_parameters
 *
 * in this function we calculate the values of all the parameters. the
 * parameters are converted to their most useful unit for the DSP
 * algorithm, for example, number of samples instead of
 * timecents. Some parameters keep their "perceptual" unit and
 * conversion will be done in the DSP function. This is the case, for
 * example, for the pitch since it is modulated by the controllers in
 * cents. */
int 
fluid_voice_calculate_runtime_synthesis_parameters(fluid_voice_t* voice)
{
  fluid_real_t x;
  fluid_real_t q_db;
  int i;

  int list_of_generators_to_initialize[35] = {
    GEN_STARTADDROFS,                    /* SF2.01 page 48 #0   */
    GEN_ENDADDROFS,                      /*                #1   */
    GEN_STARTLOOPADDROFS,                /*                #2   */
    GEN_ENDLOOPADDROFS,                  /*                #3   */
    /* GEN_STARTADDRCOARSEOFS see comment below [1]        #4   */
    GEN_MODLFOTOPITCH,                   /*                #5   */
    GEN_VIBLFOTOPITCH,                   /*                #6   */
    GEN_MODENVTOPITCH,                   /*                #7   */
    GEN_FILTERFC,                        /*                #8   */
    GEN_FILTERQ,                         /*                #9   */
    GEN_MODLFOTOFILTERFC,                /*                #10  */
    GEN_MODENVTOFILTERFC,                /*                #11  */
    /* GEN_ENDADDRCOARSEOFS [1]                            #12  */
    GEN_MODLFOTOVOL,                     /*                #13  */
    /* not defined                                         #14  */
    GEN_CHORUSSEND,                      /*                #15  */
    GEN_REVERBSEND,                      /*                #16  */
    GEN_PAN,                             /*                #17  */
    /* not defined                                         #18  */
    /* not defined                                         #19  */
    /* not defined                                         #20  */
    GEN_MODLFODELAY,                     /*                #21  */
    GEN_MODLFOFREQ,                      /*                #22  */
    GEN_VIBLFODELAY,                     /*                #23  */
    GEN_VIBLFOFREQ,                      /*                #24  */
    GEN_MODENVDELAY,                     /*                #25  */
    GEN_MODENVATTACK,                    /*                #26  */
    GEN_MODENVHOLD,                      /*                #27  */
    GEN_MODENVDECAY,                     /*                #28  */
    /* GEN_MODENVSUSTAIN [1]                               #29  */
    GEN_MODENVRELEASE,                   /*                #30  */
    /* GEN_KEYTOMODENVHOLD [1]                             #31  */
    /* GEN_KEYTOMODENVDECAY [1]                            #32  */
    GEN_VOLENVDELAY,                     /*                #33  */
    GEN_VOLENVATTACK,                    /*                #34  */
    GEN_VOLENVHOLD,                      /*                #35  */
    GEN_VOLENVDECAY,                     /*                #36  */
    /* GEN_VOLENVSUSTAIN [1]                               #37  */
    GEN_VOLENVRELEASE,                   /*                #38  */
    /* GEN_KEYTOVOLENVHOLD [1]                             #39  */
    /* GEN_KEYTOVOLENVDECAY [1]                            #40  */
    /* GEN_STARTLOOPADDRCOARSEOFS [1]                      #45  */
    GEN_KEYNUM,                          /*                #46  */
    GEN_VELOCITY,                        /*                #47  */
    GEN_ATTENUATION,                     /*                #48  */
    /* GEN_ENDLOOPADDRCOARSEOFS [1]                        #50  */ 
    /* GEN_COARSETUNE           [1]                        #51  */
    /* GEN_FINETUNE             [1]                        #52  */
    GEN_OVERRIDEROOTKEY,                 /*                #58  */
    GEN_PITCH,                           /*                ---  */
    -1};                                 /* end-of-list marker  */
  
  /* When the voice is made ready for the synthesis process, a lot of
   * voice-internal parameters have to be calculated.
   *
   * At this point, the sound font has already set the -nominal- value
   * for all generators (excluding GEN_PITCH). Most generators can be
   * modulated - they include a nominal value and an offset (which
   * changes with velocity, note number, channel parameters like
   * aftertouch, mod wheel...) Now this offset will be calculated as
   * follows:
   *
   *  - Process each modulator once.
   *  - Calculate its output value.
   *  - Find the target generator.
   *  - Add the output value to the modulation value of the generator.
   *
   * Note: The generators have been initialized with
   * fluid_gen_set_default_values.
   */

  for (i = 0; i < voice->mod_count; i++) {
    fluid_mod_t* mod = &voice->mod[i];
    fluid_real_t modval = fluid_mod_get_value(mod, voice->channel, voice);
    int dest_gen_index = mod->dest;
    fluid_gen_t* dest_gen = &voice->gen[dest_gen_index];
    dest_gen->mod += modval;
    /*      fluid_dump_modulator(mod); */
  }  

  /* The GEN_PITCH is a hack to fit the pitch bend controller into the
   * modulator paradigm.  Now the nominal pitch of the key is set.
   * Note about SCALETUNE: SF2.01 8.1.3 says, that this generator is a
   * non-realtime parameter. So we don't allow modulation (as opposed
   * to _GEN(voice, GEN_SCALETUNE) When the scale tuning is varied,
   * one key remains fixed. Here C3 (MIDI number 60) is used.
   */
  if (fluid_channel_has_tuning(voice->channel)) {
    /* pitch(60) + scale * (pitch(key) - pitch(60)) */
    #define __pitch(_k) fluid_tuning_get_pitch(tuning, _k)
    fluid_tuning_t* tuning = fluid_channel_get_tuning(voice->channel);
    voice->gen[GEN_PITCH].val = (__pitch(60) + (voice->gen[GEN_SCALETUNE].val / 100.0f * 
					   (__pitch(voice->key) - __pitch(60))));
  } else {
    voice->gen[GEN_PITCH].val = (voice->gen[GEN_SCALETUNE].val * (voice->key - 60.0f) 
				 + 100.0f * 60.0f);
  }

  /* Now the generators are initialized, nominal and modulation value.
   * The voice parameters (which depend on generators) are calculated
   * with fluid_voice_update_param. Processing the list of generator
   * changes will calculate each voice parameter once.
   *
   * Note [1]: Some voice parameters depend on several generators. For
   * example, the pitch depends on GEN_COARSETUNE, GEN_FINETUNE and
   * GEN_PITCH.  voice->pitch.  Unnecessary recalculation is avoided
   * by removing all but one generator from the list of voice
   * parameters.  Same with GEN_XXX and GEN_XXXCOARSE: the
   * initialisation list contains only GEN_XXX.
   */
  
  /* Calculate the voice parameter(s) dependent on each generator. */
  for (i = 0; list_of_generators_to_initialize[i] != -1; i++) {
    fluid_voice_update_param(voice, list_of_generators_to_initialize[i]);
  }

  /* Make an estimate on how loud this voice can get at any time (attenuation). */
  voice->min_attenuation_cB = fluid_voice_get_lower_boundary_for_attenuation(voice);
    
  return FLUID_OK;
}

/* 
 * calculate_hold_decay_buffers
 */
int calculate_hold_decay_buffers(fluid_voice_t* voice, int gen_base, 
				 int gen_key2base, int is_decay)
{
  /* Purpose:
   *
   * Returns the number of DSP loops, that correspond to the hold
   * (is_decay=0) or decay (is_decay=1) time.
   * gen_base=GEN_VOLENVHOLD, GEN_VOLENVDECAY, GEN_MODENVHOLD,
   * GEN_MODENVDECAY gen_key2base=GEN_KEYTOVOLENVHOLD,
   * GEN_KEYTOVOLENVDECAY, GEN_KEYTOMODENVHOLD, GEN_KEYTOMODENVDECAY
   */
  
  fluid_real_t timecents;
  fluid_real_t seconds;
  int buffers;

  /* SF2.01 section 8.4.3 # 31, 32, 39, 40
   * GEN_KEYTOxxxENVxxx uses key 60 as 'origin'.
   * The unit of the generator is timecents per key number.
   * If KEYTOxxxENVxxx is 100, a key one octave over key 60 (72)
   * will cause (60-72)*100=-1200 timecents of time variation.
   * The time is cut in half.
   */
  timecents = (_GEN(voice, gen_base) + _GEN(voice, gen_key2base) * (60.0 - voice->key));
  
  /* Range checking */
  if (is_decay){
    /* SF 2.01 section 8.1.3 # 28, 36 */
    if (timecents > 8000.0) {
      timecents = 8000.0;
    }
  } else {
    /* SF 2.01 section 8.1.3 # 27, 35 */
    if (timecents > 5000) { 
      timecents = 5000.0;
    }
    /* SF 2.01 section 8.1.2 # 27, 35:
     * The most negative number indicates no hold time 
     */
    if (timecents <= -32768.) {
      return 0;
    }
  }
  /* SF 2.01 section 8.1.3 # 27, 28, 35, 36 */
  if (timecents < -12000.0) {
    timecents = -12000.0;
  }
  
  seconds = fluid_tc2sec(timecents);
  /* Each DSP loop processes FLUID_BUFSIZE samples. */
 
  /* round to next full number of buffers */
  buffers = (int)(((fluid_real_t)voice->sample_rate * seconds) 
		  / (fluid_real_t)FLUID_BUFSIZE
		  +0.5);

  return buffers;
}

/*
 * fluid_voice_update_param
 *
 * Purpose: 
 *
 * The value of a generator (gen) has changed.  (The different
 * generators are listed in fluidsynth.h, or in SF2.01 page 48-49)
 * Now the dependent 'voice' parameters are calculated.
 *
 * fluid_voice_update_param can be called during the setup of the
 * voice (to calculate the initial value for a voice parameter), or
 * during its operation (a generator has been changed due to
 * real-time parameter modifications like pitch-bend).
 *
 * Note: The generator holds three values: The base value .val, an
 * offset caused by modulators .mod, and an offset caused by the
 * NRPN system. _GEN(voice, generator_enumerator) returns the sum
 * of all three.
 */
void 
fluid_voice_update_param(fluid_voice_t* voice, int gen)
{
  double q_dB;
  fluid_real_t x;
  fluid_real_t y;
  unsigned int count;

  switch (gen) {
	
  case GEN_PAN:
    /* range checking is done in the fluid_pan function */
    voice->pan = _GEN(voice, GEN_PAN);
    voice->amp_left = fluid_pan(voice->pan, 1) * voice->synth_gain / 32768.0f;
    voice->amp_right = fluid_pan(voice->pan, 0) * voice->synth_gain / 32768.0f;
    break;
	    
  case GEN_ATTENUATION:
    voice->attenuation = _GEN(voice, GEN_ATTENUATION);

    /* Range: SF2.01 section 8.1.3 # 48 
     * Motivation for range checking: 
     * OHPiano.SF2 sets initial attenuation to a whooping -96 dB */
    fluid_clip(voice->attenuation, 0.0, 1440.0);
    break;
	  
    /* The pitch is calculated from three different generators. 
     * Read comment in fluidsynth.h about GEN_PITCH. 
     */
  case GEN_PITCH:
  case GEN_COARSETUNE:
  case GEN_FINETUNE:
    /* The testing for allowed range is done in 'fluid_ct2hz' */
    voice->pitch = (_GEN(voice, GEN_PITCH) 
		    + 100.0f * _GEN(voice, GEN_COARSETUNE) 
		    + _GEN(voice, GEN_FINETUNE)); 
    break;

  case GEN_REVERBSEND:
    /* The generator unit is 'tenths of a percent'. */
    voice->reverb_send = _GEN(voice, GEN_REVERBSEND) / 1000.0f;
    fluid_clip(voice->reverb_send, 0.0, 1.0);
    voice->amp_reverb = voice->reverb_send * voice->synth_gain / 32768.0f;
    break;

  case GEN_CHORUSSEND:
    /* The generator unit is 'tenths of a percent'. */
    voice->chorus_send = _GEN(voice, GEN_CHORUSSEND) / 1000.0f;
    fluid_clip(voice->chorus_send, 0.0, 1.0);
    voice->amp_chorus = voice->chorus_send * voice->synth_gain / 32768.0f;  
    break;

  case GEN_OVERRIDEROOTKEY:
    /* This is a non-realtime parameter. Therefore the .mod part of the generator
     * can be neglected. */
    if (voice->gen[GEN_OVERRIDEROOTKEY].val > -1) {   //FIXME: use flag instead of -1
      voice->root_pitch = 100.0f * voice->gen[GEN_OVERRIDEROOTKEY].val;
    } else {
      voice->root_pitch = voice->sample->origpitch * 100.0f + voice->sample->pitchadj;
    }
    voice->root_pitch = fluid_ct2hz(voice->root_pitch);
    if (voice->sample != NULL) {
      voice->root_pitch *= (fluid_real_t) voice->sample_rate / voice->sample->samplerate;
    }
    break;

  case GEN_FILTERFC:
    /* The resonance frequency is converted from absolute cents to midicents
     * .val and .mod are both used, this permits real-time modulation.
     * The allowed range is tested in the 'fluid_ct2hz' function [PH,20021214] 
     */
    voice->fres = _GEN(voice, GEN_FILTERFC);

    /* The synthesis loop will have to recalculate the filter coefficients. */
    voice->last_fres = -1.0f;
    break;

  case GEN_FILTERQ:
    /* The generator contains 'centibels' (1/10 dB) => divide by 10 to obtain dB */
    q_dB = _GEN(voice, GEN_FILTERQ) / 10.0f;
	    
    /* Range: SF2.01 section 8.1.3 # 8 (convert from cB to dB => /10) */
    fluid_clip(q_dB, 0.0f, 96.0f);
	    
    /* Short version: Modify the Q definition in a way, that a Q of 0
     * dB leads to no resonance hump in the freq. response.
     *
     * Long version: From SF2.01, page 39, item 9 (initialFilterQ):
     * "The gain at the cutoff frequency may be less than zero when
     * zero is specified".  Assume q_dB=0 / q_lin=1: If we would leave
     * q as it is, then this results in a 3 dB hump slightly below
     * fc. At fc, the gain is exactly the DC gain (0 dB).  What is
     * (probably) meant here is that the filter does not show a
     * resonance hump for q_dB=0. In this case, the corresponding
     * q_lin is 1/sqrt(2)=0.707.  The filter should have 3 dB of
     * attenuation at fc now.  In this case Q_dB is the height of the
     * resonance peak not over the DC gain, but over the frequency
     * response of a non-resonant filter.  This idea is implemented as
     * follows: */
    q_dB -= 3.01f;
	    
    /* The 'sound font' Q is defined in dB. The filter needs a linear
       q. Convert. */
    voice->q_lin = (fluid_real_t) (pow(10.0f, q_dB / 20.0f));

    /* SF 2.01 page 59:
     *
     *  The SoundFont specs ask for a gain reduction equal to half the
     *  height of the resonance peak (Q).  For example, for a 10 dB
     *  resonance peak, the gain is reduced by 5 dB.  This is done by
     *  multiplying the total gain with sqrt(1/Q).  `Sqrt' divides dB
     *  by 2 (100 lin = 40 dB, 10 lin = 20 dB, 3.16 lin = 10 dB etc)
     *  The gain is later factored into the 'b' coefficients
     *  (numerator of the filter equation).  This gain factor depends
     *  only on Q, so this is the right place to calculate it.
     */
    voice->filter_gain = (fluid_real_t) (1.0 / sqrt(voice->q_lin));
		
    /* The synthesis loop will have to recalculate the filter coefficients. */
    voice->last_fres = -1.;
    break;
		
  case GEN_MODLFOTOPITCH:
    voice->modlfo_to_pitch = _GEN(voice, GEN_MODLFOTOPITCH);
    fluid_clip(voice->modlfo_to_pitch, -12000.0, 12000.0);
    break;
		
  case GEN_MODLFOTOVOL:
    voice->modlfo_to_vol = _GEN(voice, GEN_MODLFOTOVOL);
    fluid_clip(voice->modlfo_to_vol, -960.0, 960.0);
    break;

  case GEN_MODLFOTOFILTERFC:
    voice->modlfo_to_fc = _GEN(voice, GEN_MODLFOTOFILTERFC);
    fluid_clip(voice->modlfo_to_fc, -12000, 12000);
    break;
		
  case GEN_MODLFODELAY:
    x = _GEN(voice, GEN_MODLFODELAY);
    fluid_clip(x, -12000.0f, 5000.0f);
    voice->modlfo_delay = (unsigned int) (voice->sample_rate * fluid_tc2sec_delay(x));
    break;

  case GEN_MODLFOFREQ:
    /* - the frequency is converted into a delta value, per buffer of FLUID_BUFSIZE samples
     * - the delay into a sample delay
     */
    x = _GEN(voice, GEN_MODLFOFREQ);
    fluid_clip(x, -16000.0f, 4500.0f);
    voice->modlfo_incr = (4.0f * FLUID_BUFSIZE * fluid_act2hz(x) / voice->sample_rate);
    break;

  case GEN_VIBLFOFREQ:
    /* vib lfo 
     *
     * - the frequency is converted into a delta value, per buffer of FLUID_BUFSIZE samples
     * - the delay into a sample delay
     */
    x = _GEN(voice, GEN_VIBLFOFREQ);
    fluid_clip(x, -16000.0f, 4500.0f);
    voice->viblfo_incr = (4.0f * FLUID_BUFSIZE * fluid_act2hz(x) / voice->sample_rate);
    break;
		
  case GEN_VIBLFODELAY:
    x = _GEN(voice,GEN_VIBLFODELAY);
    fluid_clip(x, -12000.0f, 5000.0f);
    voice->viblfo_delay = (unsigned int) (voice->sample_rate * fluid_tc2sec_delay(x));
    break;
			
  case GEN_VIBLFOTOPITCH:
    voice->viblfo_to_pitch = _GEN(voice, GEN_VIBLFOTOPITCH);
    fluid_clip(voice->viblfo_to_pitch, -12000.0, 12000.0);
    break;
		
  case GEN_KEYNUM:
    /* GEN_KEYNUM: SF2.01 page 46, item 46
     *
     * If this generator is active, it forces the key number to its
     * value.  Non-realtime controller.
     *
     * There is a flag, which should indicate, whether a generator is
     * enabled or not.  But here we rely on the default value of -1.
     * */
    x = _GEN(voice, GEN_KEYNUM);
    if (x >= 0){
      voice->key = x;
    }
    break;
	    
  case GEN_VELOCITY:
    /* GEN_VELOCITY: SF2.01 page 46, item 47
     *
     * If this generator is active, it forces the velocity to its
     * value. Non-realtime controller.
     *
     * There is a flag, which should indicate, whether a generator is
     * enabled or not. But here we rely on the default value of -1.  */
    x = _GEN(voice, GEN_VELOCITY);
    if (x > 0) {
      voice->vel = x;
    }
    break;
	    
  case GEN_MODENVTOPITCH:
    voice->modenv_to_pitch = _GEN(voice, GEN_MODENVTOPITCH);
    fluid_clip(voice->modenv_to_pitch, -12000.0, 12000.0);
    break;
		
  case GEN_MODENVTOFILTERFC:
    voice->modenv_to_fc = _GEN(voice,GEN_MODENVTOFILTERFC);

    /* Range: SF2.01 section 8.1.3 # 1 
     * Motivation for range checking: 
     * Filter is reported to make funny noises now and then
     */
    fluid_clip(voice->modenv_to_fc, -12000.0, 12000.0);
    break;
    
    
    /* sample start and ends points 
     * 
     * Range checking is initiated via the 
     * voice->check_sample_sanity flag,
     * because it is impossible to check here:
     * During the voice setup, all modulators are processed, while
     * the voice is inactive. Therefore, illegal settings may
     * occur during the setup (for example: First move the loop
     * end point ahead of the loop start point => invalid, then
     * move the loop start point forward => valid again. 
     */
  case GEN_STARTADDROFS:              /* SF2.01 section 8.1.3 # 0 */
  case GEN_STARTADDRCOARSEOFS:        /* SF2.01 section 8.1.3 # 4 */
    if (voice->sample != NULL) {
      voice->sample_start = (voice->sample->start
			     + (int) _GEN(voice, GEN_STARTADDROFS)
			     + 32768 * (int) _GEN(voice, GEN_STARTADDRCOARSEOFS));
      voice->check_sample_sanity_flag = FLUID_SAMPLESANITY_CHECK;
    }
    break;
  case GEN_ENDADDROFS:                 /* SF2.01 section 8.1.3 # 1 */
  case GEN_ENDADDRCOARSEOFS:           /* SF2.01 section 8.1.3 # 12 */
    if (voice->sample != NULL) { 
      voice->sample_end = (voice->sample->end
			   + (int) _GEN(voice, GEN_ENDADDROFS)
			   + 32768 * (int) _GEN(voice, GEN_ENDADDRCOARSEOFS));
      voice->check_sample_sanity_flag = FLUID_SAMPLESANITY_CHECK;
    }
    break;
  case GEN_STARTLOOPADDROFS:           /* SF2.01 section 8.1.3 # 2 */
  case GEN_STARTLOOPADDRCOARSEOFS:     /* SF2.01 section 8.1.3 # 45 */
    if (voice->sample != NULL) {
      voice->loop_start = (voice->sample->loopstart
				  + (int) _GEN(voice, GEN_STARTLOOPADDROFS)
				  + 32768 * (int) _GEN(voice, GEN_STARTLOOPADDRCOARSEOFS));
      voice->check_sample_sanity_flag = FLUID_SAMPLESANITY_CHECK;
    }
    break;
    
  case GEN_ENDLOOPADDROFS:             /* SF2.01 section 8.1.3 # 3 */
  case GEN_ENDLOOPADDRCOARSEOFS:       /* SF2.01 section 8.1.3 # 50 */
    if (voice->sample != NULL) {
      voice->loop_end = (voice->sample->loopend
				+ (int) _GEN(voice, GEN_ENDLOOPADDROFS)
				+ 32768 * (int) _GEN(voice, GEN_ENDLOOPADDRCOARSEOFS));
      voice->check_sample_sanity_flag = FLUID_SAMPLESANITY_CHECK;
    }
    break;

    /* Conversion functions differ in range limit */
#define NUM_BUFFERS_DELAY(_v)   (unsigned int) (voice->sample_rate * fluid_tc2sec_delay(_v) / FLUID_BUFSIZE)
#define NUM_BUFFERS_ATTACK(_v)  (unsigned int) (voice->sample_rate * fluid_tc2sec_attack(_v) / FLUID_BUFSIZE)
#define NUM_BUFFERS_RELEASE(_v) (unsigned int) (voice->sample_rate * fluid_tc2sec_release(_v) / FLUID_BUFSIZE)

    /* volume envelope
     *
     * - delay and hold times are converted to absolute number of samples
     * - sustain is converted to its absolute value
     * - attack, decay and release are converted to their increment per sample
     */
  case GEN_VOLENVDELAY:                /* SF2.01 section 8.1.3 # 33 */
    x = _GEN(voice, GEN_VOLENVDELAY);
    fluid_clip(x, -12000.0f, 5000.0f);
    voice->volenv_data[FLUID_VOICE_ENVDELAY].count = NUM_BUFFERS_DELAY(x);
    voice->volenv_data[FLUID_VOICE_ENVDELAY].coeff = 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVDELAY].incr = 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVDELAY].min = -1.0f;
    voice->volenv_data[FLUID_VOICE_ENVDELAY].max = 1.0f;
    break;
    
  case GEN_VOLENVATTACK:               /* SF2.01 section 8.1.3 # 34 */
    x = _GEN(voice, GEN_VOLENVATTACK);
    fluid_clip(x, -12000.0f, 8000.0f);
    count = 1 + NUM_BUFFERS_ATTACK(x);
    voice->volenv_data[FLUID_VOICE_ENVATTACK].count = count;
    voice->volenv_data[FLUID_VOICE_ENVATTACK].coeff = 1.0f;
    voice->volenv_data[FLUID_VOICE_ENVATTACK].incr = count ? 1.0f / count : 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVATTACK].min = -1.0f;
    voice->volenv_data[FLUID_VOICE_ENVATTACK].max = 1.0f;
    break;

  case GEN_VOLENVHOLD:                 /* SF2.01 section 8.1.3 # 35 */
  case GEN_KEYTOVOLENVHOLD:            /* SF2.01 section 8.1.3 # 39 */
    count = calculate_hold_decay_buffers(voice, GEN_VOLENVHOLD, GEN_KEYTOVOLENVHOLD, 0); /* 0 means: hold */
    voice->volenv_data[FLUID_VOICE_ENVHOLD].count = count;
    voice->volenv_data[FLUID_VOICE_ENVHOLD].coeff = 1.0f;
    voice->volenv_data[FLUID_VOICE_ENVHOLD].incr = 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVHOLD].min = -1.0f;
    voice->volenv_data[FLUID_VOICE_ENVHOLD].max = 2.0f;
    break;

  case GEN_VOLENVDECAY:               /* SF2.01 section 8.1.3 # 36 */
  case GEN_VOLENVSUSTAIN:             /* SF2.01 section 8.1.3 # 37 */
  case GEN_KEYTOVOLENVDECAY:          /* SF2.01 section 8.1.3 # 40 */
    count = calculate_hold_decay_buffers(voice, GEN_VOLENVDECAY, GEN_KEYTOVOLENVDECAY, 1); /* 1 for decay */
    voice->volenv_data[FLUID_VOICE_ENVDECAY].count = count;
    voice->volenv_data[FLUID_VOICE_ENVDECAY].coeff = 1.0f;
    voice->volenv_data[FLUID_VOICE_ENVDECAY].incr = count ? -1.0f / count : 0.0f;
    /* fluid_cb2amp checks the range */
    voice->volenv_data[FLUID_VOICE_ENVDECAY].min = fluid_cb2amp(_GEN(voice, GEN_VOLENVSUSTAIN)); 
    voice->volenv_data[FLUID_VOICE_ENVDECAY].max = 2.0f;
    break;

  case GEN_VOLENVRELEASE:             /* SF2.01 section 8.1.3 # 38 */
    x = _GEN(voice, GEN_VOLENVRELEASE);
    fluid_clip(x, -12000.0f, 8000.0f);
    count = 1 + NUM_BUFFERS_RELEASE(x);
    voice->volenv_data[FLUID_VOICE_ENVRELEASE].count = count;
    voice->volenv_data[FLUID_VOICE_ENVRELEASE].coeff = 1.0f;
    voice->volenv_data[FLUID_VOICE_ENVRELEASE].incr = count ? -1.0f / count : 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVRELEASE].min = 0.0f;
    voice->volenv_data[FLUID_VOICE_ENVRELEASE].max = 1.0f;
    break;
    
    /* Modulation envelope */
  case GEN_MODENVDELAY:               /* SF2.01 section 8.1.3 # 25 */
    x = _GEN(voice, GEN_MODENVDELAY);
    fluid_clip(x, -12000.0f, 5000.0f);
    voice->modenv_data[FLUID_VOICE_ENVDELAY].count = NUM_BUFFERS_DELAY(x);
    voice->modenv_data[FLUID_VOICE_ENVDELAY].coeff = 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVDELAY].incr = 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVDELAY].min = -1.0f;
    voice->modenv_data[FLUID_VOICE_ENVDELAY].max = 1.0f;
    break;
    
  case GEN_MODENVATTACK:               /* SF2.01 section 8.1.3 # 26 */
    x = _GEN(voice, GEN_MODENVATTACK);
    fluid_clip(x, -12000.0f, 8000.0f);
    count = 1 + NUM_BUFFERS_ATTACK(x);
    voice->modenv_data[FLUID_VOICE_ENVATTACK].count = count;
    voice->modenv_data[FLUID_VOICE_ENVATTACK].coeff = 1.0f;
    voice->modenv_data[FLUID_VOICE_ENVATTACK].incr = count ? 1.0f / count : 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVATTACK].min = -1.0f;
    voice->modenv_data[FLUID_VOICE_ENVATTACK].max = 1.0f;
    break;
    
  case GEN_MODENVHOLD:               /* SF2.01 section 8.1.3 # 27 */
  case GEN_KEYTOMODENVHOLD:          /* SF2.01 section 8.1.3 # 31 */
    count = calculate_hold_decay_buffers(voice, GEN_MODENVHOLD, GEN_KEYTOMODENVHOLD, 0); /* 1 means: hold */
    voice->modenv_data[FLUID_VOICE_ENVHOLD].count = count;
    voice->modenv_data[FLUID_VOICE_ENVHOLD].coeff = 1.0f;
    voice->modenv_data[FLUID_VOICE_ENVHOLD].incr = 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVHOLD].min = -1.0f;
    voice->modenv_data[FLUID_VOICE_ENVHOLD].max = 2.0f;
    break;

  case GEN_MODENVDECAY:                                   /* SF 2.01 section 8.1.3 # 28 */
  case GEN_MODENVSUSTAIN:                                 /* SF 2.01 section 8.1.3 # 29 */
  case GEN_KEYTOMODENVDECAY:                              /* SF 2.01 section 8.1.3 # 32 */
    count = calculate_hold_decay_buffers(voice, GEN_MODENVDECAY, GEN_KEYTOMODENVDECAY, 1); /* 1 for decay */
    y = 1.0f - 0.001f * _GEN(voice, GEN_MODENVSUSTAIN);
    fluid_clip(y, 0.0f, 1.0f);
    voice->modenv_data[FLUID_VOICE_ENVDECAY].count = count;
    voice->modenv_data[FLUID_VOICE_ENVDECAY].coeff = 1.0f;
    voice->modenv_data[FLUID_VOICE_ENVDECAY].incr = count ? -1.0f / count : 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVDECAY].min = y;
    voice->modenv_data[FLUID_VOICE_ENVDECAY].max = 2.0f;
    break;
    
  case GEN_MODENVRELEASE:                                  /* SF 2.01 section 8.1.3 # 30 */
    x = _GEN(voice, GEN_MODENVRELEASE);
    fluid_clip(x, -12000.0f, 8000.0f);
    count = 1 + NUM_BUFFERS_RELEASE(x);
    voice->modenv_data[FLUID_VOICE_ENVRELEASE].count = count;
    voice->modenv_data[FLUID_VOICE_ENVRELEASE].coeff = 1.0f;
    voice->modenv_data[FLUID_VOICE_ENVRELEASE].incr = count ? -1.0f / count : 0.0;
    voice->modenv_data[FLUID_VOICE_ENVRELEASE].min = 0.0f;
    voice->modenv_data[FLUID_VOICE_ENVRELEASE].max = 2.0f;
    break;

  } /* switch gen */
}

/**
 * fluid_voice_modulate
 *
 * In this implementation, I want to make sure that all controllers
 * are event based: the parameter values of the DSP algorithm should
 * only be updates when a controller event arrived and not at every
 * iteration of the audio cycle (which would probably be feasible if
 * the synth was made in silicon).
 *
 * The update is done in three steps:
 *
 * - first, we look for all the modulators that have the changed
 * controller as a source. This will yield a list of generators that
 * will be changed because of the controller event.
 *
 * - For every changed generator, calculate its new value. This is the
 * sum of its original value plus the values of al the attached
 * modulators.
 *
 * - For every changed generator, convert its value to the correct
 * unit of the corresponding DSP parameter 
 *
 * @fn int fluid_voice_modulate(fluid_voice_t* voice, int cc, int ctrl, int val)
 * @param voice the synthesis voice
 * @param cc flag to distinguish between a continous control and a channel control (pitch bend, ...)
 * @param ctrl the control number
 * */
int fluid_voice_modulate(fluid_voice_t* voice, int cc, int ctrl)
{
  int i, k;
  fluid_mod_t* mod;
  int gen;
  fluid_real_t modval;
  
/*    printf("Chan=%d, CC=%d, Src=%d, Val=%d\n", voice->channel->channum, cc, ctrl, val); */
  
  for (i = 0; i < voice->mod_count; i++) {

    mod = &voice->mod[i];

    /* step 1: find all the modulators that have the changed controller
     * as input source. */
    if (fluid_mod_has_source(mod, cc, ctrl)) {

      gen = fluid_mod_get_dest(mod);
      modval = 0.0;

      /* step 2: for every changed modulator, calculate the modulation
       * value of its associated generator */
      for (k = 0; k < voice->mod_count; k++) {
	if (fluid_mod_has_dest(&voice->mod[k], gen)) {
	  modval += fluid_mod_get_value(&voice->mod[k], voice->channel, voice);
	}
      }

      fluid_gen_set_mod(&voice->gen[gen], modval);

      /* step 3: now that we have the new value of the generator,
       * recalculate the parameter values that are derived from the
       * generator */
      fluid_voice_update_param(voice, gen);
    }
  }
  return FLUID_OK;  
}

/**
 * fluid_voice_modulate_all
 *
 * Update all the modulators. This function is called after a
 * ALL_CTRL_OFF MIDI message has been received (CC 121).
 *
 */
int fluid_voice_modulate_all(fluid_voice_t* voice)
{
  fluid_mod_t* mod;
  int i, k, gen;
  fluid_real_t modval;

  /* Loop through all the modulators. 

     FIXME: we should loop through the set of generators instead of
     the set of modulators. We risk to call 'fluid_voice_update_param'
     several times for the same generator if several modulators have
     that generator as destination. It's not an error, just a wast of
     energy (think polution, global warming, unhappy musicians,
     ...) */

  for (i = 0; i < voice->mod_count; i++) {

    mod = &voice->mod[i];
    gen = fluid_mod_get_dest(mod);
    modval = 0.0;

    /* Accumulate the modulation values of all the modulators with
     * destination generator 'gen' */
    for (k = 0; k < voice->mod_count; k++) {
      if (fluid_mod_has_dest(&voice->mod[k], gen)) {
	modval += fluid_mod_get_value(&voice->mod[k], voice->channel, voice);
      }
    }

    fluid_gen_set_mod(&voice->gen[gen], modval);

    /* Update the parameter values that are depend on the generator
     * 'gen' */
    fluid_voice_update_param(voice, gen);
  }

  return FLUID_OK;  
}

/*
 * fluid_voice_noteoff
 */
int 
fluid_voice_noteoff(fluid_voice_t* voice)
{
  fluid_profile(FLUID_PROF_VOICE_NOTE, voice->ref);

  if (voice->channel && fluid_channel_sustained(voice->channel)) {
    voice->status = FLUID_VOICE_SUSTAINED;
  } else {
    if (voice->volenv_section == FLUID_VOICE_ENVATTACK) {
      /* A voice is turned off during the attack section of the volume
       * envelope.  The attack section ramps up linearly with
       * amplitude. The other sections use logarithmic scaling.  So
       * convert from linear amplitude to cb.
       */
      if (voice->volenv_val > 0){
	fluid_real_t attn_and_lfo = voice->attenuation + voice->modlfo_val * voice->modlfo_to_vol;
        fluid_real_t amp = voice->volenv_val * pow (10.0, attn_and_lfo / -200.0);
        fluid_real_t env_value = - ((-200. * log (amp) / log (10.) - attn_and_lfo) / 960.0 - 1);
        env_value = (env_value < 0.0 ? 0.0 : env_value);
        env_value = (env_value > 1.0 ? 1.0 : env_value);
        voice->volenv_val=env_value;
      }
    }
    voice->volenv_section = FLUID_VOICE_ENVRELEASE;
    voice->volenv_count = 0;
    voice->modenv_section = FLUID_VOICE_ENVRELEASE;
    voice->modenv_count = 0;
  }
  return FLUID_OK;
}

/* 
 * fluid_voice_kill_excl
 *
 * Percussion sounds can be mutually exclusive: for example, a 'closed
 * hihat' sound will terminate an 'open hihat' sound ringing at the
 * same time. This behaviour is modeled using 'exclusive classes',
 * turning on a voice with an exclusive class other than 0 will kill
 * all other voices having that exclusive class within the same preset
 * or channel.  fluid_voice_kill_excl gets called, when 'voice' is to
 * be killed for that reason.
 */
int 
fluid_voice_kill_excl(fluid_voice_t* voice){

  if (!_PLAYING(voice)) {
    return FLUID_OK;
  }
  
  /* Turn off the exclusive class information for this voice,
     so that it doesn't get killed twice 
  */
  fluid_voice_gen_set(voice, GEN_EXCLUSIVECLASS, 0);

  /* If the voice is not yet in release state, put it into release state */
  if (voice->volenv_section != FLUID_VOICE_ENVRELEASE){
    voice->volenv_section = FLUID_VOICE_ENVRELEASE;
    voice->volenv_count = 0;
    voice->modenv_section = FLUID_VOICE_ENVRELEASE;
    voice->modenv_count = 0;
  }
  
  /* Speed up the volume envelope */
  /* The value was found through listening tests with hi-hat samples. */
  fluid_voice_gen_set(voice, GEN_VOLENVRELEASE, -200);
  fluid_voice_update_param(voice, GEN_VOLENVRELEASE);
  
  /* Speed up the modulation envelope */
  fluid_voice_gen_set(voice, GEN_MODENVRELEASE, -200);
  fluid_voice_update_param(voice, GEN_MODENVRELEASE);
  
  return FLUID_OK;   
}

/*
 * fluid_voice_off
 * 
 * Purpose:
 * Turns off a voice, meaning that it is not processed
 * anymore by the DSP loop.
 */
int 
fluid_voice_off(fluid_voice_t* voice)
{
  fluid_profile(FLUID_PROF_VOICE_RELEASE, voice->ref);

  voice->chan = NO_CHANNEL;
  voice->volenv_section = FLUID_VOICE_ENVFINISHED;
  voice->volenv_count = 0;
  voice->modenv_section = FLUID_VOICE_ENVFINISHED;
  voice->modenv_count = 0;
  voice->status = FLUID_VOICE_OFF;

  /* Decrement the reference count of the sample. */
  fluid_sample_decr_ref(voice->sample);

  voice->sample = NULL;

  return FLUID_OK;
}

/*
 * fluid_voice_add_mod
 *
 * Adds a modulator to the voice.  "mode" indicates, what to do, if
 * an identical modulator exists already.
 *
 * mode == FLUID_VOICE_ADD: Identical modulators on preset level are added
 * mode == FLUID_VOICE_OVERWRITE: Identical modulators on instrument level are overwritten
 * mode == FLUID_VOICE_DEFAULT: This is a default modulator, there can be no identical modulator.
 *                             Don't check.
 */
void 
fluid_voice_add_mod(fluid_voice_t* voice, fluid_mod_t* mod, int mode)
{
  int i;
  
  /*
   * Some soundfonts come with a huge number of non-standard
   * controllers, because they have been designed for one particular
   * sound card.  Discard them, maybe print a warning.  
   */

  if (((mod->flags1 & FLUID_MOD_CC) == 0) 
      && ((mod->src1 != 0)          /* SF2.01 section 8.2.1: Constant value */ 
	  && (mod->src1 != 2)       /* Note-on velocity */ 
	  && (mod->src1 != 3)       /* Note-on key number */
	  && (mod->src1 != 10)      /* Poly pressure */
	  && (mod->src1 != 13)      /* Channel pressure */
	  && (mod->src1 != 14)      /* Pitch wheel */
	  && (mod->src1 != 16))) {  /* Pitch wheel sensitivity */
    FLUID_LOG(FLUID_WARN, "Ignoring invalid controller, using non-CC source %i.", mod->src1);
    return;
  }

  if (mode == FLUID_VOICE_ADD) {

    /* if identical modulator exists, add them */
    for (i = 0; i < voice->mod_count; i++) {
      if (fluid_mod_test_identity(&voice->mod[i], mod)) {
	//		printf("Adding modulator...\n");
	voice->mod[i].amount += mod->amount;
	return;
      }
    }

  } else if (mode == FLUID_VOICE_OVERWRITE) {
    
    /* if identical modulator exists, replace it (only the amount has to be changed) */
    for (i = 0; i < voice->mod_count; i++) {
      if (fluid_mod_test_identity(&voice->mod[i], mod)) {
	//		printf("Replacing modulator...amount is %f\n",mod->amount);
	voice->mod[i].amount = mod->amount;
	return;
      }
    }   
  }

  /* Add a new modulator (No existing modulator to add / overwrite).
     Also, default modulators (FLUID_VOICE_DEFAULT) are added without
     checking, if the same modulator already exists. */
  if (voice->mod_count < FLUID_NUM_MOD) {
    fluid_mod_clone(&voice->mod[voice->mod_count++], mod);
  }
}

unsigned int fluid_voice_get_id(fluid_voice_t* voice)
{
  return voice->id;
}

int fluid_voice_is_playing(fluid_voice_t* voice)
{
  return _PLAYING(voice);
}

/*
 * fluid_voice_get_lower_boundary_for_attenuation
 *
 * Purpose:
 *
 * A lower boundary for the attenuation (as in 'the minimum
 * attenuation of this voice, with volume pedals, modulators
 * etc. resulting in minimum attenuation, cannot fall below x cB) is
 * calculated.  This has to be called during fluid_voice_init, after
 * all modulators have been run on the voice once.  Also,
 * voice->attenuation has to be initialized.
 */
fluid_real_t fluid_voice_get_lower_boundary_for_attenuation(fluid_voice_t* voice)
{
  int i;
  fluid_mod_t* mod;
  fluid_real_t possible_att_reduction_cB=0;
  fluid_real_t lower_bound;

  for (i = 0; i < voice->mod_count; i++) {
    mod = &voice->mod[i];

    /* Modulator has attenuation as target and can change over time? */
    if ((mod->dest == GEN_ATTENUATION)  
	&& ((mod->flags1 & FLUID_MOD_CC) || (mod->flags2 & FLUID_MOD_CC))) {
      
      fluid_real_t current_val=fluid_mod_get_value(mod, voice->channel, voice);
      fluid_real_t v=fabs(mod->amount);

      if ((mod->src1 == FLUID_MOD_PITCHWHEEL) 
	  || (mod->flags1 & FLUID_MOD_BIPOLAR) 
	  || (mod->flags2 & FLUID_MOD_BIPOLAR) 
	  || (mod->amount < 0)) {
	/* Can this modulator produce a negative contribution? */
	v *= -1.0;
      } else {
	/* No negative value possible. But still, the minimum contribution is 0. */
	v = 0;
      }

      /* For example: 
       * - current_val=100
       * - min_val=-4000
       * - possible_att_reduction_cB += 4100
       */
      if (current_val > v){
	possible_att_reduction_cB += (current_val - v); 
      }
    }
  }
  
  lower_bound = voice->attenuation-possible_att_reduction_cB;
  
  /* SF2.01 specs do not allow negative attenuation */
  if (lower_bound < 0) {
    lower_bound = 0;
  }
  return lower_bound;
}


/* Purpose:
 *
 * Make sure, that sample start / end point and loop points are in
 * proper order. When starting up, calculate the initial phase.
 */
void fluid_voice_check_sample_sanity(fluid_voice_t* voice)
{
    int min_index_nonloop=(int) voice->sample->start;
    int max_index_nonloop=(int) voice->sample->end;

    /* make sure we have enough samples surrounding the loop */
    int min_index_loop=(int) voice->sample->start + FLUID_MIN_LOOP_PAD;
    int max_index_loop=(int) voice->sample->end - FLUID_MIN_LOOP_PAD;
    fluid_check_fpe("voice_check_sample_sanity start");

    if (!voice->check_sample_sanity_flag){
	return;
    }

#if 0
    printf("Sample from %i to %i\n",voice->sample->start, voice->sample->end);
    printf("Sample loop from %i %i\n",voice->sample->loopstart, voice->sample->loopend);
    printf("Playback from %i to %i\n", voice->sample_start, voice->sample_end);
    printf("Playback loop from %i to %i\n",voice->loop_start, voice->loop_end);
#endif

    /* Keep the start point within the sample data */
    if (voice->sample_start < min_index_nonloop){
	voice->sample_start = min_index_nonloop;
    } else if (voice->sample_start > max_index_nonloop){
	voice->sample_start = max_index_nonloop;
    }

    /* Keep the end point within the sample data */
    if (voice->sample_end < min_index_nonloop){
      voice->sample_end = min_index_nonloop;
    } else if (voice->sample_end > max_index_nonloop){
      voice->sample_end = max_index_nonloop;
    }
    
    /* Keep start and end point in the right order */
    if (voice->sample_start > voice->sample_end){
	int temp = voice->sample_start;
	voice->sample_start = voice->sample_end;
	voice->sample_end = temp;
	/*FLUID_LOG(FLUID_DBG, "Loop / sample sanity check: Changing order of start / end points!"); */
    }
    
    /* Zero length? */
    if (voice->sample_start == voice->sample_end){
	fluid_voice_off(voice);
	return;
    }

    if ((_SAMPLEMODE(voice) == FLUID_LOOP) 
	|| (_SAMPLEMODE(voice) == FLUID_LOOP_DURING_RELEASE)) {
	/* Keep the loop start point within the sample data */
	if (voice->loop_start < min_index_loop){
	    voice->loop_start = min_index_loop;
      } else if (voice->loop_start > max_index_loop){
	voice->loop_start = max_index_loop;
      }
      
      /* Keep the loop end point within the sample data */
      if (voice->loop_end < min_index_loop){
	voice->loop_end = min_index_loop;
      } else if (voice->loop_end > max_index_loop){
	voice->loop_end = max_index_loop;
      }
      
      /* Keep loop start and end point in the right order */
      if (voice->loop_start > voice->loop_end){
	int temp=voice->loop_start;
	voice->loop_start=voice->loop_end;
	voice->loop_end=temp;
	/*FLUID_LOG(FLUID_DBG, "Loop / sample sanity check: Changing order of loop points!"); */
      }
      
      /* Loop too short? Then don't loop. */
      if (voice->loop_end < voice->loop_start + FLUID_MIN_LOOP_SIZE){
	  voice->gen[GEN_SAMPLEMODE].val=FLUID_UNLOOPED;      
      }      

      /* The loop points may have changed. Obtain a new estimate for the loop volume. */
      /* Is the voice loop within the sample loop? */
      if ((int)voice->loop_start >= (int)voice->sample->loopstart 
	  && (int)voice->loop_end <= (int)voice->sample->loopend){
	/* Is there a valid peak amplitude available for the loop? */
	if (voice->sample->amplitude_that_reaches_noise_floor_is_valid){
	  voice->amplitude_that_reaches_noise_floor_loop=voice->sample->amplitude_that_reaches_noise_floor / voice->synth_gain;
	} else {
	  /* Worst case */
	  voice->amplitude_that_reaches_noise_floor_loop=voice->amplitude_that_reaches_noise_floor_nonloop;	    
	};
      };

    } /* if sample mode is looped */
    
    /* Run startup specific code (only once, when the voice is started) */
    if (voice->check_sample_sanity_flag & FLUID_SAMPLESANITY_STARTUP){
      if (max_index_loop-min_index_loop < FLUID_MIN_LOOP_SIZE){
	if (_SAMPLEMODE(voice) == FLUID_LOOP 
	    || (_SAMPLEMODE(voice) == FLUID_LOOP_DURING_RELEASE)){
	  voice->gen[GEN_SAMPLEMODE].val=FLUID_UNLOOPED;
	}
      }
      
      /* Set the initial phase of the voice (using the result from the
	 start offset modulators). */
      fluid_phase_set_int(voice->phase, voice->sample_start);
    } /* if startup */
    
    /* Is this voice run in loop mode, or does it run straight to the
       end of the waveform data? */
    if (((_SAMPLEMODE(voice) == FLUID_LOOP) 
	 && (voice->volenv_section < FLUID_VOICE_ENVRELEASE)) 
	|| (_SAMPLEMODE(voice) == FLUID_LOOP_DURING_RELEASE)) {
      /* Yes, it will loop as soon as it reaches the loop point.  In
       * this case we must prevent, that the playback pointer (phase)
       * happens to end up beyond the 2nd loop point, because the
       * point has moved.  The DSP algorithm is unable to cope with
       * that situation.  So if the phase is beyond the 2nd loop
       * point, set it to the start of the loop. No way to avoid some
       * noise here.  Note: If the sample pointer ends up -before the
       * first loop point- instead, then the DSP loop will just play
       * the sample, enter the loop and proceed as expected => no
       * actions required.
       */
      int index_in_sample = fluid_phase_index(voice->phase);
      if (index_in_sample >= voice->loop_end){
	/* FLUID_LOG(FLUID_DBG, "Loop / sample sanity check: Phase after 2nd loop point!"); */
	fluid_phase_set_int(voice->phase,voice->loop_start);
      }
    }
/*    FLUID_LOG(FLUID_DBG, "Loop / sample sanity check: Sample from %i to %i, loop from %i to %i", voice->sample_start, voice->sample_end, voice->loop_start, voice->loop_end); */
   
    /* Sample sanity has been assured. Don't check again, until some
       sample parameter is changed by modulation. */
    voice->check_sample_sanity_flag=0;
#if 0
    printf("Sane? playback loop from %i to %i\n",voice->loop_start, voice->loop_end);
#endif
    fluid_check_fpe("voice_check_sample_sanity");
}


int fluid_voice_set_param(fluid_voice_t* voice, int gen, fluid_real_t nrpn_value)
{
  voice->gen[gen].nrpn = nrpn_value;
  fluid_voice_update_param(voice, gen);
  return FLUID_OK;
}

int fluid_voice_set_gain(fluid_voice_t* voice, fluid_real_t gain)
{
  /* avoid division by zero*/
  if (gain < 0.0000001){
    gain = 0.0000001;
  }

  voice->synth_gain = gain;
  voice->amp_left = fluid_pan(voice->pan, 1) * gain / 32768.0f;
  voice->amp_right = fluid_pan(voice->pan, 0) * gain / 32768.0f;
  voice->amp_reverb = voice->reverb_send * gain / 32768.0f;
  voice->amp_chorus = voice->chorus_send * gain / 32768.0f;  

  return FLUID_OK;
}

/* - Scan the loop
 * - determine the peak level 
 * - Calculate, what factor will make the loop inaudible
 * - Store in sample
 */
int fluid_voice_optimize_sample(fluid_sample_t* s)
{
  signed short peak_max = 0;
  signed short peak_min = 0;
  signed short peak;
  double result;
  int i;
  
  if (!s->amplitude_that_reaches_noise_floor_is_valid){ /* Only once */
    /* Scan the loop */
    for (i = (int)s->loopstart; i < (int) s->loopend; i ++){
      signed short val = s->data[i];
      if (val > peak_max) {
	peak_max = val;
      } else if (val < peak_min) {
	peak_min = val;
      }
    }
    
    /* Determine the peak level */
    if (peak_max >- peak_min){
      peak = peak_max;
    } else {
      peak =- peak_min;
    };
    if (peak == 0){
      /* Avoid division by zero */
      peak = 1;
    };

    /* Calculate what factor will make the loop inaudible
     * For example: Take a peak of 3277 (10 % of 32768).  The
     * normalized amplitude is 0.1 (10 % of 32768).  An amplitude
     * factor of 0.0001 (as opposed to the default 0.00001) will
     * drop this sample to the noise floor.
     */
    
    /* 16 bits => 96+4=100 dB dynamic range => 0.00001 */
    fluid_real_t normalized_amplitude_during_loop=((fluid_real_t)peak)/32768.;
    result = FLUID_NOISE_FLOOR / normalized_amplitude_during_loop;
    
    /* Store in sample */
    s->amplitude_that_reaches_noise_floor = (double)result;
    s->amplitude_that_reaches_noise_floor_is_valid = 1;
#if 0
    printf("Sample peak detection: factor %f\n", (double)result);
#endif
  };
  return FLUID_OK;
}

