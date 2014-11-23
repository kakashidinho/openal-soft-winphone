#include "alSimple3DSound.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <vector>
#include <string>
#include <string.h>
#include <fstream>
#include <stdint.h>

namespace alSimple3DSound {
	struct AudioInfo
	{
		size_t size;
		int channels;
		int bits;//bits per channel
		size_t samples;//total samples
		size_t sampleRate;
	};


	static ALCcontext *g_context = NULL;
	static ALCdevice* g_device = NULL;
	static ALuint g_source = 0, g_buffer = 0;

	static bool createSound(const char *fileName);
	static void createDevicesList(const char* devicesStr, std::vector<std::string> &devicesListOut);

	bool initSound(const char* soundFile, unsigned int playbackDevice)
	{
		if (g_context)
			return false;
		//get list of devices
		const ALchar* devicesStr = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
		std::vector<std::string> deviceList;
		createDevicesList(devicesStr, deviceList);

		if (playbackDevice >= deviceList.size())
			return false;

		//open device
		g_device = alcOpenDevice(deviceList[playbackDevice].c_str());
		if (g_device == NULL)
			return false;

		//create context
		g_context = alcCreateContext(g_device, NULL);
		if (!g_context || alcMakeContextCurrent(g_context) == ALC_FALSE)
		{
			release();
			return false;
		}

		if (!createSound(soundFile))
		{
			release();
			return false;
		}

		alcMakeContextCurrent(NULL);//stop using openal from this thread until start() function is called

		return true;
	}

	void release()
	{
		if (g_source)
		{
			alDeleteSources(1, &g_source);
			g_source = 0;
		}

		if (g_buffer)
		{
			alDeleteBuffers(1, &g_buffer);
			g_buffer = 0;
		}

		if (g_context)
		{
			alcMakeContextCurrent(NULL);
			alcDestroyContext(g_context);
			g_context = NULL;
		}

		if (g_device)
		{
			alcCloseDevice(g_device);
			g_device = NULL;
		}
	}

	void start(float volume)
	{
		if (g_context != NULL && g_source != NULL)
		{
			alcMakeContextCurrent(g_context);

			alListenerf(AL_GAIN, volume);
			alSourcePlay(g_source);
		}
	}

	void setListenerOrientations(float up[3], float direction[3])
	{
		float orientationArray[] = 
		{
			direction[0], direction[1], direction[2],
			up[0], up[1], up[2]
		};
		alListenerfv(AL_ORIENTATION, orientationArray);
	}

	void setListenerPosition(float position[3])
	{
		if (g_context)
			alListener3f(AL_POSITION, position[0], position[1], position[2]);
	}

	void setSoundPosition(float pos[3])
	{
		if (g_source)
			alSourcefv(g_source, AL_POSITION, pos);
	}

	static bool isWaveFile(std::ifstream &is)
	{
		bool isWave = false;
		char data[4];
		is.read((char*)data, 4);
		if (!strncmp(data, "RIFF" , 4))
		{
			is.seekg(4 , std::ios_base::cur);//chunk size
			is.read((char*)data, 4);
			if (!strncmp(data, "WAVE" , 4))
			{
				is.read((char*)data, 4);
				if (!strncmp(data, "fmt " , 4))
					isWave = true;
			}
		}

		//rewind
		is.seekg(0, std::ios_base::beg);

		return isWave;
	}

	static bool getWaveInfo(std::ifstream &is, AudioInfo &info)
	{
		int32_t chunkSize;
		int32_t data32;//32 bit data
		int16_t data16;//16 bit data
		int16_t blockAlign;
		char data8[4];
		bool dataFound = false;
		std::streampos chunkDataPos;

		is.seekg(4, std::ios_base::beg);//to file's data size
		is.read((char*)&chunkSize, 4);//read file data size
		chunkSize = 4;//actual header size
		chunkDataPos = is.tellg();

		while (!dataFound && is.good())
		{
			//got to next chunk
			std::streampos curPos = is.tellg();
			std::streampos offset = chunkSize - (curPos - chunkDataPos);
			is.seekg(offset, std::ios_base::cur);

			is.read((char*)data8, 4);//read chunk id
			is.read((char*)&chunkSize, 4);//read chunk size
			chunkDataPos = is.tellg();

			if (strncmp(data8, "fmt ", 4) == 0)//format chunk
			{
				is.read((char*)&data16, 2);
				if (data16 != 1)//must be 1 (PCM)
				{
					return false;
				}
				is.read((char*)&data16, 2);//read num channels
				info.channels = data16;
				if (info.channels > 2)//this multi channels type not supported
				{
					return  false;
				}
				is.read((char*)&data32, 4);//read sample rate
				info.sampleRate = data32;
				is.seekg(4, std::ios_base::cur);//byte rate ignored
				is.read((char*)&blockAlign, 2);//block align

				is.read((char*)&data16, 2);//read bits per channel
				info.bits = data16;
			}//if (strncmp(data8, "fmt ", 4) == 0)
			else if (strncmp(data8, "data", 4) == 0)//data chunk
			{
				info.size = chunkSize;

				dataFound = true;//stop when we found data
			}//else if (strncmp(data8, "data", 4) == 0)

		} //while (!dataFound && is.good())

		info.samples = info.size / blockAlign;//number of samples

		return dataFound;
	}

	bool createSound(const char *fileName)
	{
		//read wave data
		std::ifstream is(fileName, std::ios_base::in | std::ios_base::binary);
		if (!is.good() || !isWaveFile(is))
			return false;

		char *buffer;

		//get audio info
		AudioInfo info;
		if (!getWaveInfo(is, info))
			return false;

		//get audio data
		buffer = new (std::nothrow) char[info.size];
		if (!buffer)
			return false;

		is.read(buffer, info.size);

		is.close();

		/*---------create openAL buffer and source-------------*/
		ALenum format = 0;

		switch(info.channels)
		{
			case 1:
				if (info.bits == 8)
					format = AL_FORMAT_MONO8;
				else
					format = AL_FORMAT_MONO16;
				break;
			case 2:
				if (info.bits == 8)
					format = AL_FORMAT_STEREO8;
				else
					format = AL_FORMAT_STEREO16;
				break;
		}

		//al buffer
		alGenBuffers(1, &g_buffer);
		alBufferData(g_buffer, format, buffer, info.size, info.sampleRate);
		delete[] buffer;//delete data
		if (AL_OUT_OF_MEMORY == alGetError())
			return false;

		//al source
		alGenSources(1, &g_source);
		alSourcei (g_source, AL_BUFFER, g_buffer);
		if (AL_OUT_OF_MEMORY == alGetError())
			return false;

		//loop forever
		alSourcei(g_source, AL_LOOPING, AL_TRUE);

		return true;
	}

	void createDevicesList(const char* devicesStr, std::vector<std::string> &devicesListOut)
	{
		const ALCchar *device = devicesStr, *next = devicesStr + 1;
        size_t len = 0;

        while (device && *device != '\0' && next && *next != '\0') {
			devicesListOut.push_back(device);

			len = strlen(device);
			device += (len + 1);
			next += (len + 2);
        }
	}
}