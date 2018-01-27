//
//  AKSynthOneDSPKernel.hpp
//  AudioKit
//
//  Created by Aurelius Prochazka, revision history on Github.
//  Copyright © 2017 Aurelius Prochazka. All rights reserved.
//
//  20170926: Super HD refactor by Marcus Hobbs

#pragma once

#import <Foundation/Foundation.h>

#import "AKSoundPipeKernel.hpp"
#import "AKSynthOneAudioUnit.h"
#import "AKSynthOneParameter.h"
#import "AEMessageQueue.h"
#import "AEArray.h"
#import <vector>
#import <list>

//#import "AKLinearParameterRamp.hpp"  // have to put this here to get it included in umbrella header


#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#import <AVFoundation/AVFoundation.h>

#define SAMPLE_RATE (44100.f)
#define MAX_POLYPHONY (6)
#define NUM_MIDI_NOTES (128)
#define FTABLE_SIZE (4096)
#define NUM_FTABLES (4)
#define RELEASE_AMPLITUDE_THRESHOLD (0.00001f)
#define DELAY_TIME_FLOOR (0.0001f)

#define DEBUG_DSP_LOGGING (1)
#define DEBUG_NOTE_STATE_LOGGING (0)

#ifdef __cplusplus

static inline double pow2(double x) {
    return x * x;
}

// Convert absolute 12ET note number to frequency
static inline double etNNToHz(int noteNumber) {
    return 440.f * exp2((noteNumber - 69.f)/12.f);
}

// Relative note number to frequency
static inline float nnToHz(float noteNumber) {
    return exp2(noteNumber/12.f);
}

// Convert note number to [possibly] microtonal frequency.  12ET is the default.
// If there are performance issues from calling Swift in the realtime-audio thread we can fall back to copying the tuning table locally
// Profiling shows this is ~0% of CPU on a device
static inline double noteToHz(int noteNumber) {
//    return [AKPolyphonicNode.tuningTable frequencyForNoteNumber:noteNumber];
     return 440.f * exp2((noteNumber - 69.f)/12.f);
}

class AKSynthOneDSPKernel : public AKSoundpipeKernel, public AKOutputBuffered {
public:

    // helper for midi/render thread communication, held-notes, etc
    struct NoteNumber {
        
        int noteNumber;
        
        void init() {
            noteNumber = 60;
        }
    };
    
    
    // helper for arp/seq
    struct SeqNoteNumber {
        
        int noteNumber;
        int onOff;
        
        void init() {
            noteNumber = 60;
            onOff = 1;
        }
        
        void init(int nn, int o) {
            noteNumber = nn;
            onOff = o;
        }
    };
    
    
    // MARK: NoteState: atomic unit of a "note"
    struct NoteState {
        AKSynthOneDSPKernel* kernel;
        
        enum NoteStateStage { stageOff, stageOn, stageRelease };
        NoteStateStage stage = stageOff;
        
        float internalGate = 0;
        float amp = 0;
        float filter = 0;
        int rootNoteNumber = 0; // -1 denotes an invalid note number
        
        //Amplitude ADSR
        sp_adsr *adsr;
        
        //Filter Cutoff Frequency ADSR
        sp_adsr *fadsr;
        
        //Morphing Oscillator 1 & 2
        sp_oscmorph *oscmorph1;
        sp_oscmorph *oscmorph2;
        sp_crossfade *morphCrossFade;
        
        //Subwoofer OSC
        sp_osc *subOsc;
        
        //FM OSC
        sp_fosc *fmOsc;
        
        //NOISE OSC
        sp_noise *noise;
        
        //FILTERS
        sp_moogladder *loPass;
        sp_buthp *hiPass;
        sp_butbp *bandPass;
        sp_crossfade *filterCrossFade;
        
        void init() {
            // OSC AMPLITUDE ENVELOPE
            sp_adsr_create(&adsr);
            sp_adsr_init(kernel->sp, adsr);
            
            // FILTER FREQUENCY ENVELOPE
            sp_adsr_create(&fadsr);
            sp_adsr_init(kernel->sp, fadsr);
            
            // OSC1
            sp_oscmorph_create(&oscmorph1);
            sp_oscmorph_init(kernel->sp, oscmorph1, kernel->ft_array, NUM_FTABLES, 0);
            oscmorph1->freq = 0;
            oscmorph1->amp = 0;
            oscmorph1->wtpos = 0;
            
            // OSC2
            sp_oscmorph_create(&oscmorph2);
            sp_oscmorph_init(kernel->sp, oscmorph2, kernel->ft_array, NUM_FTABLES, 0);
            oscmorph2->freq = 0;
            oscmorph2->amp = 0;
            oscmorph2->wtpos = 0;
            
            // CROSSFADE OSC1 and OSC2
            sp_crossfade_create(&morphCrossFade);
            sp_crossfade_init(kernel->sp, morphCrossFade);
            
            // CROSSFADE DRY AND FILTER
            sp_crossfade_create(&filterCrossFade);
            sp_crossfade_init(kernel->sp, filterCrossFade);
            
            // SUB OSC
            sp_osc_create(&subOsc);
            sp_osc_init(kernel->sp, subOsc, kernel->sine, 0.f);
            
            // FM osc
            sp_fosc_create(&fmOsc);
            sp_fosc_init(kernel->sp, fmOsc, kernel->sine);
            
            // NOISE
            sp_noise_create(&noise);
            sp_noise_init(kernel->sp, noise);
            
            // FILTER
            sp_moogladder_create(&loPass);
            sp_moogladder_init(kernel->sp, loPass);
            sp_butbp_create(&bandPass);
            sp_butbp_init(kernel->sp, bandPass);
            sp_buthp_create(&hiPass);
            sp_buthp_init(kernel->sp, hiPass);
        }
        
        void destroy() {
            sp_adsr_destroy(&adsr);
            sp_adsr_destroy(&fadsr);
            sp_oscmorph_destroy(&oscmorph1);
            sp_oscmorph_destroy(&oscmorph2);
            sp_crossfade_destroy(&morphCrossFade);
            sp_crossfade_destroy(&filterCrossFade);
            sp_osc_destroy(&subOsc);
            sp_fosc_destroy(&fmOsc);
            sp_noise_destroy(&noise);
            sp_moogladder_destroy(&loPass);
            sp_butbp_destroy(&bandPass);
            sp_buthp_destroy(&hiPass);
        }
        
        void clear() {
            internalGate = 0;
            stage = stageOff;
            amp = 0;
            rootNoteNumber = -1;
        }
        
        // helper...supports initialization of playing note for both mono and poly
        inline void startNoteHelper(int noteNumber, int velocity, float frequency) {
            oscmorph1->freq = frequency;
            oscmorph2->freq = frequency;
            subOsc->freq = frequency;
            fmOsc->freq = frequency;

            const float amplitude = (float)pow2(velocity / 127.f);
            oscmorph1->amp = amplitude;
            oscmorph2->amp = amplitude;
            subOsc->amp = amplitude;
            fmOsc->amp = amplitude;
            noise->amp = amplitude;
            
            stage = NoteState::stageOn;
            internalGate = 1;
            rootNoteNumber = noteNumber;
        }
        
        //MARK:NoteState.run()
        //This function needs to be heavily optimized...it is called at SampleRate for each NoteState
        void run(int frameIndex, float *outL, float *outR) {
            
            // isMono
            const bool isMonoMode = (kernel->p[isMono] == 1);
            
            //LFO coefficients used throughout run function; range on [0, amplitude]
            const float lfo1_0_1 = 0.5f * (1.f + kernel->lfo1) * kernel->p[lfo1Amplitude];
            const float lfo2_0_1 = 0.5f * (1.f + kernel->lfo2) * kernel->p[lfo2Amplitude];
            
            //pitchLFO common frequency coefficient
            float commonFrequencyCoefficient = 1.0;
            if (kernel->p[pitchLFO] == 1) {
                commonFrequencyCoefficient = 1 + lfo1_0_1;
            } else if (kernel->p[pitchLFO] == 2) {
                commonFrequencyCoefficient = 1 + lfo2_0_1;
            }
            
            //OSC1 frequency
            const float cachedFrequencyOsc1 = oscmorph1->freq;
            float newFrequencyOsc1 = isMonoMode ?kernel->monoFrequencySmooth :cachedFrequencyOsc1;
            newFrequencyOsc1 *= nnToHz((int)kernel->p[morph1SemitoneOffset]);
            newFrequencyOsc1 *= kernel->detuningMultiplierSmooth * commonFrequencyCoefficient;
            newFrequencyOsc1 = clamp(newFrequencyOsc1, 0.f, 0.5f*SAMPLE_RATE);
            oscmorph1->freq = newFrequencyOsc1;
            
            //OSC1: wavetable
            oscmorph1->wtpos = kernel->p[index1];
            
            //OSC2 frequency
            const float cachedFrequencyOsc2 = oscmorph2->freq;
            float newFrequencyOsc2 = isMonoMode ?kernel->monoFrequencySmooth :cachedFrequencyOsc2;
            newFrequencyOsc2 *= nnToHz((int)kernel->p[morph2SemitoneOffset]);
            newFrequencyOsc2 *= kernel->detuningMultiplierSmooth * commonFrequencyCoefficient;
            

            //LFO DETUNE OSC2: original additive method, now with scaled range based on 4Hz at C3
            const float magicDetune = cachedFrequencyOsc2/261.f;
            if (kernel->p[detuneLFO] == 1) {
                newFrequencyOsc2 += lfo1_0_1 * kernel->p[morph2Detuning] * magicDetune;
            } else if (kernel->p[detuneLFO] == 2) {
                newFrequencyOsc2 += lfo2_0_1 * kernel->p[morph2Detuning] * magicDetune;
            } else {
                newFrequencyOsc2 += kernel->p[morph2Detuning] * magicDetune;
            }
            newFrequencyOsc2 = clamp(newFrequencyOsc2, 0.f, 0.5f*SAMPLE_RATE);
            oscmorph2->freq = newFrequencyOsc2;
            
            //OSC2: wavetable
            oscmorph2->wtpos = kernel->p[index2];
            
            //SUB OSC FREQ
            const float cachedFrequencySub = subOsc->freq;
            float newFrequencySub = isMonoMode ?kernel->monoFrequencySmooth :cachedFrequencySub;
            newFrequencySub *= kernel->detuningMultiplierSmooth / (2.f * (1.f + kernel->p[subOctaveDown])) * commonFrequencyCoefficient;
            newFrequencySub = clamp(newFrequencySub, 0.f, 0.5f*SAMPLE_RATE);
            subOsc->freq = newFrequencySub;
            
            //FM OSC FREQ
            const float cachedFrequencyFM = fmOsc->freq;
            float newFrequencyFM = isMonoMode ?kernel->monoFrequencySmooth :cachedFrequencyFM;
            newFrequencyFM *= kernel->detuningMultiplierSmooth * commonFrequencyCoefficient;
            newFrequencyFM = clamp(newFrequencyFM, 0.f, 0.5f*SAMPLE_RATE);
            fmOsc->freq = newFrequencyFM;
            
            //FM LFO
            float fmOscIndx = kernel->p[fmAmount];
            if (kernel->p[fmLFO] == 1) {
                fmOscIndx = kernel->p[fmAmount] * lfo1_0_1;
            } else if (kernel->p[fmLFO] == 2) {
                fmOscIndx = kernel->p[fmAmount] * lfo2_0_1;
            }
            fmOscIndx = kernel->parameterClamp(fmAmount, fmOscIndx);
            fmOsc->indx = fmOscIndx;
            
            //ADSR
            adsr->atk = (float)kernel->p[attackDuration];
            adsr->rel = (float)kernel->p[releaseDuration];

            //ADSR decay LFO
            float dec = kernel->p[decayDuration];
            if (kernel->p[decayLFO] == 1) {
                dec *= lfo1_0_1;
            } else if (kernel->p[decayLFO] == 2) {
                dec *= lfo2_0_1;
            }
            dec = kernel->parameterClamp(decayDuration, dec);
            adsr->dec = dec;

            //ADSR sustain LFO
            float sus = kernel->p[sustainLevel];
            if (kernel->p[sustainLFO] == 1) {
                sus *= lfo1_0_1;
            } else if (kernel->p[sustainLFO] == 2) {
                sus *= lfo2_0_1;
            }
            sus = kernel->parameterClamp(sustainLevel, sus);
            adsr->sus = sus;
            
            //FILTER FREQ CUTOFF ADSR
            fadsr->atk = (float)kernel->p[filterAttackDuration];
            fadsr->dec = (float)kernel->p[filterDecayDuration];
            fadsr->sus = (float)kernel->p[filterSustainLevel];
            fadsr->rel = (float)kernel->p[filterReleaseDuration];
            
            //OSCMORPH CROSSFADE
            float crossFadePos = kernel->morphBalanceSmooth;
            if (kernel->p[oscMixLFO] == 1.f) {
                crossFadePos = kernel->morphBalanceSmooth + lfo1_0_1;
            } else if (kernel->p[oscMixLFO] == 2.f) {
                crossFadePos = kernel->morphBalanceSmooth + lfo2_0_1;
            }
            crossFadePos = clamp(crossFadePos, 0.f, 1.f);
            morphCrossFade->pos = crossFadePos;
            
            //filterMix is currently hard-coded to 1
            filterCrossFade->pos = kernel->p[filterMix];
            
            //FILTER RESONANCE LFO
            float filterResonance = kernel->resonanceSmooth;
            if (kernel->p[resonanceLFO] == 1) {
                filterResonance *= lfo1_0_1;
            } else if (kernel->p[resonanceLFO] == 2) {
                filterResonance *= lfo2_0_1;
            }
            filterResonance = kernel->parameterClamp(resonance, filterResonance);
            if(kernel->p[filterType] == 0) {
                loPass->res = filterResonance;
            } else if(kernel->p[filterType] == 1) {
                // bandpass bandwidth is a different unit than lopass resonance.
                // take advantage of the range of resonance [0,1].
                const float bandwidth = (0.5f * 0.5f * 0.5f * 0.5f) * SAMPLE_RATE * (-1.f + exp2( clamp(1.f - filterResonance, 0.f, 1.f) ) );
                bandPass->bw = bandwidth;
            }
            
            //FINAL OUTs
            float oscmorph1_out = 0.f;
            float oscmorph2_out = 0.f;
            float osc_morph_out = 0.f;
            float subOsc_out = 0.f;
            float fmOsc_out = 0.f;
            float noise_out = 0.f;
            float filterOut = 0.f;
            float finalOut = 0.f;
            
            // osc amp adsr
            sp_adsr_compute(kernel->sp, adsr, &internalGate, &amp);
            
            // filter cutoff adsr
            sp_adsr_compute(kernel->sp, fadsr, &internalGate, &filter);
            
            // filter frequency cutoff calculation
            float filterCutoffFreq = kernel->cutoffSmooth;
            if (kernel->p[cutoffLFO] == 1) {
                filterCutoffFreq *= lfo1_0_1;
            } else if (kernel->p[cutoffLFO] == 2) {
                filterCutoffFreq *= lfo2_0_1;
            }
            
            // filter env lfo crossfade
            float filterEnvLFOMix = kernel->p[filterADSRMix];
            if (kernel->p[filterEnvLFO] == 1.f) {
                filterEnvLFOMix *= lfo1_0_1;
            } else if (kernel->p[filterEnvLFO] == 2.f) {
                filterEnvLFOMix *= lfo2_0_1;
            }
            filterCutoffFreq -= filterCutoffFreq * filterEnvLFOMix * (1.f - filter);
            filterCutoffFreq = kernel->parameterClamp(cutoff, filterCutoffFreq);
            loPass->freq = filterCutoffFreq;
            bandPass->freq = filterCutoffFreq;
            hiPass->freq = filterCutoffFreq;
            
            //oscmorph1_out
            sp_oscmorph_compute(kernel->sp, oscmorph1, nil, &oscmorph1_out);
            oscmorph1_out *= kernel->p[morph1Volume];
            
            //oscmorph2_out
            sp_oscmorph_compute(kernel->sp, oscmorph2, nil, &oscmorph2_out);
            oscmorph2_out *= kernel->p[morph2Volume];
            
            //osc_morph_out
            sp_crossfade_compute(kernel->sp, morphCrossFade, &oscmorph1_out, &oscmorph2_out, &osc_morph_out);
            
            //subOsc_out
            sp_osc_compute(kernel->sp, subOsc, nil, &subOsc_out);
            if (kernel->p[subIsSquare]) {
                if (subOsc_out > 0.f) {
                    subOsc_out = kernel->p[subVolume];
                } else {
                    subOsc_out = -kernel->p[subVolume];
                }
            } else {
                // make sine louder
                subOsc_out *= kernel->p[subVolume] * 2.f * 1.5f;
            }
            
            //fmOsc_out
            sp_fosc_compute(kernel->sp, fmOsc, nil, &fmOsc_out);
            fmOsc_out *= kernel->p[fmVolume];
            
            //noise_out
            sp_noise_compute(kernel->sp, noise, nil, &noise_out);
            noise_out *= kernel->p[noiseVolume];
            if (kernel->p[noiseLFO] == 1) {
                noise_out *= lfo1_0_1;
            } else if (kernel->p[noiseLFO] == 2) {
                noise_out *= lfo2_0_1;
            }

            //synthOut
            float synthOut = amp * (osc_morph_out + subOsc_out + fmOsc_out + noise_out);
            
            //filterOut
            if(kernel->p[filterType] == 0) {
                sp_moogladder_compute(kernel->sp, loPass, &synthOut, &filterOut);
            } else if (kernel->p[filterType] == 1) {
                sp_butbp_compute(kernel->sp, bandPass, &synthOut, &filterOut);
            } else if (kernel->p[filterType] == 2) {
                sp_buthp_compute(kernel->sp, hiPass, &synthOut, &filterOut);
            }
            
            // filter crossfade
            sp_crossfade_compute(kernel->sp, filterCrossFade, &synthOut, &filterOut, &finalOut);
            
            // final output
            outL[frameIndex] += finalOut;
            outR[frameIndex] += finalOut;
            
            // restore cached values
            oscmorph1->freq = cachedFrequencyOsc1;
            oscmorph2->freq = cachedFrequencyOsc2;
            subOsc->freq = cachedFrequencySub;
            fmOsc->freq = cachedFrequencyFM;
        }
    };
    
    // MARK: AKSynthOneDSPKernel Member Functions
    
    AKSynthOneDSPKernel() {}
    
    //efficient parameter setter/getter method
    inline void setAK1Parameter(AKSynthOneParameter param, float inputValue) {
        const float value = parameterClamp(param, inputValue);
        if(p[param] != value) {
            p[param] = value;
#if DEBUG_DSP_LOGGING
            const char* d = parameterCStr(param);
            printf("AKSynthOneDSPKernel.hpp:setAK1Parameter(): %i:%s --> %f\n", param, d, value);
#endif
        }
    }
    
    inline float getAK1Parameter(int param) {
        return p[param];
    }
    
    void setParameters(float params[]) {
        for (int i = 0; i < AKSynthOneParameter::AKSynthOneParameterCount; i++) {
#if DEBUG_DSP_LOGGING
            const float oldValue = p[i];
            const float newValue = params[i];
            const bool changed = (oldValue != newValue);
            if(changed) {
                const char* desc = parameterCStr((AKSynthOneParameter)i);
                printf("AKSynthOneDSPKernel.hpp:setParameters(): #%i: %s: = %s%f\n", i, desc, changed ? "*" : " ", params[i] );
            }
#endif
            p[i] = params[i];
        }
    }
    
    void setParameter(AUParameterAddress address, AUValue value) {
        const int i = (int)address;
        const float oldValue = p[i];
        const bool changed = (oldValue != value);
        if(changed) {
            p[i] = value;
        }
    }
    
    AUValue getParameter(AUParameterAddress address) {
        int i = (int)address;
        return p[i];
    }
    
    void startRamp(AUParameterAddress address, AUValue value, AUAudioFrameCount duration) override {}
    
    ///DEBUG_NOTE_STATE_LOGGING (1) can cause race conditions
    inline void print_debug() {
#if DEBUG_NOTE_STATE_LOGGING
        printf("\n-------------------------------------\n");
        printf("\nheldNoteNumbers:\n");
        for (NSNumber* nnn in heldNoteNumbers) {
            printf("%li, ", (long)nnn.integerValue);
        }
        
        if(p[isMono] == 1) {
            printf("\nmonoNote noteNumber:%i, freq:%f, freqSmooth:%f\n",monoNote.rootNoteNumber, monoFrequency, monoFrequencySmooth);
            
        } else {
            printf("\nplayingNotes:\n");
            for(int i=0; i<MAX_POLYPHONY; i++) {
                if(playingNoteStatesIndex == i)
                    printf("*");
                const int nn = noteStates[i].rootNoteNumber;
                printf("%i:%i, ", i, nn);
            }
        }
        printf("\n-------------------------------------\n");
#endif
    }
    
    ///panic...hard-resets DSP.  artifacts.
    void resetDSP() {
        [heldNoteNumbers removeAllObjects];
        [heldNoteNumbersAE updateWithContentsOfArray:heldNoteNumbers];
        arpSeqLastNotes.clear();
        arpSeqNotes.clear();
        arpSeqNotes2.clear();
        arpBeatCounter = 0;
        p[arpIsOn] = 0.f;
        monoNote.clear();
        for(int i =0; i < MAX_POLYPHONY; i++)
            noteStates[i].clear();
        
        print_debug();
    }
    
    
    ///puts all notes in release mode...no artifacts
    void stopAllNotes() {
        [heldNoteNumbers removeAllObjects];
        [heldNoteNumbersAE updateWithContentsOfArray:heldNoteNumbers];
        if (p[isMono] == 1) {
            // MONO
            stopNote(60);
        } else {
            // POLY
            for(int i=0; i<NUM_MIDI_NOTES; i++)
                stopNote(i);
        }
        print_debug();
    }
    
    void handleTempoSetting(float currentTempo) {
        if (currentTempo != tempo) {
            tempo = currentTempo;
        }
    }
    
    ///can be called from within the render loop
    inline void beatCounterDidChange() {
        AEMessageQueuePerformSelectorOnMainThread(audioUnit->_messageQueue,
                                                  audioUnit,
                                                  @selector(arpBeatCounterDidChange),
                                                  AEArgumentNone);
    }
    
    ///can be called from within the render loop
    inline void playingNotesDidChange() {
        AEMessageQueuePerformSelectorOnMainThread(audioUnit->_messageQueue,
                                                  audioUnit,
                                                  @selector(playingNotesDidChange),
                                                  AEArgumentNone);
    }
    
    ///can be called from within the render loop
    inline void heldNotesDidChange() {
        AEMessageQueuePerformSelectorOnMainThread(audioUnit->_messageQueue,
                                                  audioUnit,
                                                  @selector(heldNotesDidChange),
                                                  AEArgumentNone);
    }
    
    //MARK: PROCESS
    void process(AUAudioFrameCount frameCount, AUAudioFrameCount bufferOffset) override {
        initializeNoteStates();
        
        // PREPARE FOR RENDER LOOP...updates here happen at 44100/512 HZ
        
        // define buffers
        float* outL = (float*)outBufferListPtr->mBuffers[0].mData + bufferOffset;
        float* outR = (float*)outBufferListPtr->mBuffers[1].mData + bufferOffset;
        
        // update lfo and portamento input parameters at 44100/512 HZ
        monoFrequencyPort->htime = p[glide]; // htime is half-time in seconds
        lfo1Phasor->freq = p[lfo1Rate];
        lfo2Phasor->freq = p[lfo2Rate];
        panOscillator->freq = p[autoPanFrequency];
        panOscillator->amp = p[autoPanAmount];
        bitcrush->bitdepth = p[bitCrushDepth];
        delayL->del = p[delayTime] * 2.f + DELAY_TIME_FLOOR;
        delayR->del = p[delayTime] * 2.f + DELAY_TIME_FLOOR;
        delayRR->del = p[delayTime] + DELAY_TIME_FLOOR;
        delayFillIn->del = p[delayTime] + DELAY_TIME_FLOOR;
        delayL->feedback = p[delayFeedback];
        delayR->feedback = p[delayFeedback];
        *phaser0->Notch_width = p[phaserNotchWidth];
        *phaser0->feedback_gain = p[phaserFeedback];
        *phaser0->lfobpm = p[phaserRate];
        
        // transition playing notes from release to off...outside render block because it's not expensive to let the release linger
        bool transitionedToOff = false;
        if (p[isMono] == 1.f) {
            if (monoNote.stage == NoteState::stageRelease && monoNote.amp < RELEASE_AMPLITUDE_THRESHOLD) {
                monoNote.clear();
                transitionedToOff = true;
            }
        } else {
            for(int i=0; i<polyphony; i++) {
                NoteState& note = noteStates[i];
                if (note.stage == NoteState::stageRelease && note.amp < RELEASE_AMPLITUDE_THRESHOLD) {
                    note.clear();
                    transitionedToOff = true;
                }
            }
        }
        if (transitionedToOff)
            playingNotesDidChange();
        
        const float arpTempo = p[arpRate];
        const double secPerBeat = 0.5f * 0.5f * 60.f / arpTempo;
        
        // RENDER LOOP: Render one audio frame at sample rate, i.e. 44100 HZ ////////////////
        for (AUAudioFrameCount frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            
            // CLEAR BUFFER
            outL[frameIndex] = outR[frameIndex] = 0.f;
            
            // Clear all notes when changing Mono <==> Poly
            if( p[isMono] != previousProcessMonoPolyStatus ) {
                previousProcessMonoPolyStatus = p[isMono];
                reset(); // clears all mono and poly notes
                arpSeqLastNotes.clear();
            }
            
            //MARK: ARP/SEQ
            if( p[arpIsOn] == 1.f || !!arpSeqLastNotes.size() ) {
                //TODO:here is where we are not sending beat zero to the delegate
                const double oldArpTime = arpTime;
                const double r0 = fmod(oldArpTime, secPerBeat);
                arpTime = arpSampleCounter/SAMPLE_RATE;
                const double r1 = fmod(arpTime, secPerBeat);
                arpSampleCounter += 1.0;
                if (r1 < r0 || oldArpTime >= arpTime) {
                    
                    // MARK: ARP+SEQ: NEW beatCounter: Create Arp/Seq array based on held notes and/or sequence parameters
                    if (p[arpIsOn] == 1.f) {
                        arpSeqNotes.clear();
                        arpSeqNotes2.clear();

                        // only update "notes per octave" when beat counter changes so arpSeqNotes and arpSeqLastNotes match
                        
                        notesPerOctave = (int)AKPolyphonicNode.tuningTable.npo; // Profiling shows this is ~0% of CPU on a device
                        if(notesPerOctave <= 0) notesPerOctave = 12;
                        const float npof = (float)notesPerOctave/12.f; // 12ET ==> npof = 1
                        
                        // SEQUENCER
                        if(p[arpIsSequencer] == 1.f) {
                            const int numSteps = p[arpTotalSteps] > 16 ? 16 : (int)p[arpTotalSteps];
                            for(int i = 0; i < numSteps; i++) {
                                const float onOff = p[i + arpSeqNoteOn00];
                                const int octBoost = p[i + arpSeqOctBoost00];
                                const int nn = p[i + arpSeqPattern00] * npof;
                                const int nnob = (nn < 0) ? (nn - octBoost * notesPerOctave) : (nn + octBoost * notesPerOctave);
                                struct SeqNoteNumber snn;
                                snn.init(nnob, onOff);
                                arpSeqNotes.push_back(snn);
                            }
                        } else {
                            
                            // ARP state
                            
                            // reverse
                            AEArrayEnumeratePointers(heldNoteNumbersAE, struct NoteNumber *, note) {
                                std::vector<NoteNumber>::iterator it = arpSeqNotes2.begin();
                                arpSeqNotes2.insert(it, *note);
                            }
                            const int heldNotesCount = (int)arpSeqNotes2.size();
                            const int arpIntervalUp = p[arpInterval] * npof;
                            const int onOff = 1;
                            const int arpOctaves = (int)p[arpOctave] + 1;

                            if (p[arpDirection] == 0.f) {
                                
                                // ARP Up
                                int index = 0;
                                for (int octave = 0; octave < arpOctaves; octave++) {
                                    for (int i = 0; i < heldNotesCount; i++) {
                                        struct NoteNumber& note = arpSeqNotes2[i];
                                        const int nn = note.noteNumber + (octave * arpIntervalUp);
                                        struct SeqNoteNumber snn;
                                        snn.init(nn, onOff);
                                        std::vector<SeqNoteNumber>::iterator it = arpSeqNotes.begin() + index;
                                        arpSeqNotes.insert(it, snn);
                                        ++index;
                                    }
                                }

                            } else if (p[arpDirection] == 1.f) {
                                
                                ///ARP Up + Down
                                //up
                                int index = 0;
                                for (int octave = 0; octave < arpOctaves; octave++) {
                                    for (int i = 0; i < heldNotesCount; i++) {
                                        struct NoteNumber& note = arpSeqNotes2[i];
                                        const int nn = note.noteNumber + (octave * arpIntervalUp);
                                        struct SeqNoteNumber snn;
                                        snn.init(nn, onOff);
                                        std::vector<SeqNoteNumber>::iterator it = arpSeqNotes.begin() + index;
                                        arpSeqNotes.insert(it, snn);
                                        ++index;
                                    }
                                }
                                //down, minus head and tail
                                for (int octave = arpOctaves - 1; octave >= 0; octave--) {
                                    for (int i = heldNotesCount - 1; i >= 0; i--) {
                                        const bool firstNote = (i == heldNotesCount - 1) && (octave == arpOctaves - 1);
                                        const bool lastNote = (i == 0) && (octave == 0);
                                        if (!firstNote && !lastNote) {
                                            struct NoteNumber& note = arpSeqNotes2[i];
                                            const int nn = note.noteNumber + (octave * arpIntervalUp);
                                            struct SeqNoteNumber snn;
                                            snn.init(nn, onOff);
                                            std::vector<SeqNoteNumber>::iterator it = arpSeqNotes.begin() + index;
                                            arpSeqNotes.insert(it, snn);
                                            ++index;
                                        }
                                    }
                                }
                                
                            } else if (p[arpDirection] == 2.f) {
                                
                                // ARP Down
                                int index = 0;
                                for (int octave = arpOctaves - 1; octave >= 0; octave--) {
                                    for (int i = heldNotesCount - 1; i >= 0; i--) {
                                        struct NoteNumber& note = arpSeqNotes2[i];
                                        const int nn = note.noteNumber + (octave * arpIntervalUp);
                                        struct SeqNoteNumber snn;
                                        snn.init(nn, onOff);
                                        std::vector<SeqNoteNumber>::iterator it = arpSeqNotes.begin() + index;
                                        arpSeqNotes.insert(it, snn);
                                        ++index;
                                    }
                                }
                            }
                        }
                    }
                    
                    // MARK: ARP+SEQ: turnOff previous beat's notes
                    for (std::list<int>::iterator arpLastNotesIterator = arpSeqLastNotes.begin(); arpLastNotesIterator != arpSeqLastNotes.end(); ++arpLastNotesIterator) {
                        turnOffKey(*arpLastNotesIterator);
                    }
                    
                    // Remove last played notes
                    arpSeqLastNotes.clear();
                    
                    // NOP: no midi input
                    if(heldNoteNumbersAE.count == 0) {
                        if(arpBeatCounter > 0) {
                            arpBeatCounter = 0;
                            beatCounterDidChange();
                        }
                        continue;
                    }
                    
                    // NOP: the arp/seq sequence is null
                    if(arpSeqNotes.size() == 0)
                        continue;
                    
                    // Advance arp/seq beatCounter, notify delegates
                    const int seqNotePosition = arpBeatCounter % arpSeqNotes.size();
                    ++arpBeatCounter;
                    beatCounterDidChange();
                    
                    // MARK: ARP+SEQ: turnOn the note of the sequence
                    SeqNoteNumber& snn = arpSeqNotes[seqNotePosition];
                    if (p[arpIsSequencer] == 1.f) {
                        // SEQUENCER
                        if(snn.onOff == 1) {
                            AEArrayEnumeratePointers(heldNoteNumbersAE, struct NoteNumber *, noteStruct) {
                                const int baseNote = noteStruct->noteNumber;
                                const int note = baseNote + snn.noteNumber;
                                if(note >= 0 && note < NUM_MIDI_NOTES) {
                                    turnOnKey(note, 127);
                                    arpSeqLastNotes.push_back(note);
                                }
                            }
                        }
                    } else {
                        // ARPEGGIATOR
                        const int note = snn.noteNumber;
                        if(note >= 0 && note < NUM_MIDI_NOTES) {
                            turnOnKey(note, 127);
                            arpSeqLastNotes.push_back(note);
                        }
                    }
                }
            }
            
            //LFO1 on [-1, 1]
            sp_phasor_compute(sp, lfo1Phasor, nil, &lfo1);
            if (p[lfo1Index] == 0) { // Sine
                lfo1 = sin(lfo1 * M_PI * 2.f);
            } else if (p[lfo1Index] == 1) { // Square
                if (lfo1 > 0.5f) {
                    lfo1 = 1.f;
                } else {
                    lfo1 = -1.f;
                }
            } else if (p[lfo1Index] == 2) { // Saw
                lfo1 = (lfo1 - 0.5f) * 2.f;
            } else if (p[lfo1Index] == 3) { // Reversed Saw
                lfo1 = (0.5f - lfo1) * 2.f;
            }
            
            //LFO2 on [-1, 1]
            sp_phasor_compute(sp, lfo2Phasor, nil, &lfo2);
            if (p[lfo2Index] == 0) { // Sine
                lfo2 = sin(lfo2 * M_PI * 2.0);
            } else if (p[lfo2Index] == 1) { // Square
                if (lfo2 > 0.5f) {
                    lfo2 = 1.f;
                } else {
                    lfo2 = -1.f;
                }
            } else if (p[lfo2Index] == 2) { // Saw
                lfo2 = (lfo2 - 0.5f) * 2.f;
            } else if (p[lfo2Index] == 3) { // Reversed Saw
                lfo2 = (0.5f - lfo2) * 2.f;
            }
            
            //PORTAMENTO
            sp_port_compute(sp, multiplierPort,    &(p[detuningMultiplier]), &detuningMultiplierSmooth);
            sp_port_compute(sp, balancePort,       &(p[morphBalance]),       &morphBalanceSmooth);
            sp_port_compute(sp, cutoffPort,        &(p[cutoff]),             &cutoffSmooth);
            sp_port_compute(sp, resonancePort,     &(p[resonance]),          &resonanceSmooth);
            sp_port_compute(sp, monoFrequencyPort, &monoFrequency,           &monoFrequencySmooth);

            // RENDER NoteState into (outL, outR)
            if(p[isMono] == 1.f) {
                if(monoNote.rootNoteNumber != -1 && monoNote.stage != NoteState::stageOff)
                    monoNote.run(frameIndex, outL, outR);
            } else {
                for(int i=0; i<polyphony; i++) {
                    NoteState& note = noteStates[i];
                    if (note.rootNoteNumber != -1 && note.stage != NoteState::stageOff)
                        note.run(frameIndex, outL, outR);
                }
            }
            
            ///TODO:still true?
            // NoteState render output "synthOut" is mono
            float synthOut = outL[frameIndex];
            
            //BITCRUSH
            float bitcrushSrate = p[bitCrushSampleRate];
            if(p[bitcrushLFO] == 1.f) {
                bitcrushSrate *= (1.f + 0.5f * lfo1 * p[lfo1Amplitude]);
            } else if (p[bitcrushLFO] == 2.f) {
                bitcrushSrate *= (1.f + 0.5f * lfo2 * p[lfo2Amplitude]);
            }
            bitcrush->srate = parameterClamp(bitCrushSampleRate, bitcrushSrate);
            float bitCrushOut = 0.f;
            sp_bitcrush_compute(sp, bitcrush, &synthOut, &bitCrushOut);
            
            //AUTOPAN
            float panValue = 0.f;
            sp_osc_compute(sp, panOscillator, nil, &panValue);
            panValue *= p[autoPanAmount];
            if(p[autopanLFO] == 1.f) {
                panValue *= 0.5f * (1.f + lfo1) * p[lfo1Amplitude];
            } else if(p[autopanLFO] == 2.f) {
                panValue *= 0.5f * (1.f + lfo2) * p[lfo2Amplitude];
            }
            pan->pan = panValue;
            float panL = 0.f, panR = 0.f;
            sp_pan2_compute(sp, pan, &bitCrushOut, &panL, &panR);
            
            //PHASER
            float phaserOutL = panL;
            float phaserOutR = panR;
            float lPhaserMix = p[phaserMix];
            
            // crossfade phaser
            if(lPhaserMix != 0.f) {
                lPhaserMix = 1.f - lPhaserMix; // invert, baby
                sp_phaser_compute(sp, phaser0, &panL, &panR, &phaserOutL, &phaserOutR);
                phaserOutL = lPhaserMix * panL + (1.f - lPhaserMix) * phaserOutL;
                phaserOutR = lPhaserMix * panR + (1.f - lPhaserMix) * phaserOutR;
            }
            
            // delays
            float delayOutL = 0.f;
            float delayOutR = 0.f;
            float delayOutRR = 0.f;
            float delayFillInOut = 0.f;
            sp_smoothdelay_compute(sp, delayL,      &phaserOutL, &delayOutL);
            sp_smoothdelay_compute(sp, delayR,      &phaserOutR, &delayOutR);
            sp_smoothdelay_compute(sp, delayFillIn, &phaserOutR, &delayFillInOut);
            sp_smoothdelay_compute(sp, delayRR,     &delayOutR,  &delayOutRR);
            delayOutRR += delayFillInOut;
            
            // delays mixer
            float mixedDelayL = 0.f;
            float mixedDelayR = 0.f;
            delayCrossfadeL->pos = p[delayMix] * p[delayOn];
            delayCrossfadeR->pos = p[delayMix] * p[delayOn];
            sp_crossfade_compute(sp, delayCrossfadeL, &phaserOutL, &delayOutL, &mixedDelayL);
            sp_crossfade_compute(sp, delayCrossfadeR, &phaserOutR, &delayOutRR, &mixedDelayR);
            
            // Butterworth hi-pass filter for reverb input
            float butOutL = 0.f;
            float butOutR = 0.f;
            butterworthHipassL->freq = p[reverbHighPass];
            butterworthHipassR->freq = p[reverbHighPass];
            sp_buthp_compute(sp, butterworthHipassL, &mixedDelayL, &butOutL);
            sp_buthp_compute(sp, butterworthHipassR, &mixedDelayR, &butOutR);
            
            // X2 has an AKPeakLimiter with "preGain" of 3db but we're just doing the gain here
            butOutL *= 2.f;
            butOutR *= 2.f;
            
            //TODO:Put dynamics compressor here to match X2 hipass peaklimiter input to reverb per https://trello.com/c/b20JqxTR
            // reverb
            float revOutL = 0.f;
            float revOutR = 0.f;
            reverbCostello->lpfreq = 0.5f * SAMPLE_RATE;
            reverbCostello->feedback = p[reverbFeedback];
            sp_revsc_compute(sp, reverbCostello, &butOutL, &butOutR, &revOutL, &revOutR);
            
            // reverb crossfade
            float reverbCrossfadeOutL = 0.f;
            float reverbCrossfadeOutR = 0.f;
            const float reverbMixFactor = p[reverbMix] * p[reverbOn];
            revCrossfadeL->pos = reverbMixFactor;
            revCrossfadeR->pos = reverbMixFactor;
            sp_crossfade_compute(sp, revCrossfadeL, &mixedDelayL, &revOutL, &reverbCrossfadeOutL);
            sp_crossfade_compute(sp, revCrossfadeR, &mixedDelayR, &revOutR, &reverbCrossfadeOutR);
            
            // X2 AKPeakLimiter ==> AKDynamicsCompressor
            // X2 has an AKPeakLimiter with preGain of 3db
            reverbCrossfadeOutL *= 2.f;
            reverbCrossfadeOutR *= 2.f;
            float compressorOutL = 0.f;
            float compressorOutR = 0.f;
            sp_compressor_compute(sp, compressor0, &reverbCrossfadeOutL, &compressorOutL);
            sp_compressor_compute(sp, compressor1, &reverbCrossfadeOutR, &compressorOutR);
            
            // MASTER
            outL[frameIndex] = compressorOutL * p[masterVolume];
            outR[frameIndex] = compressorOutR * p[masterVolume];
        }
    }
    
    inline void turnOnKey(int noteNumber, int velocity) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        
        const float frequency = noteToHz(noteNumber);
        turnOnKey(noteNumber, velocity, frequency);
    }
    
    // turnOnKey is called by render thread in "process", so access note via AEArray
    inline void turnOnKey(int noteNumber, int velocity, float frequency) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        initializeNoteStates();
        
        if(p[isMono] == 1.f) {
            NoteState& note = monoNote;
            monoFrequency = frequency;
            
            // PORTAMENTO: set the ADSRs to release mode here, then into attack mode inside startNoteHelper
            if(p[monoIsLegato] == 0) {
                note.internalGate = 0;
                note.stage = NoteState::stageRelease;
                sp_adsr_compute(sp, note.adsr, &note.internalGate, &note.amp);
                sp_adsr_compute(sp, note.fadsr, &note.internalGate, &note.filter);
            }
            
            // legato+portamento: Legato means that Presets with low sustains will sound like they did not retrigger.
            note.startNoteHelper(noteNumber, velocity, frequency);
            
        } else {
            // Note Stealing: Is noteNumber already playing?
            int index = -1;
            for(int i = 0 ; i < polyphony; i++) {
                if(noteStates[i].rootNoteNumber == noteNumber) {
                    index = i;
                    break;
                }
            }
            if(index != -1) {
                
                // noteNumber is playing...steal it
                playingNoteStatesIndex = index;
            } else {
                
                // noteNumber is not playing: search for non-playing notes (-1) starting with current index
                for(int i = 0; i < polyphony; i++) {
                    const int modIndex = (playingNoteStatesIndex + i) % polyphony;
                    if(noteStates[modIndex].rootNoteNumber == -1) {
                        index = modIndex;
                        break;
                    }
                }
                
                if(index == -1) {
                    
                    // if there are no non-playing notes then steal oldest note
                    ++playingNoteStatesIndex %= polyphony;
                } else {
                    
                    // use non-playing note slot
                    playingNoteStatesIndex = index;
                }
            }
            
            // POLY: INIT NoteState
            NoteState& note = noteStates[playingNoteStatesIndex];
            note.startNoteHelper(noteNumber, velocity, frequency);
        }
        
        heldNotesDidChange();
        playingNotesDidChange();
    }
    
    // turnOffKey is called by render thread in "process", so access note via AEArray
    inline void turnOffKey(int noteNumber) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        initializeNoteStates();
        if(p[isMono] == 1.f) {
            
            if (heldNoteNumbersAE.count == 0 || p[arpIsOn] == 1.f) {
                
                // the case where this was the only held note and now it should be off
                // the case where the sequencer turns off this key even though a note is held down
                monoNote.stage = NoteState::stageRelease;
                monoNote.internalGate = 0;
            } else {
                
                // the case where you had more than one held note and released one (CACA): Keep note ON and set to freq of head
                AEArrayToken token = AEArrayGetToken(heldNoteNumbersAE);
                struct NoteNumber* nn = (struct NoteNumber*)AEArrayGetItem(token, 0);
                const int headNN = nn->noteNumber;
                monoFrequency = noteToHz(headNN);
                monoNote.rootNoteNumber = headNN;
                monoFrequency = noteToHz(headNN);
                monoNote.oscmorph1->freq = monoFrequency;
                monoNote.oscmorph2->freq = monoFrequency;
                monoNote.subOsc->freq = monoFrequency;
                monoNote.fmOsc->freq = monoFrequency;
                
                // PORTAMENTO: reset the ADSR inside the render loop
                if(p[monoIsLegato] == 0.f) {
                    monoNote.internalGate = 0;
                    monoNote.stage = NoteState::stageRelease;
                    sp_adsr_compute(sp, monoNote.adsr, &monoNote.internalGate, &monoNote.amp);
                    sp_adsr_compute(sp, monoNote.fadsr, &monoNote.internalGate, &monoNote.filter);
                }
                
                // legato+portamento: Legato means that Presets with low sustains will sound like they did not retrigger.
                monoNote.stage = NoteState::stageOn;
                monoNote.internalGate = 1;
            }
        } else {
            
            // Poly:
            int index = -1;
            for(int i=0; i<polyphony; i++) {
                if(noteStates[i].rootNoteNumber == noteNumber) {
                    index = i;
                    break;
                }
            }
            
            if(index != -1) {
                
                // put NoteState into release
                NoteState& note = noteStates[index];
                note.stage = NoteState::stageRelease;
                note.internalGate = 0;
            } else {
                
                // the case where a note was stolen before the noteOff
            }
        }
        heldNotesDidChange();
        playingNotesDidChange();
    }
    
    // NOTE ON
    // startNote is not called by render thread, but turnOnKey is
    inline void startNote(int noteNumber, int velocity) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        
        const float frequency = noteToHz(noteNumber);
        startNote(noteNumber, velocity, frequency);
    }
    
    // NOTE ON
    // startNote is not called by render thread, but turnOnKey is
    inline void startNote(int noteNumber, int velocity, float frequency) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        
        NSNumber* nn = @(noteNumber);
        [heldNoteNumbers removeObject:nn];
        [heldNoteNumbers insertObject:nn atIndex:0];
        [heldNoteNumbersAE updateWithContentsOfArray:heldNoteNumbers];
        
        // ARP/SEQ
        if(p[arpIsOn] == 1.f) {
            return;
        } else {
            turnOnKey(noteNumber, velocity, frequency);
        }
    }
    
    // NOTE OFF...put into release mode
    inline void stopNote(int noteNumber) {
        if(noteNumber < 0 || noteNumber >= NUM_MIDI_NOTES)
            return;
        
        NSNumber* nn = @(noteNumber);
        [heldNoteNumbers removeObject: nn];
        [heldNoteNumbersAE updateWithContentsOfArray: heldNoteNumbers];
        
        // ARP/SEQ
        if(p[arpIsOn] == 1.f) {
            return;
        } else {
            turnOffKey(noteNumber);
        }
    }
    
    void reset() {
        for (int i = 0; i<MAX_POLYPHONY; i++)
            noteStates[i].clear();
        monoNote.clear();
        resetted = true;
    }
    
    void resetSequencer() {
        arpBeatCounter = 0;
        arpSampleCounter = 0;
        arpTime = 0;
        beatCounterDidChange();
    }
    
    // MIDI
    virtual void handleMIDIEvent(AUMIDIEvent const& midiEvent) override { \
        if (midiEvent.length != 3) return; \
        uint8_t status = midiEvent.data[0] & 0xF0; \
        switch (status) { \
            case 0x80 : {  \
                // note off
                uint8_t note = midiEvent.data[1]; \
                if (note > 127) break; \
                stopNote(note); \
                break; \
            } \
            case 0x90 : {  \
                // note on
                uint8_t note = midiEvent.data[1]; \
                uint8_t veloc = midiEvent.data[2]; \
                if (note > 127 || veloc > 127) break; \
                startNote(note, veloc); \
                break; \
            } \
            case 0xB0 : { \
                uint8_t num = midiEvent.data[1]; \
                if (num == 123) { \
                    stopAllNotes(); \
                } \
                break; \
            } \
        } \
    }
    
    void init(int _channels, double _sampleRate) override {
        AKSoundpipeKernel::init(_channels, _sampleRate);
        sp_ftbl_create(sp, &sine, FTABLE_SIZE);
        sp_gen_sine(sp, sine);
        sp_phasor_create(&lfo1Phasor);
        sp_phasor_init(sp, lfo1Phasor, 0);
        sp_phasor_create(&lfo2Phasor);
        sp_phasor_init(sp, lfo2Phasor, 0);
        sp_port_create(&midiNotePort);
        sp_port_init(sp, midiNotePort, 0.0);
        sp_port_create(&multiplierPort);
        sp_port_init(sp, multiplierPort, 0.02);
        sp_port_create(&balancePort);
        sp_port_init(sp, balancePort, 0.1);
        sp_port_create(&cutoffPort);
        sp_port_init(sp, cutoffPort, 0.05);
        sp_port_create(&resonancePort);
        sp_port_init(sp, resonancePort, 0.05);
        sp_bitcrush_create(&bitcrush);
        sp_bitcrush_init(sp, bitcrush);
        sp_phaser_create(&phaser0);
        sp_phaser_init(sp, phaser0);
        *phaser0->MinNotch1Freq = 100;
        *phaser0->MaxNotch1Freq = 800;
        *phaser0->Notch_width = 1000;
        *phaser0->NotchFreq = 1.5;
        *phaser0->VibratoMode = 1;
        *phaser0->depth = 1;
        *phaser0->feedback_gain = 0;
        *phaser0->invert = 0;
        *phaser0->lfobpm = 30;
        sp_osc_create(&panOscillator);
        sp_osc_init(sp, panOscillator, sine, 0.f);
        sp_pan2_create(&pan);
        sp_pan2_init(sp, pan);
        sp_smoothdelay_create(&delayL);
        sp_smoothdelay_create(&delayR);
        sp_smoothdelay_create(&delayRR);
        sp_smoothdelay_create(&delayFillIn);
        sp_smoothdelay_init(sp, delayL, 10.f, 512);
        sp_smoothdelay_init(sp, delayR, 10.f, 512);
        sp_smoothdelay_init(sp, delayRR, 10.f, 512);
        sp_smoothdelay_init(sp, delayFillIn, 10.f, 512);
        sp_crossfade_create(&delayCrossfadeL);
        sp_crossfade_create(&delayCrossfadeR);
        sp_crossfade_init(sp, delayCrossfadeL);
        sp_crossfade_init(sp, delayCrossfadeR);
        sp_revsc_create(&reverbCostello);
        sp_revsc_init(sp, reverbCostello);
        sp_buthp_create(&butterworthHipassL);
        sp_buthp_init(sp, butterworthHipassL);
        sp_buthp_create(&butterworthHipassR);
        sp_buthp_init(sp, butterworthHipassR);
        sp_crossfade_create(&revCrossfadeL);
        sp_crossfade_create(&revCrossfadeR);
        sp_crossfade_init(sp, revCrossfadeL);
        sp_crossfade_init(sp, revCrossfadeR);
        sp_compressor_create(&compressor0);
        sp_compressor_create(&compressor1);
        sp_compressor_init(sp, compressor0);
        sp_compressor_init(sp, compressor1);
        *compressor0->ratio = 10.f;
        *compressor1->ratio = 10.f;
        *compressor0->thresh = -3.f;
        *compressor1->thresh = -3.f;
        *compressor0->atk = 0.001f;
        *compressor1->atk = 0.001f;
        *compressor0->rel = 0.01f;
        *compressor1->rel = 0.01f;
        sp_port_create(&monoFrequencyPort);
        sp_port_init(sp, monoFrequencyPort, 0.05f);
        
        // array of NoteState structs
        noteStates = (NoteState*)malloc(MAX_POLYPHONY * sizeof(NoteState));
        
        heldNoteNumbers = (NSMutableArray<NSNumber*>*)[NSMutableArray array];
        heldNoteNumbersAE = [[AEArray alloc] initWithCustomMapping:^void *(id item) {
                             struct NoteNumber* noteNumber = (struct NoteNumber*)malloc(sizeof(struct NoteNumber));
                             const int nn = [(NSNumber*)item intValue];
                             noteNumber->noteNumber = nn;
                             return noteNumber;
                             }];
        
        // copy default dsp values
        for(int i = 0; i< AKSynthOneParameter::AKSynthOneParameterCount; i++) {
            const float value = parameterDefault((AKSynthOneParameter)i);
            p[i] = value;
#if DEBUG_DSP_LOGGING
            const char* d = parameterCStr((AKSynthOneParameter)i);
            printf("AKSynthOneDSPKernel.hpp:setAK1Parameter(): %i:%s --> %f\n", i, d, value);
#endif
        }
        previousProcessMonoPolyStatus = p[isMono];
        
        // Reserve arp note cache to reduce possibility of reallocation on audio thread.
        arpSeqNotes.reserve(maxArpSeqNotes);
        arpSeqNotes2.reserve(maxArpSeqNotes);
        arpSeqLastNotes.resize(maxArpSeqNotes);
        
        // initializeNoteStates() must be called AFTER init returns, BEFORE process, turnOnKey, and turnOffKey
    }
    
    void destroy() {
        sp_ftbl_destroy(&sine);
        sp_phasor_destroy(&lfo1Phasor);
        sp_phasor_destroy(&lfo2Phasor);
        sp_port_destroy(&midiNotePort);
        sp_port_destroy(&multiplierPort);
        sp_port_destroy(&balancePort);
        sp_port_destroy(&cutoffPort);
        sp_port_destroy(&resonancePort);
        sp_bitcrush_destroy(&bitcrush);
        sp_phaser_destroy(&phaser0);
        sp_osc_destroy(&panOscillator);
        sp_pan2_destroy(&pan);
        sp_smoothdelay_destroy(&delayL);
        sp_smoothdelay_destroy(&delayR);
        sp_smoothdelay_destroy(&delayRR);
        sp_smoothdelay_destroy(&delayFillIn);
        sp_crossfade_destroy(&delayCrossfadeL);
        sp_crossfade_destroy(&delayCrossfadeR);
        sp_revsc_destroy(&reverbCostello);
        sp_buthp_destroy(&butterworthHipassL);
        sp_buthp_destroy(&butterworthHipassR);
        sp_crossfade_destroy(&revCrossfadeL);
        sp_crossfade_destroy(&revCrossfadeR);
        sp_compressor_destroy(&compressor0);
        sp_compressor_destroy(&compressor1);
        sp_port_destroy(&monoFrequencyPort);
        free(noteStates);
    }
    
    // initializeNoteStates() must be called AFTER init returns
    inline void initializeNoteStates() {
        if(initializedNoteStates == false) {
            initializedNoteStates = true;
            // POLY INIT
            for (int i = 0; i < MAX_POLYPHONY; i++) {
                NoteState& state = noteStates[i];
                state.kernel = this;
                state.init();
                state.stage = NoteState::stageOff;
                state.rootNoteNumber = -1;
            }
            
            // MONO INIT
            monoNote.kernel = this;
            monoNote.init();
            monoNote.stage = NoteState::stageOff;
            monoNote.rootNoteNumber = -1;
        }
    }
    
    void setupWaveform(uint32_t waveform, uint32_t size) {
        tbl_size = size;
        sp_ftbl_create(sp, &ft_array[waveform], tbl_size);
    }
    
    void setWaveformValue(uint32_t waveform, uint32_t index, float value) {
        ft_array[waveform]->tbl[index] = value;
    }
    
    ///parameter min
    inline float parameterMin(AKSynthOneParameter i) {
        return aks1p[i].min;
    }
    
    ///parameter max
    inline float parameterMax(AKSynthOneParameter i) {
        return aks1p[i].max;
    }
    
    ///parameter defaults
    inline float parameterDefault(AKSynthOneParameter i) {
        return parameterClamp(i, aks1p[i].defaultValue);
    }
    
    
    // MARK: Member Variables
public:
    
    AKSynthOneAudioUnit* audioUnit; // link back to parent for AEMessageQueue
    bool resetted = false;
    int arpBeatCounter = 0;
    
    // dsp params
    float p[AKSynthOneParameter::AKSynthOneParameterCount];
    
    // Portamento values
    float morphBalanceSmooth = 0.5666f;
    float detuningMultiplierSmooth = 1.f;
    float cutoffSmooth = 1666.f;
    float resonanceSmooth = 0.5f;
    float monoFrequency = 440.f * exp2((60.f - 69.f)/12.f);
    
    // phasor values
    float lfo1 = 0.f;
    float lfo2 = 0.f;
    
    // midi
    bool notesHeld = false;
    
    
private:
    
    // array of struct NoteState of count MAX_POLYPHONY
    NoteState* noteStates;
    
    // monophonic: single instance of NoteState
    NoteState monoNote;
    
    bool initializedNoteStates = false;
    
    // MAX_POLYPHONY is the limit of hard-coded number of simultaneous notes to render to manage computation.
    // New noteOn events will steal voices to keep this number.
    // For now "polyphony" is constant equal to MAX_POLYPHONY, but with some refactoring we could make it dynamic.
    const int polyphony = MAX_POLYPHONY;
    int playingNoteStatesIndex = 0;
    sp_ftbl *ft_array[NUM_FTABLES];
    UInt32 tbl_size = FTABLE_SIZE;
    sp_phasor *lfo1Phasor;
    sp_phasor *lfo2Phasor;
    sp_ftbl *sine;
    sp_bitcrush *bitcrush;
    sp_pan2 *pan;
    sp_osc *panOscillator;
    sp_phaser *phaser0;
    sp_smoothdelay *delayL;
    sp_smoothdelay *delayR;
    sp_smoothdelay *delayRR;
    sp_smoothdelay *delayFillIn;
    sp_crossfade *delayCrossfadeL;
    sp_crossfade *delayCrossfadeR;
    sp_revsc *reverbCostello;
    sp_buthp *butterworthHipassL;
    sp_buthp *butterworthHipassR;
    sp_crossfade *revCrossfadeL;
    sp_crossfade *revCrossfadeR;
    sp_compressor *compressor0;
    sp_compressor *compressor1;
    sp_port *midiNotePort;
    float midiNote = 0.f;
    float midiNoteSmooth = 0.f;
    sp_port *multiplierPort;
    sp_port *balancePort;
    sp_port *cutoffPort;
    sp_port *resonancePort;
    sp_port *monoFrequencyPort;
    float monoFrequencySmooth = 261.f;
    float tempo = 120.f;
    float previousProcessMonoPolyStatus = 0.f;
    
    // Arp/Seq
    double arpSampleCounter = 0;
    double arpTime = 0;
    int notesPerOctave = 12;
    
    ///once init'd: arpSeqNotes can be accessed and mutated only within process and resetDSP
    std::vector<SeqNoteNumber> arpSeqNotes;
    std::vector<NoteNumber> arpSeqNotes2;
    const int maxArpSeqNotes = 1024; // 128 midi note numbers * 4 arp octaves * up+down

    ///once init'd: arpSeqLastNotes can be accessed and mutated only within process and resetDSP
    std::list<int> arpSeqLastNotes;
    
    // Array of midi note numbers of NoteState's which have had a noteOn event but not yet a noteOff event.
    NSMutableArray<NSNumber*>* heldNoteNumbers;
    AEArray* heldNoteNumbersAE;
    
    ///return clamped value
    inline float parameterClamp(AKSynthOneParameter i, float inputValue) {
        AKS1Param param = aks1p[i];
        const float paramMin = param.min;
        const float paramMax = param.max;
        const float retVal = std::min(std::max(inputValue, paramMin), paramMax);
        return retVal;
    }
    
    ///parameter friendly name as c string
    inline const char* parameterCStr(AKSynthOneParameter i) {
        return aks1p[i].friendlyName.c_str();
    }
    
    ///parameter friendly name
    inline std::string parameterFriendlyName(AKSynthOneParameter i) {
        return aks1p[i].friendlyName;
    }
    
    ///parameter presetKey
    inline std::string parameterPresetKey(AKSynthOneParameter i) {
        return aks1p[i].presetKey;
    }

    // TODO: Ask Aure if this should be converted to AUParameter/AUValue
    typedef struct AKS1Param {
        AKSynthOneParameter param;
        float min;
        float defaultValue;
        float max;
        std::string presetKey;
        std::string friendlyName;
    } AKS1Param;
    
    const AKS1Param aks1p[AKSynthOneParameter::AKSynthOneParameterCount] = {
        { index1,                0, 0, 1, "index1", "Index 1"},
        { index2,                0, 0, 1, "index2", "Index 2"},
        { morphBalance,          0, 0.5, 1, "morphBalance", "morphBalance" },
        { morph1SemitoneOffset,  -12, 0, 12, "morph1SemitoneOffset", "morph1SemitoneOffset" },
        { morph2SemitoneOffset,  -12, 0, 12, "morph2SemitoneOffset", "morph2SemitoneOffset" },
        { morph1Volume,          0, 0.8, 1, "morph1Volume", "morph1Volume" },
        { morph2Volume,          0, 0.8, 1, "morph2Volume", "morph2Volume" },
        { subVolume,             0, 0, 1, "subVolume", "subVolume" },
        { subOctaveDown,         0, 0, 1, "subOctaveDown", "subOctaveDown" },
        { subIsSquare,           0, 0, 1, "subIsSquare", "subIsSquare" },
        { fmVolume,              0, 0, 1, "fmVolume", "fmVolume" },
        { fmAmount,              0, 0, 15, "fmAmount", "fmAmount" },
        { noiseVolume,           0, 0, .25, "noiseVolume", "noiseVolume" },
        { lfo1Index,             0, 0, 3, "lfo1Index", "lfo1Index" },
        { lfo1Amplitude,         0, 0, 1, "lfo1Amplitude", "lfo1Amplitude" },
        { lfo1Rate,              0, 0.25, 10, "lfo1Rate", "lfo1Rate" },
        { cutoff,                256, 2000, 28000, "cutoff", "cutoff" },
        { resonance,             0, 0.1, 0.75, "resonance", "resonance" },
        { filterMix,             0, 1, 1, "filterMix", "filterMix" },
        { filterADSRMix,         0, 0, 1.2, "filterADSRMix", "filterADSRMix" },
        { isMono,                0, 0, 1, "isMono", "isMono" },
        { glide,                 0, 0, 0.2, "glide", "glide" },
        { filterAttackDuration,  0.0005, 0.05, 2, "filterAttackDuration", "filterAttackDuration" },
        { filterDecayDuration,   0.005, 0.05, 2, "filterDecayDuration", "filterDecayDuration" },
        { filterSustainLevel,    0, 1, 1, "filterSustainLevel", "filterSustainLevel" },
        { filterReleaseDuration, 0, 0.5, 2, "filterReleaseDuration", "filterReleaseDuration" },
        { attackDuration,        0.0005, 0.05, 2, "attackDuration", "attackDuration" },
        { decayDuration,         0, 0.005, 2, "decayDuration", "decayDuration" },
        { sustainLevel,          0, 0.8, 1, "sustainLevel", "sustainLevel" },
        { releaseDuration,       0.004, 0.05, 2, "releaseDuration", "releaseDuration" },
        { morph2Detuning,        -4, 0, 4, "morph2Detuning", "morph2Detuning" },
        { detuningMultiplier,    1, 1, 2, "detuningMultiplier", "detuningMultiplier" },
        { masterVolume,          0, 0.5, 2, "masterVolume", "masterVolume" },//
        { bitCrushDepth,         1, 24, 24, "bitCrushDepth", "bitCrushDepth" },
        { bitCrushSampleRate,    4096, 44100, 44100, "bitCrushSampleRate", "bitCrushSampleRate" },
        { autoPanAmount,         0, 0, 1, "autoPanAmount", "autoPanAmount" },
        { autoPanFrequency,      0, 0.25, 10, "autoPanFrequency", "autoPanFrequency" },
        { reverbOn,              0, 1, 1, "reverbOn", "reverbOn" },
        { reverbFeedback,        0, 0.5, 1, "reverbFeedback", "reverbFeedback" },
        { reverbHighPass,        80, 700, 900, "reverbHighPass", "reverbHighPass" },
        { reverbMix,             0, 0, 1, "reverbMix", "reverbMix" },
        { delayOn,               0, 0, 1, "delayOn", "delayOn" },
        { delayFeedback,         0, 0.1, 0.9, "delayFeedback", "delayFeedback" },
        { delayTime,             0.1, 0.5, 1.5, "delayTime", "delayTime" },
        { delayMix,              0, 0.125, 1, "delayMix", "delayMix" },
        { lfo2Index,             0, 0, 3, "lfo2Index", "lfo2Index" },
        { lfo2Amplitude,         0, 0, 1, "lfo2Amplitude", "lfo2Amplitude" },
        { lfo2Rate,              0, 0.25, 10, "lfo2Rate", "lfo2Rate" },
        { cutoffLFO,             0, 0, 2, "cutoffLFO", "cutoffLFO" },
        { resonanceLFO,          0, 0, 2, "resonanceLFO", "resonanceLFO" },
        { oscMixLFO,             0, 0, 2, "oscMixLFO", "oscMixLFO" },
        { sustainLFO,            0, 0, 2, "sustainLFO", "sustainLFO" },
        { decayLFO,              0, 0, 2, "decayLFO", "decayLFO" },
        { noiseLFO,              0, 0, 2, "noiseLFO", "noiseLFO" },
        { fmLFO,                 0, 0, 2, "fmLFO", "fmLFO" },
        { detuneLFO,             0, 0, 2, "detuneLFO", "detuneLFO" },
        { filterEnvLFO,          0, 0, 2, "filterEnvLFO", "filterEnvLFO" },
        { pitchLFO,              0, 0, 2, "pitchLFO", "pitchLFO" },
        { bitcrushLFO,           0, 0, 2, "bitcrushLFO", "bitcrushLFO" },
        { autopanLFO,            0, 0, 2, "autopanLFO", "autopanLFO" },
        { arpDirection,          0, 1, 2, "arpDirection", "arpDirection" },
        { arpInterval,           0, 12, 12, "arpInterval", "arpInterval" },
        { arpIsOn,               0, 0, 1, "arpIsOn", "arpIsOn" },
        { arpOctave,             0, 1, 3, "arpOctave", "arpOctave" },
//        { arpRate,               10, 60, 280, "arpRate", "arpRate" },
        { arpRate,               1, 64, 256, "arpRate", "arpRate" },
        { arpIsSequencer,        0, 0, 1, "arpIsSequencer", "arpIsSequencer" },
        { arpTotalSteps,         1, 4, 16, "arpTotalSteps", "arpTotalSteps" },
        { arpSeqPattern00,       -24, 0, 24, "arpSeqPattern00", "arpSeqPattern00" },
        { arpSeqPattern01,       -24, 0, 24, "arpSeqPattern01", "arpSeqPattern01" },
        { arpSeqPattern02,       -24, 0, 24, "arpSeqPattern02", "arpSeqPattern02" },
        { arpSeqPattern03,       -24, 0, 24, "arpSeqPattern03", "arpSeqPattern03" },
        { arpSeqPattern04,       -24, 0, 24, "arpSeqPattern04", "arpSeqPattern04" },
        { arpSeqPattern05,       -24, 0, 24, "arpSeqPattern05", "arpSeqPattern05" },
        { arpSeqPattern06,       -24, 0, 24, "arpSeqPattern06", "arpSeqPattern06" },
        { arpSeqPattern07,       -24, 0, 24, "arpSeqPattern07", "arpSeqPattern07" },
        { arpSeqPattern08,       -24, 0, 24, "arpSeqPattern08", "arpSeqPattern08" },
        { arpSeqPattern09,       -24, 0, 24, "arpSeqPattern09", "arpSeqPattern09" },
        { arpSeqPattern10,       -24, 0, 24, "arpSeqPattern10", "arpSeqPattern10" },
        { arpSeqPattern11,       -24, 0, 24, "arpSeqPattern11", "arpSeqPattern11" },
        { arpSeqPattern12,       -24, 0, 24, "arpSeqPattern12", "arpSeqPattern12" },
        { arpSeqPattern13,       -24, 0, 24, "arpSeqPattern13", "arpSeqPattern13" },
        { arpSeqPattern14,       -24, 0, 24, "arpSeqPattern14", "arpSeqPattern14" },
        { arpSeqPattern15,       -24, 0, 24, "arpSeqPattern15", "arpSeqPattern15" },
        { arpSeqOctBoost00,      0, 0, 1, "arpSeqOctBoost00", "arpSeqOctBoost00" },
        { arpSeqOctBoost01,      0, 0, 1, "arpSeqOctBoost01", "arpSeqOctBoost01" },
        { arpSeqOctBoost02,      0, 0, 1, "arpSeqOctBoost02", "arpSeqOctBoost02" },
        { arpSeqOctBoost03,      0, 0, 1, "arpSeqOctBoost03", "arpSeqOctBoost03" },
        { arpSeqOctBoost04,      0, 0, 1, "arpSeqOctBoost04", "arpSeqOctBoost04" },
        { arpSeqOctBoost05,      0, 0, 1, "arpSeqOctBoost05", "arpSeqOctBoost05" },
        { arpSeqOctBoost06,      0, 0, 1, "arpSeqOctBoost06", "arpSeqOctBoost06" },
        { arpSeqOctBoost07,      0, 0, 1, "arpSeqOctBoost07", "arpSeqOctBoost07" },
        { arpSeqOctBoost08,      0, 0, 1, "arpSeqOctBoost08", "arpSeqOctBoost08" },
        { arpSeqOctBoost09,      0, 0, 1, "arpSeqOctBoost09", "arpSeqOctBoost09" },
        { arpSeqOctBoost10,      0, 0, 1, "arpSeqOctBoost10", "arpSeqOctBoost10" },
        { arpSeqOctBoost11,      0, 0, 1, "arpSeqOctBoost11", "arpSeqOctBoost11" },
        { arpSeqOctBoost12,      0, 0, 1, "arpSeqOctBoost12", "arpSeqOctBoost12" },
        { arpSeqOctBoost13,      0, 0, 1, "arpSeqOctBoost13", "arpSeqOctBoost13" },
        { arpSeqOctBoost14,      0, 0, 1, "arpSeqOctBoost14", "arpSeqOctBoost14" },
        { arpSeqOctBoost15,      0, 0, 1, "arpSeqOctBoost15", "arpSeqOctBoost15" },
        { arpSeqNoteOn00,        0, 0, 1, "arpSeqNoteOn00", "arpSeqNoteOn00" },
        { arpSeqNoteOn01,        0, 0, 1, "arpSeqNoteOn01", "arpSeqNoteOn01" },
        { arpSeqNoteOn02,        0, 0, 1, "arpSeqNoteOn02", "arpSeqNoteOn02" },
        { arpSeqNoteOn03,        0, 0, 1, "arpSeqNoteOn03", "arpSeqNoteOn03" },
        { arpSeqNoteOn04,        0, 0, 1, "arpSeqNoteOn04", "arpSeqNoteOn04" },
        { arpSeqNoteOn05,        0, 0, 1, "arpSeqNoteOn05", "arpSeqNoteOn05" },
        { arpSeqNoteOn06,        0, 0, 1, "arpSeqNoteOn06", "arpSeqNoteOn06" },
        { arpSeqNoteOn07,        0, 0, 1, "arpSeqNoteOn07", "arpSeqNoteOn07" },
        { arpSeqNoteOn08,        0, 0, 1, "arpSeqNoteOn08", "arpSeqNoteOn08" },
        { arpSeqNoteOn09,        0, 0, 1, "arpSeqNoteOn09", "arpSeqNoteOn09" },
        { arpSeqNoteOn10,        0, 0, 1, "arpSeqNoteOn10", "arpSeqNoteOn10" },
        { arpSeqNoteOn11,        0, 0, 1, "arpSeqNoteOn11", "arpSeqNoteOn11" },
        { arpSeqNoteOn12,        0, 0, 1, "arpSeqNoteOn12", "arpSeqNoteOn12" },
        { arpSeqNoteOn13,        0, 0, 1, "arpSeqNoteOn13", "arpSeqNoteOn13" },
        { arpSeqNoteOn14,        0, 0, 1, "arpSeqNoteOn14", "arpSeqNoteOn14" },
        { arpSeqNoteOn15,        0, 0, 1, "arpSeqNoteOn15", "arpSeqNoteOn15" },
        { filterType,            0, 0, 2, "filterType", "filterType" },
        { phaserMix,             0, 0, 1, "phaserMix", "phaserMix" },
        { phaserRate,            12, 12, 300, "phaserRate", "phaserRate" },
        { phaserFeedback,        0, 0.0, 0.8, "phaserFeedback", "phaserFeedback" },
        { phaserNotchWidth,      100, 800, 1000, "phaserNotchWidth", "phaserNotchWidth" },
        { monoIsLegato,          0, 0, 1, "monoIsLegato", "monoIsLegato" }
    };
};

#endif