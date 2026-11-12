/* External scanner for tree-sitter-strada.
 *
 * Supplies the three token classes a context-free grammar can't express:
 *   - heredoc_start / heredoc_body: <<TAG, <<'TAG', <<"TAG" record the tag
 *     in a FIFO queue (two heredocs on one line deliver bodies in source
 *     order); the body token fires at the next newline and consumes lines
 *     through the terminator line.
 *   - c_block_body: raw balanced-brace region inside __C__ { } and
 *     extern "C" { }, aware of C strings/chars/comments so a brace inside
 *     "..." or a comment doesn't unbalance the region.
 *   - pod: column-0 =word ... =cut blocks, as an extra.
 */

#include "tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
  HEREDOC_START,
  HEREDOC_BODY,
  C_BLOCK_BODY,
  POD,
};

#define MAX_HEREDOCS 8
#define MAX_TAG_LEN 63

typedef struct {
  char tags[MAX_HEREDOCS][MAX_TAG_LEN + 1];
  unsigned count;
} Scanner;

void *tree_sitter_strada_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  return s;
}

void tree_sitter_strada_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_strada_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned n = 0;
  buffer[n++] = (char)s->count;
  for (unsigned i = 0; i < s->count; i++) {
    size_t len = strlen(s->tags[i]);
    buffer[n++] = (char)len;
    memcpy(buffer + n, s->tags[i], len);
    n += len;
  }
  return n;
}

void tree_sitter_strada_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = (Scanner *)payload;
  s->count = 0;
  if (length == 0) return;
  unsigned n = 0;
  unsigned count = (unsigned char)buffer[n++];
  for (unsigned i = 0; i < count && i < MAX_HEREDOCS && n < length; i++) {
    unsigned len = (unsigned char)buffer[n++];
    if (len > MAX_TAG_LEN || n + len > length) break;
    memcpy(s->tags[i], buffer + n, len);
    s->tags[i][len] = '\0';
    n += len;
    s->count++;
  }
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static bool is_tag_char(int32_t c) {
  return iswalnum(c) || c == '_';
}

/* <<TAG, <<'TAG', <<"TAG" — records the tag, token spans the whole opener.
 * The scanner runs before internal whitespace skipping, so skip leading
 * spaces itself (skip=true keeps them out of the token). */
static bool scan_heredoc_start(Scanner *s, TSLexer *lexer) {
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, true);
  }
  if (lexer->lookahead != '<') return false;
  advance(lexer);
  if (lexer->lookahead != '<') return false;
  advance(lexer);

  int32_t quote = 0;
  if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
    quote = lexer->lookahead;
    advance(lexer);
  }

  if (!iswalpha(lexer->lookahead) && lexer->lookahead != '_') return false;

  char tag[MAX_TAG_LEN + 1];
  unsigned len = 0;
  while (is_tag_char(lexer->lookahead) && len < MAX_TAG_LEN) {
    tag[len++] = (char)lexer->lookahead;
    advance(lexer);
  }
  tag[len] = '\0';

  if (quote) {
    if (lexer->lookahead != quote) return false;
    advance(lexer);
  }

  if (s->count >= MAX_HEREDOCS) return false;
  memcpy(s->tags[s->count], tag, len + 1);
  s->count++;

  lexer->result_symbol = HEREDOC_START;
  lexer->mark_end(lexer);
  return true;
}

/* At (whitespace then) a newline with pending tags: consume lines through
 * the terminator line of the FRONT tag. */
static bool scan_heredoc_body(Scanner *s, TSLexer *lexer) {
  if (s->count == 0) return false;

  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r') {
    advance(lexer);
  }
  if (lexer->lookahead != '\n') return false;
  advance(lexer);

  const char *tag = s->tags[0];
  size_t tag_len = strlen(tag);

  for (;;) {
    /* Compare the upcoming line against the terminator tag. */
    unsigned matched = 0;
    bool line_is_tag = true;
    while (matched < tag_len) {
      if (lexer->lookahead == (int32_t)(unsigned char)tag[matched]) {
        advance(lexer);
        matched++;
      } else {
        line_is_tag = false;
        break;
      }
    }
    if (line_is_tag && (lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer))) {
      /* Terminator found: pop the front tag (FIFO). */
      memmove(s->tags[0], s->tags[1], sizeof(s->tags[0]) * (MAX_HEREDOCS - 1));
      s->count--;
      lexer->result_symbol = HEREDOC_BODY;
      lexer->mark_end(lexer);
      return true;
    }
    /* Not the terminator: consume the rest of the line. */
    while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
      advance(lexer);
    }
    if (lexer->eof(lexer)) {
      /* Unterminated heredoc: emit what we have so the parse can recover. */
      memmove(s->tags[0], s->tags[1], sizeof(s->tags[0]) * (MAX_HEREDOCS - 1));
      s->count--;
      lexer->result_symbol = HEREDOC_BODY;
      lexer->mark_end(lexer);
      return true;
    }
    advance(lexer); /* the newline */
  }
}

/* Raw region between the braces of __C__ { ... }: balanced-brace count,
 * skipping C strings, char literals, and comments. Stops BEFORE the
 * closing brace (the grammar owns it). */
static bool scan_c_block_body(TSLexer *lexer) {
  if (lexer->lookahead == '}') return false; /* empty body -> optional() */

  int depth = 0;
  bool consumed = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer)) break;
    if (c == '}' && depth == 0) break;

    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
    } else if (c == '"' || c == '\'') {
      int32_t q = c;
      advance(lexer); consumed = true;
      while (!lexer->eof(lexer) && lexer->lookahead != q) {
        if (lexer->lookahead == '\\') advance(lexer);
        if (!lexer->eof(lexer)) advance(lexer);
      }
      if (lexer->eof(lexer)) break;
      /* fall through to consume the closing quote below */
    } else if (c == '/') {
      advance(lexer); consumed = true;
      if (lexer->lookahead == '/') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') advance(lexer);
      } else if (lexer->lookahead == '*') {
        advance(lexer);
        while (!lexer->eof(lexer)) {
          if (lexer->lookahead == '*') {
            advance(lexer);
            if (lexer->lookahead == '/') { advance(lexer); break; }
          } else {
            advance(lexer);
          }
        }
      }
      continue;
    }

    advance(lexer);
    consumed = true;
  }

  if (!consumed) return false;
  lexer->result_symbol = C_BLOCK_BODY;
  lexer->mark_end(lexer);
  return true;
}

/* Column-0 =word ... through the =cut line (or EOF). Skips leading
 * whitespace itself (the scanner runs before internal whitespace
 * skipping, and internal skipping never re-invokes the scanner at the
 * new position). */
static bool scan_pod(TSLexer *lexer) {
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\r' || lexer->lookahead == '\n') {
    lexer->advance(lexer, true);
  }
  if (lexer->get_column(lexer) != 0) return false;
  if (lexer->lookahead != '=') return false;
  advance(lexer);
  if (!iswalpha(lexer->lookahead)) return false;

  for (;;) {
    bool is_cut = false;
    /* Consume the rest of the current line, then check whether the next
     * line starts with =cut (the first =word line was consumed above). */
    while (lexer->lookahead != '\n' && !lexer->eof(lexer)) advance(lexer);
    if (lexer->eof(lexer)) break;
    advance(lexer); /* newline */
    if (lexer->lookahead == '=') {
      advance(lexer);
      if (lexer->lookahead == 'c') {
        advance(lexer);
        if (lexer->lookahead == 'u') {
          advance(lexer);
          if (lexer->lookahead == 't') {
            advance(lexer);
            is_cut = true;
          }
        }
      }
      if (is_cut) {
        while (lexer->lookahead != '\n' && !lexer->eof(lexer)) advance(lexer);
        break;
      }
    }
  }

  lexer->result_symbol = POD;
  lexer->mark_end(lexer);
  return true;
}

bool tree_sitter_strada_external_scanner_scan(void *payload, TSLexer *lexer,
                                              const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  /* Error recovery marks every external token valid; producing one there
   * (especially c_block_body, which consumes greedily) turns a local error
   * into a whole-file ERROR. Bail and let internal recovery run. */
  if (valid_symbols[HEREDOC_START] && valid_symbols[HEREDOC_BODY] &&
      valid_symbols[C_BLOCK_BODY] && valid_symbols[POD]) {
    return false;
  }

  if (valid_symbols[HEREDOC_BODY] && s->count > 0) {
    if (scan_heredoc_body(s, lexer)) return true;
  }
  if (valid_symbols[C_BLOCK_BODY]) {
    return scan_c_block_body(lexer);
  }
  if (valid_symbols[POD]) {
    if (scan_pod(lexer)) return true;
  }
  if (valid_symbols[HEREDOC_START]) {
    return scan_heredoc_start(s, lexer);
  }
  return false;
}
