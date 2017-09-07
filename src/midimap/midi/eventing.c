#include "midi/eventing.h"

static int note_owners[129] = {0};
static int active_lookup[129] = {0};

static void release_mapping(mm_midi_output* output, mm_key_set* dst_set) {
    for (int i = 0; i < dst_set->count; i++) {
        mm_keylet* k = dst_set->keys[i];
        mm_send_midi(output, k->key, false, k->ch, 0);
    }
}
static void trigger_mapping(mm_midi_output* output, mm_key_set* dst_set,
                            int vel) {
    for (int i = 0; i < dst_set->count; i++) {
        mm_keylet* k = dst_set->keys[i];
        mm_send_midi(output, k->key, true, k->ch, vel);
    }
}

int mm_event_process(mm_midi_output* output, mm_options* options,
                     mm_key_node* list, mm_key_set** active_keyset, int midi,
                     int chan, int vel, int note_on) {

    int process_event = 0;
    mm_key_set* dsts_set = *active_keyset;
    mm_key_group* grp = options->mapping->index[midi];

    // acquire ownership of self key
    if (note_owners[midi] < 0) {
        mm_debug("initialising note_owners[midi] = %d\n", midi);
        note_owners[midi] = midi;

    } else if (note_owners[midi] != midi) {
        mm_debug("could not initialise note_owners[midi] because: "
                 "%d != %d\n",
                 note_owners[midi], midi);
    }

    if (grp != NULL) {
        // mm_debug("event_loop: mappings for event "
        //          "(midi:%d, note_on: %d, note_off: %d)\n",
        //          midi, note_on, note_off);

        // mm_key_set* new_keys =
        //     mm_mapping_group_single_src_dsts(grp);

        mm_key_set* new_keys =
            mm_mapping_group_all_dsts(grp, list, note_on, chan);

        char* key_dump = mm_key_set_dump(new_keys);

        mm_debug("[%d]key(%d)\n%s\n", chan, midi, key_dump);
        free(key_dump);

        if (note_on) {
            int remove_hits[129] = {0};

            for (int i = 0; i < new_keys->count; ++i) {
                // set the latest owner to `midi` if it's available
                int k = new_keys->keys[i]->key;

                if (active_lookup[k]) {
                    // mark key for pre-emptive removal due to it
                    // being active already
                    remove_hits[k] = 5;
                } else {
                    note_owners[k] = midi;
                }
            }

            // stage 2 hit removals
            for (int i = 0; i < 129; ++i) {
                if (remove_hits[i] == 5) {
                    remove_hits[i] = 0;
                    mm_debug("removing single_key due to active "
                             "hit: %d\n",
                             i);
                    mm_key_set_remove_single_key(new_keys, i);
                }
            }

            if (!active_lookup[midi] && new_keys->count > 0) {
                // dont process default event if we have mapped a
                // key
                process_event = 0;
            }

            trigger_mapping(output, new_keys, vel);
            mm_combine_key_set(dsts_set, new_keys);

        } else if (!note_on) {

            if (!active_lookup[midi] || new_keys->count > 0) {
                // dont process default event if we have mapped a
                // key
                process_event = 0;
            }

            if (!active_lookup[midi]) {
                process_event = 0;
            }

            int keys_to_release_deeped = 0;
            mm_key_set* release_keys = new_keys;

            for (int i = 0; i < new_keys->count; ++i) {
                int k = new_keys->keys[i]->key;
                int owner = note_owners[k];
                int active = mm_key_node_contains(list, k);

                // relinquish ownership if `midi` still owns it
                if (owner == midi && !active) {
                    note_owners[k] = -1;
                } else {

                    if (active) {
                        // if active, set the owner to `k` itself,
                        // as `list` query will only indicate as
                        // such.
                        note_owners[k] = k;
                    }

                    // we don't own the key, don't release_mapping
                    // ye. ergo we must remove frm
                    // `release_keys`

                    // But first, we need to turn keys_to_release
                    // into a deep copy.
                    if (!keys_to_release_deeped) {
                        release_keys = mm_key_set_copy(new_keys);

                        // we're done, flip the bit.
                        keys_to_release_deeped = 1;
                    }

                    mm_key_set_remove_single_key(release_keys, k);
                }

                // Check if the owner is a virtual key, in which we
                // will not remove it from the dsts_set.
                if (owner >= 0 && options->mapping->index[owner] == NULL) {
                    // remove from virtual dsts_set
                    mm_key_set_remove_single_key(dsts_set, k);
                }
            }

            release_mapping(output, release_keys);

            mm_remove_key_set(dsts_set, new_keys);

            mm_keylets_free(new_keys->keys, new_keys->count);
            free(new_keys);

            // If we have deep copied new_keys, or removed a key
            // from it, we need to free the new copy.
            if (new_keys != release_keys) {
                mm_keylets_free(release_keys->keys, release_keys->count);

                free(release_keys);
                release_keys = NULL;
            }

            new_keys = NULL;
        }

    } else {
        if (!note_on) {
            // apply checks to determine if we can release or not,
            // due to any other active mappings at this moment.
            if (note_owners[midi] != midi) {
                mm_debug("cannot release note_off because: %d != %d\n", midi,
                         note_owners[midi]);
                process_event = 0;
            } else {
                note_owners[midi] = -1;
                mm_debug("released note_owners[midi] = -1\n");
            }

            active_lookup[midi] = 0;
        }
    }

    active_lookup[midi] = note_on;
    *active_keyset = dsts_set;

    // Return the status flag to further process this event or not.
    return process_event;
}