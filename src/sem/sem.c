#include "sem_impl.h"

#include <stdarg.h>

/** Whole-program model filled by SemAnalyze */
static SemProg semprog;

/** Main source file for diagnostics */
const char *sem_source_file;

/** Recovery buffer for one statement (mirrors old try_gen_statement) */
jmp_buf sem_jmp;

/** Non-zero while checking statements (errors abandon the statement) */
int sem_in_stmt;

/** Active scope while checking bodies */
SemScope sem_scope;

/** Recorded expression types for the IR builder */
typedef struct {
  Node *node;
  SemType type;
  const char *kind;
} TypeRec;

static TypeRec *trecs;
static int ntrecs;
static int captrecs;

/** Recorded declaration slots for the IR builder */
typedef struct {
  Node *node;
  int slot;
} SlotRec;

static SlotRec *srecs;
static int nsrecs;
static int capsrecs;

SemProg *sem_prog(void) { return &semprog; }

const char *sem_type_name(SemType type) {
  switch (type) {
  case ST_BOOL:
    return "bool";
  case ST_FLOAT:
    return "num";
  case ST_STR:
    return "str";
  case ST_ARR:
    return "arr";
  case ST_STRUCT:
    return "struct";
  case ST_CLASS:
    return "class";
  default:
    return "num";
  }
}

int sem_is_numeric(SemType type) {
  return type == ST_INT || type == ST_FLOAT || type == ST_BOOL;
}

int sem_compatible(SemType declared, SemType given) {
  if (given == ST_NULL) {
    return declared == ST_STR || declared == ST_STRUCT ||
           declared == ST_CLASS || declared == ST_ARR;
  }
  if (declared == ST_NULL) {
    return 0;
  }
  if (declared == ST_STR || given == ST_STR) {
    return declared == ST_STR && given == ST_STR;
  }
  if (declared == ST_ARR || given == ST_ARR) {
    return declared == ST_ARR && given == ST_ARR;
  }
  return sem_is_numeric(declared) && sem_is_numeric(given);
}

char *sem_dup(const char *s) {
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

/**
 * @brief Extracts the diagnostic position of a node
 * @param node Fault node, may be NULL
 * @param line Receives 1-based line
 * @param col Receives 1-based column
 * @param width Receives squiggle width
 * @param source Receives owning file
 */
static void sem_at(Node *node, int *line, int *col, int *width,
                   const char **source) {
  *line = 1;
  *col = 1;
  *width = 1;
  *source = sem_source_file;
  if (node != NULL) {
    if (node->line > 0) {
      *line = node->line;
    }
    if (node->col > 0) {
      *col = node->col;
    }
    if (node->width > 0) {
      *width = node->width;
    }
    if (node->source != NULL) {
      *source = node->source;
    }
  }
}

void sem_error(Node *node, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line, col, width;
  const char *source;
  sem_at(node, &line, &col, &width, &source);
  term_report(TERM_ERROR, source, line, col, width, message);
  if (sem_in_stmt) {
    longjmp(sem_jmp, 1);
  }
}

void sem_warn(Node *node, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line, col, width;
  const char *source;
  sem_at(node, &line, &col, &width, &source);
  term_report(TERM_WARNING, source, line, col, width, message);
}

/**
 * @brief Finds a plain function by name
 * @param name Function name
 * @return Function index, or -1
 */
static int find_func(const char *name) {
  for (int i = 0; i < semprog.nfuncs; i++) {
    if (strcmp(semprog.funcs[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_func_at(const char *name) { return find_func(name); }

int sem_find_global(const char *name) {
  for (int i = 0; i < semprog.nglobals; i++) {
    if (strcmp(semprog.globals[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_find_struct(const char *name) {
  for (int i = 0; i < semprog.nstructs; i++) {
    if (strcmp(semprog.structs[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_find_class(const char *name) {
  for (int i = 0; i < semprog.nclasses; i++) {
    if (strcmp(semprog.classes[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_find_method(int class_idx, const char *method, int *owner_idx) {
  if (owner_idx != NULL) {
    *owner_idx = class_idx;
  }
  for (int c = class_idx; c >= 0; c = semprog.classes[c].base_idx) {
    for (int i = 0; i < semprog.classes[c].nmethods; i++) {
      if (strcmp(semprog.classes[c].methods[i].name, method) == 0) {
        if (owner_idx != NULL) {
          *owner_idx = c;
        }
        return i;
      }
    }
  }
  return -1;
}

int sem_is_subclass(const char *kind, const char *base) {
  if (kind == NULL || base == NULL) {
    return 0;
  }
  if (strcmp(kind, base) == 0) {
    return 1;
  }
  for (int c = sem_find_class(kind); c >= 0;
       c = semprog.classes[c].base_idx) {
    if (strcmp(semprog.classes[c].name, base) == 0) {
      return 1;
    }
  }
  return 0;
}

int sem_find_struct_field(int struct_idx, const char *field) {
  if (struct_idx < 0) {
    return -1;
  }
  for (int i = 0; i < semprog.structs[struct_idx].nfields; i++) {
    if (strcmp(semprog.structs[struct_idx].fields[i].name, field) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_find_class_field(int class_idx, const char *field) {
  if (class_idx < 0) {
    return -1;
  }
  for (int i = 0; i < semprog.classes[class_idx].nfields; i++) {
    if (strcmp(semprog.classes[class_idx].fields[i].name, field) == 0) {
      return i;
    }
  }
  return -1;
}

int sem_find_instance_field(const char *kind, const char *field,
                            SemField **out_def) {
  if (out_def != NULL) {
    *out_def = NULL;
  }
  int s = sem_find_struct(kind);
  if (s >= 0) {
    int f = sem_find_struct_field(s, field);
    if (f >= 0 && out_def != NULL) {
      *out_def = &semprog.structs[s].fields[f];
    }
    return f;
  }
  int c = sem_find_class(kind);
  if (c >= 0) {
    int f = sem_find_class_field(c, field);
    if (f >= 0 && out_def != NULL) {
      *out_def = &semprog.classes[c].fields[f];
    }
    return f;
  }
  return -1;
}

int sem_method_slot_assign(const char *name) {
  for (int i = 0; i < semprog.nmeth_slots; i++) {
    if (strcmp(semprog.meth_slots[i], name) == 0) {
      return i;
    }
  }
  if (semprog.nmeth_slots >= SEM_MAX_METH_SLOTS) {
    sem_error(NULL, "Too many distinct method names");
    return 0;
  }
  semprog.meth_slots[semprog.nmeth_slots] = sem_dup(name);
  return semprog.nmeth_slots++;
}

int sem_method_slot(const char *name) {
  for (int i = 0; i < semprog.nmeth_slots; i++) {
    if (strcmp(semprog.meth_slots[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

SemType sem_resolve_decl(const char *keyword, Node *node,
                         const char **out_kind) {
  if (out_kind != NULL) {
    *out_kind = NULL;
  }
  if (strcmp(keyword, "bool") == 0) {
    return ST_BOOL;
  }
  if (strcmp(keyword, "str") == 0) {
    return ST_STR;
  }
  if (strcmp(keyword, "arr") == 0) {
    return ST_ARR;
  }
  if (strcmp(keyword, "num") == 0) {
    return ST_INT;
  }
  if (strcmp(keyword, "char") == 0) {
    return ST_INT;
  }
  int s = sem_find_struct(keyword);
  if (s >= 0) {
    if (out_kind != NULL) {
      *out_kind = semprog.structs[s].name;
    }
    return ST_STRUCT;
  }
  int c = sem_find_class(keyword);
  if (c >= 0) {
    if (out_kind != NULL) {
      *out_kind = semprog.classes[c].name;
    }
    return ST_CLASS;
  }
  sem_error(node, "Unknown type '%s'", keyword);
  return ST_ERR;
}

void sem_resolve_field(SemField *field, const char *declared, Node *fault) {
  if (strcmp(declared, "bool") == 0) {
    field->type = ST_BOOL;
    field->kind = NULL;
    return;
  }
  if (strcmp(declared, "str") == 0) {
    field->type = ST_STR;
    field->kind = NULL;
    return;
  }
  if (strcmp(declared, "num") == 0 || strcmp(declared, "char") == 0) {
    field->type = ST_FLOAT;
    field->kind = NULL;
    return;
  }
  int s = sem_find_struct(declared);
  if (s >= 0) {
    field->type = ST_STRUCT;
    field->kind = semprog.structs[s].name;
    return;
  }
  int c = sem_find_class(declared);
  if (c >= 0) {
    field->type = ST_CLASS;
    field->kind = semprog.classes[c].name;
    return;
  }
  sem_error(fault, "Unknown field type '%s'", declared);
  field->type = ST_ERR;
  field->kind = NULL;
}

int sem_count_params(Node *params) {
  int n = 0;
  for (Node *p = params; p != NULL; p = p->right) {
    n++;
  }
  return n;
}

void sem_flatten_class(int c) {
  SemClass *d = &semprog.classes[c];
  if (d->flattened) {
    return;
  }
  if (d->base_idx < 0) {
    d->flattened = 1;
    return;
  }
  if (d->visiting) {
    sem_error(d->decl, "Inheritance cycle through class '%s'", d->name);
    d->base_idx = -1;
    d->flattened = 1;
    return;
  }
  d->visiting = 1;
  sem_flatten_class(d->base_idx);
  d->visiting = 0;
  if (d->flattened) {
    return;
  }
  SemClass *b = &semprog.classes[d->base_idx];
  for (int m = 0; m < d->nmethods; m++) {
    int owner = -1;
    int bm = sem_find_method(d->base_idx, d->methods[m].name, &owner);
    if (bm >= 0 && sem_count_params(d->methods[m].params) !=
                        sem_count_params(
                            semprog.classes[owner].methods[bm].params)) {
      sem_warn(d->methods[m].decl,
               "Method '%s' overrides with different parameters",
               d->methods[m].name);
    }
  }
  for (int f = 0; f < d->nfields; f++) {
    for (int bf = 0; bf < b->nfields; bf++) {
      if (strcmp(d->fields[f].name, b->fields[bf].name) == 0) {
        sem_error(d->decl, "Field '%s' in class '%s' hides a base class field",
                  d->fields[f].name, d->name);
      }
    }
  }
  if (d->nfields + b->nfields > SEM_MAX_FIELDS) {
    sem_error(d->decl, "Too many fields in class '%s' with base '%s'", d->name,
              b->name);
    d->flattened = 1;
    return;
  }
  memmove(&d->fields[b->nfields], &d->fields[0],
          (size_t)d->nfields * sizeof(SemField));
  memcpy(&d->fields[0], b->fields, (size_t)b->nfields * sizeof(SemField));
  d->nfields += b->nfields;
  d->flattened = 1;
}

void sem_record_type(Node *node, SemType type, const char *kind) {
  if (node == NULL) {
    return;
  }
  for (int i = 0; i < ntrecs; i++) {
    if (trecs[i].node == node) {
      return;
    }
  }
  if (ntrecs >= captrecs) {
    captrecs = captrecs != 0 ? captrecs * 2 : 256;
    trecs = realloc(trecs, (size_t)captrecs * sizeof(TypeRec));
  }
  trecs[ntrecs].node = node;
  trecs[ntrecs].type = type;
  trecs[ntrecs].kind = kind;
  ntrecs++;
}

SemType sem_expr_type(Node *node) {
  for (int i = 0; i < ntrecs; i++) {
    if (trecs[i].node == node) {
      return trecs[i].type;
    }
  }
  return ST_ERR;
}

const char *sem_expr_kind(Node *node) {
  for (int i = 0; i < ntrecs; i++) {
    if (trecs[i].node == node) {
      return trecs[i].kind;
    }
  }
  return NULL;
}

void sem_record_slot(Node *decl, int slot) {
  if (nsrecs >= capsrecs) {
    capsrecs = capsrecs != 0 ? capsrecs * 2 : 256;
    srecs = realloc(srecs, (size_t)capsrecs * sizeof(SlotRec));
  }
  srecs[nsrecs].node = decl;
  srecs[nsrecs].slot = slot;
  nsrecs++;
}

int sem_decl_slot(Node *decl) {
  for (int i = 0; i < nsrecs; i++) {
    if (srecs[i].node == decl) {
      return srecs[i].slot;
    }
  }
  return -1;
}

int sem_scope_find(const char *name) {
  for (int i = sem_scope.nbind - 1; i >= 0; i--) {
    if (strcmp(sem_scope.bind_name[i], name) == 0) {
      return sem_scope.bind_slot[i];
    }
  }
  return -1;
}

int sem_scope_declare(Node *node, const char *name, SemType type,
                      int is_param) {
  if (sem_scope.nbind >= SEM_MAX_VARS) {
    sem_error(node, "Too many variables in function");
    return -1;
  }
  if (sem_scope.nvars >= sem_scope.capvars) {
    sem_scope.capvars = sem_scope.capvars != 0 ? sem_scope.capvars * 2 : 64;
    sem_scope.vars =
        realloc(sem_scope.vars, (size_t)sem_scope.capvars * sizeof(SemVar));
  }
  SemVar *v = &sem_scope.vars[sem_scope.nvars];
  v->name = sem_dup(name);
  v->type = type;
  v->kind = NULL;
  v->is_param = is_param;
  v->line = node != NULL ? node->line : 1;
  v->col = node != NULL ? node->col : 1;
  v->used = 0;
  v->slot = sem_scope.nvars;
  v->elem = ST_INT;
  v->elem_kind = NULL;
  sem_scope.bind_name[sem_scope.nbind] = v->name;
  sem_scope.bind_slot[sem_scope.nbind] = v->slot;
  sem_scope.nbind++;
  sem_scope.nvars++;
  return v->slot;
}

int sem_scope_require(Node *node, const char *name) {
  int slot = sem_scope_find(name);
  if (slot < 0) {
    int g = sem_find_global(name);
    if (g >= 0) {
      return -(g + 2);
    }
    sem_error(node, "Variable '%s' not declared", name);
    return -1;
  }
  if (sem_scope.vars[slot].is_param && !sem_scope.no_param_warn &&
      sem_scope.vars[slot].type != ST_ARR) {
    sem_warn(node, "Parameter '%s' should be accessed as self.%s", name, name);
  }
  sem_scope.vars[slot].used = 1;
  return slot;
}

int sem_ref_is_global(int ref) { return ref < -1; }

int sem_ref_to_global(int ref) { return (-ref) - 2; }

int sem_scope_require_param(Node *node, const char *member) {
  int slot = sem_scope_find(member);
  if (slot < 0 || !sem_scope.vars[slot].is_param) {
    sem_error(node, "'%s' is not a parameter", member);
    return -1;
  }
  sem_scope.vars[slot].used = 1;
  return slot;
}

void sem_check_unused(int from) {
  char message[96];
  for (int i = from; i < sem_scope.nvars; i++) {
    if (!sem_scope.vars[i].is_param && !sem_scope.vars[i].used) {
      snprintf(message, sizeof(message), "Variable '%s' is never used",
               sem_scope.vars[i].name);
      term_report(TERM_WARNING, sem_source_file, sem_scope.vars[i].line,
                  sem_scope.vars[i].col, 1, message);
    }
  }
}

/**
 * @brief Registers function signatures in source order
 * @param root Top level statements
 */
static void collect_funcs(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && semprog.nfuncs < SEM_MAX_FUNCS) {
      SemFunc *f = &semprog.funcs[semprog.nfuncs];
      f->name = sem_dup(s->function.name);
      f->is_public = s->function.is_public;
      f->ret = s->function.return_type != NULL
                   ? sem_dup(s->function.return_type)
                   : NULL;
      f->params = s->function.params;
      f->body = s->function.body;
      f->decl = s;
      f->nparams = 0;
      f->ptypes = NULL;
      f->pkinds = NULL;
      f->pnames = NULL;
      f->line = s->line;
      f->col = s->col;
      f->source = s->source;
      f->ret_float = 0;
      f->bool_only = 0;
      memset(f->pfloat, 0, sizeof(f->pfloat));
      f->nvars = 0;
      f->vars = NULL;
      semprog.nfuncs++;
    }
  }
}

/**
 * @brief Registers struct and class definitions with duplicate checks
 * @param root Top level statements
 */
static void collect_types(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_STRUCT_DEF) {
      for (int i = 0; i < semprog.nfuncs; i++) {
        if (strcmp(semprog.funcs[i].name, s->struct_def.name) == 0) {
          sem_error(s, "Struct '%s' is already defined", s->struct_def.name);
        }
      }
      for (int i = 0; i < semprog.nstructs; i++) {
        if (strcmp(semprog.structs[i].name, s->struct_def.name) == 0) {
          sem_error(s, "Struct '%s' is already defined", s->struct_def.name);
        }
      }
      for (int i = 0; i < semprog.nclasses; i++) {
        if (strcmp(semprog.classes[i].name, s->struct_def.name) == 0) {
          sem_error(s, "Struct '%s' is already defined", s->struct_def.name);
        }
      }
      if (semprog.nstructs >= SEM_MAX_STRUCTS) {
        sem_error(s, "Too many structs defined");
        continue;
      }
      SemStruct *d = &semprog.structs[semprog.nstructs];
      d->name = sem_dup(s->struct_def.name);
      d->is_public = s->struct_def.is_public;
      d->nfields = 0;
      for (Node *f = s->struct_def.fields; f != NULL; f = f->right) {
        if (d->nfields >= SEM_MAX_FIELDS) {
          sem_error(f, "Too many fields in struct '%s'", d->name);
          continue;
        }
        d->fields[d->nfields].name = sem_dup(f->var_decl.name);
        d->fields[d->nfields].type = ST_ERR;
        d->fields[d->nfields].kind = NULL;
        d->nfields++;
      }
      semprog.nstructs++;
    } else if (s->type == NODE_CLASS_DEF) {
      for (int i = 0; i < semprog.nfuncs; i++) {
        if (strcmp(semprog.funcs[i].name, s->class_def.name) == 0) {
          sem_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      for (int i = 0; i < semprog.nstructs; i++) {
        if (strcmp(semprog.structs[i].name, s->class_def.name) == 0) {
          sem_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      for (int i = 0; i < semprog.nclasses; i++) {
        if (strcmp(semprog.classes[i].name, s->class_def.name) == 0) {
          sem_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      if (semprog.nclasses >= SEM_MAX_CLASSES) {
        sem_error(s, "Too many classes defined");
        continue;
      }
      SemClass *d = &semprog.classes[semprog.nclasses];
      d->name = sem_dup(s->class_def.name);
      d->is_public = s->class_def.is_public;
      d->base = s->class_def.base != NULL ? sem_dup(s->class_def.base) : NULL;
      d->base_idx = -1;
      d->flattened = 0;
      d->visiting = 0;
      d->class_id = semprog.nclasses;
      d->nfields = 0;
      for (Node *f = s->class_def.fields; f != NULL; f = f->right) {
        if (d->nfields >= SEM_MAX_FIELDS) {
          sem_error(f, "Too many fields in class '%s'", d->name);
          continue;
        }
        d->fields[d->nfields].name = sem_dup(f->var_decl.name);
        d->fields[d->nfields].type = ST_ERR;
        d->fields[d->nfields].kind = NULL;
        d->nfields++;
      }
      d->nmethods = 0;
      for (Node *m = s->class_def.methods; m != NULL; m = m->right) {
        if (d->nmethods >= SEM_MAX_METHODS) {
          sem_error(m, "Too many methods in class '%s'", d->name);
          continue;
        }
        if (strcmp(m->method_def.ret_type, "num") != 0 &&
            strcmp(m->method_def.ret_type, "bool") != 0 &&
            strcmp(m->method_def.ret_type, "str") != 0) {
          sem_error(m, "Method '%s' must return num, bool or str",
                    m->method_def.name);
          continue;
        }
        SemFunc *md = &d->methods[d->nmethods];
        md->name = sem_dup(m->method_def.name);
        md->is_public = 0;
        md->ret = sem_dup(m->method_def.ret_type);
        md->params = m->method_def.params;
        md->body = m->method_def.body;
        md->decl = m;
        md->nparams = 0;
        md->ptypes = NULL;
        md->pkinds = NULL;
        md->pnames = NULL;
        md->line = m->line;
        md->col = m->col;
        md->source = m->source;
        md->ret_float = 0;
        md->bool_only = 0;
        memset(md->pfloat, 0, sizeof(md->pfloat));
        md->nvars = 0;
        md->vars = NULL;
        d->nmethods++;
      }
      d->decl = s;
      semprog.nclasses++;
    }
  }
}

/**
 * @brief Resolves field types once every struct/class name is known
 * @param root Top level statements
 */
static void resolve_fields(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_STRUCT_DEF) {
      int si = sem_find_struct(s->struct_def.name);
      if (si < 0) {
        continue;
      }
      int fi = 0;
      for (Node *f = s->struct_def.fields;
           f != NULL && fi < semprog.structs[si].nfields; f = f->right, fi++) {
        sem_resolve_field(&semprog.structs[si].fields[fi], f->var_decl.var_type,
                          f);
      }
    } else if (s->type == NODE_CLASS_DEF) {
      int ci = sem_find_class(s->class_def.name);
      if (ci < 0) {
        continue;
      }
      int fi = 0;
      for (Node *f = s->class_def.fields;
           f != NULL && fi < semprog.classes[ci].nfields; f = f->right, fi++) {
        sem_resolve_field(&semprog.classes[ci].fields[fi], f->var_decl.var_type,
                          f);
      }
    }
  }
}

/**
 * @brief Resolves inheritance, flattens fields, assigns vtable slots
 */
static void resolve_inheritance(void) {
  for (int c = 0; c < semprog.nclasses; c++) {
    if (semprog.classes[c].base != NULL) {
      semprog.classes[c].base_idx = sem_find_class(semprog.classes[c].base);
      if (semprog.classes[c].base_idx < 0) {
        sem_error(semprog.classes[c].decl, "Unknown base class '%s'",
                  semprog.classes[c].base);
        semprog.classes[c].base_idx = -1;
      } else if (semprog.classes[c].base_idx == c) {
        sem_error(semprog.classes[c].decl, "Class '%s' cannot inherit itself",
                  semprog.classes[c].name);
        semprog.classes[c].base_idx = -1;
      }
    }
  }
  for (int c = 0; c < semprog.nclasses; c++) {
    sem_flatten_class(c);
  }
  for (int c = 0; c < semprog.nclasses; c++) {
    for (int m = 0; m < semprog.classes[c].nmethods; m++) {
      sem_method_slot_assign(semprog.classes[c].methods[m].name);
    }
  }
}

/**
 * @brief Registers globals in source order
 * @param root Top level statements
 */
static void collect_globals(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_VAR_DECL && s->var_decl.is_global) {
      const char *kind = NULL;
      SemType dt = sem_resolve_decl(s->var_decl.var_type, s, &kind);
      if (sem_find_global(s->var_decl.name) >= 0) {
        sem_error(s, "Duplicate global variable '%s'", s->var_decl.name);
        continue;
      }
      if (semprog.nglobals >= SEM_MAX_GLOBALS) {
        continue;
      }
      SemGlobal *g = &semprog.globals[semprog.nglobals];
      g->name = sem_dup(s->var_decl.name);
      g->type = dt;
      g->kind = kind != NULL ? sem_dup(kind) : NULL;
      g->init = s->var_decl.value;
      g->line = s->line;
      g->col = s->col;
      g->source = s->source;
      semprog.nglobals++;
    }
  }
}

/**
 * @brief Copies the checked frame variables for the IR builder
 * @param dst Receives the owned copy
 * @param n Receives the variable count
 */
void sem_snapshot_vars(SemVar **dst, int *n) {
  *n = sem_scope.nvars;
  *dst = NULL;
  if (*n == 0) {
    return;
  }
  *dst = malloc((size_t)(*n) * sizeof(SemVar));
  for (int i = 0; i < *n; i++) {
    (*dst)[i].name = sem_dup(sem_scope.vars[i].name);
    (*dst)[i].type = sem_scope.vars[i].type;
    (*dst)[i].kind = sem_scope.vars[i].kind != NULL
                         ? sem_dup(sem_scope.vars[i].kind)
                         : NULL;
    (*dst)[i].is_param = sem_scope.vars[i].is_param;
    (*dst)[i].line = sem_scope.vars[i].line;
    (*dst)[i].col = sem_scope.vars[i].col;
    (*dst)[i].used = sem_scope.vars[i].used;
    (*dst)[i].slot = sem_scope.vars[i].slot;
    (*dst)[i].elem = sem_scope.vars[i].elem;
    (*dst)[i].elem_kind = sem_scope.vars[i].elem_kind != NULL
                              ? sem_dup(sem_scope.vars[i].elem_kind)
                              : NULL;
  }
}

int SemAnalyze(Node *root, const char *source) {
  memset(&semprog, 0, sizeof(semprog));
  memset(&sem_scope, 0, sizeof(sem_scope));
  sem_source_file = source;
  sem_in_stmt = 0;
  ntrecs = 0;
  nsrecs = 0;

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, "main") == 0) {
      semprog.has_user_main = 1;
      break;
    }
  }

  collect_funcs(root);
  collect_types(root);
  resolve_fields(root);
  resolve_inheritance();
  collect_globals(root);
  sem_scan_fix(root);

  /* Checking always runs, even with collection errors: per-statement
     recovery reports the same follow-on diagnostics as before. */

  /* Top level statements form the entry program. */
  memset(&sem_scope, 0, sizeof(sem_scope));
  sem_scope.func = -2;
  sem_scope.cls = -1;
  sem_scope.mth = -1;
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION && s->type != NODE_STRUCT_DEF &&
        s->type != NODE_CLASS_DEF) {
      sem_check_stmt(s);
    }
  }
  sem_check_unused(0);
  sem_snapshot_vars(&semprog.entry_vars, &semprog.nentry_vars);

  for (int i = 0; i < semprog.nfuncs; i++) {
    jmp_buf saved;
    memcpy(saved, sem_jmp, sizeof(sem_jmp));
    if (setjmp(sem_jmp) == 0) {
      sem_in_stmt = 0;
      sem_check_body(&semprog.funcs[i], i, -1, -1);
    }
    memcpy(sem_jmp, saved, sizeof(sem_jmp));
  }
  sem_in_stmt = 0;

  for (int c = 0; c < semprog.nclasses; c++) {
    for (int m = 0; m < semprog.classes[c].nmethods; m++) {
      jmp_buf saved;
      memcpy(saved, sem_jmp, sizeof(sem_jmp));
      if (setjmp(sem_jmp) == 0) {
        sem_in_stmt = 0;
        sem_check_body(&semprog.classes[c].methods[m], -1, c, m);
      }
      memcpy(sem_jmp, saved, sizeof(sem_jmp));
    }
  }
  sem_in_stmt = 0;

  return term_errors() > 0 ? 1 : 0;
}
