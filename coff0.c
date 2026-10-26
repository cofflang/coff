/*
 * coff0.c - stage-0 bootstrap compiler for the c0 language
 *
 * Copyright (C) 2026 tavro
 *
 * This file is part of coff, the compiler toolchain for c0.
 *
 * coff is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * coff is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with coff. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * This is the bootstrap stage. Plain C, buildable by any C compiler --
 * tcc by preference, because tcc can be bootstrapped from a hand-auditable
 * seed and gcc cannot (see TRUST.md) -- and it exists so the self-hosted
 * compiler (c0/coff.c0) has something to be compiled by the first time.
 * After that it stays around as the oracle. run_tests.sh diffs the output
 * of every c0-written stage against this one, byte for byte, error messages
 * included. It is not the compiler you are meant to use day to day - that
 * is coff1, the binary built from c0/coff.c0.
 *
 * The c0 grammar, as accepted here
 *
 *   program    := (function | global | extern | layout | embed)*
 *   include    := "include" STRING ";"                 (anywhere, replaced by the
 *                                                        file's text before
 *                                                        lexing, each file once.
 *                                                        The path is tried next
 *                                                        to the including file,
 *                                                        then in each -I
 *                                                        directory. This makes
 *                                                        "include" a reserved
 *                                                        word everywhere)
 *   global     := type IDENT ("=" constexpr)? ";"     (folded at compile
 *                                                        time, so only
 *                                                        numbers, characters
 *                                                        and operators)
 *   type       := "int" | IDENT                        (IDENT names a layout,
 *                                                        as in `Point p`. It is
 *                                                        still an int, but its
 *                                                        fields are looked up
 *                                                        in that layout)
 *   extern     := "extern" "int" IDENT ";"            (declares a global that
 *                                                        some other object file
 *                                                        defines; c0 emits no
 *                                                        storage for it)
 *   layout     := "layout" IDENT "{" ("int" IDENT ";")* "}"
 *                                                     (named offsets into a
 *                                                        block of memory. Every
 *                                                        field is "int" because
 *                                                        that is the only type
 *                                                        c0 has; the layout only
 *                                                        gives each name an
 *                                                        offset, it allocates
 *                                                        nothing)
 *   embed      := "embed" IDENT STRING ";"             (a file's bytes as data.
 *                                                        IDENT is their address
 *                                                        and IDENT_size their
 *                                                        length. "embed" is a
 *                                                        word, not a keyword)
 *   function   := ("int" | "void" | "(int, int)") IDENT "(" params? ")" block
 *                                                     ("(int, int)" returns two
 *                                                        values, in rax and rdx)
 *   params     := type IDENT ("," type IDENT)*          (max 16 total; first 6
 *                                                          register-passed, rest
 *                                                          on the stack -- see
 *                                                          gen_call/gen_function)
 *   block      := "{" stmt* "}"
 *   stmt       := decl_stmt | assign_stmt | multi_assign | if_stmt
 *               | while_stmt | for_stmt | return_stmt | break_stmt
 *               | continue_stmt | defer_stmt | expr_stmt | block
 *   decl_stmt  := type IDENT ("=" expr)? ";"
 *   assign_stmt:= (IDENT | postfix) assign_op expr ";" (a postfix target must end
 *                                                        in an index or a field,
 *                                                        e.g. buf[i] = v or
 *                                                        e.hp = 10)
 *   assign_op  := "=" | "+=" | "-=" | "*=" | "/=" | "%="
 *                                                     (x OP= e is sugar for
 *                                                        x = x OP (e))
 *   multi_assign := IDENT "," IDENT "=" call ";"      (the call must be to a
 *                                                        "(int, int)" function)
 *   if_stmt    := "if" "(" expr ")" stmt ("else" stmt)?
 *   while_stmt := "while" "(" expr ")" stmt
 *   for_stmt   := "for" "(" stmt expr ";" simple ")" stmt
 *                                                     (the first stmt brings its
 *                                                        own ";" and is meant to
 *                                                        be a declaration or an
 *                                                        assignment. simple is an
 *                                                        assignment or an
 *                                                        expression without its
 *                                                        ";". None of the three
 *                                                        may be left out.
 *                                                        continue runs the step,
 *                                                        as in C)
 *   return_stmt:= "return" (expr ("," expr)?)? ";"      (a void function may only
 *                                                          use the bare form. An int
 *                                                          function may use it too,
 *                                                          like falling off the end,
 *                                                          and then returns whatever
 *                                                          rax holds)
 *   break_stmt := "break" ";"                            (must be inside a loop)
 *   continue_stmt := "continue" ";"                      (must be inside a loop)
 *   defer_stmt := "defer" stmt                          (runs stmt at every exit
 *                                                          of the function, the
 *                                                          last defer first. It
 *                                                          may not return, break,
 *                                                          continue or defer, nor
 *                                                          use a local of a block
 *                                                          that has ended by then)
 *   expr_stmt  := expr ";"                               (a bare call, e.g. `f(1);`)
 *
 *   expr       := logic_or
 *   logic_or   := logic_and ("||" logic_and)*        (short-circuiting)
 *   logic_and  := bitor ("&&" bitor)*                  (short-circuiting)
 *   bitor      := bitxor ("|" bitxor)*
 *   bitxor     := bitand ("^" bitand)*
 *   bitand     := equality ("&" equality)*
 *   equality   := relational (("==" | "!=") relational)*
 *   relational := shift (("<" | "<=" | ">" | ">=") shift)*
 *   shift      := additive (("<<" | ">>") additive)*
 *   additive   := term (("+" | "-") term)*
 *   term       := unary (("*" | "/" | "%") unary)*
 *   unary      := ("-" | "~" | "!") unary | postfix      (!e is e == 0)
 *   postfix    := primary (("[" expr "]") | ("." IDENT))*
 *                                                     (buf[i] is sugar over
 *                                                        load8(buf+i) / store8;
 *                                                        e.field is sugar over
 *                                                        load64(e + offset) /
 *                                                        store64, where the
 *                                                        offset comes from
 *                                                        whichever layout
 *                                                        declared that field
 *                                                        name)
 *   primary    := NUM | STRING | IDENT ("(" args? ")")? | "(" expr ")"
 *               | "sizeof" "(" IDENT ")"              (IDENT must name a
 *                                                        declared layout.
 *                                                        Resolves at compile
 *                                                        time to its field
 *                                                        count times 8)
 *   args       := expr ("," expr)*
 *
 * The operators mean what they mean in C on a 64-bit signed integer. / and
 * % truncate toward zero, so % takes the sign of the dividend, and >> is
 * arithmetic. I kept C's precedence on purpose, including the old wart
 * that & binds more loosely than ==, because anyone reading c0 reads C.
 *
 * A `main` function is required, takes no parameters, and must return
 * one int (not void, not two). No function, extern or embed may be named
 * `_start`, the entry point, or like a register. The generated `_start` calls main and exits the
 * process with its return value. Every other function returns normally
 * to its caller, args in rdi/rsi/rdx/rcx/r8/r9 and the return value in
 * rax. A void function may only use a bare `return;`, or fall off the
 * end.
 *
 * c0 has one type, the 64-bit integer. A string literal is stored
 * null-terminated in .rodata and evaluates to its address, as a plain
 * int. A bare function name (an IDENT not followed by "(") evaluates to
 * that function's entry address the same way, so functions can be stored
 * in variables and passed around; `IDENT(args)` where IDENT is such a
 * variable becomes an indirect call through its value. There is no arity
 * checking on indirect calls - an int carries no arity - the caller is
 * trusted, like everywhere else in this language.
 *
 * The builtins are listed below. They are not real c0 functions, and the
 * names cannot be redefined.
 *
 *   print(s)           -- writes the null-terminated string at address s to
 *                          stdout (length computed at runtime by scanning for
 *                          the terminator)
 *   write(fd, ptr, n)  -- raw wrapper around the write(2) syscall
 *   read(fd, ptr, n)   -- raw wrapper around the read(2) syscall
 *   open_read(path)    -- open(2) with O_RDONLY; returns fd, or -errno
 *   open_write(path)   -- open(2) with O_WRONLY|O_CREAT|O_TRUNC, mode 0644
 *   close(fd)          -- close(2)
 *   alloc(n)           -- bump-allocates n bytes via the brk(2) syscall and
 *                          returns the address (fresh pages are kernel-zeroed;
 *                          nothing is ever freed)
 *   load8(addr)        -- the byte at addr, zero-extended
 *   store8(addr, v)    -- stores the low byte of v at addr; evaluates to v
 *   load64(addr)       -- the 64-bit value at addr
 *   store64(addr, v)   -- stores v (all 8 bytes) at addr; evaluates to v
 *   load32(addr)       -- the 32-bit value at addr, zero-extended
 *   store32(addr, v)   -- stores the low 4 bytes of v at addr; evaluates to v
 *   exit(code)         -- terminates the process immediately (exit(2) syscall)
 *   argc()             -- the process's argument count (argv[0] is the
 *                          program name, same convention as C)
 *   argv(i)            -- the address of the i'th argument string
 *                          (null-terminated); i must be < argc()
 *   outb(port, v)      -- x86 `out dx, al`, writes the low byte of v to
 *                          I/O port `port`; evaluates to v. Faults in
 *                          normal Linux userspace (needs ring 0 or IOPL);
 *                          this one is for freestanding/kernel targets.
 *   inb(port)          -- x86 `in al, dx`, reads a byte from I/O port
 *                          `port`, zero-extended. Same ring-0 caveat.
 *   outw / inw         -- the same, 16 bits wide.
 *
 * With --elf the output targets ring 3 of Moonshot, my OS kernel, instead
 * of Linux. It is the same backend with different syscall numbers. argc,
 * argv and the file builtins become that kernel's syscalls, the port
 * builtins are refused, and win_info, win_max, fill, blit, key_poll, ticks,
 * readfile, present, mouse_poll, glyph, dealloc, sound and writefile exist
 * beside them, plus
 * syscall(n, a, b, c, d, e) for any system call by number. Each is one
 * kernel syscall; see the BUILTINS table and gen_expr_node.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

static char *src_name;

/* Where the next error() points. Every phase keeps this current. The lexer
 * sets it at the start of each token, the parser at the last token it read, resolve
 * and codegen at the node they are working on, the declaration checks at
 * the declaration's name. A line of 0 means the error is about the file as
 * a whole ("no 'main' function defined"). c0/coff.c0 does the same, and
 * run_tests.sh holds the two compilers' messages equal. */
static const char *err_file;
static int err_line;

static void error(const char *fmt, ...) __attribute__((noreturn));
static void error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (err_line > 0) fprintf(stderr, "coff0: %s:%d: ", err_file, err_line);
  else fprintf(stderr, "coff0: %s: ", src_name);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

/* ---------------------------------------------------------------- lexer */

typedef enum {
  TK_NUM, TK_IDENT, TK_STRING,
  TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
  TK_ASSIGN, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
  TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_SEMI, TK_COMMA,
  TK_KW_INT, TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_RETURN,
  TK_EOF,
  TK_AND, TK_OR,
  TK_KW_BREAK, TK_KW_CONTINUE,
  TK_LBRACKET, TK_RBRACKET,
  TK_KW_VOID,
  TK_KW_EXTERN,
  TK_DOT,
  TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
  TK_KW_LAYOUT, TK_KW_SIZEOF,
  TK_NOT,
  TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
  TK_KW_FOR,
  TK_KW_DEFER,
} TokenKind;

typedef struct {
  TokenKind kind;
  long ival;
  char *name;
  const char *file; /* where the token starts, after include expansion has */
  int line;         /* been mapped back to the original files */
} Token;

/* Include expansion splices every file into one flat buffer, so a position
 * in that buffer says nothing about which file it came from. The expander
 * records a segment each time it starts copying from a file, entering an
 * include or coming back out of one, and loc_at() walks them forward. */
static long *seg_off;
static const char **seg_file;
static int *seg_line;
static int nseg, seg_cap;

static void add_seg(long off, const char *file, int line) {
  if (nseg >= seg_cap) {
    seg_cap = seg_cap ? seg_cap * 2 : 64;
    seg_off = realloc(seg_off, sizeof(long) * seg_cap);
    seg_file = realloc(seg_file, sizeof(char *) * seg_cap);
    seg_line = realloc(seg_line, sizeof(int) * seg_cap);
  }
  seg_off[nseg] = off;
  seg_file[nseg] = file;
  seg_line[nseg] = line;
  nseg++;
}

/* The location of byte `off` of the flat buffer. Only ever asked for
 * increasing offsets (the lexer moves forward), so it keeps a cursor. */
static int loc_seg, loc_line;
static long loc_off;
static void loc_at(const char *buf, long off, const char **file, int *line) {
  while (loc_seg + 1 < nseg && seg_off[loc_seg + 1] <= off) {
    loc_seg++;
    loc_off = seg_off[loc_seg];
    loc_line = seg_line[loc_seg];
  }
  while (loc_off < off) {
    if (buf[loc_off] == '\n') loc_line++;
    loc_off++;
  }
  *file = seg_file[loc_seg];
  *line = loc_line;
}

static Token *tokens;
static int ntokens;

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_char(int c) { return isalnum(c) || c == '_'; }

static Token *tokenize(char *src) {
  loc_seg = 0;
  loc_off = 0;
  loc_line = nseg > 0 ? seg_line[0] : 1;
  if (nseg == 0) add_seg(0, src_name, 1);
  int cap = 512;
  Token *out = malloc(sizeof(Token) * cap);
  int n = 0;
  char *p = src;

  while (*p) {
    if (isspace((unsigned char)*p)) { p++; continue; }
    if (p[0] == '/' && p[1] == '/') {
      while (*p && *p != '\n') p++;
      continue;
    }

    if (n >= cap) { cap *= 2; out = realloc(out, sizeof(Token) * cap); }
    Token *t = &out[n];
    loc_at(src, p - src, &t->file, &t->line);
    err_file = t->file;
    err_line = t->line;

    if (isdigit((unsigned char)*p)) {
      if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        /* The digits are added up in an unsigned long, which wraps modulo
         * 2^64, so a literal is what its low 64 bits are, reinterpreted as
         * signed. The c0-written lexers (lex.c0, parse.c0, coff.c0) do
         * `v = v * 16 + digit` on a plain int and wrap the same way.
         * strtoul would agree up to 2^64 and then clamp. Hex literals with
         * the top bit set are common, an 8x8 font glyph's packed bitmap such
         * as 0xF88484F88484F800. */
        unsigned long v = 0;
        while (isxdigit((unsigned char)*p)) {
          int c = (unsigned char)*p;
          int d = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
          v = v * 16 + (unsigned long)d;
          p++;
        }
        t->kind = TK_NUM;
        t->ival = (long)v;
        n++;
        continue;
      }
      /* Decimal the same way, wrapping modulo 2^64 like the c0-written
       * lexers' `v = v * 10 + digit`. c0 has no unsigned type, so a literal
       * at or past 2^63 is just what its bits are, reinterpreted as
       * negative, like every other c0 int. */
      unsigned long v = 0;
      while (isdigit((unsigned char)*p)) {
        v = v * 10 + (unsigned long)(*p - '0');
        p++;
      }
      t->kind = TK_NUM;
      t->ival = (long)v;
      n++;
      continue;
    }

    if (*p == '\'') {
      p++;
      long v;
      if (*p == '\\') {
        p++;
        switch (*p) {
          case 'n': v = '\n'; break;
          case 't': v = '\t'; break;
          case '\\': v = '\\'; break;
          case '\'': v = '\''; break;
          case '0': v = 0; break;
          case 0: error("unterminated character literal");
          default: error("unknown escape in character literal '\\%c'", *p);
        }
        p++;
      } else if (*p == 0 || *p == '\'') {
        error("empty or unterminated character literal");
      } else {
        v = (unsigned char)*p;
        p++;
      }
      if (*p != '\'') error("unterminated character literal");
      p++;
      t->kind = TK_NUM; /* a char literal is just a number with nicer syntax */
      t->ival = v;
      n++;
      continue;
    }

    if (*p == '"') {
      p++;
      char *buf = malloc(strlen(p) + 1);
      int len = 0;
      while (*p && *p != '"') {
        if (*p == '\\') {
          p++;
          switch (*p) {
            case 'n': buf[len++] = '\n'; break;
            case 't': buf[len++] = '\t'; break;
            case '\\': buf[len++] = '\\'; break;
            case '"': buf[len++] = '"'; break;
            case 0: error("unterminated string literal");
            default: error("unknown escape sequence '\\%c'", *p);
          }
          p++;
        } else {
          buf[len++] = *p++;
        }
      }
      if (*p != '"') error("unterminated string literal");
      p++;
      t->kind = TK_STRING;
      t->name = buf;
      t->ival = len;
      n++;
      continue;
    }

    if (is_ident_start(*p)) {
      char *start = p;
      while (is_ident_char(*p)) p++;
      int len = p - start;
      char *word = strndup(start, len);
      if (strcmp(word, "int") == 0) t->kind = TK_KW_INT;
      else if (strcmp(word, "if") == 0) t->kind = TK_KW_IF;
      else if (strcmp(word, "else") == 0) t->kind = TK_KW_ELSE;
      else if (strcmp(word, "while") == 0) t->kind = TK_KW_WHILE;
      else if (strcmp(word, "for") == 0) t->kind = TK_KW_FOR;
      else if (strcmp(word, "defer") == 0) t->kind = TK_KW_DEFER;
      else if (strcmp(word, "return") == 0) t->kind = TK_KW_RETURN;
      else if (strcmp(word, "break") == 0) t->kind = TK_KW_BREAK;
      else if (strcmp(word, "continue") == 0) t->kind = TK_KW_CONTINUE;
      else if (strcmp(word, "void") == 0) t->kind = TK_KW_VOID;
      else if (strcmp(word, "extern") == 0) t->kind = TK_KW_EXTERN;
      else if (strcmp(word, "layout") == 0) t->kind = TK_KW_LAYOUT;
      else if (strcmp(word, "sizeof") == 0) t->kind = TK_KW_SIZEOF;
      else { t->kind = TK_IDENT; t->name = word; }
      n++;
      continue;
    }

    switch (*p) {
      case '+': if (p[1] == '=') { t->kind = TK_PLUSEQ; p += 2; } else { t->kind = TK_PLUS; p++; } break;
      case '-': if (p[1] == '=') { t->kind = TK_MINUSEQ; p += 2; } else { t->kind = TK_MINUS; p++; } break;
      case '*': if (p[1] == '=') { t->kind = TK_STAREQ; p += 2; } else { t->kind = TK_STAR; p++; } break;
      case '/': if (p[1] == '=') { t->kind = TK_SLASHEQ; p += 2; } else { t->kind = TK_SLASH; p++; } break;
      case '%': if (p[1] == '=') { t->kind = TK_PERCENTEQ; p += 2; } else { t->kind = TK_PERCENT; p++; } break;
      case '(': t->kind = TK_LPAREN; p++; break;
      case ')': t->kind = TK_RPAREN; p++; break;
      case '{': t->kind = TK_LBRACE; p++; break;
      case '}': t->kind = TK_RBRACE; p++; break;
      case ';': t->kind = TK_SEMI; p++; break;
      case ',': t->kind = TK_COMMA; p++; break;
      case '[': t->kind = TK_LBRACKET; p++; break;
      case ']': t->kind = TK_RBRACKET; p++; break;
      case '.': t->kind = TK_DOT; p++; break;
      case '=':
        if (p[1] == '=') { t->kind = TK_EQ; p += 2; } else { t->kind = TK_ASSIGN; p++; }
        break;
      case '!':
        if (p[1] == '=') { t->kind = TK_NE; p += 2; break; }
        t->kind = TK_NOT; p++;
        break;
      case '<':
        if (p[1] == '=') { t->kind = TK_LE; p += 2; }
        else if (p[1] == '<') { t->kind = TK_SHL; p += 2; }
        else { t->kind = TK_LT; p++; }
        break;
      case '>':
        if (p[1] == '=') { t->kind = TK_GE; p += 2; }
        else if (p[1] == '>') { t->kind = TK_SHR; p += 2; }
        else { t->kind = TK_GT; p++; }
        break;
      case '&':
        if (p[1] == '&') { t->kind = TK_AND; p += 2; } else { t->kind = TK_AMP; p++; }
        break;
      case '|':
        if (p[1] == '|') { t->kind = TK_OR; p += 2; } else { t->kind = TK_PIPE; p++; }
        break;
      case '^': t->kind = TK_CARET; p++; break;
      case '~': t->kind = TK_TILDE; p++; break;
      default:
        error("unexpected character '%c'", *p);
    }
    n++;
  }

  if (n >= cap) { cap += 1; out = realloc(out, sizeof(Token) * cap); }
  out[n].kind = TK_EOF;
  loc_at(src, p - src, &out[n].file, &out[n].line);
  n++;

  ntokens = n;
  return out;
}

/* --------------------------------------------------------------- parser */

typedef enum {
  ND_NUM, ND_STR, ND_VAR, ND_ASSIGN, ND_DECL, ND_CALL,
  ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
  ND_EQ, ND_NE, ND_LT, ND_LE, ND_GT, ND_GE,
  ND_NEG,
  ND_IF, ND_WHILE, ND_RETURN, ND_EXPR_STMT, ND_BLOCK,
  ND_AND, ND_OR,
  ND_BREAK, ND_CONTINUE,
  ND_INDEX, ND_INDEX_ASSIGN,
  ND_FUNCREF,
  ND_FIELD, ND_FIELD_ASSIGN, ND_SIZEOF,
  ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR, ND_BITNOT,
  ND_FORLOOP,
  ND_MULTIASSIGN, /* a, b = f(...) -- lhs = call, cond = target1 VAR, rhs = target2 VAR */
  ND_DEFER,       /* defer <stmt>; -- lhs = the deferred statement, run at every exit */
} NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *lhs, *rhs;    /* binary ops; ASSIGN/DECL rhs = value expr (may be NULL for DECL);
                         INDEX/INDEX_ASSIGN lhs = base pointer expr, rhs = index expr */
  long ival;          /* NUM literal; resolved stack offset for VAR/ASSIGN/DECL/(indirect) CALL */
  int is_global;      /* VAR/ASSIGN/(indirect) CALL whose name resolved to a global, not a stack slot */
  int is_indirect;    /* CALL whose `name` is a function-pointer-valued variable, not a
                         declared function -- call through its value (see resolve()) */
  int str_id;         /* STR, index into the string-literal table */
  char *name;         /* VAR / ASSIGN / DECL / CALL / FUNCREF */
  Node *cond, *then, *els; /* IF; INDEX_ASSIGN cond = value expr (`buf[i] = value`) */
  Node *body;         /* WHILE body; BLOCK head of statement list */
  Node *args;         /* CALL, head of argument-expression list (chained via next) */
  int nargs;          /* CALL, argument count */
  Node *next;         /* next statement in a BLOCK, or next arg in a CALL's args list */
  char *type;         /* DECL, the layout it was declared with (`Point p`), or NULL for int */
  const char *file;   /* where it came from, for error messages */
  int line;
};

#define MAX_PARAMS 6          /* register-passed params (rdi/rsi/rdx/rcx/r8/r9) */
#define MAX_TOTAL_PARAMS 16   /* overall cap on declared params, register + stack */

typedef struct Param Param;
struct Param {
  char *name;
  char *type;
  Param *next;
};

typedef struct Function Function;
struct Function {
  char *name;
  char *params[MAX_PARAMS]; /* first MAX_PARAMS params (register-passed) */
  char *ptypes[MAX_PARAMS]; /* their layout types, NULL for int */
  Param *extra_params;      /* params beyond MAX_PARAMS (stack-passed), in order */
  int nparams;
  int is_void;      /* declared "void" instead of "int" -- may not return a value */
  int two_ret;      /* declared "(int, int)" -- returns two values, in rax and rdx */
  Node *body;
  long stack_size; /* filled in by resolve_function() */
  int ret_label;    /* filled in just before codegen */
  Function *next;
  const char *file; /* the name's location, for error messages */
  int line;
};

/* Name of the i'th declared parameter (0-indexed), regardless of whether it
 * lives in the inline register-param slots or the extra_params list. */
static char *param_name(Function *f, int i) {
  if (i < MAX_PARAMS) return f->params[i];
  Param *p = f->extra_params;
  for (int k = i - MAX_PARAMS; k > 0; k--) p = p->next;
  return p->name;
}

static char *param_type(Function *f, int i) {
  if (i < MAX_PARAMS) return f->ptypes[i];
  Param *p = f->extra_params;
  for (int k = i - MAX_PARAMS; k > 0; k--) p = p->next;
  return p->type;
}

typedef struct Global Global;
struct Global {
  char *name;
  char *type;       /* the layout it was declared with, or NULL for int */
  long init;
  Global *next;
  const char *file; /* the name's location, for error messages */
  int line;
};

/* `embed name "file";` -- the file's bytes, placed in the program as data by
 * the assembler (.incbin). `name` is their address and `name_size` their
 * length. The compiler never opens the file. The size is the distance
 * between two labels, worked out by the assembler, so the compiler's output
 * depends only on its input text. The path is relative to the source file
 * that says `embed`, like an include. */
typedef struct Embed Embed;
struct Embed {
  char *name;
  char *path;       /* as written */
  char *resolved;   /* joined with the source file's directory */
  Embed *next;
  const char *file;
  int line;
};
static Embed *prog_embeds;
static char *join_path(const char *dir, const char *rel);
static char *dirname_with_slash(const char *path);

typedef struct LayoutField LayoutField;
struct LayoutField {
  char *name;
  LayoutField *next;
  const char *file;
  int line;
};

typedef struct Layout Layout;
struct Layout {
  char *name;
  LayoutField *fields;
  int nfields;
  Layout *next;
  const char *file;
  int line;
};

/* The parser's position, defined here so new_node() can see it. A node is
 * placed at the last token read when it is made, which is the operator,
 * keyword or name it stands for. */
static int pos;

/* Points the next error at the last token the parser read, which is where a missing
 * ';' or ')' should have been, or the token that started the bad construct. */
static void at_prev(void) {
  Token *t = &tokens[pos > 0 ? pos - 1 : 0];
  err_file = t->file;
  err_line = t->line;
}

static Node *new_node(NodeKind kind) {
  Node *n = calloc(1, sizeof(Node));
  n->kind = kind;
  Token *t = &tokens[pos > 0 ? pos - 1 : 0];
  n->file = t->file;
  n->line = t->line;
  return n;
}

#define MAX_STRINGS 4096
static char *string_data[MAX_STRINGS];
static int string_len[MAX_STRINGS];
static int string_count;

static int register_string(char *decoded, int len) {
  if (string_count >= MAX_STRINGS) at_prev();
  if (string_count >= MAX_STRINGS) error("too many string literals (max %d)", MAX_STRINGS);
  string_data[string_count] = decoded;
  string_len[string_count] = len;
  return string_count++;
}

static Token *peek(void) { return &tokens[pos]; }
static Token *advance(void) { return &tokens[pos++]; }
/* A layout name used as a type, as in `Point p`. Two identifiers in a row can only
 * be that, since no expression starts with two names. */
static int at_typed_decl(void) {
  return tokens[pos].kind == TK_IDENT && tokens[pos + 1].kind == TK_IDENT;
}
static int check(TokenKind k) { return peek()->kind == k; }
static Token *expect(TokenKind k, const char *what) {
  if (!check(k)) { at_prev(); error("expected %s", what); }
  return advance();
}

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_simple(int want_semi);
static Node *parse_block(void);

static Node *parse_primary(void) {
  if (check(TK_LPAREN)) {
    advance();
    Node *n = parse_expr();
    expect(TK_RPAREN, "')'");
    return n;
  }
  if (check(TK_NUM)) {
    Node *n = new_node(ND_NUM);
    n->ival = advance()->ival;
    return n;
  }
  if (check(TK_STRING)) {
    Token *t = advance();
    Node *n = new_node(ND_STR);
    n->str_id = register_string(t->name, (int)t->ival);
    return n;
  }
  if (check(TK_IDENT)) {
    char *name = advance()->name;
    if (check(TK_LPAREN)) {
      advance();
      Node *n = new_node(ND_CALL);
      n->name = name;
      Node head = {0};
      Node *tail = &head;
      if (!check(TK_RPAREN)) {
        for (;;) {
          tail->next = parse_expr();
          tail = tail->next;
          n->nargs++;
          if (!check(TK_COMMA)) break;
          advance();
        }
      }
      expect(TK_RPAREN, "')'");
      n->args = head.next;
      return n;
    }
    Node *n = new_node(ND_VAR);
    n->name = name;
    return n;
  }
  if (check(TK_KW_SIZEOF)) {
    advance();
    expect(TK_LPAREN, "'('");
    Node *n = new_node(ND_SIZEOF);
    n->name = expect(TK_IDENT, "a layout name")->name;
    expect(TK_RPAREN, "')'");
    return n;
  }
  at_prev();
  error("expected an expression");
  return NULL;
}

/* postfix := primary (("[" expr "]") | ("." IDENT))*   -- buf[i] is sugar
 * over load8(buf+i); e.field is sugar over load64(e + field's offset), the
 * offset coming from whichever `layout` declared that field name (see the
 * global field-offset table built in main()). Chains (buf[i][j], e.f.g) are
 * allowed for free by the grammar, same "an int that happens to be a valid
 * address" spirit as everything else in c0. */
static Node *parse_postfix(void) {
  Node *n = parse_primary();
  for (;;) {
    if (check(TK_LBRACKET)) {
      advance();
      Node *idx = new_node(ND_INDEX);
      idx->lhs = n;
      idx->rhs = parse_expr();
      expect(TK_RBRACKET, "']'");
      n = idx;
      continue;
    }
    if (check(TK_DOT)) {
      advance();
      Node *f = new_node(ND_FIELD);
      f->lhs = n;
      f->name = expect(TK_IDENT, "a field name")->name;
      n = f;
      continue;
    }
    break;
  }
  return n;
}

static Node *parse_unary(void) {
  if (check(TK_MINUS)) {
    advance();
    Node *n = new_node(ND_NEG);
    n->lhs = parse_unary();
    return n;
  }
  if (check(TK_TILDE)) {
    advance();
    Node *n = new_node(ND_BITNOT);
    n->lhs = parse_unary();
    return n;
  }
  /* !E is sugar for (E == 0), so no new node kind or codegen is needed. */
  if (check(TK_NOT)) {
    advance();
    Node *n = new_node(ND_EQ);
    n->lhs = parse_unary();
    Node *z = new_node(ND_NUM);
    z->ival = 0;
    n->rhs = z;
    return n;
  }
  return parse_postfix();
}

static Node *parse_term(void) {
  Node *n = parse_unary();
  while (check(TK_STAR) || check(TK_SLASH) || check(TK_PERCENT)) {
    NodeKind k = check(TK_STAR) ? ND_MUL : check(TK_SLASH) ? ND_DIV : ND_MOD;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_unary();
    n = b;
  }
  return n;
}

static Node *parse_additive(void) {
  Node *n = parse_term();
  while (check(TK_PLUS) || check(TK_MINUS)) {
    NodeKind k = check(TK_PLUS) ? ND_ADD : ND_SUB;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_term();
    n = b;
  }
  return n;
}

/* The bitwise operators sit where C puts them, including the famous wart
 * that & ^ | bind more loosely than == and !=, so `x & 1 == 0` means
 * `x & (1 == 0)` here exactly as it does in C. Matching C costs nothing to
 * anyone who reads C, and inventing a "fixed" order would trip all of them. */
static Node *parse_shift(void) {
  Node *n = parse_additive();
  while (check(TK_SHL) || check(TK_SHR)) {
    NodeKind k = check(TK_SHL) ? ND_SHL : ND_SHR;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_additive();
    n = b;
  }
  return n;
}

static Node *parse_relational(void) {
  Node *n = parse_shift();
  for (;;) {
    NodeKind k;
    if (check(TK_LT)) k = ND_LT;
    else if (check(TK_LE)) k = ND_LE;
    else if (check(TK_GT)) k = ND_GT;
    else if (check(TK_GE)) k = ND_GE;
    else break;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_shift();
    n = b;
  }
  return n;
}

static Node *parse_equality(void) {
  Node *n = parse_relational();
  while (check(TK_EQ) || check(TK_NE)) {
    NodeKind k = check(TK_EQ) ? ND_EQ : ND_NE;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_relational();
    n = b;
  }
  return n;
}

static Node *parse_bitand(void) {
  Node *n = parse_equality();
  while (check(TK_AMP)) {
    advance();
    Node *b = new_node(ND_BITAND);
    b->lhs = n; b->rhs = parse_equality();
    n = b;
  }
  return n;
}

static Node *parse_bitxor(void) {
  Node *n = parse_bitand();
  while (check(TK_CARET)) {
    advance();
    Node *b = new_node(ND_BITXOR);
    b->lhs = n; b->rhs = parse_bitand();
    n = b;
  }
  return n;
}

static Node *parse_bitor(void) {
  Node *n = parse_bitxor();
  while (check(TK_PIPE)) {
    advance();
    Node *b = new_node(ND_BITOR);
    b->lhs = n; b->rhs = parse_bitxor();
    n = b;
  }
  return n;
}

static Node *parse_logic_and(void) {
  Node *n = parse_bitor();
  while (check(TK_AND)) {
    advance();
    Node *b = new_node(ND_AND);
    b->lhs = n; b->rhs = parse_bitor();
    n = b;
  }
  return n;
}

static Node *parse_logic_or(void) {
  Node *n = parse_logic_and();
  while (check(TK_OR)) {
    advance();
    Node *b = new_node(ND_OR);
    b->lhs = n; b->rhs = parse_logic_and();
    n = b;
  }
  return n;
}

static Node *parse_expr(void) {
  return parse_logic_or();
}

static Node *parse_stmt(void) {
  if (check(TK_LBRACE)) return parse_block();

  if (check(TK_KW_INT) || at_typed_decl()) {
    char *type = NULL;
    if (check(TK_IDENT)) type = advance()->name; else advance();
    Node *n = new_node(ND_DECL);
    n->type = type;
    n->name = expect(TK_IDENT, "an identifier")->name;
    if (check(TK_ASSIGN)) {
      advance();
      n->rhs = parse_expr();
    }
    expect(TK_SEMI, "';'");
    return n;
  }

  if (check(TK_KW_IF)) {
    advance();
    expect(TK_LPAREN, "'('");
    Node *n = new_node(ND_IF);
    n->cond = parse_expr();
    expect(TK_RPAREN, "')'");
    n->then = parse_stmt();
    if (check(TK_KW_ELSE)) {
      advance();
      n->els = parse_stmt();
    }
    return n;
  }

  if (check(TK_KW_WHILE)) {
    advance();
    expect(TK_LPAREN, "'('");
    Node *n = new_node(ND_WHILE);
    n->cond = parse_expr();
    expect(TK_RPAREN, "')'");
    n->body = parse_stmt();
    return n;
  }

  if (check(TK_KW_RETURN)) {
    advance();
    Node *n = new_node(ND_RETURN);
    if (!check(TK_SEMI)) {
      n->lhs = parse_expr();
      /* return a, b; -- two-value return (rhs set marks it). */
      if (check(TK_COMMA)) { advance(); n->rhs = parse_expr(); }
    }
    expect(TK_SEMI, "';'");
    return n;
  }

  if (check(TK_KW_DEFER)) {
    advance();
    Node *n = new_node(ND_DEFER);
    n->lhs = parse_stmt(); /* the statement to run at every exit */
    return n;
  }

  if (check(TK_KW_BREAK)) {
    advance();
    expect(TK_SEMI, "';'");
    return new_node(ND_BREAK);
  }

  if (check(TK_KW_CONTINUE)) {
    advance();
    expect(TK_SEMI, "';'");
    return new_node(ND_CONTINUE);
  }

  /* for (init; cond; step) body parses to a BLOCK holding the init statement
   * then a FORLOOP node, so init's declarations are scoped to the loop and
   * `continue` runs the step. The step reuses parse_simple with no ';'. */
  if (check(TK_KW_FOR)) {
    advance();
    expect(TK_LPAREN, "'('");
    Node *init = parse_stmt();
    Node *cond = parse_expr();
    expect(TK_SEMI, "';'");
    Node *step = parse_simple(0);
    expect(TK_RPAREN, "')'");
    Node *body = parse_stmt();
    Node *loop = new_node(ND_FORLOOP);
    loop->cond = cond;
    loop->body = body;
    loop->then = step;
    Node *blk = new_node(ND_BLOCK);
    init->next = loop;
    blk->body = init;
    return blk;
  }

  return parse_simple(1);
}

/* A plain/indexed/field assignment, a compound assignment, or -- if no
 * assignment follows -- an expression statement. Needs arbitrary lookahead
 * (buf[i] can be any length). want_semi is 0 for a for-loop step, which has
 * ')' after it instead of ';'. */
static Node *parse_simple(int want_semi) {
  Node *e = parse_expr();
  /* q, r = f(...) -- a two-value assignment from a call that returns two. */
  if (check(TK_COMMA)) {
    if (e->kind != ND_VAR) { at_prev(); error("left of ',' in a two-value assignment must be a variable"); }
    advance();
    Node *t2 = parse_expr();
    if (t2->kind != ND_VAR) { at_prev(); error("both targets of a two-value assignment must be variables"); }
    expect(TK_ASSIGN, "'='");
    Node *call = parse_expr();
    if (call->kind != ND_CALL) { at_prev(); error("a two-value assignment needs a function call on the right"); }
    Node *n = new_node(ND_MULTIASSIGN);
    n->lhs = call;   /* the two-value call */
    n->cond = e;     /* target 1 */
    n->rhs = t2;     /* target 2 */
    if (want_semi) expect(TK_SEMI, "';'");
    return n;
  }
  if (check(TK_ASSIGN)) {
    advance();
    if (e->kind == ND_VAR) {
      Node *n = new_node(ND_ASSIGN);
      n->name = e->name;
      n->rhs = parse_expr();
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_INDEX) {
      Node *n = new_node(ND_INDEX_ASSIGN);
      n->lhs = e->lhs;
      n->rhs = e->rhs;
      n->cond = parse_expr();
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_FIELD) {
      Node *n = new_node(ND_FIELD_ASSIGN);
      n->lhs = e->lhs;
      n->name = e->name;
      n->rhs = parse_expr();
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    at_prev();
    error("invalid assignment target");
  }

  /* A compound assignment lhs OP= rhs is sugar for lhs = (lhs OP rhs), which
   * builds the same AST a hand-written lhs = lhs OP rhs would, so codegen
   * needs nothing of its own for it, and an index or field target is
   * evaluated twice exactly as in the hand-written form. */
  if (check(TK_PLUSEQ) || check(TK_MINUSEQ) || check(TK_STAREQ)
      || check(TK_SLASHEQ) || check(TK_PERCENTEQ)) {
    NodeKind bk = ND_ADD;
    if (check(TK_MINUSEQ)) bk = ND_SUB;
    if (check(TK_STAREQ)) bk = ND_MUL;
    if (check(TK_SLASHEQ)) bk = ND_DIV;
    if (check(TK_PERCENTEQ)) bk = ND_MOD;
    advance();
    Node *bin = new_node(bk);
    bin->lhs = e;
    bin->rhs = parse_expr();
    if (e->kind == ND_VAR) {
      Node *n = new_node(ND_ASSIGN);
      n->name = e->name;
      n->rhs = bin;
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_INDEX) {
      Node *n = new_node(ND_INDEX_ASSIGN);
      n->lhs = e->lhs;
      n->rhs = e->rhs;
      n->cond = bin;
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_FIELD) {
      Node *n = new_node(ND_FIELD_ASSIGN);
      n->lhs = e->lhs;
      n->name = e->name;
      n->rhs = bin;
      if (want_semi) expect(TK_SEMI, "';'");
      return n;
    }
    at_prev();
    error("invalid assignment target");
  }

  Node *n = new_node(ND_EXPR_STMT);
  n->lhs = e;
  if (want_semi) expect(TK_SEMI, "';'");
  return n;
}

static Node *parse_block(void) {
  expect(TK_LBRACE, "'{'");
  Node *blk = new_node(ND_BLOCK);
  Node head = {0};
  Node *tail = &head;
  while (!check(TK_RBRACE)) {
    tail->next = parse_stmt();
    tail = tail->next;
  }
  expect(TK_RBRACE, "'}'");
  blk->body = head.next;
  return blk;
}

static Function *parse_function(char *name) {
  Function *f = calloc(1, sizeof(Function));
  f->name = name;
  expect(TK_LPAREN, "'('");
  if (!check(TK_RPAREN)) {
    Param *extra_tail = NULL;
    for (;;) {
      char *ptype = NULL;
      if (at_typed_decl()) ptype = advance()->name;
      else expect(TK_KW_INT, "'int' or a layout name");
      if (f->nparams >= MAX_TOTAL_PARAMS) at_prev();
      if (f->nparams >= MAX_TOTAL_PARAMS) error("too many parameters for '%s' (max %d)", f->name, MAX_TOTAL_PARAMS);
      char *pname = expect(TK_IDENT, "a parameter name")->name;
      if (f->nparams < MAX_PARAMS) {
        f->params[f->nparams] = pname;
        f->ptypes[f->nparams] = ptype;
      } else {
        Param *p = calloc(1, sizeof(Param));
        p->name = pname;
        p->type = ptype;
        if (extra_tail) extra_tail->next = p; else f->extra_params = p;
        extra_tail = p;
      }
      f->nparams++;
      if (!check(TK_COMMA)) break;
      advance();
    }
  }
  expect(TK_RPAREN, "')'");
  f->body = parse_block();
  return f;
}

/* A global's initializer is folded at compile time, so it may be any
 * constant expression, meaning numbers and character literals joined by unary - and
 * ~ and the arithmetic and bitwise operators, in parentheses or not. The
 * arithmetic is the generated code's. / and % truncate toward zero like
 * idiv, << and >> use the count's low six bits, >> is arithmetic, and
 * overflow wraps (done unsigned here, where C would call it undefined).
 * parse.c0 and coff.c0 fold the same way, and the AST dump shows the value. */
static long const_fold(Node *n) {
  long a, b;
  switch (n->kind) {
    case ND_NUM: return n->ival;
    case ND_NEG: return (long)(0UL - (unsigned long)const_fold(n->lhs));
    case ND_BITNOT: return ~const_fold(n->lhs);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
      a = const_fold(n->lhs);
      b = const_fold(n->rhs);
      switch (n->kind) {
        case ND_ADD: return (long)((unsigned long)a + (unsigned long)b);
        case ND_SUB: return (long)((unsigned long)a - (unsigned long)b);
        case ND_MUL: return (long)((unsigned long)a * (unsigned long)b);
        case ND_DIV:
        case ND_MOD:
          if (b == 0) { at_prev(); error("division by zero in a global initializer"); }
          if (b == -1 && a == (long)(1UL << 63)) { at_prev(); error("division overflow in a global initializer"); }
          return n->kind == ND_DIV ? a / b : a % b;
        case ND_BITAND: return a & b;
        case ND_BITOR: return a | b;
        case ND_BITXOR: return a ^ b;
        case ND_SHL: return (long)((unsigned long)a << (b & 63));
        default: return a >> (b & 63);
      }
    default:
      at_prev();
      error("a global initializer must be a constant: numbers, characters and operators");
      return 0;
  }
}

static Global *parse_global(char *name) {
  Global *g = calloc(1, sizeof(Global));
  g->name = name;
  if (check(TK_ASSIGN)) {
    advance();
    g->init = const_fold(parse_expr());
  }
  expect(TK_SEMI, "';'");
  return g;
}

static Function *prog_functions;
static Global *prog_globals;
static Global *prog_externs;
static Layout *prog_layouts;

static Global *parse_extern(char *name) {
  Global *e = calloc(1, sizeof(Global));
  e->name = name;
  e->init = 0;
  return e;
}

/* layout := "layout" IDENT "{" ("int" IDENT ";")* "}"   -- every field is
 * "int" (c0 has only one type), spelled out the same way a local/global
 * declaration is, so the grammar does not grow a second way to say "a field
 * named x" beyond what parse_stmt's decl_stmt already knows. */
static Layout *parse_layout(void) {
  Layout *l = calloc(1, sizeof(Layout));
  Token *nt = expect(TK_IDENT, "a layout name");
  l->name = nt->name;
  l->file = nt->file;
  l->line = nt->line;
  expect(TK_LBRACE, "'{'");
  LayoutField head = {0};
  LayoutField *tail = &head;
  while (!check(TK_RBRACE)) {
    expect(TK_KW_INT, "'int' (every layout field is an int)");
    LayoutField *f = calloc(1, sizeof(LayoutField));
    Token *ft = expect(TK_IDENT, "a field name");
    f->name = ft->name;
    f->file = ft->file;
    f->line = ft->line;
    expect(TK_SEMI, "';'");
    tail->next = f;
    tail = f;
    l->nfields++;
  }
  expect(TK_RBRACE, "'}'");
  l->fields = head.next;
  return l;
}

static void parse_program(void) {
  Function fhead = {0};
  Function *ftail = &fhead;
  Global ghead = {0};
  Global *gtail = &ghead;
  Global ehead = {0};
  Global *etail = &ehead;
  Layout lhead = {0};
  Layout *ltail = &lhead;
  Embed embhead = {0};
  Embed *embtail = &embhead;
  while (!check(TK_EOF)) {
    /* `embed` is a word, not a keyword. Only `embed name "file";` at the top
     * level means this, so nothing else named embed is affected. */
    if (check(TK_IDENT) && strcmp(peek()->name, "embed") == 0
        && tokens[pos + 1].kind == TK_IDENT && tokens[pos + 2].kind == TK_STRING) {
      advance();
      Token *nt = advance();
      Token *pt = advance();
      expect(TK_SEMI, "';'");
      Embed *e = calloc(1, sizeof(Embed));
      e->name = nt->name;
      e->path = pt->name;
      /* The whole path the assembler will be given, the source file's own
       * directory included, since it goes between quotes in .incbin. */
      e->resolved = join_path(dirname_with_slash(nt->file), e->path);
      for (char *c = e->resolved; *c; c++) {
        if (*c == '"' || *c == '\\' || *c == '\n') { at_prev(); error("embed: the path may not contain quotes, backslashes or newlines"); }
      }
      e->file = nt->file;
      e->line = nt->line;
      embtail->next = e;
      embtail = e;
      continue;
    }
    if (check(TK_KW_EXTERN)) {
      advance();
      expect(TK_KW_INT, "'int' after 'extern'");
      Token *nt = expect(TK_IDENT, "a name");
      expect(TK_SEMI, "';'");
      etail->next = parse_extern(nt->name);
      etail = etail->next;
      etail->file = nt->file;
      etail->line = nt->line;
      continue;
    }
    if (check(TK_KW_LAYOUT)) {
      advance();
      ltail->next = parse_layout();
      ltail = ltail->next;
      continue;
    }
    /* (int, int) name(...) -- a function returning two values. */
    if (check(TK_LPAREN)) {
      advance();
      expect(TK_KW_INT, "'int'");
      expect(TK_COMMA, "',' (a two-value return is '(int, int)')");
      expect(TK_KW_INT, "'int'");
      expect(TK_RPAREN, "')'");
      Token *nt = expect(TK_IDENT, "a function name");
      if (!check(TK_LPAREN)) { at_prev(); error("'(int, int)' must be followed by a function"); }
      ftail->next = parse_function(nt->name);
      ftail->next->two_ret = 1;
      ftail = ftail->next;
      ftail->file = nt->file;
      ftail->line = nt->line;
      continue;
    }
    int is_void = 0;
    char *gtype = NULL;
    if (check(TK_KW_VOID)) { is_void = 1; advance(); }
    else if (at_typed_decl()) gtype = advance()->name;
    else expect(TK_KW_INT, "'int' or 'void'");
    Token *nt = expect(TK_IDENT, "a name");
    char *name = nt->name;
    if (gtype && check(TK_LPAREN)) { at_prev(); error("a function returns int or void, not a layout"); }
    if (check(TK_LPAREN)) {
      ftail->next = parse_function(name);
      ftail->next->is_void = is_void;
      ftail = ftail->next;
      ftail->file = nt->file;
      ftail->line = nt->line;
    } else {
      if (is_void) { at_prev(); error("'void' is not a valid type for a global"); }
      gtail->next = parse_global(name);
      gtail = gtail->next;
      gtail->type = gtype;
      gtail->file = nt->file;
      gtail->line = nt->line;
    }
  }
  prog_functions = fhead.next;
  prog_globals = ghead.next;
  prog_externs = ehead.next;
  prog_layouts = lhead.next;
  prog_embeds = embhead.next;
}

/* ------------------------------------------------------------- resolve */
/* Two jobs. (1) A first pass over every function collects name+arity into a
 * signature table so calls can reference functions regardless of source
 * order (including mutual recursion); (2) a per-function pass assigns every
 * declared variable (including parameters) a stack slot and checks that
 * every reference/assignment/call refers to something already declared with
 * the right arity. Blocks are scopes, see scope_marks. */

#define MAX_VARS 4096
#define MAX_FUNCS 4096

typedef struct { const char *name; int nargs; } Builtin;
static const Builtin BUILTINS[] = {
  { "print", 1 }, { "write", 3 }, { "read", 3 },
  { "open_read", 1 }, { "open_write", 1 }, { "close", 1 },
  { "alloc", 1 }, { "load8", 1 }, { "store8", 2 },
  { "load32", 1 }, { "store32", 2 },
  { "load64", 1 }, { "store64", 2 }, { "exit", 1 },
  { "argc", 0 }, { "argv", 1 },
  { "outb", 2 }, { "inb", 1 },
  { "outw", 2 }, { "inw", 1 },
  /* Moonshot ring-3 builtins, --elf only, each one Moonshot syscall. */
  { "win_info", 0 }, { "win_max", 0 }, { "fill", 5 }, { "blit", 5 },
  { "key_poll", 0 }, { "ticks", 0 }, { "readfile", 3 }, { "present", 1 },
  { "mouse_poll", 0 }, { "glyph", 2 }, { "dealloc", 1 },
  { "sound", 2 }, { "writefile", 3 },
  { "syscall", 6 },
};
#define NBUILTINS ((int)(sizeof(BUILTINS) / sizeof(BUILTINS[0])))

/* Functions, externs and embeds become assembler symbols under their own
 * names, so one named like a register would be read as the register (`call
 * rax` calls through rax). Such names are refused. The same list is in
 * c0/coff.c0. */
static const char *REGISTER_NAMES[] = {
  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
  "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
  "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
  "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
  "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
  "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh", "sil", "dil", "bpl", "spl",
  "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
  "rip", "eip", "cs", "ds", "es", "fs", "gs", "ss",
  "cr0", "cr2", "cr3", "cr4", "cr8",
};
#define NREGISTER_NAMES ((int)(sizeof(REGISTER_NAMES) / sizeof(REGISTER_NAMES[0])))

/* _start is the entry point every program gets, so a second one would only
 * fail later, in the assembler. */
static void check_symbol_name(const char *name, const char *what) {
  for (int i = 0; i < NREGISTER_NAMES; i++) {
    if (strcmp(REGISTER_NAMES[i], name) == 0) error("'%s' is a register name and cannot name %s", name, what);
  }
  if (strcmp(name, "_start") == 0) error("'_start' is the entry point and cannot name %s", what);
}

static const Builtin *find_builtin(const char *name) {
  for (int i = 0; i < NBUILTINS; i++) {
    if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
  }
  return NULL;
}

static char *func_names[MAX_FUNCS];
static int func_nparams[MAX_FUNCS];
static int func_two_ret[MAX_FUNCS];
static int func_count;

static void func_declare_sig(char *name, int nparams, int two_ret) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) error("redefinition of function '%s'", name);
  }
  if (func_count >= MAX_FUNCS) error("too many functions (max %d)", MAX_FUNCS);
  func_names[func_count] = name;
  func_nparams[func_count] = nparams;
  func_two_ret[func_count] = two_ret;
  func_count++;
}

static int func_lookup_nparams(char *name) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) return func_nparams[i];
  }
  error("call to undeclared function '%s'", name);
  return 0;
}

static int func_lookup_two_ret(char *name) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) return func_two_ret[i];
  }
  return 0;
}

static int func_exists(char *name) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) return 1;
  }
  return 0;
}

static char *glob_names[MAX_VARS];
static int glob_count;

static void glob_declare(char *name) {
  for (int i = 0; i < glob_count; i++) {
    if (strcmp(glob_names[i], name) == 0) error("redefinition of global '%s'", name);
  }
  if (glob_count >= MAX_VARS) error("too many globals (max %d)", MAX_VARS);
  glob_names[glob_count++] = name;
}

static int glob_exists(char *name) {
  for (int i = 0; i < glob_count; i++) {
    if (strcmp(glob_names[i], name) == 0) return 1;
  }
  return 0;
}

static char *extern_names[256];
static int extern_count;

static int extern_exists(char *name) {
  for (int i = 0; i < extern_count; i++) {
    if (strcmp(extern_names[i], name) == 0) return 1;
  }
  return 0;
}

static void extern_declare(char *name) {
  for (int i = 0; i < extern_count; i++) {
    if (strcmp(extern_names[i], name) == 0) error("redefinition of extern '%s'", name);
  }
  if (extern_count >= 256) error("too many externs (max 256)");
  extern_names[extern_count++] = name;
}

/* Every field name is recorded once, with the offset the first layout gave
 * it. That table serves bases whose layout is unknown (untyped variables and
 * expressions). Layouts may give a name different offsets; such a name is
 * marked ambiguous and can then only be used through a variable declared
 * with its layout (`Point p`), which looks fields up in that layout alone
 * (layout_field_offset). */
#define MAX_FIELDS 4096
static char *field_names[MAX_FIELDS];
static long field_offsets[MAX_FIELDS];
static int field_ambiguous[MAX_FIELDS]; /* layouts disagree on its offset */
static int field_count;

/* The offset of `name` for a base whose layout is not known, which is an
 * untyped variable or an expression. Fine for a name every layout that has
 * it puts at the same offset; ambiguous otherwise, and then the variable has
 * to say which layout it is (`Point p`, see layout_field_offset). */
static long field_lookup(const char *name) {
  for (int i = 0; i < field_count; i++) {
    if (strcmp(field_names[i], name) == 0) {
      if (field_ambiguous[i]) {
        error("field '%s' is at different offsets in different layouts; "
              "declare the variable with its layout, e.g. 'Point p'", name);
      }
      return field_offsets[i];
    }
  }
  error("unknown field '%s' (not declared in any layout)", name);
  return -1;
}

static void field_declare_or_check(const char *name, long offset) {
  for (int i = 0; i < field_count; i++) {
    if (strcmp(field_names[i], name) == 0) {
      if (field_offsets[i] != offset) field_ambiguous[i] = 1;
      return;
    }
  }
  if (field_count >= MAX_FIELDS) error("too many distinct field names (max %d)", MAX_FIELDS);
  field_names[field_count] = (char *)name;
  field_offsets[field_count] = offset;
  field_ambiguous[field_count] = 0;
  field_count++;
}

#define MAX_LAYOUTS 256
static char *layout_names[MAX_LAYOUTS];
static long layout_sizes[MAX_LAYOUTS];
static int layout_count;

static int layout_exists(const char *name) {
  for (int i = 0; i < layout_count; i++) {
    if (strcmp(layout_names[i], name) == 0) return 1;
  }
  return 0;
}

/* The offset of `field` in layout `lname`, for a base declared with that
 * layout (`Point p; p.x`). */
static long layout_field_offset(const char *lname, const char *field) {
  for (Layout *l = prog_layouts; l; l = l->next) {
    if (strcmp(l->name, lname) != 0) continue;
    long off = 0;
    for (LayoutField *f = l->fields; f; f = f->next) {
      if (strcmp(f->name, field) == 0) return off;
      off += 8;
    }
    error("layout '%s' has no field '%s'", lname, field);
  }
  error("unknown layout '%s'", lname);
  return -1;
}

static void check_type(const char *type) {
  if (type && !layout_exists(type)) error("unknown type '%s' (a type is int or a layout name)", type);
}

static long layout_lookup_size(const char *name) {
  for (int i = 0; i < layout_count; i++) {
    if (strcmp(layout_names[i], name) == 0) return layout_sizes[i];
  }
  error("unknown layout '%s' in sizeof(...)", name);
  return -1;
}

/* Validates every `layout` declaration (called once, up front, same spot
 * globals/externs are declared). It checks for duplicate field names within
 * one layout, registers each field's offset in the shared field table
 * (erroring on a cross-layout conflict), and records the layout's own
 * size (nfields * 8) for sizeof(...). */
static void layout_declare(Layout *l) {
  err_file = l->file;
  err_line = l->line;
  for (int i = 0; i < layout_count; i++) {
    if (strcmp(layout_names[i], l->name) == 0) error("redefinition of layout '%s'", l->name);
  }
  if (layout_count >= MAX_LAYOUTS) error("too many layouts (max %d)", MAX_LAYOUTS);

  long offset = 0;
  for (LayoutField *f = l->fields; f; f = f->next) {
    err_file = f->file;
    err_line = f->line;
    for (LayoutField *g = l->fields; g != f; g = g->next) {
      if (strcmp(g->name, f->name) == 0) error("duplicate field '%s' in layout '%s'", f->name, l->name);
    }
    field_declare_or_check(f->name, offset);
    offset += 8;
  }
  layout_names[layout_count] = l->name;
  layout_sizes[layout_count] = (long)l->nfields * 8;
  layout_count++;
}

static char *sym_names[MAX_VARS];
static long sym_offsets[MAX_VARS];
static char *sym_types[MAX_VARS];  /* layout name, or NULL for int */
static int sym_count;
static long stack_size;
static int loop_depth; /* nesting depth of loops (while and for) during resolve(); break/continue require loop_depth > 0 */
static int current_fn_is_void; /* whether the function currently being resolved is void */
static int current_fn_two_ret; /* whether the function currently being resolved returns two values */
static int resolving_multi_call; /* set while resolving the call on the right of `a, b = f(...)` */
static int in_defer; /* set while resolving a deferred statement; it may not return, break, continue or defer */
static int defer_sym_base; /* sym_count when the deferred statement began, so its own locals are told apart */

/* Block scoping. Each '{' block marks the sym_count at entry; sym_declare's
 * redeclaration check only looks back to that mark, so sibling blocks can
 * reuse a name without colliding. Leaving a block truncates sym_count back
 * to the mark, which also means sibling blocks' locals get the same stack
 * offsets (safe, since their lifetimes never overlap) -- a size win that falls
 * out of the scheme for free, not something specially engineered. */
#define MAX_SCOPE_DEPTH 64
static int scope_marks[MAX_SCOPE_DEPTH];
static int scope_depth;

/* Returns the stack offset, or -1 if `name` is not a local in the current
 * function (the caller then falls back to the globals table). Scans
 * innermost-declared-first so an inner scope's variable correctly shadows
 * an outer one of the same name. */
static long sym_try_lookup(char *name) {
  for (int i = sym_count - 1; i >= 0; i--) {
    if (strcmp(sym_names[i], name) == 0) return sym_offsets[i];
  }
  return -1;
}

/* The layout a variable was declared with, or NULL. Locals are searched innermost
 * first, then globals. */
static char *var_type(char *name) {
  for (int i = sym_count - 1; i >= 0; i--) {
    if (strcmp(sym_names[i], name) == 0) return sym_types[i];
  }
  for (Global *g = prog_globals; g; g = g->next) {
    if (strcmp(g->name, name) == 0) return g->type;
  }
  return NULL;
}

/* A deferred statement runs when the function returns, and by then every
 * inner block has ended and its locals' slots may hold another block's
 * variables. So a defer may use the parameters, the locals of the
 * function's outermost block and its own locals, and nothing declared in an
 * inner block. */
static void check_defer_local(char *name) {
  if (!in_defer || scope_depth < 2) return;
  for (int i = sym_count - 1; i >= 0; i--) {
    if (strcmp(sym_names[i], name) != 0) continue;
    if (i >= scope_marks[1] && i < defer_sym_base)
      error("a defer runs when the function returns, after the block that declares '%s' has ended", name);
    return;
  }
}

static long sym_declare(char *name) {
  int scope_start = scope_depth > 0 ? scope_marks[scope_depth - 1] : 0;
  for (int i = scope_start; i < sym_count; i++) {
    if (strcmp(sym_names[i], name) == 0) error("redeclaration of '%s'", name);
  }
  if (sym_count >= MAX_VARS) error("too many variables (max %d)", MAX_VARS);
  long offset = (sym_count + 1) * 8;
  sym_names[sym_count] = name;
  sym_offsets[sym_count] = offset;
  sym_types[sym_count] = NULL;
  sym_count++;
  if (offset > stack_size) stack_size = offset;
  return offset;
}

static void resolve_node(Node *n);

/* Points errors at n while it is resolved, and back at its parent after,
 * so a check the parent makes after resolving a child still reports the
 * parent's line. */
static void resolve(Node *n) {
  if (!n) return;
  const char *saved_file = err_file;
  int saved_line = err_line;
  err_file = n->file;
  err_line = n->line;
  resolve_node(n);
  err_file = saved_file;
  err_line = saved_line;
}

static void resolve_node(Node *n) {
  if (!n) return;
  switch (n->kind) {
    case ND_NUM: return;
    case ND_STR: return;
    case ND_VAR: {
      long off = sym_try_lookup(n->name); /* locals shadow globals shadow functions */
      if (off >= 0) { check_defer_local(n->name); n->ival = off; return; }
      if (glob_exists(n->name)) { n->is_global = 1; return; }
      if (extern_exists(n->name)) { n->kind = ND_FUNCREF; return; }
      if (func_exists(n->name)) { n->kind = ND_FUNCREF; return; }
      error("undeclared variable '%s'", n->name);
      return;
    }
    case ND_FUNCREF: return; /* only ever reached already-resolved, via the VAR case above */
    case ND_NEG: case ND_BITNOT: resolve(n->lhs); return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
    case ND_AND: case ND_OR:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
      resolve(n->lhs); resolve(n->rhs); return;
    case ND_ASSIGN: {
      resolve(n->rhs);
      long off = sym_try_lookup(n->name);
      if (off >= 0) { check_defer_local(n->name); n->ival = off; }
      else if (glob_exists(n->name)) n->is_global = 1;
      else error("undeclared variable '%s'", n->name);
      return;
    }
    case ND_MULTIASSIGN: {
      int saved = resolving_multi_call;
      resolving_multi_call = 1;
      resolve(n->lhs); /* the call */
      resolving_multi_call = saved;
      if (n->lhs->kind != ND_CALL || !func_lookup_two_ret(n->lhs->name))
        error("the right of a two-value assignment must call an '(int, int)' function");
      resolve(n->cond); /* target 1 */
      resolve(n->rhs);  /* target 2 */
      if (n->cond->kind != ND_VAR || n->rhs->kind != ND_VAR)
        error("two-value assignment targets must be variables");
      return;
    }
    case ND_DECL:
      resolve(n->rhs); /* must resolve before the new name enters scope */
      check_type(n->type);
      n->ival = sym_declare(n->name);
      sym_types[sym_count - 1] = n->type;
      return;
    case ND_CALL: {
      const Builtin *b = find_builtin(n->name);
      if (b) {
        if (n->nargs != b->nargs) {
          error("builtin '%s' takes exactly %d argument(s), got %d", b->name, b->nargs, n->nargs);
        }
      } else if (func_exists(n->name)) {
        int expected = func_lookup_nparams(n->name);
        if (expected != n->nargs) {
          error("'%s' takes %d argument(s), got %d", n->name, expected, n->nargs);
        }
        if (func_lookup_two_ret(n->name) && !resolving_multi_call) {
          error("'%s' returns two values; call it as 'a, b = %s(...)'", n->name, n->name);
        }
      } else {
        /* Not a builtin, not a declared function -- an indirect call through
         * a function-pointer-valued variable, if `name` names one. */
        long off = sym_try_lookup(n->name);
        if (off >= 0) { check_defer_local(n->name); n->ival = off; n->is_indirect = 1; }
        else if (glob_exists(n->name)) { n->is_global = 1; n->is_indirect = 1; }
        else error("call to undeclared function '%s'", n->name);
      }
      for (Node *a = n->args; a; a = a->next) resolve(a);
      return;
    }
    case ND_IF:
      resolve(n->cond); resolve(n->then); resolve(n->els); return;
    case ND_WHILE:
      resolve(n->cond);
      loop_depth++;
      resolve(n->body);
      loop_depth--;
      return;
    case ND_FORLOOP:
      resolve(n->cond);
      loop_depth++;
      resolve(n->body);
      loop_depth--;
      resolve(n->then); /* step; outside loop_depth, it holds no break/continue */
      return;
    case ND_RETURN:
      if (in_defer) error("'return' inside a defer");
      if (current_fn_is_void && n->lhs) error("'void' function cannot return a value");
      if (current_fn_two_ret && !n->rhs) error("a two-value function must 'return a, b'");
      if (!current_fn_two_ret && n->rhs) error("only a '(int, int)' function may return two values");
      resolve(n->lhs); resolve(n->rhs); return;
    case ND_EXPR_STMT:
      resolve(n->lhs); return;
    case ND_DEFER: {
      /* The deferred statement runs at .Lret, outside every loop, so it is
       * resolved with loop_depth 0 and may not jump out of the epilogue. */
      if (in_defer) error("'defer' inside a defer");
      int saved_loop_depth = loop_depth;
      loop_depth = 0;
      in_defer = 1;
      defer_sym_base = sym_count;
      resolve(n->lhs);
      in_defer = 0;
      loop_depth = saved_loop_depth;
      return;
    }
    case ND_BLOCK: {
      if (scope_depth >= MAX_SCOPE_DEPTH) error("blocks nested too deeply (max %d)", MAX_SCOPE_DEPTH);
      scope_marks[scope_depth++] = sym_count;
      for (Node *s = n->body; s; s = s->next) resolve(s);
      scope_depth--;
      sym_count = scope_marks[scope_depth];
      return;
    }
    case ND_BREAK:
      if (loop_depth <= 0 && in_defer) error("'break' inside a defer");
      if (loop_depth <= 0) error("'break' outside of a loop");
      return;
    case ND_CONTINUE:
      if (loop_depth <= 0 && in_defer) error("'continue' inside a defer");
      if (loop_depth <= 0) error("'continue' outside of a loop");
      return;
    case ND_INDEX:
      resolve(n->lhs); resolve(n->rhs); return;
    case ND_INDEX_ASSIGN:
      resolve(n->lhs); resolve(n->rhs); resolve(n->cond); return;
    case ND_FIELD:
    case ND_FIELD_ASSIGN: {
      /* A base that is a variable declared with a layout looks the field
       * up in that layout; anything else uses the shared table. */
      char *t = n->lhs->kind == ND_VAR ? var_type(n->lhs->name) : NULL;
      resolve(n->lhs);
      if (n->kind == ND_FIELD_ASSIGN) resolve(n->rhs);
      n->ival = t ? layout_field_offset(t, n->name) : field_lookup(n->name);
      return;
    }
    case ND_SIZEOF:
      n->ival = layout_lookup_size(n->name);
      n->kind = ND_NUM; /* same in-place rewrite-to-constant trick as FUNCREF */
      return;
  }
}

static void resolve_function(Function *f) {
  sym_count = 0;
  stack_size = 0;
  scope_depth = 0;
  current_fn_is_void = f->is_void;
  current_fn_two_ret = f->two_ret;
  err_file = f->file;
  err_line = f->line;
  for (int i = 0; i < f->nparams; i++) {
    check_type(param_type(f, i));
    sym_declare(param_name(f, i));
    sym_types[sym_count - 1] = param_type(f, i);
  }
  resolve(f->body);
  f->stack_size = stack_size;
}

/* -------------------------------------------------------------- codegen */

static const char *ARG_REGS[MAX_PARAMS] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static FILE *out;
static int label_count;
/* --elf targets Moonshot's ring-3 ABI instead of Linux. Same backend; the
 * entry stub and the builtins that reach the operating system emit
 * Moonshot's syscall numbers. Mirrors elf_mode in coff.c0 so that
 * `coff0 --elf` and `coff1 --elf` can be diffed like every other output --
 * that diff is the ring-3 backend's oracle. */
static int elf_mode = 0;

static int is_moonshot_builtin(const char *name) {
  return strcmp(name, "win_info") == 0 || strcmp(name, "win_max") == 0
      || strcmp(name, "key_poll") == 0 || strcmp(name, "ticks") == 0
      || strcmp(name, "present") == 0 || strcmp(name, "readfile") == 0
      || strcmp(name, "blit") == 0 || strcmp(name, "fill") == 0
      || strcmp(name, "mouse_poll") == 0 || strcmp(name, "glyph") == 0
      || strcmp(name, "dealloc") == 0 || strcmp(name, "sound") == 0
      || strcmp(name, "writefile") == 0 || strcmp(name, "syscall") == 0;
}
/* Port I/O has no meaning in ring 3. The file builtins do, Moonshot has
 * syscalls for them. */
static int is_linux_only_builtin(const char *name) {
  return strcmp(name, "outb") == 0 || strcmp(name, "inb") == 0
      || strcmp(name, "outw") == 0 || strcmp(name, "inw") == 0;
}
static int current_ret_label;
#define MAX_DEFERS 64
static Node *defers[MAX_DEFERS]; /* deferred statements collected while generating a function body */
static int n_defers;             /* reset per function; emitted in reverse at .Lret */

#define MAX_LOOP_DEPTH 64
static int loop_labels[MAX_LOOP_DEPTH]; /* one label id per active loop; .Lend%d is break's target, .Lstart%d (.Lstep%d in a for) continue's */
static int loop_is_for[MAX_LOOP_DEPTH]; /* 1 if the active loop is a for; continue then targets .Lstep%d (the step) */
static int gen_loop_depth;

/* Register allocation, mirroring coff.c0. Every function is generated twice.
 * The first pass (emit_dry, output to /dev/null) weighs each stack slot by its
 * uses, each loop around a use multiplying its weight by 8, up to three
 * loops deep. The busiest
 * slots then live in callee-saved registers for the real pass, so a call
 * never disturbs them and c0, having no address-of for locals, can never
 * reach them through a pointer. */
static int emit_dry;
static long *slot_weight; /* per slot of the current function, filled by the dry pass */
static int *slot_reg;     /* per slot, 0 for the stack or 1-5 for rbx, r12-r15 */
static FILE *dry_out;

static const char *saved_reg_name(int r) {
  static const char *names[] = { "", "rbx", "r12", "r13", "r14", "r15" };
  return names[r];
}

/* A local's operand, its register or its stack slot. The dry pass only
 * weighs the use. */
static const char *slot(long off) {
  static char buf[4][32];
  static int next;
  if (emit_dry) {
    long w = 1;
    if (gen_loop_depth == 1) w = 8;
    if (gen_loop_depth == 2) w = 64;
    if (gen_loop_depth >= 3) w = 512;
    slot_weight[off / 8] += w;
    return "";
  }
  int r = slot_reg[off / 8];
  if (r != 0) return saved_reg_name(r);
  next = (next + 1) % 4;
  snprintf(buf[next], sizeof buf[next], "[rbp - %ld]", off);
  return buf[next];
}

static void gen_expr_node(Node *n);

/* Same idea as resolve(), codegen errors point at the node. */
static void gen_expr(Node *n) {
  const char *saved_file = err_file;
  int saved_line = err_line;
  if (n) { err_file = n->file; err_line = n->line; }
  gen_expr_node(n);
  err_file = saved_file;
  err_line = saved_line;
}

/* A leaf operand is a number or a variable. Loading one has no side effect
 * and touches no other register, so a binary operator can load it straight
 * into rcx instead of saving rax on the stack around it. */
static int is_leaf(Node *n) {
  return n->kind == ND_NUM || n->kind == ND_VAR;
}

/* The register a local lives in, or 0 when it is on the stack, a global or
 * something else, and always 0 in the dry pass. */
static int reg_local(Node *n) {
  if (emit_dry || n->kind != ND_VAR || n->is_global) return 0;
  return slot_reg[n->ival / 8];
}

/* Leaves lhs in rax and rhs in rcx, the operand layout of every binary
 * operator. */
static void gen_operands(Node *lhs, Node *rhs) {
  gen_expr(lhs);
  if (is_leaf(rhs)) {
    if (rhs->kind == ND_NUM) fprintf(out, "    mov rcx, %ld\n", rhs->ival);
    else if (rhs->is_global) fprintf(out, "    mov rcx, [.G%s]\n", rhs->name);
    else fprintf(out, "    mov rcx, %s\n", slot(rhs->ival));
    return;
  }
  fprintf(out, "    push rax\n");
  gen_expr(rhs);
  fprintf(out, "    mov rcx, rax\n");
  fprintf(out, "    pop rax\n");
}

/* add, sub, imul, and, or, xor and cmp take a sign-extended 32-bit
 * immediate, so a number in that range on their right needs no register at
 * all. */
static int is_imm32(Node *n) {
  return n->kind == ND_NUM && n->ival >= -2147483647L - 1 && n->ival <= 2147483647L;
}

/* Leaves lhs in rax and returns 1 when rhs can be an immediate, otherwise
 * loads both operands as gen_operands does and returns 0. */
static int gen_operands_imm(Node *lhs, Node *rhs) {
  if (is_imm32(rhs)) {
    gen_expr(lhs);
    return 1;
  }
  if (reg_local(rhs)) {
    gen_expr(lhs);
    return 2;
  }
  gen_operands(lhs, rhs);
  return 0;
}

/* The right operand of an instruction, after gen_operands_imm. */
static void emit_rhs(int imm, Node *rhs) {
  if (imm == 1) fprintf(out, "%ld\n", rhs->ival);
  else if (imm == 2) fprintf(out, "%s\n", saved_reg_name(reg_local(rhs)));
  else fprintf(out, "rcx\n");
}

/* A number or a local can trade places with the other operand of a
 * commutative operator. A global cannot, since a call on the other side may
 * write it, and c0 has no way to take a local's address. */
static int is_stable_leaf(Node *n) {
  return n->kind == ND_NUM || (n->kind == ND_VAR && !n->is_global);
}

/* The jump taken when a comparison of rax against the right operand holds
 * (sense 1) or fails (sense 0), or NULL for a node that is not a comparison. */
static const char *cmp_jump(int kind, int sense) {
  switch (kind) {
    case ND_EQ: return sense ? "je" : "jne";
    case ND_NE: return sense ? "jne" : "je";
    case ND_LT: return sense ? "jl" : "jge";
    case ND_LE: return sense ? "jle" : "jg";
    case ND_GT: return sense ? "jg" : "jle";
    case ND_GE: return sense ? "jge" : "jl";
    default: return NULL;
  }
}

/* Jumps to .<label><l> when cond is (sense 1) or is not (sense 0) true. A
 * comparison branches on the flags directly instead of turning them into 0
 * or 1 and comparing that against 0, and && and || become chains of such
 * branches. */
static void gen_jump(Node *cond, int sense, const char *label, int l) {
  const char *jcc = cmp_jump(cond->kind, sense);
  int lr = jcc ? reg_local(cond->lhs) : 0;
  if (lr && (is_imm32(cond->rhs) || reg_local(cond->rhs))) {
    if (is_imm32(cond->rhs)) fprintf(out, "    cmp %s, %ld\n", saved_reg_name(lr), cond->rhs->ival);
    else fprintf(out, "    cmp %s, %s\n", saved_reg_name(lr), saved_reg_name(reg_local(cond->rhs)));
    fprintf(out, "    %s .%s%d\n", jcc, label, l);
    return;
  }
  if (jcc) {
    int imm = gen_operands_imm(cond->lhs, cond->rhs);
    fprintf(out, "    cmp rax, ");
    emit_rhs(imm, cond->rhs);
    fprintf(out, "    %s .%s%d\n", jcc, label, l);
    return;
  }
  /* (a && b) is false when either is false, (a || b) is true when either is
   * true. The other two cases skip to .Lskip when the left side decides. */
  if ((cond->kind == ND_AND && !sense) || (cond->kind == ND_OR && sense)) {
    gen_jump(cond->lhs, sense, label, l);
    gen_jump(cond->rhs, sense, label, l);
    return;
  }
  if (cond->kind == ND_AND || cond->kind == ND_OR) {
    int skip = label_count++;
    gen_jump(cond->lhs, cond->kind == ND_OR, "Lskip", skip);
    gen_jump(cond->rhs, sense, label, l);
    fprintf(out, ".Lskip%d:\n", skip);
    return;
  }
  gen_expr(cond);
  fprintf(out, "    cmp rax, 0\n");
  fprintf(out, "    %s .%s%d\n", sense ? "jne" : "je", label, l);
}

static void gen_jump_if_false(Node *cond, const char *label, int l) {
  gen_jump(cond, 0, label, l);
}

/* Leaves an address in rdi and a value in rax, evaluated in that order. A
 * leaf value loads into rax without disturbing rdi, so the address need not
 * wait on the stack. */
static void gen_addr_value(Node *addr, Node *value) {
  gen_expr(addr);
  if (is_leaf(value)) {
    fprintf(out, "    mov rdi, rax\n");
    gen_expr(value);
    return;
  }
  fprintf(out, "    push rax\n");
  gen_expr(value);
  fprintf(out, "    pop rdi\n");
}

/* A number, a local or a string literal, loaded straight into an argument
 * register. Nothing an argument expression does can change one, so it may
 * be loaded after the others. */
static int is_stable_arg(Node *n) {
  return is_stable_leaf(n) || n->kind == ND_STR;
}

static void gen_stable_arg(Node *n, const char *reg) {
  if (n->kind == ND_NUM) fprintf(out, "    mov %s, %ld\n", reg, n->ival);
  else if (n->kind == ND_STR) fprintf(out, "    lea %s, [.Lstr%d]\n", reg, n->str_id);
  else fprintf(out, "    mov %s, %s\n", reg, slot(n->ival));
}

static void gen_expr_node(Node *n) {
  switch (n->kind) {
    case ND_NUM:
      fprintf(out, "    mov rax, %ld\n", n->ival);
      return;
    case ND_STR:
      fprintf(out, "    lea rax, [.Lstr%d]\n", n->str_id);
      return;
    case ND_VAR:
      if (n->is_global) fprintf(out, "    mov rax, [.G%s]\n", n->name);
      else fprintf(out, "    mov rax, %s\n", slot(n->ival));
      return;
    case ND_FUNCREF:
      fprintf(out, "    lea rax, [%s]\n", n->name);
      return;
    case ND_NEG:
      gen_expr(n->lhs);
      fprintf(out, "    neg rax\n");
      return;
    case ND_BITNOT:
      gen_expr(n->lhs);
      fprintf(out, "    not rax\n");
      return;
    case ND_INDEX:
      /* base + index, so the operands may trade places like ADD's. */
    {
      Node *ia = n->lhs, *ib = n->rhs;
      if (is_stable_leaf(ia) && !is_leaf(ib)) { ia = n->rhs; ib = n->lhs; }
      int iimm = gen_operands_imm(ia, ib);
      fprintf(out, "    add rax, ");
      emit_rhs(iimm, ib);
      fprintf(out, "    movzx rax, byte ptr [rax]\n");
    }
      return;
    case ND_FIELD:
      gen_expr(n->lhs);
      fprintf(out, "    mov rax, [rax + %ld]\n", n->ival);
      return;
    case ND_CALL: {
      if (strcmp(n->name, "print") == 0) {
        int l = label_count++;
        /* rsi and rcx, not rbx, because the ABI has a function preserve
         * rbx. */
        gen_expr(n->args); /* -> rax = pointer to a null-terminated string */
        fprintf(out, "    mov rsi, rax\n");
        fprintf(out, "    xor rcx, rcx\n");
        fprintf(out, ".Lstrlen%d:\n", l);
        fprintf(out, "    cmp byte ptr [rsi + rcx], 0\n");
        fprintf(out, "    je .Lstrlend%d\n", l);
        fprintf(out, "    inc rcx\n");
        fprintf(out, "    jmp .Lstrlen%d\n", l);
        fprintf(out, ".Lstrlend%d:\n", l);
        if (elf_mode) {
          /* Moonshot SYS_DEBUG_PRINT(str, len) is 1. */
          fprintf(out, "    mov rdi, rsi\n");
          fprintf(out, "    mov rsi, rcx\n");
          fprintf(out, "    mov rax, 1\n");
          fprintf(out, "    syscall\n");
        } else {
          fprintf(out, "    mov rdx, rcx\n");
          fprintf(out, "    mov rdi, 1\n");
          fprintf(out, "    mov rax, 1\n");
          fprintf(out, "    syscall\n");
        }
        return;
      }
      if (strcmp(n->name, "alloc") == 0 && elf_mode) {
        /* SYS_ALLOC(n) (6) returns n zeroed bytes from the process's own heap,
         * which Moonshot's lager can take back through dealloc(). */
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n");
        fprintf(out, "    mov rax, 6\n");
        fprintf(out, "    syscall\n");
        return;
      }
      if (strcmp(n->name, "alloc") == 0) {
        /* Bump allocator over brk(2). The current break is cached in .Lcurbrk
         * (0 = not fetched yet). Note that syscall clobbers rcx/r11, so live values
         * stay on the stack across each syscall. */
        int l = label_count++;
        gen_expr(n->args); /* -> rax = n */
        fprintf(out, "    push rax\n");
        fprintf(out, "    mov rax, [.Lcurbrk]\n");
        fprintf(out, "    test rax, rax\n");
        fprintf(out, "    jnz .Lbrkok%d\n", l);
        fprintf(out, "    mov rdi, 0\n");
        fprintf(out, "    mov rax, 12\n");
        fprintf(out, "    syscall\n");
        fprintf(out, ".Lbrkok%d:\n", l);
        fprintf(out, "    pop rdi\n");        /* n */
        fprintf(out, "    push rax\n");       /* old break = result */
        fprintf(out, "    add rdi, rax\n");   /* new break = old + n */
        fprintf(out, "    mov rax, 12\n");
        fprintf(out, "    syscall\n");
        fprintf(out, "    mov [.Lcurbrk], rax\n");
        fprintf(out, "    pop rax\n");        /* return the old break */
        return;
      }
      if (strcmp(n->name, "load8") == 0) {
        gen_expr(n->args);
        fprintf(out, "    movzx rax, byte ptr [rax]\n");
        return;
      }
      if (strcmp(n->name, "store8") == 0) {
        gen_addr_value(n->args, n->args->next);
        fprintf(out, "    mov byte ptr [rdi], al\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "exit") == 0) {
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n");
        if (elf_mode) fprintf(out, "    mov rax, 0\n");   /* Moonshot SYS_EXIT */
        else          fprintf(out, "    mov rax, 60\n");  /* Linux exit */
        fprintf(out, "    syscall\n");
        return;
      }
      /* Moonshot ring-3 builtins (--elf only). Arguments follow the SYSCALL
       * ABI, in rdi, rsi, rdx, r10, r8 -- r10 for the fourth, not rcx, because
       * SYSCALL clobbers rcx with the return RIP. */
      if (!elf_mode && is_moonshot_builtin(n->name)) error("'%s' exists only for the Moonshot target (--elf)", n->name);
      if (elf_mode && is_linux_only_builtin(n->name)) error("'%s' has no meaning in ring 3 on Moonshot", n->name);
      if (strcmp(n->name, "win_info") == 0) { fprintf(out, "    mov rax, 2\n    syscall\n"); return; }
      if (strcmp(n->name, "win_max") == 0)  { fprintf(out, "    mov rax, 10\n    syscall\n"); return; }
      if (strcmp(n->name, "key_poll") == 0) { fprintf(out, "    mov rax, 4\n    syscall\n"); return; }
      if (strcmp(n->name, "ticks") == 0)    { fprintf(out, "    mov rax, 7\n    syscall\n"); return; }
      if (strcmp(n->name, "mouse_poll") == 0) { fprintf(out, "    mov rax, 11\n    syscall\n"); return; }
      if (strcmp(n->name, "dealloc") == 0) {
        /* SYS_DEALLOC(ptr) (13) gives back a block alloc() returned.
         * Returns 0, or -1 if ptr is not the start of a live block. */
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n");
        fprintf(out, "    mov rax, 13\n");
        fprintf(out, "    syscall\n");
        return;
      }
      if (strcmp(n->name, "sound") == 0) {
        /* SYS_SOUND(buf, len) (14) queues len bytes of 8-bit mono PCM
         * for oljud and returns how many fit. buf == 0 returns how many
         * bytes are still waiting to be played. */
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop rsi\n    pop rdi\n");
        fprintf(out, "    mov rax, 14\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "glyph") == 0) {
        /* SYS_GLYPH(ch, buf) (12) copies one character's font bitmap into
         * the caller's memory and returns the cell size packed
         * width * 65536 + height. buf == 0 returns the size only. */
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop rsi\n    pop rdi\n");
        fprintf(out, "    mov rax, 12\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "present") == 0) {
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n");
        fprintf(out, "    mov rax, 9\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "writefile") == 0) {
        /* SYS_WRITEFILE(name, buf, len) (15) makes the jakel file hold
         * exactly those len bytes, creating it if needed. Returns len or -1. */
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop rdx\n    pop rsi\n    pop rdi\n");
        fprintf(out, "    mov rax, 15\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "readfile") == 0) {
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop rdx\n    pop rsi\n    pop rdi\n");
        fprintf(out, "    mov rax, 8\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "blit") == 0 || strcmp(n->name, "fill") == 0) {
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop r8\n    pop r10\n    pop rdx\n    pop rsi\n    pop rdi\n");
        fprintf(out, "    mov rax, %d\n    syscall\n", strcmp(n->name, "blit") == 0 ? 5 : 3);
        return;
      }
      if (strcmp(n->name, "load32") == 0) {
        /* A 32-bit load clears the upper half of rax by itself. */
        gen_expr(n->args);
        fprintf(out, "    mov eax, dword ptr [rax]\n");
        return;
      }
      if (strcmp(n->name, "store32") == 0) {
        gen_addr_value(n->args, n->args->next);
        fprintf(out, "    mov dword ptr [rdi], eax\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "load64") == 0) {
        gen_expr(n->args);
        fprintf(out, "    mov rax, [rax]\n");
        return;
      }
      if (strcmp(n->name, "store64") == 0) {
        gen_addr_value(n->args, n->args->next);
        fprintf(out, "    mov [rdi], rax\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "syscall") == 0) {
        /* syscall(n, a, b, c, d, e) (--elf) makes any Moonshot syscall by number,
         * so a new one needs a kernel change and a library wrapper, not a
         * compiler change. Arguments in rdi, rsi, rdx, r10, r8; the number
         * in rax. Unused arguments are passed as 0. */
        for (Node *a = n->args; a; a = a->next) { gen_expr(a); fprintf(out, "    push rax\n"); }
        fprintf(out, "    pop r8\n    pop r10\n    pop rdx\n    pop rsi\n    pop rdi\n    pop rax\n");
        fprintf(out, "    syscall\n");
        return;
      }
      if (elf_mode && strcmp(n->name, "argc") == 0) {
        /* SYS_ARGC (17) returns how many arguments the program was started with,
         * its own name included, as on Linux. */
        fprintf(out, "    mov rax, 17\n    syscall\n");
        return;
      }
      if (elf_mode && strcmp(n->name, "argv") == 0) {
        /* SYS_ARGV(i) (18) returns argument i, a NUL-terminated string in the
         * program's own memory, or 0 past the end. */
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n    mov rax, 18\n    syscall\n");
        return;
      }
      if (strcmp(n->name, "argc") == 0) {
        /* .Largv0 holds the process's initial rsp, saved by _start before
         * anything is pushed, so [rsp] = argc at that point. */
        fprintf(out, "    mov rax, [.Largv0]\n");
        fprintf(out, "    mov rax, [rax]\n");
        return;
      }
      if (strcmp(n->name, "argv") == 0) {
        gen_expr(n->args); /* -> rax = index i */
        fprintf(out, "    mov rcx, rax\n");
        fprintf(out, "    mov rax, [.Largv0]\n");
        fprintf(out, "    mov rax, [rax + rcx*8 + 8]\n");
        return;
      }
      if (strcmp(n->name, "outb") == 0) {
        gen_expr(n->args);        /* port */
        fprintf(out, "    push rax\n");
        gen_expr(n->args->next);  /* value */
        fprintf(out, "    pop rdx\n");   /* dx = port; al = value's low byte */
        fprintf(out, "    out dx, al\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "inb") == 0) {
        gen_expr(n->args); /* -> rax = port */
        fprintf(out, "    mov rdx, rax\n");
        fprintf(out, "    in al, dx\n");
        fprintf(out, "    movzx rax, al\n");
        return;
      }
      if (strcmp(n->name, "outw") == 0) {
        gen_expr(n->args);        /* port */
        fprintf(out, "    push rax\n");
        gen_expr(n->args->next);  /* value */
        fprintf(out, "    pop rdx\n");   /* dx = port; ax = value's low 16 bits */
        fprintf(out, "    out dx, ax\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "inw") == 0) {
        gen_expr(n->args); /* -> rax = port */
        fprintf(out, "    mov rdx, rax\n");
        fprintf(out, "    in ax, dx\n");
        fprintf(out, "    movzx rax, ax\n");
        return;
      }

      /* Thin syscall wrappers and real function calls share the same argument
       * plumbing, evaluating left-to-right onto the stack. Builtins always have
       * arity <= MAX_PARAMS, so they only ever hit the pop-based path below.
       * User calls with more than MAX_PARAMS args read the extra ones back
       * non-destructively (they must stay on the stack -- the callee reads
       * them the same way a caller-past-us would via [rbp+16+...]) and the
       * whole pushed block is reclaimed with one `add rsp` after the call. */
      int i = 0;
      if (n->nargs <= MAX_PARAMS) {
        /* The other arguments are evaluated left to right as usual. The last
         * of them goes straight to its register, the rest wait on the stack,
         * and the stable ones are loaded last, directly into place. */
        int last = -1;
        for (Node *a = n->args; a; a = a->next, i++)
          if (!is_stable_arg(a)) last = i;
        i = 0;
        for (Node *a = n->args; a; a = a->next, i++) {
          if (is_stable_arg(a)) continue;
          gen_expr(a);
          if (i == last) fprintf(out, "    mov %s, rax\n", ARG_REGS[i]);
          else fprintf(out, "    push rax\n");
        }
        Node *by_index[MAX_PARAMS];
        i = 0;
        for (Node *a = n->args; a; a = a->next, i++) by_index[i] = a;
        for (i = n->nargs - 1; i >= 0; i--)
          if (!is_stable_arg(by_index[i]) && i != last) fprintf(out, "    pop %s\n", ARG_REGS[i]);
        for (i = 0; i < n->nargs; i++)
          if (is_stable_arg(by_index[i])) gen_stable_arg(by_index[i], ARG_REGS[i]);
      } else {
        for (Node *a = n->args; a; a = a->next, i++) {
          gen_expr(a);
          fprintf(out, "    push rax\n");
        }
        for (i = 0; i < MAX_PARAMS; i++) {
          fprintf(out, "    mov %s, [rsp + %d]\n", ARG_REGS[i], (n->nargs - 1 - i) * 8);
        }
      }
      /* Under --elf the file builtins are Moonshot's SYS_OPEN (23, mode 0
       * to read and 1 to write), SYS_READ (24), SYS_WRITE (25) and
       * SYS_CLOSE (26), with the arguments where Linux has them. */
      if (elf_mode && strcmp(n->name, "write") == 0) {
        fprintf(out, "    mov rax, 25\n    syscall\n");
      } else if (elf_mode && strcmp(n->name, "read") == 0) {
        fprintf(out, "    mov rax, 24\n    syscall\n");
      } else if (elf_mode && strcmp(n->name, "close") == 0) {
        fprintf(out, "    mov rax, 26\n    syscall\n");
      } else if (elf_mode && strcmp(n->name, "open_read") == 0) {
        fprintf(out, "    mov rsi, 0\n");
        fprintf(out, "    mov rax, 23\n    syscall\n");
      } else if (elf_mode && strcmp(n->name, "open_write") == 0) {
        fprintf(out, "    mov rsi, 1\n");
        fprintf(out, "    mov rax, 23\n    syscall\n");
      } else if (strcmp(n->name, "write") == 0) {
        fprintf(out, "    mov rax, 1\n    syscall\n");
      } else if (strcmp(n->name, "read") == 0) {
        fprintf(out, "    mov rax, 0\n    syscall\n");
      } else if (strcmp(n->name, "close") == 0) {
        fprintf(out, "    mov rax, 3\n    syscall\n");
      } else if (strcmp(n->name, "open_read") == 0) {
        fprintf(out, "    mov rsi, 0\n");       /* O_RDONLY */
        fprintf(out, "    mov rax, 2\n    syscall\n");
      } else if (strcmp(n->name, "open_write") == 0) {
        fprintf(out, "    mov rsi, 577\n");     /* O_WRONLY|O_CREAT|O_TRUNC */
        fprintf(out, "    mov rdx, 420\n");     /* mode 0644 */
        fprintf(out, "    mov rax, 2\n    syscall\n");
      } else if (n->is_indirect) {
        /* r10 is scratch here (not one of the arg registers above, and
         * caller-saved) -- load the pointer variable's value into it after
         * the real args are already in place, then call through it. */
        if (n->is_global) fprintf(out, "    mov r10, [.G%s]\n", n->name);
        else fprintf(out, "    mov r10, %s\n", slot(n->ival));
        fprintf(out, "    call r10\n");
      } else {
        fprintf(out, "    call %s\n", n->name);
      }
      if (n->nargs > MAX_PARAMS) fprintf(out, "    add rsp, %d\n", n->nargs * 8);
      return;
    }
    case ND_AND: {
      int l = label_count++;
      gen_jump(n, 0, "Lfalse", l);
      fprintf(out, "    mov rax, 1\n");
      fprintf(out, "    jmp .Landend%d\n", l);
      fprintf(out, ".Lfalse%d:\n", l);
      fprintf(out, "    mov rax, 0\n");
      fprintf(out, ".Landend%d:\n", l);
      return;
    }
    case ND_OR: {
      int l = label_count++;
      gen_jump(n, 1, "Ltrue", l);
      fprintf(out, "    mov rax, 0\n");
      fprintf(out, "    jmp .Lorend%d\n", l);
      fprintf(out, ".Ltrue%d:\n", l);
      fprintf(out, "    mov rax, 1\n");
      fprintf(out, ".Lorend%d:\n", l);
      return;
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: {
      int imm = 0;
      Node *lhs = n->lhs;
      Node *rhs = n->rhs;
      int commutes = n->kind == ND_ADD || n->kind == ND_MUL || n->kind == ND_BITAND
        || n->kind == ND_BITOR || n->kind == ND_BITXOR;
      /* With the leaf on the right, it loads straight into place instead of
       * rax being saved around the other operand. */
      if (commutes && is_stable_leaf(lhs) && !is_leaf(rhs)) { lhs = n->rhs; rhs = n->lhs; }
      if (commutes || n->kind == ND_SUB || (n->kind >= ND_EQ && n->kind <= ND_GE))
        imm = gen_operands_imm(lhs, rhs);
      else
        gen_operands(lhs, rhs);
      if (n->kind >= ND_EQ && n->kind <= ND_GE) {
        fprintf(out, "    cmp rax, ");
        emit_rhs(imm, rhs);
      }
      switch (n->kind) {
        case ND_ADD: fprintf(out, "    add rax, "); emit_rhs(imm, rhs); break;
        case ND_SUB: fprintf(out, "    sub rax, "); emit_rhs(imm, rhs); break;
        case ND_MUL:
          if (imm == 1) fprintf(out, "    imul rax, rax, %ld\n", rhs->ival);
          else { fprintf(out, "    imul rax, "); emit_rhs(imm, rhs); }
          break;
        case ND_DIV: fprintf(out, "    cqo\n    idiv rcx\n"); break;
        /* idiv leaves the remainder in rdx, with the sign of the dividend,
         * the same as C, so -7 % 2 is -1. */
        case ND_MOD: fprintf(out, "    cqo\n    idiv rcx\n    mov rax, rdx\n"); break;
        case ND_EQ:  fprintf(out, "    sete al\n    movzx rax, al\n"); break;
        case ND_NE:  fprintf(out, "    setne al\n    movzx rax, al\n"); break;
        case ND_LT:  fprintf(out, "    setl al\n    movzx rax, al\n"); break;
        case ND_LE:  fprintf(out, "    setle al\n    movzx rax, al\n"); break;
        case ND_GT:  fprintf(out, "    setg al\n    movzx rax, al\n"); break;
        case ND_GE:  fprintf(out, "    setge al\n    movzx rax, al\n"); break;
        case ND_BITAND: fprintf(out, "    and rax, "); emit_rhs(imm, rhs); break;
        case ND_BITOR:  fprintf(out, "    or rax, "); emit_rhs(imm, rhs); break;
        case ND_BITXOR: fprintf(out, "    xor rax, "); emit_rhs(imm, rhs); break;
        /* the count is in cl, and x86 uses only its low six bits, so a
         * count outside 0..63 shifts by count & 63. >> is arithmetic (sar),
         * the sign bit fills in from the left, which is what gcc does for a
         * signed int in C. */
        case ND_SHL:    fprintf(out, "    shl rax, cl\n"); break;
        case ND_SHR:    fprintf(out, "    sar rax, cl\n"); break;
        default: break;
      }
      return;
    }
    default:
      error("internal error: not an expression node");
  }
}

static void gen_stmt(Node *n);

static void gen_block(Node *n) {
  for (Node *s = n->body; s; s = s->next) gen_stmt(s);
}

static void gen_stmt_node(Node *n);

static void gen_stmt(Node *n) {
  const char *saved_file = err_file;
  int saved_line = err_line;
  if (n) { err_file = n->file; err_line = n->line; }
  gen_stmt_node(n);
  err_file = saved_file;
  err_line = saved_line;
}

/* The instruction for x = x op y done in place, or NULL. */
static const char *inplace_op(int kind) {
  switch (kind) {
    case ND_ADD: return "add";
    case ND_SUB: return "sub";
    case ND_BITAND: return "and";
    case ND_BITOR: return "or";
    case ND_BITXOR: return "xor";
    default: return NULL;
  }
}

/* An assignment to a local in a register that needs no trip through rax. A
 * number (or no initialiser) loads straight in, and x = x op y, which is
 * also what x op= y is, works in place when y is a number or another
 * register. Returns 1 when it emitted the assignment. */
static int gen_reg_assign(long off, Node *val) {
  if (emit_dry) return 0;
  int r = slot_reg[off / 8];
  if (!r) return 0;
  if (!val || val->kind == ND_NUM) {
    fprintf(out, "    mov %s, %ld\n", saved_reg_name(r), val ? val->ival : 0L);
    return 1;
  }
  const char *op = inplace_op(val->kind);
  if (!op) return 0;
  Node *a = val->lhs, *b = val->rhs;
  if (a->kind != ND_VAR || a->is_global || a->ival != off) return 0;
  if (!is_imm32(b) && !reg_local(b)) return 0;
  if (is_imm32(b)) fprintf(out, "    %s %s, %ld\n", op, saved_reg_name(r), b->ival);
  else fprintf(out, "    %s %s, %s\n", op, saved_reg_name(r), saved_reg_name(reg_local(b)));
  return 1;
}

static void gen_stmt_node(Node *n) {
  switch (n->kind) {
    case ND_DECL:
      if (gen_reg_assign(n->ival, n->rhs)) return;
      if (n->rhs) gen_expr(n->rhs);
      else fprintf(out, "    mov rax, 0\n");
      fprintf(out, "    mov %s, rax\n", slot(n->ival));
      return;
    case ND_ASSIGN:
      if (!n->is_global && gen_reg_assign(n->ival, n->rhs)) return;
      gen_expr(n->rhs);
      if (n->is_global) fprintf(out, "    mov [.G%s], rax\n", n->name);
      else fprintf(out, "    mov %s, rax\n", slot(n->ival));
      return;
    case ND_INDEX_ASSIGN:
      /* base + index, then the value, as for store8. */
    {
      Node *ia = n->lhs, *ib = n->rhs;
      if (is_stable_leaf(ia) && !is_leaf(ib)) { ia = n->rhs; ib = n->lhs; }
      int iimm = gen_operands_imm(ia, ib);
      fprintf(out, "    add rax, ");
      emit_rhs(iimm, ib);
    }
      if (is_leaf(n->cond)) {
        fprintf(out, "    mov rdi, rax\n");
        gen_expr(n->cond);
      } else {
        fprintf(out, "    push rax\n");
        gen_expr(n->cond);
        fprintf(out, "    pop rdi\n");
      }
      fprintf(out, "    mov byte ptr [rdi], al\n");
      return;
    case ND_FIELD_ASSIGN:
      gen_addr_value(n->lhs, n->rhs);
      fprintf(out, "    mov [rdi + %ld], rax\n", n->ival);
      return;
    case ND_EXPR_STMT:
      gen_expr(n->lhs);
      return;
    case ND_IF: {
      int l = label_count++;
      gen_jump_if_false(n->cond, "Lelse", l);
      gen_stmt(n->then);
      /* Without an else, .Lend follows .Lelse directly, so the jump would
       * land on the next instruction. */
      if (n->els) fprintf(out, "    jmp .Lend%d\n", l);
      fprintf(out, ".Lelse%d:\n", l);
      if (n->els) gen_stmt(n->els);
      fprintf(out, ".Lend%d:\n", l);
      return;
    }
    case ND_WHILE: {
      int l = label_count++;
      fprintf(out, ".Lstart%d:\n", l);
      gen_jump_if_false(n->cond, "Lend", l);
      if (gen_loop_depth >= MAX_LOOP_DEPTH) error("loops nested too deeply (max %d)", MAX_LOOP_DEPTH);
      loop_is_for[gen_loop_depth] = 0;
      loop_labels[gen_loop_depth++] = l;
      gen_stmt(n->body);
      gen_loop_depth--;
      fprintf(out, "    jmp .Lstart%d\n", l);
      fprintf(out, ".Lend%d:\n", l);
      return;
    }
    case ND_FORLOOP: {
      /* child cond/body/then(step). continue jumps to .Lstep so the step
       * always runs, matching C. */
      int l = label_count++;
      fprintf(out, ".Lstart%d:\n", l);
      gen_jump_if_false(n->cond, "Lend", l);
      if (gen_loop_depth >= MAX_LOOP_DEPTH) error("loops nested too deeply (max %d)", MAX_LOOP_DEPTH);
      loop_is_for[gen_loop_depth] = 1;
      loop_labels[gen_loop_depth++] = l;
      gen_stmt(n->body);
      gen_loop_depth--;
      fprintf(out, ".Lstep%d:\n", l);
      gen_stmt(n->then);
      fprintf(out, "    jmp .Lstart%d\n", l);
      fprintf(out, ".Lend%d:\n", l);
      return;
    }
    case ND_RETURN:
      if (n->rhs) {
        /* return a, b; -- first value in rax, second in rdx. */
        gen_expr(n->lhs);
        fprintf(out, "    push rax\n");
        gen_expr(n->rhs);
        fprintf(out, "    mov rdx, rax\n");
        fprintf(out, "    pop rax\n");
      } else if (n->lhs) {
        gen_expr(n->lhs);
      }
      fprintf(out, "    jmp .Lret%d\n", current_ret_label);
      return;
    case ND_MULTIASSIGN: {
      /* a, b = f(...), where the call leaves value1 in rax and value2 in rdx. */
      gen_expr(n->lhs);
      Node *t1 = n->cond, *t2 = n->rhs;
      if (t1->is_global) fprintf(out, "    mov [.G%s], rax\n", t1->name);
      else fprintf(out, "    mov %s, rax\n", slot(t1->ival));
      if (t2->is_global) fprintf(out, "    mov [.G%s], rdx\n", t2->name);
      else fprintf(out, "    mov %s, rdx\n", slot(t2->ival));
      return;
    }
    case ND_DEFER:
      /* Collected now, emitted (reversed) at .Lret -- see gen_function. */
      if (n_defers >= MAX_DEFERS) error("too many defers in one function (max %d)", MAX_DEFERS);
      defers[n_defers++] = n->lhs;
      return;
    case ND_BREAK:
      fprintf(out, "    jmp .Lend%d\n", loop_labels[gen_loop_depth - 1]);
      return;
    case ND_CONTINUE:
      if (loop_is_for[gen_loop_depth - 1])
        fprintf(out, "    jmp .Lstep%d\n", loop_labels[gen_loop_depth - 1]);
      else
        fprintf(out, "    jmp .Lstart%d\n", loop_labels[gen_loop_depth - 1]);
      return;
    case ND_BLOCK:
      gen_block(n);
      return;
    default:
      error("internal error: not a statement node");
  }
}

/* The parameters' arrival and the body, the part of a function generated
 * twice. */
static void gen_body(Function *f) {
  for (int i = 0; i < f->nparams; i++) {
    if (i < MAX_PARAMS) {
      fprintf(out, "    mov %s, %s\n", slot((long)(i + 1) * 8), ARG_REGS[i]);
    } else {
      /* Stack-passed, the caller left it at [rbp+16+8*(nparams-1-i)], mirroring
       * how its own stack looked at the moment of `call` (see gen_call). */
      long stack_off = 16 + (long)(f->nparams - 1 - i) * 8;
      fprintf(out, "    mov rax, [rbp + %ld]\n", stack_off);
      fprintf(out, "    mov %s, rax\n", slot((long)(i + 1) * 8));
    }
  }
  n_defers = 0;
  gen_block(f->body);
}

/* Gives the up to five heaviest slots a register each, heaviest first, ties
 * to the lower slot. A slot weighing less than 3 stays on the stack, since
 * saving and restoring the register would cost what it saves. Returns how
 * many registers are in use. */
static int pick_slot_regs(long nslots) {
  int used = 0;
  while (used < 5) {
    long best = -1, bestw = 2;
    for (long i = 1; i <= nslots; i++)
      if (slot_reg[i] == 0 && slot_weight[i] > bestw) { best = i; bestw = slot_weight[i]; }
    if (best < 0) return used;
    slot_reg[best] = ++used;
  }
  return used;
}

static void gen_function(Function *f) {
  long nslots = f->stack_size / 8;
  slot_weight = calloc(nslots + 1, sizeof *slot_weight);
  slot_reg = calloc(nslots + 1, sizeof *slot_reg);
  if (!slot_weight || !slot_reg) error("out of memory");
  if (!dry_out) dry_out = fopen("/dev/null", "w");
  if (!dry_out) error("cannot open /dev/null");

  FILE *real_out = out;
  int first_label = label_count;
  out = dry_out;
  emit_dry = 1;
  current_ret_label = label_count++;
  gen_body(f);
  emit_dry = 0;
  out = real_out;
  label_count = first_label;
  int nregs = pick_slot_regs(nslots);

  /* The saved registers sit below the locals. */
  long frame = f->stack_size + nregs * 8;
  if (frame % 16 != 0) frame += 16 - (frame % 16);

  current_ret_label = label_count++;
  f->ret_label = current_ret_label;

  fprintf(out, "%s:\n", f->name);
  fprintf(out, "    push rbp\n");
  fprintf(out, "    mov rbp, rsp\n");
  if (frame > 0) fprintf(out, "    sub rsp, %ld\n", frame);
  for (int i = 1; i <= nregs; i++)
    fprintf(out, "    mov [rbp - %ld], %s\n", f->stack_size + i * 8, saved_reg_name(i));

  gen_body(f);

  fprintf(out, ".Lret%d:\n", current_ret_label);
  /* Deferred statements run here, before the frame is torn down, in reverse
   * (LIFO) order. The frame is still valid so they can touch locals; rax/rdx
   * are saved so a cleanup call cannot clobber the return value(s). Nothing is
   * emitted when a function has no defers. */
  if (n_defers > 0) {
    fprintf(out, "    push rax\n");
    fprintf(out, "    push rdx\n");
    for (int i = n_defers - 1; i >= 0; i--) gen_stmt(defers[i]);
    fprintf(out, "    pop rdx\n");
    fprintf(out, "    pop rax\n");
  }
  for (int i = 1; i <= nregs; i++)
    fprintf(out, "    mov %s, [rbp - %ld]\n", saved_reg_name(i), f->stack_size + i * 8);
  fprintf(out, "    mov rsp, rbp\n");
  fprintf(out, "    pop rbp\n");
  fprintf(out, "    ret\n");
  free(slot_weight);
  free(slot_reg);
}

/* ------------------------------------------------------------ token dump */
/* `coff0 -t file.c0` prints the token stream, one token per line. This is
 * the reference output the c0-written lexer (c0/lex.c0) is diffed against,
 * so the format is a contract and the two must be kept in sync. */

static void dump_tokens(void) {
  for (int i = 0; i < ntokens; i++) {
    Token *t = &tokens[i];
    switch (t->kind) {
      case TK_NUM:    printf("NUM %ld\n", t->ival); break;
      case TK_IDENT:  printf("IDENT %s\n", t->name); break;
      case TK_STRING:
        printf("STR");
        for (long j = 0; j < t->ival; j++) printf(" %d", (unsigned char)t->name[j]);
        printf("\n");
        break;
      case TK_PLUS:   printf("PLUS\n"); break;
      case TK_MINUS:  printf("MINUS\n"); break;
      case TK_STAR:   printf("STAR\n"); break;
      case TK_SLASH:  printf("SLASH\n"); break;
      case TK_PERCENT: printf("PERCENT\n"); break;
      case TK_ASSIGN: printf("ASSIGN\n"); break;
      case TK_EQ:     printf("EQ\n"); break;
      case TK_NE:     printf("NE\n"); break;
      case TK_NOT:    printf("NOT\n"); break;
      case TK_PLUSEQ:    printf("PLUSEQ\n"); break;
      case TK_MINUSEQ:   printf("MINUSEQ\n"); break;
      case TK_STAREQ:    printf("STAREQ\n"); break;
      case TK_SLASHEQ:   printf("SLASHEQ\n"); break;
      case TK_PERCENTEQ: printf("PERCENTEQ\n"); break;
      case TK_LT:     printf("LT\n"); break;
      case TK_LE:     printf("LE\n"); break;
      case TK_GT:     printf("GT\n"); break;
      case TK_GE:     printf("GE\n"); break;
      case TK_LPAREN: printf("LPAREN\n"); break;
      case TK_RPAREN: printf("RPAREN\n"); break;
      case TK_LBRACE: printf("LBRACE\n"); break;
      case TK_RBRACE: printf("RBRACE\n"); break;
      case TK_SEMI:   printf("SEMI\n"); break;
      case TK_COMMA:  printf("COMMA\n"); break;
      case TK_KW_INT:    printf("INT\n"); break;
      case TK_KW_IF:     printf("IF\n"); break;
      case TK_KW_ELSE:   printf("ELSE\n"); break;
      case TK_KW_WHILE:  printf("WHILE\n"); break;
      case TK_KW_FOR:    printf("FOR\n"); break;
      case TK_KW_DEFER:  printf("DEFER\n"); break;
      case TK_KW_RETURN: printf("RETURN\n"); break;
      case TK_EOF:    printf("EOF\n"); break;
      case TK_AND:    printf("AND\n"); break;
      case TK_OR:     printf("OR\n"); break;
      case TK_KW_BREAK:    printf("BREAK\n"); break;
      case TK_KW_CONTINUE: printf("CONTINUE\n"); break;
      case TK_LBRACKET: printf("LBRACKET\n"); break;
      case TK_RBRACKET: printf("RBRACKET\n"); break;
      case TK_KW_VOID: printf("VOID\n"); break;
      case TK_KW_EXTERN: printf("EXTERN\n"); break;
      case TK_DOT: printf("DOT\n"); break;
      case TK_KW_LAYOUT: printf("LAYOUT\n"); break;
      case TK_KW_SIZEOF: printf("SIZEOF\n"); break;
      case TK_AMP:   printf("AMP\n"); break;
      case TK_PIPE:  printf("PIPE\n"); break;
      case TK_CARET: printf("CARET\n"); break;
      case TK_TILDE: printf("TILDE\n"); break;
      case TK_SHL:   printf("SHL\n"); break;
      case TK_SHR:   printf("SHR\n"); break;
    }
  }
}

/* -------------------------------------------------------------- ast dump */
/* `coff0 -a file.c0` prints the parse tree, one node per line, two-space
 * indent per depth, pre-resolve (names, not offsets). Like -t, this format
 * is the contract the c0-written parser (c0/parse.c0) is diffed against. */

static void dump_node(Node *n, int depth) {
  if (!n) return;
  for (int i = 0; i < depth; i++) printf("  ");
  switch (n->kind) {
    case ND_NUM: printf("NUM %ld\n", n->ival); return;
    case ND_STR:
      printf("STR");
      for (int j = 0; j < string_len[n->str_id]; j++)
        printf(" %d", (unsigned char)string_data[n->str_id][j]);
      printf("\n");
      return;
    case ND_VAR: printf("VAR %s\n", n->name); return;
    case ND_FUNCREF: printf("FUNCREF %s\n", n->name); return; /* never actually
      hit, since -a dumps pre-resolve, and FUNCREF only exists post-resolve (see
      resolve()'s ND_VAR case) -- here purely for switch exhaustiveness. */
    case ND_ASSIGN:
      printf("ASSIGN %s\n", n->name);
      dump_node(n->rhs, depth + 1);
      return;
    case ND_DECL:
      printf("DECL %s\n", n->name);
      dump_node(n->rhs, depth + 1);
      return;
    case ND_CALL:
      printf("CALL %s\n", n->name);
      for (Node *a = n->args; a; a = a->next) dump_node(a, depth + 1);
      return;
    case ND_ADD: printf("ADD\n"); goto binary;
    case ND_SUB: printf("SUB\n"); goto binary;
    case ND_MUL: printf("MUL\n"); goto binary;
    case ND_DIV: printf("DIV\n"); goto binary;
    case ND_MOD: printf("MOD\n"); goto binary;
    case ND_EQ:  printf("EQ\n");  goto binary;
    case ND_NE:  printf("NE\n");  goto binary;
    case ND_LT:  printf("LT\n");  goto binary;
    case ND_LE:  printf("LE\n");  goto binary;
    case ND_GT:  printf("GT\n");  goto binary;
    case ND_GE:  printf("GE\n");  goto binary;
    case ND_AND: printf("AND\n"); goto binary;
    case ND_OR:  printf("OR\n");  goto binary;
    case ND_BITAND: printf("BITAND\n"); goto binary;
    case ND_BITOR:  printf("BITOR\n");  goto binary;
    case ND_BITXOR: printf("BITXOR\n"); goto binary;
    case ND_SHL:    printf("SHL\n");    goto binary;
    case ND_SHR:    printf("SHR\n");    goto binary;
    case ND_BITNOT:
      printf("BITNOT\n");
      dump_node(n->lhs, depth + 1);
      return;
    case ND_NEG:
      printf("NEG\n");
      dump_node(n->lhs, depth + 1);
      return;
    case ND_IF:
      printf("IF\n");
      dump_node(n->cond, depth + 1);
      dump_node(n->then, depth + 1);
      if (n->els) {
        for (int i = 0; i < depth + 1; i++) printf("  ");
        printf("ELSE\n");
        dump_node(n->els, depth + 1);
      }
      return;
    case ND_WHILE:
      printf("WHILE\n");
      dump_node(n->cond, depth + 1);
      dump_node(n->body, depth + 1);
      return;
    case ND_FORLOOP:
      printf("FORLOOP\n");
      dump_node(n->cond, depth + 1);
      dump_node(n->body, depth + 1);
      dump_node(n->then, depth + 1);
      return;
    case ND_RETURN:
      printf("RETURN\n");
      dump_node(n->lhs, depth + 1);
      if (n->rhs) dump_node(n->rhs, depth + 1);
      return;
    case ND_MULTIASSIGN:
      printf("MULTIASSIGN\n");
      dump_node(n->cond, depth + 1);
      dump_node(n->rhs, depth + 1);
      dump_node(n->lhs, depth + 1);
      return;
    case ND_DEFER:
      printf("DEFER\n");
      dump_node(n->lhs, depth + 1);
      return;
    case ND_EXPR_STMT:
      printf("EXPRSTMT\n");
      dump_node(n->lhs, depth + 1);
      return;
    case ND_BLOCK:
      printf("BLOCK\n");
      for (Node *s = n->body; s; s = s->next) dump_node(s, depth + 1);
      return;
    case ND_BREAK: printf("BREAK\n"); return;
    case ND_CONTINUE: printf("CONTINUE\n"); return;
    case ND_INDEX:
      printf("INDEX\n");
      dump_node(n->lhs, depth + 1);
      dump_node(n->rhs, depth + 1);
      return;
    case ND_INDEX_ASSIGN:
      printf("INDEXASSIGN\n");
      dump_node(n->lhs, depth + 1);
      dump_node(n->rhs, depth + 1);
      dump_node(n->cond, depth + 1);
      return;
    case ND_FIELD:
      printf("FIELD %s\n", n->name);
      dump_node(n->lhs, depth + 1);
      return;
    case ND_FIELD_ASSIGN:
      printf("FIELDASSIGN %s\n", n->name);
      dump_node(n->lhs, depth + 1);
      dump_node(n->rhs, depth + 1);
      return;
    case ND_SIZEOF:
      printf("SIZEOF %s\n", n->name);
      return;
  }
  return;
binary:
  dump_node(n->lhs, depth + 1);
  dump_node(n->rhs, depth + 1);
}

static void dump_ast(void) {
  for (Embed *e = prog_embeds; e; e = e->next)
    printf("EMBED %s %s\n", e->name, e->path);
  for (Global *g = prog_globals; g; g = g->next)
    printf("GLOBAL %s %ld\n", g->name, g->init);
  for (Layout *l = prog_layouts; l; l = l->next) {
    printf("LAYOUT %s\n", l->name);
    for (LayoutField *f = l->fields; f; f = f->next)
      printf("  FIELDDECL %s\n", f->name);
  }
  for (Function *f = prog_functions; f; f = f->next) {
    if (f->is_void) printf("FUNC void %s", f->name);
    else if (f->two_ret) printf("FUNC (int, int) %s", f->name);
    else printf("FUNC %s", f->name);
    for (int i = 0; i < f->nparams; i++) printf(" %s", param_name(f, i));
    printf("\n");
    dump_node(f->body, 1);
  }
}

/* ------------------------------------------------------------------ io */

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) { fprintf(stderr, "coff0: %s: cannot tell its size\n", path); exit(1); }
  char *buf = malloc(size + 1);
  if (!buf || fread(buf, 1, size, f) != (size_t)size) {
    fprintf(stderr, "coff0: %s: could not read the whole file\n", path);
    exit(1);
  }
  buf[size] = 0;
  fclose(f);
  return buf;
}

/* -------------------------------------------------------------- includes
 *
 * `include "path.c0";` is pure textual substitution, resolved entirely
 * before the real lexer ever runs -- like C's #include, not a language
 * feature the parser/resolver/codegen need to know exists at all. This
 * function reads a file, scans its raw text for include directives (while
 * skipping over string literals and // comments so neither can trigger a
 * false match), and recursively splices in each referenced file's own
 * fully-expanded content in place of the directive -- producing one flat
 * buffer that gets handed to tokenize() exactly as a single file would be.
 * Deliberately NOT separate compilation. There is no linking between c0
 * files and still one flat symbol namespace. Includes solve a
 * file-organization problem (one program too large to navigate as a single
 * file), not a compilation-model one, and nothing more.
 *
 * Include paths are resolved relative to the file containing the
 * directive (not the current working directory), so nested includes work
 * regardless of where coff is invoked from -- the same convention C's
 * #include "..." form uses.
 *
 * An include that is not found next to the including file is looked for
 * in each -I directory, in order (inc_dirs). Errors in a directive are
 * reported at the directive's own file and line.
 */

typedef struct Growbuf Growbuf;
struct Growbuf {
  char *data;
  long len, cap;
};

static void gb_init(Growbuf *b) {
  b->cap = 4096;
  b->len = 0;
  b->data = malloc(b->cap);
}

static void gb_putc(Growbuf *b, char c) {
  if (b->len + 1 >= b->cap) {
    b->cap *= 2;
    b->data = realloc(b->data, b->cap);
  }
  b->data[b->len++] = c;
}

static void gb_puts_n(Growbuf *b, const char *s, long n) {
  for (long i = 0; i < n; i++) gb_putc(b, s[i]);
}

/* Directory portion of `path`, trailing slash included (so callers can
 * concatenate directly), or "" if `path` has no slash (same-directory
 * relative resolution, e.g. a top-level `coff1 main.c0 out.s` invocation). */
static char *dirname_with_slash(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return strdup("");
  long len = slash - path + 1;
  char *d = malloc(len + 1);
  memcpy(d, path, len);
  d[len] = 0;
  return d;
}

static char *join_path(const char *dir, const char *rel) {
  long dl = strlen(dir), rl = strlen(rel);
  char *out = malloc(dl + rl + 1);
  memcpy(out, dir, dl);
  memcpy(out + dl, rel, rl);
  out[dl + rl] = 0;
  return out;
}

/* -I directories, each kept with a trailing slash so join_path can use it
 * like an including file's own directory. An include is looked for next to
 * the file that includes it first, and only when it is not there, in these,
 * in the order given. That is what lets programs in different directories
 * (and in different places in the dev and release trees) share one library
 * by name. */
#define MAX_INC_DIRS 8
static const char *inc_dirs[MAX_INC_DIRS];
static int n_inc_dirs = 0;

static int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

/* Currently-open include chain, for cycle detection. A file that ends up
 * including itself would otherwise recurse until the stack overflows with no
 * clear error. A diamond, where two files include the same third one, is
 * fine and is handled by `included` below. */
#define INCLUDE_MAX_DEPTH 32
static const char *include_stack[INCLUDE_MAX_DEPTH];
static int include_depth = 0;

/* Every file ever spliced, by its path_key, so a file reached a second
 * time (a diamond, two included files that both include a third) is spliced
 * once and skipped after -- faster, and it removes the duplicate-definition
 * error that a second textual copy would otherwise cause. A true cycle is
 * still caught above by include_stack before we get here. */
#define MAX_INCLUDED 256
static const char *included[MAX_INCLUDED];
static int n_included = 0;

/* The path with "." parts dropped and "dir/.." folded away, the key that
 * tells whether two include paths name the same file. A ".." with nothing
 * left to fold stays, and one above the root is the root. Messages keep the
 * path as written. Symbolic links are not followed, so two names for one
 * file through a link still count as two. */
static char *path_key(const char *path) {
  long n = strlen(path);
  char *out = malloc(n + 2);
  long len = 0;
  int abs = path[0] == '/';
  if (abs) out[len++] = '/';
  long base = len;
  long i = 0;
  while (i < n) {
    while (i < n && path[i] == '/') i++;
    long s = i;
    while (i < n && path[i] != '/') i++;
    long plen = i - s;
    if (plen == 0 || (plen == 1 && path[s] == '.')) {
      /* nothing */
    } else if (plen == 2 && path[s] == '.' && path[s + 1] == '.') {
      long ls = len;
      while (ls > base && out[ls - 1] != '/') ls--;
      int last_is_up = len - ls == 2 && out[ls] == '.' && out[ls + 1] == '.';
      if (len > base && !last_is_up) {
        len = ls;
        if (len > base) len--;
      } else if (!abs) {
        if (len > base) out[len++] = '/';
        out[len++] = '.';
        out[len++] = '.';
      }
    } else {
      if (len > base) out[len++] = '/';
      memcpy(out + len, path + s, plen);
      len += plen;
    }
  }
  if (len == 0) out[len++] = '.';
  out[len] = 0;
  return out;
}

/* The line `at` is on, counting from the start of `src`. */
static int line_in(const char *src, const char *at) {
  int line = 1;
  for (const char *c = src; c < at; c++) if (*c == '\n') line++;
  return line;
}

static void expand_file(Growbuf *out, const char *path) {
  const char *key = path_key(path);
  for (int i = 0; i < n_included; i++)
    if (strcmp(included[i], key) == 0) return; /* include-once, already spliced */
  if (n_included >= MAX_INCLUDED) error("include: more than %d distinct files", MAX_INCLUDED);
  included[n_included++] = key;
  include_stack[include_depth++] = key;

  char *src = read_file(path);
  char *dir = dirname_with_slash(path);
  char *p = src;
  add_seg(out->len, path, 1);

  while (*p) {
    if (*p == '"') {
      const char *start = p;
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (*p == '"') p++;
      gb_puts_n(out, start, p - start);
      continue;
    }
    /* Char literals ('x', including quote characters like '"' or '\'')
     * need the same verbatim-copy treatment as strings. This scanner's own
     * source contains `!= '"'` (a char literal for the double-quote
     * character). Without this case the scanner would see that bare `"`
     * and enter STRING mode there, scanning forward to whatever `"` comes
     * next in the file and silently swallowing a large, arbitrary span of
     * real code as if it were string content. */
    if (*p == '\'') {
      const char *start = p;
      p++;
      if (*p == '\\' && p[1]) p += 2;
      else if (*p) p++;
      if (*p == '\'') p++;
      gb_puts_n(out, start, p - start);
      continue;
    }
    if (p[0] == '/' && p[1] == '/') {
      const char *start = p;
      while (*p && *p != '\n') p++;
      gb_puts_n(out, start, p - start);
      continue;
    }
    if (is_ident_start((unsigned char)*p)) {
      const char *word_start = p;
      while (is_ident_char((unsigned char)*p)) p++;
      long wlen = p - word_start;
      if (wlen == 7 && strncmp(word_start, "include", 7) == 0) {
        /* Every problem with a directive is reported at the directive. */
        err_file = path;
        err_line = line_in(src, word_start);
        char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"') error("include: expected a string after 'include'");
        q++;
        char *pstart = q;
        while (*q && *q != '"') q++;
        if (*q != '"') error("include: unterminated path string");
        char *rel = malloc(q - pstart + 1);
        memcpy(rel, pstart, q - pstart);
        rel[q - pstart] = 0;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ';') error("include: expected ';' after the include path");
        q++;
        char *inc_path = join_path(dir, rel);
        if (!file_exists(inc_path) && rel[0] != '/') {
          for (int k = 0; k < n_inc_dirs; k++) {
            char *cand = join_path(inc_dirs[k], rel);
            if (file_exists(cand)) { inc_path = cand; break; }
          }
        }
        char *inc_key = path_key(inc_path);
        for (int i = 0; i < include_depth; i++) {
          if (strcmp(include_stack[i], inc_key) == 0) error("include: circular include of %s", inc_path);
        }
        if (include_depth >= INCLUDE_MAX_DEPTH) error("include: nested more than %d files deep", INCLUDE_MAX_DEPTH);
        FILE *probe = fopen(inc_path, "rb");
        if (!probe) error("include: cannot open %s", inc_path);
        fclose(probe);
        expand_file(out, inc_path);
        /* back in this file, at the line the directive ended on */
        add_seg(out->len, path, line_in(src, q));
        p = q;
        continue;
      }
      gb_puts_n(out, word_start, wlen);
      continue;
    }
    gb_putc(out, *p);
    p++;
  }

  include_depth--;
}

static char *resolve_includes(const char *path) {
  Growbuf out;
  gb_init(&out);
  expand_file(&out, path);
  gb_putc(&out, 0);
  return out.data;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "-t") == 0) {
    src_name = argv[2];
    tokens = tokenize(resolve_includes(argv[2]));
    dump_tokens();
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "-a") == 0) {
    src_name = argv[2];
    tokens = tokenize(resolve_includes(argv[2]));
    pos = 0;
    parse_program();
    dump_ast();
    return 0;
  }
  /* [--elf] [-I dir]... input output, in any order. */
  char *pos_args[2];
  int npos = 0;
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--elf") == 0) {
      elf_mode = 1;
    } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc && n_inc_dirs < MAX_INC_DIRS) {
      const char *d = argv[++i];
      long dl = strlen(d);
      char *withslash = malloc(dl + 2);
      memcpy(withslash, d, dl);
      withslash[dl] = 0;
      if (dl == 0 || d[dl - 1] != '/') { withslash[dl] = '/'; withslash[dl + 1] = 0; }
      inc_dirs[n_inc_dirs++] = withslash;
    } else if (npos < 2) {
      pos_args[npos++] = argv[i];
    } else {
      bad = 1;
    }
  }
  if (bad || npos != 2) {
    fprintf(stderr, "usage: coff0 [--elf] [-I dir]... <input.c0> <output.s>  |  coff0 -t <input.c0>\n");
    return 1;
  }
  const char *out_path = pos_args[1];
  src_name = pos_args[0];
  char *src = resolve_includes(pos_args[0]);

  tokens = tokenize(src);
  pos = 0;
  parse_program();

  int found_main = 0;
  /* Each declaration check points at the declaration's name. */
  for (Function *f = prog_functions; f; f = f->next) {
    err_file = f->file;
    err_line = f->line;
    if (find_builtin(f->name)) {
      error("'%s' is a builtin and cannot be redefined", f->name);
    }
    check_symbol_name(f->name, "a function");
    func_declare_sig(f->name, f->nparams, f->two_ret);
    if (strcmp(f->name, "main") == 0) {
      if (f->nparams != 0) error("'main' must take no parameters");
      if (f->is_void) error("'main' must return int, not void");
      if (f->two_ret) error("'main' must return one int, not two");
      found_main = 1;
    }
  }
  err_line = 0;
  if (!found_main) error("no 'main' function defined");

  for (Global *g = prog_globals; g; g = g->next) {
    err_file = g->file;
    err_line = g->line;
    if (find_builtin(g->name)) error("'%s' is a builtin and cannot be redefined", g->name);
    for (Function *f = prog_functions; f; f = f->next) {
      if (strcmp(f->name, g->name) == 0) error("'%s' is both a function and a global", g->name);
    }
    glob_declare(g->name);
  }

  for (Global *e = prog_externs; e; e = e->next) {
    err_file = e->file;
    err_line = e->line;
    if (find_builtin(e->name)) error("'%s' is a builtin and cannot be used as extern", e->name);
    check_symbol_name(e->name, "an extern");
    for (Global *g = prog_globals; g; g = g->next) {
      if (strcmp(g->name, e->name) == 0) error("'%s' is both a global and an extern", e->name);
    }
    extern_declare(e->name);
  }

  /* An embed's name is an address, like an extern; name_size is a global
   * the assembler fills in. */
  for (Embed *e = prog_embeds; e; e = e->next) {
    err_file = e->file;
    err_line = e->line;
    if (find_builtin(e->name)) error("'%s' is a builtin and cannot be used as embed", e->name);
    check_symbol_name(e->name, "an embed");
    if (glob_exists(e->name) || func_exists(e->name) || extern_exists(e->name))
      error("'%s' is declared twice", e->name);
    extern_declare(e->name);
    char *size_name = malloc(strlen(e->name) + 6);
    sprintf(size_name, "%s_size", e->name);
    if (glob_exists(size_name) || func_exists(size_name) || extern_exists(size_name))
      error("'%s' is declared twice", size_name);
    glob_declare(size_name);
  }

  for (Layout *l = prog_layouts; l; l = l->next) layout_declare(l);
  for (Global *g = prog_globals; g; g = g->next) {
    err_file = g->file;
    err_line = g->line;
    check_type(g->type);
  }

  for (Function *f = prog_functions; f; f = f->next) resolve_function(f);

  out = fopen(out_path, "w");
  if (!out) { perror(out_path); return 1; }

  fprintf(out, ".intel_syntax noprefix\n");
  fprintf(out, ".global _start\n");
  fprintf(out, "_start:\n");
  fprintf(out, "    mov [.Largv0], rsp\n");
  fprintf(out, "    call main\n");
  fprintf(out, "    mov rdi, rax\n");
  if (elf_mode) fprintf(out, "    mov rax, 0\n");   /* Moonshot SYS_EXIT with main's value */
  else          fprintf(out, "    mov rax, 60\n");  /* Linux exit */
  fprintf(out, "    syscall\n");

  for (Function *f = prog_functions; f; f = f->next) gen_function(f);

  if (string_count > 0) {
    fprintf(out, "\n.section .rodata\n");
    for (int i = 0; i < string_count; i++) {
      fprintf(out, ".Lstr%d:\n", i);
      fprintf(out, "    .byte");
      for (int j = 0; j < string_len[i]; j++) {
        fprintf(out, " %d,", (unsigned char)string_data[i][j]);
      }
      fprintf(out, " 0\n");
    }
  }

  /* Embedded files, as the assembler reads them, each followed by a label
   * so their length is a difference of two addresses. */
  int emb_i = 0;
  for (Embed *e = prog_embeds; e; e = e->next, emb_i++) {
    if (emb_i == 0) fprintf(out, "\n.section .rodata\n");
    fprintf(out, ".balign 8\n%s:\n    .incbin \"%s\"\n.Lembend%d:\n", e->name, e->resolved, emb_i);
  }

  /* alloc()'s cached program break, argc/argv's saved initial rsp, then one
   * .quad slot per global. */
  fprintf(out, "\n.section .data\n");
  fprintf(out, ".Lcurbrk:\n    .quad 0\n");
  fprintf(out, ".Largv0:\n    .quad 0\n");
  for (Global *g = prog_globals; g; g = g->next) {
    fprintf(out, ".G%s:\n    .quad %ld\n", g->name, g->init);
  }
  emb_i = 0;
  for (Embed *e = prog_embeds; e; e = e->next, emb_i++) {
    fprintf(out, ".G%s_size:\n    .quad .Lembend%d - %s\n", e->name, emb_i, e->name);
  }

  fclose(out);
  return 0;
}
