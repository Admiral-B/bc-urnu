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
	static float enginePitchValue;     // 0.5 = half speed, 1.0 = normal, 2.0 = double
	static double engineReadPos;       // fractional read position for pitch-shifted playback

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

		// Pitch-shifted engine playback: read more samples than needed, resample
		float pitch = enginePitchValue;
		if (pitch < 0.25f) pitch = 0.25f;
		if (pitch > 4.0f) pitch = 4.0f;
		// We need frameCount output samples; read ceil(frameCount*pitch)+2 source samples
		sf_count_t srcNeeded = (sf_count_t)(frameCount * pitch) + 2;
		std::vector<float> engineSrc(srcNeeded * channels);
		sf_count_t totalRead = 0;
		while (totalRead < srcNeeded) {
			num_read = sf_read_float(p_data->fileEngine, engineSrc.data() + totalRead * channels,
			                         (srcNeeded - totalRead) * channels);
			if (num_read <= 0) {
				// Loop: seek to start
				sf_count_t seekLocation = sf_seek(p_data->fileEngine, 0, SEEK_SET);
				if (seekLocation == -1) return paComplete;
				num_read = sf_read_float(p_data->fileEngine, engineSrc.data() + totalRead * channels,
				                         (srcNeeded - totalRead) * channels);
				if (num_read <= 0) return paComplete;
			}
			totalRead += num_read / channels;
		}
		// Resample engine source to output frameCount using linear interpolation
		std::vector<float> engineBuffer(frameCount * channels);
		for (unsigned long i = 0; i < frameCount; i++) {
			double srcPos = i * (double)pitch;
			sf_count_t idx = (sf_count_t)srcPos;
			float frac = (float)(srcPos - idx);
			if (idx + 1 >= totalRead) idx = totalRead - 2;
			if (idx < 0) idx = 0;
			for (int c = 0; c < channels; c++) {
				float s0 = engineSrc[idx * channels + c];
				float s1 = engineSrc[(idx + 1) * channels + c];
				engineBuffer[i * channels + c] = s0 + frac * (s1 - s0);
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
