#include <pebble.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "allowed_words_index.h"
#include "answers_metadata.h"

#define WORD_LENGTH 5
#define MAX_GUESSES 6
#define HARD_MODE_MESSAGE_KEY 0

// ------------------------------------------------------------
// Game state
// ------------------------------------------------------------

typedef enum {
	TILE_EMPTY,
	TILE_NORMAL,
	TILE_CORRECT,
	TILE_PRESENT,
	TILE_ABSENT
} TileState;

static Window *s_window;
static Layer *s_board_layer;
static Layer *s_alphabet_layer;
static TextLayer *s_selector_layer;
static Layer *s_results_layer;

static char s_answer[WORD_LENGTH + 1];
static char s_guesses[MAX_GUESSES][WORD_LENGTH + 1];
static TileState s_states[MAX_GUESSES][WORD_LENGTH];
static TileState s_alphabet_states[26];

static int s_current_row = 0;
static int s_current_col = 0;
static char s_current_letter = 'A';
static bool s_game_over = false;
static bool s_won = false;

static AppTimer *s_scroll_timer = NULL;
static AppTimer *s_invalid_word_timer = NULL;
static int s_scroll_direction = 0;
static int s_scroll_delay = 180;

typedef enum {
	SCREEN_GAME,
	SCREEN_RESULTS
} Screen;

typedef struct {
	uint16_t games_played;
	uint16_t games_won;
	uint16_t wins_by_row[MAX_GUESSES];
	uint16_t current_streak;
	uint16_t best_streak;
} GameStats;

static GameStats s_stats;
static Screen s_screen = SCREEN_GAME;
#define STATS_KEY 1
#define HARD_MODE_KEY 3

static bool s_hard_mode = false;

//Daily game state

#define DAILY_GAME_KEY 2

typedef struct {
	int32_t game_day;

	char answer[WORD_LENGTH + 1];

	char guesses[MAX_GUESSES][WORD_LENGTH + 1];

	TileState states[MAX_GUESSES][WORD_LENGTH];

	TileState alphabet_states[26];

	int8_t current_row;
	int8_t current_col;

	char current_letter;

	bool game_over;
	bool won;
} DailyGameState;

static DailyGameState s_daily_game;

// ------------------------------------------------------------
// Utility
// ------------------------------------------------------------

static uint32_t pack_word(const char *word) {
	uint32_t packed = 0;

	for (int i = 0; i < WORD_LENGTH; i++) {
		char letter = (char)toupper((unsigned char)word[i]);
		packed = (packed << 5) | (uint32_t)(letter - 'A');
	}

	return packed;
}

static void unpack_word(uint32_t packed, char output[WORD_LENGTH + 1]) {
	for (int i = WORD_LENGTH - 1; i >= 0; i--) {
		output[i] = (char)('A' + (packed & 0x1F));
		packed >>= 5;
	}

	output[WORD_LENGTH] = '\0';
}

static bool read_packed_word(ResHandle resource, uint16_t index,
		uint32_t *packed) {
	uint8_t bytes[PACKED_WORD_SIZE];
	size_t bytes_read = resource_load_byte_range(
		resource,
		(uint32_t)index * PACKED_WORD_SIZE,
		bytes,
		PACKED_WORD_SIZE
	);

	if (bytes_read != PACKED_WORD_SIZE) {
		return false;
	}

	// The converter always writes records in big-endian byte order.
	*packed = ((uint32_t)bytes[0] << 24) |
		((uint32_t)bytes[1] << 16) |
		((uint32_t)bytes[2] << 8) |
		(uint32_t)bytes[3];
	return true;
}

static void choose_new_word(void) {
	uint16_t index = (uint16_t)(rand() % ANSWER_WORD_COUNT);
	uint32_t packed;
	ResHandle answers = resource_get_handle(RESOURCE_ID_ANSWERS);

	if (read_packed_word(answers, index, &packed)) {
		unpack_word(packed, s_answer);
	} else {
		APP_LOG(APP_LOG_LEVEL_ERROR, "Could not read answer %u", (unsigned int)index);
		strcpy(s_answer, "ABACK");
	}

	memset(s_guesses, 0, sizeof(s_guesses));
	memset(s_states, 0, sizeof(s_states));
	memset(s_alphabet_states, 0, sizeof(s_alphabet_states));

	s_current_row = 0;
	s_current_col = 0;
	s_current_letter = 'A';
	s_game_over = false;
	s_won = false;
}

static bool is_guess_complete(void) {
	return s_current_col == WORD_LENGTH;
}

static void load_stats(void) {
	if (persist_exists(STATS_KEY)) {
		persist_read_data(
		STATS_KEY,
		&s_stats,
		sizeof(s_stats)
		);
	}
	else {
		memset(&s_stats, 0, sizeof(s_stats));
	}
}

static void save_stats(void) {
	persist_write_data(
	STATS_KEY,
	&s_stats,
	sizeof(s_stats)
	);
}

static int32_t get_current_day(void) {
	time_t now = time(NULL);

	struct tm *local_time = localtime(&now);

	if (local_time == NULL) {
	return 0;
}

// Number representing the calendar day.
// The exact absolute value doesn't matter. We only need it to change when the local date changes.

return local_time->tm_year * 366
+ local_time->tm_yday;
}

static void save_daily_game(void) {
	s_daily_game.game_day = get_current_day();

	strcpy(
	s_daily_game.answer,
	s_answer
	);

	memcpy(
		s_daily_game.guesses,
		s_guesses,
		sizeof(s_guesses)
	);

	memcpy(
		s_daily_game.states,
		s_states,
		sizeof(s_states)
	);

	memcpy(
		s_daily_game.alphabet_states,
		s_alphabet_states,
		sizeof(s_alphabet_states)
	);

	s_daily_game.current_row = s_current_row;
	s_daily_game.current_col = s_current_col;
	s_daily_game.current_letter = s_current_letter;

	s_daily_game.game_over = s_game_over;
	s_daily_game.won = s_won;

	persist_write_data(
		DAILY_GAME_KEY,
		&s_daily_game,
		sizeof(s_daily_game)
		);
}

static bool load_daily_game(void) {
	if (!persist_exists(DAILY_GAME_KEY)) {
		return false;
	}

	persist_read_data(
		DAILY_GAME_KEY,
		&s_daily_game,
		sizeof(s_daily_game)
	);

	// Check whether this saved game belongs to today.
	if (s_daily_game.game_day != get_current_day()) {
		return false;
	}

	strcpy(
		s_answer,
		s_daily_game.answer
	);

	memcpy(
		s_guesses,
		s_daily_game.guesses,
		sizeof(s_guesses)
	);

	memcpy(
		s_states,
		s_daily_game.states,
		sizeof(s_states)
	);

	memcpy(
		s_alphabet_states,
		s_daily_game.alphabet_states,
		sizeof(s_alphabet_states)
	);

	s_current_row = s_daily_game.current_row;
	s_current_col = s_daily_game.current_col;
	s_current_letter = s_daily_game.current_letter;

	s_game_over = s_daily_game.game_over;
	s_won = s_daily_game.won;

	return true;
}

//Recording Wins and Losses!

static void record_win(int row) {
	s_stats.games_played++;
	s_stats.games_won++;
	s_won = true;

	if (row >= 0 && row < MAX_GUESSES) {
	s_stats.wins_by_row[row]++;
	}

	s_stats.current_streak++;

	if (s_stats.current_streak > s_stats.best_streak) {
	s_stats.best_streak = s_stats.current_streak;
	}

	save_stats();
}

static void record_loss(void) {
	s_stats.games_played++;
	s_won = false;

	// Losing breaks the current streak.
	s_stats.current_streak = 0;

save_stats();
}

// ------------------------------------------------------------
// Word checking
// ------------------------------------------------------------

static bool word_is_in_wordlist(const char *word) {
	if (!word || word[0] == '\0') {
		return false;
	}

	char first_letter = (char)toupper((unsigned char)word[0]);
	if (first_letter < 'A' || first_letter > 'Z') {
		return false;
	}

	uint32_t target = pack_word(word);
	uint16_t prefix = (uint16_t)(first_letter - 'A');
	uint16_t low = ALLOWED_WORD_LETTER_START[prefix];
	uint16_t high = ALLOWED_WORD_LETTER_START[prefix + 1];
	ResHandle allowed_words = resource_get_handle(RESOURCE_ID_ALLOWED_WORDS);

	// Binary-search the alphabetically sorted range for the first letter.
	while (low < high) {
		uint16_t middle = (uint16_t)(low + (high - low) / 2);
		uint32_t candidate;

		if (!read_packed_word(allowed_words, middle, &candidate)) {
			APP_LOG(APP_LOG_LEVEL_ERROR, "Could not read allowed word %u",
				(unsigned int)middle);
			return false;
		}

		if (candidate == target) {
			return true;
		}

		if (candidate < target) {
			low = (uint16_t)(middle + 1);
		} else {
			high = middle;
		}
	}

	return false;
}

static void evaluate_guess(int row) {
	char answer_copy[WORD_LENGTH + 1];

	strcpy(answer_copy, s_answer);

	// First pass: exact matches.
	for (int i = 0; i < WORD_LENGTH; i++) {
		if (s_guesses[row][i] == answer_copy[i]) {
		s_states[row][i] = TILE_CORRECT;
		answer_copy[i] = '-';
}
else {
		s_states[row][i] = TILE_NORMAL;
	}
}

// Second pass: letters present elsewhere.
for (int i = 0; i < WORD_LENGTH; i++) {
	if (s_states[row][i] == TILE_CORRECT) {
		continue;
	}

	bool found = false;

for (int j = 0; j < WORD_LENGTH; j++) {
	if (answer_copy[j] == s_guesses[row][i]) {
		found = true;
		answer_copy[j] = '-';
		break;
		}
	}

	s_states[row][i] = found ? TILE_PRESENT : TILE_ABSENT;
	}
}

static void update_alphabet(int row) {
	for (int i = 0; i < WORD_LENGTH; i++) {
	char letter = s_guesses[row][i];

	if (letter < 'A' || letter > 'Z') {
		continue;
	}

	int index = letter - 'A';
	TileState new_state = s_states[row][i];

	// Keep the strongest result we've discovered.
	//
	// CORRECT > PRESENT > ABSENT
	//
	// A correct result should never be downgraded
	// to yellow or gray by a later guess.

	if (new_state == TILE_CORRECT) {
	s_alphabet_states[index] = TILE_CORRECT;
	}
	else if (new_state == TILE_PRESENT) {
		if (s_alphabet_states[index] != TILE_CORRECT) {
		s_alphabet_states[index] = TILE_PRESENT;
		}
	}
	else if (new_state == TILE_ABSENT) {
		if (s_alphabet_states[index] == TILE_EMPTY) {
		s_alphabet_states[index] = TILE_ABSENT;
			}
		}
	}
}

static bool guess_is_correct(void) {
	return strcmp(s_guesses[s_current_row], s_answer) == 0;
}

static bool guess_obeys_hard_mode(int row) {
	if (!s_hard_mode || row <= 0) {
		return true;
	}

	char required_positions[WORD_LENGTH] = {0};
	uint8_t minimum_counts[26] = {0};

	for (int previous_row = 0; previous_row < row; previous_row++) {
		uint8_t row_hit_counts[26] = {0};

		for (int column = 0; column < WORD_LENGTH; column++) {
			TileState state = s_states[previous_row][column];
			char letter = s_guesses[previous_row][column];

			if (state == TILE_CORRECT) {
				required_positions[column] = letter;
			}

			if ((state == TILE_CORRECT || state == TILE_PRESENT) &&
				letter >= 'A' && letter <= 'Z') {
				row_hit_counts[letter - 'A']++;
			}
		}

		for (int letter = 0; letter < 26; letter++) {
			if (row_hit_counts[letter] > minimum_counts[letter]) {
				minimum_counts[letter] = row_hit_counts[letter];
			}
		}
	}

	uint8_t guess_counts[26] = {0};
	for (int column = 0; column < WORD_LENGTH; column++) {
		char letter = s_guesses[row][column];

		if (required_positions[column] != 0 &&
			letter != required_positions[column]) {
			return false;
		}

		if (letter >= 'A' && letter <= 'Z') {
			guess_counts[letter - 'A']++;
		}
	}

	for (int letter = 0; letter < 26; letter++) {
		if (guess_counts[letter] < minimum_counts[letter]) {
			return false;
		}
	}

	return true;
}

// ------------------------------------------------------------
// Drawing
// ------------------------------------------------------------

static GColor color_for_state(TileState state) {
	switch (state) {
	case TILE_CORRECT:
	return GColorIslamicGreen;

	case TILE_PRESENT:
	return GColorChromeYellow;

	case TILE_ABSENT:
	return GColorDarkGray;

	default:
	return GColorWhite;
  }
}

static void draw_alphabet(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);

//Perform the check to figure out whether it's a round display!
#if defined(PBL_ROUND)

	// Draw the alphabet around the circumference of the board.

	const int center_x = bounds.size.w / 2;
	const int center_y = bounds.size.h / 2;

	// Radius of the alphabet circle.
	//
	// We leave some room between the board and alphabet.
	// The value is based on the smaller screen dimension so
	// this works on both Chalk and Gabbro.
	const int radius =
	(bounds.size.w < bounds.size.h
		? bounds.size.w
	: bounds.size.h) / 2 - 10;

	const int letter_count = 26;

	// Start at the top of the circle.
	// Pebble graphics angles use degrees.
	const float start_angle = -90.0f;

	for (int i = 0; i < letter_count; i++) {

		TileState state = s_alphabet_states[i];

		// Hide letters that have been determined to be absent.
		if (state == TILE_ABSENT) {
			continue;
		}

		// Calculate the angle for this letter.
		float angle =
			start_angle +
			(360.0f * i / letter_count);

		// Convert degrees to radians.
		float radians =
			angle * 3.14159265f / 180.0f;

		// Calculate the letter's position on the circle.
		int x =
			center_x +
			(int)(radius * cosf(radians));

		int y =
			center_y +
			(int)(radius * sinf(radians));

		char letter[2] = {
			'A' + i,
			'\0'
		};

		// Choose color based on state.
		if (state == TILE_CORRECT) {
			graphics_context_set_text_color(
				ctx,
				GColorIslamicGreen
			);
		}
		else if (state == TILE_PRESENT) {
			graphics_context_set_text_color(
				ctx,
				GColorChromeYellow
			);
		}
		else {
			graphics_context_set_text_color(
				ctx,
				GColorWhite
			);
		}

		// Center the letter around its calculated point.
		graphics_draw_text(
			ctx,
			letter,
			fonts_get_system_font(
				FONT_KEY_GOTHIC_14_BOLD
			),
			GRect(
				x - 9,
				y - 9,
				18,
				18
			),
			GTextOverflowModeFill,
			GTextAlignmentCenter,
			NULL
		);
	}

#else

	// RECTANGULAR DISPLAY

	const int columns = 13;
	const int rows = 2;

	const int cell_width =
		bounds.size.w / columns;

	const int cell_height =
		bounds.size.h / rows;

	for (int i = 0; i < 26; i++) {

		TileState state =
			s_alphabet_states[i];

		// Hide letters that have been determined to be absent.
		if (state == TILE_ABSENT) {
			continue;
		}

		int row = i / columns;
		int col = i % columns;

		GRect rect = GRect(
			col * cell_width,
			row * cell_height,
			cell_width,
			cell_height
		);

		char letter[2] = {
			'A' + i,
			'\0'
		};

		// Choose color based on state.
		if (state == TILE_CORRECT) {
			graphics_context_set_text_color(
				ctx,
				GColorIslamicGreen
			);
		}
		else if (state == TILE_PRESENT) {
			graphics_context_set_text_color(
				ctx,
				GColorChromeYellow
			);
		}
		else {
			graphics_context_set_text_color(
				ctx,
				GColorWhite
			);
		}

		graphics_draw_text(
			ctx,
			letter,
			fonts_get_system_font(
				FONT_KEY_GOTHIC_14_BOLD
			),
			rect,
			GTextOverflowModeFill,
			GTextAlignmentCenter,
			NULL
		);
	}

#endif
}


static void draw_board(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);

	const int gap = 3;

#if defined(PBL_ROUND)

	// Leave room around the board for the alphabet.
	//
	// The board is deliberately smaller on the round display
	// so the alphabet can follow its circumference.

	const int available_width =
		bounds.size.w - 120;

	const int available_height =
		bounds.size.h - 74;

#else

	const int available_width =
		bounds.size.w;

	const int available_height =
		bounds.size.h;

#endif

	// Maximum tile size that fits horizontally.
	const int tile_width =
		(available_width - (WORD_LENGTH - 1) * gap)
		/ WORD_LENGTH;

	// Maximum tile size that fits vertically.
	const int tile_height =
		(available_height - (MAX_GUESSES - 1) * gap)
		/ MAX_GUESSES;

	// Use the smaller dimension so the board always fits.
	const int cell_size =
		tile_width < tile_height
			? tile_width
			: tile_height;

	const int board_width =
		WORD_LENGTH * cell_size +
		(WORD_LENGTH - 1) * gap;

	const int board_height =
		MAX_GUESSES * cell_size +
		(MAX_GUESSES - 1) * gap;

#if defined(PBL_ROUND)

	const int start_x =
		(bounds.size.w - board_width) / 2;

	const int start_y =
		(bounds.size.h - board_height) / 2;

#else

	const int start_x =
		(bounds.size.w - board_width) / 2;

	const int start_y =
		(bounds.size.h - board_height) / 2;

#endif

	for (int row = 0; row < MAX_GUESSES; row++) {
	for (int col = 0; col < WORD_LENGTH; col++) {

	int x = start_x + col * (cell_size + gap);
	int y = start_y + row * (cell_size + gap);

	GRect rect =
		GRect(x, y, cell_size, cell_size);

		TileState state = s_states[row][col];

	// Tile background
	if (state == TILE_EMPTY) {
		graphics_context_set_fill_color(
		ctx,
		GColorOxfordBlue
	);

	graphics_fill_rect(
		ctx,
		rect,
		0,
		GCornerNone
	);

	graphics_context_set_stroke_color(
		ctx,
		GColorLightGray
	);

	graphics_draw_rect(ctx, rect);

	}
	else
	{
		graphics_context_set_fill_color(
		ctx,
		color_for_state(state)
	);

		graphics_fill_rect(
		ctx,
		rect,
		0,
		GCornerNone
	);
}

	// Letter
	if (s_guesses[row][col] != '\0') {
		char letter[2] = {
		s_guesses[row][col],
		'\0'
	};

	graphics_context_set_text_color(
		ctx,
		GColorWhite
	);

	graphics_draw_text(
		ctx,
		letter,
		fonts_get_system_font(
			FONT_KEY_GOTHIC_18_BOLD
			),
		rect,
		GTextOverflowModeFill,
		GTextAlignmentCenter,
		NULL
		);
	  }
	}
  }
}

static void draw_results(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);

	// Background
	graphics_context_set_fill_color(
		ctx,
		GColorOxfordBlue
	);

	graphics_fill_rect(
		ctx,
		bounds,
		0,
		GCornerNone
	);

	// Title

	graphics_context_set_text_color(
		ctx,
		GColorWhite
	);

	graphics_draw_text(
		ctx,
		"RESULTS",
		fonts_get_system_font(
			FONT_KEY_GOTHIC_18_BOLD
		),
		GRect(
			0,
			2,
			bounds.size.w,
			24
		),
		GTextOverflowModeFill,
		GTextAlignmentCenter,
		NULL
	);

	// Guess distribution

	uint16_t max_wins = 1;

	for (int i = 0; i < MAX_GUESSES; i++) {
		if (s_stats.wins_by_row[i] > max_wins) {
		max_wins = s_stats.wins_by_row[i];
		}
	}

	//Shift results for round screens
	#if defined(PBL_ROUND)

	const int label_width = 18;
	const int bar_x = 30;
	const int max_bar_width = bounds.size.w - bar_x - 45;
	const int row_height = 12;
	const int chart_y = 34;

	#else

	const int label_width = 18;
	const int bar_x = 22;
	const int max_bar_width = bounds.size.w - bar_x - 25;
	const int row_height = 14;
	const int chart_y = 30;

	#endif

	for (int i = 0; i < MAX_GUESSES; i++) {
		char row_label[4];

		snprintf(
			row_label,
			sizeof(row_label),
			"%d",
			i + 1
		);

		graphics_context_set_text_color(
			ctx,
			GColorWhite
		);

		graphics_draw_text(
			ctx,
			row_label,
			fonts_get_system_font(
				FONT_KEY_GOTHIC_14
			),
			GRect(
				0,
				chart_y + i * row_height,
				label_width,
				row_height
			),
			GTextOverflowModeFill,
			GTextAlignmentCenter,
			NULL
		);

	if (s_stats.wins_by_row[i] > 0) {
	int bar_width =
	(s_stats.wins_by_row[i] * max_bar_width)
	/ max_wins;

		if (bar_width < 3) {
			bar_width = 3;
		}

		graphics_context_set_fill_color(
			ctx,
			GColorIslamicGreen
		);

		graphics_fill_rect(
			ctx,
			GRect(
			bar_x,
			chart_y + i * row_height + 1,
			bar_width,
			row_height - 2
			),
			2,
			GCornersAll
		);
	}

	// Number at the end of the bar.
	char count[8];

	snprintf(
		count,
		sizeof(count),
		"%d",
		s_stats.wins_by_row[i]
	);

	graphics_context_set_text_color(
		ctx,
		GColorWhite
	);

	graphics_draw_text(
		ctx,
		count,
		fonts_get_system_font(
			FONT_KEY_GOTHIC_14
		),
		GRect(
			bounds.size.w - 22,
			chart_y + i * row_height,
			22,
			row_height
		),
		GTextOverflowModeFill,
		GTextAlignmentCenter,
		NULL
	);
}

// ----------------------------------------------------------
// Statistics
// ----------------------------------------------------------

int stats_y =
chart_y + MAX_GUESSES * row_height + 3;

// Show the answer if the player lost.
if (!s_won) {
char answer_text[32];

	snprintf(
		answer_text,
		sizeof(answer_text),
		"ANSWER: %s",
		s_answer
	);

	graphics_context_set_text_color(
		ctx,
		GColorWhite
	);

	graphics_draw_text(
		ctx,
		answer_text,
		fonts_get_system_font(
			FONT_KEY_GOTHIC_14
		),
		GRect(
			0,
			stats_y,
			bounds.size.w,
			18
		),
		GTextOverflowModeFill,
		GTextAlignmentCenter,
		NULL
	);
	stats_y += 17;
}

// Statistics

uint16_t win_percent = 0;

if (s_stats.games_played > 0) {
	win_percent =
		(s_stats.games_won * 100)
		/ s_stats.games_played;
}

char stats_line[64];

graphics_context_set_text_color(
	ctx,
	GColorWhite
);

// Played
snprintf(
	stats_line,
	sizeof(stats_line),
	"Played: %d",
	s_stats.games_played
);

graphics_draw_text(
	ctx,
	stats_line,
	fonts_get_system_font(
		FONT_KEY_GOTHIC_14
	),
	GRect(
		0,
		stats_y,
		bounds.size.w,
		18
	),
	GTextOverflowModeFill,
	GTextAlignmentCenter,
	NULL
);

// Win Percentage
snprintf(
	stats_line,
	sizeof(stats_line),
	"Win Percentage: %d%%",
	win_percent
);

graphics_draw_text(
	ctx,
	stats_line,
	fonts_get_system_font(
		FONT_KEY_GOTHIC_14
	),
	GRect(
		0,
		stats_y + 15,
		bounds.size.w,
		18
	),
	GTextOverflowModeFill,
	GTextAlignmentCenter,
	NULL
);

// Streak
snprintf(
	stats_line,
	sizeof(stats_line),
	"Current Streak: %d   Best: %d",
	s_stats.current_streak,
	s_stats.best_streak
);

graphics_draw_text(
	ctx,
	stats_line,
	fonts_get_system_font(
		FONT_KEY_GOTHIC_14
	),
	GRect(
		0,
		stats_y + 30,
		bounds.size.w,
		18
	),
	GTextOverflowModeFill,
	GTextAlignmentCenter,
	NULL
);

// ----------------------------------------------------------
// Footer
// ----------------------------------------------------------

graphics_context_set_text_color(
	ctx,
	GColorLightGray
);

graphics_draw_text(
	ctx,
	"New Word Tomorrow!",
	fonts_get_system_font(
		FONT_KEY_GOTHIC_14
	),
	GRect(
		0,
		bounds.size.h - 18,
		bounds.size.w,
		18
		),
		GTextOverflowModeFill,
		GTextAlignmentCenter,
		NULL
	);
}


// ------------------------------------------------------------
// UI updates
// ------------------------------------------------------------

static void update_selector(void) {
	static char selector[32];

		if (s_game_over) {
			selector[0] = '\0';
		}
		else {
		char previous2 = (s_current_letter <= 'B')
			? s_current_letter - 'A' + 'Y'
			: s_current_letter - 2;

		char previous = (s_current_letter == 'A')
			? 'Z'
			: s_current_letter - 1;

		char next = (s_current_letter == 'Z')
			? 'A'
			: s_current_letter + 1;

		char next2 = (s_current_letter >= 'Y')
			? s_current_letter - 'Y' + 'A'
			: s_current_letter + 2;

		snprintf(
			selector,
			sizeof(selector),
			"< %c %c [%c] %c %c >",
			previous2,
			previous,
			s_current_letter,
			next,
			next2
		);
	}
	text_layer_set_text(s_selector_layer, selector);
}

static void redraw(void) {
	if (s_screen == SCREEN_RESULTS) {
	layer_set_hidden(s_results_layer, false);
	layer_set_hidden(s_alphabet_layer, true);
	layer_set_hidden(s_board_layer, true);
	layer_set_hidden(
		text_layer_get_layer(s_selector_layer),
		true
	);

	layer_mark_dirty(s_results_layer);

	}
	else {
	layer_set_hidden(s_results_layer, true);
	layer_set_hidden(s_alphabet_layer, false);
	layer_set_hidden(s_board_layer, false);
	layer_set_hidden(
		text_layer_get_layer(s_selector_layer),
		false
	);

	layer_mark_dirty(s_alphabet_layer);
	layer_mark_dirty(s_board_layer);
	update_selector();
	}
}

static void clear_invalid_word_message(void *context) {
	s_invalid_word_timer = NULL;

	if (!s_game_over) {
		update_selector();
	}
}

static void show_temporary_message(const char *message) {
	// Cancel an existing timer if there is one.
	if (s_invalid_word_timer != NULL) {
	app_timer_cancel(s_invalid_word_timer);
	s_invalid_word_timer = NULL;
	}

	text_layer_set_text(
		s_selector_layer,
		message
	);

	vibes_short_pulse();

	// Show the message for 1.5 seconds.
	s_invalid_word_timer = app_timer_register(
	1500,
	clear_invalid_word_message,
	NULL
	);
}

// ------------------------------------------------------------
// Game actions
// ------------------------------------------------------------

static void submit_guess(void);
static void restart_game(void);

static void add_current_letter(void) {
	if (s_game_over || is_guess_complete()) {
		return;
	}

	s_guesses[s_current_row][s_current_col] = s_current_letter;
	s_current_col++;

	save_daily_game();

	// Automatically submit when the fifth letter is entered.
	if (is_guess_complete()) {
	redraw();
	submit_guess();
	return;
	}
	
	redraw();
}

static void delete_letter(void) {
	if (s_game_over) {
	return;
	}

	if (s_current_col > 0) {
	s_current_col--;

	s_guesses[s_current_row][s_current_col] = '\0';

	save_daily_game();

	redraw();
	}
}

static void cycle_letter(int direction) {
	if (s_game_over || is_guess_complete()) {
	return;
	}

	int letter = s_current_letter - 'A';
	letter += direction;

	if (letter < 0) {
		letter = 25;
	}

	if (letter > 25) {
		letter = 0;
	}

	s_current_letter = 'A' + letter;

	update_selector();
}

static void alphabet_scroll_callback(void *context) {
	// Stop if the button is no longer being held.
	if (s_scroll_direction == 0) {
		s_scroll_timer = NULL;
		return;
	}

	// Don't scroll if the game has ended or the row is full.
	if (s_game_over || is_guess_complete()) {
		s_scroll_timer = NULL;
		s_scroll_direction = 0;
		return;
	}

	// Move one letter.
	cycle_letter(s_scroll_direction);

	// Accelerate as the button continues to be held.
	if (s_scroll_delay > 60) {
	s_scroll_delay -= 20;
	}

	// Schedule the next letter.
	s_scroll_timer = app_timer_register(
	s_scroll_delay,
	alphabet_scroll_callback,
	NULL
	);
}


static void start_alphabet_scroll(int direction) {
	// Don't start scrolling if the game isn't accepting letters.
	if (s_game_over || is_guess_complete()) {
	return;
	}

	s_scroll_direction = direction;

	// Start relatively slowly.
	s_scroll_delay = 180;

	// Wait 250 ms before the first automatic move.
	s_scroll_timer = app_timer_register(
	250,
	alphabet_scroll_callback,
	NULL
	);
}


static void stop_alphabet_scroll(void) {
	s_scroll_direction = 0;

	if (s_scroll_timer != NULL) {
		app_timer_cancel(s_scroll_timer);
		s_scroll_timer = NULL;
	}
}

static void submit_guess(void) {
	if (s_game_over) {
	return;
	}

	if (!is_guess_complete()) {
	return;
	}

	// Check whether the guess is a valid word
	if (!word_is_in_wordlist(s_guesses[s_current_row])) {
		show_temporary_message("NOT A WORD");
		return;
	}

	if (!guess_obeys_hard_mode(s_current_row)) {
		show_temporary_message("USE HINTS");
		return;
	}

	vibes_short_pulse();
	evaluate_guess(s_current_row);
	update_alphabet(s_current_row);

	// Correct answer
	if (guess_is_correct()) {
		record_win(s_current_row);

		s_game_over = true;
		s_screen = SCREEN_RESULTS;

		save_daily_game();

		vibes_double_pulse();
		redraw();
		return;
	}

	// Final guess was incorrect
	if (s_current_row == MAX_GUESSES - 1) {
		record_loss();

		s_game_over = true;
		s_screen = SCREEN_RESULTS;

		save_daily_game();

		vibes_long_pulse();
		redraw();
		return;
	}

	// Continue to next guess
	s_current_row++;
	s_current_col = 0;

	save_daily_game();
	
	redraw();
}

static void restart_game(void) {
	// A completed daily puzzle cannot be replayed until the calendar day changes.
	if (s_daily_game.game_day == get_current_day() &&
		s_daily_game.game_over) {
		return;
	}

	choose_new_word();

	s_daily_game.game_day = get_current_day();

	s_screen = SCREEN_GAME;

	save_daily_game();
	redraw();
}

// ------------------------------------------------------------
// Button handling
// ------------------------------------------------------------

static void up_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	cycle_letter(1);
}

static void down_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	cycle_letter(-1);
}

static void up_long_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	start_alphabet_scroll(1);
}


static void down_long_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	start_alphabet_scroll(-1);
}


static void up_release_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	stop_alphabet_scroll();
}


static void down_release_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	stop_alphabet_scroll();
}

static void select_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	if (s_screen == SCREEN_RESULTS) {
	restart_game();
	return;
	}

	add_current_letter();
}

static void back_click_handler(
	ClickRecognizerRef recognizer,
	void *context) {

	delete_letter();
}

static void click_config_provider(void *context) {
	// UP/DOWN automatically repeat while held.
	window_single_repeating_click_subscribe(
		BUTTON_ID_UP,
		150,
		up_click_handler
	);

	window_single_repeating_click_subscribe(
		BUTTON_ID_DOWN,
		150,
		down_click_handler
	);

	window_single_click_subscribe(
	BUTTON_ID_SELECT,
	select_click_handler
	);

	// Back behaves as backspace.
	window_single_click_subscribe(
	BUTTON_ID_BACK,
	back_click_handler
	);
}

// ------------------------------------------------------------
// LAYERS
// ------------------------------------------------------------

static void window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);

    window_set_background_color(
        window,
        GColorOxfordBlue
    );

    const int selector_height = 25;

#if defined(PBL_ROUND)

    // ========================================================
    // ROUND DISPLAY
    // ========================================================

    // Selector location adjustment. Swap out this number here v if looks like dookie.
	const int selector_y = bounds.size.h - selector_height - 24;
	
	s_selector_layer = text_layer_create(
    GRect(
        0,
        selector_y,
        bounds.size.w,
        selector_height
		)
	);

    text_layer_set_background_color(
        s_selector_layer,
        GColorOxfordBlue
    );

    text_layer_set_text_color(
        s_selector_layer,
        GColorWhite
    );

    text_layer_set_text_alignment(
        s_selector_layer,
        GTextAlignmentCenter
    );

    text_layer_set_font(
        s_selector_layer,
        fonts_get_system_font(
            FONT_KEY_GOTHIC_18_BOLD
        )
    );

    layer_add_child(
        root,
        text_layer_get_layer(s_selector_layer)
    );


    // --------------------------------------------------------
    // Round play area
    // --------------------------------------------------------

	s_alphabet_layer = layer_create(
    GRect(
        0,
        0,
        bounds.size.w,
        bounds.size.h
    )
	);

    layer_set_update_proc(
        s_alphabet_layer,
        draw_alphabet
    );

    layer_add_child(
        root,
        s_alphabet_layer
    );


    // Board occupies the same area.
    // It is drawn after the alphabet, so the board sits
    // visually in the middle of the alphabet ring.
	s_board_layer = layer_create(
    GRect(
        0,
        0,
        bounds.size.w,
        bounds.size.h 
		)
	);

    layer_set_update_proc(
        s_board_layer,
        draw_board
    );

    layer_add_child(
        root,
        s_board_layer
    );


#else

    // ========================================================
    // RECTANGULAR DISPLAY
    // ========================================================

    const int alphabet_height = 32;
    const int gap = 2;

    // Alphabet
    s_alphabet_layer = layer_create(
        GRect(
            0,
            0,
            bounds.size.w,
            alphabet_height
        )
    );

    layer_set_update_proc(
        s_alphabet_layer,
        draw_alphabet
    );

    layer_add_child(
        root,
        s_alphabet_layer
    );


    // Board
    const int board_y =
        alphabet_height + gap;

    const int board_height =
        bounds.size.h
        - alphabet_height
        - selector_height
        - (gap * 2);

    s_board_layer = layer_create(
        GRect(
            0,
            board_y,
            bounds.size.w,
            board_height
        )
    );

    layer_set_update_proc(
        s_board_layer,
        draw_board
    );

    layer_add_child(
        root,
        s_board_layer
    );


    // Selector
    s_selector_layer = text_layer_create(
        GRect(
            0,
            bounds.size.h - selector_height,
            bounds.size.w,
            selector_height
        )
    );

    text_layer_set_background_color(
        s_selector_layer,
        GColorOxfordBlue
    );

    text_layer_set_text_color(
        s_selector_layer,
        GColorWhite
    );

    text_layer_set_text_alignment(
        s_selector_layer,
        GTextAlignmentCenter
    );

    text_layer_set_font(
        s_selector_layer,
        fonts_get_system_font(
            FONT_KEY_GOTHIC_18_BOLD
        )
    );

    layer_add_child(
        root,
        text_layer_get_layer(s_selector_layer)
    );

#endif

  // Results layer
	s_results_layer = layer_create(
		GRect(
		0,
		0,
		bounds.size.w,
		bounds.size.h
		)
	);

	layer_set_update_proc(
		s_results_layer,
		draw_results
	);

	layer_add_child(
	root,
	s_results_layer
	);

	layer_set_hidden(
	s_results_layer,
	true
	);

	// Load today's game or start a new one

	if (!load_daily_game()) {
		// If saved game for today start a fresh puzzle.
		choose_new_word();

		// Mark this puzzle as today's puzzle.
		s_daily_game.game_day = get_current_day();

		save_daily_game();
	}

	// If the saved game was already completed, show the results screen.
	if (s_game_over) {
	s_screen = SCREEN_RESULTS;
	}
	else {
	s_screen = SCREEN_GAME;
	}

redraw();

}

static void window_unload(Window *window) {
	text_layer_destroy(s_selector_layer);

	layer_destroy(s_alphabet_layer);
	layer_destroy(s_board_layer);
	layer_destroy(s_results_layer);
}

static void inbox_received_handler(
    DictionaryIterator *iterator,
    void *context
) {
    Tuple *hard_mode_tuple =
        dict_find(iterator, HARD_MODE_MESSAGE_KEY);

    if (hard_mode_tuple != NULL) {
        s_hard_mode =
            hard_mode_tuple->value->int32 != 0;

        persist_write_bool(
            HARD_MODE_KEY,
            s_hard_mode
        );

        APP_LOG(
            APP_LOG_LEVEL_INFO,
            "Hard Mode: %s",
            s_hard_mode ? "on" : "off"
        );
    }
}

static void init(void) {
	srand(time(NULL));

	load_stats();
	if (persist_exists(HARD_MODE_KEY)) {
		s_hard_mode = persist_read_bool(HARD_MODE_KEY);
	}

	app_message_register_inbox_received(inbox_received_handler);
	AppMessageResult app_message_result = app_message_open(64, 64);
	if (app_message_result != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage open failed: %d",
			(int)app_message_result);
	}

	s_window = window_create();

	window_set_window_handlers(
		s_window,
		(WindowHandlers) {
		.load = window_load,
		.unload = window_unload
		}
	);

	window_set_click_config_provider(
		s_window,
		click_config_provider
	);
	
	window_stack_push(s_window, true);
}

static void deinit(void) {
	app_message_deregister_callbacks();
	window_destroy(s_window);
}

int main(void) {
	init();
	app_event_loop();
	deinit();
}
