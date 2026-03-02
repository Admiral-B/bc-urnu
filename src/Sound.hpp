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
		std::vector<float> engineBuffer(frameCount * channels, 0.0f);
		if (engineBufFrames > 0 && engineBufChannels == channels) {
			float pitch = enginePitchValue;
			if (pitch < 0.25f) pitch = 0.25f;
			if (pitch > 2.0f) pitch = 2.0f;

			// Constrained playback rate: small variation avoids unnatural pitch shift
			// pitch 0.5 (idle) -> rate 0.92, pitch 1.0 (full) -> rate 1.08
			float playRate = 0.85f + pitch * 0.23f;
			if (playRate < 0.75f) playRate = 0.75f;
			if (playRate > 1.25f) playRate = 1.25f;

			// RPM-dependent low-pass: darker at low RPM, brighter at high
			float lpCutoff = 1200.0f + pitch * 3000.0f;
			float alpha = 1.0f - std::exp(-6.2832f * lpCutoff / (float)engineSampleRate);
			if (alpha > 1.0f) alpha = 1.0f;

			// Diesel firing pulse: 6-cyl 4-stroke, firing freq = RPM*6/120
			// Map pitch to RPM: 0.5 -> ~400 RPM, 1.0 -> ~1000 RPM
			float rpm = 200.0f + pitch * 800.0f;
			float firingHz = rpm * 6.0f / 120.0f;
			double dieselInc = (double)firingHz / (double)engineSampleRate;

			for (unsigned long i = 0; i < frameCount; i++) {
				sf_count_t idx0 = ((sf_count_t)enginePhase) % engineBufFrames;
				sf_count_t idx1 = (idx0 + 1) % engineBufFrames;
				float frac = (float)(enginePhase - (double)(sf_count_t)enginePhase);

				// Procedural diesel: short raised-cosine burst at firing rate
				float dp = (float)dieselPhase;
				float pulse = (dp < 0.3f)
					? 0.5f * (1.0f - std::cos(dp / 0.3f * 6.2832f)) : 0.0f;
				float noise = ((float)(rand() & 0x7FFF) / 16384.0f - 1.0f);
				float diesel = pulse * 0.15f + noise * pulse * 0.08f;

				dieselPhase += dieselInc;
				if (dieselPhase >= 1.0) dieselPhase -= 1.0;

				for (int c = 0; c < channels; c++) {
					float s0 = engineBuf[idx0 * channels + c];
					float s1 = engineBuf[idx1 * channels + c];
					float raw = s0 + frac * (s1 - s0);

					// One-pole low-pass filter
					lpState[c] += alpha * (raw - lpState[c]);

					engineBuffer[i * channels + c] = lpState[c] * 0.75f + diesel * 0.25f;
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

		//Copy into output buffer, with mixing
		for (int i = 0; i < frameCount * p_data->infoWave.channels; i++) {
			out[i] = engineVolume*engineBuffer[i] * 0.33;
			if (waveSoundLoaded) {
				out[i] += waveVolume*waveBuffer[i] * 0.33;
			}
			if (hornSoundLoaded) {
				out[i] += hornVolume*hornBuffer[i] * 0.33;
			}
			if (alarmSoundLoaded) {
				out[i] += alarmVolume*alarmBuffer[i] * 0.33;
			}
		}

		return paContinue;
	}

#endif // WITH_SOUND

};

#endif
