//////////////////////////////////////////////////////////////////
//
// Test για το state.h module
//
//////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <math.h>
#include <memory.h>
#include "acutest.h"			// Απλή βιβλιοθήκη για unit testing

#include "state.h"


///// Βοηθητικές συναρτήσεις ////////////////////////////////////////
//
// σύγκριση double
static bool double_equal(double a, double b) {
	return fabs(a-b) < 1e-6;
}

// αρχικοποίηση πλήκτρων (κανένα πατημένο)
static struct key_state empty_key_state() {
	struct key_state ks = { .space=false, .n=false };
	memset(ks.active_keys, 0, sizeof(ks.active_keys));
	memset(ks.changed_keys, -1, sizeof(ks.changed_keys));
	return ks;
}
/////////////////////////////////////////////////////////////////////


void test_state_create() {
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);

	StateInfo info = state_info(state);
	TEST_ASSERT(info != NULL);
	TEST_ASSERT(info->paused);
	TEST_ASSERT(!info->game_over);
	TEST_ASSERT(double_equal(info->time, 0.0));
	TEST_ASSERT(info->level == 1);

	state_destroy(state);
}

void test_state_basic_functions() {
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);

	TEST_ASSERT(state_playing_channel(state) == 9);
	TEST_ASSERT(double_equal(state_measure_duration(state), 1.93548));
	TEST_ASSERT(double_equal(state_total_duration(state), 18.987));

	state_destroy(state);
}

void test_state_update() {
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);

	StateInfo info = state_info(state);

	//the game starts paused
	TEST_ASSERT(info->paused);
	TEST_ASSERT(double_equal(info->time, 0.0));

	//pressing space should toggle pause
	struct key_state ks = empty_key_state();
	ks.space = true;
	state_update(state, &ks, 0.0);
	TEST_ASSERT(!info->paused);
	TEST_ASSERT(double_equal(info->time, 0.0));

	//time should advance normally while running
	ks = empty_key_state();
	state_update(state, &ks, 0.5);
	TEST_ASSERT(double_equal(info->time, 0.5));

	//pressing space again should pause the game
	ks = empty_key_state();
	ks.space = true;
	state_update(state, &ks, 0.0);
	TEST_ASSERT(info->paused);

	//time should not advance while paused
	ks = empty_key_state();
	state_update(state, &ks, 1.0);
	TEST_ASSERT(double_equal(info->time, 0.5));

	//frame stepping does exactly one update while paused
	ks = empty_key_state();
	ks.n = true;
	state_update(state, &ks, 0.25);
	TEST_ASSERT(double_equal(info->time, 0.75));

	state_destroy(state);
}

//2. additional tests


//a.checks if only MIDI_NOTE events are returned
//b.checks if the events are in the correct time window
//c.checks if order is preserved
void test_state_displayed_notes(){
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);
	StateInfo info = state_info(state);

	//at first time=0, so we check the first events
	List notes = state_displayed_notes(state, 2.0);
	TEST_ASSERT(notes != NULL);

	double prev_time = -1.0;

	for(ListNode node = list_first(notes); node != LIST_EOF; node = list_next(notes, node)){
		MidiEvent event = list_node_value(notes, node);
		TEST_ASSERT(event->type == MIDI_NOTE);
		TEST_ASSERT(event->time >= info->time);
		TEST_ASSERT(event->time <= info->time + 2.0);
		TEST_ASSERT(event->time >= prev_time);	
		prev_time = event->time;
	}

	//test again after changing the time
	info->time = 5.0;
	List notes2 = state_displayed_notes(state, 1.0);

	for(ListNode node = list_first(notes2); node != LIST_EOF; node = list_next(notes2, node)){
		MidiEvent event = list_node_value(notes2, node);
		TEST_ASSERT(event->time >= 5.0);
		TEST_ASSERT(event->time <= 6.0);
	}
	state_destroy(state);
}

//a.checks if the events are in the correct time window [time-since,time]
//b.checks if order is preserved
void test_state_playback_events(){
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);

	StateInfo info = state_info(state);

	info->time = 10.0; 		//advance time to ensure events exist in the past window
	List events = state_playback_events(state, 2.0);
	TEST_ASSERT(events != NULL);

	double prev_time = -1.0;
	
	for(ListNode node = list_first(events); node != LIST_EOF; node = list_next(events, node)){
		MidiEvent event = list_node_value(events, node);
		TEST_ASSERT(event->time >=8.0);
		TEST_ASSERT(event->time <= 10.0);
		TEST_ASSERT(event->time >= prev_time);	
		prev_time = event->time;
	}

	//test again with smaller window
	List events2 = state_playback_events(state, 0.5);
	
	for(ListNode node = list_first(events2); node != LIST_EOF; node = list_next(events2, node)){
		MidiEvent event = list_node_value(events2, node);
		TEST_ASSERT(event->time >= 9.5);
		TEST_ASSERT(event->time <= 10.0);
	}
	state_destroy(state);
}

void test_state_progression_and_scoring(){
	State state = state_create("test.mid");
	TEST_ASSERT(state != NULL);

	StateInfo info = state_info(state);

	//initial score state
	TEST_ASSERT(double_equal(info->score, 0.0));
	TEST_ASSERT(double_equal(info->accuracy, 0.0));
	TEST_ASSERT(info->level == 1);
	TEST_ASSERT(!info->game_over);

	//resume game
	struct key_state ks = empty_key_state();
	ks.space = true;
	state_update(state, &ks, 0.0);
	TEST_ASSERT(!info->paused);

	//advance time in a manner that the song ends to check if the end of song handling works correctly
	ks = empty_key_state();
	state_update(state, &ks, state_total_duration(state) + 1.0);

	//we also consider the case in which no notes are played
	TEST_ASSERT(info->score >= 0.0);	//score should have increased
	TEST_ASSERT(info->accuracy >= 0.0);	//accuracy should have increased
	TEST_ASSERT(info->accuracy <= 1.0);	//accuracy should be at most 1.0

	//after the end of the song, the game should pause(game over) or restart at a valid level
	TEST_ASSERT(info->game_over || info->level >= 1);
	
	state_destroy(state);
}


// Λίστα με όλα τα tests προς εκτέλεση
TEST_LIST = {
	{ "test_state_create", test_state_create },
	{ "test_state_basic_functions", test_state_basic_functions },
	{ "test_state_update", test_state_update },
	{ "test_state_displayed_notes", test_state_displayed_notes },
	{ "test_state_playback_events", test_state_playback_events },
	{ "test_state_progression_and_scoring", test_state_progression_and_scoring },

	{ NULL, NULL } // τερματίζουμε τη λίστα με NULL
};
