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
 * Purpose: the bootstrap stage. Plain C, compiled with the host gcc, and
 * it exists so the self-hosted compiler (c0/coff.c0) has something to be
 * compiled by the first time. After that it stays around as the oracle:
 * run_tests.sh diffs the output of every c0-written stage against this
 * one, byte for byte. It is not the compiler you are meant to use day to
 * day - that is coff1, the binary built from c0/coff.c0.
 *
 * The c0 grammar, as accepted here:
 *
 *   program    := (function | global | extern | layout)*
 *   global     := "int" IDENT ("=" "-"? NUM)? ";"     (constant initializer only)
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
 *   function   := ("int" | "void") IDENT "(" params? ")" block
 *   params     := "int" IDENT ("," "int" IDENT)*        (max 16 total; first 6
 *                                                          register-passed, rest
 *                                                          on the stack -- see
 *                                                          gen_call/gen_function)
 *   block      := "{" stmt* "}"
 *   stmt       := decl_stmt | assign_stmt | if_stmt | while_stmt
 *               | return_stmt | break_stmt | continue_stmt | expr_stmt | block
 *   decl_stmt  := "int" IDENT ("=" expr)? ";"
 *   assign_stmt:= (IDENT | postfix) "=" expr ";"      (a postfix target must end
 *                                                        in an index or a field,
 *                                                        e.g. buf[i] = v or
 *                                                        e.hp = 10)
 *   if_stmt    := "if" "(" expr ")" stmt ("else" stmt)?
 *   while_stmt := "while" "(" expr ")" stmt
 *   return_stmt:= "return" expr? ";"                    ("return;" bare is required
 *                                                          for void functions, optional
 *                                                          for int functions -- falling
 *                                                          off the end already left the
 *                                                          return value undefined)
 *   break_stmt := "break" ";"                            (must be inside a while loop)
 *   continue_stmt := "continue" ";"                      (must be inside a while loop)
 *   expr_stmt  := expr ";"                               (a bare call, e.g. `f(1);`)
 *
 *   expr       := logic_or
 *   logic_or   := logic_and ("||" logic_and)*        (short-circuiting)
 *   logic_and  := equality ("&&" equality)*            (short-circuiting)
 *   equality   := relational (("==" | "!=") relational)*
 *   relational := additive (("<" | "<=" | ">" | ">=") additive)*
 *   additive   := term (("+" | "-") term)*
 *   term       := unary (("*" | "/") unary)*
 *   unary      := "-" unary | postfix
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
 * A `main` function is required, takes no parameters, and must return
 * int (not void). The generated `_start` calls main and exits the
 * process with its return value. Every other function returns normally
 * to its caller, args in rdi/rsi/rdx/rcx/r8/r9 and the return value in
 * rax. A void function may only use a bare `return;`, or fall off the
 * end.
 *
 * c0 has one type: the 64-bit integer. A string literal is stored
 * null-terminated in .rodata and evaluates to its address, as a plain
 * int. A bare function name (an IDENT not followed by "(") evaluates to
 * that function's entry address the same way, so functions can be stored
 * in variables and passed around; `IDENT(args)` where IDENT is such a
 * variable becomes an indirect call through its value. There is no arity
 * checking on indirect calls - an int carries no arity - the caller is
 * trusted, like everywhere else in this language.
 *
 * Builtins (not real c0 functions; the names cannot be redefined):
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
 *   exit(code)         -- terminates the process immediately (exit(2) syscall)
 *   argc()             -- the process's argument count (argv[0] is the
 *                          program name, same convention as C)
 *   argv(i)            -- the address of the i'th argument string
 *                          (null-terminated); i must be < argc()
 *   outb(port, v)      -- x86 `out dx, al`: writes the low byte of v to
 *                          I/O port `port`; evaluates to v. Faults in
 *                          normal Linux userspace (needs ring 0 or IOPL);
 *                          this one is for freestanding/kernel targets.
 *   inb(port)          -- x86 `in al, dx`: reads a byte from I/O port
 *                          `port`, zero-extended. Same ring-0 caveat.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

static char *src_name;

static void error(const char *fmt, ...) __attribute__((noreturn));
static void error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "coff0: %s: ", src_name);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

/* ---------------------------------------------------------------- lexer */

typedef enum {
  TK_NUM, TK_IDENT, TK_STRING,
  TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
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
  TK_KW_LAYOUT, TK_KW_SIZEOF,
} TokenKind;

typedef struct {
  TokenKind kind;
  long ival;
  char *name;
} Token;

static Token *tokens;
static int ntokens;

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_char(int c) { return isalnum(c) || c == '_'; }

static Token *tokenize(char *src) {
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

    if (isdigit((unsigned char)*p)) {
      char *start = p;
      if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        while (isxdigit((unsigned char)*p)) p++;
        t->kind = TK_NUM;
        /* strtoul, not strtol -- see the comment on the decimal path below,
         * same bug, found here first with a real high-bit-set value (an
         * 8x8 font glyph's packed bitmap, 0xF88484F88484F800) silently
         * coming back as 0x7FFFFFFFFFFFFFFF instead of itself. */
        t->ival = (long)strtoul(start, NULL, 16);
        n++;
        continue;
      }
      while (isdigit((unsigned char)*p)) p++;
      t->kind = TK_NUM;
      /* strtoul, not strtol: c0's ival is a raw 64-bit bit pattern (c0 has
       * no unsigned type -- a literal at or past 2^63 is just what its bits
       * are, reinterpreted as negative, same as every other c0 int). strtol
       * clamps any digit sequence that does not fit as a *signed* long to
       * LONG_MAX with ERANGE, silently corrupting the top half of the
       * range instead of wrapping -- the c0-written lexers (lex.c0/
       * parse.c0/coff.c0) never had this bug, since their digit
       * accumulation (`v = v*10 + digit`) is pure arithmetic on the CPU's
       * own registers, which wraps naturally with no library-level
       * clamping. strtoul parses the same digits as an unsigned magnitude
       * with no clamping, and converting that back to `long` just
       * reinterprets the bits (well-defined in practice on every platform
       * this targets). */
      t->ival = (long)strtoul(start, NULL, 10);
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
      case '+': t->kind = TK_PLUS; p++; break;
      case '-': t->kind = TK_MINUS; p++; break;
      case '*': t->kind = TK_STAR; p++; break;
      case '/': t->kind = TK_SLASH; p++; break;
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
        error("unexpected character '!'");
        break;
      case '<':
        if (p[1] == '=') { t->kind = TK_LE; p += 2; } else { t->kind = TK_LT; p++; }
        break;
      case '>':
        if (p[1] == '=') { t->kind = TK_GE; p += 2; } else { t->kind = TK_GT; p++; }
        break;
      case '&':
        if (p[1] == '&') { t->kind = TK_AND; p += 2; break; }
        error("unexpected character '&'");
        break;
      case '|':
        if (p[1] == '|') { t->kind = TK_OR; p += 2; break; }
        error("unexpected character '|'");
        break;
      default:
        error("unexpected character '%c'", *p);
    }
    n++;
  }

  if (n >= cap) { cap += 1; out = realloc(out, sizeof(Token) * cap); }
  out[n].kind = TK_EOF;
  n++;

  ntokens = n;
  return out;
}

/* --------------------------------------------------------------- parser */

typedef enum {
  ND_NUM, ND_STR, ND_VAR, ND_ASSIGN, ND_DECL, ND_CALL,
  ND_ADD, ND_SUB, ND_MUL, ND_DIV,
  ND_EQ, ND_NE, ND_LT, ND_LE, ND_GT, ND_GE,
  ND_NEG,
  ND_IF, ND_WHILE, ND_RETURN, ND_EXPR_STMT, ND_BLOCK,
  ND_AND, ND_OR,
  ND_BREAK, ND_CONTINUE,
  ND_INDEX, ND_INDEX_ASSIGN,
  ND_FUNCREF,
  ND_FIELD, ND_FIELD_ASSIGN, ND_SIZEOF,
} NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *lhs, *rhs;    /* binary ops; ASSIGN/DECL: rhs = value expr (may be NULL for DECL);
                         INDEX/INDEX_ASSIGN: lhs = base pointer expr, rhs = index expr */
  long ival;          /* NUM literal; resolved stack offset for VAR/ASSIGN/DECL/(indirect) CALL */
  int is_global;      /* VAR/ASSIGN/(indirect) CALL: name resolved to a global, not a stack slot */
  int is_indirect;    /* CALL: `name` is a function-pointer-valued variable, not a
                         declared function -- call through its value (see resolve()) */
  int str_id;         /* STR: index into the string-literal table */
  char *name;         /* VAR / ASSIGN / DECL / CALL / FUNCREF */
  Node *cond, *then, *els; /* IF; INDEX_ASSIGN: cond = value expr (`buf[i] = value`) */
  Node *body;         /* WHILE body; BLOCK: head of statement list */
  Node *args;         /* CALL: head of argument-expression list (chained via next) */
  int nargs;          /* CALL: argument count */
  Node *next;         /* next statement in a BLOCK, or next arg in a CALL's args list */
};

#define MAX_PARAMS 6          /* register-passed params (rdi/rsi/rdx/rcx/r8/r9) */
#define MAX_TOTAL_PARAMS 16   /* overall cap on declared params, register + stack */

typedef struct Param Param;
struct Param {
  char *name;
  Param *next;
};

typedef struct Function Function;
struct Function {
  char *name;
  char *params[MAX_PARAMS]; /* first MAX_PARAMS params (register-passed) */
  Param *extra_params;      /* params beyond MAX_PARAMS (stack-passed), in order */
  int nparams;
  int is_void;      /* declared "void" instead of "int" -- may not return a value */
  Node *body;
  long stack_size; /* filled in by resolve_function() */
  int ret_label;    /* filled in just before codegen */
  Function *next;
};

/* Name of the i'th declared parameter (0-indexed), regardless of whether it
 * lives in the inline register-param slots or the extra_params list. */
static char *param_name(Function *f, int i) {
  if (i < MAX_PARAMS) return f->params[i];
  Param *p = f->extra_params;
  for (int k = i - MAX_PARAMS; k > 0; k--) p = p->next;
  return p->name;
}

typedef struct Global Global;
struct Global {
  char *name;
  long init;
  Global *next;
};

typedef struct LayoutField LayoutField;
struct LayoutField {
  char *name;
  LayoutField *next;
};

typedef struct Layout Layout;
struct Layout {
  char *name;
  LayoutField *fields;
  int nfields;
  Layout *next;
};

static Node *new_node(NodeKind kind) {
  Node *n = calloc(1, sizeof(Node));
  n->kind = kind;
  return n;
}

#define MAX_STRINGS 4096
static char *string_data[MAX_STRINGS];
static int string_len[MAX_STRINGS];
static int string_count;

static int register_string(char *decoded, int len) {
  if (string_count >= MAX_STRINGS) error("too many string literals (max %d)", MAX_STRINGS);
  string_data[string_count] = decoded;
  string_len[string_count] = len;
  return string_count++;
}

static int pos;
static Token *peek(void) { return &tokens[pos]; }
static Token *advance(void) { return &tokens[pos++]; }
static int check(TokenKind k) { return peek()->kind == k; }
static Token *expect(TokenKind k, const char *what) {
  if (!check(k)) error("expected %s", what);
  return advance();
}

static Node *parse_expr(void);
static Node *parse_stmt(void);
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
  return parse_postfix();
}

static Node *parse_term(void) {
  Node *n = parse_unary();
  while (check(TK_STAR) || check(TK_SLASH)) {
    NodeKind k = check(TK_STAR) ? ND_MUL : ND_DIV;
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

static Node *parse_relational(void) {
  Node *n = parse_additive();
  for (;;) {
    NodeKind k;
    if (check(TK_LT)) k = ND_LT;
    else if (check(TK_LE)) k = ND_LE;
    else if (check(TK_GT)) k = ND_GT;
    else if (check(TK_GE)) k = ND_GE;
    else break;
    advance();
    Node *b = new_node(k);
    b->lhs = n; b->rhs = parse_additive();
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

static Node *parse_logic_and(void) {
  Node *n = parse_equality();
  while (check(TK_AND)) {
    advance();
    Node *b = new_node(ND_AND);
    b->lhs = n; b->rhs = parse_equality();
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

  if (check(TK_KW_INT)) {
    advance();
    Node *n = new_node(ND_DECL);
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
    if (!check(TK_SEMI)) n->lhs = parse_expr();
    expect(TK_SEMI, "';'");
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

  /* Parse a full expression first, then decide: a plain assignment
   * (VAR '=' expr), an indexed assignment (buf[i] '=' expr), or -- if not
   * followed by '=' at all -- an expression statement (e.g. `f(1);`). This
   * needs arbitrary lookahead (buf[i] can be any length), so unlike the
   * other statement forms it cannot be dispatched on a fixed token peek. */
  Node *e = parse_expr();
  if (check(TK_ASSIGN)) {
    advance();
    if (e->kind == ND_VAR) {
      Node *n = new_node(ND_ASSIGN);
      n->name = e->name;
      n->rhs = parse_expr();
      expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_INDEX) {
      Node *n = new_node(ND_INDEX_ASSIGN);
      n->lhs = e->lhs;
      n->rhs = e->rhs;
      n->cond = parse_expr();
      expect(TK_SEMI, "';'");
      return n;
    }
    if (e->kind == ND_FIELD) {
      Node *n = new_node(ND_FIELD_ASSIGN);
      n->lhs = e->lhs;
      n->name = e->name;
      n->rhs = parse_expr();
      expect(TK_SEMI, "';'");
      return n;
    }
    error("invalid assignment target");
  }

  Node *n = new_node(ND_EXPR_STMT);
  n->lhs = e;
  expect(TK_SEMI, "';'");
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
      expect(TK_KW_INT, "'int' (only int parameters are supported)");
      if (f->nparams >= MAX_TOTAL_PARAMS) error("too many parameters for '%s' (max %d)", f->name, MAX_TOTAL_PARAMS);
      char *pname = expect(TK_IDENT, "a parameter name")->name;
      if (f->nparams < MAX_PARAMS) {
        f->params[f->nparams] = pname;
      } else {
        Param *p = calloc(1, sizeof(Param));
        p->name = pname;
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

static Global *parse_global(char *name) {
  Global *g = calloc(1, sizeof(Global));
  g->name = name;
  if (check(TK_ASSIGN)) {
    advance();
    long sign = 1;
    if (check(TK_MINUS)) { advance(); sign = -1; }
    g->init = sign * expect(TK_NUM, "a constant integer initializer")->ival;
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
  l->name = expect(TK_IDENT, "a layout name")->name;
  expect(TK_LBRACE, "'{'");
  LayoutField head = {0};
  LayoutField *tail = &head;
  while (!check(TK_RBRACE)) {
    expect(TK_KW_INT, "'int' (every layout field is an int)");
    LayoutField *f = calloc(1, sizeof(LayoutField));
    f->name = expect(TK_IDENT, "a field name")->name;
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
  while (!check(TK_EOF)) {
    if (check(TK_KW_EXTERN)) {
      advance();
      expect(TK_KW_INT, "'int' after 'extern'");
      char *name = expect(TK_IDENT, "a name")->name;
      expect(TK_SEMI, "';'");
      etail->next = parse_extern(name);
      etail = etail->next;
      continue;
    }
    if (check(TK_KW_LAYOUT)) {
      advance();
      ltail->next = parse_layout();
      ltail = ltail->next;
      continue;
    }
    int is_void = 0;
    if (check(TK_KW_VOID)) { is_void = 1; advance(); }
    else expect(TK_KW_INT, "'int' or 'void'");
    char *name = expect(TK_IDENT, "a name")->name;
    if (check(TK_LPAREN)) {
      ftail->next = parse_function(name);
      ftail->next->is_void = is_void;
      ftail = ftail->next;
    } else {
      if (is_void) error("'void' is not a valid type for a global");
      gtail->next = parse_global(name);
      gtail = gtail->next;
    }
  }
  prog_functions = fhead.next;
  prog_globals = ghead.next;
  prog_externs = ehead.next;
  prog_layouts = lhead.next;
}

/* ------------------------------------------------------------- resolve */
/* Two jobs: (1) a first pass over every function collects name+arity into a
 * signature table so calls can reference functions regardless of source
 * order (including mutual recursion); (2) a per-function pass assigns every
 * declared variable (including parameters) a stack slot and checks that
 * every reference/assignment/call refers to something already declared with
 * the right arity. c0 still has a single flat per-function scope regardless
 * of block nesting -- a deliberate simplification, not an oversight. */

#define MAX_VARS 4096
#define MAX_FUNCS 4096

typedef struct { const char *name; int nargs; } Builtin;
static const Builtin BUILTINS[] = {
  { "print", 1 }, { "write", 3 }, { "read", 3 },
  { "open_read", 1 }, { "open_write", 1 }, { "close", 1 },
  { "alloc", 1 }, { "load8", 1 }, { "store8", 2 },
  { "load64", 1 }, { "store64", 2 }, { "exit", 1 },
  { "argc", 0 }, { "argv", 1 },
  { "outb", 2 }, { "inb", 1 },
  { "outw", 2 }, { "inw", 1 },
};
#define NBUILTINS ((int)(sizeof(BUILTINS) / sizeof(BUILTINS[0])))

static const Builtin *find_builtin(const char *name) {
  for (int i = 0; i < NBUILTINS; i++) {
    if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
  }
  return NULL;
}

static char *func_names[MAX_FUNCS];
static int func_nparams[MAX_FUNCS];
static int func_count;

static void func_declare_sig(char *name, int nparams) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) error("redefinition of function '%s'", name);
  }
  if (func_count >= MAX_FUNCS) error("too many functions (max %d)", MAX_FUNCS);
  func_names[func_count] = name;
  func_nparams[func_count] = nparams;
  func_count++;
}

static int func_lookup_nparams(char *name) {
  for (int i = 0; i < func_count; i++) {
    if (strcmp(func_names[i], name) == 0) return func_nparams[i];
  }
  error("call to undeclared function '%s'", name);
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

/* Field names live in a single flat namespace shared across every `layout`
 * declaration, not one scoped per layout -- c0 has no type system, so
 * `e.x` cannot be checked against "the layout e was allocated as", and
 * does not try to be. Instead, the first layout to declare a field name
 * fixes that name's byte offset for good; every other layout that reuses
 * the name must agree on the same offset (i.e. the same position within
 * its own field list), or it is a compile error. This is a real,
 * deliberate restriction (two layouts cannot independently give "x" two
 * different meanings) -- the same "trust the programmer, one flat
 * namespace" spirit as c0's single function/global namespace. */
#define MAX_FIELDS 4096
static char *field_names[MAX_FIELDS];
static long field_offsets[MAX_FIELDS];
static int field_count;

static long field_lookup(const char *name) {
  for (int i = 0; i < field_count; i++) {
    if (strcmp(field_names[i], name) == 0) return field_offsets[i];
  }
  error("unknown field '%s' (not declared in any layout)", name);
  return -1;
}

static void field_declare_or_check(const char *name, long offset) {
  for (int i = 0; i < field_count; i++) {
    if (strcmp(field_names[i], name) == 0) {
      if (field_offsets[i] != offset) {
        error("field '%s' at offset %ld conflicts with its earlier use at offset %ld "
              "(every layout that shares a field name must declare it at the same position)",
              name, offset, field_offsets[i]);
      }
      return;
    }
  }
  if (field_count >= MAX_FIELDS) error("too many distinct field names (max %d)", MAX_FIELDS);
  field_names[field_count] = (char *)name;
  field_offsets[field_count] = offset;
  field_count++;
}

#define MAX_LAYOUTS 256
static char *layout_names[MAX_LAYOUTS];
static long layout_sizes[MAX_LAYOUTS];
static int layout_count;

static long layout_lookup_size(const char *name) {
  for (int i = 0; i < layout_count; i++) {
    if (strcmp(layout_names[i], name) == 0) return layout_sizes[i];
  }
  error("unknown layout '%s' in sizeof(...)", name);
  return -1;
}

/* Validates every `layout` declaration (called once, up front, same spot
 * globals/externs are declared): checks for duplicate field names within
 * one layout, registers each field's offset in the shared field table
 * (erroring on a cross-layout conflict), and records the layout's own
 * size (nfields * 8) for sizeof(...). */
static void layout_declare(Layout *l) {
  for (int i = 0; i < layout_count; i++) {
    if (strcmp(layout_names[i], l->name) == 0) error("redefinition of layout '%s'", l->name);
  }
  if (layout_count >= MAX_LAYOUTS) error("too many layouts (max %d)", MAX_LAYOUTS);

  long offset = 0;
  for (LayoutField *f = l->fields; f; f = f->next) {
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
static int sym_count;
static long stack_size;
static int loop_depth; /* nesting depth of while loops during resolve(); break/continue require loop_depth > 0 */
static int current_fn_is_void; /* whether the function currently being resolved is void */

/* Block scoping: each '{' block marks the sym_count at entry; sym_declare's
 * redeclaration check only looks back to that mark, so sibling blocks can
 * reuse a name without colliding. Leaving a block truncates sym_count back
 * to the mark, which also means sibling blocks' locals get the same stack
 * offsets (safe: their lifetimes never overlap) -- a size win that falls
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

static long sym_declare(char *name) {
  int scope_start = scope_depth > 0 ? scope_marks[scope_depth - 1] : 0;
  for (int i = scope_start; i < sym_count; i++) {
    if (strcmp(sym_names[i], name) == 0) error("redeclaration of '%s'", name);
  }
  if (sym_count >= MAX_VARS) error("too many variables (max %d)", MAX_VARS);
  long offset = (sym_count + 1) * 8;
  sym_names[sym_count] = name;
  sym_offsets[sym_count] = offset;
  sym_count++;
  if (offset > stack_size) stack_size = offset;
  return offset;
}

static void resolve(Node *n) {
  if (!n) return;
  switch (n->kind) {
    case ND_NUM: return;
    case ND_STR: return;
    case ND_VAR: {
      long off = sym_try_lookup(n->name); /* locals shadow globals shadow functions */
      if (off >= 0) { n->ival = off; return; }
      if (glob_exists(n->name)) { n->is_global = 1; return; }
      if (extern_exists(n->name)) { n->kind = ND_FUNCREF; return; }
      if (func_exists(n->name)) { n->kind = ND_FUNCREF; return; }
      error("undeclared variable '%s'", n->name);
      return;
    }
    case ND_FUNCREF: return; /* only ever reached already-resolved, via the VAR case above */
    case ND_NEG: resolve(n->lhs); return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
    case ND_AND: case ND_OR:
      resolve(n->lhs); resolve(n->rhs); return;
    case ND_ASSIGN: {
      resolve(n->rhs);
      long off = sym_try_lookup(n->name);
      if (off >= 0) n->ival = off;
      else if (glob_exists(n->name)) n->is_global = 1;
      else error("undeclared variable '%s'", n->name);
      return;
    }
    case ND_DECL:
      resolve(n->rhs); /* must resolve before the new name enters scope */
      n->ival = sym_declare(n->name);
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
      } else {
        /* Not a builtin, not a declared function -- an indirect call through
         * a function-pointer-valued variable, if `name` names one. */
        long off = sym_try_lookup(n->name);
        if (off >= 0) { n->ival = off; n->is_indirect = 1; }
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
    case ND_RETURN:
      if (current_fn_is_void && n->lhs) error("'void' function cannot return a value");
      resolve(n->lhs); return;
    case ND_EXPR_STMT:
      resolve(n->lhs); return;
    case ND_BLOCK: {
      if (scope_depth >= MAX_SCOPE_DEPTH) error("blocks nested too deeply (max %d)", MAX_SCOPE_DEPTH);
      scope_marks[scope_depth++] = sym_count;
      for (Node *s = n->body; s; s = s->next) resolve(s);
      scope_depth--;
      sym_count = scope_marks[scope_depth];
      return;
    }
    case ND_BREAK:
      if (loop_depth <= 0) error("'break' outside of a loop");
      return;
    case ND_CONTINUE:
      if (loop_depth <= 0) error("'continue' outside of a loop");
      return;
    case ND_INDEX:
      resolve(n->lhs); resolve(n->rhs); return;
    case ND_INDEX_ASSIGN:
      resolve(n->lhs); resolve(n->rhs); resolve(n->cond); return;
    case ND_FIELD:
      resolve(n->lhs);
      n->ival = field_lookup(n->name);
      return;
    case ND_FIELD_ASSIGN:
      resolve(n->lhs); resolve(n->rhs);
      n->ival = field_lookup(n->name);
      return;
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
  for (int i = 0; i < f->nparams; i++) sym_declare(param_name(f, i));
  resolve(f->body);
  f->stack_size = stack_size;
}

/* -------------------------------------------------------------- codegen */

static const char *ARG_REGS[MAX_PARAMS] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static FILE *out;
static int label_count;
static int current_ret_label;

#define MAX_LOOP_DEPTH 64
static int loop_labels[MAX_LOOP_DEPTH]; /* one label id per active while loop; .Lstart%d/.Lend%d are break/continue's targets */
static int gen_loop_depth;

static void gen_expr(Node *n) {
  switch (n->kind) {
    case ND_NUM:
      fprintf(out, "    mov rax, %ld\n", n->ival);
      return;
    case ND_STR:
      fprintf(out, "    lea rax, [.Lstr%d]\n", n->str_id);
      return;
    case ND_VAR:
      if (n->is_global) fprintf(out, "    mov rax, [.G%s]\n", n->name);
      else fprintf(out, "    mov rax, [rbp - %ld]\n", n->ival);
      return;
    case ND_FUNCREF:
      fprintf(out, "    lea rax, [%s]\n", n->name);
      return;
    case ND_NEG:
      gen_expr(n->lhs);
      fprintf(out, "    neg rax\n");
      return;
    case ND_INDEX:
      gen_expr(n->lhs);        /* base */
      fprintf(out, "    push rax\n");
      gen_expr(n->rhs);        /* index */
      fprintf(out, "    mov rcx, rax\n");
      fprintf(out, "    pop rax\n");
      fprintf(out, "    add rax, rcx\n");
      fprintf(out, "    movzx rax, byte ptr [rax]\n");
      return;
    case ND_FIELD:
      gen_expr(n->lhs);
      fprintf(out, "    mov rax, [rax + %ld]\n", n->ival);
      return;
    case ND_CALL: {
      if (strcmp(n->name, "print") == 0) {
        int l = label_count++;
        gen_expr(n->args); /* -> rax: pointer to a null-terminated string */
        fprintf(out, "    mov rbx, rax\n");
        fprintf(out, "    xor rcx, rcx\n");
        fprintf(out, ".Lstrlen%d:\n", l);
        fprintf(out, "    cmp byte ptr [rbx + rcx], 0\n");
        fprintf(out, "    je .Lstrlend%d\n", l);
        fprintf(out, "    inc rcx\n");
        fprintf(out, "    jmp .Lstrlen%d\n", l);
        fprintf(out, ".Lstrlend%d:\n", l);
        fprintf(out, "    mov rdx, rcx\n");
        fprintf(out, "    mov rsi, rbx\n");
        fprintf(out, "    mov rdi, 1\n");
        fprintf(out, "    mov rax, 1\n");
        fprintf(out, "    syscall\n");
        return;
      }
      if (strcmp(n->name, "alloc") == 0) {
        /* Bump allocator over brk(2). The current break is cached in .Lcurbrk
         * (0 = not fetched yet). NB: syscall clobbers rcx/r11, so live values
         * stay on the stack across each syscall. */
        int l = label_count++;
        gen_expr(n->args); /* -> rax: n */
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
        gen_expr(n->args);        /* addr */
        fprintf(out, "    push rax\n");
        gen_expr(n->args->next);  /* value */
        fprintf(out, "    pop rdi\n");
        fprintf(out, "    mov byte ptr [rdi], al\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "exit") == 0) {
        gen_expr(n->args);
        fprintf(out, "    mov rdi, rax\n");
        fprintf(out, "    mov rax, 60\n");
        fprintf(out, "    syscall\n");
        return;
      }
      if (strcmp(n->name, "load64") == 0) {
        gen_expr(n->args);
        fprintf(out, "    mov rax, [rax]\n");
        return;
      }
      if (strcmp(n->name, "store64") == 0) {
        gen_expr(n->args);        /* addr */
        fprintf(out, "    push rax\n");
        gen_expr(n->args->next);  /* value */
        fprintf(out, "    pop rdi\n");
        fprintf(out, "    mov [rdi], rax\n");
        return;                   /* rax still holds the value */
      }
      if (strcmp(n->name, "argc") == 0) {
        /* .Largv0 holds the process's initial rsp, saved by _start before
         * anything is pushed: [rsp] = argc at that point. */
        fprintf(out, "    mov rax, [.Largv0]\n");
        fprintf(out, "    mov rax, [rax]\n");
        return;
      }
      if (strcmp(n->name, "argv") == 0) {
        gen_expr(n->args); /* -> rax: index i */
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
        gen_expr(n->args); /* -> rax: port */
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
        gen_expr(n->args); /* -> rax: port */
        fprintf(out, "    mov rdx, rax\n");
        fprintf(out, "    in ax, dx\n");
        fprintf(out, "    movzx rax, ax\n");
        return;
      }

      /* Thin syscall wrappers and real function calls share the same argument
       * plumbing: evaluate left-to-right onto the stack. Builtins always have
       * arity <= MAX_PARAMS, so they only ever hit the pop-based path below.
       * User calls with more than MAX_PARAMS args read the extra ones back
       * non-destructively (they must stay on the stack -- the callee reads
       * them the same way a caller-past-us would via [rbp+16+...]) and the
       * whole pushed block is reclaimed with one `add rsp` after the call. */
      int i = 0;
      for (Node *a = n->args; a; a = a->next, i++) {
        gen_expr(a);
        fprintf(out, "    push rax\n");
      }
      if (n->nargs <= MAX_PARAMS) {
        for (i = n->nargs - 1; i >= 0; i--) {
          fprintf(out, "    pop %s\n", ARG_REGS[i]);
        }
      } else {
        for (i = 0; i < MAX_PARAMS; i++) {
          fprintf(out, "    mov %s, [rsp + %d]\n", ARG_REGS[i], (n->nargs - 1 - i) * 8);
        }
      }
      if (strcmp(n->name, "write") == 0) {
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
        else fprintf(out, "    mov r10, [rbp - %ld]\n", n->ival);
        fprintf(out, "    call r10\n");
      } else {
        fprintf(out, "    call %s\n", n->name);
      }
      if (n->nargs > MAX_PARAMS) fprintf(out, "    add rsp, %d\n", n->nargs * 8);
      return;
    }
    case ND_AND: {
      int l = label_count++;
      gen_expr(n->lhs);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    je .Lfalse%d\n", l);
      gen_expr(n->rhs);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    je .Lfalse%d\n", l);
      fprintf(out, "    mov rax, 1\n");
      fprintf(out, "    jmp .Landend%d\n", l);
      fprintf(out, ".Lfalse%d:\n", l);
      fprintf(out, "    mov rax, 0\n");
      fprintf(out, ".Landend%d:\n", l);
      return;
    }
    case ND_OR: {
      int l = label_count++;
      gen_expr(n->lhs);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    jne .Ltrue%d\n", l);
      gen_expr(n->rhs);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    jne .Ltrue%d\n", l);
      fprintf(out, "    mov rax, 0\n");
      fprintf(out, "    jmp .Lorend%d\n", l);
      fprintf(out, ".Ltrue%d:\n", l);
      fprintf(out, "    mov rax, 1\n");
      fprintf(out, ".Lorend%d:\n", l);
      return;
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
      gen_expr(n->lhs);
      fprintf(out, "    push rax\n");
      gen_expr(n->rhs);
      fprintf(out, "    mov rcx, rax\n");
      fprintf(out, "    pop rax\n");
      switch (n->kind) {
        case ND_ADD: fprintf(out, "    add rax, rcx\n"); break;
        case ND_SUB: fprintf(out, "    sub rax, rcx\n"); break;
        case ND_MUL: fprintf(out, "    imul rax, rcx\n"); break;
        case ND_DIV: fprintf(out, "    cqo\n    idiv rcx\n"); break;
        case ND_EQ:  fprintf(out, "    cmp rax, rcx\n    sete al\n    movzx rax, al\n"); break;
        case ND_NE:  fprintf(out, "    cmp rax, rcx\n    setne al\n    movzx rax, al\n"); break;
        case ND_LT:  fprintf(out, "    cmp rax, rcx\n    setl al\n    movzx rax, al\n"); break;
        case ND_LE:  fprintf(out, "    cmp rax, rcx\n    setle al\n    movzx rax, al\n"); break;
        case ND_GT:  fprintf(out, "    cmp rax, rcx\n    setg al\n    movzx rax, al\n"); break;
        case ND_GE:  fprintf(out, "    cmp rax, rcx\n    setge al\n    movzx rax, al\n"); break;
        default: break;
      }
      return;
    default:
      error("internal error: not an expression node");
  }
}

static void gen_stmt(Node *n);

static void gen_block(Node *n) {
  for (Node *s = n->body; s; s = s->next) gen_stmt(s);
}

static void gen_stmt(Node *n) {
  switch (n->kind) {
    case ND_DECL:
      if (n->rhs) gen_expr(n->rhs);
      else fprintf(out, "    mov rax, 0\n");
      fprintf(out, "    mov [rbp - %ld], rax\n", n->ival);
      return;
    case ND_ASSIGN:
      gen_expr(n->rhs);
      if (n->is_global) fprintf(out, "    mov [.G%s], rax\n", n->name);
      else fprintf(out, "    mov [rbp - %ld], rax\n", n->ival);
      return;
    case ND_INDEX_ASSIGN:
      gen_expr(n->lhs);                       /* base */
      fprintf(out, "    push rax\n");
      gen_expr(n->rhs);                       /* index */
      fprintf(out, "    pop rcx\n");
      fprintf(out, "    add rcx, rax\n");      /* rcx = base + index (address) */
      fprintf(out, "    push rcx\n");
      gen_expr(n->cond);                      /* value */
      fprintf(out, "    pop rdi\n");
      fprintf(out, "    mov byte ptr [rdi], al\n");
      return;
    case ND_FIELD_ASSIGN:
      gen_expr(n->lhs);                  /* base */
      fprintf(out, "    push rax\n");
      gen_expr(n->rhs);                  /* value */
      fprintf(out, "    pop rdi\n");
      fprintf(out, "    mov [rdi + %ld], rax\n", n->ival);
      return;
    case ND_EXPR_STMT:
      gen_expr(n->lhs);
      return;
    case ND_IF: {
      int l = label_count++;
      gen_expr(n->cond);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    je .Lelse%d\n", l);
      gen_stmt(n->then);
      fprintf(out, "    jmp .Lend%d\n", l);
      fprintf(out, ".Lelse%d:\n", l);
      if (n->els) gen_stmt(n->els);
      fprintf(out, ".Lend%d:\n", l);
      return;
    }
    case ND_WHILE: {
      int l = label_count++;
      fprintf(out, ".Lstart%d:\n", l);
      gen_expr(n->cond);
      fprintf(out, "    cmp rax, 0\n");
      fprintf(out, "    je .Lend%d\n", l);
      if (gen_loop_depth >= MAX_LOOP_DEPTH) error("loops nested too deeply (max %d)", MAX_LOOP_DEPTH);
      loop_labels[gen_loop_depth++] = l;
      gen_stmt(n->body);
      gen_loop_depth--;
      fprintf(out, "    jmp .Lstart%d\n", l);
      fprintf(out, ".Lend%d:\n", l);
      return;
    }
    case ND_RETURN:
      if (n->lhs) gen_expr(n->lhs);
      fprintf(out, "    jmp .Lret%d\n", current_ret_label);
      return;
    case ND_BREAK:
      fprintf(out, "    jmp .Lend%d\n", loop_labels[gen_loop_depth - 1]);
      return;
    case ND_CONTINUE:
      fprintf(out, "    jmp .Lstart%d\n", loop_labels[gen_loop_depth - 1]);
      return;
    case ND_BLOCK:
      gen_block(n);
      return;
    default:
      error("internal error: not a statement node");
  }
}

static void gen_function(Function *f) {
  long frame = f->stack_size;
  if (frame % 16 != 0) frame += 16 - (frame % 16);

  current_ret_label = label_count++;
  f->ret_label = current_ret_label;

  fprintf(out, "%s:\n", f->name);
  fprintf(out, "    push rbp\n");
  fprintf(out, "    mov rbp, rsp\n");
  if (frame > 0) fprintf(out, "    sub rsp, %ld\n", frame);

  for (int i = 0; i < f->nparams; i++) {
    if (i < MAX_PARAMS) {
      fprintf(out, "    mov [rbp - %ld], %s\n", (long)(i + 1) * 8, ARG_REGS[i]);
    } else {
      /* Stack-passed: caller left it at [rbp+16+8*(nparams-1-i)], mirroring
       * how its own stack looked at the moment of `call` (see gen_call). */
      long stack_off = 16 + (long)(f->nparams - 1 - i) * 8;
      fprintf(out, "    mov rax, [rbp + %ld]\n", stack_off);
      fprintf(out, "    mov [rbp - %ld], rax\n", (long)(i + 1) * 8);
    }
  }

  gen_block(f->body);

  fprintf(out, ".Lret%d:\n", current_ret_label);
  fprintf(out, "    mov rsp, rbp\n");
  fprintf(out, "    pop rbp\n");
  fprintf(out, "    ret\n");
}

/* ------------------------------------------------------------ token dump */
/* `coff0 -t file.c0` prints the token stream, one token per line. This is
 * the reference output the c0-written lexer (c0/lex.c0) is diffed against,
 * so the format is a contract: keep the two in sync. */

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
      case TK_ASSIGN: printf("ASSIGN\n"); break;
      case TK_EQ:     printf("EQ\n"); break;
      case TK_NE:     printf("NE\n"); break;
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
    }
  }
}

/* -------------------------------------------------------------- ast dump */
/* `coff0 -a file.c0` prints the parse tree, one node per line, two-space
 * indent per depth, pre-resolve (names, not offsets). Like -t, this format
 * is the contract the c0-written parser will be diffed against. */

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
      hit: -a dumps pre-resolve, and FUNCREF only exists post-resolve (see
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
    case ND_EQ:  printf("EQ\n");  goto binary;
    case ND_NE:  printf("NE\n");  goto binary;
    case ND_LT:  printf("LT\n");  goto binary;
    case ND_LE:  printf("LE\n");  goto binary;
    case ND_GT:  printf("GT\n");  goto binary;
    case ND_GE:  printf("GE\n");  goto binary;
    case ND_AND: printf("AND\n"); goto binary;
    case ND_OR:  printf("OR\n");  goto binary;
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
    case ND_RETURN:
      printf("RETURN\n");
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
  for (Global *g = prog_globals; g; g = g->next)
    printf("GLOBAL %s %ld\n", g->name, g->init);
  for (Layout *l = prog_layouts; l; l = l->next) {
    printf("LAYOUT %s\n", l->name);
    for (LayoutField *f = l->fields; f; f = f->next)
      printf("  FIELDDECL %s\n", f->name);
  }
  for (Function *f = prog_functions; f; f = f->next) {
    if (f->is_void) printf("FUNC void %s", f->name);
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
  char *buf = malloc(size + 1);
  fread(buf, 1, size, f);
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
 * buffer that gets handed to tokenize() exactly as a single file always
 * has. Deliberately NOT true separate compilation (no linking, no extern
 * declarations, still one flat symbol namespace): that would need solving
 * "how does one translation unit call into another by name," a wall this
 * project has already hit and worked around with indirect calls several
 * times, because nothing has needed it yet. The actual problem (my
 * kernel's main source file grew too large to navigate as one file) is a
 * file-organization problem, not a compilation-model one -- this solves
 * exactly that, nothing more.
 *
 * Include paths are resolved relative to the file containing the
 * directive (not the current working directory), so nested includes work
 * regardless of where coff is invoked from -- the same convention C's
 * #include "..." form uses.
 *
 * Error messages have no line-number info, matching every other error in
 * this file (PARSEERROR/CODEGENERROR also carry none) -- not a regression
 * introduced here.
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

/* Currently-open include chain, for cycle detection -- a real doubly-
 * nested or diamond-shaped include graph is not the expected shape (the
 * driving use case is one entry file including several leaf subsystem
 * files, none of which include each other), but a cycle would otherwise
 * recurse until the stack overflows with no clear error, so it is cheap
 * insurance to check for directly rather than leave as a footgun. */
#define INCLUDE_MAX_DEPTH 32
static const char *include_stack[INCLUDE_MAX_DEPTH];
static int include_depth = 0;

static void expand_file(Growbuf *out, const char *path) {
  for (int i = 0; i < include_depth; i++) {
    if (strcmp(include_stack[i], path) == 0) {
      fprintf(stderr, "include: circular include involving %s\n", path);
      exit(1);
    }
  }
  if (include_depth >= INCLUDE_MAX_DEPTH) {
    fprintf(stderr, "include: nested too deeply (>%d) at %s\n", INCLUDE_MAX_DEPTH, path);
    exit(1);
  }
  include_stack[include_depth++] = path;

  char *src = read_file(path);
  char *dir = dirname_with_slash(path);
  char *p = src;

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
     * need the same verbatim-copy treatment as strings -- found by a real
     * false positive: this very scanner's own source contains `!= '"'`
     * (a char literal for the double-quote character). Without this case,
     * the scanner has no notion of "inside a char literal" at all, so it
     * sees that bare `"` and wrongly enters STRING mode there instead,
     * scanning forward to whatever `"` happens to appear next in the file
     * -- silently swallowing a large, arbitrary span of real code as if
     * it were string content, then corrupting everything scanned after
     * that point. */
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
        char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"') { fprintf(stderr, "include: expected a string after 'include' in %s\n", path); exit(1); }
        q++;
        char *pstart = q;
        while (*q && *q != '"') q++;
        if (*q != '"') { fprintf(stderr, "include: unterminated path string in %s\n", path); exit(1); }
        char *rel = malloc(q - pstart + 1);
        memcpy(rel, pstart, q - pstart);
        rel[q - pstart] = 0;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ';') { fprintf(stderr, "include: expected ';' after include path in %s\n", path); exit(1); }
        q++;
        char *inc_path = join_path(dir, rel);
        expand_file(out, inc_path);
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
  if (argc != 3) {
    fprintf(stderr, "usage: coff0 <input.c0> <output.s>  |  coff0 -t <input.c0>\n");
    return 1;
  }
  src_name = argv[1];
  char *src = resolve_includes(argv[1]);

  tokens = tokenize(src);
  pos = 0;
  parse_program();

  int found_main = 0;
  for (Function *f = prog_functions; f; f = f->next) {
    if (find_builtin(f->name)) {
      error("'%s' is a builtin and cannot be redefined", f->name);
    }
    func_declare_sig(f->name, f->nparams);
    if (strcmp(f->name, "main") == 0) {
      if (f->nparams != 0) error("'main' must take no parameters");
      if (f->is_void) error("'main' must return int, not void");
      found_main = 1;
    }
  }
  if (!found_main) error("no 'main' function defined");

  for (Global *g = prog_globals; g; g = g->next) {
    if (find_builtin(g->name)) error("'%s' is a builtin and cannot be redefined", g->name);
    for (Function *f = prog_functions; f; f = f->next) {
      if (strcmp(f->name, g->name) == 0) error("'%s' is both a function and a global", g->name);
    }
    glob_declare(g->name);
  }

  for (Global *e = prog_externs; e; e = e->next) {
    if (find_builtin(e->name)) error("'%s' is a builtin and cannot be used as extern", e->name);
    for (Global *g = prog_globals; g; g = g->next) {
      if (strcmp(g->name, e->name) == 0) error("'%s' is both a global and an extern", e->name);
    }
    extern_declare(e->name);
  }

  for (Layout *l = prog_layouts; l; l = l->next) layout_declare(l);

  for (Function *f = prog_functions; f; f = f->next) resolve_function(f);

  out = fopen(argv[2], "w");
  if (!out) { perror(argv[2]); return 1; }

  fprintf(out, ".intel_syntax noprefix\n");
  fprintf(out, ".global _start\n");
  fprintf(out, "_start:\n");
  fprintf(out, "    mov [.Largv0], rsp\n");
  fprintf(out, "    call main\n");
  fprintf(out, "    mov rdi, rax\n");
  fprintf(out, "    mov rax, 60\n");
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

  /* alloc()'s cached program break, argc/argv's saved initial rsp, then one
   * .quad slot per global. */
  fprintf(out, "\n.section .data\n");
  fprintf(out, ".Lcurbrk:\n    .quad 0\n");
  fprintf(out, ".Largv0:\n    .quad 0\n");
  for (Global *g = prog_globals; g; g = g->next) {
    fprintf(out, ".G%s:\n    .quad %ld\n", g->name, g->init);
  }

  fclose(out);
  return 0;
}
