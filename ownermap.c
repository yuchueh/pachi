#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "board.h"
#include "debug.h"
#include "move.h"
#include "mq.h"
#include "ownermap.h"

void
ownermap_init(struct ownermap *ownermap)
{
	memset(ownermap, 0, sizeof(*ownermap));
}

static void
printhook(struct board *board, coord_t c, strbuf_t *buf, void *data)
{
        struct ownermap *ownermap = data;

	if (c == pass) { /* Stuff to display in header */
		if (!ownermap || !ownermap->playouts) return;
		sbprintf(buf, "Score Est: %s", ownermap_score_est_str(board, ownermap));
		return;
	}

        if (!ownermap) {
		sbprintf(buf, ". ");
		return;
        }
        const char chr[] = ":XO,"; // dame, black, white, unclear
        const char chm[] = ":xo,";
        char ch = chr[ownermap_judge_point(ownermap, c, GJ_THRES)];
        if (ch == ',') { // less precise estimate then?
                ch = chm[ownermap_judge_point(ownermap, c, 0.67)];
        }
        sbprintf(buf, "%c ", ch);
}

void
board_print_ownermap(struct board *b, FILE *f, struct ownermap *ownermap)
{
        board_print_custom(b, stderr, printhook, ownermap);
}

void
ownermap_fill(struct ownermap *ownermap, struct board *b)
{
	ownermap->playouts++;
	foreach_point(b) {
		enum stone color = board_at(b, c);
		if (color == S_NONE)
			color = board_get_one_point_eye(b, c);
		ownermap->map[c][color]++;
	} foreach_point_end;
}

void
ownermap_merge(int bsize2, struct ownermap *dst, struct ownermap *src)
{
	dst->playouts += src->playouts;
	for (int i = 0; i < bsize2; i++)
		for (int j = 0; j < S_MAX; j++)
			dst->map[i][j] += src->map[i][j];
}

float
ownermap_estimate_point(struct ownermap *ownermap, coord_t c)
{
	assert(ownermap->map);
	assert(!is_pass(c));
	int b = ownermap->map[c][S_BLACK];
	int w = ownermap->map[c][S_WHITE];
	int total = ownermap->playouts;
	return 1.0 * (b - w) / total;
}

enum point_judgement
ownermap_judge_point(struct ownermap *ownermap, coord_t c, floating_t thres)
{
	assert(ownermap->map);
	assert(!is_pass(c));
	int n = ownermap->map[c][S_NONE];
	int b = ownermap->map[c][S_BLACK];
	int w = ownermap->map[c][S_WHITE];
	int total = ownermap->playouts;
	if (n >= total * thres)
		return PJ_DAME;
	else if (n + b >= total * thres)
		return PJ_BLACK;
	else if (n + w >= total * thres)
		return PJ_WHITE;
	else
		return PJ_UNKNOWN;
}

enum stone
ownermap_color(struct ownermap *ownermap, coord_t c, floating_t thres)
{
	enum stone colors[4] = {S_NONE, S_BLACK, S_WHITE, S_NONE };
	enum point_judgement pj = ownermap_judge_point(ownermap, c, thres);
	return colors[pj];
}

void
ownermap_judge_groups(struct board *b, struct ownermap *ownermap, struct group_judgement *judge)
{
	assert(ownermap->map);
	assert(judge->gs);
	memset(judge->gs, GS_NONE, board_size2(b) * sizeof(judge->gs[0]));

	foreach_point(b) {
		enum stone color = board_at(b, c);
		group_t g = group_at(b, c);
		if (!g) continue;

		enum point_judgement pj = ownermap_judge_point(ownermap, c, judge->thres);
		// assert(judge->gs[g] == GS_NONE || judge->gs[g] == pj);
		if (pj == PJ_UNKNOWN) {
			/* Fate is uncertain. */
			judge->gs[g] = GS_UNKNOWN;

		} else if (judge->gs[g] != GS_UNKNOWN) {
			/* Update group state. */
			enum gj_state new;

			// Comparing enum types, casting (int) avoids compiler warnings
			if ((int)pj == (int)color) { 
				new = GS_ALIVE;
			} else if ((int)pj == (int)stone_other(color)) {
				new = GS_DEAD;
			} else { assert(pj == PJ_DAME);
				/* Exotic! */
				new = GS_UNKNOWN;
			}

			if (judge->gs[g] == GS_NONE) {
				judge->gs[g] = new;
			} else if (judge->gs[g] != new) {
				/* Contradiction. :( */
				judge->gs[g] = GS_UNKNOWN;
			}
		}
	} foreach_point_end;
}

void
groups_of_status(struct board *b, struct group_judgement *judge, enum gj_state s, struct move_queue *mq)
{
	foreach_point(b) { /* foreach_group, effectively */
		group_t g = group_at(b, c);
		if (!g || g != c) continue;

		assert(judge->gs[g] != GS_NONE);
		if (judge->gs[g] == s)
			mq_add(mq, g, 0);
	} foreach_point_end;
}

enum point_judgement
ownermap_score_est_coord(struct board *b, struct ownermap *ownermap, coord_t c)
{
	enum point_judgement j = ownermap_judge_point(ownermap, c, 0.67);
	enum stone s = board_at(b, c);
	
	/* If status is unclear and there's a stone there assume it's alive. */
	if (j != PJ_BLACK && j != PJ_WHITE && (s == S_BLACK || s == S_WHITE))
		return (enum point_judgement)s;
	return j;
}

float
ownermap_score_est(struct board *b, struct ownermap *ownermap)
{
	float scores[S_MAX] = {0.0, };  /* Number of points owned by each color */
	foreach_point(b) {
		enum point_judgement j = ownermap_score_est_coord(b, ownermap, c);
		scores[j]++;
	} foreach_point_end;

	int handi_comp = board_score_handicap_compensation(b);
	return ((scores[PJ_WHITE] + b->komi + handi_comp) - scores[PJ_BLACK]);
}

float
ownermap_score_est_color(struct board *b, struct ownermap *ownermap, enum stone color)
{
	floating_t score = ownermap_score_est(b, ownermap);
	return (color == S_BLACK ? -score : score);
}

/* Returns static buffer */
char *
ownermap_score_est_str(struct board *b, struct ownermap *ownermap)
{
	static char buf[32];
	float s = ownermap_score_est(b, ownermap);
	sprintf(buf, "%s+%.1f\n", (s > 0 ? "W" : "B"), fabs(s));
	return buf;
}
