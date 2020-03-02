#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>

#include "unistd.h"
#include "sem.h"

int visitor(int n, long time);
int guide(int n, long time);
bool next_arrives_immediatly(int prob);
long real_time(long now);
void run();
void print(const char * c); // use only for pure text, no var args
void down(cs1550_sem *sem) { syscall(__NR_cs1550_down, sem); }
void up  (cs1550_sem *sem) { syscall(__NR_cs1550_up,   sem); }
void initialize_sems();


//default values
int visitors 			= 5; 	// m
int visitors_burst_prob = 70;	// pv
int visitors_delay 		= 1; 	// dv
int visitors_prob_seed	= 0;	// sv
int guides 				= 5;	// k
int guides_burst_prob 	= 30;	// pg
int guides_delay 		= 1;	// dg
int guides_prob_seed 	= 0;	// sg

struct semlist {

	long start_time;

	cs1550_sem*	museum_open_sem;
	bool 		museum_open;

	cs1550_sem*	guide_available_sem;
	bool 		guide_available;

	cs1550_sem* visitor_count_sem;
	int 		visitor_count;

	cs1550_sem* guide_count_sem;
	int 		guide_count;

	cs1550_sem* guides_in_museum; // initialize to 2

	
} typedef semlist;

semlist * sems;

int main(int argc, char * argv[]) {

	// create shared memory space
	initialize_sems();

	print("Program start\n");
	printf("Start time: %lu\n", sems->start_time);
	fflush(stdout);

	// parse all input arguments
	int i;
	for (i = 1; i < argc; i++) {

		printf("%s\n", argv[i]);
		fflush(stdout);

		if (argv[i][0] == '-') {

			if (argv[i][1] == 'k') {

				guides = atoi(argv[i+1]); 
				
			} else if (argv[i][1] == 'm') {

				visitors = atoi(argv[i+1]);

			} else if (argv[i][1] == 'p') {

				if (argv[i][2] == 'v')  { visitors_burst_prob = atoi(argv[i+1]); } 
				else 					{ guides_burst_prob   = atoi(argv[i+1]); }

			} else if (argv[i][1] == 'd') {

				if (argv[i][2] == 'v')  { visitors_delay = atoi(argv[i+1]); } 
				else 					{ guides_delay   = atoi(argv[i+1]); }

			} else if (argv[i][1] == 's') {

				if (argv[i][2] == 'v')  { visitors_prob_seed = atoi(argv[i+1]); } 
				else 					{ guides_prob_seed   = atoi(argv[i+1]); }

			}

		}

	}

	run();

	return 0;
}

void print(const char * c) {
    printf(c);
    fflush(stdout);
}

void initialize_sems() {
	
	sems = (semlist*)mmap(NULL, sizeof(semlist), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, 0, 0);
	
	sems->start_time = time(NULL);
	
	sems->museum_open_sem->value = 1;
	sems->museum_open = false;

	sems->visitor_count_sem->value = 1;
	sems->visitor_count = 0;

	sems->guide_count_sem->value = 1;
	sems->guide_count = 0;

	sems->guides_in_museum->value = 2;
}

void spawner(int (* func)(int, long), int n, int delay, int prob, int seed) {

	srand(seed);
	int i;
	for (i = 0; i < n; i++) {

		if (fork() == 0) {

			(*func)(i, time(NULL));
			if (!next_arrives_immediatly(prob)) { 
				sleep(delay); 
				print("Hit delay\n"); 
				fflush(stdout); 
			}
			break;

		}
		

	}

	exit(0);

}

void run() {

	if (fork() == 0) {
		spawner(guide, guides, guides_delay, guides_burst_prob, guides_prob_seed);
	} else {
		spawner(visitor, visitors, visitors_delay, visitors_burst_prob, guides_prob_seed);
	}
}

/////////////
// VISITOR //
/////////////

void visitorArrives() {}

void tourMuseum() {}

void visitorLeaves() {}

int visitor(int n, long time) {

	printf("Visitor %d arrived at %lu.\n", n, real_time(time));
	fflush(stdout);


	printf("Visitor %d left.\n", n);
	fflush(stdout);


	exit(0);
}

/////////////
//  GUIDE  //
/////////////

void tourguideArrives() {}

void openMuseum() {}

void tourguideLeaves() {}

int guide(int n, long time) {

	// printf("Guide %d arrived.\n", n);
	// fflush(stdout);


	// printf("Guide %d left.\n", n);
	// fflush(stdout);

	exit(0);
}

/////////////
// UTILITY //
/////////////

bool next_arrives_immediatly(int prob) {
	int r = (rand() % 100);
	printf("Rand % 100 = %d, prob = %d%", r, prob);
	fflush(stdout);
	return (r < prob);
}

long real_time(long now) {
	return (now - sems->start_time);
}