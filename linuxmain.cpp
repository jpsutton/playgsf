#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <libgen.h>
#include <chrono>
#include <assert.h>
#include <algorithm>
#include <ao/ao.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>


using std::chrono::steady_clock;
using std::chrono::duration;
#include "types.h"

extern "C" {
#include "VBA/psftag.h"
#include "gsf.h"
}

extern "C" { 
int defvolume=1000;
int relvolume=1000;
int TrackLength=0;
int FadeLength=0;
int IgnoreTrackLength, DefaultLength=150000;
int playforever=0;
int fileoutput=0;
int TrailingSilence=1000;
int DetectSilence=0, silencedetected=0, silencelength=5;

}
int cpupercent=0, sndSamplesPerSec, sndNumChannels;
int sndBitsPerSample=16;

int deflen=120,deffade=4;
#define W 800
int draw_buf[4][2*W];
int n_old[4];
// Draw buf starts full, all samples are 0
int last[4] = {2*W, 2*W, 2*W, 2*W};

extern unsigned short soundFinalWave[1470];
extern int soundBufferLen;
extern int soundIndex;
extern int8_t soundBuffer[4][735];
extern uint8_t *ioMem;
extern uint16_t directBuffer[2][735];
extern int soundLevel1;

extern char soundEcho;
extern char soundLowPass;
extern char soundReverse;
extern char soundQuality;

double decode_pos_ms; // current decoding position, in milliseconds
int seek_needed; // if != -1, it is the point that the decode thread should seek to, in ms.

static int g_playing = 0;
static int g_must_exit = 0;
static SDL_Renderer *rr;
static SDL_Window *w;

static ao_device *snd_ao;

extern "C" int LengthFromString(const char * timestring);
extern "C" int VolumeFromString(const char * volumestring);

extern "C" void end_of_track()
{
	g_playing = 0;
}
	
void updateBuf(int ch, float m) {
	int zeroCrossing = -1;
	int min = *std::min_element(draw_buf[ch], draw_buf[ch]+W);
	int max = *std::max_element(draw_buf[ch], draw_buf[ch]+W);
	int th = (max+min)/2;

	int min_need = W-soundIndex;
	int search_head = last[ch]-min_need;
	for (int i = search_head; i >= 1; i--) {
		if (draw_buf[ch][i-1] >= th && draw_buf[ch][i] < th) {
			zeroCrossing = i;
			break;
		}
	}

	// throw away first `zeroCrossing` samples
	if (zeroCrossing < n_old[ch])
		zeroCrossing = search_head;

	// Update the number of stale samples
	n_old[ch] = last[ch]-zeroCrossing;

	// Move stale samples to the front
	memmove(draw_buf[ch], draw_buf[ch]+zeroCrossing, sizeof(int)*n_old[ch]);

	// Add fresh samples
	for (int i = 0; i < soundIndex; i++)
		draw_buf[ch][n_old[ch]+i] = soundBuffer[ch][i] * m;
	last[ch] = n_old[ch]+soundIndex;
	assert(last[ch] >= W);
}
extern "C" void writeSound(void)
{
	int ret = soundBufferLen;
	//static auto last = steady_clock::now();
	//auto now = steady_clock::now();
	//duration<double> diff = now-last;
	//fprintf(stderr, "%dhz\n", (int)(1/diff.count()));
	//last = now;
	int ratio = ioMem[0x82] & 3;
	SDL_SetRenderDrawColor(rr, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(rr);
	SDL_SetRenderDrawColor(rr, 0, 255, 0, SDL_ALPHA_OPAQUE);
	float m = soundLevel1;
	switch(ratio) {
		case 0:
		case 3:
			m /= 4.0;
			break;
		case 1:
			m /= 2.0;
			break;
		case 2:
			break;
	}
	for (int i = 0; i < 4; i++) {
		updateBuf(i, m);
		int offset = 200+150*i;
		for (int j = 1; j < W; j++)
			SDL_RenderDrawLine(rr, j-1, offset-draw_buf[i][j-1], j, offset-draw_buf[i][j]);
	}
	SDL_RenderPresent(rr);

	ao_play(snd_ao, (char*)soundFinalWave, ret);

	decode_pos_ms += (ret/(2*sndNumChannels) * 1000)/(float)sndSamplesPerSec;
}

extern "C" void signal_handler(int sig)
{
	struct timeval tv_now;
	int elaps_milli;
	
	static int first=1;
	static struct timeval last_int = {0,0};
	
	g_playing = 0;
	gettimeofday(&tv_now, NULL);

	if (first) {
		first = 0;
	}
	else {
		elaps_milli = (tv_now.tv_sec - last_int.tv_sec)*1000;
		elaps_milli += (tv_now.tv_usec - last_int.tv_usec)/1000;

		if (elaps_milli < 1500) {
			g_must_exit = 1;
		}
	}
	memcpy(&last_int, &tv_now, sizeof(struct timeval));
}

static void shuffle_list(char *filelist[], int num_files)
{
	int i, n;
	char *tmp;
	srand((int)time(NULL));
	for (i=0; i<num_files; i++)
	{  
		tmp = filelist[i];
		n = (int)((double)num_files*rand()/(RAND_MAX+1.0));
		filelist[i] = filelist[n];
		filelist[n] = tmp;
	}
}


#define BOLD() printf("%c[36m", 27);
#define NORMAL() printf("%c[0m", 27);




int main(int argc, char **argv)
{
	int r, tmp, fi, random=0;
	char Buffer[1024];
	char length_str[256], fade_str[256], volume[256], title_str[256];
	char tmp_str[256];
	char *tag;

	soundLowPass = 0;
	soundEcho = 0;
	soundQuality = 0;

	DetectSilence=1;
	silencelength=5;	
	IgnoreTrackLength=0;
	DefaultLength=150000;
	TrailingSilence=1000;
	playforever=0;

	SDL_Init(SDL_INIT_VIDEO);
	w = SDL_CreateWindow("playgsf", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W,  800, 0);
	rr = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);

	while((r=getopt(argc, argv, "hlsrieWL:t:"))>=0)	
	{
		char *e;
		switch(r)
		{
			case 'h':
				printf("playgsf version %s (based on Highly Advanced version %s)\n\n", 
						VERSION_STR, HA_VERSION_STR);
				printf("Usage: ./playgsf [options] files...\n\n");
				printf("  -l        Enable low pass filer\n");
				printf("  -s        Detect silence\n");
				printf("  -L        Set silence length in seconds (for detection). Default 5\n");
				printf("  -t        Set default track length in milliseconds. Default 150000 ms\n");
				printf("  -i        Ignore track length (use default length)\n");
				printf("  -e        Endless play\n");
				printf("  -r        Play files in random order\n");
				printf("  -W        output to 'output.wav' rather than soundcard\n");
				printf("  -h        Displays what you are reading right now\n");
				return 0;
				break;
			case 'i':
				IgnoreTrackLength = 1;
				break;
			case 'l':
				soundLowPass = 1;
				break;
			case 's':
				DetectSilence = 1;
				break;
			case 'L':
				silencelength = strtol(optarg, &e, 0);
				if (e==optarg) {
					fprintf(stderr, "Bad value\n");
					return 1;
				}				
				break;
			case 'e':
				playforever = 1;
				break;
			case 't':
				DefaultLength = strtol(optarg, &e, 0);
				if (e==optarg) {
					fprintf(stderr, "Bad value\n");
					return 1;
				}				
				break;
			case 'r':
				random = 1;				
				break;
			case 'W':
				fileoutput = 1;
				break;
			case '?':
				fprintf(stderr, "Unknown argument. try -h\n");
				return 1;
				break;
		}
	}
	
	if (argc-optind<=0) {
		printf("No files specified! For help, try -h\n");
		return 1;
	}


	if (random) { shuffle_list(&argv[optind], argc-optind); }

	printf("playgsf version %s (based on Highly Advanced version %s)\n\n", 
				VERSION_STR, HA_VERSION_STR);

	signal(SIGINT, signal_handler);

	tag = (char*)malloc(50001);

	fi = optind;
	while (!g_must_exit && fi < argc)
	{
		decode_pos_ms = 0;
		seek_needed = -1;
		TrailingSilence=1000;
		
		r = GSFRun(argv[fi]);
		if (!r) {
			fi++;
			continue;
		}

		g_playing = 1;

		psftag_readfromfile((void*)tag, argv[fi]);
	
		BOLD(); printf("Filename: "); NORMAL();
		printf("%s\n", basename(argv[fi]));
		BOLD(); printf("Channels: "); NORMAL();
		printf("%d\n", sndNumChannels);
		BOLD(); printf("Sample rate: "); NORMAL();
		printf("%d\n", sndSamplesPerSec);
		
		if (!psftag_getvar(tag, "title", title_str, sizeof(title_str)-1)) {
			BOLD(); printf("Title: "); NORMAL();
			printf("%s\n", title_str);
		}
		
		if (!psftag_getvar(tag, "artist", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Artist: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "game", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Game: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "year", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Year: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "copyright", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Copyright: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "gsfby", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("GSF By: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "tagger", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Tagger: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "comment", tmp_str, sizeof(tmp_str)-1)) {
			BOLD(); printf("Comment: "); NORMAL();
			printf("%s\n", tmp_str);
		}
		
		if (!psftag_getvar(tag, "fade", fade_str, sizeof(fade_str)-1)) {
			FadeLength = LengthFromString(fade_str);
			BOLD(); printf("Fade: "); NORMAL();
			printf("%s (%d ms)\n", fade_str, FadeLength);
		}
		
		if (!psftag_raw_getvar(tag, "length", length_str, sizeof(length_str)-1)) {
			TrackLength = LengthFromString(length_str) + FadeLength;
			BOLD(); printf("Length: "); NORMAL();
			printf("%s (%d ms) ", length_str, TrackLength);
			if (IgnoreTrackLength) {
				printf("(ignored)");
				TrackLength = DefaultLength;
			}
			printf("\n");
		}
		else {
			TrackLength = DefaultLength;
		}


		/* Must be done after GSFrun so sndNumchannels and 
		 * sndSamplesPerSec are set to valid values */
		ao_initialize();
		ao_sample_format format_ao = {
		  16, sndSamplesPerSec, sndNumChannels, AO_FMT_LITTLE
		};
		if(fileoutput) {
		  snd_ao = ao_open_file(ao_driver_id("wav"),
					"output.wav", 1,
					&format_ao,
					NULL);
		} else {
		  snd_ao = ao_open_live(ao_default_driver_id(),
					&format_ao,
					NULL);
		}
		
		while(g_playing)
		{
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT)
					g_playing = false;
			}
			int remaining = TrackLength - (int)decode_pos_ms;
			if (remaining<0) { 
				// this happens during silence period
				remaining = 0;
			}
			EmulationLoop();
		
			BOLD(); printf("Time: "); NORMAL();
			printf("%02d:%02d.%02d ",
					(int)(decode_pos_ms/1000.0)/60,
					(int)(decode_pos_ms/1000.0)%60,
					(int)(decode_pos_ms/10.0)%100);
			if (!playforever) {
				/*BOLD();*/ printf("["); /*NORMAL();*/
				printf("%02d:%02d.%02d",
					remaining/1000/60, (remaining/1000)%60, (remaining/10%100)
						);
				/*BOLD();*/ printf("] of "); /*NORMAL();*/
				printf("%02d:%02d.%02d ", 
					TrackLength/1000/60, (TrackLength/1000)%60, (TrackLength/10%100)); 
			}
			BOLD(); printf("  GBA Cpu: "); NORMAL();
			printf("%02d%% ", cpupercent);
			printf("     \r");
			
			fflush(stdout);
		}
		printf("\n--\n");
		ao_close(snd_ao);
		fi++;
	}

	ao_shutdown();
	SDL_Quit();
	return 0;
}



