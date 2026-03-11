/*   Bridge Command 5.0 Ship Simulator
Copyright (C) 2018 James Packer

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
GNU General Public License For more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

//Based on sample code from https://github.com/hosackm/wavplayer/blob/master/main.c

#ifndef __SOUND_HPP_INCLUDED__
#define __SOUND_HPP_INCLUDED__

#define FRAMES_PER_BUFFER   (512)

#ifdef WITH_SOUND
#include <sndfile.h>
#include <portaudio.h>
#endif // WITH_SOUND
#include <string.h>
#include <iostream>
#include <vector>
#include <cmath>

#include "ISound.hpp"

class Sound : public ISound
{
public:

	Sound();
	~Sound() override;
	void load(std::string engineSoundFile, std::string waveSoundFile, std::string hornSoundFile, std::string alarmSoundFile) override;
	void StartSound() override;
	void setVolumeWave(float vol) override;
	void setVolumeEngine(float vol) override;
	void setVolumeHorn(float vol) override;
	void setVolumeAlarm(float vol) override;
	float getVolumeWave() const override;
	float getVolumeEngine() const override;
	float getVolumeHorn() const override;
	float getVolumeAlarm() const override;
	void setEnginePitch(float pitch) override;
	void setEnvironment(float beaufort, float windSpeedKn) override;
	void setEngineCharacter(float maxRPM, int cylinders, int stroke) override;

#ifdef WITH_SOUND
private:

	typedef struct
	{
		SNDFILE     *fileWave;
		SNDFILE     *fileEngine;
		SNDFILE     *fileHorn;
		SNDFILE     *fileAlarm;
		SF_INFO      infoWave;
		SF_INFO      infoEngine;
		SF_INFO      infoHorn;
		SF_INFO      infoAlarm;
	} callback_data_s;

	static float hornVolume;
	static float waveVolume;
	static float engineVolume;
	static float alarmVolume;
	static float enginePitchValue;     // 0.5 = idle, 1.0 = full power
	static double engineReadPos;       // (legacy, unused)

	// Pre-decoded engine buffer for glitch-free playback
	static std::vector<float> engineBuf;   // interleaved float samples
	static sf_count_t engineBufFrames;     // total frames in buffer
	static int engineBufChannels;          // channel count
	static int engineSampleRate;           // sample rate (Hz)
	static double enginePhase;             // fractional playback position (frames)
	static double dieselPhase;             // procedural diesel firing oscillator
	static float lpState[2];              // one-pole low-pass state (per channel)

	// Environmental audio state
	static float beaufortLevel;           // current Beaufort number (0-12)
	static float windSpeedKnots;          // wind speed in knots
	static float windLpState[2];          // low-pass filter state for wind noise (per channel)
	static float windBpState[2];          // band-pass state for wind tonal component
	static float windGustPhase;           // slow oscillator for gust modulation

	// Engine character parameters (set per vessel)
	static float engMaxRPM;               // engine max RPM
	static int engCylinders;              // number of cylinders
	static int engStroke;                 // 2 or 4 stroke
	static float engIdleRPM;             // derived idle RPM
	static float engLpBase;              // base LP cutoff Hz
	static float engLpRange;             // LP cutoff range Hz
	static float engDieselMix;           // diesel synthesis mix (0-1)
	static float engPlayRateBase;        // base playback rate
	static float engPlayRateRange;       // playback rate variation

	bool soundLoaded;
	static bool waveSoundLoaded;
	static bool hornSoundLoaded;
	static bool alarmSoundLoaded;
	PaError portAudioError;
	PaStream *stream;
	//SNDFILE *file;
	callback_data_s data;

	static int callback
	(const void                     *input
		, void                           *output
		, unsigned long                   frameCount
		, const PaStreamCallbackTimeInfo *timeInfo
		, PaStreamCallbackFlags           statusFlags
		, void                           *userData
	)
	{
		float           *out;
		callback_data_s *p_data = (callback_data_s*)userData;
		sf_count_t       num_read;

		out = (float*)output;
		p_data = (callback_data_s*)userData;

		//Note that we've ensured already that channels are the same for all files, if both waveSoundLoaded and hornSoundLoaded are true

		/* clear output buffer */
		memset(out, 0, sizeof(float) * frameCount * p_data->infoEngine.channels);

		int channels = p_data->infoEngine.channels;

		//Create four buffers, for wave, engine, horn and alarm
		std::vector<float> waveBuffer(frameCount * channels);
		std::vector<float> hornBuffer(frameCount * channels);
		std::vector<float> alarmBuffer(frameCount * channels);

		// Engine: pre-decoded buffer with RPM-dependent filtering + diesel synthesis
		// Parameters adapt to vessel type via engMaxRPM/engCylinders/engStroke
		std::vector<float> engineBuffer(frameCount * channels, 0.0f);
		if (engineBufFrames > 0 && engineBufChannels == channels) {
			float pitch = enginePitchValue;
			if (pitch < 0.25f) pitch = 0.25f;
			if (pitch > 2.0f) pitch = 2.0f;

			// Playback rate: configurable base + range
			float playRate = engPlayRateBase + pitch * engPlayRateRange;
			if (playRate < 0.5f) playRate = 0.5f;
			if (playRate > 2.5f) playRate = 2.5f;

			// RPM-dependent low-pass: configurable per vessel class
			float lpCutoff = engLpBase + pitch * engLpRange;
			float alpha = 1.0f - std::exp(-6.2832f * lpCutoff / (float)engineSampleRate);
			if (alpha > 1.0f) alpha = 1.0f;

			// Diesel firing: cylinders and stroke determine firing frequency
			// 4-stroke: fires every other revolution -> freq = RPM * cyl / 120
			// 2-stroke: fires every revolution -> freq = RPM * cyl / 60
			float rpm = engIdleRPM + pitch * (engMaxRPM - engIdleRPM);
			float strokeDiv = (engStroke == 2) ? 60.0f : 120.0f;
			float firingHz = rpm * (float)engCylinders / strokeDiv;
			double dieselInc = (double)firingHz / (double)engineSampleRate;

			// Pulse width: slower engines have longer, heavier pulses
			// Slow (60 RPM) -> 0.5 cycle, Fast (6000 RPM) -> 0.15 cycle
			float pulseWidth = 0.5f - 0.35f * std::min(1.0f, engMaxRPM / 3000.0f);

			// Diesel amplitude: louder for slow-speed (more mechanical),
			// quieter for high-speed (smoother, more WAV-dependent)
			float dieselAmp = 0.08f + 0.20f * (1.0f - std::min(1.0f, engMaxRPM / 3000.0f));
			float dieselNoise = dieselAmp * 0.5f;

			// Sub-harmonic rumble for slow-speed diesels (< 300 RPM)
			float subRumble = (engMaxRPM < 300.0f) ? 0.12f : 0.0f;
			float subFreqHz = rpm / ((engStroke == 2) ? 1.0f : 2.0f); // crankshaft rotation frequency

			// Mix ratio: WAV vs diesel synthesis
			float wavMix = 1.0f - engDieselMix;
			float synMix = engDieselMix;

			for (unsigned long i = 0; i < frameCount; i++) {
				sf_count_t idx0 = ((sf_count_t)enginePhase) % engineBufFrames;
				sf_count_t idx1 = (idx0 + 1) % engineBufFrames;
				float frac = (float)(enginePhase - (double)(sf_count_t)enginePhase);

				// Procedural diesel: raised-cosine burst at firing rate
				float dp = (float)dieselPhase;
				float pulse = (dp < pulseWidth)
					? 0.5f * (1.0f - std::cos(dp / pulseWidth * 6.2832f)) : 0.0f;
				float noise = ((float)(rand() & 0x7FFF) / 16384.0f - 1.0f);
				float diesel = pulse * dieselAmp + noise * pulse * dieselNoise;

				// Sub-harmonic rumble for slow-speed engines
				if (subRumble > 0.0f) {
					float subPhase = (float)std::fmod(enginePhase * subFreqHz / (double)engineSampleRate * 6.2832, 6.2832);
					diesel += subRumble * std::sin(subPhase) * (0.8f + 0.2f * pulse);
				}

				dieselPhase += dieselInc;
				if (dieselPhase >= 1.0) dieselPhase -= 1.0;

				for (int c = 0; c < channels; c++) {
					float s0 = engineBuf[idx0 * channels + c];
					float s1 = engineBuf[idx1 * channels + c];
					float raw = s0 + frac * (s1 - s0);

					// One-pole low-pass filter
					lpState[c] += alpha * (raw - lpState[c]);

					engineBuffer[i * channels + c] = lpState[c] * wavMix + diesel * synMix;
				}

				enginePhase += (double)playRate;
				if (enginePhase >= (double)engineBufFrames)
					enginePhase -= (double)engineBufFrames;
			}
		}

		if (waveSoundLoaded) {
			num_read = sf_read_float(p_data->fileWave, waveBuffer.data(), frameCount * p_data->infoEngine.channels);
			/*  If we couldn't read a full frameCount of samples we've reached EOF */
			//Try to restart
			if (num_read < frameCount)
			{

				sf_count_t seekLocation = sf_seek(p_data->fileWave, 0, SEEK_SET);
				if (seekLocation == -1) {
					return paComplete;
				}

				//Read again
				/* read directly into output buffer */
				num_read = sf_read_float(p_data->fileWave, waveBuffer.data(), frameCount * p_data->infoEngine.channels);

				/*  If we couldn't read a full frameCount of samples we've reached EOF */
				if (num_read < frameCount) {
					return paComplete;
				}
			}
		}

		if (hornSoundLoaded) {
			num_read = sf_read_float(p_data->fileHorn, hornBuffer.data(), frameCount * p_data->infoEngine.channels);
			/*  If we couldn't read a full frameCount of samples we've reached EOF */
			//Try to restart
			if (num_read < frameCount)
			{

				sf_count_t seekLocation = sf_seek(p_data->fileHorn, 0, SEEK_SET);
				if (seekLocation == -1) {
					return paComplete;
				}

				//Read again
				/* read directly into output buffer */
				num_read = sf_read_float(p_data->fileHorn, hornBuffer.data(), frameCount * p_data->infoEngine.channels);

				/*  If we couldn't read a full frameCount of samples we've reached EOF */
				if (num_read < frameCount) {
					return paComplete;
				}
			}
		}

		if (alarmSoundLoaded) {
			num_read = sf_read_float(p_data->fileAlarm, alarmBuffer.data(), frameCount * p_data->infoEngine.channels);
			/*  If we couldn't read a full frameCount of samples we've reached EOF */
			//Try to restart
			if (num_read < frameCount)
			{

				sf_count_t seekLocation = sf_seek(p_data->fileAlarm, 0, SEEK_SET);
				if (seekLocation == -1) {
					return paComplete;
				}

				//Read again
				/* read directly into output buffer */
				num_read = sf_read_float(p_data->fileAlarm, alarmBuffer.data(), frameCount * p_data->infoEngine.channels);

				/*  If we couldn't read a full frameCount of samples we've reached EOF */
				if (num_read < frameCount) {
					return paComplete;
				}
			}
		}

		// Beaufort-scaled wave volume: silent at B0, full at B6+
		float bWaveVol = waveVolume;
		if (beaufortLevel <= 6.0f) {
			bWaveVol *= beaufortLevel / 6.0f;
		}

		// Procedural wind: filtered white noise with gust modulation
		// Wind audible from ~B3, strong by B7+
		float windIntensity = 0.0f;
		if (beaufortLevel > 2.0f) {
			windIntensity = (beaufortLevel - 2.0f) / 5.0f; // 0 at B2, 1.0 at B7
			if (windIntensity > 1.0f) windIntensity = 1.0f;
		}

		// Wind filter cutoff: higher wind = brighter noise (more high freq)
		// ~200 Hz at light wind, ~2000 Hz at gale
		float windCutoff = 200.0f + windIntensity * 1800.0f;
		float windAlpha = 1.0f - std::exp(-6.2832f * windCutoff / (float)engineSampleRate);
		if (windAlpha > 1.0f) windAlpha = 1.0f;

		// Band-pass centre for tonal "howl" component (~400-800 Hz)
		float howlFreq = 400.0f + windIntensity * 400.0f;
		float howlBw = 0.05f; // narrow Q
		float howlAlpha = 1.0f - std::exp(-6.2832f * howlFreq * howlBw / (float)engineSampleRate);

		// Gust modulation: slow random oscillation (0.05-0.2 Hz)
		float gustInc = (0.05f + windIntensity * 0.15f) / (float)engineSampleRate;

		//Copy into output buffer, with mixing
		for (unsigned long frame = 0; frame < frameCount; frame++) {
			// Wind synthesis (mono, then copy to all channels)
			float windSample = 0.0f;
			if (windIntensity > 0.001f) {
				float noise = ((float)(rand() & 0x7FFF) / 16384.0f - 1.0f);

				// Low-pass filtered noise (broadband wind)
				windLpState[0] += windAlpha * (noise - windLpState[0]);
				float broadband = windLpState[0];

				// Band-pass for tonal howl
				windBpState[0] += howlAlpha * (noise - windBpState[0]);
				windBpState[1] += howlAlpha * (windBpState[0] - windBpState[1]);
				float howl = windBpState[0] - windBpState[1];

				// Gust envelope: slow sine modulation
				windGustPhase += gustInc;
				if (windGustPhase >= 1.0f) windGustPhase -= 1.0f;
				float gust = 0.7f + 0.3f * std::sin(windGustPhase * 6.2832f);

				windSample = (broadband * 0.7f + howl * 0.3f) * windIntensity * gust * 0.5f;
			}

			for (int c = 0; c < channels; c++) {
				int idx = frame * channels + c;
				float sample = engineVolume * engineBuffer[idx] * 0.30f;
				if (waveSoundLoaded) {
					sample += bWaveVol * waveBuffer[idx] * 0.30f;
				}
				sample += windSample * 0.25f;
				if (hornSoundLoaded) {
					sample += hornVolume * hornBuffer[idx] * 0.30f;
				}
				if (alarmSoundLoaded) {
					sample += alarmVolume * alarmBuffer[idx] * 0.30f;
				}
				out[idx] = sample;
			}
		}

		return paContinue;
	}

#endif // WITH_SOUND

};

#endif
