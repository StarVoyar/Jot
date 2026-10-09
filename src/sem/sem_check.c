#include "sem_impl.h"

/** Print placeholder resolutions for the IR builder, keyed by print node */
typedef struct {
  Node *node;
  SemPrintInfo *info;
} PrintRec;

static PrintRec *precs;
static int nprecs;
static int capprecs;

SemPrintInfo *sem_print_info(Node *printNode) {
  for (int i = 0; i < nprecs; i++) {
    if (precs[i].node == printNode) {
      return precs[i].info;
    }
  }
  return NULL;
}

/**
 * @brief Records placeholder parts for a print statement
 * @param printNode Print statement node
 * @return Fresh info record owned by the map
 */
static SemPrintInfo *new_print_info(Node *printNode) {
  if (nprecs >= capprecs) {
    capprecs = capprecs != 0 ? capprecs * 2 : 64;
    precs = realloc(precs, (size_t)capprecs * sizeof(PrintRec));
  }
  SemPrintInfo *info = malloc(sizeof(SemPrintInfo));
  info->parts = NULL;
  info->nparts = 0;
  info->capparts = 0;
  precs[nprecs].node = printNode;
  precs[nprecs].info = info;
  nprecs++;
  return info;
}

/**
 * @brief Appends a literal text chunk to print info
 * @param info Info record
 * @param text Chunk text (copied)
 */
static void print_info_text(SemPrintInfo *info, const char *text) {
  if (info->nparts >= info->capparts) {
    info->capparts = info->capparts != 0 ? info->capparts * 2 : 8;
    info->parts =
        realloc(info->parts, (size_t)info->capparts * sizeof(SemPrintPart));
  }
  SemPrintPart *p = &info->parts[info->nparts++];
  p->is_text = 1;
  p->text = sem_dup(text);
  p->nsegs = 0;
  p->segs = NULL;
  p->type = ST_INT;
  p->kind = NULL;
  p->base = 0;
}

/**
 * @brief Appends a placeholder to print info
 * @param info Info record
 * @param segs Path segments (borrowed count, strings copied)
 * @param nsegs Segment count
 * @param type Resolved value type
 * @param kind Instance kind, may be NULL
 * @param base Base kind (0 name, 1 self param, 2 this field)
 */
static void print_info_place(SemPrintInfo *info, char **segs, int nsegs,
                             SemType type, const char *kind, int base) {
  if (info->nparts >= info->capparts) {
    info->capparts = info->capparts != 0 ? info->capparts * 2 : 8;
    info->parts =
        realloc(info->parts, (size_t)info->capparts * sizeof(SemPrintPart));
  }
  SemPrintPart *p = &info->parts[info->nparts++];
  p->is_text = 0;
  p->text = NULL;
  p->nsegs = nsegs;
  p->segs = malloc((size_t)nsegs * sizeof(char *));
  for (int i = 0; i < nsegs; i++) {
    p->segs[i] = sem_dup(segs[i]);
  }
  p->type = type;
  p->kind = kind;
  p->base = base;
}

/**
 * @brief Checks if a character can start an interpolated name
 * @param c Character to check
 * @return Non-zero if it can start a name
 */
static int is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/**
 * @brief Checks if a character can continue an interpolated name
 * @param c Character to check
 * @return Non-zero if it can continue a name
 */
static int is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9');
}

/**
 * @brief Resolves one placeholder path, recording its type
 * @param strnode String literal node at fault
 * @param info Info record receiving the placeholder
 * @param name Dotted path text (without braces)
 */
static void check_placeholder(Node *strnode, SemPrintInfo *info,
                              const char *name) {
  char *segs[9];
  int nsegs = 0;
  for (const char *rest = name;;) {
    const char *dot = strchr(rest, '.');
    size_t seg_len = dot != NULL ? (size_t)(dot - rest) : strlen(rest);
    if (nsegs >= 9) {
      sem_error(strnode, "Placeholder path too deep");
    }
    char *part = malloc(seg_len + 1);
    memcpy(part, rest, seg_len);
    part[seg_len] = '\0';
    segs[nsegs++] = part;
    if (dot == NULL) {
      break;
    }
    rest = dot + 1;
  }
  SemType type = ST_INT;
  const char *kind = NULL;
  int base = 0;
  if (strcmp(segs[0], "self") == 0) {
    if (nsegs < 2) {
      sem_error(strnode, "'self' is not a value by itself");
    }
    int pslot = sem_scope_find(segs[1]);
    if (pslot >= 0 && sem_scope.vars[pslot].is_param) {
      sem_scope.vars[pslot].used = 1;
      type = sem_scope.vars[pslot].type;
      kind = sem_scope.vars[pslot].kind;
      base = 1;
    } else if (sem_scope.in_method && sem_scope.cls >= 0) {
      int f = sem_find_class_field(sem_scope.cls, segs[1]);
      if (f < 0) {
        sem_error(strnode, "'%s' is not a parameter or field of class '%s'",
                  segs[1], sem_prog()->classes[sem_scope.cls].name);
      }
      int tslot = sem_scope_find("this ");
      if (tslot >= 0) {
        sem_scope.vars[tslot].used = 1;
      }
      SemField *fd = &sem_prog()->classes[sem_scope.cls].fields[f];
      type = fd->type;
      kind = fd->kind;
      base = 2;
    } else {
      int slot = sem_scope_require_param(strnode, segs[1]);
      type = sem_scope.vars[slot].type;
      kind = sem_scope.vars[slot].kind;
      base = 1;
    }
    /* Resolve the remaining segments as field lookups on the kind. */
    const char *cur_kind = kind;
    for (int i = 2; i < nsegs; i++) {
      SemField *def = NULL;
      int f = sem_find_instance_field(cur_kind, segs[i], &def);
      if (f < 0) {
        sem_error(strnode, "'%s' has no field '%s'", cur_kind, segs[i]);
      }
      type = def != NULL ? def->type : ST_INT;
      cur_kind = def != NULL ? def->kind : NULL;
    }
    kind = cur_kind;
  } else {
    int ref = sem_scope_require(strnode, segs[0]);
    if (sem_ref_is_global(ref)) {
      SemGlobal *g = &sem_prog()->globals[sem_ref_to_global(ref)];
      type = g->type;
      kind = g->kind;
    } else {
      SemVar *v = &sem_scope.vars[ref];
      type = v->type;
      kind = (v->type == ST_STRUCT || v->type == ST_CLASS) ? v->kind : NULL;
    }
    const char *cur_kind = kind;
    for (int i = 1; i < nsegs; i++) {
      SemField *def = NULL;
      int f = sem_find_instance_field(cur_kind, segs[i], &def);
      if (f < 0) {
        if (cur_kind == NULL) {
          sem_error(strnode, "Can only access fields on a struct or class "
                             "instance");
        }
        sem_error(strnode, "'%s' has no field '%s'", cur_kind, segs[i]);
      }
      type = def != NULL ? def->type : ST_INT;
      cur_kind = def != NULL ? def->kind : NULL;
    }
    kind = cur_kind;
  }
  if (type == ST_STRUCT || type == ST_CLASS || type == ST_ARR) {
    sem_error(strnode, "Cannot print %s '%s' directly",
              kind != NULL ? kind : sem_type_name(type), name);
  }
  print_info_place(info, segs, nsegs, type, kind, base);
  /* Emission evaluates the placeholder root again (second warning pass). */
  if (base == 0) {
    sem_scope_require(strnode, segs[0]);
  } else if (base == 1) {
    int pslot = sem_scope_find(segs[1]);
    if (pslot >= 0) {
      sem_scope.vars[pslot].used = 1;
    }
  } else {
    int tslot = sem_scope_find("this ");
    if (tslot >= 0) {
      sem_scope.vars[tslot].used = 1;
    }
  }
  for (int i = 0; i < nsegs; i++) {
    free(segs[i]);
  }
}

/**
 * @brief Validates an interpolated print string, recording its parts
 * @param printNode Print statement node
 * @param strnode String literal node at fault on bad placeholders
 * @param value Raw string value (may contain {name} placeholders)
 */
static void check_print_string(Node *printNode, Node *strnode,
                               const char *value) {
  SemPrintInfo *info = new_print_info(printNode);
  size_t start = 0;
  size_t i = 0;
  while (value[i] != '\0') {
    if (value[i] == '{') {
      size_t j = i + 1;
      if (is_name_start(value[j])) {
        while (is_name_char(value[j])) {
          j++;
        }
        while (value[j] == '.' && is_name_start(value[j + 1])) {
          j++;
          while (is_name_char(value[j])) {
            j++;
          }
        }
        if (value[j] == '}') {
          if (i > start) {
            size_t len = i - start;
            char *chunk = malloc(len + 1);
            memcpy(chunk, value + start, len);
            chunk[len] = '\0';
            print_info_text(info, chunk);
            free(chunk);
          }
          size_t name_len = j - (i + 1);
          char *name = malloc(name_len + 1);
          memcpy(name, value + i + 1, name_len);
          name[name_len] = '\0';
          check_placeholder(strnode, info, name);
          free(name);
          i = j + 1;
          start = i;
          continue;
        }
      }
    }
    i++;
  }
  if (value[i] == '\0' && i > start) {
    size_t len = i - start;
    char *chunk = malloc(len + 1);
    memcpy(chunk, value + start, len);
    chunk[len] = '\0';
    print_info_text(info, chunk);
    free(chunk);
  }
}

/** Global claim tracking for duplicate detection (mirrors global_init) */
static int claimed[SEM_MAX_GLOBALS];

static void check_block(Node *list);
static void check_stmt_inner(Node *node);

/**
 * @brief Checks a block, warning about unused locals at its end
 * @param list First statement in the block (linked via right)
 */
static void check_block(Node *list) {
  int saved_bind = sem_scope.nbind;
  int saved_slot = sem_scope.nvars;
  for (Node *s = list; s != NULL; s = s->right) {
    sem_check_stmt(s);
  }
  sem_check_unused(saved_slot);
  sem_scope.nbind = saved_bind;
}

/**
 * @brief Checks a global declaration claim and its initializer
 * @param node Global VAR_DECL node
 */
static void check_global_decl(Node *node) {
  int g = sem_find_global(node->var_decl.name);
  if (g < 0) {
    sem_error(node, "Global '%s' was not registered", node->var_decl.name);
  }
  if (claimed[g]) {
    sem_error(node, "Duplicate global variable '%s'", node->var_decl.name);
  }
  claimed[g] = 1;
  if (node->var_decl.value != NULL) {
    SemType given = sem_validate(node->var_decl.value);
    if (sem_prog()->globals[g].type == ST_INT && given == ST_FLOAT) {
      sem_prog()->globals[g].type = ST_FLOAT;
    }
    sem_touch(node->var_decl.value);
  }
}

/**
 * @brief Checks a local variable declaration
 * @param node VAR_DECL node
 */
static void check_var_decl(Node *node) {
  int existing = sem_scope_find(node->var_decl.name);
  if (existing >= 0 && !sem_scope.vars[existing].is_param) {
    sem_error(node, "Variable '%s' already declared", node->var_decl.name);
  }
  if (existing >= 0) {
    sem_warn(node, "Shadows parameter '%s'", node->var_decl.name);
  }
  const char *declared_kind = NULL;
  SemType declared =
      sem_resolve_decl(node->var_decl.var_type, node, &declared_kind);
  SemType given = ST_INT;
  int has_value = (node->var_decl.value != NULL);
  if (has_value) {
    given = sem_validate(node->var_decl.value);
    if (declared == ST_STRUCT || declared == ST_CLASS) {
      if (node->var_decl.value->type == NODE_NEW) {
        if (!sem_is_subclass(node->var_decl.value->new_expr.type_name,
                             declared_kind)) {
          sem_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                    node->var_decl.value->new_expr.type_name, declared_kind,
                    node->var_decl.name);
        }
        given = declared;
      } else if (given == declared) {
        const char *given_kind = sem_peek_kind(node->var_decl.value);
        if (given_kind != NULL &&
            !sem_is_subclass(given_kind, declared_kind)) {
          sem_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                    given_kind, declared_kind, node->var_decl.name);
        }
      } else if (given == ST_INT &&
                 node->var_decl.value->type == NODE_FUNC_CALL) {
        given = declared;
      } else if (given == ST_NULL) {
        given = declared;
      } else {
        sem_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                  sem_type_name(given), declared_kind, node->var_decl.name);
      }
    } else if (!sem_compatible(declared, given)) {
      sem_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                sem_type_name(given), sem_type_name(declared),
                node->var_decl.name);
    }
  }
  int slot = sem_scope_declare(node, node->var_decl.name, declared, 0);
  if (declared_kind != NULL) {
    sem_scope.vars[slot].kind = sem_dup(declared_kind);
  }
  sem_record_slot(node, slot);
  if (has_value && declared == ST_INT && given == ST_FLOAT) {
    sem_scope.vars[slot].type = ST_FLOAT;
  }
  if (has_value) {
    sem_touch(node->var_decl.value);
  }
}

/**
 * @brief Checks an assignment to a local or global
 * @param node ASSIGNMENT node
 */
static void check_assignment(Node *node) {
  int ref = sem_scope_require(node, node->assignment.name);
  if (sem_ref_is_global(ref)) {
    int gslot = sem_ref_to_global(ref);
    SemType given = sem_validate(node->assignment.value);
    if (!sem_compatible(sem_prog()->globals[gslot].type, given)) {
      sem_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                sem_type_name(given),
                sem_type_name(sem_prog()->globals[gslot].type),
                node->assignment.name);
    }
    sem_touch(node->assignment.value);
    if (sem_prog()->globals[gslot].type == ST_INT && given == ST_FLOAT) {
      sem_prog()->globals[gslot].type = ST_FLOAT;
    }
    return;
  }
  int slot = ref;
  if (sem_scope.vars[slot].is_param && !sem_scope.no_param_warn &&
      sem_scope.vars[slot].type != ST_STRUCT &&
      sem_scope.vars[slot].type != ST_CLASS) {
    sem_warn(node, "Parameter '%s' should be accessed as self.%s",
             node->assignment.name, node->assignment.name);
  }
  sem_scope.vars[slot].used = 1;
  SemType given = sem_validate(node->assignment.value);
  if (sem_scope.vars[slot].type == ST_STRUCT ||
      sem_scope.vars[slot].type == ST_CLASS) {
    const char *kind = sem_scope.vars[slot].kind;
    if (given == ST_NULL) {
      return;
    }
    if (node->assignment.value->type == NODE_NEW) {
      if (!sem_is_subclass(node->assignment.value->new_expr.type_name,
                           kind)) {
        sem_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                  node->assignment.value->new_expr.type_name, kind,
                  node->assignment.name);
      }
      sem_touch(node->assignment.value);
      return;
    }
    if (given == sem_scope.vars[slot].type &&
        (node->assignment.value->type == NODE_IDENTIFIER ||
         node->assignment.value->type == NODE_INDEX)) {
      const char *given_kind = sem_peek_kind(node->assignment.value);
      if (given_kind != NULL && !sem_is_subclass(given_kind, kind)) {
        sem_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                  given_kind, kind, node->assignment.name);
      }
      sem_touch(node->assignment.value);
      return;
    }
    if (given == ST_INT && node->assignment.value->type == NODE_FUNC_CALL) {
      sem_touch(node->assignment.value);
      return;
    }
    sem_error(node->assignment.value, "Cannot assign %s to %s '%s'",
              sem_type_name(given), kind, node->assignment.name);
    return;
  }
  if (!sem_compatible(sem_scope.vars[slot].type, given)) {
    sem_error(node->assignment.value, "Cannot assign %s to %s '%s'",
              sem_type_name(given),
              sem_type_name(sem_scope.vars[slot].type),
              node->assignment.name);
  }
  sem_touch(node->assignment.value);
  if (sem_scope.vars[slot].type == ST_INT && given == ST_FLOAT) {
    sem_scope.vars[slot].type = ST_FLOAT;
  }
}

/**
 * @brief Checks += or -= on a local or global
 * @param node ADD_ASSIGN or SUB_ASSIGN node
 * @param is_add Non-zero for +=, zero for -=
 */
static void check_add_sub(Node *node, int is_add) {
  const char *name = is_add ? node->add_assign.name : node->sub_assign.name;
  Node *value = is_add ? node->add_assign.value : node->sub_assign.value;
  int ref = sem_scope_require(node, name);
  const char *opword = is_add ? "+=" : "-=";
  (void)opword;
  if (sem_ref_is_global(ref)) {
    int gi = sem_ref_to_global(ref);
    SemType gtype = sem_prog()->globals[gi].type;
    SemType given = sem_validate(value);
    if (gtype == ST_STR) {
      if (is_add) {
        if (given != ST_STR) {
          sem_error(value, "Cannot concatenate string with %s",
                    sem_type_name(given));
        }
      } else {
        sem_error(node, "Operator '-=' cannot be applied to str");
      }
      sem_touch(value);
      return;
    }
    if (!sem_is_numeric(gtype)) {
      sem_error(node, "Cannot use %s on non-numeric type '%s'",
                is_add ? "+=" : "-=", sem_type_name(gtype));
    }
    sem_touch(value);
    if (gtype == ST_INT && given == ST_FLOAT) {
      sem_prog()->globals[gi].type = ST_FLOAT;
    }
    return;
  }
  int slot = ref;
  if (sem_scope.vars[slot].type == ST_STR) {
    if (is_add) {
      SemType given = sem_validate(value);
      if (given != ST_STR) {
        sem_error(value, "Cannot concatenate string with %s",
                  sem_type_name(given));
      }
      sem_touch(value);
    } else {
      sem_touch(value);
      sem_error(node, "Operator '-=' cannot be applied to str");
    }
    return;
  }
  if (!sem_is_numeric(sem_scope.vars[slot].type)) {
    sem_error(node, "Cannot use %s on non-numeric type '%s'",
              is_add ? "+=" : "-=", sem_type_name(sem_scope.vars[slot].type));
  }
  SemType given = sem_validate(value);
  sem_touch(value);
  if (sem_scope.vars[slot].type == ST_INT && given == ST_FLOAT) {
    sem_scope.vars[slot].type = ST_FLOAT;
  }
}

/**
 * @brief Checks array element assignment (a[i] = v, +=, -=)
 * @param node INDEX_ASSIGN node
 */
static void check_index_assign(Node *node) {
  Node *base = node->index_assign.base;
  Node *index = node->index_assign.index;
  Node *value = node->index_assign.value;
  const char *op = node->index_assign.op;
  if (base->type != NODE_IDENTIFIER) {
    sem_error(base, "Cannot index-assign a non-variable base");
  }
  int ref = sem_scope_require(node, base->identifier.name);
  if (sem_ref_is_global(ref) ||
      sem_scope.vars[ref].type != ST_ARR) {
    sem_error(node, "Can only assign into an array");
  }
  int slot = ref;
  SemType elem = sem_scope.vars[slot].elem;
  const char *ekind = sem_scope.vars[slot].elem_kind;
  sem_scope.vars[slot].used = 1;
  SemType idx_t = sem_peek(index);
  if (!sem_is_numeric(idx_t)) {
    sem_validate(index);
    sem_error(index, "Index must be a number, got %s",
              sem_type_name(idx_t));
  }
  if ((strcmp(op, "+=") == 0 || strcmp(op, "-=") == 0) &&
      (elem == ST_STRUCT || elem == ST_CLASS)) {
    sem_error(node, "Can only use '=' on struct/class array elements");
  }
  if (strcmp(op, "=") == 0) {
    SemType given = sem_validate(value);
    if (elem == ST_FLOAT) {
      if (!sem_is_numeric(given)) {
        sem_error(value, "Cannot assign %s to array element",
                  sem_type_name(given));
      }
    } else if (elem == ST_INT) {
      if (given != ST_INT && given != ST_BOOL) {
        sem_error(value, "Cannot assign %s to num array element",
                  sem_type_name(given));
      }
    } else if (elem == ST_STR) {
      if (given != ST_NULL && given != ST_STR) {
        sem_error(value, "Cannot assign %s to str array element",
                  sem_type_name(given));
      }
    } else if (elem == ST_STRUCT || elem == ST_CLASS) {
      if (given == ST_NULL) {
        sem_touch(base);
        sem_touch(index);
        return;
      }
      if (given == elem) {
        if (ekind != NULL) {
          const char *vk = sem_peek_kind(value);
          if (vk != NULL && strcmp(vk, ekind) != 0) {
            sem_error(value, "Cannot assign %s to %s array element", vk,
                      ekind);
          }
        }
      } else {
        sem_error(value, "Cannot assign %s to %s array element",
                  sem_type_name(given),
                  ekind != NULL ? ekind : sem_type_name(elem));
      }
    }
  } else if (elem == ST_STR) {
    if (strcmp(op, "-=") == 0) {
      sem_error(node, "Operator '-=' cannot be applied to str");
    }
    SemType given = sem_expr(value);
    if (given != ST_STR) {
      sem_error(value, "Cannot concatenate string with %s",
                sem_type_name(given));
    }
  } else {
    if (!sem_is_numeric(elem)) {
      sem_error(node, "Operator '%s' needs a numeric array element", op);
    }
    SemType given = sem_expr(value);
    if (!sem_is_numeric(given)) {
      sem_error(value, "Operator '%s' needs a numeric value, got %s", op,
                sem_type_name(given));
    }
    if (elem != ST_FLOAT && given == ST_FLOAT) {
      sem_error(value, "Cannot apply '%s' with float to int element", op);
    }
  }
  sem_touch(base);
  sem_touch(index);
  sem_touch(value);
}

/**
 * @brief Resolves a member-assignment target to a field
 * @param node MEMBER_ASSIGN node
 * @param field_type Receives the field category
 * @param field_kind Receives the instance kind for struct/class fields
 */
static void check_member_target(Node *node, SemType *field_type,
                                const char **field_kind) {
  const char *object = node->member_assign.object;
  const char *member = node->member_assign.member;
  *field_type = ST_INT;
  *field_kind = NULL;
  if (node->member_assign.object_expr != NULL) {
    const char *kind = sem_peek_kind(node->member_assign.object_expr);
    if (kind == NULL) {
      sem_error(node->member_assign.object_expr,
                "Can only assign fields on a struct or class instance");
    }
    SemField *def = NULL;
    int f = sem_find_instance_field(kind, member, &def);
    if (f < 0) {
      sem_error(node, "'%s' has no field '%s'", kind, member);
    }
    *field_type = def != NULL ? def->type : ST_INT;
    *field_kind = def != NULL ? def->kind : NULL;
    return;
  }
  if (strcmp(object, "self") == 0) {
    int pslot = sem_scope_find(member);
    if (pslot >= 0 && sem_scope.vars[pslot].is_param) {
      return;
    }
    if (sem_scope.in_method && sem_scope.cls >= 0 &&
        sem_find_class_field(sem_scope.cls, member) >= 0) {
      int f = sem_find_class_field(sem_scope.cls, member);
      SemField *fd = &sem_prog()->classes[sem_scope.cls].fields[f];
      *field_type = fd->type;
      *field_kind = fd->kind;
      int tslot = sem_scope_find("this ");
      if (tslot >= 0) {
        sem_scope.vars[tslot].used = 1;
      }
      return;
    }
    if (sem_scope.in_method && sem_scope.cls >= 0) {
      sem_error(node, "'%s' is not a parameter or field of class '%s'",
                member, sem_prog()->classes[sem_scope.cls].name);
    }
    int pslot2 = sem_scope_find(member);
    if (pslot2 >= 0 && sem_scope.vars[pslot2].is_param) {
      return;
    }
    sem_scope_require_param(node, member);
    return;
  }
  if (sem_find_struct(object) >= 0 || sem_find_class(object) >= 0) {
    sem_error(node, "Cannot use type '%s' as a value, use an instance",
              object);
  }
  int slot = sem_scope_find(object);
  if (slot < 0) {
    sem_error(node, "Variable '%s' not declared", object);
  }
  sem_scope.vars[slot].used = 1;
  if (sem_scope.vars[slot].type == ST_STRUCT) {
    int s = sem_find_struct(sem_scope.vars[slot].kind);
    int f = sem_find_struct_field(s, member);
    if (f < 0) {
      sem_error(node, "Struct '%s' has no field '%s'",
                sem_scope.vars[slot].kind, member);
    }
    *field_type = sem_prog()->structs[s].fields[f].type;
    *field_kind = sem_prog()->structs[s].fields[f].kind;
    return;
  }
  if (sem_scope.vars[slot].type == ST_CLASS) {
    int c = sem_find_class(sem_scope.vars[slot].kind);
    int f = sem_find_class_field(c, member);
    if (f < 0) {
      sem_error(node, "Class '%s' has no field '%s'",
                sem_scope.vars[slot].kind, member);
    }
    *field_type = sem_prog()->classes[c].fields[f].type;
    *field_kind = sem_prog()->classes[c].fields[f].kind;
    return;
  }
  sem_error(node, "'%s' is not a struct or class instance", object);
}

/**
 * @brief Marks the receiver of a member assignment like emission does
 * @param node MEMBER_ASSIGN node (already resolved)
 */
static void touch_member_base(Node *node) {
  if (node->member_assign.object_expr != NULL) {
    sem_touch(node->member_assign.object_expr);
    return;
  }
  if (strcmp(node->member_assign.object, "self") == 0) {
    int tslot = sem_scope_find("this ");
    if (tslot >= 0) {
      sem_scope.vars[tslot].used = 1;
    }
    return;
  }
  int slot = sem_scope_find(node->member_assign.object);
  if (slot >= 0) {
    sem_scope.vars[slot].used = 1;
  }
}

/**
 * @brief Checks member assignment (obj.field = v, +=, -=)
 * @param node MEMBER_ASSIGN node
 */
static void check_member_assign(Node *node) {
  const char *member = node->member_assign.member;
  const char *op = node->member_assign.op;
  Node *value = node->member_assign.value;
  int is_param_target = 0;
  if (node->member_assign.object_expr == NULL &&
      strcmp(node->member_assign.object, "self") == 0) {
    int pslot = sem_scope_find(member);
    if (pslot >= 0 && sem_scope.vars[pslot].is_param) {
      is_param_target = 1;
    }
  }
  if (is_param_target) {
    Node tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.line = node->line;
    tmp.col = node->col;
    tmp.width = node->width;
    if (strcmp(op, "=") == 0) {
      tmp.type = NODE_ASSIGNMENT;
      tmp.assignment.name = (char *)member;
      tmp.assignment.value = value;
    } else if (strcmp(op, "+=") == 0) {
      tmp.type = NODE_ADD_ASSIGN;
      tmp.add_assign.name = (char *)member;
      tmp.add_assign.value = value;
    } else {
      tmp.type = NODE_SUB_ASSIGN;
      tmp.sub_assign.name = (char *)member;
      tmp.sub_assign.value = value;
    }
    int saved = sem_scope.no_param_warn;
    sem_scope.no_param_warn = 1;
    check_stmt_inner(&tmp);
    sem_scope.no_param_warn = saved;
    return;
  }
  SemType field_type = ST_INT;
  const char *field_kind = NULL;
  check_member_target(node, &field_type, &field_kind);
  SemType given = sem_expr(value);
  if (field_type == ST_STR && strcmp(op, "-=") == 0) {
    sem_error(node, "Operator '-=' cannot be applied to str");
  }
  if (field_type == ST_STR && given != ST_STR) {
    if (strcmp(op, "=") == 0) {
      sem_error(value, "Cannot assign %s to str field '%s'",
                sem_type_name(given), member);
    }
    sem_error(value, "Cannot concatenate string with %s",
              sem_type_name(given));
  }
  if (field_type == ST_STRUCT || field_type == ST_CLASS) {
    if (strcmp(op, "=") != 0) {
      sem_error(node, "Operator '%s' cannot be applied to instance fields",
                op);
    }
    if (given != ST_NULL) {
      if (given != field_type) {
        sem_error(value, "Cannot assign %s to %s field '%s'",
                  sem_type_name(given),
                  field_kind != NULL ? field_kind : "instance", member);
      } else if (field_kind != NULL) {
        const char *ak = sem_peek_kind(value);
        if (ak != NULL && !sem_is_subclass(ak, field_kind)) {
          sem_error(value, "Cannot assign %s to %s field '%s'", ak,
                    field_kind, member);
        }
      }
    }
    sem_touch(value);
    touch_member_base(node);
    return;
  }
  if (field_type != ST_STR && !sem_is_numeric(given)) {
    sem_error(value, "Cannot assign %s to %s field '%s'",
              sem_type_name(given),
              field_type == ST_FLOAT ? "num" : "bool", member);
  }
  if (strcmp(op, "=") == 0) {
    sem_touch(value);
    touch_member_base(node);
  } else {
    touch_member_base(node);
    sem_touch(value);
  }
}

/**
 * @brief Checks a print statement
 * @param node PRINT node
 */
static void check_print(Node *node) {
  Node *value = node->print_stmt.value;
  if (value != NULL && value->type == NODE_STRING_LITERAL) {
    check_print_string(node, value, value->string_literal.value);
    return;
  }
  if (value != NULL && value->type == NODE_IDENTIFIER) {
    int ref = sem_scope_require(value, value->identifier.name);
    if (!sem_ref_is_global(ref)) {
      SemVar *v = &sem_scope.vars[ref];
      if (v->type == ST_STRUCT || v->type == ST_CLASS) {
        sem_error(value, "Cannot print %s '%s' directly", v->kind,
                  value->identifier.name);
      }
    }
    return;
  }
  SemType etype = sem_peek(value);
  if (etype == ST_STRUCT || etype == ST_CLASS) {
    const char *k = sem_peek_kind(value);
    if (value->type == NODE_NEW) {
      k = value->new_expr.type_name;
    }
    if (k != NULL) {
      sem_error(value, "Cannot print %s directly", k);
    }
    sem_error(value, "Cannot print struct or class values directly");
  }
  sem_touch(value);
}

/**
 * @brief Checks a return statement
 * @param node RETURN node
 */
static void check_return(Node *node) {
  if (node->return_stmt.value == NULL) {
    if (sem_scope.ret == NULL || strcmp(sem_scope.ret, "void") != 0) {
      sem_error(node, "Bare 'return;' is only allowed in functions declared "
                      "'-> void'");
    }
    return;
  }
  if (sem_scope.ret != NULL && strcmp(sem_scope.ret, "void") == 0) {
    sem_error(node->return_stmt.value,
              "Function declared '-> void' cannot return a value");
  }
  if (sem_scope.in_method && sem_scope.cls >= 0) {
    SemFunc *md =
        &sem_prog()->classes[sem_scope.cls].methods[sem_scope.mth];
    const char *mname = md->name;
    SemType given = sem_validate(node->return_stmt.value);
    if (strcmp(md->ret, "str") == 0) {
      if (given != ST_STR) {
        sem_error(node->return_stmt.value,
                  "Method '%s' must return string, got %s", mname,
                  sem_type_name(given));
      }
    } else if (strcmp(md->ret, "bool") == 0) {
      if (!sem_is_numeric(given)) {
        sem_error(node->return_stmt.value,
                  "Method '%s' must return bool, got %s", mname,
                  sem_type_name(given));
      }
    } else {
      if (!sem_is_numeric(given)) {
        sem_error(node->return_stmt.value,
                  "Method '%s' must return num, got %s", mname,
                  sem_type_name(given));
      }
    }
    sem_touch(node->return_stmt.value);
    return;
  }
  sem_touch(node->return_stmt.value);
}

/**
 * @brief Checks a condition expression
 * @param cond Condition node
 */
static void check_condition(Node *cond) {
  if (cond == NULL) {
    sem_touch(NULL);
    return;
  }
  SemType ct = sem_peek(cond);
  if (!sem_is_numeric(ct)) {
    sem_validate(cond);
    sem_error(cond, "Condition must be a number, got %s",
              sem_type_name(ct));
  }
  if (cond->type == NODE_BINARY_OP) {
    const char *op = cond->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
      SemType lt = sem_peek(cond->binary_op.left);
      SemType rt = sem_peek(cond->binary_op.right);
      if (lt != ST_FLOAT && rt != ST_FLOAT) {
        /* Integer conditions compare directly with no operand checks. */
        sem_touch(cond->binary_op.left);
        sem_touch(cond->binary_op.right);
        return;
      }
    }
  }
  sem_touch(cond);
}

/**
 * @brief Checks an array declaration
 * @param node ARRAY_DECL node
 */
static void check_array_decl(Node *node) {
  int existing = sem_scope_find(node->array_decl.name);
  if (existing >= 0 && !sem_scope.vars[existing].is_param) {
    sem_error(node, "Variable '%s' already declared", node->array_decl.name);
  }
  if (existing >= 0) {
    sem_warn(node, "Shadows parameter '%s'", node->array_decl.name);
  }
  int slot = sem_scope_declare(node, node->array_decl.name, ST_ARR, 0);
  sem_record_slot(node, slot);
  int narrs = 0;
  for (int i = 0; i < sem_scope.nbind; i++) {
    if (sem_scope.vars[sem_scope.bind_slot[i]].type == ST_ARR) {
      narrs++;
    }
  }
  if (narrs > SEM_MAX_ARRAYS) {
    sem_error(node, "Too many arrays in function");
  }
  Node *rhs = node->array_decl.elements;
  if (rhs != NULL && rhs->type == NODE_IDENTIFIER) {
    int src = sem_scope_find(rhs->identifier.name);
    if (src < 0 || sem_scope.vars[src].type != ST_ARR) {
      sem_error(rhs, "Cannot initialize 'array' from non-array '%s'",
                rhs->identifier.name);
    }
    sem_scope.vars[slot].elem = sem_scope.vars[src].elem;
    if (sem_scope.vars[src].elem_kind != NULL) {
      sem_scope.vars[slot].elem_kind =
          sem_dup(sem_scope.vars[src].elem_kind);
    }
    sem_touch(rhs);
    return;
  }
  if (rhs == NULL || rhs->type != NODE_ARRAY_LITERAL) {
    sem_error(node, "Array must be initialized with [...] or another array");
  }
  SemType elem = ST_INT;
  const char *elem_kind = NULL;
  int seen = 0, any_float = 0, struct_like = 0;
  for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
    SemType v = sem_validate(e);
    const char *k =
        (v == ST_STRUCT || v == ST_CLASS) ? sem_peek_kind(e) : NULL;
    if (!seen) {
      elem = v;
      elem_kind = k;
      seen = 1;
      struct_like = (v == ST_STRUCT || v == ST_CLASS);
      any_float = (v == ST_FLOAT);
      continue;
    }
    if (struct_like) {
      if (v != elem || (v != ST_STRUCT && v != ST_CLASS) ||
          (elem_kind != NULL && k != NULL && strcmp(elem_kind, k) != 0)) {
        sem_error(e, "Array elements must all have the same struct/class "
                     "type");
      }
      if (elem_kind == NULL && k != NULL) {
        elem_kind = k;
      }
    } else if (elem == ST_STR) {
      if (v != ST_STR) {
        sem_error(e, "Array elements must all be strings");
      }
    } else if (sem_is_numeric(elem) && sem_is_numeric(v)) {
      if (v == ST_FLOAT) {
        any_float = 1;
      }
    } else {
      sem_error(e, "Array elements must all be numbers, all strings, or "
                   "instances of one struct/class");
    }
  }
  if (!seen) {
    elem = ST_INT;
    elem_kind = NULL;
  } else if (sem_is_numeric(elem) && any_float) {
    elem = ST_FLOAT;
  }
  sem_scope.vars[slot].elem = elem;
  if (elem_kind != NULL) {
    sem_scope.vars[slot].elem_kind = sem_dup(elem_kind);
  }
  for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
    sem_touch(e);
  }
}

/**
 * @brief Checks a for-in loop over an array
 * @param node FOR node
 */
static void check_for(Node *node) {
  int aslot = sem_scope_find(node->for_stmt.array_name);
  if (aslot < 0 || sem_scope.vars[aslot].type != ST_ARR) {
    if (aslot >= 0) {
      SemVar *av = &sem_scope.vars[aslot];
      const char *kind = (av->type == ST_STRUCT || av->type == ST_CLASS)
                             ? av->kind
                             : sem_type_name(av->type);
      sem_error(node, "Cannot iterate over %s '%s', only arrays can", kind,
                node->for_stmt.array_name);
    }
    sem_error(node, "Array '%s' is not declared", node->for_stmt.array_name);
  }
  int saved = sem_scope.nbind;
  int existing = sem_scope_find(node->for_stmt.var_name);
  if (existing >= 0 && !sem_scope.vars[existing].is_param) {
    sem_error(node, "Variable '%s' already declared",
              node->for_stmt.var_name);
  }
  if (existing >= 0) {
    sem_warn(node, "Shadows parameter '%s'", node->for_stmt.var_name);
  }
  SemType elem_vtype = sem_scope.vars[aslot].elem;
  sem_scope.vars[aslot].used = 1;
  int var_slot =
      sem_scope_declare(node, node->for_stmt.var_name, elem_vtype, 0);
  if ((elem_vtype == ST_STRUCT || elem_vtype == ST_CLASS) &&
      sem_scope.vars[aslot].elem_kind != NULL) {
    sem_scope.vars[var_slot].kind =
        sem_dup(sem_scope.vars[aslot].elem_kind);
  }
  sem_record_slot(node, var_slot);
  if (sem_scope.loop_depth >= SEM_MAX_LOOP_DEPTH) {
    sem_error(node, "Loops nested too deeply");
  }
  sem_scope.loop_depth++;
  check_block(node->for_stmt.body);
  sem_scope.loop_depth--;
  sem_scope.nbind = saved;
}

/**
 * @brief Checks one statement without installing recovery
 * @param node Statement node
 */
static void check_stmt_inner(Node *node) {
  switch (node->type) {
  case NODE_VAR_DECL:
    if (node->var_decl.is_global) {
      check_global_decl(node);
    } else {
      check_var_decl(node);
    }
    break;
  case NODE_ASSIGNMENT:
    check_assignment(node);
    break;
  case NODE_ADD_ASSIGN:
    check_add_sub(node, 1);
    break;
  case NODE_SUB_ASSIGN:
    check_add_sub(node, 0);
    break;
  case NODE_INDEX_ASSIGN:
    check_index_assign(node);
    break;
  case NODE_MEMBER_ASSIGN:
    check_member_assign(node);
    break;
  case NODE_METHOD_CALL:
    sem_touch(node);
    break;
  case NODE_PRINT:
    check_print(node);
    break;
  case NODE_RETURN:
    check_return(node);
    break;
  case NODE_IF:
    check_condition(node->if_stmt.condition);
    check_block(node->if_stmt.body);
    if (node->if_stmt.else_body != NULL) {
      if (node->if_stmt.else_body->type == NODE_IF) {
        check_stmt_inner(node->if_stmt.else_body);
      } else {
        check_block(node->if_stmt.else_body);
      }
    }
    break;
  case NODE_WHILE:
    if (sem_scope.loop_depth >= SEM_MAX_LOOP_DEPTH) {
      sem_error(node, "Loops nested too deeply");
    }
    sem_scope.loop_depth++;
    check_condition(node->while_stmt.condition);
    check_block(node->while_stmt.body);
    sem_scope.loop_depth--;
    break;
  case NODE_BREAK:
    if (sem_scope.loop_depth <= 0) {
      sem_error(node, "break outside of a loop");
    }
    break;
  case NODE_CONTINUE:
    if (sem_scope.loop_depth <= 0) {
      sem_error(node, "continue outside of a loop");
    }
    break;
  case NODE_FUNC_CALL:
    sem_check_call(node, 1);
    break;
  case NODE_FUNCTION:
    sem_error(node, "Nested functions not supported in codegen");
    break;
  case NODE_ARRAY_DECL:
    check_array_decl(node);
    break;
  case NODE_FOR:
    check_for(node);
    break;
  default:
    sem_error(node, "Unexpected statement in codegen");
    break;
  }
}

void sem_check_stmt(Node *node) {
  jmp_buf saved;
  memcpy(saved, sem_jmp, sizeof(sem_jmp));
  sem_in_stmt = 1;
  if (setjmp(sem_jmp) == 0) {
    check_stmt_inner(node);
  }
  sem_in_stmt = 0;
  memcpy(sem_jmp, saved, sizeof(sem_jmp));
}

/**
 * @brief Checks a function or method body with function recovery
 * @param func Function record with params, body, and return info
 * @param func_idx Plain function index, or -2 for the entry program
 * @param cls Class index for methods, -1 otherwise
 * @param mth Method index for methods, -1 otherwise
 */
void sem_check_body(SemFunc *func, int func_idx, int cls, int mth) {
  memset(&sem_scope, 0, sizeof(sem_scope));
  sem_scope.func = func_idx;
  sem_scope.cls = cls;
  sem_scope.mth = mth;
  sem_scope.in_method = (cls >= 0 && mth >= 0);
  sem_scope.ret = func->ret;
  if (sem_scope.in_method) {
    int tslot = sem_scope_declare(NULL, "this ", ST_CLASS, 1);
    sem_scope.vars[tslot].kind =
        sem_dup(sem_prog()->classes[cls].name);
  }
  int nparams = sem_count_params(func->params);
  func->nparams = nparams;
  if (nparams > 0) {
    func->ptypes = malloc((size_t)nparams * sizeof(SemType));
    func->pkinds = malloc((size_t)nparams * sizeof(char *));
    func->pnames = malloc((size_t)nparams * sizeof(char *));
  } else {
    func->ptypes = NULL;
    func->pkinds = NULL;
    func->pnames = NULL;
  }
  int idx = 0;
  for (Node *p = func->params; p != NULL; p = p->right, idx++) {
    const char *pname = NULL;
    SemType pt = ST_INT;
    const char *pkind = NULL;
    if (p->type == NODE_VAR_DECL) {
      pname = p->var_decl.name;
      pt = sem_resolve_decl(p->var_decl.var_type, p, &pkind);
      if (pt == ST_INT && idx < SEM_MAX_PARAMS && func->pfloat[idx]) {
        pt = ST_FLOAT;
      }
    } else if (p->type == NODE_IDENTIFIER) {
      pname = p->identifier.name;
      if (idx < SEM_MAX_PARAMS && func->pfloat[idx]) {
        pt = ST_FLOAT;
      }
    } else {
      sem_error(p, "Unexpected parameter in codegen");
      pname = "?";
    }
    int slot = sem_scope_declare(p, pname, pt, 1);
    if (pkind != NULL) {
      sem_scope.vars[slot].kind = sem_dup(pkind);
    }
    sem_record_slot(p, slot);
    if (sem_scope.in_method &&
        sem_find_class_field(cls, pname) >= 0) {
      sem_warn(p, "Parameter '%s' shadows a field of class '%s'", pname,
               sem_prog()->classes[cls].name);
    }
    func->pnames[idx] = sem_dup(pname);
    func->ptypes[idx] = pt;
    func->pkinds[idx] = pkind != NULL ? sem_dup(pkind) : NULL;
  }
  /* Check the body without popping its variables: the snapshot below
     needs the full frame (old emission popped because it was done). */
  int base = sem_scope.nvars;
  for (Node *s = func->body; s != NULL; s = s->right) {
    sem_check_stmt(s);
  }
  sem_check_unused(base);
  sem_snapshot_vars(&func->vars, &func->nvars);
}

