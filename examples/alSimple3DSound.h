#ifndef AL_SIMPLE_3D_SOUND_H
#define AL_SIMPLE_3D_SOUND_H

namespace alSimple3DSound {
	///
	///note that the 3D coordinates system is right-handed
	///

	///
	///return false if already initialized
	///
	bool initSound(const char* soundFile, unsigned int playbackDevice = 0);
	void release();
	void start(float volume);
	void setListenerOrientations(float up[3], float direction[3]);
	void setListenerPosition(float pos[3]);
	void setSoundPosition(float pos[3]);
};

#endif