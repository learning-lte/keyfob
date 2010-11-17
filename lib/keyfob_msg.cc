/* -*- c++ -*- */
/*
 * Copyright 2004,2010 Free Software Foundation, Inc.
 * 
 * This file is part of GNU Radio
 * 
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <keyfob_msg.h>
#include <gr_io_signature.h>
#include <stdio.h>

/*
 * Create a new instance of keyfob_msg and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
keyfob_msg_sptr 
keyfob_make_msg (gr_msg_queue_sptr queue, double rate, double threshold)
{
  return gnuradio::get_initial_sptr(new keyfob_msg (queue, rate, threshold));
}

/*
 * The private constructor
 */
keyfob_msg::keyfob_msg (gr_msg_queue_sptr queue, double rate, double threshold)
  : gr_sync_block ("keyfob_msg",
	      gr_make_io_signature (1, 1, sizeof (float)),
	      gr_make_io_signature (0, 0, 0)),
    d_queue(queue),
    d_rate(rate),
    d_threshold(threshold)
{
    d_bitrate_min = 2200;
    d_bitrate_max = 2600;
    d_bitrate = 2400;
    d_bitrate_step = 20; //FIXME: this will change
    d_samples_per_bit = d_rate / d_bitrate;
    
    set_history(d_samples_per_bit * 150); //128-bit packets
}

/*
 * Our virtual destructor.
 */
keyfob_msg::~keyfob_msg ()
{
    
}

static float bit_energy(const float *data, int samples_per_chip) { 
    //return total bit energy of a chip centered at the current point (we bias right for even samples per chip)
	float energy = 0;
	if(samples_per_chip <= 2) {
		energy = data[0];
	} else {
		for(int j = 1-samples_per_chip/2; j < samples_per_chip/2; j++) {
			energy += data[j];
		}
	}
	return energy;
}

static int early_late(const float *data, int samples_per_chip) {
	float gate_sum_early=0, gate_sum_now=0, gate_sum_late=0;

	gate_sum_early = bit_energy(&data[-1], samples_per_chip);
	gate_sum_now = bit_energy(&data[0], samples_per_chip);
	gate_sum_late = bit_energy(&data[1], samples_per_chip);

	if(gate_sum_early > gate_sum_now) return -1;
	else if(gate_sum_late > gate_sum_now) return 1;
	else return 0;
}

float keyfob_msg::get_energy_diff(const float *data, float samples_per_bit) {
    float zerosum=0, onesum=0;
    for(int i = 0; i < 36; i++) {
        zerosum += bit_energy(data + (0 + int(samples_per_bit * (13 + 3*i))), samples_per_bit);
        onesum  += bit_energy(data + (0 + int(samples_per_bit * (15 + 3*i))), samples_per_bit);
    }
    
    return onesum-zerosum;
}

int keyfob_msg::get_clock_rate_dir(const float *data, float bitrate) {
    float currdiff = get_energy_diff(data, d_rate / bitrate);
    float nextdiff = get_energy_diff(data, d_rate / (bitrate + d_bitrate_step));
    float prevdiff = get_energy_diff(data, d_rate / (bitrate - d_bitrate_step));
    //printf("Currdiff: %f nextdiff: %f prevdiff: %f\n", currdiff, nextdiff, prevdiff);
    if(prevdiff > currdiff) return -1;
    else if (nextdiff > currdiff) return 1;
    else return 0;
}

int 
keyfob_msg::work (int noutput_items,
			       gr_vector_const_void_star &input_items,
			       gr_vector_void_star &output_items)
{
  const float *in = (const float *) input_items[0];
  //ok, really? the bit rate is insanely variable. and clock recovery is hard and expensive. we can do data-aided clock
  //recovery without too much effort. we can spec a minimum and maximum data rate and know that every 6-chip symbol is
  //"011011" or "001011" or "001001"
  //every 3-bit chip is "011" or "001"
  //every third bit is 0,x,1 <- there's your clock recovery
  //so go through the whole packet and sample the first and third bits
  //and maximize the difference over the allowable clock range
  //then pick out your bits from the packet
  //i guess you can start at the center
  //ok so we set a threshold above which the preamble has to sit; this can be coded into a constructor parameter or just hardcoded
  //the preamble detector can use the center rate to look for preambles, it's short enough it shouldn't drift out of window for 6 bits
  
  int i,j,k;
  int switches[8], address[10];
  float ref, refmin, refmax, d, d_next, zerosum, onesum;
  bool preamble_data[13] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1};
  bool preamble_found;
  
  for(i=0; i < noutput_items; i++) {
    if(in[i] > d_threshold) { //look, a spike
        //look for pulses in the appropriate places
        if(in[i+1] > in[i]) continue; //we're not on top of the pulse yet
        //ok we're on top of a pulse, let's see if it's part of a preamble
        ref = in[i];
        refmin = 0.7 * ref;
        refmax = 1.3 * ref;
        
        preamble_found = true;
        for(j = 1; j < 13; j++) {
            d = in[i+int(d_samples_per_bit*j)];
            if(preamble_data[j]) {
                if(d < refmin || d > refmax) preamble_found = false;
            }
            else {
                if(d > refmin) preamble_found = false;
            }
        }
        if(preamble_found == false) continue; //forever alone
        
        //okay, we're pretty sure we have a preamble
        //now we find bit centers and clock rate
        
        //first, the bit center; we'll use the 0th bit as our guinea pig
        //for simplicity we'll assume that we're currently before the bit center (see line 126)
        while(early_late(in+(i++), d_samples_per_bit) > 0);
        //now we're at the bit center.
        
        //to find out what clock rate we're at, we start at the minimum possible clock rate and
        //calculate the sum of the bit energy at all the expected "1" bits, and at the expected "0" bits.
        //the difference forms a metric which indicates how close we are to the correct clock rate.
        //to save CPU we're just going to use the center bits instead of the bit energy.
        //this is really cheesy and throws away data but saves lots of CPU
        //we can calculate the reference level in parallel with this
        //FIXME: use a search method that looks for a peak instead of searching the whole space
        
        //here we search starting at the peak and moving away
        int clock_rate_dir = 1;
        float temp_bitrate=d_bitrate, temp_spb=d_samples_per_bit;
        while(clock_rate_dir != 0) {
            if(temp_bitrate > d_bitrate_max) continue;
            if(temp_bitrate < d_bitrate_min) continue;
            clock_rate_dir = get_clock_rate_dir(in+i, temp_bitrate);
            if(clock_rate_dir == 0) break;
            //printf("clock_rate_dir: %i\n", clock_rate_dir);
            temp_bitrate += clock_rate_dir * d_bitrate_step;
            temp_spb = d_rate / temp_bitrate;
            printf("trying new rate: %f\n", temp_bitrate);
        }
        
        
        //printf("Clock rate: %f\n", temp_bitrate);
        //printf("Starting clock rate: %f\n", d_bitrate);
        //printf("Reference: %f\n", ref);
        
        ref = in[i] / 2.0; //FIXME TEMP

        //now let's validate that all the "one" bits are one and all the "zero" bits are zero
        //sometimes this thing sends incomplete packets or just complete junk
        bool its_on = true;
        for(j=0;j<36;j++) {
            if(in[i+int(temp_spb * (13 + 3*j))] > ref) {
                its_on = false;
                break;
            }
            if(in[i+int(temp_spb * (15 + 3*j))] < ref) {
                its_on = false;
                break;
            }
        }
        if(!its_on) {
            i += 128 * d_samples_per_bit;
            continue;
        }
        
        d_bitrate = temp_bitrate;
        d_samples_per_bit = temp_spb;
        
        //now we've got a clock rate in d_samples_per_bit and a reference level in ref
        
        
        //now we can slice and output raw bits! we'll say there are 20 addr bits and 16 data bits, because they're (sort of) duplicated
        int addr_bits = 0;
        for(j=0; j<10; j++) {
            address[j] = ((in[i + int(d_samples_per_bit * (14 + 6*j + 0))] > ref) << 1) | 
                         ((in[i + int(d_samples_per_bit * (14 + 6*j + 3))] > ref) << 0);
            addr_bits += (address[j] == 3) ? 0 : (1 << j);
        }
        
        int switch_bits = 0;
        for(j=0; j<8; j++) {
            switches[j] = ((in[i + int(d_samples_per_bit * (74 + 6*j + 0))] > ref) << 1) |
                          ((in[i + int(d_samples_per_bit * (74 + 6*j + 3))] > ref) << 0);
            switch_bits += (switches[j] == 1) ? 0 : (1 << j);
        }
        //now we can post a message
        //printf("Addr: 0x%x Switches: 0x%x\n", addr_bits, switch_bits);
        std::ostringstream payload;
        payload << ref << " " << addr_bits << " " << switch_bits;
        gr_message_sptr msg = gr_make_message_from_string(std::string(payload.str()));
        d_queue->handle(msg);
        
        //and now we're done with these samples
        i += int(128 * d_samples_per_bit);
    }

  }



  // Tell runtime system how many items we consumed.
  return i;
}
