/*
 * Copyright (c) 2011, 2012 Joseph Gaeddert
 * Copyright (c) 2011, 2012 Virginia Polytechnic Institute & State University
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

//
// wlanframesync.c
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "liquid-wlan.internal.h"

#define DEBUG_WLANFRAMESYNC             0
#define DEBUG_WLANFRAMESYNC_PRINT       0
#define DEBUG_WLANFRAMESYNC_FILENAME    "wlanframesync_internal_debug.m"
#define DEBUG_WLANFRAMESYNC_BUFFER_LEN  (2048)

// Thresholds for detecting short sequences
#define WLANFRAMESYNC_S0A_ABS_THRESH    (0.4f)
//#define WLANFRAMESYNC_S0B_ABS_THRESH    (0.5f)

// Thresholds for detecting first long sequence, S1[a]
#define WLANFRAMESYNC_S1A_ABS_THRESH    (0.5f)
#define WLANFRAMESYNC_S1A_ARG_THRESH    (0.2f)

// Thresholds for detecting second long sequence, S1[b]
#define WLANFRAMESYNC_S1B_ABS_THRESH    (0.5f)
#define WLANFRAMESYNC_S1B_ARG_THRESH    (0.2f)

struct wlanframesync_s {
    // callback
    wlanframesync_callback callback;
    void * userdata;

    // options
    unsigned int rate;      // primitive data rate
    unsigned int length;    // original data length (bytes)
    unsigned int seed;      // data scrambler seed

    // transform object
    FFT_PLAN fft;           // ifft object
    float complex * X;      // frequency-domain buffer
    float complex * x;      // time-domain buffer
    windowcf input_buffer;  // input sequence buffer

    // synchronizer objects
    nco_crcf nco_rx;        // numerically-controlled oscillator
    wlan_lfsr ms_pilot;     // pilot sequence generator
    unsigned int mod_scheme;// DATA field (de)modulation scheme
    float phi_prime;        // stored pilot phase

    // gain arrays
    float g0;                       // nominal gain
    float complex G0a[64], G0b[64]; // complex channel gain (short sequences)
    float complex s0a_hat;          // first 'short' sequence statistic
    float complex s0b_hat;          // second 'short' sequence statistic
    float complex G1a[64], G1b[64]; // complex channel gain (long sequences)
    float complex s1a_hat;          // first 'long' sequence statistic
    float complex s1b_hat;          // second 'long' sequence statistic
    float complex G[64];            // complex channel gain (composite)
    float complex R[64];            // complex channel correction (composite)

    // lengths
    unsigned int ndbps;             // number of data bits per OFDM symbol
    unsigned int ncbps;             // number of coded bits per OFDM symbol
    unsigned int nbpsc;             // number of bits per subcarrier (modulation depth)
    unsigned int dec_msg_len;       // length of decoded message (bytes)
    unsigned int enc_msg_len;       // length of encoded message (bytes)
    unsigned int nsym;              // number of OFDM symbols in the DATA field
    unsigned int ndata;             // number of bits in the DATA field
    unsigned int npad;              // number of pad bits
    unsigned int bytes_per_symbol;  // number of encoded data bytes per OFDM symbol

    // data arrays
    unsigned char   signal_int[6];  // interleaved message (SIGNAL field)
    unsigned char   signal_enc[6];  // encoded message (SIGNAL field)
    unsigned char   signal_dec[3];  // decoded message (SIGNAL field)
    unsigned char * msg_enc;        // encoded message (DATA field)
    unsigned char * msg_dec;        // decoded message (DATA field)
    unsigned char   modem_syms[48]; // modem symbols
    int signal_valid;               // SIGNAL field decoded properly?
    
    // counters/states
    enum {
        WLANFRAMESYNC_STATE_SEEKPLCP=0, // seek initial PLCP
        WLANFRAMESYNC_STATE_RXSHORT0,   // receive first 'short' sequence
        WLANFRAMESYNC_STATE_RXSHORT1,   // receive second 'short' sequence
        WLANFRAMESYNC_STATE_RXLONG0,    // receive first 'long' sequence
        WLANFRAMESYNC_STATE_RXLONG1,    // receive second 'long' sequence
        WLANFRAMESYNC_STATE_RXSIGNAL,   // receive SIGNAL field
        WLANFRAMESYNC_STATE_RXDATA,     // receive DATA field
    } state;
    signed int timer;                   // sample timer
    unsigned int num_symbols;           // number of received OFDM data symbols

#if DEBUG_WLANFRAMESYNC
    // debugging structures
    int debug_enabled;
    agc_crcf agc_rx;        // automatic gain control (rssi)
    windowcf debug_x;
    windowf  debug_rssi;
    windowcf debug_framesyms;
#endif
};

// create WLAN framing synchronizer object
//  _callback   :   user-defined callback function
//  _userdata   :   user-defined data structure
wlanframesync wlanframesync_create(wlanframesync_callback _callback,
                                   void *                 _userdata)
{
    // allocate main object memory
    wlanframesync q = (wlanframesync) malloc(sizeof(struct wlanframesync_s));
    
    // set callback data
    q->callback = _callback;
    q->userdata = _userdata;

    // create transform object
    q->X = (float complex*) malloc(64*sizeof(float complex));
    q->x = (float complex*) malloc(64*sizeof(float complex));
    q->fft = FFT_CREATE_PLAN(64, q->x, q->X, FFT_DIR_FORWARD, FFT_METHOD);
 
    // create input buffer the length of the transform
    q->input_buffer = windowcf_create(80);

    // synchronizer objects
    q->nco_rx = nco_crcf_create(LIQUID_VCO);
    q->ms_pilot = wlan_lfsr_create(7, 0x91, 0x7f);
    q->mod_scheme = WLAN_MODEM_BPSK;

    // set initial properties
    q->rate   = WLANFRAME_RATE_6;
    q->length = 100;
    q->seed   = 0x5d;

    // allocate memory for encoded message
    q->enc_msg_len = wlan_packet_compute_enc_msg_len(q->rate, q->length);
    q->msg_enc = (unsigned char*) malloc(q->enc_msg_len*sizeof(unsigned char));

    // allocate memory for decoded message
    q->dec_msg_len = 1;
    q->msg_dec = (unsigned char*) malloc(q->dec_msg_len*sizeof(unsigned char));

    // reset object
    wlanframesync_reset(q);
    
#if DEBUG_WLANFRAMESYNC
    // debugging structures
    q->debug_enabled   = 0;
    q->agc_rx          = NULL;
    q->debug_x         = NULL;
    q->debug_rssi      = NULL;
    q->debug_framesyms = NULL;
#endif

    // return object
    return q;
}

// destroy WLAN framing synchronizer object
void wlanframesync_destroy(wlanframesync _q)
{
#if DEBUG_WLANFRAMESYNC
    // free debugging objects if necessary
    if (_q->agc_rx          != NULL) agc_crcf_destroy(_q->agc_rx);
    if (_q->debug_x         != NULL) windowcf_destroy(_q->debug_x);
    if (_q->debug_rssi      != NULL) windowf_destroy(_q->debug_rssi);
    if (_q->debug_framesyms != NULL) windowcf_destroy(_q->debug_framesyms);
#endif

    // free transform object
    windowcf_destroy(_q->input_buffer);
    free(_q->X);
    free(_q->x);
    FFT_DESTROY_PLAN(_q->fft);
    
    // destroy synchronizer objects
    nco_crcf_destroy(_q->nco_rx);       // numerically-controlled oscillator
    wlan_lfsr_destroy(_q->ms_pilot);    // pilot sequence generator

    // free memory for encoded message
    free(_q->msg_enc);

    // free main object memory
    free(_q);
}

// print WLAN framing synchronizer object internals
void wlanframesync_print(wlanframesync _q)
{
    printf("wlanframesync:\n");
}

// reset WLAN framing synchronizer object internal state
void wlanframesync_reset(wlanframesync _q)
{
    // clear buffer
    windowcf_reset(_q->input_buffer);

    // reset NCO object
    nco_crcf_reset(_q->nco_rx);

    // reset timers/state
    _q->state = WLANFRAMESYNC_STATE_SEEKPLCP;
    _q->timer = 0;
    _q->num_symbols = 0;    // number of received OFDM data symbols
    _q->phi_prime = 0.0f;   // reset phase offset estimate

    // reset pilot sequence generator
    wlan_lfsr_reset(_q->ms_pilot);
}

// execute framing synchronizer on input buffer
//  _q      :   framing synchronizer object
//  _buffer :   input buffer [size: _n x 1]
//  _n      :   input buffer size
void wlanframesync_execute(wlanframesync          _q,
                           liquid_float_complex * _buffer,
                           unsigned int           _n)
{
    unsigned int i;
    float complex x;
    for (i=0; i<_n; i++) {
        x = _buffer[i];

        // correct for carrier frequency offset (only if not in
        // initial 'seek PLCP' state)
        if (_q->state != WLANFRAMESYNC_STATE_SEEKPLCP) {
            nco_crcf_mix_down(_q->nco_rx, x, &x);
            nco_crcf_step(_q->nco_rx);
        }

        // save input sample to buffer
        windowcf_push(_q->input_buffer,x);

#if DEBUG_WLANFRAMESYNC
        if (_q->debug_enabled) {
            // apply agc (estimate initial signal gain)
            float complex y;
            agc_crcf_execute(_q->agc_rx, x, &y);

            windowcf_push(_q->debug_x, x);
            windowf_push(_q->debug_rssi, agc_crcf_get_rssi(_q->agc_rx));
        }
#endif

        switch (_q->state) {
        case WLANFRAMESYNC_STATE_SEEKPLCP:
            wlanframesync_execute_seekplcp(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSHORT0:
            wlanframesync_execute_rxshort0(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSHORT1:
            wlanframesync_execute_rxshort1(_q);
            break;
        case WLANFRAMESYNC_STATE_RXLONG0:
            wlanframesync_execute_rxlong0(_q);
            break;
        case WLANFRAMESYNC_STATE_RXLONG1:
            wlanframesync_execute_rxlong1(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSIGNAL:
            wlanframesync_execute_rxsignal(_q);
            break;
        case WLANFRAMESYNC_STATE_RXDATA:
            wlanframesync_execute_rxdata(_q);
            break;
        default:;
            // should never get to this point
            fprintf(stderr,"error: wlanframesync_execute(), invalid state\n");
            exit(1);
        }
    } // for (i=0; i<_n; i++)
}

// get receiver RSSI
float wlanframesync_get_rssi(wlanframesync _q)
{
    return 0.0f;
}

// get receiver carrier frequency offset estimate
float wlanframesync_get_cfo(wlanframesync _q)
{
    return 0.0f;
}


//
// internal methods
//

// frame detection
void wlanframesync_execute_seekplcp(wlanframesync _q)
{
    _q->timer++;

    // TODO : only check every 100 - 150 (decimates/reduced complexity)
    if (_q->timer < 64)
        return;

    // reset timer
    _q->timer = 0;

    // read contents of input buffer
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);
    
    // estimate gain
    // TODO : use gain from result of FFT
    unsigned int i;
    float g = 0.0f;
    for (i=16; i<80; i+=4) {
        // compute |rc[i]|^2 efficiently
        g += crealf(rc[i  ])*crealf(rc[i  ]) + cimagf(rc[i  ])*cimagf(rc[i  ]);
        g += crealf(rc[i+1])*crealf(rc[i+1]) + cimagf(rc[i+1])*cimagf(rc[i+1]);
        g += crealf(rc[i+2])*crealf(rc[i+2]) + cimagf(rc[i+2])*cimagf(rc[i+2]);
        g += crealf(rc[i+3])*crealf(rc[i+3]) + cimagf(rc[i+3])*cimagf(rc[i+3]);
    }
    g = 64.0f / (g + 1e-12f);
    
    // save gain (permits dynamic invocation of get_rssi() method)
    _q->g0 = g;

    // estimate S0 gain
    wlanframesync_estimate_gain_S0(_q, &rc[16], _q->G0a);
    
    // compute S0 metrics
    float complex s_hat;
    wlanframesync_S0_metrics(_q, _q->G0a, &s_hat);
    s_hat *= g;

    float tau_hat  = cargf(s_hat) * (float)(16.0f) / (2*M_PI);
#if DEBUG_WLANFRAMESYNC_PRINT
    printf(" - gain=%12.3f, rssi=%8.2f dB, s_hat=%12.4f <%12.8f>, tau_hat=%8.3f\n",
            sqrt(g),
            -10*log10(g),
            cabsf(s_hat), cargf(s_hat),
            tau_hat);
#endif

    // 
    if (cabsf(s_hat) > WLANFRAMESYNC_S0A_ABS_THRESH) {

        int dt = (int)roundf(tau_hat);
        // set timer appropriately...
        _q->timer = (16 + dt) % 16;
        //_q->timer += 32; // add delay to help ensure good S0 estimate (multiple of 16)
        _q->state = WLANFRAMESYNC_STATE_RXSHORT0;

#if DEBUG_WLANFRAMESYNC_PRINT
        printf("********** frame detected! ************\n");
        printf("    s_hat   :   %12.8f <%12.8f>\n", cabsf(s_hat), cargf(s_hat));
        printf("  tau_hat   :   %12.8f\n", tau_hat);
        printf("    dt      :   %12d\n", dt);
        printf("    timer   :   %12u\n", _q->timer);
#endif
    }

}

// frame detection
void wlanframesync_execute_rxshort0(wlanframesync _q)
{
    _q->timer++;
    if (_q->timer < 16)
        return;

    // reset timer
    _q->timer = 0;

    // read contents of input buffer
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);

    // re-estimate S0 gain
    wlanframesync_estimate_gain_S0(_q, &rc[16], _q->G0a);

    float complex s_hat;
    wlanframesync_S0_metrics(_q, _q->G0a, &s_hat);
    //float g = agc_crcf_get_gain(_q->agc_rx);
    s_hat *= _q->g0;

    // save first 'short' symbol statistic
    _q->s0a_hat = s_hat;

#if DEBUG_WLANFRAMESYNC_PRINT
    float tau_hat  = cargf(s_hat) * 16.0f / (2*M_PI);
    printf("********** S0[a] received ************\n");
    printf("    s_hat   :   %12.8f <%12.8f>\n", cabsf(s_hat), cargf(s_hat));
    printf("  tau_hat   :   %12.8f\n", tau_hat);
#endif

    _q->state = WLANFRAMESYNC_STATE_RXSHORT1;
}

// frame detection
void wlanframesync_execute_rxshort1(wlanframesync _q)
{
    _q->timer++;
    if (_q->timer < 16)
        return;

    // reset timer
    _q->timer = 0;

    // read contents of input buffer
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);

    // estimate S0 gain
    wlanframesync_estimate_gain_S0(_q, &rc[16], _q->G0b);

    float complex s_hat;
    wlanframesync_S0_metrics(_q, _q->G0b, &s_hat);
    //float g = agc_crcf_get_gain(_q->agc_rx);
    s_hat *= _q->g0;

    // save second 'short' symbol statistic
    _q->s0b_hat = s_hat;

#if DEBUG_WLANFRAMESYNC_PRINT
    float tau_hat  = cargf(s_hat) * 16.0f / (2*M_PI);
    printf("********** S0[b] received ************\n");
    printf("    s_hat   :   %12.8f <%12.8f>\n", cabsf(s_hat), cargf(s_hat));
    printf("  tau_hat   :   %12.8f\n", tau_hat);
    
    // new timing offset estimate
    tau_hat  = cargf(_q->s0a_hat + _q->s0b_hat) * 16.0f / (2*M_PI);
    printf("  tau_hat * :   %12.8f\n", tau_hat);
#endif

#if 0
    // compute carrier frequency offset estimate using ML method
    float complex t0 = 0.0f;
    for (i=0; i<48; i++) {
        t0 += conjf(rc[i])   *       wlanframe_s0[i] * 
                    rc[i+16] * conjf(wlanframe_s0[i+16]);
    }
    float nu_hat = cargf(t0) / (float)(_q->M2);
#else
    // compute carrier frequency offset estimate using freq. domain method
    float nu_hat = wlanframesync_estimate_cfo_S0(_q->G0a, _q->G0b);
#endif

    // set NCO frequency
    nco_crcf_set_frequency(_q->nco_rx, nu_hat);

#if DEBUG_WLANFRAMESYNC_PRINT
    printf("   nu_hat[0]:   %12.8f\n", nu_hat);
#endif

    // set state
    _q->state = WLANFRAMESYNC_STATE_RXLONG0;
}

void wlanframesync_execute_rxlong0(wlanframesync _q)
{
    // set timer to 16, wait for phase to be relatively small
    
    _q->timer++;
    if (_q->timer < 16)
        return;

    // reset timer
    _q->timer = 0;

    // run fft
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);

    // estimate S1 gain, adding backoff in gain estimation
    wlanframesync_estimate_gain_S1(_q, &rc[16-2], _q->G1a);

    // compute S1 metrics
    float complex s_hat;
    wlanframesync_S1_metrics(_q, _q->G1a, &s_hat);
    s_hat *= _q->g0;    // scale output by raw gain estimate

    // rotate by complex phasor relative to timing backoff
    //s_hat *= cexpf(_Complex_I * 2.0f * 2.0f * M_PI / 64.0f);
    s_hat *= cexpf(_Complex_I * 0.19635f);

    // save first 'long' symbol statistic
    _q->s1a_hat = s_hat;

#if DEBUG_WLANFRAMESYNC_PRINT
    printf("    s_hat   :   %12.8f <%12.8f>\n", cabsf(s_hat), cargf(s_hat));
#endif

    float s_hat_abs = cabsf(s_hat);
    float s_hat_arg = cargf(s_hat);
    if (s_hat_arg >  M_PI) s_hat_arg -= 2.0f*M_PI;
    if (s_hat_arg < -M_PI) s_hat_arg += 2.0f*M_PI;
    
    // check conditions for s_hat:
    //  1. magnitude should be large (near unity) when aligned
    //  2. phase should be very near zero (time aligned)
    if (s_hat_abs        > WLANFRAMESYNC_S1A_ABS_THRESH &&
        fabsf(s_hat_arg) < WLANFRAMESYNC_S1A_ARG_THRESH)
    {
#if DEBUG_WLANFRAMESYNC_PRINT
        printf("    acquisition S1[a]\n");
#endif
        
        // set state
        _q->state = WLANFRAMESYNC_STATE_RXLONG1;

        // reset timer
        _q->timer = 0;
    }

}

void wlanframesync_execute_rxlong1(wlanframesync _q)
{
    _q->timer++;
    if (_q->timer < 64)
        return;

    // run fft
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);

    // estimate S1 gain, adding backoff in gain estimation
    wlanframesync_estimate_gain_S1(_q, &rc[16-2], _q->G1b);

    // compute S1 metrics
    float complex s_hat;
    wlanframesync_S1_metrics(_q, _q->G1b, &s_hat);
    s_hat *= _q->g0;    // scale output by raw gain estimate

    // rotate by complex phasor relative to timing backoff
    //s_hat *= cexpf(_Complex_I * 2.0f * 2.0f * M_PI / 64.0f);
    s_hat *= cexpf(_Complex_I * 0.19635f);

    // save second 'long' symbol statistic
    _q->s1b_hat = s_hat;

    // rotate by complex phasor relative to timing backoff
    //s_hat *= liquid_cexpjf((float)(_q->backoff)*2.0f*M_PI/(float)(_q->M));

#if DEBUG_WLANFRAMESYNC_PRINT
    printf("    s_hat   :   %12.8f <%12.8f>\n", cabsf(s_hat), cargf(s_hat));
#endif

    // check conditions for s_hat
    float s_hat_abs = cabsf(s_hat);
    float s_hat_arg = cargf(s_hat);
    if (s_hat_arg >  M_PI) s_hat_arg -= 2.0f*M_PI;
    if (s_hat_arg < -M_PI) s_hat_arg += 2.0f*M_PI;
        
    // check conditions for s_hat:
    //  1. magnitude should be large (near unity) when aligned
    //  2. phase should be very near zero (time aligned)
    if (s_hat_abs        > WLANFRAMESYNC_S1B_ABS_THRESH &&
        fabsf(s_hat_arg) < WLANFRAMESYNC_S1B_ARG_THRESH)
    {
#if DEBUG_WLANFRAMESYNC_PRINT
        printf("    acquisition S1[b]\n");
#endif
        
        // refine CFO estimate with G1a, G1b and adjust NCO appropriately
        float nu_hat = wlanframesync_estimate_cfo_S1(_q->G1a, _q->G1b);
        nco_crcf_adjust_frequency(_q->nco_rx, nu_hat);
#if DEBUG_WLANFRAMESYNC_PRINT
        printf("   nu_hat[1]:   %12.8f\n", nu_hat);
#endif
        // TODO : de-rotate S1b by phase offset (help with equalizer)

        // estimate equalizer with G1a, G1b
        wlanframesync_estimate_eqgain_poly(_q);
        
        // set state
        _q->state = WLANFRAMESYNC_STATE_RXLONG1;

        // reset timer
        _q->timer = 0;
    }

    // set state
    _q->state = WLANFRAMESYNC_STATE_RXSIGNAL;

    // reset timer
    _q->timer = 0;

}

// receive the 'SIGNAL' field
void wlanframesync_execute_rxsignal(wlanframesync _q)
{
    _q->timer++;
    if (_q->timer < 80)
        return;

    // reset timer
    _q->timer = 0;

    // run fft
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);
    memmove(_q->x, &rc[16-2], 64*sizeof(float complex));

    // compute fft, storing result into _q->X
    FFT_EXECUTE(_q->fft);
  
    // recover symbol, correcting for gain, pilot phase, etc.
    wlanframesync_rxsymbol(_q);
    
    // demodulate, decode, ...
    memset(_q->signal_int, 0x00, 6*sizeof(unsigned char));

    _q->signal_int[0] |= crealf(_q->X[38]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[39]) > 0.0f ? 0x40 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[40]) > 0.0f ? 0x20 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[41]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[42]) > 0.0f ? 0x08 : 0x00;
    //  43 : pilot
    _q->signal_int[0] |= crealf(_q->X[44]) > 0.0f ? 0x04 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[45]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[0] |= crealf(_q->X[46]) > 0.0f ? 0x01 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[47]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[48]) > 0.0f ? 0x40 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[49]) > 0.0f ? 0x20 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[50]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[51]) > 0.0f ? 0x08 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[52]) > 0.0f ? 0x04 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[53]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[1] |= crealf(_q->X[54]) > 0.0f ? 0x01 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[55]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[56]) > 0.0f ? 0x40 : 0x00;
    //  57 : pilot
    _q->signal_int[2] |= crealf(_q->X[58]) > 0.0f ? 0x20 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[59]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[60]) > 0.0f ? 0x08 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[61]) > 0.0f ? 0x04 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[62]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[2] |= crealf(_q->X[63]) > 0.0f ? 0x01 : 0x00;
    //   0 : NULL
    _q->signal_int[3] |= crealf(_q->X[ 1]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 2]) > 0.0f ? 0x40 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 3]) > 0.0f ? 0x20 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 4]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 5]) > 0.0f ? 0x08 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 6]) > 0.0f ? 0x04 : 0x00;
    //   7 : pilot
    _q->signal_int[3] |= crealf(_q->X[ 8]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[3] |= crealf(_q->X[ 9]) > 0.0f ? 0x01 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[10]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[11]) > 0.0f ? 0x40 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[12]) > 0.0f ? 0x20 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[13]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[14]) > 0.0f ? 0x08 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[15]) > 0.0f ? 0x04 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[16]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[4] |= crealf(_q->X[17]) > 0.0f ? 0x01 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[18]) > 0.0f ? 0x80 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[19]) > 0.0f ? 0x40 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[20]) > 0.0f ? 0x20 : 0x00;
    //  21 : pilot
    _q->signal_int[5] |= crealf(_q->X[22]) > 0.0f ? 0x10 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[23]) > 0.0f ? 0x08 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[24]) > 0.0f ? 0x04 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[25]) > 0.0f ? 0x02 : 0x00;
    _q->signal_int[5] |= crealf(_q->X[26]) > 0.0f ? 0x01 : 0x00;

    // decode SIGNAL field
    wlanframesync_decode_signal(_q);

    // validate proper decoding
    if (!_q->signal_valid) {
        // reset synchronizer and return
        wlanframesync_reset(_q);
        return;
    }

    // set state
    _q->state = WLANFRAMESYNC_STATE_RXDATA;
}

// receive data symbols
void wlanframesync_execute_rxdata(wlanframesync _q)
{
    _q->timer++;
    if (_q->timer < 80)
        return;

    //printf("    receiving symbol %u...\n", _q->num_symbols);

    // reset timer
    _q->timer = 0;

    // run fft
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);
    memmove(_q->x, &rc[16-2], 64*sizeof(float complex));

    // compute fft, storing result into _q->X
    FFT_EXECUTE(_q->fft);
  
    // recover symbol, correcting for gain, pilot phase, etc.
    wlanframesync_rxsymbol(_q);
   
    // demodulate and pack
    unsigned int i;
    unsigned int n=0;
    unsigned int sym;
    for (i=0; i<64; i++) {
        unsigned int k = (i + 32) % 64;

        if ( k==0 || (k > 26 && k < 38) ) {
            // NULL subcarrier
        } else if (k==43 || k==57 || k==7 || k==21) {
            // PILOT subcarrier
        } else {
            // DATA subcarrier
            assert(n<48);
            sym = wlan_demodulate(_q->mod_scheme, _q->X[k]);
            _q->modem_syms[n] = sym;
            n++;
#if DEBUG_WLANFRAMESYNC
            // TODO : move this outside loop
            if (_q->debug_enabled)
                windowcf_push(_q->debug_framesyms, _q->X[k]);
#endif

        }
    }
    assert(n==48);

    // pack modem symbols
    //printf("  %3u = %3u * %3u\n", _q->enc_msg_len, _q->nsym, _q->bytes_per_symbol);
    unsigned int num_written;
    liquid_wlan_repack_bytes(_q->modem_syms, _q->nbpsc, 48,
                             &_q->msg_enc[_q->num_symbols * _q->bytes_per_symbol], 8, _q->bytes_per_symbol,
                             &num_written);
    assert(num_written == _q->bytes_per_symbol);

    // increment number of received symbols
    _q->num_symbols++;

    // check number of symbols
    if (_q->num_symbols == _q->nsym) {
        // decode message
        wlan_packet_decode(_q->rate, _q->seed, _q->length, _q->msg_enc, _q->msg_dec);

        // assemble RX vector
        struct wlan_rxvector_s rxvector;
        rxvector.LENGTH     = _q->length;
        rxvector.RSSI       = 200 + (unsigned int) (10*log10f(_q->g0));
        rxvector.DATARATE   = _q->rate;
        rxvector.SERVICE    = 0;

        // invoke callback
        if (_q->callback != NULL) {
            //int retval = 
            _q->callback(_q->msg_dec, rxvector, _q->userdata);
        }

        // reset and return
        wlanframesync_reset(_q);
    }
}

// estimate short sequence gain
//  _q      :   wlanframesync object
//  _x      :   input array (time), [size: M x 1]
//  _G      :   output gain (freq)
void wlanframesync_estimate_gain_S0(wlanframesync _q,
                                    float complex * _x,
                                    float complex * _G)
{
    // move input array into fft input buffer
    memmove(_q->x, _x, 64*sizeof(float complex));

    // compute fft, storing result into _q->X
    FFT_EXECUTE(_q->fft);
    
    // compute gain, ignoring NULL subcarriers
    unsigned int i;
    float gain = 0.054127f; // sqrt(12)/64 ; sqrtf(_q->M_S0) / (float)(_q->M);

    // clear input
    for (i=0; i<64; i++) _G[i] = 0.0f;

    // NOTE : if cabsf(_q->S0[i]) == 0 then we can multiply by conjugate
    //        rather than compute division
    //_G[i] = _q->X[i] / _q->S0[i];
    _G[40] = _q->X[40] * conjf(wlanframe_S0[40]) * gain;
    _G[44] = _q->X[44] * conjf(wlanframe_S0[44]) * gain;
    _G[48] = _q->X[48] * conjf(wlanframe_S0[48]) * gain;
    _G[52] = _q->X[52] * conjf(wlanframe_S0[52]) * gain;
    _G[56] = _q->X[56] * conjf(wlanframe_S0[56]) * gain;
    _G[60] = _q->X[60] * conjf(wlanframe_S0[60]) * gain;
    //
    _G[ 4] = _q->X[ 4] * conjf(wlanframe_S0[ 4]) * gain;
    _G[ 8] = _q->X[ 8] * conjf(wlanframe_S0[ 8]) * gain;
    _G[12] = _q->X[12] * conjf(wlanframe_S0[12]) * gain;
    _G[16] = _q->X[16] * conjf(wlanframe_S0[16]) * gain;
    _G[20] = _q->X[20] * conjf(wlanframe_S0[20]) * gain;
    _G[24] = _q->X[24] * conjf(wlanframe_S0[24]) * gain;
}

// compute S0 metrics
void wlanframesync_S0_metrics(wlanframesync _q,
                              float complex * _G,
                              float complex * _s_hat)
{
    // timing, carrier offset correction
    float complex s_hat = 0.0f;

    // compute timing estimate, accumulate phase difference across
    // gains on subsequent pilot subcarriers (note that all the odd
    // subcarriers are NULL)
#if 0
    unsigned int i;
    for (i=0; i<64; i+=4)
        s_hat += _G[(i+4)%64]*conjf(_G[i]);
#else
    s_hat += _G[44] * conjf(_G[40]);
    s_hat += _G[48] * conjf(_G[44]);
    s_hat += _G[52] * conjf(_G[48]);
    s_hat += _G[56] * conjf(_G[52]);
    s_hat += _G[60] * conjf(_G[56]);
    //           0             60
    //           4              0
    s_hat += _G[ 8] * conjf(_G[ 4]);
    s_hat += _G[12] * conjf(_G[ 8]);
    s_hat += _G[16] * conjf(_G[12]);
    s_hat += _G[20] * conjf(_G[16]);
    s_hat += _G[24] * conjf(_G[20]);
#endif

    // set output values, normalizing by number of elements
    *_s_hat = s_hat * 0.1f;
}

// estimate carrier frequency offset from S0 gains
float wlanframesync_estimate_cfo_S0(float complex * _G0a,
                                    float complex * _G0b)
{
    // compute carrier frequency offset estimate using freq. domain method
    float complex g_hat = 0.0f;
    g_hat += _G0b[40] * conjf(_G0a[40]);
    g_hat += _G0b[44] * conjf(_G0a[44]);
    g_hat += _G0b[48] * conjf(_G0a[48]);
    g_hat += _G0b[52] * conjf(_G0a[52]);
    g_hat += _G0b[56] * conjf(_G0a[56]);
    g_hat += _G0b[60] * conjf(_G0a[60]);
    //
    g_hat += _G0b[ 4] * conjf(_G0a[ 4]);
    g_hat += _G0b[ 8] * conjf(_G0a[ 8]);
    g_hat += _G0b[12] * conjf(_G0a[12]);
    g_hat += _G0b[16] * conjf(_G0a[16]);
    g_hat += _G0b[20] * conjf(_G0a[20]);
    g_hat += _G0b[24] * conjf(_G0a[24]);

    return 4.0f * cargf(g_hat) / 64.0f;
}


// estimate long sequence gain
//  _q      :   wlanframesync object
//  _x      :   input array (time), [size: M x 1]
//  _G      :   output gain (freq)
void wlanframesync_estimate_gain_S1(wlanframesync _q,
                                    float complex * _x,
                                    float complex * _G)
{
    // move input array into fft input buffer
    memmove(_q->x, _x, 64*sizeof(float complex));

    // compute fft, storing result into _q->X
    FFT_EXECUTE(_q->fft);
    
    // nominal gain (normalization factor)
    float gain = 0.11267f; // sqrt(52)/64 ; sqrtf(_q->M_S1) / (float)(_q->M);

    // compute gain, ignoring NULL subcarriers
    unsigned int i;
    for (i=0; i<64; i++) {
        if (i == 0 || (i>26 && i<38) ) {
            // NULL subcarrier
            _G[i] = 0.0f;
        } else {
            // DATA/PILOT subcarrier (S1 enabled)
            _G[i] = _q->X[i] * conjf(wlanframe_S1[i]) * gain;
        }
    }
}

// compute S1 metrics
void wlanframesync_S1_metrics(wlanframesync _q,
                              float complex * _G,
                              float complex * _s_hat)
{
    // compute detector output
    float complex s_hat = 0.0f;

    unsigned int i;
    for (i=0; i<64; i++)
        s_hat += _G[(i+1)%64]*conjf(_G[i]);

    // set output values, normalizing by number of elements
    *_s_hat = s_hat * 0.019231f;    // 1/52
}

// estimate carrier frequency offset from S1 gains
float wlanframesync_estimate_cfo_S1(float complex * _G1a,
                                    float complex * _G1b)
{
    // compute carrier frequency offset estimate using freq. domain method
    float complex g_hat = 0.0f;
    unsigned int i;
    for (i=0; i<64; i++)
        g_hat += _G1b[i] * conjf(_G1a[i]);

    // return CFO offset estimate
    // TODO : check if this needs to be negated
    return cargf(g_hat) / 64.0f;
}



// estimate complex equalizer gain from G0 and G1
//  _q      :   wlanframesync object
//  _ntaps  :   number of time-domain taps for smoothing
void wlanframesync_estimate_eqgain(wlanframesync _q,
                                   unsigned int _ntaps)
{
}

// estimate complex equalizer gain from G0 and G1 using polynomial fit
void wlanframesync_estimate_eqgain_poly(wlanframesync _q)
{
    // polynomial order
    unsigned int order = 2;

    // equalizer (polynomial)
    float x_eq[52];             // frequency array
    float y_eq_abs[52];         // magnitude array
    float y_eq_arg[52];         // phase array
    float p_eq_abs[order+1];    // polynomial coefficients (magnitude)
    float p_eq_arg[order+1];    // polynomial coefficients (phase)

    // average complex gains
    unsigned int i;
    unsigned int k;
    unsigned int n=0;
    for (i=0; i<64; i++) {
        // start at mid-point (effective fftshift)
        k = (i + 32) % 64;

        if (k == 0 || (k>26 && k<38) ) {
            // NULL subcarrier
        } else {
            // validate counter
            assert(n < 52);

            // DATA/PILOT subcarrier (S1 enabled)
            //float complex G = 0.5f*(_q->G1a + _q->G1b);
            float complex G = _q->G1b[k];

            // store resulting...
            x_eq[n] = (k > 31) ? (float)k - (float)(64) : (float)k;
            x_eq[n] = x_eq[n] / (float)(64);
            y_eq_abs[n] = cabsf(G);
            y_eq_arg[n] = cargf(G);

            // update counter
            n++;
        }
    }
    
    // validate counter
    assert(n == 52);

    // try to unwrap phase
    for (i=1; i<52; i++) {
        while ((y_eq_arg[i] - y_eq_arg[i-1]) >  M_PI)
            y_eq_arg[i] -= 2*M_PI;
        while ((y_eq_arg[i] - y_eq_arg[i-1]) < -M_PI)
            y_eq_arg[i] += 2*M_PI;
    }

    // fit to polynomial(s)
    polyf_fit(x_eq, y_eq_abs, 52, p_eq_abs, order+1);
    polyf_fit(x_eq, y_eq_arg, 52, p_eq_arg, order+1);

    // compute subcarrier gain
    for (i=0; i<64; i++) {
        
        if (i == 0 || (i>26 && i<38) ) {
            // NULL subcarrier
            _q->G[i] = 0.0f;
            _q->R[i] = 0.0f;
        } else {
            // DATA/PILOT subcarrier (S1 enabled)
            float freq = (i > 31) ? (float)i - (float)(64) : (float)i;
            freq = freq / (float)(64);
            float A     = polyf_val(p_eq_abs, order+1, freq);
            float theta = polyf_val(p_eq_arg, order+1, freq);

            // composite channel estimation
            _q->G[i] = A * cexpf(_Complex_I*theta);

            // composite channel correction
            // 0.11267 = sqrt(52)/64
            _q->R[i] = 0.11267f / (A + 1e-12f) * cexpf(-_Complex_I*theta);
        }
    }

}

// recover symbol, correcting for gain, pilot phase, etc.
void wlanframesync_rxsymbol(wlanframesync _q)
{
    // apply gain
    unsigned int i;
    for (i=0; i<64; i++)
        _q->X[i] *= _q->R[i];

    // polynomial curve-fit
    float x_phase[4] = {-21.0f, -7.0f, 7.0f, 21.0f};
    float y_phase[4];
    float p_phase[2];

    // update pilot phase
    unsigned int pilot_phase = wlan_lfsr_advance(_q->ms_pilot);

    y_phase[0] = pilot_phase ? cargf(-_q->X[43]) : cargf( _q->X[43]);
    y_phase[1] = pilot_phase ? cargf(-_q->X[57]) : cargf( _q->X[57]);
    y_phase[2] = pilot_phase ? cargf(-_q->X[ 7]) : cargf( _q->X[ 7]);
    y_phase[3] = pilot_phase ? cargf( _q->X[21]) : cargf(-_q->X[21]);

    // unwrap phase
    if ( (y_phase[1]-y_phase[0]) >  M_PI ) y_phase[1] -= 2*M_PI;
    if ( (y_phase[1]-y_phase[0]) < -M_PI ) y_phase[1] += 2*M_PI;

    if ( (y_phase[2]-y_phase[1]) >  M_PI ) y_phase[2] -= 2*M_PI;
    if ( (y_phase[2]-y_phase[1]) < -M_PI ) y_phase[2] += 2*M_PI;

    if ( (y_phase[3]-y_phase[2]) >  M_PI ) y_phase[3] -= 2*M_PI;
    if ( (y_phase[3]-y_phase[2]) < -M_PI ) y_phase[3] += 2*M_PI;

#if 0
    printf("    x = [-21 -7 7 21]; y = [%6.3f %6.3f %6.3f %6.3f];\n", y_phase[0], y_phase[1], y_phase[2], y_phase[3]);
#endif

    // fit phase to 1st-order polynomial (2 coefficients)
    polyf_fit(x_phase, y_phase, 4, p_phase, 2);

    // compensate for phase offset
    // TODO : find more computationally efficient way to do this
    for (i=0; i<64; i++) {
        float fx    = (i > 31) ? (float)i - (float)(64) : (float)i;
        float theta = polyf_val(p_phase, 2, fx);
        _q->X[i] *= cexpf(-_Complex_I*theta);
    }

    // adjust NCO frequency based on differential phase
    if (_q->num_symbols > 0) {
        // compute phase error (unwrapped)
        float dphi_prime = p_phase[0] - _q->phi_prime;
        if (dphi_prime >  M_PI) dphi_prime -= M_2_PI;
        if (dphi_prime < -M_PI) dphi_prime += M_2_PI;

        // adjust NCO proportionally to phase error
        nco_crcf_adjust_frequency(_q->nco_rx, 1e-3f*dphi_prime);
    }
    // set internal phase state
    _q->phi_prime = p_phase[0];
}

void wlanframesync_decode_signal(wlanframesync _q)
{
    // de-interleave
    wlan_interleaver_decode_symbol(WLANFRAME_RATE_6, _q->signal_int, _q->signal_enc);

    // decode
    wlan_fec_signal_decode(_q->signal_enc, _q->signal_dec);

    // unpack
    unsigned int R; // 'reserved' bit
    _q->signal_valid = wlan_signal_unpack(_q->signal_dec,
                                          &_q->rate,
                                          &R,
                                          &_q->length);

    // check validity
    if (!_q->signal_valid) {
        printf("SIGNAL field not valid\n");
        return;
    }

#if 0
    if (_q->rate == WLANFRAME_RATE_9) {
        fprintf(stderr,"warning: wlanframesync_decode_signal(), the rate 9 M bits/s is currently unsupported\n");
        _q->signal_valid = 0;
        return;
    }
#endif

    // compute frame parameters
    _q->ndbps  = wlanframe_ratetab[_q->rate].ndbps; // number of data bits per OFDM symbol
    _q->ncbps  = wlanframe_ratetab[_q->rate].ncbps; // number of coded bits per OFDM symbol
    _q->nbpsc  = wlanframe_ratetab[_q->rate].nbpsc; // number of bits per subcarrier (modulation depth)

    // compute number of OFDM symbols
    div_t d = div(16 + 8*_q->length + 6, _q->ndbps);
    _q->nsym = d.quot + (d.rem == 0 ? 0 : 1);

    // compute number of bits in the DATA field
    _q->ndata = _q->nsym * _q->ndbps;

    // compute number of pad bits
    _q->npad = _q->ndata - (16 + 8*_q->length + 6);

    // compute decoded message length (number of data bytes)
    // NOTE : because ndbps is _always_ divisible by 8, so must ndata be
    _q->dec_msg_len = _q->ndata / 8;

    // re-allocate buffer for decoded message
    _q->msg_dec = (unsigned char*) realloc(_q->msg_dec, _q->dec_msg_len*sizeof(unsigned char));

    // compute encoded message length (number of data bytes)
    _q->enc_msg_len = (_q->dec_msg_len * _q->ncbps) / _q->ndbps;

    // compute number of encoded data bytes per OFDM symbol
    _q->bytes_per_symbol = _q->enc_msg_len / _q->nsym;

    // validate encoded message length
    //assert(_q->enc_msg_len == wlan_packet_compute_enc_msg_len(_q->rate, _q->length));

    // re-allocate buffer for encoded message
    _q->msg_enc = (unsigned char*) realloc(_q->msg_enc, _q->enc_msg_len*sizeof(unsigned char));

    // re-create modem object
    _q->mod_scheme = wlanframe_ratetab[_q->rate].mod_scheme;

#if DEBUG_WLANFRAMESYNC_PRINT
    // print properties
    printf("    signal int  :   [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
            _q->signal_int[0],
            _q->signal_int[1],
            _q->signal_int[2],
            _q->signal_int[3],
            _q->signal_int[4],
            _q->signal_int[5]);
    printf("    signal enc  :   [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
            _q->signal_enc[0],
            _q->signal_enc[1],
            _q->signal_enc[2],
            _q->signal_enc[3],
            _q->signal_enc[4],
            _q->signal_enc[5]);
    printf("    signal dec  :   [%.2x %.2x %.2x]\n",
            _q->signal_dec[0],
            _q->signal_dec[1],
            _q->signal_dec[2]);
    printf("    rate        :   %3u Mbits/s\n", wlanframe_ratetab[_q->rate].rate);
    printf("    payload     :   %3u bytes\n", _q->length);
#endif
}

void wlanframesync_debug_enable(wlanframesync _q)
{
    // create debugging objects if necessary
#if DEBUG_WLANFRAMESYNC
    // agc, rssi
    if (_q->agc_rx == NULL)
        _q->agc_rx = agc_crcf_create();
    agc_crcf_set_bandwidth(_q->agc_rx,  1e-2f);
    agc_crcf_set_gain_limits(_q->agc_rx, 1.0f, 1e7f);

    if (_q->debug_x == NULL)
        _q->debug_x = windowcf_create(DEBUG_WLANFRAMESYNC_BUFFER_LEN);

    if (_q->debug_rssi == NULL)
        _q->debug_rssi = windowf_create(DEBUG_WLANFRAMESYNC_BUFFER_LEN);

    if (_q->debug_framesyms == NULL)
        _q->debug_framesyms = windowcf_create(DEBUG_WLANFRAMESYNC_BUFFER_LEN);

    _q->debug_enabled = 1;
#else
    fprintf(stderr,"wlanframesync_debug_enable(): compile-time debugging disabled\n");
#endif
}

void wlanframesync_debug_disable(wlanframesync _q)
{
#if DEBUG_WLANFRAMESYNC
    _q->debug_enabled = 0;
#else
    fprintf(stderr,"wlanframesync_debug_disable(): compile-time debugging disabled\n");
#endif
}

void wlanframesync_debug_print(wlanframesync _q,
                               const char * _filename)
{
#if DEBUG_WLANFRAMESYNC
    if (_q->agc_rx          == NULL ||
        _q->debug_x         == NULL ||
        _q->debug_rssi      == NULL ||
        _q->debug_framesyms == NULL)
    {
        fprintf(stderr,"error: wlanframe_debug_print(), debugging objects don't exist; enable debugging first\n");
        return;
    }

    FILE * fid = fopen(_filename,"w");
    if (!fid) {
        fprintf(stderr,"error: wlanframe_debug_print(), could not open '%s' for writing\n", _filename);
        return;
    }
    fprintf(fid,"%% %s : auto-generated file\n", _filename);

    fprintf(fid,"close all;\n");
    fprintf(fid,"clear all;\n");
    fprintf(fid,"n = %u;\n", DEBUG_WLANFRAMESYNC_BUFFER_LEN);
    unsigned int i;
    float complex * rc;
    float * r;

    fprintf(fid,"x = zeros(1,n);\n");
    windowcf_read(_q->debug_x, &rc);
    for (i=0; i<DEBUG_WLANFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"x(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(0:(n-1),real(x),0:(n-1),imag(x));\n");
    fprintf(fid,"xlabel('sample index');\n");
    fprintf(fid,"ylabel('received signal, x');\n");

    // write agc_rssi
    fprintf(fid,"\n\n");
    fprintf(fid,"agc_rssi = zeros(1,%u);\n", DEBUG_WLANFRAMESYNC_BUFFER_LEN);
    windowf_read(_q->debug_rssi, &r);
    for (i=0; i<DEBUG_WLANFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"agc_rssi(%4u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(agc_rssi)\n");
    fprintf(fid,"ylabel('RSSI [dB]');\n");
    
    // write frame symbols
    fprintf(fid,"framesyms = zeros(1,n);\n");
    windowcf_read(_q->debug_framesyms, &rc);
    for (i=0; i<DEBUG_WLANFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"framesyms(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(real(framesyms),imag(framesyms),'x','MarkerSize',2);\n");
    fprintf(fid,"axis([-1 1 -1 1]*1.5);\n");
    fprintf(fid,"axis square;\n");
    fprintf(fid,"grid on;\n");
    fprintf(fid,"xlabel('real');\n");
    fprintf(fid,"ylabel('imag');\n");

    // write gain arrays
    fprintf(fid,"\n\n");
    fprintf(fid,"G0a = zeros(1,64);\n");
    fprintf(fid,"G0b = zeros(1,64);\n");
    fprintf(fid,"G1a = zeros(1,64);\n");
    fprintf(fid,"G1b = zeros(1,64);\n");
    fprintf(fid,"G   = zeros(1,64);\n");
    for (i=0; i<64; i++) {
        unsigned int k = (i + 32) % 64;
        fprintf(fid,"G0a(%3u) = %12.8f + j*%12.8f;\n", k+1, crealf(_q->G0a[i]), cimagf(_q->G0a[i]));
        fprintf(fid,"G0b(%3u) = %12.8f + j*%12.8f;\n", k+1, crealf(_q->G0b[i]), cimagf(_q->G0b[i]));
        fprintf(fid,"G1a(%3u) = %12.8f + j*%12.8f;\n", k+1, crealf(_q->G1a[i]), cimagf(_q->G1a[i]));
        fprintf(fid,"G1b(%3u) = %12.8f + j*%12.8f;\n", k+1, crealf(_q->G1b[i]), cimagf(_q->G1b[i]));
        fprintf(fid,"G(%3u)   = %12.8f + j*%12.8f;\n", k+1, crealf(_q->G[i]),   cimagf(_q->G[i]));
    }
    fprintf(fid,"%% apply timing offset (backoff) phase shift\n");
    fprintf(fid,"f = -32:31;\n");
    fprintf(fid,"b = 2;\n");
    fprintf(fid,"G0a = G0a.*exp(j*b*2*pi*f/64);\n");
    fprintf(fid,"G0b = G0b.*exp(j*b*2*pi*f/64);\n");
    fprintf(fid,"G1a = G1a.*exp(j*b*2*pi*f/64);\n");
    fprintf(fid,"G1b = G1b.*exp(j*b*2*pi*f/64);\n");
    fprintf(fid,"G   = G.*exp(j*b*2*pi*f/64);\n");

    fprintf(fid,"figure;\n");
    fprintf(fid,"subplot(2,1,1);\n");
    fprintf(fid,"  plot(f,abs(G1a),'x', f,abs(G1b),'x', f,abs(G),'-k','LineWidth',2);\n");
    fprintf(fid,"  ylabel('G (mag)');\n");
    fprintf(fid,"subplot(2,1,2);\n");
    fprintf(fid,"  plot(f,arg(G1a),'x', f,arg(G1b),'x', f,arg(G),'-k','LineWidth',2);\n");
    fprintf(fid,"  ylabel('G (phase)');\n");

    fclose(fid);
    printf("wlanframesync/debug: results written to '%s'\n", _filename);
#else
    fprintf(stderr,"wlanframesync_debug_print(): compile-time debugging disabled\n");
#endif
}


