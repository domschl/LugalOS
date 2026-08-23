/*
 * PGN save/load. See user/chess/include/pgn.h for why SAN is the substance of
 * this file. 14b (plan/phase14_networking_and_host_tooling.md), built in
 * phase 16.
 */

#include "pgn.h"
#include "movegen.h"
#include "bitboard.h"
#include "chess_platform.h"
#include "chess_ui.h"
#include "kernel/scratch.h"
#include "kernel/time.h"
#include "fs/vfs.h"
#include <string.h>

#define STANDARD_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* --- SAN --- */

static const char PIECE_LETTER[6] = { 'P', 'N', 'B', 'R', 'Q', 'K' };

/* Does `side` have any legal reply in `pos`? Used only for the '#' vs '+'
 * distinction, so it stops at the first one rather than counting. */
static bool has_any_legal_move(Position *pos) {
    static MoveList list;   /* not on the stack: this file runs on the same
                             * deep call chain as everything else in chess */
    generate_moves(pos, &list);
    for (int i = 0; i < list.count; i++) {
        if (make_move(pos, list.moves[i])) {
            unmake_move(pos);
            return true;
        }
    }
    return false;
}

void format_move_san(Position *pos, Move m, char out[SAN_MAX]) {
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    int piece = pos->board[from];
    int p = 0;

    if (piece == NO_PIECE) { out[0] = '\0'; return; }

    /* Castling is named by the rook's side, not by squares at all. Detected
     * from the move flag rather than from the king's travel distance, which
     * would also match a (never generated) two-square king move. */
    bool k_castle = (piece == KING) && ((m >> 12) & 0xF) == MOVE_FLAG_K_CASTLE;
    bool q_castle = (piece == KING) && ((m >> 12) & 0xF) == MOVE_FLAG_Q_CASTLE;

    if (k_castle || q_castle) {
        out[p++] = 'O'; out[p++] = '-'; out[p++] = 'O';
        if (q_castle) { out[p++] = '-'; out[p++] = 'O'; }
    } else if (piece == PAWN) {
        /* A pawn is named by its file only when it captures. */
        if (move_is_capture(m)) {
            out[p++] = (char)('a' + (from % 8));
            out[p++] = 'x';
        }
        out[p++] = (char)('a' + (to % 8));
        out[p++] = (char)('1' + (to / 8));
        if (move_is_promo(m)) {
            out[p++] = '=';
            out[p++] = PIECE_LETTER[move_promo_piece(m)];
        }
    } else {
        out[p++] = PIECE_LETTER[piece];

        /* Disambiguation: only against moves that are actually *legal*, so a
         * twin whose move would leave its own king in check correctly does
         * not force a file/rank letter. Getting this wrong in the lenient
         * direction produces SAN that other programs read as ambiguous. */
        bool shares_file = false, shares_rank = false, ambiguous = false;
        {
            static MoveList list;
            generate_moves(pos, &list);
            for (int i = 0; i < list.count; i++) {
                Move o = list.moves[i];
                if (o == m) continue;
                if (MOVE_TO(o) != to) continue;
                int of = MOVE_FROM(o);
                if (pos->board[of] != piece) continue;
                if (!make_move(pos, o)) continue;   /* illegal: not a rival */
                unmake_move(pos);
                ambiguous = true;
                if ((of % 8) == (from % 8)) shares_file = true;
                if ((of / 8) == (from / 8)) shares_rank = true;
            }
        }
        if (ambiguous) {
            /* File alone if it distinguishes, else rank, else both -- the
             * order the PGN spec requires, not merely one that works.
             *
             * "Distinguishes" means no *other* candidate stands on the same
             * file (or rank) as this one. The first version of this had the
             * two conditions crossed and emitted "N1d2" where two knights on
             * b1 and f1 both bear on d2 -- a rank disambiguator between
             * pieces that share a rank, which distinguishes nothing. Caught
             * by pgn_selftest()'s round-trip, not by reading the code. */
            if (!shares_file) {
                out[p++] = (char)('a' + (from % 8));
            } else if (!shares_rank) {
                out[p++] = (char)('1' + (from / 8));
            } else {
                out[p++] = (char)('a' + (from % 8));
                out[p++] = (char)('1' + (from / 8));
            }
        }

        if (move_is_capture(m)) out[p++] = 'x';
        out[p++] = (char)('a' + (to % 8));
        out[p++] = (char)('1' + (to / 8));
    }

    /* Check/mate suffix. Playing the move is the only way to know, so it is
     * played and taken back -- pos is left exactly as it was found. */
    if (make_move(pos, m)) {
        int them = pos->side;
        uint64_t kb = pos->piece_bbs[KING] & pos->color_bbs[them];
        bool in_check = false;
        if (kb) {
            int ksq = 0;
            uint64_t t = kb;
            while (!(t & 1ULL)) { t >>= 1; ksq++; }
            in_check = is_square_attacked(pos, ksq, them ^ 1);
        }
        if (in_check) out[p++] = has_any_legal_move(pos) ? '+' : '#';
        unmake_move(pos);
    }

    out[p] = '\0';
}

Move parse_move_san(Position *pos, const char *san) {
    if (!san || !*san) return 0;

    static MoveList list;
    generate_moves(pos, &list);
    for (int i = 0; i < list.count; i++) {
        Move m = list.moves[i];
        if (!make_move(pos, m)) continue;   /* only legal moves can be named */
        unmake_move(pos);
        char buf[SAN_MAX];
        format_move_san(pos, m, buf);
        if (strcmp(buf, san) == 0) return m;
    }
    return 0;
}

/* --- Self-test ---
 *
 * SAN is easy to get subtly wrong and hard to notice: a missing
 * disambiguator produces a file some other program silently misreads as a
 * different game. This checks the two properties that matter, over positions
 * chosen for the cases that break naive implementations.
 *
 * The round-trip is the strong half. It cannot be satisfied by a formatter
 * that under-disambiguates: if two legal moves format identically, one of
 * them parses back to the other and the check fails. The literal expectations
 * are the weak half but catch the opposite error -- a formatter that
 * disambiguates everything unconditionally round-trips perfectly and is still
 * wrong.
 */
struct san_case { const char *fen; const char *expect_present; };

static const struct san_case SAN_CASES[] = {
    /* Start position: plain piece and pawn moves, nothing to disambiguate. */
    { STANDARD_START_FEN, "Nf3" },
    /* Two knights bearing on d2: file disambiguation (Nbd2 / Nfd2). */
    { "8/8/8/8/8/8/8/1N1K1N1k w - - 0 1", "Nbd2" },
    /* Two rooks on the same *file* bearing on a4: rank disambiguation
     * (R1a4 / R8a4). The first attempt here put rooks on a1 and h1 and
     * expected "Rag1" -- but the white king on d1 blocks a1's path, so only
     * one rook reaches g1 and correct SAN is the undisambiguated "Rg1". The
     * second attempt put the black king on h8, where the a8 rook already
     * checks it with White to move -- an illegal position the generator
     * happily accepts, which made every move come back with a '+' suffix.
     * The test data was wrong twice before the code was wrong once. */
    { "R7/8/7k/8/8/8/8/R3K3 w - - 0 1", "R1a4" },
    /* Kiwipete: castling both sides, captures, plenty of piece traffic. */
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "O-O" },
    /* Pawn captures and promotions, including capture-promotions. Not "+":
     * the new queen on c8 is blocked along the eighth rank by Black's own
     * queen on d8, so it gives no check -- another expectation this test
     * caught as wrong before the code was. */
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", "dxc8=Q" },
    /* Mate in one: the '#' suffix, which '+' logic alone gets wrong. */
    { "6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1", "Re8#" },
};

int pgn_selftest(void) {
    int failures = 0;
    int checked = 0;

    /* Self-contained: its own Position from the heap, and its own call to
     * init_bitboards(). This must run without a chess *session* -- it is a
     * notation test, not a game -- so it cannot borrow chess_ui.c's
     * session-scoped scratch, and cannot assume the attack tables some
     * earlier session happened to fill. init_bitboards() is idempotent and
     * cheap (chess_ensure_init() re-runs it every session for that reason). */
    init_bitboards();

    scratch_t sc;
    if (!scratch_acquire(&sc, sizeof(Position))) {
        printf("san: out of memory\n");
        return -1;
    }
    Position *pos = (Position *)sc.base;

    for (unsigned c = 0; c < sizeof(SAN_CASES) / sizeof(SAN_CASES[0]); c++) {
        parse_fen(pos, SAN_CASES[c].fen);

        static MoveList list;
        generate_moves(pos, &list);

        bool saw_expected = false;
        for (int i = 0; i < list.count; i++) {
            Move m = list.moves[i];
            if (!make_move(pos, m)) continue;
            unmake_move(pos);

            char san[SAN_MAX];
            format_move_san(pos, m, san);
            checked++;
            if (SAN_CASES[c].expect_present &&
                strcmp(san, SAN_CASES[c].expect_present) == 0) {
                saw_expected = true;
            }

            Move back = parse_move_san(pos, san);
            if (back != m) {
                printf("san: '%s' round-tripped to a different move (case %d)\n",
                       san, (int)c);
                failures++;
            }
        }

        if (SAN_CASES[c].expect_present && !saw_expected) {
            printf("san: case %d never produced the expected '%s'\n",
                   (int)c, SAN_CASES[c].expect_present);
            failures++;
        }
    }

    /* End-to-end: play a game, write it, read it back, and require the two
     * positions to be identical. This is what catches the errors SAN's own
     * round-trip cannot -- tag handling, move numbering, the ellipsis for a
     * game that starts with Black to move, and the [FEN] tag that makes a
     * game set up mid-play reload as itself rather than as a fresh board. */
    {
        static const char *PGN_CASES[2] = {
            STANDARD_START_FEN,
            /* Not the initial array: forces [SetUp]/[FEN], and starts with
             * Black to move, which forces the "12... Nf6" ellipsis form. */
            "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
        };
        for (unsigned c = 0; c < 2; c++) {
            parse_fen(pos, PGN_CASES[c]);

            /* A handful of real moves, chosen by the engine's own generator so
             * this works from either starting position without hardcoding. */
            for (int ply = 0; ply < 8; ply++) {
                static MoveList l;
                generate_moves(pos, &l);
                bool played = false;
                for (int i = 0; i < l.count && !played; i++) {
                    if (make_move(pos, l.moves[i])) played = true;
                }
                if (!played) break;   /* mate or stalemate: a fine game too */
            }

            char before[128];
            generate_fen(pos, before);
            int plies = pos->history_ply;

            /* Whichever volume this board can actually write to. /ram0 is
             * not mounted on the RP2350 chess persona -- the RAM disk costs
             * heap and that board has an SD card instead -- so hardcoding it
             * made this half of the test pass on QEMU and fail on the only
             * hardware anyone runs it on. */
            const char *dir = vfs_volume_writable("/sd0") ? "/sd0/chess"
                            : vfs_volume_writable("/ram0") ? "/ram0/chess"
                            : NULL;
            if (!dir) {
                printf("pgn: no writable volume; cannot test file round-trip\n");
                failures++;
                continue;
            }
            vfs_mkdir(dir);
            char tmp[64];
            {
                int n = 0;
                for (const char *q = dir; *q; q++) tmp[n++] = *q;
                for (const char *q = "/pgn_selftest.pgn"; *q; q++) tmp[n++] = *q;
                tmp[n] = '\0';
            }
            if (pgn_save(pos, tmp, "*") != 0) {
                printf("pgn: case %u would not save\n", c);
                failures++;
                continue;
            }
            if (pgn_load(pos, tmp) != 0) {
                printf("pgn: case %u would not load\n", c);
                failures++;
                continue;
            }

            char after[128];
            generate_fen(pos, after);
            if (strcmp(before, after) != 0) {
                printf("pgn: case %u round-tripped to a different position\n", c);
                printf("  before: %s\n  after:  %s\n", before, after);
                failures++;
            } else if (pos->history_ply != plies) {
                printf("pgn: case %u lost move history (%d -> %d)\n",
                       c, plies, pos->history_ply);
                failures++;
            }
            vfs_remove(tmp);
        }
    }

    scratch_release(&sc);
    printf("SAN Results: %d moves checked, %d errors.\n", checked, failures);
    return failures;
}

/* --- PGN files --- */

/* Appends to a bounded buffer, tracking the write position. Silently stops at
 * the cap rather than overrunning: a truncated PGN is a bad file, an overrun
 * is a corrupted board. */
static void put(char *buf, uint32_t cap, uint32_t *used, const char *s) {
    while (*s && *used + 1 < cap) buf[(*used)++] = *s++;
    buf[*used] = '\0';
}

static void put_uint(char *buf, uint32_t cap, uint32_t *used, unsigned v, int pad) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < pad && n < (int)sizeof(tmp)) tmp[n++] = '0';
    while (n-- > 0 && *used + 1 < cap) buf[(*used)++] = tmp[n];
    buf[*used] = '\0';
}

/* Rewinds a copy of `src` to the position the game started from. */
static void rewind_to_start(const Position *src, Position *out) {
    *out = *src;
    while (out->history_ply > 0) unmake_move(out);
}

int pgn_save(const Position *pos, const char *path, const char *result) {
    if (!pos || !path) return -1;

    /* One allocation for both the replay board and the text: they are used
     * together and freed together, and a Position is far too large to put on
     * the stack of a call chain that already runs deep. */
    const uint32_t TEXT_CAP = 4096;
    scratch_t sc;
    if (!scratch_acquire(&sc, sizeof(Position) + TEXT_CAP)) return -1;

    Position *board = (Position *)sc.base;
    char *buf = (char *)sc.base + sizeof(Position);
    uint32_t used = 0;
    buf[0] = '\0';

    rewind_to_start(pos, board);

    char start_fen[128];
    generate_fen(board, start_fen);
    bool from_standard = (strcmp(start_fen, STANDARD_START_FEN) == 0);

    put(buf, TEXT_CAP, &used, "[Event \"LugalOS game\"]\n");
    put(buf, TEXT_CAP, &used, "[Site \"LugalOS\"]\n");

    /* The RTC is optional on this board. PGN's own convention for an unknown
     * date is "????.??.??", which is better than inventing one -- a board
     * without an RTC reports a software clock that starts at its own epoch,
     * and stamping that on every game would be worse than saying nothing. */
    {
        /* Local time: PGN's Date and Time tags are the local ones, per the
         * standard, and a game record is read by people. */
        rtc_time_t tm;
        time_get_local(&tm);
        put(buf, TEXT_CAP, &used, "[Date \"");
        if (tm.year >= 2000) {
            put_uint(buf, TEXT_CAP, &used, tm.year, 4);
            put(buf, TEXT_CAP, &used, ".");
            put_uint(buf, TEXT_CAP, &used, tm.month, 2);
            put(buf, TEXT_CAP, &used, ".");
            put_uint(buf, TEXT_CAP, &used, tm.day, 2);
        } else {
            put(buf, TEXT_CAP, &used, "????.??.??");
        }
        put(buf, TEXT_CAP, &used, "\"]\n");
    }

    put(buf, TEXT_CAP, &used, "[Round \"-\"]\n[White \"?\"]\n[Black \"?\"]\n");
    put(buf, TEXT_CAP, &used, "[Result \"");
    put(buf, TEXT_CAP, &used, result ? result : "*");
    put(buf, TEXT_CAP, &used, "\"]\n");

    /* A game that did not start from the initial array needs its own starting
     * position recorded, or the movetext below means nothing. This is what
     * lets `fen <position>` followed by real moves reload exactly. */
    if (!from_standard) {
        put(buf, TEXT_CAP, &used, "[SetUp \"1\"]\n[FEN \"");
        put(buf, TEXT_CAP, &used, start_fen);
        put(buf, TEXT_CAP, &used, "\"]\n");
    }
    put(buf, TEXT_CAP, &used, "\n");

    /* Movetext: replay from the start, naming each move in the position it is
     * actually played in -- which is the only way SAN can be produced. Moves
     * come from the caller's own history, untouched by the replay. */
    uint32_t col = 0;
    int start_fullmove = board->fullmove;
    for (int i = 0; i < pos->history_ply; i++) {
        Move m = pos->history[i].move;
        char san[SAN_MAX];
        format_move_san(board, m, san);
        if (san[0] == '\0') break;   /* history and board disagree; stop clean */

        char item[SAN_MAX + 8];
        uint32_t n = 0;
        if (board->side == WHITE) {
            put_uint(item, sizeof(item), &n, (unsigned)board->fullmove, 0);
            put(item, sizeof(item), &n, ". ");
        } else if (i == 0) {
            /* Black to move first: PGN writes the ellipsis so the numbering
             * is unambiguous. Only possible for a game set up mid-play. */
            put_uint(item, sizeof(item), &n, (unsigned)board->fullmove, 0);
            put(item, sizeof(item), &n, "... ");
        }
        put(item, sizeof(item), &n, san);

        if (col + n + 1 > 78) { put(buf, TEXT_CAP, &used, "\n"); col = 0; }
        else if (col) { put(buf, TEXT_CAP, &used, " "); col++; }
        put(buf, TEXT_CAP, &used, item);
        col += n;

        if (!make_move(board, m)) break;
    }
    (void)start_fullmove;

    if (col + 4 > 78) put(buf, TEXT_CAP, &used, "\n");
    else if (col) put(buf, TEXT_CAP, &used, " ");
    put(buf, TEXT_CAP, &used, result ? result : "*");
    put(buf, TEXT_CAP, &used, "\n");

    int rc = vfs_write(path, buf, used);
    scratch_release(&sc);
    return rc;
}

/* Skips a PGN tag pair, returning the position just past its newline. */
static const char *skip_line(const char *p) {
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

int pgn_load(Position *pos, const char *path) {
    if (!pos || !path) return -1;

    const uint32_t TEXT_CAP = 4096;
    scratch_t sc;
    if (!scratch_acquire(&sc, TEXT_CAP)) return -1;
    char *buf = (char *)sc.base;

    int len = vfs_read(path, buf, TEXT_CAP - 1);
    if (len <= 0) { scratch_release(&sc); return -1; }
    buf[len] = '\0';

    /* Tag section: everything before the first line that is not a tag pair.
     * Only [FEN] is acted on -- the rest is metadata this engine has no state
     * for, and skipping unknown tags is what makes a file written by some
     * other program loadable here. */
    const char *p = buf;
    char fen[128];
    bool have_fen = false;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (*p != '[') break;
        if (strncmp(p, "[FEN \"", 6) == 0) {
            const char *q = p + 6;
            uint32_t n = 0;
            while (*q && *q != '"' && n + 1 < sizeof(fen)) fen[n++] = *q++;
            fen[n] = '\0';
            have_fen = true;
        }
        p = skip_line(p);
    }

    parse_fen(pos, have_fen ? fen : STANDARD_START_FEN);

    /* Movetext. Tokens that are not moves -- move numbers, the ellipsis, the
     * result, comments, NAGs -- are skipped rather than parsed, so this
     * accepts more than pgn_save() produces without needing a grammar. */
    while (*p) {
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '.') p++;
        if (!*p) break;

        if (*p == '{') { while (*p && *p != '}') p++; if (*p) p++; continue; }
        if (*p == ';') { p = skip_line(p); continue; }
        if (*p == '(') { /* variation: not represented in a single game */
            int depth = 1; p++;
            while (*p && depth) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
            continue;
        }
        if (*p == '$') { while (*p && *p != ' ' && *p != '\n') p++; continue; }

        char tok[SAN_MAX];
        uint32_t n = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && n + 1 < sizeof(tok)) {
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++;  /* overlong: drop */
        if (n == 0) continue;

        /* Move numbers ("12.") and results ("1-0", "*") are not moves. */
        if (tok[0] >= '0' && tok[0] <= '9') continue;
        if (tok[0] == '*' || tok[0] == '-') continue;

        Move m = parse_move_san(pos, tok);
        if (m == 0) continue;      /* annotation or notation this build cannot
                                    * name; skipping beats refusing the file */
        if (!make_move(pos, m)) break;
    }

    scratch_release(&sc);
    return 0;
}
