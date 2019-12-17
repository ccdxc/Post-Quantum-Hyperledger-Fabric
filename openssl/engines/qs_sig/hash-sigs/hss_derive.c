#include <string.h>
#include "hss_derive.h"
#include "hss_internal.h"
#include "hash.h"
#include "endian.h"

#if SEED_LEN == 32
#define HASH HASH_SHA256  /* We always use SHA-256 to derive seeds */
#else
#error We need to define a hash function for this seed length
#endif

#if SECRET_METHOD == 0
/*
 * This is the method of deriving LM-OTS keys that conforms to the
 * Appendix A method
 * As you can see, it's fairly simple
 */

/* This creates a seed derivation object */
bool hss_seed_derive_init( struct seed_derive *derive,
                 param_set_t lm, param_set_t ots,
                 const unsigned char *I, const unsigned char *seed ) {
    derive->I = I;
    derive->master_seed = seed;
    /* q, j will be set later */

    return true;
}

/* This sets the internal 'q' value for seed derivation object */
void hss_seed_derive_set_q( struct seed_derive *derive, merkle_index_t q ) {
    derive->q = q;
}

/* This sets the internal 'j' value for seed derivation object */
void hss_seed_derive_set_j( struct seed_derive *derive, unsigned j ) {
    derive->j = j;
}


/* This derives the current seed value.  If increment_j is set, it'll then */
/* reset the object to the next j value */
void hss_seed_derive( unsigned char *seed, struct seed_derive *derive,
                 bool increment_j ) {
    unsigned char buffer[ PRG_MAX_LEN ];
    memcpy( buffer + PRG_I, derive->I, I_LEN );
    put_bigendian( buffer + PRG_Q, derive->q, 4 );
    put_bigendian( buffer + PRG_J, derive->j, 2 );
    buffer[PRG_FF] = 0xff;
    memcpy( buffer + PRG_SEED, derive->master_seed, SEED_LEN );

    hss_hash( seed, HASH, buffer, PRG_LEN(SEED_LEN) );

    hss_zeroize( buffer, PRG_LEN(SEED_LEN) );

    if (increment_j) derive->j += 1;
}

/* This is called when we're done with a seed derivation object */
void hss_seed_derive_done( struct seed_derive *derive ) {
    /* No secrets here */
}

#elif SECRET_METHOD == 1
/*
 * This is a method of deriving LM-OTS keys that tries to be more
 * side-channel resistant; in particular, we never include any
 * specific secret value in more than 2**SECRET_MAX distinct
 * hashes.
 * We do this by deriving subseeds using a tree-based structure;
 * each node in the tree has up to 2**SECRET_MAX children, and we use any
 * seed within the node (including the root) in no other hash.
 * We actually have two levels of trees; one based on q (Merkle tree index),
 * the other based on j (Winternitz digit); we could design a single level
 * tree that could incorporate both, but it'd be more complex
 *
 * Much of the complexity that does exist is there to avoid recomputation
 */
#include "lm_common.h"
#include "lm_ots_common.h"
static unsigned my_log2(merkle_index_t n);

/* This creates a seed derivation object */
bool hss_seed_derive_init( struct seed_derive *derive,
                 param_set_t lm, param_set_t ots,
                 const unsigned char *I, const unsigned char *seed ) {
    derive->I = I;
    derive->master_seed = seed;

    /* These parameter sets will define the size of the trees we'll use */
    unsigned height, p;
    if (!lm_look_up_parameter_set(lm, 0, 0, &height) ||
        !lm_ots_look_up_parameter_set(ots, 0, 0, 0, &p, 0)) {
        return false;
    }

    p += NUM_ARTIFICIAL_SEEDS; /* We use one artifical value for the */
         /* randomizer and two artificial values to generate seed, I */
         /* for child trees */

    /* Compute the number of r-levels we have */
    derive->q_levels = (height + SECRET_MAX - 1)/SECRET_MAX;

    /* And which bit to set when converting 'q' to 'r' */
    derive->r_mask = (merkle_index_t)1 << height;

    /* Compute the number of j-levels we have */
    unsigned j_height = my_log2(p);
    derive->j_levels = (j_height + SECRET_MAX - 1)/SECRET_MAX;

    /* And which bit to set when writing q values into the hash */
    derive->j_mask = 1 << j_height;

    /* We reset the current 'q' value to am impossible value; we do this so */
    /* that the initial 'q' value given to use by the application will */
    /* rebuild the entire path through the tree */
    derive->q = derive->r_mask;

    return true;
}

/* This sets the internal 'q' value for seed derivation object */
/* This also updates our internal q-path (the q_index/q_seed arrays) */
/* to reflect the new 'q' value, while minimizing the number of hashes */
/* done (by reusing as much of the previous path as possible) */
void hss_seed_derive_set_q( struct seed_derive *derive, merkle_index_t q ) {
    merkle_index_t change = q ^ derive->q;
    derive->q = q;
    unsigned bits_change = my_log2(change);
    unsigned q_levels = derive->q_levels;

        /* levels_change will be the number of levels of the q-tree we'll */
        /* need to recompute */
    unsigned levels_change = (bits_change + SECRET_MAX - 1) / SECRET_MAX;
    if (levels_change > q_levels) levels_change = q_levels;

    int i;
    union hash_context ctx;
    unsigned char buffer[ QTREE_MAX_LEN ];
    merkle_index_t r = q | derive->r_mask;

    for (i = levels_change; i > 0; i--) {
        int j = q_levels - i;
        int shift = (i-1) * SECRET_MAX;

        memcpy( buffer + QTREE_I, derive->I, I_LEN );
        put_bigendian( buffer + QTREE_Q, r >> shift, 4 );
        SET_D( buffer + QTREE_D, D_QTREE );
        if (j == 0) {
            memcpy( buffer + QTREE_SEED, derive->master_seed, SEED_LEN );
        } else {
            memcpy( buffer + QTREE_SEED, derive->q_seed[j-1], SEED_LEN );
        }

        hss_hash_ctx( derive->q_seed[j], HASH, &ctx, buffer, QTREE_LEN );
    }

    hss_zeroize( buffer, PRG_LEN(SEED_LEN) );
    hss_zeroize( &ctx, sizeof ctx );
}

/* Helper function to recompute the j_seed[i] value, based on the */
/* j_value[i] already set */
/* ctx, buffer are passed are areas this function can use; we reuse those */
/* areas so we need to zeroize those buffers only once */
static void set_j_seed( struct seed_derive *derive, int i,
                        union hash_context *ctx, unsigned char *buffer) {

    memcpy( buffer + PRG_I, derive->I, I_LEN );
    put_bigendian( buffer + PRG_Q, derive->q, 4 );
    put_bigendian( buffer + PRG_J, derive->j_value[i], 2 );
    buffer[PRG_FF] = 0xff;
    if (i == 0) {
        /* The root of this tree; it gets its seed from the bottom level */
        /* of the q-tree */
        memcpy( buffer + PRG_SEED, derive->q_seed[ derive->q_levels-1],
                                                         SEED_LEN );
    } else {
        /* Non-root node; it gets its seed from its parent */
        memcpy( buffer + PRG_SEED, derive->j_seed[i-1], SEED_LEN );
    }

    hss_hash_ctx( derive->j_seed[i], HASH, ctx, buffer, PRG_LEN(SEED_LEN) );
}

/* This sets the internal 'j' value for seed derivation object */
/* This computes the entire path to the 'j' value.  Because this is used */
/* immediately after resetting the q value, we don't try to reuse the */
/* previous hashes (as there won't be anything there we could reuse) */
/* Note that we don't try to take advantage of any preexisting hashes */
/* in the j_seed array; we don't bother because this function is typically */
/* used only immediately after a set_q call, and so there aren't any */
/* hashes we could take advantage of */
void hss_seed_derive_set_j( struct seed_derive *derive, unsigned j ) {
    int i;
    unsigned j_levels = derive->j_levels;
    unsigned shift = SECRET_MAX * j_levels;

    unsigned j_mask = derive->j_mask;
    j &= j_mask-1; /* Set the high-order bit; clear any bits above that */
    j |= j_mask;  /* This ensures that when we do the hashes, that the */
                  /* prefix for the hashes at two different levels of the */
                  /* tree are distinct */

    union hash_context ctx;
    unsigned char buffer[ PRG_MAX_LEN ];

    for (i = 0; i<j_levels; i++ ) {
        shift -= SECRET_MAX;
        derive->j_value[i] = (j >> shift);
        set_j_seed( derive, i, &ctx, buffer );
    }

    hss_zeroize( &ctx, sizeof ctx );
    hss_zeroize( buffer, PRG_LEN(SEED_LEN) );
}

/* This derives the current seed value (actually, we've already computed */
/* it); we just need to copy it to the buffer) */
/* If increment_j is set, it'll then reset the object to the next j value */
/* (which means incrementally computing that path) */
void hss_seed_derive( unsigned char *seed, struct seed_derive *derive,
                      bool increment_j ) {
    memcpy( seed, derive->j_seed[ derive->j_levels - 1], SEED_LEN );

    if (increment_j) {
        int i;

        /* Update the j_values, and figure out which hashes we'll need */
        /* to recompute */
        for (i = derive->j_levels-1;; i--) {
            unsigned index = derive->j_value[i];
            index += 1;
            derive->j_value[i] = index;
            if (0 != (index & SECRET_MAX_MASK)) {
                /* The increment didn't cause a carry to the next level; */
                /* we can stop propogating the increment here (and we */
                /* also know this is the top level that we need to */
                /* recompute the hashes */
                break;
            }
            if (i == 0) {
               /* This is the top level; stop here */
               break;
            }
        }

        /* Recompute the hashes that need updating; we need to do it */
        /* top-down, as each hash depends on the previous one */
        union hash_context ctx;
        unsigned char buffer[ PRG_MAX_LEN ];
        for (; i < derive->j_levels; i++) {
            set_j_seed( derive, i, &ctx, buffer );
        }
        hss_zeroize( &ctx, sizeof ctx );
        hss_zeroize( buffer, PRG_LEN(SEED_LEN) );
    }
}

/* This is called when we're done with a seed derivation object */
/* This makes sure any secret values are zeroized */
void hss_seed_derive_done( struct seed_derive *derive ) {
    /* These values are secret, and should never be leaked */
    hss_zeroize( derive->q_seed, sizeof derive->q_seed );
    hss_zeroize( derive->j_seed, sizeof derive->j_seed );
}

static unsigned my_log2(merkle_index_t n) {
    unsigned lg;
    for (lg = 0; n > 0; lg++) n >>= 1;
    return lg;
}

#else

#error Unknown secret method

#endif

