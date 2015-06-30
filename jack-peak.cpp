/** jack-peak - JACK audio peak server
 *
 * This tool is based on capture_client.c from the jackaudio.org examples
 * and modified by Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Copyright (C) 2001 Paul Davis
 * Copyright (C) 2003 Jack O'Quin
 * Copyright (C) 2008, 2012 Robin Gareus
 *
 * compile with
 *   gcc -o jack-peak jack-peak.c -ljack -lm -lpthread
 */

#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <signal.h>
#include <math.h>
#include <errno.h>
#include <jack/jack.h>
#include <sys/file.h>
#include "oscpack/osc/OscOutboundPacketStream.h"
#include "oscpack/ip/UdpSocket.h"

#define ADDRESS "127.0.0.1"
#define PORT 7770

#define OUTPUT_BUFFER_SIZE 1024

#ifndef VERSION
#define VERSION "0.8"
#endif

typedef struct _thread_info {
	pthread_t thread_id;
	pthread_t mesg_thread_id;
	jack_nframes_t samplerate;
	jack_nframes_t buffersize;
	jack_client_t *client;
	unsigned int channels;
	useconds_t delay;
	float *peak;
	float *pcur;
	float *pmax;
	int   *ptme;
	/* format - bitwise
	 * 1  (1) -- on: print carrige-return, off: print newline
	 * 2  (2) -- on: IEC-268 dB scale, off: linear float
	 * 3  (4) -- on: JSON, off: plain text
	 * 4  (8) -- include peak-hold
	 * 5 (16) --
	 */
	char *message;
	char *address;
	int port;
	int format;
	float iecmult;
	FILE *outfd;
	volatile int can_capture;
	volatile int can_process;
} jack_thread_info_t;

#define SAMPLESIZE (sizeof(jack_default_audio_sample_t))

/* JACK data */
jack_port_t **ports;
jack_default_audio_sample_t **in;
jack_nframes_t nframes;

/* Synchronization between process thread and disk thread. */
pthread_mutex_t io_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

/* global options/status */
int want_quiet = 0;
volatile int run = 1;

void cleanup(jack_thread_info_t *info) {
	free(info->peak);
	free(info->pcur);
	free(info->pmax);
	free(info->ptme);
	free(in); free(ports);
}

/* output functions and helpers */

float iec_scale(float db) {
	 float def = 0.0f;

	 if (db < -70.0f) {
		 def = 0.0f;
	 } else if (db < -60.0f) {
		 def = (db + 70.0f) * 0.25f;
	 } else if (db < -50.0f) {
		 def = (db + 60.0f) * 0.5f + 2.5f;
	 } else if (db < -40.0f) {
		 def = (db + 50.0f) * 0.75f + 7.5;
	 } else if (db < -30.0f) {
		 def = (db + 40.0f) * 1.5f + 15.0f;
	 } else if (db < -20.0f) {
		 def = (db + 30.0f) * 2.0f + 30.0f;
	 } else if (db < 0.0f) {
		 def = (db + 20.0f) * 2.5f + 50.0f;
	 } else {
		 def = 100.0f;
	 }
	 return def;
}

int peak_db(float peak, float bias, float mult) {
	return (int) (iec_scale(20.0f * log10f(peak * bias)) * mult);
}

void * io_thread (void *arg) {
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&io_thread_lock);

	while (run) {
		const int pkhld = ceilf(2.0 / ( (info->delay/1000000.0) + ((float)info->buffersize / (float)info->samplerate)));

		if (info->can_capture) {
			int chn;
			memcpy(info->peak, info->pcur, sizeof(float)*info->channels);
			for (chn = 0; chn < info->channels; ++chn) { info->pcur[chn]=0.0; }

			if ((info->format&1)==0) {
				if (flock(fileno(info->outfd), LOCK_EX)) continue; // pthread_cond_wait ?
				fseek(info->outfd, 0L, SEEK_SET);
			}
			switch (info->format&6) {
				case 6:
				case 4:
					fprintf(info->outfd,"{\"cnt\":%d,\"peak\":[", info->channels);
					break;
				default:
					break;
			}
			for (chn = 0; chn < info->channels; ++chn) {
				switch (info->format&6) {
					case 0:
						printf("%3.3f  ", info->peak[chn]); break;
						//printf("%s  ", info->address); break;
					case 2:
						fprintf(info->outfd,"%3d  ", peak_db(info->peak[chn], 1.0, info->iecmult)); break;
					case 4:
						printf("%.3f,", info->peak[chn]); break;
					case 6:
						fprintf(info->outfd,"%d,", peak_db(info->peak[chn], 1.0, info->iecmult)); break;
				}

				/* Insert OSC stuff here */
				UdpTransmitSocket transmitSocket( IpEndpointName( info->address, info->port ) );
    		
				char buffer[OUTPUT_BUFFER_SIZE];
				osc::OutboundPacketStream stream( buffer, OUTPUT_BUFFER_SIZE );
		
				stream //<< osc::BeginBundleImmediate
					<< osc::BeginMessage( info->message ) 
			    			<< (float)info->peak[chn] << osc::EndMessage
				;//<< osc::EndBundle;

				transmitSocket.Send( stream.Data(), stream.Size() );

				if (info->peak[chn] > info->pmax[chn]) {
					info->pmax[chn] = info->peak[chn];
					info->ptme[chn] = 0;
				} else if (info->ptme[chn] <= pkhld) {
					(info->ptme[chn])++;
				} else {
					info->pmax[chn] = info->peak[chn];
				}
			}
			if (info->format&8) { // add peak-hold
				if (info->format&4) {
					fprintf(info->outfd,"],\"max\":[");
				} else {
					fprintf(info->outfd," | ");
				}

				for (chn = 0; chn < info->channels; ++chn) {
					switch (info->format&6) {
						case 0:
							printf("%3.3f  ", info->pmax[chn]); break;
						case 2:
							fprintf(info->outfd,"%3d  ", peak_db(info->pmax[chn], 1.0, info->iecmult)); break;
						case 4:
							printf("%.3f,", info->pmax[chn]); break;
						case 6:
							fprintf(info->outfd,"%d,", peak_db(info->pmax[chn], 1.0, info->iecmult)); break;
					}
				}
			}
			switch (info->format&6) {
				case 6:
				case 4:
					fprintf(info->outfd,"]}");
					break;
				default:
					break;
			}

			if (info->format&1)
				fprintf(info->outfd, "\r");
			else {
				ftruncate(fileno(info->outfd), ftell(info->outfd));
				fprintf(info->outfd, "\n");
				flock(fileno(info->outfd), LOCK_UN);
			}

			fflush(info->outfd);

			if (info->delay>0) usleep(info->delay);
		}
		pthread_cond_wait(&data_ready, &io_thread_lock);
	}

	pthread_mutex_unlock(&io_thread_lock);
	return 0;
}

/* jack callbacks */

int jack_bufsiz_cb(jack_nframes_t nframes, void *arg) {
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	info->buffersize=nframes;
	return 0;
}
	
int process (jack_nframes_t nframes, void *arg) {
	int chn;
	size_t i;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	/* Do nothing until we're ready to begin. */
	if ((!info->can_process) || (!info->can_capture))
		return 0;

	for (chn = 0; chn < info->channels; ++chn)
		in[chn] = (jack_default_audio_sample_t*)jack_port_get_buffer(ports[chn], nframes);

	for (i = 0; i < nframes; i++) {
		for (chn = 0; chn < info->channels; ++chn) {
			const float js = fabsf(in[chn][i]);
			if (js > info->pcur[chn]) info->pcur[chn] = js;
		}
	}

	/* Tell the io thread there is work to do. */
	if (pthread_mutex_trylock(&io_thread_lock) == 0) {
	    pthread_cond_signal(&data_ready);
	    pthread_mutex_unlock(&io_thread_lock);
	}
	return 0;
}

void jack_shutdown (void *arg) {
	fprintf(stderr, "JACK shutdown\n");
	abort();
}

void setup_ports (int nports, char *source_names[], jack_thread_info_t *info) {
	unsigned int i;
	const size_t in_size =  nports * sizeof(jack_default_audio_sample_t *);

	info->peak = (float*) malloc(sizeof(float) * nports);
	info->pcur = (float*) malloc(sizeof(float) * nports);
	info->pmax = (float*) malloc(sizeof(float) * nports);
	info->ptme = (int*) malloc(sizeof(int  ) * nports);

	/* Allocate data structures that depend on the number of ports. */
	ports = (jack_port_t **) malloc(sizeof(jack_port_t *) * nports);
	in = (jack_default_audio_sample_t **) malloc(in_size);
	memset(in, 0, in_size);

	for (i = 0; i < nports; i++) {
		char name[64];
		info->peak[i]=0.0;
		info->pcur[i]=0.0;
		info->ptme[i]=0;

		sprintf(name, "input%d", i+1);

		if ((ports[i] = jack_port_register(info->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
			fprintf(stderr, "cannot register input port \"%s\"!\n", name);
			jack_client_close(info->client);
			cleanup(info);
			exit(1);
		}
	}

	for (i = 0; i < nports; i++) {
		if (jack_connect(info->client, source_names[i], jack_port_name(ports[i]))) {
			fprintf(stderr, "cannot connect input port %s to %s\n", jack_port_name(ports[i]), source_names[i]);
#if 0 /* not fatal - connect manually */
			jack_client_close(info->client);
			exit(1);
#endif
		}
	}

	/* process() can start, now */
	info->can_process = 1;
}

/* main */

void catchsig (int sig) {
#ifndef _WIN32
	signal(SIGHUP, catchsig); /* reset signal */
#endif
	if (!want_quiet)
		fprintf(stderr,"\n CAUGHT SIGNAL - shutting down.\n");
	run=0;
	/* signal writer thread */
	pthread_mutex_lock(&io_thread_lock);
	pthread_cond_signal(&data_ready);
	pthread_mutex_unlock(&io_thread_lock);
}


static void usage (char *name, int status) {
  fprintf (status?stderr:stdout,
			"%s - live peak-signal meter for JACK\n", basename(name));
  fprintf (status?stderr:stdout,
"Utility to write audio peak data to stdout of file as plain text of JSON data.\n"
"\n"
	);
	fprintf(status?stderr:stdout,
			"Usage: %s [ OPTIONS ] port1 [ port2 ... ]\n\n", name);
	fprintf(status?stderr:stdout,
"Options:\n"
"  -d, --delay <int>            output speed in miliseconds (default 100ms)\n"
"                               a delay of zero writes at jack-period intervals\n"
"  -f, --file <path>            write to file instead of stdout\n"
"  -h, --help                   print this message\n"
"  -i, --iec268 <mult>          use dB scale; output range 0-<mult> (integer)\n"
"                               - if not specified, output range is linear [0..1]\n"
"  -j, --json                   write JSON format instead of plain text\n"
"  -p, --peakhold               add peak-hold information.\n"
"  -q, --quiet                  inhibit usual output\n"
"  -o, --osc <address>          the destination of OSC messages of message -m\n"
"  -x, --port <port number>     the port of OSC messages of message. default 7770 -m\n"
"  -m, --message                the OSC message - example /1/fader1"
"\n"
"Examples:\n"
"jack-peak system:capture_1 system:capture_2\n"
"\n"
"jack-peak --iec268 200 --json --file /tmp/peaks.json system:capture_1\n"
"\n"
"Report bugs to <robin@gareus.org>.\n"
"Website and manual: <http://gareus.org/oss/jack_peak>\n"
	);
	exit(status);
}

int main (int argc, char **argv) {
	jack_client_t *client;
	jack_thread_info_t thread_info;
	jack_status_t jstat;
	int c;

	memset(&thread_info, 0, sizeof(thread_info));
	thread_info.channels = 2;
	thread_info.delay = 100000;
	thread_info.format = 0;
	thread_info.iecmult = 2.0;
	thread_info.outfd = NULL;
	thread_info.address = "127.0.0.1";
	thread_info.port = 7770;
	thread_info.message = "/1/fader1";

	const char *optstring = "hqpjiomx:d:f:V";
	struct option long_options[] = {
		{ "help",     no_argument,       0, 'h' },
		{ "json",     no_argument,       0, 'j' },
		{ "iec268",   required_argument, 0, 'i' },
		{ "file",     required_argument, 0, 'f' },
		{ "delay",    required_argument, 0, 'd' },
		{ "peakhold", required_argument, 0, 'p' },
		{ "osc",      required_argument, 0, 'o' },
		{ "port",     required_argument, 0, 'x' },
		{ "message",  required_argument, 0, 'm' },
		{ "quiet",    no_argument,       0, 'q' },
		{ "version",  no_argument,       0, 'V' },
		{ 0, 0, 0, 0 }
	};

	int option_index;

	while ((c = getopt_long(argc, argv, optstring, long_options, &option_index)) != -1) {
		switch (c) {
			case 'h':
				usage(argv[0], 0);
				break;
			case 'q':
				want_quiet = 1;
				break;
			case 'i':
				thread_info.format|=2;
				thread_info.iecmult = atof(optarg)/100.0;
				break;
			case 'j':
				thread_info.format|=4;
				break;
			case 'p':
				thread_info.format|=8;
				break;
			case 'f':
				if (thread_info.outfd) fclose(thread_info.outfd);
				thread_info.outfd = fopen(optarg, "w");
				break;
			case 'd':
				if (atol(optarg) < 0 || atol(optarg) > 60000)
					fprintf(stderr, "delay: time out of bounds.\n");
				else
					thread_info.delay = 1000*atol(optarg);
				break;
			case 'm':
				thread_info.message = optarg;
				break;
			case 'o':
				thread_info.address = optarg;
				break;
			case 'x':
				thread_info.port = atoi(optarg); 
				break;
			case 'V':
				printf ("%s %s\n\n",argv[0], VERSION);
				printf(
"Copyright (C) 2012 Robin Gareus <robin@gareus.org>\n"
"This is free software; see the source for copying conditions.  There is NO\n"
"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n"
);
				break;
			default:
				fprintf(stderr, "invalid argument.\n");
				usage(argv[0], 0);
				break;
		}
	}


	if (!thread_info.outfd) {
		thread_info.outfd=stdout;
		thread_info.format|=1;
	}

	if (argc <= optind) {
		fprintf(stderr, "At least one port/audio-channel must be given.\n");
		usage(argv[0], 1);
	}

	/* set up JACK client */
	if ((client = jack_client_open("jackpeak", JackNoStartServer, &jstat)) == 0) {
		fprintf(stderr, "Can not connect to JACK.\n");
		exit(1);
	}

	thread_info.client = client;
	thread_info.can_process = 0;
	thread_info.channels = argc - optind;

	jack_set_process_callback(client, process, &thread_info);
	jack_on_shutdown(client, jack_shutdown, &thread_info);
	jack_set_buffer_size_callback(client, jack_bufsiz_cb, &thread_info);

	if (jack_activate(client)) {
		fprintf(stderr, "cannot activate client");
	}

	setup_ports(thread_info.channels, &argv[optind], &thread_info);

	/* set up i/o thread */
	pthread_create(&thread_info.thread_id, NULL, io_thread, &thread_info);
#ifndef _WIN32
	signal (SIGHUP, catchsig);
#endif
	thread_info.samplerate = jack_get_sample_rate(thread_info.client);

	if (!want_quiet) {
		fprintf(stderr, "%i channel%s, @%iSPS.\naddress: %s:%i\nmessage: %s\n",
			thread_info.channels,
			(thread_info.channels>1)?"s":"",
			thread_info.samplerate,
			thread_info.address,
			thread_info.port,
			thread_info.message
		);
	}

	/* all systems go - run the i/o thread */
	thread_info.can_capture = 1;
	pthread_join(thread_info.thread_id, NULL);

	if (thread_info.outfd != stdout)
		fclose(thread_info.outfd);

	jack_client_close(client);

	cleanup(&thread_info);

	return(0);
}
