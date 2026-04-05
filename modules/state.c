#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ADTList.h"
#include "ADTVector.h"
#include "k08midi.h"
#include "state.h"


// Οι ολοκληρωμένες πληροφορίες της κατάστασης του παιχνιδιού.
// Ο τύπος State είναι pointer σε αυτό το struct, αλλά το ίδιο το struct
// δεν είναι ορατό στον χρήστη.

struct state {
	struct state_info info;  // Γενικές πληροφορίες για την κατάσταση του παιχνιδιού
	MidiFile midi_file;      // Το .mid αρχείο που διαβάσαμε με τη k08midi_file_read
	List midi_events;        // MIDI events προς αναπαραγωγή
	Vector clips;            // Clip προς ηχογράφηση
	uint recording_index;    // index του τρέχοντος (ή επόμενου) clip
};

// Τμήμα του τραγουδιού στο οποίο υπάρχει marker "rec" και ο χρήστης πρέπει να
// ηχογραφήσει, παίζοντας τις νότες από το αντίστοιχο channel του τραγουδιού. Το
// ηχογραφημένο τμήμα αναπαράγεται σε loop στα σημεία που υπάρχουν οι
// αντίστοιχοι "play" markers.

typedef struct clip {
	String name;		// όνομα
	int channel;		// MIDI κανάλι
	double start;		// χρονικό σημείο έναρξης
	double duration;	// διάρκεια
	List recorded;  	// Ηχογραφημένα MIDI_NOTE events
	List plays;     	// Λίστα με τα σημεία αναπαραγωγής του clip (τύπος ClipPlay)
}* Clip;

// Τμήμα αναπαραγωγής ενός clip, που προκύπτει από τα "play" markers.

typedef struct clip_play {
	double start;		// χρονικό σημείο έναρξης
	double duration;	// διάρκεια
}* ClipPlay;


// Bοηθητικές συναρτήσεις /////////////////////////////////////////////////////////////////////////////////
//
// Δημιουργεί και επιστρέφει ένα σημείο αναπαραγωγής clip
static ClipPlay create_clip_play(double start, double duration) {
	ClipPlay play = malloc(sizeof(*play));
	play->start = start;
	play->duration = duration;
	return play;
}

// Δημιουργεί και επιστρέφει ένα clip
static Clip create_clip(State state, String name, int channel, double start, double duration) {
	Clip clip = malloc(sizeof(*clip));
	clip->name = strdup(name);
	clip->channel = channel;
	clip->start = start;
	clip->duration = duration;
	clip->recorded = list_create(free);
	clip->plays = list_create(free);
	return clip;
}

// Καταστρέφει ένα clip
static void destroy_clip(Pointer value) {
	Clip clip = value;
	free(clip->name);
	list_destroy(clip->recorded);
	list_destroy(clip->plays);
	free(clip);
}

// Βρίσκει και επιστρέφει ένα clip με βάση το όνομά του, NULL αν δεν υπάρχει.
static Clip find_clip_by_name(State state, String clip_name) {
	for (int ci = 0; ci < vector_size(state->clips); ci++) {
		Clip clip = vector_get_at(state->clips, ci);
		if (strcmp(clip->name, clip_name) == 0)
			return clip;
	}
	return NULL;
}

// Αντιγράφει στο state->midi_events όλα τα PROGRAM_CHANGE (αλλαγή οργάνου) και
// CONTROL_CHANGE (έλεγχος volume, modulation, sustain, κλπ), ώστε να είναι
// έτοιμα για αναπαραγωγή μαζί με τις ηχογραφημένες νότες. 
static void append_song_control_events(State state) {
	for (ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)) {
		MidiEvent event = list_node_value(state->midi_file->events, node);
		if (event->type == MIDI_CONTROL_CHANGE || event->type == MIDI_PROGRAM_CHANGE) {
			MidiEvent clone = malloc(sizeof(*clone));
			*clone = *event; // clone
			list_insert_next(state->midi_events, list_last(state->midi_events), clone);
		}
	}
}

// Δημιουργεί clips και clip_plays με βάση τα markers του αρχείου MIDI
static void create_clips(State state) {
	state->clips = vector_create(0, destroy_clip);
	double measure_duration = state_measure_duration(state);

	for (ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)) {
		MidiEvent event = list_node_value(state->midi_file->events, node);
		if (event->type != MIDI_MARKER)
			continue;

		int channel = 0;
		int measures = 0;
		char clip_name[51]; // στη sscanf διαβάζουμε το πολύ 50 χαρακτήρες για όνομα

		// rec και play markers
		if (sscanf(event->marker, "rec,%50[^,],%d,%d", clip_name, &channel, &measures) == 3) {
			// "rec,<name>,<channel>,<measures>" marker, ορίζει ένα clip προς ηχογράφηση
			Clip clip = create_clip(state, clip_name, channel, event->time, measures * measure_duration);
			vector_insert_last(state->clips, clip);

		} else if (sscanf(event->marker, "play,%50[^,],%d", clip_name, &measures) == 2) {
			// "play,<name>,<measures>" marker, ορίζει ένα σημείο αναπαραγωγής του clip
			Clip clip = find_clip_by_name(state, clip_name);
			assert(clip);	// το .mid αρχείο πρέπει να έχει φτιαχτεί σωστά, για κάθε play να προηγείται ένα rec
			list_insert_next(clip->plays, list_last(clip->plays), create_clip_play(event->time, measures * measure_duration));
		}
	}
}

//extra helper function for ask3
//inserts an event to the playback timeline and preserves time order
static void insert_midi_sorted(List events, MidiEvent event){
	ListNode prev = LIST_BOF;
	for(ListNode node = list_first(events); node != LIST_EOF; node = list_next(events, node)){
		MidiEvent current = list_node_value(events, node);
		if(current->time > event->time)		//found the first event that is after the new event, we insert before it
			break;
		prev = node;
	}
	list_insert_next(events, prev, event);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////

State state_create(String midi_file) {
	State state = malloc(sizeof(*state));
	assert(state != NULL);

	state->info.paused = true;
	state->info.game_over = false;
	state->info.accuracy = 0.0;
	state->info.score = 0.0;
	state->info.time = 0.0;
	state->info.level = 1;

	state->midi_file = NULL;
	state->midi_events = list_create(free);

	state->midi_file = k08midi_file_read(midi_file);
	assert(state->midi_file != NULL);

	create_clips(state);
	assert(vector_size(state->clips) > 0);
	state->recording_index = 0;

	list_destroy(state->midi_events);
	state->midi_events = list_create(free);
	append_song_control_events(state);

	return state;
}

StateInfo state_info(State state) {
	// Προς υλοποίηση
	return &state->info;
}

int state_playing_channel(State state) {
	// Το state->recording_index ξεκινάει από το 0 και δείχνει πάντα είτε στο
	// clip που ηχογραφείται τώρα, είτε (αν δεν υπάρχει ενεργή ηχογράφηση) στο
	// αμέσως επόμενο clip. Αφού ολοκληρωθούν όλες οι ηχογραφήσεις το
	// recording_index συνεχίζει να δείχνει το τελευταίο clip.
	//
	// Το κανάλι στο οποίο παίζει "live" ο χρήστης είναι το κανάλι του clip στο
	// οποίο δείχνει το recording_index.

	// Προς υλοποίηση
	Clip clip = vector_get_at(state->clips, state->recording_index);
	return clip->channel;
}

double state_measure_duration(State state) {
	MidiFile midi = state->midi_file;
	return midi->time_signature[0] * (60.0 / midi->tempo) * (4.0 / midi->time_signature[1]);
}

double state_total_duration(State state) {
	// Προς υλοποίηση
	double total_duration = 0.0;
	for(ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)){
		MidiEvent event = list_node_value(state->midi_file->events, node);
		if(event->time > total_duration)	//we do not sum the duration of each event because they might occure at the same time, we look at the time of the last event
			total_duration = event->time;
	}
	return total_duration;
}

List state_displayed_notes(State state, double time_window) {
	// Προς υλοποίηση
	assert(state != NULL);
	//return a view of the original MIDI events
	List displayed_notes = list_create(NULL);	//the events will be freed when the state is destroyed
	double current_time= state->info.time;
	double end_time= current_time + time_window;
	//the MIDI events are already sorted by time so we keep the correct order by inserting at the end of the list
	for(ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)){
		MidiEvent event = list_node_value(state->midi_file->events, node);
		//filter only note events in the future window, [current_time,current_time + time_window]
		if(event->type == MIDI_NOTE && event->time >= current_time && event->time <= end_time)
			list_insert_next(displayed_notes, list_last(displayed_notes), event);
	}
	return displayed_notes;
}

List state_playback_events(State state, double since) {
	// Προς υλοποίηση
	assert(state != NULL);
	//return a view of the playback MIDI events
	List playback_events = list_create(NULL);
	double current_time = state->info.time;
	double start_time = current_time - since;

	//palyback happens at the state->midi_events that contains the timeline of the game
	for(ListNode node = list_first(state->midi_events); node != LIST_EOF; node = list_next(state->midi_events, node)){
		MidiEvent event = list_node_value(state->midi_events, node);
		//events that need to be played at the current time frame, [current-since,current]
		if(event->time >= start_time && event->time <= current_time)
			list_insert_next(playback_events, list_last(playback_events), event);
	}

	return playback_events;
}

void state_update(State state, KeyState ks, double elapsed_time) {
	// Προς υλοποίηση
	assert(state != NULL);

	state->info.game_over = false;	//reset game_over flag (becomes true only for one frame)
	double prev_time = state->info.time;

	//pause and resume handling
	if(ks->space)
		state->info.paused = !state->info.paused;	//pause state
	//frame stepping
	bool step = false;
	if(state->info.paused && ks->n)
		step = true;	//exactly one frame update while paused
	bool advance_frame = !state->info.paused || step;	
	if(!advance_frame)
		return;	//if we are paused and not stepping, we do not update the state
	//time progression
	state->info.time += elapsed_time;

	//clip implementation

	Clip clip = vector_get_at(state->clips, state->recording_index);
	double clip_end_time = clip->start + clip->duration;

	//record note changes to the active clip while its recording window is open
	if(state->info.time >= clip->start && state->info.time <= clip_end_time){
		for(int key = 0; key < 128; key++){
			if(ks->changed_keys[key] >=0){	//if the key changed
				MidiEvent event = malloc(sizeof(*event));
				assert(event != NULL);
				event->type = MIDI_NOTE;
				event->time = state->info.time;
				event->channel = clip->channel;
				event->key = key;
				event->velocity = ks->changed_keys[key];	//velocity is 0 for note off and >0 for note on
				list_insert_next(clip->recorded, list_last(clip->recorded), event);		//add the event to the recorded events of the clip
			}
		}
	}
	//finalize the clip when the recording window is closed
	if(prev_time <= clip_end_time && state->info.time > clip_end_time){
		//note_off events go to the end so that recorded notes do not remain open 
		for(int key = 0; key< 128; key++){
			if(ks->active_keys[key] > 0){
				MidiEvent event = malloc(sizeof(*event));
				assert(event != NULL);
				event->type = MIDI_NOTE;
				event->time = clip_end_time;
				event->channel = clip->channel;
				event->key = key;
				event->velocity = 0;	//note off
				list_insert_next(clip->recorded, list_last(clip->recorded), event);
			}
		}
		//copy the recorded notes to every play segment and repeat it for as long as the segment lasts
		for(ListNode play_node = list_first(clip->plays); play_node != LIST_EOF; play_node = list_next(clip->plays, play_node)){
			ClipPlay play = list_node_value(clip->plays, play_node);
			double play_end_time = play->start + play->duration;
			for(double offset = 0.0; offset < play->duration; offset += clip->duration){
				//repeat the clip every clip->duration seconds in the play segment
				for(ListNode rec_node = list_first(clip->recorded); rec_node != LIST_EOF; rec_node = list_next(clip->recorded, rec_node)){
					MidiEvent recorded = list_node_value(clip->recorded, rec_node);
					//convert from clip-local time to play-segment time
					double relative_time = recorded->time - clip->start;
					double new_time = play->start + offset + relative_time;

					//ignore events that are outside the play segment
					if(new_time > play_end_time)
						continue;

					MidiEvent clone = malloc(sizeof(*clone));
					assert(clone != NULL);
					*clone = *recorded;	//clone the recorded event
					clone->time = new_time;	//adjust the time to the play segment
					insert_midi_sorted(state->midi_events, clone);	//insert the event to the playback timeline
				}
			}
		}
		//go to next clip unless we are at the last one
		if(state->recording_index +1 < vector_size(state->clips))
			state->recording_index++;
	}

}

void state_destroy(State state) {
	// Προς υλοποίηση
	if(state == NULL)
		return;		//nothing to destroy
	if(state->midi_file != NULL)
		k08midi_file_destroy(state->midi_file);		//free through the the library as it contains internal structures 
	if(state->midi_events != NULL)
		list_destroy(state->midi_events);
	if(state->clips != NULL)
		vector_destroy(state->clips);
	free(state);	//free the struct itself
}
