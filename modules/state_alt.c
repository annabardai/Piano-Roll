#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ADTList.h"
#include "ADTVector.h"
#include "ADTMap.h"
#include "k08midi.h"
#include "state.h"

#define TIME_BUCKET 0.1 	//discretize time into buckets to accelerate range queries


// Οι ολοκληρωμένες πληροφορίες της κατάστασης του παιχνιδιού.
// Ο τύπος State είναι pointer σε αυτό το struct, αλλά το ίδιο το struct
// δεν είναι ορατό στον χρήστη.

struct state {
	struct state_info info;  // Γενικές πληροφορίες για την κατάσταση του παιχνιδιού
	MidiFile midi_file;      // Το .mid αρχείο που διαβάσαμε με τη k08midi_file_read
	List midi_events;        // MIDI events προς αναπαραγωγή
	Vector clips;            // Clip προς ηχογράφηση
	uint recording_index;    // index του τρέχοντος (ή επόμενου) clip

	//add metrics for ask4
	double clips_score_sum;    //sum of per clip scores
	double clips_accuracy_sum; //sum of per clip accuracies
	int completed_clips;       //number of clips evaluated 

	//optimizaton for ask5
    Map displayed_index;   //song MIDI_NOTE events indexed by time bucket 
    Map playback_index;    //playback events indexed by time bucket 
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
			add_playback_event(state, clone); 
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


//extra helper functions for ask4

//1.builds a view of the notes of the given clip
//keeps only MIDI_NOTE eventsthat are inside the clip time window and belong to the clip channel
static List create_clip_song(State state, Clip clip){
	List clip_song = list_create(NULL);		//we keep a view of the original MIDI events so we do not free them when destroying the list, they will be freed when the state is destroyed
	double clip_start = clip->start;
	double clip_end = clip->start + clip->duration;

	for(ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)){
		MidiEvent event = list_node_value(state->midi_file->events, node);

		if(event->type == MIDI_NOTE && event->channel == clip->channel && event->time >= clip_start && event->time <= clip_end)
			list_insert_next(clip_song, list_last(clip_song), event);
	}
	return clip_song;
}

//2.evaluates a completed clip by comparing the recorded events with the original song events
//matches recorded note_on events to song note_on events
//updates the score/accuracy metrics
static void evaluate_clip(State state, Clip clip){
	List clip_song = create_clip_song(state, clip);
	double clip_score = 0.0;
	int played_note_on_count = 0;
	int song_note_on_count = 0;
	
	//count song note_on events before matching 
	for(ListNode node = list_first(clip_song); node != LIST_EOF; node = list_next(clip_song, node)){
		MidiEvent event = list_node_value(clip_song, node);
		if(event->type == MIDI_NOTE && event->velocity > 0)
			song_note_on_count++;
	}
	//for every recorded note_on, find the first matching song note_on with the same key and a time distance at most 0.1 seconds
	for(ListNode rec_node = list_first(clip->recorded); rec_node !=  LIST_EOF; rec_node = list_next(clip->recorded, rec_node)){
		MidiEvent recorded_on = list_node_value(clip->recorded, rec_node);

		if(!(recorded_on->type == MIDI_NOTE && recorded_on->velocity > 0))
			continue;	
		played_note_on_count++;

		ListNode matched_song_node = LIST_EOF;
		for(ListNode song_node = list_first(clip_song); song_node != LIST_EOF; song_node = list_next(clip_song, song_node)){
			MidiEvent song_on = list_node_value(clip_song, song_node);
			if(!(song_on->type == MIDI_NOTE && song_on->velocity > 0))
				continue;
			if(song_on->key != recorded_on->key)
				continue;
			if(fabs(song_on->time - recorded_on->time) <= 0.1){
				matched_song_node = song_node;
				break;
			}	
		}
		if(matched_song_node == LIST_EOF)
			continue;
		
		MidiEvent song_on = list_node_value(clip_song, matched_song_node);

		//find the the corresponding note_off for the recorded note_on
		MidiEvent recorded_off = NULL;
		for(ListNode node = list_next(clip->recorded, rec_node); node != LIST_EOF; node = list_next(clip->recorded, node)){
			MidiEvent event = list_node_value(clip->recorded, node);
			if(event->type == MIDI_NOTE && event->key == recorded_on->key && event->velocity == 0){	
				recorded_off = event;
				break;
			}
		}

		//find the the corresponding note_off for the song note_on
		MidiEvent song_off = NULL;
		for(ListNode node = list_next(clip_song, matched_song_node); node != LIST_EOF; node = list_next(clip_song, node)){
			MidiEvent event = list_node_value(clip_song, node);
			if(event->type == MIDI_NOTE && event->key == song_on->key && event->velocity == 0){	
				song_off = event;
				break;
			}
		}
		double time_diff = fabs(recorded_on->time - song_on->time);
		double duration_diff = 0.0;
		if(recorded_off != NULL && song_off != NULL && clip->channel != 9){
			double recorded_duration = recorded_off->time - recorded_on->time;
			double song_duration = song_off->time - song_on->time;
			duration_diff = fabs(recorded_duration - song_duration);
		}
		double diff = time_diff + duration_diff;
		double note_score = exp(-pow(diff / 0.1, 2.0));	
		clip_score += note_score;

		//correct matched recorded events so that later we can use the original song timing instead of the player's mistakes
		recorded_on->time = song_on->time;
		if(recorded_off != NULL && song_off != NULL)
			recorded_off->time = song_off->time;
		
		ListNode prev = LIST_BOF;	//remove the matched song note_on so it is not matched again
		for(ListNode node = list_first(clip_song); node != LIST_EOF; node = list_next(clip_song, node)){
			if(node == matched_song_node){
				list_remove_next(clip_song, prev);
				break;
			}
			prev = node;
		}
	}
	double denominator = fmax(song_note_on_count, played_note_on_count);
	double clip_accuracy = 0.0;
	if(denominator > 0.0)
		clip_accuracy = clip_score / denominator;
	state->clips_score_sum += clip_score;
	state->clips_accuracy_sum += clip_accuracy;
	state->completed_clips++;
	state->info.score = state->clips_score_sum;
	state->info.accuracy = state->clips_accuracy_sum / state->completed_clips;
	list_destroy(clip_song);
}
//3.defines the minimum accuracy required for each level
//the threshold increases gradually 
static double minimum_accuracy_for_level(int level){
	double threshold = 0.5 + (level - 1)*0.03;
	if(threshold > 0.85)
		threshold = 0.85;
	return threshold;
}

/////////////////////////////////////////
////////////////////////////////////////
//bucket helpers 
static int compare_ints(Pointer a, Pointer b){ 
    int x = *(int*)a; 
    int y = *(int*)b; 
    if (x < y) return -1; 
    if (x > y) return 1; 
    return 0; 
} 

static void destroy_bucket_list(Pointer value){ 
    if(value != NULL) 
        list_destroy(value); 
} 

//converts a timestamp into a bucket index 
static int bucket_index(double time){ 
    if(time < 0.0) 
        time = 0.0; 
    return (int)floor(time / TIME_BUCKET); 
} 

//returns the list corresponding to a given index
//if it does not exist, it is created dynamically
static List get_or_create_bucket(Map map, int index){ 
    List bucket = map_find(map, &index); 
    if(bucket != NULL) 
        return bucket; 
    int* key = malloc(sizeof(*key)); 
    assert(key != NULL); 
    *key = index; 

    bucket = list_create(NULL); 
    map_insert(map, key, bucket); 
    return bucket; 
} 

//inserts an event into a bucket and preserves time order in it
static void insert_event_in_bucket(List bucket, MidiEvent event){ 
    ListNode prev = LIST_BOF; 
    for(ListNode node = list_first(bucket); node != LIST_EOF; node = list_next(bucket, node)){ 
        MidiEvent ev = list_node_value(bucket, node); 
        //keep each bucket sorted by time so range queries preserve order
        if(event->time < ev->time) 
            break; 
        prev = node; 
    } 
    list_insert_next(bucket, prev, event); 
} 

//index helpers 
static void index_displayed_note(State state, MidiEvent event){ 
    int idx = bucket_index(event->time); 
    List bucket = get_or_create_bucket(state->displayed_index, idx); 
    insert_event_in_bucket(bucket, event); 
} 

 //adds a playback event both to the global playback list and to the indexed structure
static void add_playback_event(State state, MidiEvent event){ 
    list_insert_next(state->midi_events, list_last(state->midi_events), event); 
    int idx = bucket_index(event->time); 
    List bucket = get_or_create_bucket(state->playback_index, idx); 
    insert_event_in_bucket(bucket, event); 
} 

 //builds the index for displayed notes once during initialization
 //reduces the cost of per-frame queries
static void build_displayed_index(State state){ 
    for(ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)){ 
        MidiEvent event = list_node_value(state->midi_file->events, node); 
        if(event->type == MIDI_NOTE) 
            index_displayed_note(state, event); 
    } 
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

	state->clips_score_sum = 0.0;
	state->clips_accuracy_sum = 0.0;
	state->completed_clips = 0;

	state->midi_file = NULL;
	state->midi_events = list_create(free);

	state->midi_file = k08midi_file_read(midi_file);
	assert(state->midi_file != NULL);

	create_clips(state);
	assert(vector_size(state->clips) > 0);
	state->recording_index = 0;

    state->displayed_index = map_create(compare_ints, free, destroy_bucket_list);
    state->playback_index = map_create(compare_ints, free, destroy_bucket_list);

    build_displayed_index(state);

	list_destroy(state->midi_events);
	state->midi_events = list_create(free);
	append_song_control_events(state);

	return state;
}

StateInfo state_info(State state) {
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
   
	Clip clip = vector_get_at(state->clips, state->recording_index);
	return clip->channel;
}

double state_measure_duration(State state) {
	MidiFile midi = state->midi_file;
	return midi->time_signature[0] * (60.0 / midi->tempo) * (4.0 / midi->time_signature[1]);
}

double state_total_duration(State state) {
	double total_duration = 0.0;
	for(ListNode node = list_first(state->midi_file->events); node != LIST_EOF; node = list_next(state->midi_file->events, node)){
		MidiEvent event = list_node_value(state->midi_file->events, node);
		if(event->time > total_duration)	//we do not sum the duration of each event because they might occure at the same time, we look at the time of the last event
			total_duration = event->time;
	}
	return total_duration;
}

List state_displayed_notes(State state, double time_window) {
	assert(state != NULL);
	//return a view of the original MIDI events
	List displayed_notes = list_create(NULL);	//the events will be freed when the state is destroyed
	double current_time= state->info.time;
	double end_time= current_time + time_window;

    int start_bucket = bucket_index(current_time); 
    int end_bucket = bucket_index(end_time);

    //we scan the buckets that are in the visible time window 
    //order is preserved since we insert the events in the buckets sorted by time 
    for(int b = start_bucket; b <= end_bucket; b++){ 
        List bucket = map_find(state->displayed_index, &b); 
        if(bucket == NULL) 
            continue; 
        for(ListNode node = list_first(bucket); node != LIST_EOF; node = list_next(bucket, node)){ 
            MidiEvent event = list_node_value(bucket, node); 
            if(event->time >= current_time && event->time <= end_time) 
                list_insert_next(displayed_notes, list_last(displayed_notes), event); 
        } 

    } 
	return displayed_notes;
}

List state_playback_events(State state, double since){
	assert(state != NULL);
	//return a view of the playback MIDI events
	List playback_events = list_create(NULL);
	double current_time = state->info.time;
	double start_time = current_time - since;
    if(start_time < 0.0) 
        start_time = 0.0; 

    int start_bucket = bucket_index(start_time); 
    int end_bucket = bucket_index(current_time); 

    //we scan only the buckets that inersect the palyback window 
    //order is preserved since we insert the events in the buckets sorted by time 
    for(int b = start_bucket; b <= end_bucket; b++){ 
        List bucket = map_find(state->playback_index, &b); 
        if(bucket == NULL) 
            continue; 
        for(ListNode node = list_first(bucket); node != LIST_EOF; node = list_next(bucket, node)){ 
            MidiEvent event = list_node_value(bucket, node); 
            if(event->time >= start_time && event->time <= current_time) 
                list_insert_next(playback_events, list_last(playback_events), event); 
        } 
    } 
	return playback_events;
}

void state_update(State state, KeyState ks, double elapsed_time) {
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
	if(!state->info.paused || step)
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
				event->velocity = ks->active_keys[key];	//velocity is 0 for note off and >0 for note on
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
		evaluate_clip(state, clip);	//evaluate the completed clip and update the score/accuracy metrics

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
					add_playback_event(state, clone);	//insert the event to the playback timeline
				}
			}
		}
		//go to next clip unless we are at the last one
		if(state->recording_index +1 < vector_size(state->clips))
			state->recording_index++;
		
		//end of song
		if(state->info.time >= state->midi_file->duration){
			if(state->info.accuracy >= minimum_accuracy_for_level(state->info.level)){
				//advance level and restart game state
				state->info.level++;
				state->info.time = 0.0;
				state->info.paused = true;
				state->info.game_over = false;
				state->info.score = 0.0;
				state->info.accuracy = 0.0;

				state->clips_score_sum = 0.0;
				state->clips_accuracy_sum = 0.0;
				state->completed_clips = 0;
				state->recording_index = 0;

				//rebuild the playbacl timeline for the new run
				list_destroy(state->midi_events);
				state->midi_events = list_create(free);

                map_destroy(state->playback_index);
                state->playback_index = map_create(compare_ints, free, destroy_bucket_list);

                append_song_control_events(state);

				//erase the recorded clips
				for(int ci = 0; ci < vector_size(state->clips); ci++){
					Clip clip = vector_get_at(state->clips, ci);
					list_destroy(clip->recorded);
					clip->recorded = list_create(free);
				}

			}
			else{
				//song ended but player did not reach the minimum accuracy, game over
				state->info.game_over = true;
				state->info.paused = true;
			}
		}
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
    if(state->displayed_index != NULL)
        map_destroy(state->displayed_index);
    if(state->playback_index != NULL)
        map_destroy(state->playback_index);
	free(state);	//free the struct itself
}