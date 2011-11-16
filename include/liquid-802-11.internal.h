/*
 * Copyright (c) 2011 Joseph Gaeddert
 * Copyright (c) 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __LIQUID_802_11_INTERNAL_H__
#define __LIQUID_802_11_INTERNAL_H__

// Configuration file
#include "config.h"

#include <complex.h>
#include <liquid/liquid.h>
#include <fec.h>

#include "liquid-802-11.h"

// Use fftw library if installed, otherwise use liquid-dsp (less
// efficient) fft library.
#if HAVE_FFTW3_H
#   include <fftw3.h>
#   define FFT_PLAN             fftwf_plan
#   define FFT_CREATE_PLAN      fftwf_plan_dft_1d
#   define FFT_DESTROY_PLAN     fftwf_destroy_plan
#   define FFT_EXECUTE          fftwf_execute
#   define FFT_DIR_FORWARD      FFTW_FORWARD
#   define FFT_DIR_BACKWARD     FFTW_BACKWARD
#   define FFT_METHOD           FFTW_ESTIMATE
#else
#   define FFT_PLAN             fftplan
#   define FFT_CREATE_PLAN      fft_create_plan
#   define FFT_DESTROY_PLAN     fft_destroy_plan
#   define FFT_EXECUTE          fft_execute
#   define FFT_DIR_FORWARD      FFT_FORWARD
#   define FFT_DIR_BACKWARD     FFT_REVERSE
#   define FFT_METHOD           0
#endif

//
// wi-fi frame (common objects)
//

// PLCP short sequence
extern const float complex wififrame_S0[64]; // freq
extern const float complex wififrame_s0[64]; // time

// PLCP long sequence
extern const float complex wififrame_S1[64]; // freq
extern const float complex wififrame_s1[64]; // time

//
// signal definition
//

struct wifi_signal_s {
    // Data rate (RATE)
    enum {
        WIFI_SIGNAL_RATE_6  = 13,   // BPSK,   r1/2, 1101
        WIFI_SIGNAL_RATE_9  = 15,   // BPSK,   r3/4, 1111
        WIFI_SIGNAL_RATE_12 =  5,   // QPSK,   r1/2, 0101
        WIFI_SIGNAL_RATE_18 =  7,   // QPSK,   r3/4, 0111
        WIFI_SIGNAL_RATE_24 =  9,   // 16-QAM, r1/2, 1001
        WIFI_SIGNAL_RATE_36 = 11,   // 16-QAM, r3/4, 1011
        WIFI_SIGNAL_RATE_48 =  1,   // 64-QAM, r2/3, 0001
        WIFI_SIGNAL_RATE_54 =  3,   // 64-QAM, r3/4, 0011
    } rate;

    // reserved bit
    unsigned char R;

    // 12-bit data length
    unsigned int length;
};

// print SIGNAL structure
void wifi_signal_print(struct wifi_signal_s * _q);

// pack SIGNAL structure into 3-byte array
void wifi_signal_pack(struct wifi_signal_s * _q,
                      unsigned char * _signal);

// unpack SIGNAL structure from 3-byte array
void wifi_signal_unpack(unsigned char * _signal,
                        struct wifi_signal_s * _q);


// 
// encoding/decoding
//

#define LIQUID_802_11_SOFTBIT_1 (255)
#define LIQUID_802_11_SOFTBIT_0 (0)

// encode SIGNAL field using half-rate convolutional code
//  _msg_dec    :   24-bit signal field [size: 3 x 1]
//  _msg_enc    :   48-bit signal field [size: 6 x 1]
void wifi_fec_signal_encode(unsigned char * _msg_dec,
                            unsigned char * _msg_enc);

// decode SIGNAL field using half-rate convolutional code
//  _msg_enc    :   48-bit signal field [size: 6 x 1]
//  _msg_dec    :   24-bit signal field [size: 3 x 1]
void wifi_fec_signal_decode(unsigned char * _msg_enc,
                            unsigned char * _msg_dec);

// puncturing matrices
extern const char wifi_fec_conv27p23_matrix[12];
extern const char wifi_fec_conv27p34_matrix[18];

// encode data using convolutional code
//  _fec_scheme :   error-correction scheme
//  _dec_msg_len:   length of decoded message
//  _msg_dec    :   decoded message (with tail bits inserted)
//  _msg_enc    :   encoded message
void wifi_fec_encode(unsigned int    _fec_scheme,
                     unsigned int    _dec_msg_len,
                     unsigned char * _msg_dec,
                     unsigned char * _msg_enc);


//
// data scrambler/de-scrambler
//

// scramble data
//  _msg_dec    :   original data message [size: _n x 1]
//  _msg_enc    :   scrambled data message [size: _n x 1]
//  _n          :   length of input/output (bytes)
//  _seed       :   linear feedback shift register initial state
void wifi_data_scramble(unsigned char * _msg_dec,
                        unsigned char * _msg_enc,
                        unsigned int _n,
                        unsigned int _seed);

// unscramble data
//  _msg_enc    :   scrambled data message [size: _n x 1]
//  _msg_dec    :   original data message [size: _n x 1]
//  _n          :   length of input/output (bytes)
//  _seed       :   linear feedback shift register initial state
void wifi_data_unscramble(unsigned char * _msg_enc,
                          unsigned char * _msg_dec,
                          unsigned int _n,
                          unsigned int _seed);

// intereleave one OFDM symbol
//  _ncbps      :   number of coded bits per OFDM symbol
//  _nbpsc      :   number of bits per subcarrier (modulation depth)
//  _msg_dec    :   decoded message (de-iterleaved)
//  _msg_enc    :   encoded message (interleaved)
void wifi_interleaver_encode_symbol(unsigned int _ncbps,
                                    unsigned int _nbpsc,
                                    unsigned char * _msg_dec,
                                    unsigned char * _msg_enc);

// intereleave message
//  _ncbps      :   number of coded bits per OFDM symbol
//  _nbpsc      :   number of bits per subcarrier (modulation depth)
//  _n          :   input messge length (bytes)
//  _msg_dec    :   decoded message (de-iterleaved)
//  _msg_enc    :   encoded message (interleaved)
void wifi_interleaver_encode(unsigned int _ncbps,
                             unsigned int _nbpsc,
                             unsigned int _n,
                             unsigned char * _msg_dec,
                             unsigned char * _msg_enc);

// de-intereleave one OFDM symbol
//  _ncbps      :   number of coded bits per OFDM symbol
//  _nbpsc      :   number of bits per subcarrier (modulation depth)
//  _msg_enc    :   encoded message (interleaved)
//  _msg_dec    :   decoded message (de-iterleaved)
void wifi_interleaver_decode_symbol(unsigned int _ncbps,
                                    unsigned int _nbpsc,
                                    unsigned char * _msg_dec,
                                    unsigned char * _msg_enc);

// de-intereleave message
//  _ncbps      :   number of coded bits per OFDM symbol
//  _nbpsc      :   number of bits per subcarrier (modulation depth)
//  _n          :   input messge length (bytes)
//  _msg_enc    :   encoded message (interleaved)
//  _msg_dec    :   decoded message (de-iterleaved)
void wifi_interleaver_decode(unsigned int _ncbps,
                             unsigned int _nbpsc,
                             unsigned int _n,
                             unsigned char * _msg_enc,
                             unsigned char * _msg_dec);


//
// wi-fi frame generator
//

//
// wi-fi frame synchronizer
//

#endif // __LIQUID_802_11_INTERNAL_H__
