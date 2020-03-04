// Derek Nadeau CS1550 Project 2 Spring 2020 // DRN16@pitt.edu

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include "unistd.h"
#include "sem.h"

int visitor(int n);
int guide(int n);
int real_time();
bool next_arrives_immediatly(int prob);
void spawner(int (* func)(int), int n, int delay, int prob, int seed);
void down(struct cs1550_sem * sem) { syscall(__NR_cs1550_down, sem); }
void up  (struct cs1550_sem * sem) { syscall(__NR_cs1550_up,   sem); }
void initialize_sems();

//default values
int visitors 			= 50; 	// m
int visitors_burst_prob = 100;	// pv
int visitors_delay 		= 1; 	// dv
int visitors_prob_seed	= 10;	// sv
int guides 				= 5;	// k
int guides_burst_prob 	= 0;	// pg
int guides_delay 		= 3;	// dg
int guides_prob_seed 	= 20;	// sg

struct semlist {

	struct cs1550_sem 	visitor_count_sem;
	int 				visitor_count; 			// # of waiting visitors not yet in museum

	struct cs1550_sem  	guide_count_sem;
	int 				guide_count; 			// # of waiting guides not yet in museum

	struct cs1550_sem  	visitors_in_museum_sem; 
	int 				visitors_in_museum;		// # visitors currently in museum

	struct cs1550_sem  	guides_in_museum_sem; 	
	int 				guides_in_museum;		// # guides currently in museum

	struct cs1550_sem	claim_leaving_visitor_sem;
	int 				claim_leaving_visitor;	// used to keep track of leaving visitors unclaimed by a tour guide

	struct cs1550_sem 	spots_to_claim_sem;
	int 				spots_to_claim;			// used to keep track of how many arriving visitors can currently enter the museum 

} typedef semlist;

semlist * sems;
struct timeval * start_time;

int main(int argc, char * argv[]) {

	// create shared memory space
	initialize_sems();

	// initalize time vars
	start_time = malloc(sizeof(struct timeval));
	gettimeofday(start_time, NULL);

	// parse all input arguments in any order
	int i;
	for (i = 1; i < argc; i++) {

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

	printf("The museum is now empty.\n");
	if (fork() == 0)      { spawner(visitor, visitors, visitors_delay, visitors_burst_prob, guides_prob_seed); } 
	else if (fork() == 0) { spawner(guide,   guides,   guides_delay,   guides_burst_prob,   guides_prob_seed); }
	else 				  { wait(NULL); wait(NULL); }

	return 0;

}


void initialize_sems() {
	
	sems = (semlist*)mmap(NULL, sizeof(semlist), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, 0, 0);

	sems->visitor_count_sem.value 			= 1;
	sems->visitor_count 					= 0;

	sems->guide_count_sem.value 			= 1;
	sems->guide_count 						= 0;

	sems->guides_in_museum_sem.value		= 1;
	sems->guides_in_museum 					= 0;

	sems->visitors_in_museum_sem.value 		= 1;
	sems->visitors_in_museum 				= 0;

	sems->claim_leaving_visitor_sem.value 	= 1;
	sems->claim_leaving_visitor 			= 0;

	sems->spots_to_claim_sem.value 			= 1;
	sems->spots_to_claim 					= 0;

}

void spawner(int (* func)(int), int n, int delay, int prob, int seed) {

	srand(seed);
	int i;
	for (i = 0; i < n; i++) {

		// if not first visitor, check for burst delay and simulate accordingly
		if (!next_arrives_immediatly(prob) && i != 0) {	sleep(delay); }
		// create new visitor and guide processes
		if (fork() == 0) { (*func)(i); }
		// no need to break, visitor/guide process will exit() on its own

	}

	wait(NULL);
	exit(0);

}

/////////////
// VISITOR //
/////////////

void visitorArrives(int n) {

	down(&(sems->visitor_count_sem));

	sems->visitor_count++;
	printf("Visitor %d arrives at time %d.\n", n, real_time()); fflush(stdout);

	up(&(sems->visitor_count_sem));

}

void tourMuseum(int n) {

	bool can_enter = false;
	while (!can_enter) {

		down(&(sems->guides_in_museum_sem));
		down(&(sems->visitor_count_sem));
		down(&(sems->spots_to_claim_sem));
		down(&(sems->visitors_in_museum_sem));

		can_enter = (sems->visitors_in_museum < (sems->guides_in_museum * 10)) && sems->spots_to_claim > 0;
		
		if (can_enter) {

			sems->visitor_count--;
			
			sems->spots_to_claim--;
			
			sems->visitors_in_museum++;

			printf("Visitor %d tours the museum at time %d.\n", n, real_time()); fflush(stdout);

		}

		up(&(sems->guides_in_museum_sem));
		up(&(sems->spots_to_claim_sem));
		up(&(sems->visitor_count_sem));
		up(&(sems->visitors_in_museum_sem));
		

	}

	sleep(2);

}

void visitorLeaves(int n) {

	down(&(sems->claim_leaving_visitor_sem));
	down(&(sems->visitors_in_museum_sem));

	sems->claim_leaving_visitor++;
	sems->visitors_in_museum--;

	printf("Visitor %d leaves the museum at time %d.\n", n, real_time()); fflush(stdout);
	
	up(&(sems->claim_leaving_visitor_sem));
	up(&(sems->visitors_in_museum_sem));	

}

int visitor(int n) {

	visitorArrives(n);
	tourMuseum(n);
	visitorLeaves(n);
	exit(0);

}

/////////////
//  GUIDE  //
/////////////

void tourguideArrives(int n) {
	
	down(&(sems->guide_count_sem));

	sems->guide_count++;
	printf("Tour guide %d arrives at time %d.\n", n, real_time()); fflush(stdout);

	up(&(sems->guide_count_sem));

}

void openMuseum(int n) {

	bool can_open = false;
	while (!can_open) {

		down(&(sems->guide_count_sem));
		down(&(sems->guides_in_museum_sem));
		down(&(sems->spots_to_claim_sem));
		down(&(sems->visitor_count_sem));

		can_open = (sems->visitor_count > 0) && (sems->guides_in_museum < 2);

		if (can_open) {

			sems->guide_count--;

			sems->guides_in_museum++;

			sems->spots_to_claim += 10;
			
			printf("Tour guide %d opens the museum for tours at time %d.\n", n, real_time());
			fflush(stdout);

		}		

		up(&(sems->guide_count_sem));
		up(&(sems->guides_in_museum_sem));
		up(&(sems->spots_to_claim_sem));
		up(&(sems->visitor_count_sem));

	}

}

void tourguideLeaves(int n) {

	bool can_leave = false;
	int claimed_visitors = 0;
	while (!can_leave) {

		down(&(sems->claim_leaving_visitor_sem));
		down(&(sems->guides_in_museum_sem));
		down(&(sems->visitor_count_sem));
		down(&(sems->visitors_in_museum_sem));

		while (claimed_visitors < 10 && sems-> claim_leaving_visitor > 0) {
			claimed_visitors++;
			sems->claim_leaving_visitor--;
		}
		
		can_leave = (claimed_visitors == 10) || 
					((sems->visitors_in_museum <= ((sems->guides_in_museum - 1) * 10)) && (sems->visitor_count == 0));


		if (can_leave) {
			sems->guides_in_museum--;
			printf("Tour guide %d leaves the museum at time %d.\n", n, real_time()); fflush(stdout);
		}

		up(&(sems->claim_leaving_visitor_sem));
		up(&(sems->guides_in_museum_sem));
		up(&(sems->visitor_count_sem));
		up(&(sems->visitors_in_museum_sem));
		
	}

}

int guide(int n) {

	tourguideArrives(n);
	openMuseum(n);
	tourguideLeaves(n);
	exit(0);

}

/////////////
// UTILITY //
/////////////

bool next_arrives_immediatly(int prob) {
	return ((rand() % 100) < prob);

}

int real_time() {
	
	struct timeval * now = malloc(sizeof(struct timeval));
	gettimeofday(now, NULL);
	// struct timeval * rel = malloc(sizeof(struct timeval));

	int tv_sec  = (int) now->tv_sec  - start_time->tv_sec;
	int tv_usec = (int) now->tv_usec - start_time->tv_usec;

	if (tv_usec < 0) { tv_sec--; }

	free(now);

	return tv_sec;

}