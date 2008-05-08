// ----------------------------------------------------------------------------
//
//      sound.cxx
//
// Copyright (C) 2006-2007
//              Dave Freese, W1HKJ
//
// Copyright (C) 2007-2008
//              Stelios Bounanos, M0GLD
//
// This file is part of fldigi.
//
// fldigi is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
// ----------------------------------------------------------------------------

#include <config.h>

#include <iostream>
#include <string>

#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <limits.h>

#if USE_OSS
#    include <sys/soundcard.h>
#endif
#include <math.h>

#if HAVE_DLOPEN
#  include <dlfcn.h>
#endif

#include "sound.h"
#include "configuration.h"
#include "status.h"
#include "fileselect.h"

#include "timeops.h"
#include "ringbuffer.h"

// We always read and write 2 channels from/to the audio device.
// * input:  we ignore the right channel of captured samples
// * output: we copy the left channel to the right channel,
//   unless we are in CW or RTTY mode with QSK or PseudoFSK enabled --
//   this data then goes into the right channel.
#define CHANNELS 2
// We never write duplicate/QSK/PseudoFSK data to the sound files
#define SNDFILE_CHANNELS 1

SoundBase::SoundBase()
        : sample_frequency(0),
	  txppm(progdefaults.TX_corr), rxppm(progdefaults.RX_corr),
          tx_src_state(0), tx_src_data(0), rx_src_state(0), rx_src_data(0),
          snd_buffer(0), src_buffer(0),
#if USE_SNDFILE
          ofCapture(0), ifPlayback(0), ofGenerate(0),
#endif
	  capture(false), playback(false), generate(false)
{ }

SoundBase::~SoundBase()
{
	if (snd_buffer) delete [] snd_buffer;
	if (src_buffer) delete [] src_buffer;
	if (tx_src_data) delete tx_src_data;
	if (rx_src_data) delete rx_src_data;
	if (rx_src_state) src_delete (rx_src_state);
	if (tx_src_state) src_delete (tx_src_state);
#if USE_SNDFILE
	if (ofGenerate) sf_close(ofGenerate);
	if (ofCapture) sf_close(ofCapture);
	if (ifPlayback) sf_close(ifPlayback);
#endif
}

#if USE_SNDFILE
void SoundBase::get_file_params(const char* def_fname, const char** fname, int* format)
{
	std::string filters = "Waveform Audio Format\t*.wav\n" "AU\t*.{au,snd}\n";
	if (format_supported(SF_FORMAT_FLAC | SF_FORMAT_PCM_16))
		filters += "Free Lossless Audio Codec\t*.flac";

	if (strstr(def_fname, "playback"))
		*fname = file_select("Audio file", filters.c_str(), def_fname);
	else
		*fname = file_saveas("Audio file", filters.c_str(), def_fname);
	if (!*fname)
		return;

	char* suffix = strrchr(*fname, '.');
	if (suffix && !strcasecmp(suffix, ".flac"))
		*format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
	else if (suffix && !strcasecmp(suffix, ".au"))
		*format = SF_FORMAT_AU | SF_FORMAT_FLOAT | SF_ENDIAN_CPU;
	else
		*format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
}

int SoundBase::Capture(bool val)
{
	if (!val) {
		if (ofCapture) {
			int err;
			if ((err = sf_close(ofCapture)) != 0)
				cerr << "sf_close error: " << sf_error_number(err) << '\n';
			ofCapture = 0;
		}
		capture = false;
		return 1;
	}

	const char* fname;
	int format;
	get_file_params("./capture.wav", &fname, &format);
	if (!fname)
		return 0;

	// frames (ignored), freq, channels, format, sections (ignored), seekable (ignored)
	SF_INFO info = { 0, sample_frequency, SNDFILE_CHANNELS, format, 0, 0 };
	if ((ofCapture = sf_open(fname, SFM_WRITE, &info)) == NULL) {
		cerr << "Could not write " << fname << '\n';
		return 0;
	}
	if (sf_command(ofCapture, SFC_SET_UPDATE_HEADER_AUTO, NULL, SF_TRUE) != SF_TRUE)
		cerr << "ofCapture update header command failed: " << sf_strerror(ofCapture) << '\n';
	tag_file(ofCapture, "Captured audio");

	capture = true;
	return 1;
}

int SoundBase::Playback(bool val)
{
	if (!val) {
		if (ifPlayback) {
			int err;
			if ((err = sf_close(ifPlayback)) != 0)
				cerr << "sf_close error: " << sf_error_number(err) << '\n';
			ifPlayback = 0;
		}
		playback = false;
		return 1;
	}
	const char* fname;
	int format;
	get_file_params("./playback.wav", &fname, &format);
	if (!fname)
		return 0;

	SF_INFO info = { 0, 0, 0, 0, 0, 0 };
	if ((ifPlayback = sf_open(fname, SFM_READ, &info)) == NULL) {
		cerr << "Could not read " << fname << '\n';
		return 0;
	}

	playback = true;
	return 1;
}

int SoundBase::Generate(bool val)
{
	if (!val) {
		if (ofGenerate) {
			int err;
			if ((err = sf_close(ofGenerate)) != 0)
				cerr << "sf_close error: " << sf_error_number(err) << '\n';
			ofGenerate = 0;
		}
		generate = false;
		return 1;
	}

	const char* fname;
	int format;
	get_file_params("./generate.wav", &fname, &format);
	if (!fname)
		return 0;

	// frames (ignored), freq, channels, format, sections (ignored), seekable (ignored)
	SF_INFO info = { 0, sample_frequency, SNDFILE_CHANNELS, format, 0, 0 };
	if ((ofGenerate = sf_open(fname, SFM_WRITE, &info)) == NULL) {
		cerr << "Could not write " << fname << '\n';
		return 0;
	}
	if (sf_command(ofGenerate, SFC_SET_UPDATE_HEADER_AUTO, NULL, SF_TRUE) != SF_TRUE)
		cerr << "ofGenerate update header command failed: " << sf_strerror(ofGenerate) << '\n';
	tag_file(ofGenerate, "Generated audio");

	generate = true;
	return 1;
}

sf_count_t SoundBase::read_file(SNDFILE* file, double* buf, size_t count)
{
	sf_count_t r = sf_readf_double(file, buf, count);

	while (r < (sf_count_t)count) {
		sf_seek(file, 0, SEEK_SET);
		r += sf_readf_double(file, buf + r, count - r);
                if (r == 0)
                        break;
        }

	return r;
}

sf_count_t SoundBase::write_file(SNDFILE* file, double* buf, size_t count)
{
	return sf_writef_double(file, buf, count);
}

bool SoundBase::format_supported(int format)
{

        SF_INFO fmt_test = { 0, sample_frequency, SNDFILE_CHANNELS, format, 0, 0 };
        return sf_format_check(&fmt_test);
}

void SoundBase::tag_file(SNDFILE *sndfile, const char *title)
{
	int err;
	if ((err = sf_set_string(sndfile, SF_STR_TITLE, title)) != 0) {
		cerr << "sf_set_string STR_TITLE: " << sf_error_number(err) << '\n';
		return;
	}

	sf_set_string(sndfile, SF_STR_COPYRIGHT, progdefaults.myName.c_str());
	sf_set_string(sndfile, SF_STR_SOFTWARE, PACKAGE_NAME "-" PACKAGE_VERSION);
	sf_set_string(sndfile, SF_STR_ARTIST, progdefaults.myCall.c_str());

	char s[64];
	snprintf(s, sizeof(s), "%s freq=%s",
		 active_modem->get_mode_name(), inpFreq->value());
	sf_set_string(sndfile, SF_STR_COMMENT, s);

	time_t t = time(0);
	struct tm zt;
	(void)gmtime_r(&t, &zt);
	if (strftime(s, sizeof(s), "%F %Tz", &zt) > 0)
		sf_set_string(sndfile, SF_STR_DATE, s);
}
#endif // USE_SNDFILE


#if USE_OSS
SoundOSS::SoundOSS(const char *dev ) {
	device			= dev;
	cbuff			= 0;
	try {
		Open(O_RDONLY);
		getVersion();
		getCapabilities();
		getFormats();
		Close();
	}
	catch (SndException e) {
		std::cout << e.what()
			 << " <" << device.c_str()
			 << ">" << std::endl;
	}

	int err;
	try {
		snd_buffer	= new float [2*SND_BUF_LEN];
		src_buffer	= new float [2*SND_BUF_LEN];
		cbuff		= new unsigned char [4 * SND_BUF_LEN];
	}
	catch (const std::bad_alloc& e) {
		cerr << "Cannot allocate libsamplerate buffers\n";
		throw;
	}
	for (int i = 0; i < 2*SND_BUF_LEN; i++)
		snd_buffer[i] = src_buffer[i] = 0.0;
	for (int i = 0; i < 4 * SND_BUF_LEN; i++)
		cbuff[i] = 0;

	try {
		tx_src_data = new SRC_DATA;
		rx_src_data = new SRC_DATA;
	}
	catch (const std::bad_alloc& e) {
		cerr << "Cannot create libsamplerate data structures\n";
		throw;
	}

	rx_src_state = src_new(progdefaults.sample_converter, 2, &err);
	if (rx_src_state == 0)
		throw SndException(src_strerror(err));

	tx_src_state = src_new(progdefaults.sample_converter, 2, &err);
	if (tx_src_state == 0)
		throw SndException(src_strerror(err));

	rx_src_data->src_ratio = 1.0/(1.0 + rxppm/1e6);
	src_set_ratio ( rx_src_state, 1.0/(1.0 + rxppm/1e6));

	tx_src_data->src_ratio = 1.0 + txppm/1e6;
	src_set_ratio ( tx_src_state, 1.0 + txppm/1e6);
}

SoundOSS::~SoundOSS()
{
	Close();
	if (cbuff) delete [] cbuff;
}

void SoundOSS::setfragsize()
{
	int sndparam;
// Try to get ~100ms worth of samples per fragment
	sndparam = (int)log2(sample_frequency * 0.1);
// double since we are using 16 bit samples
	sndparam += 1;
// Unlimited amount of buffers for RX, four for TX
	if (mode == O_RDONLY)
		sndparam |= 0x7FFF0000;
	else
		sndparam |= 0x00040000;

	if (ioctl(device_fd, SNDCTL_DSP_SETFRAGMENT, &sndparam) < 0)
		throw SndException(errno);
}

int SoundOSS::Open(int md, int freq)
{
	Close();

	mode = md;
	try {
		device_fd = open(device.c_str(), mode, 0);
		if (device_fd == -1)
			throw SndException(errno);
		Format(AFMT_S16_LE);	// default: 16 bit little endian
//		Channels(1);			//          1 channel
		Channels(2);			//          2 channels
		Frequency(freq);
		setfragsize();
	}
	catch (...) {
		throw;
	}
	return device_fd;
}

void SoundOSS::Close(unsigned dir)
{
	if (device_fd == -1)
		return;
	close(device_fd);
	device_fd = -1;
}

void SoundOSS::getVersion()
{
	version = 0;
#ifndef __FreeBSD__
 	if (ioctl(device_fd, OSS_GETVERSION, &version) == -1) {
 		version = -1;
 		throw SndException("OSS Version");
 	}
#endif
}

void SoundOSS::getCapabilities()
{
	capability_mask = 0;
	if (ioctl(device_fd, SNDCTL_DSP_GETCAPS, &capability_mask) == -1) {
		capability_mask = 0;
		throw SndException("OSS capabilities");
	}
}

void SoundOSS::getFormats()
{
	format_mask = 0;
	if (ioctl(device_fd, SNDCTL_DSP_GETFMTS, &format_mask) == -1) {
		format_mask = 0;
		throw SndException("OSS formats");
	}
}

void SoundOSS::Format(int format)
{
	play_format = format;
	if (ioctl(device_fd, SNDCTL_DSP_SETFMT, &play_format) == -1) {
		device_fd = -1;
		formatok = false;
		throw SndException("Unsupported snd card format");
    }
	formatok = true;
}

void SoundOSS::Channels(int nuchannels)
{
	channels = nuchannels;
	if (ioctl(device_fd, SNDCTL_DSP_CHANNELS, &channels) == -1) {
		device_fd = -1;
		throw "Snd card channel request failed";
	}
}

void SoundOSS::Frequency(int frequency)
{
	sample_frequency = frequency;
	if (ioctl(device_fd, SNDCTL_DSP_SPEED, &sample_frequency) == -1) {
		device_fd = -1;
		throw SndException("Cannot set frequency");
    }
}

int SoundOSS::BufferSize( int seconds )
{
	int bytes_per_channel = 0;
	switch (play_format) {
        case AFMT_MU_LAW:
        case AFMT_A_LAW:
        case AFMT_IMA_ADPCM:
            bytes_per_channel = 0; /* format not supported by this program */
			break;
        case AFMT_S16_BE:
        case AFMT_U16_LE:
        case AFMT_U16_BE:
        case AFMT_MPEG:
        case AFMT_S16_LE:
			bytes_per_channel = 2;
			break;
		case AFMT_U8:
        case AFMT_S8:
            bytes_per_channel = 1;
            break;
	}
  return seconds * sample_frequency * bytes_per_channel * channels;
}

bool SoundOSS::wait_till_finished()
{
	if (ioctl(device_fd, SNDCTL_DSP_POST, 1) == -1 )
		return false;
	if (ioctl(device_fd, SNDCTL_DSP_SYNC, 0) == -1)
		return false; /* format (or ioctl()) not supported by device */
	return true; /* all sound has been played */
}

bool SoundOSS::reset_device()
{
	if (ioctl(device_fd, SNDCTL_DSP_RESET, 0) == -1) {
		device_fd = -1;
		return false; /* format (or ioctl()) not supported by device */
    }
	return 1; /* sounddevice has been reset */
}

size_t SoundOSS::Read(double *buffer, size_t buffersize)
{
	short int *ibuff = (short int *)cbuff;
	int numread;

	numread = read(device_fd, cbuff, buffersize * 4);
	if (numread == -1)
		throw SndException(errno);

	for (size_t i = 0; i < buffersize * 2; i++)
		src_buffer[i] = ibuff[i] / MAXSC;

	for (size_t i = 0; i < buffersize; i++)
		buffer[i] = src_buffer[2*i];

#if USE_SNDFILE
	if (capture)
		write_file(ofCapture, buffer, buffersize);
	if (playback) {
		read_file(ifPlayback, buffer, buffersize);
		if (progdefaults.EnableMixer) {
			double vol = progStatus.RcvMixer;
			for (size_t i = 0; i < buffersize; i++)
				buffer[i] *= vol;
		}
		return buffersize;
	}
#endif

	if (rxppm != progdefaults.RX_corr) {
		rxppm = progdefaults.RX_corr;
		rx_src_data->src_ratio = 1.0/(1.0 + rxppm/1e6);
		src_set_ratio ( rx_src_state, 1.0/(1.0 + rxppm/1e6));
	}

	if (rxppm == 0)
		return buffersize;

// process using samplerate library

	rx_src_data->data_in = src_buffer;
	rx_src_data->input_frames = buffersize;
	rx_src_data->data_out = snd_buffer;
	rx_src_data->output_frames = SND_BUF_LEN;
	rx_src_data->end_of_input = 0;

	if ((numread = src_process(rx_src_state, rx_src_data)) != 0)
		throw SndException(src_strerror(numread));

	numread = rx_src_data->output_frames_gen;

	for (int i = 0; i < numread; i++)
		buffer[i] = snd_buffer[2*i];

	return numread;

}

size_t SoundOSS::Write(double *buf, size_t count)
{
	int retval;
	short int *wbuff;
	unsigned char *p;

#if USE_SNDFILE
	if (generate)
		write_file(ofGenerate, buf, count);
#endif

	if (txppm != progdefaults.TX_corr) {
		txppm = progdefaults.TX_corr;
		tx_src_data->src_ratio = 1.0 + txppm/1e6;
		src_set_ratio ( tx_src_state, 1.0 + txppm/1e6);
	}

	if (txppm == 0) {
		wbuff = new short int[2*count];
		p = (unsigned char *)wbuff;
		for (size_t i = 0; i < count; i++) {
			wbuff[2*i] = wbuff[2*i+1] = (short int)(buf[i] * maxsc);
		}
		count *= sizeof(short int);
		retval = write(device_fd, p, 2*count);
		delete [] wbuff;
		if (retval == -1)
			throw SndException(errno);
	}
	else {
		float *inbuf;
		inbuf = new float[2*count];
		size_t bufsize;
		for (size_t i = 0; i < count; i++)
			inbuf[2*i] = inbuf[2*i+1] = buf[i];
		tx_src_data->data_in = inbuf;
		tx_src_data->input_frames = count;
		tx_src_data->data_out = src_buffer;
		tx_src_data->output_frames = SND_BUF_LEN;
		tx_src_data->end_of_input = 0;

		retval = src_process(tx_src_state, tx_src_data);
		delete [] inbuf;
		if (retval != 0)
			throw SndException(src_strerror(retval));

		bufsize = tx_src_data->output_frames_gen;
		wbuff = new short int[2*bufsize];
		p = (unsigned char *)wbuff;

		for (size_t i = 0; i < 2*bufsize; i++)
			wbuff[i] = (short int)(src_buffer[i] * maxsc);
		int num2write = bufsize * 2 * sizeof(short int);

		retval = write(device_fd, p, num2write);
		delete [] wbuff;
		if (retval != num2write)
			throw SndException(errno);
		retval = count;
	}

	return retval;
}

size_t SoundOSS::Write_stereo(double *bufleft, double *bufright, size_t count)
{
	int retval;
	short int *wbuff;
	unsigned char *p;

#if USE_SNDFILE
	if (generate)
		write_file(ofGenerate, bufleft, count);
#endif

	if (txppm != progdefaults.TX_corr) {
		txppm = progdefaults.TX_corr;
		tx_src_data->src_ratio = 1.0 + txppm/1e6;
		src_set_ratio ( tx_src_state, 1.0 + txppm/1e6);
	}

	if (txppm == 0) {
		wbuff = new short int[2*count];
		p = (unsigned char *)wbuff;
		for (size_t i = 0; i < count; i++) {
			wbuff[2*i] = (short int)(bufleft[i] * maxsc);
			wbuff[2*i + 1] = (short int)(bufright[i] * maxsc);
		}
		count *= sizeof(short int);
		retval = write(device_fd, p, 2*count);
		delete [] wbuff;
		if (retval == -1)
			throw SndException(errno);
	}
	else {
		float *inbuf;
		inbuf = new float[2*count];
		size_t bufsize;
		for (size_t i = 0; i < count; i++) {
			inbuf[2*i] = bufleft[i];
			inbuf[2*i+1] = bufright[i];
		}
		tx_src_data->data_in = inbuf;
		tx_src_data->input_frames = count;
		tx_src_data->data_out = src_buffer;
		tx_src_data->output_frames = SND_BUF_LEN;
		tx_src_data->end_of_input = 0;

		retval = src_process(tx_src_state, tx_src_data);
		delete [] inbuf;
		if (retval != 0)
			throw SndException(src_strerror(retval));

		bufsize = tx_src_data->output_frames_gen;
		wbuff = new short int[2*bufsize];
		p = (unsigned char *)wbuff;

		for (size_t i = 0; i < 2*bufsize; i++)
			wbuff[i] = (short int)(src_buffer[i] * maxsc);

		int num2write = bufsize * 2 * sizeof(short int);
		retval = write(device_fd, p, num2write);
		delete [] wbuff;
		if (retval != num2write)
			throw SndException(errno);
		retval = count;
	}

	return retval;
}
#endif // USE_OSS


#if USE_PORTAUDIO

bool SoundPort::pa_init = false;
std::vector<const PaDeviceInfo*> SoundPort::devs;
void SoundPort::initialize(void)
{
        if (pa_init)
                return;

        init_hostapi_ext();

        int err;

        if ((err = Pa_Initialize()) != paNoError)
                throw SndPortException(err);
        pa_init = true;

        PaDeviceIndex ndev;
        if ((ndev = Pa_GetDeviceCount()) < 0)
                throw SndPortException(ndev);
        if (ndev == 0)
                throw SndPortException("No available audio devices");

        devs.reserve(ndev);
        for (PaDeviceIndex i = 0; i < ndev; i++)
                devs.push_back(Pa_GetDeviceInfo(i));

}
void SoundPort::terminate(void)
{
        if (!pa_init)
                return;
        static_cast<void>(Pa_Terminate());
        pa_init = false;
        devs.clear();
}
const std::vector<const PaDeviceInfo*>& SoundPort::devices(void)
{
        return devs;
}

SoundPort::SoundPort(const char *in_dev, const char *out_dev)
        : req_sample_rate(0), fbuf(0)
{
        sd[0].device = in_dev;
        sd[1].device = out_dev;
        sd[0].stream = sd[1].stream = 0;
        sd[0].frames_per_buffer = sd[1].frames_per_buffer = paFramesPerBufferUnspecified;
        sd[0].dev_sample_rate = sd[1].dev_sample_rate = 0;
        sd[0].state = sd[1].state = spa_continue;
        sd[0].rb = sd[1].rb = 0;

	sem_t** sems[] = { &sd[0].rwsem, &sd[0].csem, &sd[1].rwsem, &sd[1].csem };
        for (size_t i = 0; i < sizeof(sems)/sizeof(*sems); i++) {
		*sems[i] = new sem_t;
		if (sem_init(*sems[i], 0, 0) == -1)
			throw SndException(errno);
	}

        try {
                rx_src_data = new SRC_DATA;
                tx_src_data = new SRC_DATA;
        }
        catch (const std::bad_alloc& e) {
                cerr << "Cannot create libsamplerate data structures\n";
                throw;
        }

        try {
                snd_buffer = new float[2 * SND_BUF_LEN];
                src_buffer = new float[2 * SND_BUF_LEN];
                fbuf = new float[2 * SND_BUF_LEN];
        }
        catch (const std::bad_alloc& e) {
                cerr << "Cannot allocate libsamplerate buffers\n";
                throw;
        }

        memset(snd_buffer, 0, CHANNELS * SND_BUF_LEN);
        memset(src_buffer, 0, CHANNELS * SND_BUF_LEN);
        memset(fbuf, 0, CHANNELS * SND_BUF_LEN);
}

SoundPort::~SoundPort()
{
        Close();

        sem_t* sems[] = { sd[0].rwsem, sd[0].csem, sd[1].rwsem, sd[1].csem };
        for (size_t i = 0; i < sizeof(sems)/sizeof(*sems); i++) {
                if (sem_destroy(sems[i]) == -1)
                        perror("sem_close");
		delete sems[i];
	}

        delete [] fbuf;
}

int SoundPort::Open(int mode, int freq)
{
        int old_sample_rate = (int)req_sample_rate;
        req_sample_rate = sample_frequency = freq;

	// do we need to (re)initialise the streams?
	int sr[2] = { progdefaults.in_sample_rate, progdefaults.out_sample_rate };
	int m[2] = { 1 << O_RDONLY, 1 << O_WRONLY };
	for (size_t i = 0; i < 2; i++) {
		if ( !(stream_active(i) && (Pa_GetHostApiInfo((*sd[i].idev)->hostApi)->type == paJACK ||
					    old_sample_rate == freq ||
					    sr[i] != SAMPLE_RATE_AUTO)) ) {
			Close(i);
			init_stream(i);
			src_data_reset(m[i]);

                        // reset the semaphores
                        sem_t* sems[] = { sd[i].rwsem, sd[i].csem };
                        for (size_t j = 0; j < sizeof(sems)/sizeof(*sems); j++) {
                                while (sem_trywait(sems[i]) == 0);
                                if (errno && errno != EAGAIN)
                                        throw SndException(errno);
                        }

			start_stream(i);
		}
		else if (old_sample_rate != freq)
			src_data_reset(m[i]);
	}

	return 0;
}

static int sem_timedwaitr(sem_t* sem, double rel_timeout)
{
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t = t + rel_timeout;

        return sem_timedwait(sem, &t);
}


void SoundPort::Close(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

        for (unsigned i = start; i <= end; i++) {
                if (!stream_active(i))
                        continue;
                sd[i].state = spa_complete;
                // first wait for buffers to be drained and for the
                // stop callback to signal us that the stream has
                // been stopped
                if (sem_timedwaitr(sd[i].csem, 2) == -1 && errno == ETIMEDOUT)
                        cerr << __func__ << ": stream " << i << " wedged\n";
                sd[i].state = spa_continue;

                int err;
                if ((err = Pa_CloseStream(sd[i].stream)) != paNoError)
                        pa_perror(err, "Pa_CloseStream");

                sd[i].stream = 0;
        }
}

void SoundPort::Abort(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

        int err;
        for (unsigned i = start; i <= end; i++) {
                if (!stream_active(i))
                        continue;
                if ((err = Pa_AbortStream(sd[i].stream)) != paNoError)
                        pa_perror(err, "Pa_AbortStream");
                sd[i].stream = 0;
        }
}


#define WAIT_FOR_COND(cond, s, t)                                       \
        do {                                                            \
                while (!(cond)) {                                       \
                        if (sem_timedwaitr(s, t) == -1) {               \
                                if (errno == ETIMEDOUT) {               \
                                        timeout = true;                 \
                                        break;                          \
                                }                                       \
                                perror("sem_timedwait");                \
                                throw SndException(errno);              \
                        }                                               \
                }                                                       \
        } while (0)


size_t SoundPort::Read(double *buf, size_t count)
{
#if USE_SNDFILE
        if (playback) {
                read_file(ifPlayback, buf, count);
                if (progdefaults.EnableMixer) {
                        double vol = valRcvMixer->value();
                        for (size_t i = 0; i < count; i++)
                                buf[i] *= vol;
                }
                if (!capture) {
			usleep((useconds_t)ceil((1e6 * count) / req_sample_rate));
			return count;
		}
        }
#endif

        size_t maxframes = (size_t)floor((sd[0].rb->length() / CHANNELS) * rx_src_data->src_ratio);
        if (unlikely(count > maxframes)) {
                size_t n = 0;
                while (count > maxframes) {
                        n += Read(buf, maxframes);
                        buf += CHANNELS * maxframes;
                        count -= maxframes;
                }
                if (count > 0)
                        n += Read(buf, count);
                return n;
        }

        // new sample count, taking into account the samplerate ratio
        size_t ncount = (size_t)floor(count / rx_src_data->src_ratio);

        // wait for data
        bool timeout = false;
        WAIT_FOR_COND( (sd[0].rb->read_space() >= CHANNELS * ncount), sd[0].rwsem,
                       (MAX(1.0, 2 * CHANNELS * ncount / sd[0].dev_sample_rate)) );
        if (timeout)
                throw SndException(ETIMEDOUT);

        // copy to fbuf if the data is not contiguous inside the ringbuffer
        float* rbuf = 0;
        bool rbadv = true;
        ringbuffer<float>::vector_type vec[2];
        sd[0].rb->get_rv(vec);
        if (likely(vec[0].len >= CHANNELS * ncount))
                rbuf = vec[0].buf;
        else { // copy
                sd[0].rb->read(fbuf, CHANNELS * ncount);
                rbuf = fbuf;
                rbadv = false;
        }

        // resample
        if (req_sample_rate != sd[0].dev_sample_rate || progdefaults.RX_corr != 0) {
                resample(1 << O_RDONLY, rbuf, snd_buffer, ncount, count);
                rbuf = rx_src_data->data_out;
                count = rx_src_data->output_frames_gen;
        }
        // if we did a no-copy read we must advance the read pointer
        if (rbadv)
                sd[0].rb->read_advance(CHANNELS * ncount);

        // deinterleave first channel into buf
        for (size_t i = 0; i < count; i++)
                buf[i] = rbuf[CHANNELS * i];

#if USE_SNDFILE
	if (capture)
		write_file(ofCapture, buf, count);
#endif

        return count;
}

size_t SoundPort::Write(double *buf, size_t count)
{
#if USE_SNDFILE
	if (generate)
                write_file(ofGenerate, buf, count);
#endif

        // copy input to both channels
        for (size_t i = 0; i < count; i++)
                fbuf[CHANNELS * i] = fbuf[CHANNELS * i + 1] = buf[i];

        return resample_write(fbuf, count);
}

size_t SoundPort::Write_stereo(double *bufleft, double *bufright, size_t count)
{
#if USE_SNDFILE
	if (generate)
                write_file(ofCapture, bufleft, count);
#endif

        // interleave into fbuf
        for (size_t i = 0; i < count; i++) {
                fbuf[CHANNELS * i] = bufleft[i];
                fbuf[CHANNELS * i + 1] = bufright[i];
        }

        return resample_write(fbuf, count);
}


size_t SoundPort::resample_write(float* buf, size_t count)
{
        size_t maxframes = (size_t)floor((sd[1].rb->length() / CHANNELS) / tx_src_data->src_ratio);
        maxframes /= 2;

        if (unlikely(count > maxframes)) {
                size_t n = 0;
                while (count > maxframes) {
                        n += resample_write(buf, maxframes);
                        buf += CHANNELS * maxframes;
                        count -= maxframes;
                }
                if (count > 0)
                        n += resample_write(buf, count);
                return n;
        }

        assert(count * CHANNELS * tx_src_data->src_ratio <= sd[1].rb->length());

        ringbuffer<float>::vector_type vec[2];
        sd[1].rb->get_wv(vec);
        float* wbuf = buf;
        if (req_sample_rate != sd[1].dev_sample_rate || progdefaults.TX_corr != 0) {
                if (vec[0].len >= CHANNELS * (size_t)ceil(count * tx_src_data->src_ratio))
                        wbuf = vec[0].buf; // direct write in the rb
                else
                        wbuf = src_buffer;
                resample(1 << O_WRONLY, buf, wbuf, count);
                count = tx_src_data->output_frames_gen;
                if (wbuf == vec[0].buf) { // advance write pointer and return
                        sd[1].rb->write_advance(CHANNELS * count);
                        sem_trywait(sd[1].rwsem);
                        return count;
                }
        }

        // if we didn't do a direct resample into the rb, or didn't resample at all,
        // we must now copy buf into the ringbuffer, possibly waiting for space first
        bool timeout = false;
        WAIT_FOR_COND( (sd[1].rb->write_space() >= CHANNELS * count), sd[1].rwsem,
                       (MAX(1.0, 2 * CHANNELS * count / sd[1].dev_sample_rate)) );
        if (timeout)
                throw SndException(ETIMEDOUT);
        sd[1].rb->write(wbuf, CHANNELS * count);

        return count;
}

void SoundPort::flush(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

        for (unsigned i = start; i <= end; i++) {
                if (!stream_active(i))
                        continue;
                sd[i].state = spa_drain;
                if (sem_timedwaitr(sd[i].csem, 2) == -1 && errno == ETIMEDOUT)
                        cerr << "timeout while flushing stream " << i << endl;
                sd[i].state = spa_continue;
        }
}

void SoundPort::src_data_reset(int mode)
{
        size_t rbsize;

        int err;
        if (mode & 1 << O_RDONLY) {
                if (rx_src_state)
                        src_delete(rx_src_state);
                rx_src_state = src_new(progdefaults.sample_converter, CHANNELS, &err);
                if (!rx_src_state)
                        throw SndException(src_strerror(err));
                rx_src_data->src_ratio = req_sample_rate / (sd[0].dev_sample_rate * (1.0 + rxppm / 1e6));

                rbsize = ceil2((unsigned)(2 * CHANNELS * SCBLOCKSIZE *
                                          MAX(req_sample_rate, sd[0].dev_sample_rate) /
                                          MIN(req_sample_rate, sd[0].dev_sample_rate)));
                rbsize = 2 * MAX(rbsize, 4096);
#ifndef NDEBUG
                cerr << "input rbsize=" << rbsize << endl;
#endif
                if (!sd[0].rb || sd[0].rb->length() != rbsize) {
                        delete sd[0].rb;
                        sd[0].rb = new ringbuffer<float>(rbsize);
                }
        }
        if (mode & 1 << O_WRONLY) {
                if (tx_src_state)
                        src_delete(tx_src_state);
                tx_src_state = src_new(progdefaults.sample_converter, CHANNELS, &err);
                if (!tx_src_state)
                        throw SndException(src_strerror(err));
                tx_src_data->src_ratio = sd[1].dev_sample_rate * (1.0 + txppm / 1e6) / req_sample_rate;

                rbsize = ceil2((unsigned)(CHANNELS * SCBLOCKSIZE *
                                          MAX(req_sample_rate, sd[1].dev_sample_rate) /
                                          MIN(req_sample_rate, sd[1].dev_sample_rate)));
                if (req_sample_rate > 8000)
                        rbsize *= 2;
                rbsize = MAX(rbsize, 2048);
#ifndef NDEBUG
                cerr << "output rbsize=" << rbsize << endl;
#endif
                if (!sd[1].rb || sd[1].rb->length() != rbsize) {
                        delete sd[1].rb;
                        sd[1].rb = new ringbuffer<float>(rbsize);
                }
        }
}

void SoundPort::resample(int mode, float* inbuf, float* outbuf, size_t count, size_t max)
{
        int r;

        if (mode & 1 << O_RDONLY) {
                if (rxppm != progdefaults.RX_corr) {
                        rxppm = progdefaults.RX_corr;
                        rx_src_data->src_ratio = req_sample_rate / (sd[0].dev_sample_rate * (1.0 + rxppm / 1e6));
                        src_set_ratio(rx_src_state, rx_src_data->src_ratio);
                }

                rx_src_data->data_in = inbuf;
                rx_src_data->input_frames = count;
                rx_src_data->data_out = outbuf;
                rx_src_data->output_frames = max ? max : SND_BUF_LEN;
                rx_src_data->end_of_input = 0;

                if ((r = src_process(rx_src_state, rx_src_data)) != 0)
			throw SndException(src_strerror(r));
        }
        else if (mode & 1 << O_WRONLY) {
                if (txppm != progdefaults.TX_corr) {
                        txppm = progdefaults.TX_corr;
                        tx_src_data->src_ratio = sd[1].dev_sample_rate * (1.0 + txppm / 1e6) / req_sample_rate;
                        src_set_ratio(tx_src_state, tx_src_data->src_ratio);
                }

                tx_src_data->data_in = inbuf;
                tx_src_data->input_frames = count;
                tx_src_data->data_out = outbuf;
                tx_src_data->output_frames = max ? max : SND_BUF_LEN;
                tx_src_data->end_of_input = 0;

                if ((r = src_process(tx_src_state, tx_src_data)) != 0)
			throw SndException(src_strerror(r));
        }
}

void SoundPort::init_stream(unsigned dir)
{
	const char* dir_str[2] = { "input", "output" };
	PaDeviceIndex conf_idx[2] = { progdefaults.PortInIndex, progdefaults.PortOutIndex };
	PaDeviceIndex idx = paNoDevice;

#ifndef NDEBUG
        cerr << "PA_debug: looking for \"" << sd[dir].device << "\"\n";
#endif
        for (sd[dir].idev = devs.begin(); sd[dir].idev != devs.end(); ++sd[dir].idev) {
                if (sd[dir].device == (*sd[dir].idev)->name) {
			idx = sd[dir].idev - devs.begin(); // save this device index
			if (idx == conf_idx[dir]) // found it
				break;
		}
	}
        if (idx == paNoDevice) { // no match
                cerr << "PA_debug: could not find \"" << sd[dir].device
		     << "\", using default " << dir_str[dir] << " device\n";
		PaDeviceIndex def = (dir == 0 ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice());
		if (def == paNoDevice)
			throw SndPortException(paDeviceUnavailable);
                sd[dir].idev = devs.begin() + def;
		idx = def;
        }
	else if (sd[dir].idev == devs.end()) // if we only found a near-match point the idev iterator to it
		sd[dir].idev = devs.begin() + idx;

#ifndef NDEBUG
        cerr << "PA_debug: using " << dir_str[dir] << " device:"
             << "\n index: " << idx
             << "\n name: " << (*sd[dir].idev)->name
             << "\n hostAPI: " << Pa_GetHostApiInfo((*sd[dir].idev)->hostApi)->name
             << "\n maxInputChannels: " << (*sd[dir].idev)->maxInputChannels
             << "\n maxOutputChannels: " << (*sd[dir].idev)->maxOutputChannels
             << "\n defaultLowInputLatency: " << (*sd[dir].idev)->defaultLowInputLatency
             << "\n defaultHighInputLatency: " << (*sd[dir].idev)->defaultHighInputLatency
             << "\n defaultLowOutputLatency: " << (*sd[dir].idev)->defaultLowOutputLatency
             << "\n defaultHighOutputLatency: " << (*sd[dir].idev)->defaultHighOutputLatency
             << "\n defaultSampleRate: " << (*sd[dir].idev)->defaultSampleRate
             << boolalpha
             << "\n isInputOnlyDevice: " << ((*sd[dir].idev)->maxOutputChannels == 0)
             << "\n isOutputOnlyDevice: " << ((*sd[dir].idev)->maxInputChannels == 0)
             << "\n isFullDuplexDevice: " << full_duplex_device(*sd[dir].idev)
             << "\n isSystemDefaultInputDevice: " << (idx == Pa_GetDefaultInputDevice())
             << "\n isSystemDefaultOutputDevice: " << (idx == Pa_GetDefaultOutputDevice())
             << "\n isHostApiDefaultInputDevice: " << (idx == Pa_GetHostApiInfo((*sd[dir].idev)->hostApi)->defaultInputDevice)
             << "\n isHostApiDefaultOutputDevice: " << (idx == Pa_GetHostApiInfo((*sd[dir].idev)->hostApi)->defaultOutputDevice)
             << "\n";
#endif

	if ((dir == 0 && (*sd[dir].idev)->maxInputChannels == 0) ||
	    (dir == 1 && (*sd[dir].idev)->maxOutputChannels == 0))
		throw SndException(EBUSY);

	if (dir == 0) {
		sd[0].params.device = idx;
		sd[0].params.channelCount = CHANNELS;
		sd[0].params.sampleFormat = paFloat32;
		sd[0].params.suggestedLatency = (*sd[dir].idev)->defaultHighInputLatency;
		sd[0].params.hostApiSpecificStreamInfo = NULL;
	}
	else {
		sd[1].params.device = idx;
		sd[1].params.channelCount = CHANNELS;
		sd[1].params.sampleFormat = paFloat32;
                if (Pa_GetHostApiInfo((*sd[dir].idev)->hostApi)->type == paMME)
                        sd[1].params.suggestedLatency = (*sd[dir].idev)->defaultLowOutputLatency;
                else
                        sd[1].params.suggestedLatency = (*sd[dir].idev)->defaultHighOutputLatency;
		sd[1].params.hostApiSpecificStreamInfo = NULL;
	}

        sd[dir].dev_sample_rate = find_srate(dir);
#ifndef NDEBUG
        if (sd[dir].dev_sample_rate != req_sample_rate)
                cerr << "PA_debug: " << dir_str[dir] << ": resampling "
		     << sd[dir].dev_sample_rate << " <-> " << req_sample_rate << "\n\n";
#endif

        if (progdefaults.PortFramesPerBuffer > 0)
                sd[dir].frames_per_buffer = progdefaults.PortFramesPerBuffer;
}

void SoundPort::start_stream(unsigned dir)
{
        int err;

        PaStreamParameters* sp[2];
        sp[dir] = &sd[dir].params;
        sp[!dir] = NULL;

        err = Pa_OpenStream(&sd[dir].stream, sp[0], sp[1],
                            sd[dir].dev_sample_rate, sd[dir].frames_per_buffer,
                            paNoFlag,
                            stream_process, &sd[dir]);
	if (err != paNoError)
		throw SndPortException(err);

        if ((err = Pa_SetStreamFinishedCallback(sd[dir].stream, stream_stopped)) != paNoError)
                throw SndPortException(err);

	if ((err = Pa_StartStream(sd[dir].stream)) != paNoError) {
		Close();
		throw SndPortException(err);
	}
}


int SoundPort::stream_process(const void* in, void* out, unsigned long nframes,
                             const PaStreamCallbackTimeInfo *time_info,
                             PaStreamCallbackFlags flags, void* data)
{
        struct stream_data* sd = reinterpret_cast<struct stream_data*>(data);

#ifndef NDEBUG
        struct {
                PaStreamCallbackFlags f;
                const char* s;
        } fa[] = { { paInputUnderflow, "Input underflow" },
                   { paInputOverflow,  "Input overflow" },
                   { paOutputUnderflow, "Output underflow" },
                   { paOutputOverflow, "Output overflow" }
        };
        for (size_t i = 0; i < sizeof(fa)/sizeof(*fa); i++)
                if (flags & fa[i].f)
                        cerr << "stream_process: " << fa[i].s << '\n';
#endif

        // input
        if (in && sd->state == spa_continue) {
                sd->rb->write(reinterpret_cast<const float*>(in), CHANNELS * nframes);
                sem_post(sd->rwsem);
        }
        if (!out) {
                if (sd->state == spa_drain) {
                        sem_post(sd->csem);
                        return paContinue;
                }
                return sd->state;
        }

        // output
        if (sd->state == spa_abort)
                return paAbort;
        size_t s;
        if ((s = sd->rb->read(reinterpret_cast<float*>(out), CHANNELS * nframes)) < CHANNELS * nframes)
                memset(reinterpret_cast<float*>(out) + s,  0,
                       (CHANNELS * nframes - s) * sizeof(float)); // fill rest with zeroes
        if (s)
                sem_post(sd->rwsem);

        // state is continue, or we still have data to send to PortAudio
        if (sd->state == spa_continue || sd->rb->read_space() > 0)
                return paContinue;

        // if we get here, state is not continue and the buffer is empty

        if (sd->state == spa_drain) { // resume
                sd->state = spa_continue;
                sem_post(sd->csem);
                return paContinue;
        }

        // complete or abort; stream_stopped() will post sd->csem
        return sd->state;
}

void SoundPort::stream_stopped(void* data)
{
        struct stream_data* sd = reinterpret_cast<struct stream_data*>(data);

        if (sd->rb)
                sd->rb->reset();
        sem_post(sd->csem);
}


bool SoundPort::stream_active(unsigned dir)
{
        if (!sd[dir].stream)
                return false;

        int err;
        if ((err = Pa_IsStreamActive(sd[dir].stream)) < 0)
                throw SndPortException(err);
        return err == 1;
}

bool SoundPort::full_duplex_device(const PaDeviceInfo* dev)
{
        return dev->maxInputChannels > 0 && dev->maxOutputChannels > 0;
}

bool SoundPort::must_close(void)
{
        return false;

	if (!stream_active(1))
		return false;
	const PaHostApiInfo* api = Pa_GetHostApiInfo((*sd[1].idev)->hostApi);
	return api && (api->type == paOSS || api->type == paMME);
}

// Determine the sample rate that we will use. We try the modem's rate
// first and fall back to the device's default rate. If there is a user
// setting we just return that without making any checks.
double SoundPort::find_srate(unsigned dir)
{
	int sr = (dir == 0 ? progdefaults.in_sample_rate : progdefaults.out_sample_rate);
        switch (sr) {
        case SAMPLE_RATE_UNSET: case SAMPLE_RATE_AUTO:
                break;
        case SAMPLE_RATE_NATIVE:
                return (*sd[dir].idev)->defaultSampleRate;
        default:
                return sr;
        }

        double srates[] = { req_sample_rate, (*sd[dir].idev)->defaultSampleRate };
        int err;
        for (size_t i = 0; i < sizeof(srates)/sizeof(srates[0]); i++) {
#ifndef NDEBUG
                cerr << "PA_debug: trying " << srates[i] << " Hz" << endl;
#endif
                if ((err = Pa_IsFormatSupported((dir == 0 ? &sd[0].params : NULL),
						(dir == 1 ? &sd[1].params : NULL),
						srates[i])) == paFormatIsSupported)
                        return srates[i];
#ifndef NDEBUG
                else
                        pa_perror(err, "Pa_IsFormatSupported");
#endif
        }

        throw SndPortException(err);
}

void SoundPort::pa_perror(int err, const char* str)
{
        if (str)
                cerr << str << ": " << Pa_GetErrorText(err) << '\n';

        if (err == paUnanticipatedHostError) {
                const PaHostErrorInfo* hosterr = Pa_GetLastHostErrorInfo();
                PaHostApiIndex i = Pa_HostApiTypeIdToHostApiIndex(hosterr->hostApiType);

                if (i < 0) { // PA failed without setting its "last host error" info. Sigh...
                        cerr << "Host API error info not available\n";
                        if ( ((sd[0].stream && Pa_GetHostApiInfo((*sd[0].idev)->hostApi)->type == paOSS) ||
			      (sd[1].stream && Pa_GetHostApiInfo((*sd[1].idev)->hostApi)->type == paOSS)) &&
			     errno )
                                cerr << "Possible OSS error " << errno << ": "
                                     << strerror(errno) << '\n';
                }
                else
                        cerr << Pa_GetHostApiInfo(i)->name << " error "
                             << hosterr->errorCode << ": " << hosterr->errorText << '\n';
        }
}

void SoundPort::init_hostapi_ext(void)
{
#if HAVE_DLOPEN
	void* handle = dlopen(NULL, RTLD_LAZY);
	if (!handle)
		return;

	PaError (*set_jack_client_name)(const char*);
	char* err = dlerror();
	set_jack_client_name = (PaError (*)(const char*))dlsym(handle, "PaJack_SetClientName");
	if (!(err = dlerror()))
		set_jack_client_name(PACKAGE_TARNAME);
#endif
}

#endif // USE_PORTAUDIO


#if USE_PULSEAUDIO

SoundPulse::SoundPulse(const char *dev)
	: fbuf(0)
{
	stream[0] = stream[1] = 0;

        try {
                rx_src_data = new SRC_DATA;
                tx_src_data = new SRC_DATA;
        }
        catch (const std::bad_alloc& e) {
                cerr << "Cannot create libsamplerate data structures\n";
                throw;
        }

        try {
                snd_buffer = new float[2 * SND_BUF_LEN];
                src_buffer = new float[2 * SND_BUF_LEN];
                fbuf = new float[2 * SND_BUF_LEN];
        }
        catch (const std::bad_alloc& e) {
                cerr << "Cannot allocate libsamplerate buffers\n";
                throw;
        }
}

SoundPulse::~SoundPulse()
{
	Close();
	delete [] fbuf;
}

int SoundPulse::Open(int mode, int freq)
{
	int old_sample_rate = sample_frequency;

	dev_sample_rate[0] = (progdefaults.in_sample_rate > 1 ?
			      progdefaults.in_sample_rate : 48000);
	dev_sample_rate[1] = (progdefaults.out_sample_rate > 1 ?
			      progdefaults.out_sample_rate : 48000);

	sample_frequency = freq;
	if (stream[0] && stream[1]) {
		if (sample_frequency != old_sample_rate) {
			src_data_reset(1 << O_RDONLY | 1 << O_WRONLY);
			return 0;
		}
	}
        else
		Close();

	const char* server = (progdefaults.PulseServer.length() ?
			      progdefaults.PulseServer.c_str() : NULL);
	char sname[32];
	int err;

	stream_params.format = PA_SAMPLE_FLOAT32LE;
	stream_params.channels = 2;

	stream_params.rate = dev_sample_rate[0];
	snprintf(sname, sizeof(sname), "capture (%u)", getpid());
	if (!stream[0]) {
		stream[0] = pa_simple_new(server, PACKAGE_NAME, PA_STREAM_RECORD, NULL,
					  sname, &stream_params, NULL, NULL, &err);
		if (!stream[0])
			throw SndPulseException(err);
	}

	stream_params.rate = dev_sample_rate[1];
	snprintf(sname, sizeof(sname), "playback (%u)", getpid());
	if (!stream[1]) {
		stream[1] = pa_simple_new(server, PACKAGE_NAME, PA_STREAM_PLAYBACK, NULL,
					   sname, &stream_params, NULL, NULL, &err);
		if (!stream[1])
			throw SndPulseException(err);
	}

	src_data_reset(1 << O_RDONLY | 1 << O_WRONLY);

	return 0;
}

void SoundPulse::Close(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

        for (unsigned i = start; i <= end; i++) {
                if (stream[i]) {
                        flush(i);
                        Abort(i);
                }
	}
}

void SoundPulse::Abort(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

        for (unsigned i = start; i <= end; i++) {
                if (stream[i]) {
                        pa_simple_free(stream[i]);
                        stream[i] = 0;
                }
        }

}

void SoundPulse::flush(unsigned dir)
{
        unsigned start, end;
        if (dir == UINT_MAX) {
                start = 0;
                end = 1;
        }
        else
                start = end = dir;

	int err = PA_OK;
        for (unsigned i = start; i <= end; i++) {
                if (!stream[i])
                        continue;
                pa_simple_drain(stream[i], &err);
		if (err != PA_OK)
			cerr << pa_strerror(err) << '\n';
        }
}

size_t SoundPulse::Write(double* buf, size_t count)
{
#if USE_SNDFILE
	if (generate)
		write_file(ofGenerate, buf, count);
#endif

        for (size_t i = 0; i < count; i++)
                fbuf[2*i] = fbuf[2*i + 1] = buf[i];

        float *wbuf = fbuf;
        if (sample_frequency != dev_sample_rate[1] || progdefaults.TX_corr != 0) {
                resample(1 << O_WRONLY, wbuf, count);
                wbuf = tx_src_data->data_out;
                count = tx_src_data->output_frames_gen;
        }

	int err;
	if (pa_simple_write(stream[1], wbuf, count * sizeof(double), &err) == -1)
		throw SndPulseException(err);

	return count;
}

size_t SoundPulse::Write_stereo(double* bufleft, double* bufright, size_t count)
{
#if USE_SNDFILE
	if (generate)
		write_file(ofGenerate, bufleft, count);
#endif

	for (size_t i = 0; i < count; i++) {
		fbuf[2*i] = bufleft[i];
		fbuf[2*i + 1] = bufright[i];
	}

        float *wbuf = fbuf;
        if (sample_frequency != dev_sample_rate[1] || progdefaults.TX_corr != 0) {
                resample(1 << O_WRONLY, wbuf, count);
                wbuf = tx_src_data->data_out;
                count = tx_src_data->output_frames_gen;
        }

	int err;
	if (pa_simple_write(stream[1], wbuf, count * sizeof(double), &err) == -1)
		throw SndPulseException(err);

	return count;
}

size_t SoundPulse::Read(double *buf, size_t count)
{
	size_t ncount = (size_t)MIN(SND_BUF_LEN, floor(count / rx_src_data->src_ratio));
	if (count == 1 && ncount == 0)
		ncount = 1;

	int err;
	if (pa_simple_read(stream[0], fbuf, sizeof(double) * ncount, &err) == -1)
		throw SndPulseException(err);

#if USE_SNDFILE
	if (playback) {
		read_file(ifPlayback, buf, count);
		if (progdefaults.EnableMixer) {
	                double vol = progStatus.RcvMixer;
	                for (size_t i = 0; i < count; i++)
        	                buf[i] *= vol;
		}
		return count;
	}
#endif

        float *rbuf = fbuf;
        if (sample_frequency != dev_sample_rate[0] || progdefaults.RX_corr != 0) {
                resample(1 << O_RDONLY, rbuf, ncount, count);
                rbuf = rx_src_data->data_out;
                count = rx_src_data->output_frames_gen;
        }

	for (size_t i = 0; i < count; i++)
                buf[i] = rbuf[2*i];

#if USE_SNDFILE
	if (capture)
                write_file(ofCapture, buf, count);
#endif

	return count;
}

void SoundPulse::src_data_reset(int mode)
{
        int err;
        if (mode & 1 << O_RDONLY) {
                if (rx_src_state)
                        src_delete(rx_src_state);
                rx_src_state = src_new(progdefaults.sample_converter, stream_params.channels, &err);
                if (!rx_src_state)
                        throw SndException(src_strerror(err));
                rx_src_data->src_ratio = sample_frequency / (dev_sample_rate[0] * (1.0 + rxppm / 1e6));
        }
        if (mode & 1 << O_WRONLY) {
                if (tx_src_state)
                        src_delete(tx_src_state);
                tx_src_state = src_new(progdefaults.sample_converter, stream_params.channels, &err);
                if (!tx_src_state)
                        throw SndException(src_strerror(err));
                tx_src_data->src_ratio = dev_sample_rate[1] * (1.0 + txppm / 1e6) / sample_frequency;
        }
}

void SoundPulse::resample(int mode, float *buf, size_t count, size_t max)
{
        int r;

        if (mode & 1 << O_RDONLY) {
                if (rxppm != progdefaults.RX_corr) {
                        rxppm = progdefaults.RX_corr;
                        rx_src_data->src_ratio = sample_frequency / (dev_sample_rate[0] * (1.0 + rxppm / 1e6));
                        src_set_ratio(rx_src_state, rx_src_data->src_ratio);
                }

                rx_src_data->data_in = buf;
                rx_src_data->input_frames = count;
                rx_src_data->data_out = snd_buffer;
                rx_src_data->output_frames = max ? max : SND_BUF_LEN;
                rx_src_data->end_of_input = 0;

                if ((r = src_process(rx_src_state, rx_src_data)) != 0)
			throw SndException(src_strerror(r));
        }
        else if (mode & 1 << O_WRONLY) {
                if (txppm != progdefaults.TX_corr) {
                        txppm = progdefaults.TX_corr;
                        tx_src_data->src_ratio = dev_sample_rate[1] * (1.0 + txppm / 1e6) / sample_frequency;
                        src_set_ratio(tx_src_state, tx_src_data->src_ratio);
                }

                tx_src_data->data_in = buf;
                tx_src_data->input_frames = count;
                tx_src_data->data_out = src_buffer;
                tx_src_data->output_frames = max ? max : SND_BUF_LEN;
                tx_src_data->end_of_input = 0;

                if ((r = src_process(tx_src_state, tx_src_data)) != 0)
			throw SndException(src_strerror(r));
        }
}

#endif // USE_PULSEAUDIO


size_t SoundNull::Write(double* buf, size_t count)
{
#if USE_SNDFILE
	if (generate)
                write_file(ofGenerate, buf, count);
#endif

	usleep((useconds_t)ceil((1e6 * count) / sample_frequency));

	return count;
}

size_t SoundNull::Write_stereo(double* bufleft, double* bufright, size_t count)
{
#if USE_SNDFILE
	if (generate)
                write_file(ofGenerate, bufleft, count);
#endif

	usleep((useconds_t)ceil((1e6 * count) / sample_frequency));

	return count;
}

size_t SoundNull::Read(double *buf, size_t count)
{
	memset(buf, 0, count * sizeof(*buf));

#if USE_SNDFILE
	if (capture)
                write_file(ofCapture, buf, count);
	if (playback) {
		read_file(ifPlayback, buf, count);
		if (progdefaults.EnableMixer) {
	                double vol = progStatus.RcvMixer;
	                for (size_t i = 0; i < count; i++)
        	                buf[i] *= vol;
		}
	}
#endif

	usleep((useconds_t)ceil((1e6 * count) / sample_frequency));

	return count;
}
