/* ------------------------------------------------------------------------- */
/*   "asm" : The Inform assembler                                            */
/*                                                                           */
/*   Part of Inform 6.36                                                     */
/*   copyright (c) Graham Nelson 1993 - 2021                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#include "header.h"

typedef struct block_t {
    int type; /* block_wc, loop_wc, if_wc, or else_wc */
    int label;
    int push_true; /* Push TRUE before branching? */
} block;

uchar *zcode_holding_area;         /* Area holding code yet to be transferred
                                      to either zcode_area or temp file no 1 */
uchar *zcode_markers;              /* Bytes holding marker values for this
                                      code                                   */
static int zcode_ha_size;          /* Number of bytes in holding area        */


memory_block zcode_area;           /* Block to hold assembled code (if
                                      temporary files are not being used)    */

int32 zmachine_pc;                 /* PC position of assembly (byte offset
                                      from start of Z-code area)             */

int32 no_instructions;             /* Number of instructions assembled       */
int execution_never_reaches_here,  /* TRUE if the current PC value in the
                                      code area cannot be reached: e.g. if
                                      the previous instruction was a "quit"
                                      opcode and no label is set to here     */
    next_label,                    /* Used to count the labels created all
                                      over Inform in current routine, from 0 */
    next_sequence_point;           /* Likewise, for sequence points          */
int no_sequence_points;            /* Kept for statistics purposes only      */

static int label_moved_error_already_given;
                                   /* When one label has moved, all subsequent
                                      ones probably have too, and this flag
                                      suppresses the runaway chain of error
                                      messages which would otherwise result  */

int  sequence_point_follows;       /* Will the next instruction assembled    */
                                   /* be at a sequence point in the routine? */

int uses_unicode_features;         /* Makes use of Glulx Unicode (3.0)
                                      features?                              */
int uses_memheap_features;         /* Makes use of Glulx mem/heap (3.1)
                                      features?                              */
int uses_acceleration_features;    /* Makes use of Glulx acceleration (3.1.1)
                                      features?                              */
int uses_float_features;           /* Makes use of Glulx floating-point (3.1.2)
                                      features?                              */

block *blocks_top, *blocks_bottom; /* current WebAssembly blocks */
memory_stack blocks_stack;

debug_location statement_debug_location;
                                   /* Location of current statement          */


variableinfo *variables;           /* The allocated size is 
                                      (MAX_LOCAL_VARIABLES + no_globals).
                                      Local variables first, then globals.   */
memory_list variables_memlist;


wasm_type *wasm_types;
uint32 no_wasm_types;
static memory_list wasm_types_memlist;

assembly_instruction AI;           /* A structure used to hold the full
                                      specification of a single Z-code
                                      instruction: effectively this is the
                                      input to the routine
                                      assemble_instruction()                 */

static char opcode_syntax_string[128];  /*  Text buffer holding the correct
                                      syntax for an opcode: used to produce
                                      helpful assembler error messages       */

static int routine_symbol;         /* The symbol index of the routine currently
                                      being compiled */
static char *routine_name;         /* The name of the routine currently being
                                      compiled                               */
static int routine_locals;         /* The number of local variables used by
                                      the routine currently being compiled   */

static int32 routine_start_pc;

int32 *named_routine_symbols;

static void transfer_routine_z(void);
static void transfer_routine_g(void);
static void transfer_routine_w(void);

/* ------------------------------------------------------------------------- */
/*   Label data                                                              */
/* ------------------------------------------------------------------------- */

static int first_label, last_label;
static int32 *label_offsets;       /* Double-linked list of label offsets    */
static int   *label_next,          /* (i.e. zmachine_pc values) in PC order  */
             *label_prev;
static int32 *label_symbols;       /* Symbol numbers if defined in source    */

static int   *sequence_point_labels;
                                   /* Label numbers for each                 */
static debug_location *sequence_point_locations;
                                   /* Source code references for each        */
                                   /* (used for making debugging file)       */

static void set_label_offset(int label, int32 offset)
{
    if (label >= MAX_LABELS) memoryerror("MAX_LABELS", MAX_LABELS);

    label_offsets[label] = offset;
    if (last_label == -1)
    {   label_prev[label] = -1;
        first_label = label;
    }
    else
    {   label_prev[label] = last_label;
        label_next[last_label] = label;
    }
    last_label = label;
    label_next[label] = -1;
    label_symbols[label] = -1;
}

/* ------------------------------------------------------------------------- */
/*   Useful tool for building operands                                       */
/* ------------------------------------------------------------------------- */

extern void set_constant_ot(assembly_operand *AO)
{
  switch (target_machine) {
    case TARGET_ZCODE:
    if (AO->value >= 0 && AO->value <= 255)
      AO->type = SHORT_CONSTANT_OT;
    else
      AO->type = LONG_CONSTANT_OT;
    break;

    case TARGET_GLULX:
    if (AO->value == 0)
      AO->type = ZEROCONSTANT_OT;
    else if (AO->value >= -0x80 && AO->value < 0x80)
      AO->type = BYTECONSTANT_OT;
    else if (AO->value >= -0x8000 && AO->value < 0x8000) 
      AO->type = HALFCONSTANT_OT;
    else
      AO->type = CONSTANT_OT;
    break;

    case TARGET_WASM:
      AO->type = CONSTANT_OT;
    break;
  }
}

extern int is_constant_ot(int otval)
{
  switch (target_machine) {
    case TARGET_ZCODE:
    return ((otval == LONG_CONSTANT_OT) 
      || (otval == SHORT_CONSTANT_OT));

    case TARGET_GLULX:
    return ((otval == CONSTANT_OT)
      || (otval == HALFCONSTANT_OT)
      || (otval == BYTECONSTANT_OT)
      || (otval == ZEROCONSTANT_OT));

    case TARGET_WASM:
    return (otval == CONSTANT_OT);
  }
}

extern int is_variable_ot(int otval)
{
  switch (target_machine) {
    case TARGET_ZCODE:
    return (otval == VARIABLE_OT);

    case TARGET_GLULX:
    return ((otval == LOCALVAR_OT)
      || (otval == GLOBALVAR_OT));

    case TARGET_WASM:
    return ((otval == LOCALVAR_OT)
      || (otval == GLOBALVAR_OT));
  }
}

/* ------------------------------------------------------------------------- */
/*   Used in printing assembly traces                                        */
/* ------------------------------------------------------------------------- */

extern char *variable_name(int32 i)
{


    switch (target_machine) {
      case TARGET_ZCODE:
      if (i==0) return("sp");
      if (i<MAX_LOCAL_VARIABLES) return local_variable_texts[i-1];
      if (i==255) return("TEMP1");
      if (i==254) return("TEMP2");
      if (i==253) return("TEMP3");
      if (i==252) return("TEMP4");
      if (i==251) return("self");
      if (i==250) return("sender");
      if (i==249) return("sw__var");
      if (i >= 256 && i < 286)
      {   if (i - 256 < NUMBER_SYSTEM_FUNCTIONS) return system_functions.keywords[i - 256];
          return "<unnamed system function>";
      }
      break;

      case TARGET_GLULX:
      if (i==0) return("sp");
      if (i<MAX_LOCAL_VARIABLES) return local_variable_texts[i-1];
      switch (i - MAX_LOCAL_VARIABLES) {
      case 0: return "temp_global";
      case 1: return "temp__global2";
      case 2: return "temp__global3";
      case 3: return "temp__global4";
      case 4: return "self";
      case 5: return "sender";
      case 6: return "sw__var";
      case 7: return "sys__glob0";
      case 8: return "sys__glob1";
      case 9: return "sys__glob2";
      case 10: return "sys_statusline_flag";
      }
      break;

      case TARGET_WASM:
      if (i == temp_var1.value) return "TEMP1";
      if (i == temp_var2.value) return "TEMP2";
      if (i == temp_var3.value) return "TEMP3";
      if (i<MAX_LOCAL_VARIABLES) return local_variable_texts[i];
WABORT;
    }

    return ((char *) symbs[variables[i].token]);
}

/* Print symbolic information about the AO, if there is any. */
static void print_operand_annotation(const assembly_operand *o)
{
    int any = FALSE;
    if (o->marker) {
        printf((!any) ? " (" : ": ");
        any = TRUE;
        printf("%s", describe_mv(o->marker));
        switch (o->marker) {
        case VROUTINE_MV:
            printf(": %s", veneer_routine_name(o->value));
            break;
        case INCON_MV:
            printf(": %s", name_of_system_constant(o->value));
            break;
        case DWORD_MV:
            printf(": '");
            print_dict_word(o->value);
            printf("'");
            break;
        }
    }
    if (o->symindex >= 0 && o->symindex < no_symbols) {
        printf((!any) ? " (" : ": ");
        any = TRUE;
        printf("%s", (char *)symbs[o->symindex]);
    }
    if (any) printf(")");       
}

static void print_operand_z(const assembly_operand *o, int annotate)
{   switch(o->type)
    {   case EXPRESSION_OT: printf("expr_"); break;
        case LONG_CONSTANT_OT: printf("long_"); break;
        case SHORT_CONSTANT_OT: printf("short_"); break;
        case VARIABLE_OT:
             if (o->value==0) { printf("sp"); return; }
             printf("%s", variable_name(o->value)); return;
        case OMITTED_OT: printf("<no value>"); return;
    }
    printf("%d", o->value);
    if (annotate)
        print_operand_annotation(o);
}

static void print_operand_g(const assembly_operand *o, int annotate)
{
  switch (o->type) {
  case EXPRESSION_OT: printf("expr_"); break;
  case CONSTANT_OT: printf("long_"); break;
  case HALFCONSTANT_OT: printf("short_"); break;
  case BYTECONSTANT_OT: printf("byte_"); break;
  case ZEROCONSTANT_OT: printf("zero_"); return;
  case DEREFERENCE_OT: printf("*"); break;
  case GLOBALVAR_OT: 
    printf("global_%d (%s)", o->value, variable_name(o->value)); 
    return;
  case LOCALVAR_OT: 
    if (o->value == 0)
      printf("stackptr"); 
    else
      printf("local_%d (%s)", o->value-1, variable_name(o->value)); 
    return;
  case SYSFUN_OT:
    if (o->value >= 0 && o->value < NUMBER_SYSTEM_FUNCTIONS)
      printf("%s", system_functions.keywords[o->value]);
    else
      printf("<unnamed system function>");
    return;
  case OMITTED_OT: printf("<no value>"); return;
  default: printf("???_"); break; 
  }
  printf("%d", o->value);
  if (annotate)
    print_operand_annotation(o);
}

static void print_operand_w(const assembly_operand *o, int annotate)
{
  switch (o->type) {
  case EXPRESSION_OT: printf("expr_"); break;
  case CONSTANT_OT: printf("constant_"); break;
  case DEREFERENCE_OT: printf("*"); break;
  case GLOBALVAR_OT: 
    printf("global_%d (%s)", o->value, variable_name(o->value)); 
    return;
  case LOCALVAR_OT: 
      printf("local_%d (%s)", o->value, variable_name(o->value)); 
    return;
  case SYSFUN_OT:
    if (o->value >= 0 && o->value < NUMBER_SYSTEM_FUNCTIONS)
      printf("%s", system_functions.keywords[o->value]);
    else
      printf("<unnamed system function>");
    return;
  case OMITTED_OT: printf("<no value>"); return;
  case BLOCKTYPE_OT: 
    switch (o->value) {
      case 0x40: printf("<void>"); return;
      case 0x7f: printf("<i32>"); return;
      case 0x7e: printf("<i64>"); return;
      case 0x7d: printf("<f32>"); return;
      case 0x7c: printf("<f64>"); return;
      case 0x70: printf("<funcref>"); return;
      case 0x6f: printf("<externref>"); return;
      default: printf("<type %d>", o->value); return;
    }
  case BLOCK_OT: 
    if (o->value < blocks_stack.no_items)
	printf("block_%d (L%d)", o->value, blocks_top[-o->value].label);
    else
	printf("block_%d (return)", o->value);
    return;

  default: printf("???_"); break; 
  }
  printf("%d", o->value);
  if (annotate)
    print_operand_annotation(o);
}


extern void print_operand(const assembly_operand *o, int annotate)
{
  switch (target_machine) {
    case TARGET_ZCODE:
    print_operand_z(o, annotate);
    break;

    case TARGET_GLULX:
    print_operand_g(o, annotate);
    break;

    case TARGET_WASM:
    print_operand_w(o, annotate);
  }
}

/* ------------------------------------------------------------------------- */
/*   Writing bytes to the code area                                          */
/* ------------------------------------------------------------------------- */

static void byteout(int32 i, int mv)
{   if (zcode_ha_size >= MAX_ZCODE_SIZE)
        memoryerror("MAX_ZCODE_SIZE",MAX_ZCODE_SIZE);
    zcode_markers[zcode_ha_size] = (uchar) mv;
    zcode_holding_area[zcode_ha_size++] = (uchar) i;
    zmachine_pc++;
}

/* ------------------------------------------------------------------------- */
/*   A database of the 115 canonical Infocom opcodes in Versions 3 to 6      */
/*   And of the however-many-there-are Glulx opcodes                         */
/* ------------------------------------------------------------------------- */

typedef struct opcodez
{   uchar *name;      /* Lower case standard name */
    int version1;     /* Valid from this version number... */
    int version2;     /* ...until this one (or forever if this is 0) */
    int extension;    /* In later versions, see this line in extension table:
                         if -1, the opcode is illegal in later versions */
    int code;         /* Opcode number within its operand-number block */
    int flags;        /* Flags (see below) */
    int op_rules;     /* Any unusual operand rule applying (see below) */
    int flags2_set;   /* If not zero, set this bit in Flags 2 in the header
                         of any game using the opcode */
    int no;           /* Number of operands (see below) */
} opcodez;

typedef struct opcodeg
{   uchar *name;      /* Lower case standard name */
    int32 code;       /* Opcode number */
    int flags;        /* Flags (see below) */
    int op_rules;     /* Any unusual operand rule applying (see below) */
    int no;           /* Number of operands */
} opcodeg;


typedef struct opcodew
{   uchar *name;      /*  Standard name */
    int32 code;       /* Opcode number */
    int flags;        /* Flags (see below) */
  //  int op_rules;     /* Any unusual operand rule applying (see below) */
    int no;           /* Number of operands */
} opcodew;

    /* Flags which can be set */

#define St      1     /* Store */
#define Br      2     /* Branch */
#define Rf      4     /* "Return flag": execution never continues after this
                         opcode (e.g., is a return or unconditional jump) */
#define St2 8     /* Store2 (second-to-last operand is store (Glulx)) */
#define Bl 16 /* Block structure opcodes used by WASM, such as "end". Doesn't check or affect execution reachability TODO another flag for else's behavior? (opposite of Rf) */

    /* Codes for any unusual operand assembly rules */

    /* Z-code: */

#define VARIAB   1    /* First operand expected to be a variable name and
                         assembled to a short constant: the variable number */
#define TEXT     2    /* One text operand, to be Z-encoded into the program */
#define LABEL    3    /* One operand, a label, given as long constant offset */
#define CALL     4    /* First operand is name of a routine, to be assembled
                         as long constant (the routine's packed address):
                         as if the name were prefixed by #r$ */

    /* Glulx: (bit flags for Glulx VM features) */

#define GOP_Unicode      1   /* uses_unicode_features */
#define GOP_MemHeap      2   /* uses_memheap_features */
#define GOP_Acceleration 4   /* uses_acceleration_features */
#define GOP_Float        8   /* uses_float_features */

    /* Codes for the number of operands */

#define TWO      1    /* 2 (with certain types of operand, compiled as VAR) */
#define VAR      2    /* 0 to 4 */
#define VAR_LONG 3    /* 0 to 8 */
#define ONE      4    /* 1 */
#define ZERO     5    /* 0 */
#define EXT      6    /* Extended opcode set VAR: 0 to 4 */
#define EXT_LONG 7    /* Extended: 0 to 8 (not used by the canonical opcodes) */

static opcodez opcodes_table_z[] =
{
    /* Opcodes introduced in Version 3 */

/* 0 */ { (uchar *) "je",              3, 0, -1, 0x01,     Br,      0, 0, TWO },
/* 1 */ { (uchar *) "jl",              3, 0, -1, 0x02,     Br,      0, 0, TWO },
/* 2 */ { (uchar *) "jg",              3, 0, -1, 0x03,     Br,      0, 0, TWO },
/* 3 */ { (uchar *) "dec_chk",         3, 0, -1, 0x04,     Br, VARIAB, 0, TWO },
/* 4 */ { (uchar *) "inc_chk",         3, 0, -1, 0x05,     Br, VARIAB, 0, TWO },
/* 5 */ { (uchar *) "jin",             3, 0, -1, 0x06,     Br,      0, 0, TWO },
/* 6 */ { (uchar *) "test",            3, 0, -1, 0x07,     Br,      0, 0, TWO },
/* 7 */ { (uchar *) "or",              3, 0, -1, 0x08,     St,      0, 0, TWO },
/* 8 */ { (uchar *) "and",             3, 0, -1, 0x09,     St,      0, 0, TWO },
/* 9 */ { (uchar *) "test_attr",       3, 0, -1, 0x0A,     Br,      0, 0, TWO },
/* 10 */ {(uchar *) "set_attr",        3, 0, -1, 0x0B,      0,      0, 0, TWO },
/* 11 */ {(uchar *) "clear_attr",      3, 0, -1, 0x0C,      0,      0, 0, TWO },
/* 12 */ {(uchar *) "store",           3, 0, -1, 0x0D,      0, VARIAB, 0, TWO },
/* 13 */ {(uchar *) "insert_obj",      3, 0, -1, 0x0E,      0,      0, 0, TWO },
/* 14 */ {(uchar *) "loadw",           3, 0, -1, 0x0F,     St,      0, 0, TWO },
/* 15 */ {(uchar *) "loadb",           3, 0, -1, 0x10,     St,      0, 0, TWO },
/* 16 */ {(uchar *) "get_prop",        3, 0, -1, 0x11,     St,      0, 0, TWO },
/* 17 */ {(uchar *) "get_prop_addr",   3, 0, -1, 0x12,     St,      0, 0, TWO },
/* 18 */ {(uchar *) "get_next_prop",   3, 0, -1, 0x13,     St,      0, 0, TWO },
/* 19 */ {(uchar *) "add",             3, 0, -1, 0x14,     St,      0, 0, TWO },
/* 20 */ {(uchar *) "sub",             3, 0, -1, 0x15,     St,      0, 0, TWO },
/* 21 */ {(uchar *) "mul",             3, 0, -1, 0x16,     St,      0, 0, TWO },
/* 22 */ {(uchar *) "div",             3, 0, -1, 0x17,     St,      0, 0, TWO },
/* 23 */ {(uchar *) "mod",             3, 0, -1, 0x18,     St,      0, 0, TWO },
/* 24 */ {(uchar *) "call",            3, 0, -1, 0x20,     St,   CALL, 0, VAR },
/* 25 */ {(uchar *) "storew",          3, 0, -1, 0x21,      0,      0, 0, VAR },
/* 26 */ {(uchar *) "storeb",          3, 0, -1, 0x22,      0,      0, 0, VAR },
/* 27 */ {(uchar *) "put_prop",        3, 0, -1, 0x23,      0,      0, 0, VAR },
            /* This is the version of "read" called "sread" internally: */
/* 28 */ {(uchar *) "read",            3, 0, -1, 0x24,      0,      0, 0, VAR },
/* 29 */ {(uchar *) "print_char",      3, 0, -1, 0x25,      0,      0, 0, VAR },
/* 30 */ {(uchar *) "print_num",       3, 0, -1, 0x26,      0,      0, 0, VAR },
/* 31 */ {(uchar *) "random",          3, 0, -1, 0x27,     St,      0, 0, VAR },
/* 32 */ {(uchar *) "push",            3, 0, -1, 0x28,      0,      0, 0, VAR },
/* 33 */ {(uchar *) "pull",            3, 5,  6, 0x29,      0, VARIAB, 0, VAR },
/* 34 */ {(uchar *) "split_window",    3, 0, -1, 0x2A,      0,      0, 0, VAR },
/* 35 */ {(uchar *) "set_window",      3, 0, -1, 0x2B,      0,      0, 0, VAR },
/* 36 */ {(uchar *) "output_stream",   3, 0, -1, 0x33,      0,      0, 0, VAR },
/* 37 */ {(uchar *) "input_stream",    3, 0, -1, 0x34,      0,      0, 0, VAR },
/* 38 */ {(uchar *) "sound_effect",    3, 0, -1, 0x35,      0,      0, 7, VAR },
/* 39 */ {(uchar *) "jz",              3, 0, -1, 0x00,     Br,      0, 0, ONE },
/* 40 */ {(uchar *) "get_sibling",     3, 0, -1, 0x01,  St+Br,      0, 0, ONE },
/* 41 */ {(uchar *) "get_child",       3, 0, -1, 0x02,  St+Br,      0, 0, ONE },
/* 42 */ {(uchar *) "get_parent",      3, 0, -1, 0x03,     St,      0, 0, ONE },
/* 43 */ {(uchar *) "get_prop_len",    3, 0, -1, 0x04,     St,      0, 0, ONE },
/* 44 */ {(uchar *) "inc",             3, 0, -1, 0x05,      0, VARIAB, 0, ONE },
/* 45 */ {(uchar *) "dec",             3, 0, -1, 0x06,      0, VARIAB, 0, ONE },
/* 46 */ {(uchar *) "print_addr",      3, 0, -1, 0x07,      0,      0, 0, ONE },
/* 47 */ {(uchar *) "remove_obj",      3, 0, -1, 0x09,      0,      0, 0, ONE },
/* 48 */ {(uchar *) "print_obj",       3, 0, -1, 0x0A,      0,      0, 0, ONE },
/* 49 */ {(uchar *) "ret",             3, 0, -1, 0x0B,     Rf,      0, 0, ONE },
/* 50 */ {(uchar *) "jump",            3, 0, -1, 0x0C,     Rf,  LABEL, 0, ONE },
/* 51 */ {(uchar *) "print_paddr",     3, 0, -1, 0x0D,      0,      0, 0, ONE },
/* 52 */ {(uchar *) "load",            3, 0, -1, 0x0E,     St, VARIAB, 0, ONE },
/* 53 */ {(uchar *) "not",             3, 3,  0, 0x0F,     St,      0, 0, ONE },
/* 54 */ {(uchar *) "rtrue",           3, 0, -1, 0x00,     Rf,      0, 0,ZERO },
/* 55 */ {(uchar *) "rfalse",          3, 0, -1, 0x01,     Rf,      0, 0,ZERO },
/* 56 */ {(uchar *) "print",           3, 0, -1, 0x02,      0,   TEXT, 0,ZERO },
/* 57 */ {(uchar *) "print_ret",       3, 0, -1, 0x03,     Rf,   TEXT, 0,ZERO },
/* 58 */ {(uchar *) "nop",             3, 0, -1, 0x04,      0,      0, 0,ZERO },
/* 59 */ {(uchar *) "save",            3, 3,  1, 0x05,     Br,      0, 0,ZERO },
/* 60 */ {(uchar *) "restore",         3, 3,  2, 0x06,     Br,      0, 0,ZERO },
/* 61 */ {(uchar *) "restart",         3, 0, -1, 0x07,      0,      0, 0,ZERO },
/* 62 */ {(uchar *) "ret_popped",      3, 0, -1, 0x08,     Rf,      0, 0,ZERO },
/* 63 */ {(uchar *) "pop",             3, 4, -1, 0x09,      0,      0, 0,ZERO },
/* 64 */ {(uchar *) "quit",            3, 0, -1, 0x0A,     Rf,      0, 0,ZERO },
/* 65 */ {(uchar *) "new_line",        3, 0, -1, 0x0B,      0,      0, 0,ZERO },
/* 66 */ {(uchar *) "show_status",     3, 3, -1, 0x0C,      0,      0, 0,ZERO },
/* 67 */ {(uchar *) "verify",          3, 0, -1, 0x0D,     Br,      0, 0,ZERO },

    /* Opcodes introduced in Version 4 */

/* 68 */ {(uchar *) "call_2s",         4, 0, -1, 0x19,     St,   CALL, 0, TWO },
/* 69 */ {(uchar *) "call_vs",         4, 0, -1, 0x20,     St,   CALL, 0, VAR },
            /* This is the version of "read" called "aread" internally: */
/* 70 */ {(uchar *) "read",            4, 0, -1, 0x24,     St,      0, 0, VAR },
/* 71 */ {(uchar *) "call_vs2",        4, 0, -1, 0x2C,     St,   CALL, 0,
                                                                     VAR_LONG },
/* 72 */ {(uchar *) "erase_window",    4, 0, -1, 0x2D,      0,      0, 0, VAR },
/* 73 */ {(uchar *) "erase_line",      4, 0, -1, 0x2E,      0,      0, 0, VAR },
/* 74 */ {(uchar *) "set_cursor",      4, 0, -1, 0x2F,      0,      0, 0, VAR },
/* 75 */ {(uchar *) "get_cursor",      4, 0, -1, 0x30,      0,      0, 0, VAR },
/* 76 */ {(uchar *) "set_text_style",  4, 0, -1, 0x31,      0,      0, 0, VAR },
/* 77 */ {(uchar *) "buffer_mode",     4, 0, -1, 0x32,      0,      0, 0, VAR },
/* 78 */ {(uchar *) "read_char",       4, 0, -1, 0x36,     St,      0, 0, VAR },
/* 79 */ {(uchar *) "scan_table",      4, 0, -1, 0x37,  St+Br,      0, 0, VAR },
/* 80 */ {(uchar *) "call_1s",         4, 0, -1, 0x08,     St,   CALL, 0, ONE },

    /* Opcodes introduced in Version 5 */

/* 81 */ {(uchar *) "call_2n",         5, 0, -1, 0x1a,      0,   CALL, 0, TWO },
/* 82 */ {(uchar *) "set_colour",      5, 0, -1, 0x1b,      0,      0, 6, TWO },
/* 83 */ {(uchar *) "throw",           5, 0, -1, 0x1c,      0,      0, 0, TWO },
/* 84 */ {(uchar *) "call_vn",         5, 0, -1, 0x39,      0,   CALL, 0, VAR },
/* 85 */ {(uchar *) "call_vn2",        5, 0, -1, 0x3a,      0,   CALL, 0,
                                                                     VAR_LONG },
/* 86 */ {(uchar *) "tokenise",        5, 0, -1, 0x3b,      0,      0, 0, VAR },
/* 87 */ {(uchar *) "encode_text",     5, 0, -1, 0x3c,      0,      0, 0, VAR },
/* 88 */ {(uchar *) "copy_table",      5, 0, -1, 0x3d,      0,      0, 0, VAR },
/* 89 */ {(uchar *) "print_table",     5, 0, -1, 0x3e,      0,      0, 0, VAR },
/* 90 */ {(uchar *) "check_arg_count", 5, 0, -1, 0x3f,     Br,      0, 0, VAR },
/* 91 */ {(uchar *) "call_1n",         5, 0, -1, 0x0F,      0,   CALL, 0, ONE },
/* 92 */ {(uchar *) "catch",           5, 0, -1, 0x09,     St,      0, 0, ZERO },
/* 93 */ {(uchar *) "piracy",          5, 0, -1, 0x0F,     Br,      0, 0, ZERO },
/* 94 */ {(uchar *) "log_shift",       5, 0, -1, 0x02,     St,      0, 0, EXT },
/* 95 */ {(uchar *) "art_shift",       5, 0, -1, 0x03,     St,      0, 0, EXT },
/* 96 */ {(uchar *) "set_font",        5, 0, -1, 0x04,     St,      0, 0, EXT },
/* 97 */ {(uchar *) "save_undo",       5, 0, -1, 0x09,     St,      0, 4, EXT },
/* 98 */ {(uchar *) "restore_undo",    5, 0, -1, 0x0A,     St,      0, 4, EXT },

    /* Opcodes introduced in Version 6 */

/* 99 */  { (uchar *) "draw_picture",  6, 6, -1, 0x05,      0,      0, 3, EXT },
/* 100 */ { (uchar *) "picture_data",  6, 6, -1, 0x06,     Br,      0, 3, EXT },
/* 101 */ { (uchar *) "erase_picture", 6, 6, -1, 0x07,      0,      0, 3, EXT },
/* 102 */ { (uchar *) "set_margins",   6, 6, -1, 0x08,      0,      0, 0, EXT },
/* 103 */ { (uchar *) "move_window",   6, 6, -1, 0x10,      0,      0, 0, EXT },
/* 104 */ { (uchar *) "window_size",   6, 6, -1, 0x11,      0,      0, 0, EXT },
/* 105 */ { (uchar *) "window_style",  6, 6, -1, 0x12,      0,      0, 0, EXT },
/* 106 */ { (uchar *) "get_wind_prop", 6, 6, -1, 0x13,     St,      0, 0, EXT },
/* 107 */ { (uchar *) "scroll_window", 6, 6, -1, 0x14,      0,      0, 0, EXT },
/* 108 */ { (uchar *) "pop_stack",     6, 6, -1, 0x15,      0,      0, 0, EXT },
/* 109 */ { (uchar *) "read_mouse",    6, 6, -1, 0x16,      0,      0, 5, EXT },
/* 110 */ { (uchar *) "mouse_window",  6, 6, -1, 0x17,      0,      0, 5, EXT },
/* 111 */ { (uchar *) "push_stack",    6, 6, -1, 0x18,     Br,      0, 0, EXT },
/* 112 */ { (uchar *) "put_wind_prop", 6, 6, -1, 0x19,      0,      0, 0, EXT },
/* 113 */ { (uchar *) "print_form",    6, 6, -1, 0x1a,      0,      0, 0, EXT },
/* 114 */ { (uchar *) "make_menu",     6, 6, -1, 0x1b,     Br,      0, 8, EXT },
/* 115 */ { (uchar *) "picture_table", 6, 6, -1, 0x1c,      0,      0, 3, EXT },

    /* Opcodes introduced in Z-Machine Specification Standard 1.0 */

/* 116 */ { (uchar *) "print_unicode", 5, 0, -1, 0x0b,      0,      0, 0, EXT },
/* 117 */ { (uchar *) "check_unicode", 5, 0, -1, 0x0c,     St,      0, 0, EXT }
};

    /* Subsequent forms for opcodes whose meaning changes with version */

static opcodez extension_table_z[] =
{
/* 0 */ { (uchar *) "not",             4, 4,  3, 0x0F,     St,      0, 0, ONE },
/* 1 */ { (uchar *) "save",            4, 4,  4, 0x05,     St,      0, 0,ZERO },
/* 2 */ { (uchar *) "restore",         4, 4,  5, 0x06,     St,      0, 0,ZERO },
/* 3 */ { (uchar *) "not",             5, 0, -1, 0x38,     St,      0, 0, VAR },
/* 4 */ { (uchar *) "save",            5, 0, -1, 0x00,     St,      0, 0, EXT },
/* 5 */ { (uchar *) "restore",         5, 0, -1, 0x01,     St,      0, 0, EXT },
/* 6 */ { (uchar *) "pull",            6, 6, -1, 0x29,     St,      0, 0, VAR }
};

static opcodez invalid_opcode_z =
        { (uchar *) "invalid",         0, 0, -1, 0xff,      0,      0, 0, ZERO};

static opcodez custom_opcode_z;

/* Note that this table assumes that all opcodes have at most two 
   branch-label or store operands, and that if they exist, they are the
   last operands. Glulx does not actually guarantee this. But it is
   true for all opcodes in the current Glulx spec, so we will assume
   it for now.

   Also note that Inform can only compile branches to constant offsets,
   even though the Glulx machine can handle stack or memory-loaded
   operands in a branch instruction.
*/

static opcodeg opcodes_table_g[] = {
  { (uchar *) "nop",        0x00,  0, 0, 0 },
  { (uchar *) "add",        0x10, St, 0, 3 },
  { (uchar *) "sub",        0x11, St, 0, 3 },
  { (uchar *) "mul",        0x12, St, 0, 3 },
  { (uchar *) "div",        0x13, St, 0, 3 },
  { (uchar *) "mod",        0x14, St, 0, 3 },
  { (uchar *) "neg",        0x15, St, 0, 2 },
  { (uchar *) "bitand",     0x18, St, 0, 3 },
  { (uchar *) "bitor",      0x19, St, 0, 3 },
  { (uchar *) "bitxor",     0x1A, St, 0, 3 },
  { (uchar *) "bitnot",     0x1B, St, 0, 2 },
  { (uchar *) "shiftl",     0x1C, St, 0, 3 },
  { (uchar *) "sshiftr",    0x1D, St, 0, 3 },
  { (uchar *) "ushiftr",    0x1E, St, 0, 3 },
  { (uchar *) "jump",       0x20, Br|Rf, 0, 1 },
  { (uchar *) "jz",     0x22, Br, 0, 2 },
  { (uchar *) "jnz",        0x23, Br, 0, 2 },
  { (uchar *) "jeq",        0x24, Br, 0, 3 },
  { (uchar *) "jne",        0x25, Br, 0, 3 },
  { (uchar *) "jlt",        0x26, Br, 0, 3 },
  { (uchar *) "jge",        0x27, Br, 0, 3 },
  { (uchar *) "jgt",        0x28, Br, 0, 3 },
  { (uchar *) "jle",        0x29, Br, 0, 3 },
  { (uchar *) "jltu",       0x2A, Br, 0, 3 },
  { (uchar *) "jgeu",       0x2B, Br, 0, 3 },
  { (uchar *) "jgtu",       0x2C, Br, 0, 3 },
  { (uchar *) "jleu",       0x2D, Br, 0, 3 },
  { (uchar *) "call",       0x30, St, 0, 3 },
  { (uchar *) "return",     0x31, Rf, 0, 1 },
  { (uchar *) "catch",      0x32, Br|St, 0, 2 },
  { (uchar *) "throw",      0x33, Rf, 0, 2 },
  { (uchar *) "tailcall",   0x34, Rf, 0, 2 },
  { (uchar *) "copy",       0x40, St, 0, 2 },
  { (uchar *) "copys",      0x41, St, 0, 2 },
  { (uchar *) "copyb",      0x42, St, 0, 2 },
  { (uchar *) "sexs",       0x44, St, 0, 2 },
  { (uchar *) "sexb",       0x45, St, 0, 2 },
  { (uchar *) "aload",      0x48, St, 0, 3 },
  { (uchar *) "aloads",     0x49, St, 0, 3 },
  { (uchar *) "aloadb",     0x4A, St, 0, 3 },
  { (uchar *) "aloadbit",   0x4B, St, 0, 3 },
  { (uchar *) "astore",     0x4C,  0, 0, 3 },
  { (uchar *) "astores",    0x4D,  0, 0, 3 },
  { (uchar *) "astoreb",    0x4E,  0, 0, 3 },
  { (uchar *) "astorebit",  0x4F,  0, 0, 3 },
  { (uchar *) "stkcount",   0x50, St, 0, 1 },
  { (uchar *) "stkpeek",    0x51, St, 0, 2 },
  { (uchar *) "stkswap",    0x52,  0, 0, 0 },
  { (uchar *) "stkroll",    0x53,  0, 0, 2 },
  { (uchar *) "stkcopy",    0x54,  0, 0, 1 },
  { (uchar *) "streamchar", 0x70,  0, 0, 1 },
  { (uchar *) "streamnum",  0x71,  0, 0, 1 },
  { (uchar *) "streamstr",  0x72,  0, 0, 1 },
  { (uchar *) "gestalt",    0x0100, St, 0, 3 },
  { (uchar *) "debugtrap",  0x0101, 0, 0, 1 },
  { (uchar *) "getmemsize",     0x0102, St, 0, 1 },
  { (uchar *) "setmemsize",     0x0103, St, 0, 2 },
  { (uchar *) "jumpabs",    0x0104, Rf, 0, 1 },
  { (uchar *) "random",     0x0110, St, 0, 2 },
  { (uchar *) "setrandom",  0x0111,  0, 0, 1 },
  { (uchar *) "quit",       0x0120, Rf, 0, 0 },
  { (uchar *) "verify",     0x0121, St, 0, 1 },
  { (uchar *) "restart",    0x0122,  0, 0, 0 },
  { (uchar *) "save",       0x0123, St, 0, 2 },
  { (uchar *) "restore",    0x0124, St, 0, 2 },
  { (uchar *) "saveundo",   0x0125, St, 0, 1 },
  { (uchar *) "restoreundo",    0x0126, St, 0, 1 },
  { (uchar *) "protect",    0x0127,  0, 0, 2 },
  { (uchar *) "glk",        0x0130, St, 0, 3 },
  { (uchar *) "getstringtbl",   0x0140, St, 0, 1 },
  { (uchar *) "setstringtbl",   0x0141, 0, 0, 1 },
  { (uchar *) "getiosys",   0x0148, St|St2, 0, 2 },
  { (uchar *) "setiosys",   0x0149, 0, 0, 2 },
  { (uchar *) "linearsearch",   0x0150, St, 0, 8 },
  { (uchar *) "binarysearch",   0x0151, St, 0, 8 },
  { (uchar *) "linkedsearch",   0x0152, St, 0, 7 },
  { (uchar *) "callf",      0x0160, St, 0, 2 },
  { (uchar *) "callfi",     0x0161, St, 0, 3 },
  { (uchar *) "callfii",    0x0162, St, 0, 4 },
  { (uchar *) "callfiii",   0x0163, St, 0, 5 },
  { (uchar *) "streamunichar", 0x73,  0, GOP_Unicode, 1 },
  { (uchar *) "mzero",      0x170,  0, GOP_MemHeap, 2 },
  { (uchar *) "mcopy",      0x171,  0, GOP_MemHeap, 3 },
  { (uchar *) "malloc",     0x178,  St, GOP_MemHeap, 2 },
  { (uchar *) "mfree",      0x179,  0, GOP_MemHeap, 1 },
  { (uchar *) "accelfunc",  0x180,  0, GOP_Acceleration, 2 },
  { (uchar *) "accelparam", 0x181,  0, GOP_Acceleration, 2 },
  { (uchar *) "numtof",     0x190,  St, GOP_Float, 2 },
  { (uchar *) "ftonumz",    0x191,  St, GOP_Float, 2 },
  { (uchar *) "ftonumn",    0x192,  St, GOP_Float, 2 },
  { (uchar *) "ceil",       0x198,  St, GOP_Float, 2 },
  { (uchar *) "floor",      0x199,  St, GOP_Float, 2 },
  { (uchar *) "fadd",       0x1A0,  St, GOP_Float, 3 },
  { (uchar *) "fsub",       0x1A1,  St, GOP_Float, 3 },
  { (uchar *) "fmul",       0x1A2,  St, GOP_Float, 3 },
  { (uchar *) "fdiv",       0x1A3,  St, GOP_Float, 3 },
  { (uchar *) "fmod",       0x1A4,  St|St2, GOP_Float, 4 },
  { (uchar *) "sqrt",       0x1A8,  St, GOP_Float, 2 },
  { (uchar *) "exp",        0x1A9,  St, GOP_Float, 2 },
  { (uchar *) "log",        0x1AA,  St, GOP_Float, 2 },
  { (uchar *) "pow",        0x1AB,  St, GOP_Float, 3 },
  { (uchar *) "sin",        0x1B0,  St, GOP_Float, 2 },
  { (uchar *) "cos",        0x1B1,  St, GOP_Float, 2 },
  { (uchar *) "tan",        0x1B2,  St, GOP_Float, 2 },
  { (uchar *) "asin",       0x1B3,  St, GOP_Float, 2 },
  { (uchar *) "acos",       0x1B4,  St, GOP_Float, 2 },
  { (uchar *) "atan",       0x1B5,  St, GOP_Float, 2 },
  { (uchar *) "atan2",      0x1B6,  St, GOP_Float, 3 },
  { (uchar *) "jfeq",       0x1C0,  Br, GOP_Float, 4 },
  { (uchar *) "jfne",       0x1C1,  Br, GOP_Float, 4 },
  { (uchar *) "jflt",       0x1C2,  Br, GOP_Float, 3 },
  { (uchar *) "jfle",       0x1C3,  Br, GOP_Float, 3 },
  { (uchar *) "jfgt",       0x1C4,  Br, GOP_Float, 3 },
  { (uchar *) "jfge",       0x1C5,  Br, GOP_Float, 3 },
  { (uchar *) "jisnan",     0x1C8,  Br, GOP_Float, 2 },
  { (uchar *) "jisinf",     0x1C9,  Br, GOP_Float, 2 },
};

/* The opmacros table is used for fake opcodes. The opcode numbers are
   ignored; this table is only used for argument parsing. */
static opcodeg opmacros_table_g[] = {
  { (uchar *) "pull", 0, St, 0, 1 },
  { (uchar *) "push", 0,  0, 0, 1 },
};

static opcodeg custom_opcode_g;

static opcodew opcodes_table_w[] = {
  /* 00 */ {(uchar *) "unreachable", 0x00, Bl|Rf, 0},
  /* 01 */ {(uchar *) "nop",         0x01, 0,     0},
  /* 02 */ {(uchar *) "block",       0x02, Bl,    1},
  /* 03 */ {(uchar *) "loop",        0x03, Bl,    1},
  /* 04 */ {(uchar *) "if" ,         0x04, 0,     1},
  /* 05 */ {(uchar *) "else",        0x05, Bl,    0},
  /* 06 */ {0, 0, 0, 0},
  /* 07 */ {0, 0, 0, 0},
  /* 08 */ {0, 0, 0, 0},
  /* 09 */ {0, 0, 0, 0},
  /* 10 */ {0, 0, 0, 0},
  /* 11 */ {(uchar *) "end",       0x0b, Bl, 0},
  /* 12 */ {(uchar *) "br",        0x0c, Rf, 1},
  /* 13 */ {(uchar *) "br_if",     0x0d, 0,  1},
  /* 14 */ {(uchar *) "br_table",  0x0e, Rf, 0},
  /* 15 */ {(uchar *) "return",    0x0f, Rf, 0},
  /* 16 */ {(uchar *) "call",      0x10, 0,  1},
  /* 17 */ {(uchar *) "drop",      0x1a, 0,  0},
  /* 18 */ {(uchar *) "local.get", 0x20, 0,  1},
  /* 19 */ {(uchar *) "local.set", 0x21, 0,  1},
  /* 20 */ {(uchar *) "local.tee", 0x22, 0,  1},
  /* 21 */ {(uchar *) "i32.store", 0x36, 0,  2},
  /* 22 */ {(uchar *) "i32.const", 0x41, 0,  1},
  /* 23 */ {(uchar *) "i32.eqz",   0x45, 0,  0},
  /* 24 */ {(uchar *) "i32.eq",    0x46, 0,  0},
  /* 25 */ {(uchar *) "i32.ne",    0x47, 0,  0},
  /* 26 */ {(uchar *) "i32.lt_s",  0x48, 0,  0},
  /* 27 */ {(uchar *) "i32.lt_u",  0x49, 0,  0},
  /* 28 */ {(uchar *) "i32.gt_s",  0x4a, 0,  0},
  /* 29 */ {(uchar *) "i32.gt_u",  0x4b, 0,  0},
  /* 30 */ {(uchar *) "i32.le_s",  0x4c, 0,  0},
  /* 31 */ {(uchar *) "i32.le_u",  0x4d, 0,  0},
  /* 32 */ {(uchar *) "i32.ge_s",  0x4e, 0,  0},
  /* 33 */ {(uchar *) "i32.ge_u",  0x4f, 0,  0},
  /* 34 */ {(uchar *) "i32.add",   0x6a, 0,  0},
  /* 35 */ {(uchar *) "i32.sub",   0x6b, 0,  0},
  /* 36 */ {(uchar *) "i32.mul",   0x6c, 0,  0},
  /* 37 */ {(uchar *) "i32.div_s", 0x6d, 0,  0},
  /* 38 */ {(uchar *) "i32.div_u", 0x6e, 0,  0},
  /* 39 */ {(uchar *) "i32.rem_s", 0x6f, 0,  0},
  /* 40 */ {(uchar *) "i32.rem_u", 0x70, 0,  0},
  /* 41 */ {(uchar *) "i32.and",   0x71, 0,  0},
  /* 42 */ {(uchar *) "i32.or",    0x72, 0,  0},
  /* 43 */ {(uchar *) "i32.xor",   0x73, 0,  0},
};

static opcodez internal_number_to_opcode_z(int32 i)
{   opcodez x;
    ASSERT_ZCODE();
    if (i == -1) return custom_opcode_z;
    x = opcodes_table_z[i];
    if (instruction_set_number < x.version1) return invalid_opcode_z;
    if (x.version2 == 0) return x;
    if (instruction_set_number <= x.version2) return x;
    i = x.extension;
    if (i < 0) return invalid_opcode_z;
    x = extension_table_z[i];
    if (instruction_set_number < x.version1) return invalid_opcode_z;
    if (x.version2 == 0) return x;
    if (instruction_set_number <= x.version2) return x;
    return extension_table_z[x.extension];
}

static void make_opcode_syntax_z(opcodez opco)
{   char *p = "", *q = opcode_syntax_string;
    sprintf(q, "%s", opco.name);
    switch(opco.no)
    {   case ONE: p=" <operand>"; break;
        case TWO: p=" <operand1> <operand2>"; break;
        case EXT:
        case VAR: p=" <0 to 4 operands>"; break;
        case VAR_LONG: p=" <0 to 8 operands>"; break;
    }
    switch(opco.op_rules)
    {   case TEXT: sprintf(q+strlen(q), " <text>"); return;
        case LABEL: sprintf(q+strlen(q), " <label>"); return;
        case VARIAB:
            sprintf(q+strlen(q), " <variable>");
        case CALL:
            if (opco.op_rules==CALL) sprintf(q+strlen(q), " <routine>");
            switch(opco.no)
            {   case ONE: p=""; break;
                case TWO: p=" <operand>"; break;
                case EXT:
                case VAR: p=" <1 to 4 operands>"; break;
                case VAR_LONG: p=" <1 to 8 operands>"; break;
            }
            break;
    }
    sprintf(q+strlen(q), "%s", p);
    if ((opco.flags & St) != 0) sprintf(q+strlen(q), " -> <result-variable>");
    if ((opco.flags & Br) != 0) sprintf(q+strlen(q), " ?[~]<label>");
}

static opcodeg internal_number_to_opcode_g(int32 i)
{   
    opcodeg x;
    if (i == -1) return custom_opcode_g;
    x = opcodes_table_g[i];
    return x;
}

static opcodeg internal_number_to_opmacro_g(int32 i)
{
    return opmacros_table_g[i];
}

static void make_opcode_syntax_g(opcodeg opco)
{
    int ix;
    char *cx;
    char *q = opcode_syntax_string;

    sprintf(q, "%s", opco.name);
    sprintf(q+strlen(q), " <%d operand%s", opco.no,
        ((opco.no==1) ? "" : "s"));
    if (opco.no) {
        cx = q+strlen(q);
        strcpy(cx, ": ");
        cx += strlen(cx);
        for (ix=0; ix<opco.no; ix++) {
            if (ix) {
                *cx = ' ';
                cx++;
            }
            if (ix == opco.no-1) {
                if (opco.flags & Br) {
                    strcpy(cx, "Lb");
                }
                else if (opco.flags & St) {
                    strcpy(cx, "S");
                }
                else {
                    strcpy(cx, "L");
                }
            }
            else if (ix == opco.no-2 && (opco.flags & Br) && (opco.flags & St)) {
                strcpy(cx, "S");
            }
            else if (ix == opco.no-2 && (opco.flags & St2)) {
                strcpy(cx, "S");
            }
            else {
                strcpy(cx, "L");
            }
            cx += strlen(cx);
            sprintf(cx, "%d", ix+1);
            cx += strlen(cx);
        }
    }
    sprintf(q+strlen(q), ">");
}

static opcodew internal_number_to_opcode_w(int32 i)
{   
    opcodew x;
    x = opcodes_table_w[i];
    return x;
}



/* ========================================================================= */
/*   The assembler itself does four things:                                  */
/*                                                                           */
/*       assembles instructions                                              */
/*       sets label N to the current code position                           */
/*       assembles routine headers                                           */
/*       assembles routine ends                                              */
/* ------------------------------------------------------------------------- */

/* This is for Z-code only. */
static void write_operand(assembly_operand op)
{   int32 j;
    if (module_switch && (op.marker != 0))
    {   if ((op.marker != VARIABLE_MV) && (op.type == SHORT_CONSTANT_OT))
            op.type = LONG_CONSTANT_OT;
    }
    j=op.value;
    switch(op.type)
    {   case LONG_CONSTANT_OT:
            byteout(j/256, op.marker); byteout(j%256, 0); return;
        case SHORT_CONSTANT_OT:
            if (op.marker == 0)
            byteout(j, 0);
            else byteout(j, 0x80 + op.marker); return;
        case VARIABLE_OT:
            byteout(j, (module_switch)?(0x80 + op.marker):0); return;
        case CONSTANT_OT:
        case HALFCONSTANT_OT:
        case BYTECONSTANT_OT:
        case ZEROCONSTANT_OT:
        case SYSFUN_OT:
        case DEREFERENCE_OT:
        case LOCALVAR_OT:
        case GLOBALVAR_OT:
            compiler_error("Glulx OT in Z-code assembly operand.");
            return;
    }
}

extern void assemblez_instruction(assembly_instruction *AI)
{
    uchar *start_pc, *operands_pc;
    int32 offset, j, topbits=0, types_byte1, types_byte2;
    int operand_rules, min=0, max=0, no_operands_given, at_seq_point = FALSE;
    assembly_operand o1, o2;
    opcodez opco;

    ASSERT_ZCODE();

    offset = zmachine_pc;

    no_instructions++;

    if (veneer_mode) sequence_point_follows = FALSE;
    if (sequence_point_follows)
    {   sequence_point_follows = FALSE; at_seq_point = TRUE;
        if (debugfile_switch)
        {   sequence_point_labels[next_sequence_point] = next_label;
            sequence_point_locations[next_sequence_point] =
                statement_debug_location;
            set_label_offset(next_label++, zmachine_pc);
        }
        next_sequence_point++;
    }

    opco = internal_number_to_opcode_z(AI->internal_number);
    if (opco.version1==0)
    {   error_named("Opcode unavailable in this Z-machine version",
            opcode_names.keywords[AI->internal_number]);
        return;
    }

    if (execution_never_reaches_here)
        warning("This statement can never be reached");

    operand_rules = opco.op_rules;
    execution_never_reaches_here = ((opco.flags & Rf) != 0);

    if (opco.flags2_set != 0) flags2_requirements[opco.flags2_set] = 1;

    no_operands_given = AI->operand_count;

    if ((opco.no == TWO) && ((no_operands_given==3)||(no_operands_given==4)))
        opco.no = VAR;

    /* 1. Write the opcode byte(s) */

    start_pc = zcode_holding_area + zcode_ha_size;

    switch(opco.no)
    {   case VAR_LONG: topbits=0xc0; min=0; max=8; break;
        case VAR:      topbits=0xc0; min=0; max=4; break;
        case ZERO:     topbits=0xb0; min=0; max=0; break;
        case ONE:      topbits=0x80; min=1; max=1; break;
        case TWO:      topbits=0x00; min=2; max=2; break;
        case EXT:      topbits=0x00; min=0; max=4;
                       byteout(0xbe, 0); opco.no=VAR; break;
        case EXT_LONG: topbits=0x00; min=0; max=8;
                       byteout(0xbe, 0); opco.no=VAR_LONG; break;
    }
    byteout(opco.code + topbits, 0);

    operands_pc = zcode_holding_area + zcode_ha_size;

    /* 2. Dispose of the special rules LABEL and TEXT */

    if (operand_rules==LABEL)
    {   j = (AI->operand[0]).value;
        byteout(j/256, LABEL_MV); byteout(j%256, 0);
        goto Instruction_Done;
    }

    if (operand_rules==TEXT)
    {   int32 i;
        uchar *tmp = translate_text(zcode_holding_area + zcode_ha_size, zcode_holding_area+MAX_ZCODE_SIZE, AI->text, STRCTX_GAMEOPC);
        if (!tmp)
            memoryerror("MAX_ZCODE_SIZE", MAX_ZCODE_SIZE);
        j = subtract_pointers(tmp, (zcode_holding_area + zcode_ha_size));
        for (i=0; i<j; i++) zcode_markers[zcode_ha_size++] = 0;
        zmachine_pc += j;
        goto Instruction_Done;
    }

    /* 3. Sort out the operands */

    if ((no_operands_given < min) || (no_operands_given > max))
        goto OpcodeSyntaxError;

    switch(opco.no)
    {   case VAR:
        case VAR_LONG:
            byteout(0, 0);
            if (opco.no == VAR_LONG) byteout(0, 0);
            types_byte1=0xff; types_byte2=0xff;
            for (j=0; j<no_operands_given; j++)
            {   int multi=0, mask=0;
                switch(j)
                {   case 0: case 4: multi=0x40; mask=0xc0; break;
                    case 1: case 5: multi=0x10; mask=0x30; break;
                    case 2: case 6: multi=0x04; mask=0x0c; break;
                    case 3: case 7: multi=0x01; mask=0x03; break;
                }
                o1 = AI->operand[j];
                write_operand(o1);
                if (j<4)
                    types_byte1 = (types_byte1 & (~mask)) + o1.type*multi;
                else
                    types_byte2 = (types_byte2 & (~mask)) + o1.type*multi;
            }
            *operands_pc=types_byte1;
            if (opco.no == VAR_LONG) *(operands_pc+1)=types_byte2;
            break;

        case ONE:
            o1 = AI->operand[0];
            *start_pc=(*start_pc) + o1.type*0x10;
            write_operand(o1);
            break;

        case TWO:
            o1 = AI->operand[0];
            o2 = AI->operand[1];

            /* Transfer to VAR form if either operand is a long constant */

            if ((o1.type==LONG_CONSTANT_OT)||(o2.type==LONG_CONSTANT_OT))
            {   *start_pc=(*start_pc) + 0xc0;
                byteout(o1.type*0x40 + o2.type*0x10 + 0x0f, 0);
            }
            else
            {   if (o1.type==VARIABLE_OT) *start_pc=(*start_pc) + 0x40;
                if (o2.type==VARIABLE_OT) *start_pc=(*start_pc) + 0x20;
            }
            write_operand(o1);
            write_operand(o2);
            break;
    }

    /* 4. Assemble a Store destination, if needed */

    if ((AI->store_variable_number) != -1)
    {   if (AI->store_variable_number >= MAX_LOCAL_VARIABLES+MAX_ZCODE_GLOBAL_VARS) {
            goto OpcodeSyntaxError;
        }
        o1.type = VARIABLE_OT;
        o1.value = AI->store_variable_number;
        variables[o1.value].usage = TRUE;
        o1.marker = 0;

        /*  Note that variable numbers 249 to 255 (i.e. globals 233 to 239)
            are used as scratch workspace, so need no mapping between
            modules and story files: nor do local variables 0 to 15  */

        if ((o1.value >= MAX_LOCAL_VARIABLES) && (o1.value < 249))
            o1.marker = VARIABLE_MV;
        write_operand(o1);
    }

    /* 5. Assemble a branch, if needed */

    if (AI->branch_label_number != -1)
    {   int32 addr, long_form;
        int branch_on_true = (AI->branch_flag)?1:0;

        switch (AI->branch_label_number)
        {   case -2: addr = 2; branch_on_true = 0; long_form = 0; break;
                                                 /* branch nowhere, carry on */
            case -3: addr = 0; long_form = 0; break;  /* rfalse on condition */
            case -4: addr = 1; long_form = 0; break;  /* rtrue on condition */
            default:
                long_form = 1; addr = AI->branch_label_number;
                break;
        }
        if (addr > 0x7fff) fatalerror("Too many branch points in routine.");
        if (long_form==1)
        {   byteout(branch_on_true*0x80 + addr/256, BRANCH_MV);
            byteout(addr%256, 0);
        }
        else
            byteout(branch_on_true*0x80+ 0x40 + (addr&0x3f), 0);
    }

    Instruction_Done:

    if (asm_trace_level > 0)
    {   int i;
        printf("%5d  +%05lx %3s %-12s ", ErrorReport.line_number,
            ((long int) offset),
            (at_seq_point)?"<*>":"   ", opco.name);

        if ((AI->internal_number == print_zc)
            || (AI->internal_number == print_ret_zc))
        {   printf("\"");
            for (i=0;(AI->text)[i]!=0 && i<35; i++) printf("%c",(AI->text)[i]);
            if (i == 35) printf("...");
            printf("\"");
        }

        for (i=0; i<AI->operand_count; i++)
        {   if ((i==0) && (opco.op_rules == VARIAB))
            {   if ((AI->operand[0]).type == VARIABLE_OT)
                {   printf("["); print_operand_z(&AI->operand[i], TRUE); }
                else
                    printf("%s", variable_name((AI->operand[0]).value));
            }
            else
            if ((i==0) && (opco.op_rules == LABEL))
            {   printf("L%d", AI->operand[0].value);
            }
            else print_operand_z(&AI->operand[i], TRUE);
            printf(" ");
        }
        if (AI->store_variable_number != -1)
        {   assembly_operand AO;
            printf("-> ");
            AO.type = VARIABLE_OT; AO.value = AI->store_variable_number;
            print_operand_z(&AO, TRUE); printf(" ");
        }

        switch(AI->branch_label_number)
        {   case -4: printf("rtrue if %s", (AI->branch_flag)?"TRUE":"FALSE");
                break;
            case -3: printf("rfalse if %s", (AI->branch_flag)?"TRUE":"FALSE");
                break;
            case -2: printf("(no branch)"); break;
            case -1: break;
            default:
                printf("to L%d if %s", AI->branch_label_number,
                   (AI->branch_flag)?"TRUE":"FALSE"); break;
        }

        if (asm_trace_level>=2)
        {   for (j=0;start_pc<zcode_holding_area + zcode_ha_size;
                 j++, start_pc++)
            {   if (j%16==0) printf("\n                               ");
                printf("%02x ", *start_pc);
            }
        }
        printf("\n");
    }

    if (module_switch) flush_link_data();

    return;

    OpcodeSyntaxError:

    make_opcode_syntax_z(opco);
    error_named("Assembly mistake: syntax is", opcode_syntax_string);
}

static void assembleg_macro(assembly_instruction *AI)
{
    /* validate macro syntax first */
    int ix, no_operands_given;
    opcodeg opco;
    
    opco = internal_number_to_opmacro_g(AI->internal_number);
    no_operands_given = AI->operand_count;
    
    if (no_operands_given != opco.no)
        goto OpcodeSyntaxError;
    
    for (ix = 0; ix < no_operands_given; ix++) {
        int type = AI->operand[ix].type;
        if ((opco.flags & St) 
          && ((!(opco.flags & Br) && (ix == no_operands_given-1))
          || ((opco.flags & Br) && (ix == no_operands_given-2)))) {
            if (is_constant_ot(type)) {
                error("*** assembly macro tried to store to a constant ***");
                goto OpcodeSyntaxError; 
            }
        }
        if ((opco.flags & St2) 
            && (ix == no_operands_given-2)) {
            if (is_constant_ot(type)) {
              error("*** assembly macro tried to store to a constant ***");
              goto OpcodeSyntaxError; 
            }
        }
    }
    
    /* expand the macro */
    switch (AI->internal_number) {
        case pull_gm:
            assembleg_store(AI->operand[0], stack_pointer);
            break;
        
        case push_gm:
            assembleg_store(stack_pointer, AI->operand[0]);
            break;
        
        default:
            compiler_error("Invalid Glulx assembly macro");
            break;
    }
    
    return;
    
    OpcodeSyntaxError:
    
    make_opcode_syntax_g(opco);
    error_named("Assembly mistake: syntax is", opcode_syntax_string);
}



extern void assembleg_instruction(assembly_instruction *AI)
{
    uchar *start_pc, *opmodes_pc;
    int32 offset, j;
    int no_operands_given, at_seq_point = FALSE;
    int ix, k;
    opcodeg opco;

    ASSERT_GLULX();

    offset = zmachine_pc;

    no_instructions++;

    if (veneer_mode) sequence_point_follows = FALSE;
    if (sequence_point_follows)
    {   sequence_point_follows = FALSE; at_seq_point = TRUE;
        if (debugfile_switch)
        {   sequence_point_labels[next_sequence_point] = next_label;
            sequence_point_locations[next_sequence_point] =
                statement_debug_location;
            set_label_offset(next_label++, zmachine_pc);
        }
        next_sequence_point++;
    }

    opco = internal_number_to_opcode_g(AI->internal_number);

    if (execution_never_reaches_here)
        warning("This statement can never be reached");

    execution_never_reaches_here = ((opco.flags & Rf) != 0);

    if (opco.op_rules & GOP_Unicode) {
        uses_unicode_features = TRUE;
    }
    if (opco.op_rules & GOP_MemHeap) {
        uses_memheap_features = TRUE;
    }
    if (opco.op_rules & GOP_Acceleration) {
        uses_acceleration_features = TRUE;
    }
    if (opco.op_rules & GOP_Float) {
        uses_float_features = TRUE;
    }

    no_operands_given = AI->operand_count;

    /* 1. Write the opcode byte(s) */

    start_pc = zcode_holding_area + zcode_ha_size; 

    if (opco.code < 0x80) {
      byteout(opco.code, 0);
    }
    else if (opco.code < 0x4000) {
      byteout(((opco.code >> 8) & 0xFF) | 0x80, 0);
      byteout((opco.code & 0xFF), 0);
    }
    else {
      byteout(((opco.code >> 24) & 0xFF) | 0xC0, 0);
      byteout(((opco.code >> 16) & 0xFF), 0);
      byteout(((opco.code >> 8) & 0xFF), 0);
      byteout(((opco.code) & 0xFF), 0);
    }

    /* ... and the operand addressing modes. There's one byte for
       every two operands (rounded up). We write zeroes for now; 
       when the operands are written, we'll go back and fix them. */

    opmodes_pc = zcode_holding_area + zcode_ha_size;

    for (ix=0; ix<opco.no; ix+=2) {
      byteout(0, 0);
    }

    /* 2. Dispose of the special rules */
    /* There aren't any in Glulx. */

    /* 3. Sort out the operands */

    if (no_operands_given != opco.no) {
      goto OpcodeSyntaxError;
    }

    for (ix=0; ix<no_operands_given; ix++) {
        int marker = AI->operand[ix].marker;
        int type = AI->operand[ix].type;
        k = AI->operand[ix].value;

        if ((opco.flags & Br) && (ix == no_operands_given-1)) {
            if (!(marker >= BRANCH_MV && marker < BRANCHMAX_MV)) {
                compiler_error("Assembling branch without BRANCH_MV marker");
                goto OpcodeSyntaxError; 
            }
            if (k == -2) {
                k = 2; /* branch no-op */
                type = BYTECONSTANT_OT;
                marker = 0;
            }
            else if (k == -3) {
                k = 0; /* branch return 0 */
                type = ZEROCONSTANT_OT;
                marker = 0;
            }
            else if (k == -4) {
                k = 1; /* branch return 1 */
                type = BYTECONSTANT_OT;
                marker = 0;
            }
            else {
                /* branch to label k */
                j = subtract_pointers((zcode_holding_area + zcode_ha_size), 
                    opmodes_pc);
                j = 2*j - ix;
                marker = BRANCH_MV + j;
                if (!(marker >= BRANCH_MV && marker < BRANCHMAX_MV)) {
                    error("*** branch marker too far from opmode byte ***");
                    goto OpcodeSyntaxError; 
                }
            }
        }
    if ((opco.flags & St) 
      && ((!(opco.flags & Br) && (ix == no_operands_given-1))
      || ((opco.flags & Br) && (ix == no_operands_given-2)))) {
        if (type == BYTECONSTANT_OT || type == HALFCONSTANT_OT
            || type == CONSTANT_OT) {
            error("*** instruction tried to store to a constant ***");
            goto OpcodeSyntaxError; 
        }
    }
    if ((opco.flags & St2) 
        && (ix == no_operands_given-2)) {
        if (type == BYTECONSTANT_OT || type == HALFCONSTANT_OT
          || type == CONSTANT_OT) {
          error("*** instruction tried to store to a constant ***");
          goto OpcodeSyntaxError; 
        }
    }

      if (marker && (type == HALFCONSTANT_OT 
        || type == BYTECONSTANT_OT
        || type == ZEROCONSTANT_OT)) {
        compiler_error("Assembling marker in less than 32-bit constant.");
        /* Actually we should store marker|0x80 for a byte constant,
           but let's hold off on that. */
        }

      switch (type) {
      case LONG_CONSTANT_OT:
      case SHORT_CONSTANT_OT:
      case VARIABLE_OT:
        j = 0;
        compiler_error("Z-code OT in Glulx assembly operand.");
        break;
      case CONSTANT_OT:
        j = 3;
        byteout((k >> 24) & 0xFF, marker);
        byteout((k >> 16) & 0xFF, 0);
        byteout((k >> 8) & 0xFF, 0);
        byteout((k & 0xFF), 0);
        break;
      case HALFCONSTANT_OT:
        j = 2;
        byteout((k >> 8) & 0xFF, marker);
        byteout((k & 0xFF), 0);
        break;
      case BYTECONSTANT_OT:
        j = 1;
        byteout((k & 0xFF), marker);
        break;
      case ZEROCONSTANT_OT:
        j = 0;
        break;
      case DEREFERENCE_OT:
        j = 7;
        byteout((k >> 24) & 0xFF, marker);
        byteout((k >> 16) & 0xFF, 0);
        byteout((k >> 8) & 0xFF, 0);
        byteout((k & 0xFF), 0);
        break;
      case GLOBALVAR_OT:
        /* Global variable -- a constant address. */
        k -= MAX_LOCAL_VARIABLES;
        if (/* DISABLES CODE */ (0)) {
            /* We could write the value as a marker and patch it later... */
            j = 7;
            byteout(((k) >> 24) & 0xFF, VARIABLE_MV);
            byteout(((k) >> 16) & 0xFF, 0);
            byteout(((k) >> 8) & 0xFF, 0);
            byteout(((k) & 0xFF), 0);
        }
        else {
            /* ...but it's more efficient to write it as a RAM operand,
                  which can be 1, 2, or 4 bytes. Remember that global variables
                  are the very first thing in RAM. */
            k = k * 4; /* each variable is four bytes */
            if (k <= 255) {
                j = 13;
                byteout(((k) & 0xFF), 0);
            }
            else if (k <= 65535) {
                j = 14;
                byteout(((k) >> 8) & 0xFF, 0);
                byteout(((k) & 0xFF), 0);
            }
            else {
                j = 15;
                byteout(((k) >> 24) & 0xFF, 0);
                byteout(((k) >> 16) & 0xFF, 0);
                byteout(((k) >> 8) & 0xFF, 0);
                byteout(((k) & 0xFF), 0);       
            }
        }
        break;
      case LOCALVAR_OT:
        if (k == 0) {
            /* Stack-pointer magic variable */
            j = 8; 
        }
        else {
            /* Local variable -- a byte or short offset from the
               frame pointer. It's an unsigned offset, so we can
               fit up to long 63 (offset 4*63) in a byte. */
            if ((k-1) < 64) {
                j = 9;
                byteout((k-1)*4, 0);
            }
            else {
                j = 10;
                byteout((((k-1)*4) >> 8) & 0xFF, 0);
                byteout(((k-1)*4) & 0xFF, 0);
            }
        }
        break;
      default:
        j = 0;
        break;
      }

      if (ix & 1)
          j = (j << 4);
      opmodes_pc[ix/2] |= j;
    }

    /* Print assembly trace. */
    if (asm_trace_level > 0) {
      int i;
      printf("%5d  +%05lx %3s %-12s ", ErrorReport.line_number,
        ((long int) offset),
        (at_seq_point)?"<*>":"   ", opco.name);
      for (i=0; i<AI->operand_count; i++) {
          if ((opco.flags & Br) && (i == opco.no-1)) {
            if (AI->operand[i].value == -4)
                printf("to rtrue");
            else if (AI->operand[i].value == -3)
                printf("to rfalse");
            else
                printf("to L%d", AI->operand[i].value);
            }
          else {
            print_operand_g(&AI->operand[i], TRUE);
          }
          printf(" ");
      }

      if (asm_trace_level>=2) {
        for (j=0;
            start_pc<zcode_holding_area + zcode_ha_size;
            j++, start_pc++) {
            if (j%16==0) printf("\n                               ");
            if (/* DISABLES CODE */ (0)) {
                printf("%02x ", *start_pc);
            }
            else {
                printf("%02x", *start_pc);
                if (zcode_markers[start_pc-zcode_holding_area])
                    printf("{%02x}", zcode_markers[start_pc-zcode_holding_area]);
                printf(" ");
            }
        }
      }
      printf("\n");
    }

    if (module_switch) flush_link_data();

    return;

    OpcodeSyntaxError:

    make_opcode_syntax_g(opco);
    error_named("Assembly mistake: syntax is", opcode_syntax_string);
}


extern void assemblew_instruction(assembly_instruction *AI)
{
    uchar *start_pc, *opmodes_pc;
    int32 offset, j;
    int no_operands_given, at_seq_point = FALSE;
    int ix, k;
    opcodew opco;

    offset = zmachine_pc;

    no_instructions++;

    if (veneer_mode) sequence_point_follows = FALSE;
    if (sequence_point_follows)
    {   sequence_point_follows = FALSE; at_seq_point = TRUE;
        if (debugfile_switch)
        {   sequence_point_labels[next_sequence_point] = next_label;
            sequence_point_locations[next_sequence_point] =
                statement_debug_location;
            set_label_offset(next_label++, zmachine_pc);
        }
        next_sequence_point++;
    }

    opco = internal_number_to_opcode_w(AI->internal_number);

    if (opco.flags & Bl) {
      if (opco.flags & Rf)
        execution_never_reaches_here = TRUE; 
    } else {
      if (execution_never_reaches_here)
          warning("This statement can never be reached");

      execution_never_reaches_here = ((opco.flags & Rf) != 0);
    }

    no_operands_given = AI->operand_count;

    /* 1. Write the opcode byte(s) */

    start_pc = zcode_holding_area + zcode_ha_size; 

    byteout(opco.code, 0);

    //for (ix=0; ix<opco.no; ix+=2) {
    //  byteout(0, 0);
    //}

    /* 2. Dispose of the special rules */
    /* There aren't any in Glulx. */

    /* 3. Sort out the operands */

    if (no_operands_given != opco.no) {
      goto OpcodeSyntaxError;
    }

    for (ix=0; ix<no_operands_given; ix++) {
        int marker = AI->operand[ix].marker;
        int type = AI->operand[ix].type;
        k = AI->operand[ix].value;

	byteout(k & 0x7f, marker); //WSTUB LEB128
        /*if ((opco.flags & Br) && (ix == no_operands_given-1)) {
            if (!(marker >= BRANCH_MV && marker < BRANCHMAX_MV)) {
                compiler_error("Assembling branch without BRANCH_MV marker");
                goto OpcodeSyntaxError; 
            }*/
            /*if (k == -2) {
                k = 2; */ /* branch no-op */
                //type = BYTECONSTANT_OT;
               // marker = 0;
            /*}
            else if (k == -3) {
                k = 0;*/ /* branch return 0 */
               // type = ZEROCONSTANT_OT;
               // marker = 0;
            /*}
            else if (k == -4) {
                k = 1; */  /* branch return 1 */
               // type = BYTECONSTANT_OT;
               // marker = 0;
            /*}
            else {*/
                /* branch to label k */
               /* j = subtract_pointers((zcode_holding_area + zcode_ha_size), 
                    opmodes_pc);
                j = 2*j - ix;
                marker = BRANCH_MV + j;
                if (!(marker >= BRANCH_MV && marker < BRANCHMAX_MV)) {
                    error("*** branch marker too far from opmode byte ***");
                    goto OpcodeSyntaxError; 
                }*/
            }
       // }
    /*if ((opco.flags & St) 
      && ((!(opco.flags & Br) && (ix == no_operands_given-1))
      || ((opco.flags & Br) && (ix == no_operands_given-2)))) {
        if (type == BYTECONSTANT_OT || type == HALFCONSTANT_OT
            || type == CONSTANT_OT) {
            error("*** instruction tried to store to a constant ***");
            goto OpcodeSyntaxError; 
        }
    }
    if ((opco.flags & St2) 
        && (ix == no_operands_given-2)) {
        if (type == BYTECONSTANT_OT || type == HALFCONSTANT_OT
          || type == CONSTANT_OT) {
          error("*** instruction tried to store to a constant ***");
          goto OpcodeSyntaxError; 
        }
    }

      if (marker && (type == HALFCONSTANT_OT 
        || type == BYTECONSTANT_OT
        || type == ZEROCONSTANT_OT)) {
        compiler_error("Assembling marker in less than 32-bit constant.");
        }

      switch (type) {
      case LONG_CONSTANT_OT:
      case SHORT_CONSTANT_OT:
      case VARIABLE_OT:
        j = 0;
        compiler_error("Z-code OT in Glulx assembly operand.");
        break;
      case CONSTANT_OT:
        j = 3;
        byteout((k >> 24) & 0xFF, marker);
        byteout((k >> 16) & 0xFF, 0);
        byteout((k >> 8) & 0xFF, 0);
        byteout((k & 0xFF), 0);
        break;
      case HALFCONSTANT_OT:
        j = 2;
        byteout((k >> 8) & 0xFF, marker);
        byteout((k & 0xFF), 0);
        break;
      case BYTECONSTANT_OT:
        j = 1;
        byteout((k & 0xFF), marker);
        break;
      case ZEROCONSTANT_OT:
        j = 0;
        break;
      case DEREFERENCE_OT:
        j = 7;
        byteout((k >> 24) & 0xFF, marker);
        byteout((k >> 16) & 0xFF, 0);
        byteout((k >> 8) & 0xFF, 0);
        byteout((k & 0xFF), 0);
        break;
      case GLOBALVAR_OT:*/
        /* Global variable -- a constant address. */
        //k -= MAX_LOCAL_VARIABLES;
        if (/* DISABLES CODE */ (0)) {
            /* We could write the value as a marker and patch it later... */
            j = 7;
            byteout(((k) >> 24) & 0xFF, VARIABLE_MV);
            byteout(((k) >> 16) & 0xFF, 0);
            byteout(((k) >> 8) & 0xFF, 0);
            byteout(((k) & 0xFF), 0);
        }
        else {
            /* ...but it's more efficient to write it as a RAM operand,
                  which can be 1, 2, or 4 bytes. Remember that global variables
                  are the very first thing in RAM. */
            k = k * 4; /* each variable is four bytes */
            /*if (k <= 255) {
                j = 13;
                byteout(((k) & 0xFF), 0);
            }
            else if (k <= 65535) {
                j = 14;
                byteout(((k) >> 8) & 0xFF, 0);
                byteout(((k) & 0xFF), 0);
            }
            else {
                j = 15;
                byteout(((k) >> 24) & 0xFF, 0);
                byteout(((k) >> 16) & 0xFF, 0);
                byteout(((k) >> 8) & 0xFF, 0);
                byteout(((k) & 0xFF), 0);       
            }
        }
        break;
      case LOCALVAR_OT:
        if (k == 0) {*/
            /* Stack-pointer magic variable */
        /*    j = 8; 
        }
        else {
            /* Local variable -- a byte or short offset from the
               frame pointer. It's an unsigned offset, so we can
               fit up to long 63 (offset 4*63) in a byte. */
           /* if ((k-1) < 64) {
                j = 9;
                byteout((k-1)*4, 0);
            }
            else {
                j = 10;
                byteout((((k-1)*4) >> 8) & 0xFF, 0);
                byteout(((k-1)*4) & 0xFF, 0);
            }
        }
        break;
      default:
        j = 0;
        break;
      }

      if (ix & 1)
          j = (j << 4);
      opmodes_pc[ix/2] |= j;*/
    }

    /* Print assembly trace. */
    if (asm_trace_level > 0) {
      int i, indent, block_op;
      switch (AI->internal_number) {
	  case block_wc:
	  case loop_wc:
	  case if_wc:
	  case else_wc:
	  case end_wc:
	      block_op = TRUE;
	      break;
	  default:
	      block_op = FALSE;
	      break;
      }
      indent = blocks_stack.no_items;
      if (block_op)
	  indent--;
      indent *= 2;
      printf("%5d  +%05lx %3s%*s %-12s ", ErrorReport.line_number,
        ((long int) offset),
        (at_seq_point)?"<*>":"   ", indent, "", opco.name);
      for (i=0; i<AI->operand_count; i++) {
          print_operand_w(&AI->operand[i], TRUE);
          printf(" ");
      }

      if (block_op)
	  printf("(L%d) ", blocks_top->label);

      if (asm_trace_level>=2) {
        for (j=0;
            start_pc<zcode_holding_area + zcode_ha_size;
            j++, start_pc++) {
            if (j%16==0) printf("\n                               ");
            if (/* DISABLES CODE */ (0)) {
                printf("%02x ", *start_pc);
            }
            else {
                printf("%02x", *start_pc);
                if (zcode_markers[start_pc-zcode_holding_area])
                    printf("{%02x}", zcode_markers[start_pc-zcode_holding_area]);
                printf(" ");
            }
        }
      }
      printf("\n");
    }

    if (module_switch) flush_link_data();

    return;

    OpcodeSyntaxError:

    WABORT;
    //make_opcode_syntax_g(opco);
    error_named("Assembly mistake: syntax is", opcode_syntax_string);
}

extern void assemble_label_no(int n)
{
    if (asm_trace_level > 0)
        printf("%5d  +%05lx    .L%d\n", ErrorReport.line_number,
            ((long int) zmachine_pc), n);
    set_label_offset(n, zmachine_pc);
    execution_never_reaches_here = FALSE;
}

extern void define_symbol_label(int symbol)
{   label_symbols[svals[symbol]] = symbol;
}

extern int32 assemble_routine_header(int no_locals,
    int routine_asterisked, char *name, int embedded_flag, int the_symbol)
{   int i, rv;
    int stackargs = FALSE;
    int name_length;

    execution_never_reaches_here = FALSE;

    routine_locals = no_locals;

    ensure_memory_list_available(&variables_memlist, MAX_LOCAL_VARIABLES);
    for (i=0; i<MAX_LOCAL_VARIABLES; i++) variables[i].usage = FALSE;

    if (no_locals >= 1 
      && !strcmp(local_variables.keywords[0], "_vararg_count")) {
      stackargs = TRUE;
    }

    if (veneer_mode) routine_starts_line = blank_brief_location;
    else routine_starts_line = get_brief_location(&ErrorReport);

    if (asm_trace_level > 0)
    {   printf("\n%5d  +%05lx  [ %s ", ErrorReport.line_number,
            ((long int) zmachine_pc), name);
        switch (target_machine) {
 	  default:
            for (i=1; i<=no_locals; i++) printf("%s ", variable_name(i));
	    break;

	  case TARGET_WASM:
            for (i=0; i<no_locals; i++) printf("%s ", variable_name(i));
	    break;
	}
        printf("\n\n");
    }

    routine_start_pc = zmachine_pc;

    if (track_unused_routines) {
        /* The name of an embedded function is in a temporary buffer,
           so we shouldn't keep a reference to it. (It is sad that we
           have to know this here.) */
        char *funcname = name;
        if (embedded_flag)
            funcname = "<embedded>";

        df_note_function_start(funcname, zmachine_pc, embedded_flag,
                               routine_starts_line);
    }

    routine_symbol = the_symbol;
    name_length = strlen(name) + 1;
    routine_name =
      my_malloc(name_length * sizeof(char), "temporary copy of routine name");
    strncpy(routine_name, name, name_length);

    /*  Update the routine counter                                           */

    no_routines++;

    /*  Actually assemble the routine header into the code area; note        */
    /*  Inform doesn't support the setting of local variables to default     */
    /*  values other than 0 in V3 and V4.  (In V5+ the Z-Machine doesn't     */
    /*  provide the possibility in any case.)                                */

    switch (target_machine) {
        case TARGET_ZCODE:

      if (stackargs) 
        warning("Z-code does not support stack-argument function definitions.");

      byteout(no_locals, 0);

      /*  Not the packed address, but the scaled offset from code area start:  */

      rv = zmachine_pc/scale_factor;

      if (instruction_set_number<5)
          for (i=0; i<no_locals; i++) { byteout(0,0); byteout(0,0); }

      next_label = 0; next_sequence_point = 0; last_label = -1;

      /*  Compile code to print out text like "a=3, b=4, c=5" when the       */
      /*  function is called, if it's required.                              */

      if ((routine_asterisked) || (define_INFIX_switch))
      {   char fnt[256]; assembly_operand PV, RFA, CON, STP, SLF; int ln, ln2;

          ln = next_label++;
          ln2 = next_label++;

          if (define_INFIX_switch)
          {
                if (embedded_flag)
            {   SLF.value = 251; SLF.type = VARIABLE_OT; SLF.marker = 0;
                  CON.value = 0; CON.type = SHORT_CONSTANT_OT; CON.marker = 0;
                assemblez_2_branch(test_attr_zc, SLF, CON, ln2, FALSE);
            }
            else
            {   i = no_named_routines++;
                  named_routine_symbols[i] = the_symbol;
                CON.value = i/8; CON.type = LONG_CONSTANT_OT; CON.marker = 0;
                RFA.value = routine_flags_array_SC;
                RFA.type = LONG_CONSTANT_OT; RFA.marker = INCON_MV;
                STP.value = 0; STP.type = VARIABLE_OT; STP.marker = 0;
                assemblez_2_to(loadb_zc, RFA, CON, STP);
                CON.value = (1 << (i%8)); CON.type = SHORT_CONSTANT_OT;
                assemblez_2_to(and_zc, STP, CON, STP);
                assemblez_1_branch(jz_zc, STP, ln2, TRUE);
            }
        }
        sprintf(fnt, "[ %s(", name);
        AI.text = fnt; assemblez_0(print_zc);
        for (i=1; (i<=7)&&(i<=no_locals); i++)
        {   if (version_number >= 5)
            {   PV.type = SHORT_CONSTANT_OT;
                PV.value = i; PV.marker = 0;
                assemblez_1_branch(check_arg_count_zc, PV, ln, FALSE);
            }
            sprintf(fnt, "%s%s = ", (i==1)?"":", ", variable_name(i));
            AI.text = fnt; assemblez_0(print_zc);
            PV.type = VARIABLE_OT; PV.value = i; PV.marker = 0;
            assemblez_1(print_num_zc, PV);
        }
        assemble_label_no(ln);
        sprintf(fnt, ") ]^"); AI.text = fnt;
        assemblez_0(print_zc);
        assemble_label_no(ln2);
      }
      break;

      case TARGET_GLULX:
      rv = zmachine_pc;

      if (stackargs)
        byteout(0xC0, 0); /* Glulx type byte for function */
      else
        byteout(0xC1, 0); /* Glulx type byte for function */

      /* Now the locals format list. This is simple; we only use
        four-byte locals. That's a single pair, unless we have more
        than 255 locals, or none at all. */
      i = no_locals;
      while (i) {
        int j = i;
        if (j > 255)
          j = 255;
        byteout(4, 0); 
        byteout(j, 0);
        i -= j;
      }
      /* Terminate the list with a (0, 0) pair. */
      byteout(0, 0);
      byteout(0, 0);

      if (stackargs) {
        /* The top stack value is the number of function arguments. Let's
           move that into the first local, which is _vararg_count. */
        /* @copy sp _vararg_count; */
        byteout(0x40, 0); byteout(0x98, 0); byteout(0x00, 0);
      }

      next_label = 0; next_sequence_point = 0; last_label = -1; 

      if ((routine_asterisked) || (define_INFIX_switch)) {
        int ix;
        char fnt[256];
        assembly_operand AO, AO2;
        if (define_INFIX_switch) {
          /* This isn't supported */
          if (embedded_flag) {
          }
          else {
            i = no_named_routines++;
            named_routine_symbols[i] = the_symbol;
          }
        }
        sprintf(fnt, "[ %s(", name);
        AO.marker = STRING_MV;
        AO.type   = CONSTANT_OT;
        AO.value  = compile_string(fnt, STRCTX_INFIX);
        assembleg_1(streamstr_gc, AO);

        if (!stackargs) {
          for (ix=1; ix<=no_locals; ix++) {
            sprintf(fnt, "%s%s = ", (ix==1)?"":", ", variable_name(ix));
            AO.marker = STRING_MV;
            AO.type   = CONSTANT_OT;
            AO.value  = compile_string(fnt, STRCTX_INFIX);
            assembleg_1(streamstr_gc, AO);
            AO.marker = 0;
            AO.type = LOCALVAR_OT;
            AO.value = ix;
            assembleg_1(streamnum_gc, AO);
          }
        }
        else {
          int lntop, lnbottom;
          sprintf(fnt, "%s = ", variable_name(1));
          AO.marker = STRING_MV;
          AO.type   = CONSTANT_OT;
          AO.value  = compile_string(fnt, STRCTX_INFIX);
          assembleg_1(streamstr_gc, AO);
          AO.marker = 0;
          AO.type = LOCALVAR_OT;
          AO.value = 1;
          assembleg_1(streamnum_gc, AO);
          AO2.type = BYTECONSTANT_OT;
          AO2.marker = 0;
          AO2.value = ':';
          assembleg_1(streamchar_gc, AO2);
          AO2.type = BYTECONSTANT_OT;
          AO2.marker = 0;
          AO2.value = ' ';
          /* for (temp_var4=0 : temp_var4<_vararg_count : temp_var4++) {
               @streamchar ' ';
               @stkpeek temp_var4 sp;
               @stream_num sp;
             }
          */
          assembleg_store(temp_var4, zero_operand);
          lntop = next_label++;
          lnbottom = next_label++;
          assemble_label_no(lntop);
          assembleg_2_branch(jge_gc, temp_var4, AO, lnbottom); /* AO is _vararg_count */
          assembleg_1(streamchar_gc, AO2); /* AO2 is space */
          assembleg_2(stkpeek_gc, temp_var4, stack_pointer);
          assembleg_1(streamnum_gc, stack_pointer);
          assembleg_3(add_gc, temp_var4, one_operand, temp_var4);
          assembleg_0_branch(jump_gc, lntop);
          assemble_label_no(lnbottom);
        }

        AO.marker = STRING_MV;
        AO.type   = CONSTANT_OT;
        AO.value  = compile_string(") ]^", STRCTX_INFIX);
        assembleg_1(streamstr_gc, AO);
      }
      break;

      case TARGET_WASM:
      rv = no_routines - 1 + 1; /* WebAssembly uses indices instead of addresses */

      i = no_named_routines++;
      named_routine_symbols[i] = the_symbol;

      get_wasm_type(no_locals, 1);

      byteout(0xee, 0); /* size */

      /* non-parameter locals */
      byteout(1, 0); /* 1 declaration */
 
      byteout(3, 0);    /* 3 */
      byteout(0x7f, 0); /* i32 */

      temp_var1.value = no_locals;
      temp_var2.value = no_locals + 1;
      temp_var3.value = no_locals + 2;
      break;
    }

    return rv;
}

void assemble_routine_end(int embedded_flag, debug_locations locations)
{   int32 i, first_local, last_local;

    switch (target_machine) {
      case TARGET_ZCODE:
      case TARGET_GLULX:
	/* locals are 1-indexed */
	first_local = 1;
	last_local = routine_locals;
        break;

      case TARGET_WASM:
	/* locals are 0-indexed */
	first_local = 0;
	last_local = routine_locals - 1;
        break;
    }


    /* No marker is made in the Z-machine's code area to indicate the        */
    /* end of a routine.  Instead, we simply assemble a return opcode if     */
    /* need be (it won't be if the last instruction was, say, a "quit").     */
    /* The return value is true (1) for normal routines, false (0) for       */
    /* embedded routines (e.g. the library uses this for "before"            */
    /* properties).                                                          */

    if (!execution_never_reaches_here)
    {   
      switch (target_machine) {
        case TARGET_ZCODE:
        if (embedded_flag) assemblez_0(rfalse_zc);
                      else assemblez_0(rtrue_zc);
        break;

	case TARGET_GLULX: {
        assembly_operand AO;
        if (embedded_flag) 
            AO = zero_operand;
        else 
            AO = one_operand;
        assembleg_1(return_gc, AO);
	} break;

	case TARGET_WASM: {
	/* returns are implicit */
        /* but we need to put the value on the stack */
        assembly_operand AO;
        if (embedded_flag) 
            AO = zero_operand;
        else 
            AO = one_operand;
	assemblew_load(AO);
	} break;
      }
    }

    if (target_machine == TARGET_WASM) {
        /* WebAssembly needs an end marker */
	byteout(0x0b, 0);

	/* reset temp assignments so they don't show up in trace of next header */
        temp_var1.value = -1;
        temp_var2.value = -1;
        temp_var3.value = -1;
    }

    /* Dump the contents of the current routine into longer-term Z-code
       storage                                                               */

    switch (target_machine) {
      case TARGET_ZCODE:
      transfer_routine_z();
      break;

      case TARGET_GLULX:
      transfer_routine_g();
      break;

      case TARGET_WASM:
      transfer_routine_w();
      break;
    }

    if (track_unused_routines)
        df_note_function_end(zmachine_pc);

    /* Tell the debugging file about the routine just ended.                 */

    if (debugfile_switch)
    {
        debug_file_printf("<routine>");
        if (embedded_flag)
        {   debug_file_printf
                ("<identifier artificial=\"true\">%s</identifier>",
                 routine_name);
        }
        else if (sflags[routine_symbol] & REPLACE_SFLAG)
        {   /* The symbol type will be set to ROUTINE_T once the replaced
               version has been given; if it is already set, we must be dealing
               with a replacement, and we can use the routine name as-is.
               Otherwise we look for a rename.  And if that doesn't work, we
               fall back to an artificial identifier. */
            if (stypes[routine_symbol] == ROUTINE_T)
            {   /* Optional because there may be further replacements. */
                write_debug_optional_identifier(routine_symbol);
            }
            else if (find_symbol_replacement(&routine_symbol))
            {   debug_file_printf
                    ("<identifier>%s</identifier>", symbs[routine_symbol]);
            }
            else
            {   debug_file_printf
                    ("<identifier artificial=\"true\">%s (replaced)"
                         "</identifier>",
                     routine_name);
            }
        } else
        {   debug_file_printf("<identifier>%s</identifier>", routine_name);
        }
        debug_file_printf("<value>");
        if (target_machine != TARGET_ZCODE)
        {   write_debug_code_backpatch(routine_start_pc);
        } else
        {   write_debug_packed_code_backpatch(routine_start_pc);
        }
        debug_file_printf("</value>");
        debug_file_printf("<address>");
        write_debug_code_backpatch(routine_start_pc);
        debug_file_printf("</address>");
        debug_file_printf
            ("<byte-count>%d</byte-count>", zmachine_pc - routine_start_pc);
        write_debug_locations(locations);
        for (i = first_local; i <= last_local; ++i)
        {   debug_file_printf("<local-variable>");
            debug_file_printf("<identifier>%s</identifier>", variable_name(i));
            switch (target_machine) {
            case TARGET_ZCODE:
                debug_file_printf("<index>%d</index>", i);
            break;

	    case TARGET_GLULX:
                debug_file_printf
                    ("<frame-offset>%d</frame-offset>", 4 * (i - 1));
	    break;

	    case TARGET_WASM:
	    WABORT;
            }
            debug_file_printf("</local-variable>");
        }
        for (i = 0; i < next_sequence_point; ++i)
        {   debug_file_printf("<sequence-point>");
            debug_file_printf("<address>");
            write_debug_code_backpatch
                (label_offsets[sequence_point_labels[i]]);
            debug_file_printf("</address>");
            write_debug_location(sequence_point_locations[i]);
            debug_file_printf("</sequence-point>");
        }
        debug_file_printf("</routine>");
    }

    my_free(&routine_name, "temporary copy of routine name");

    /* Issue warnings about any local variables not used in the routine. */

    for (i = first_local; i<=last_local; i++)
        if (!(variables[i].usage))
            dbnu_warning("Local variable", variable_name(i),
                routine_starts_line);

    for (i=0; i<next_label; i++)
    {   int j = label_symbols[i];
        if (j != -1)
        {   if (sflags[j] & CHANGE_SFLAG)
                error_named_at("Routine contains no such label as",
                    (char *) symbs[j], slines[j]);
            else
                if ((sflags[j] & USED_SFLAG) == 0)
                    dbnu_warning("Label", (char *) symbs[j], slines[j]);
            stypes[j] = CONSTANT_T;
            sflags[j] = UNKNOWN_SFLAG;
        }
    }
    no_sequence_points += next_sequence_point;
    next_label = 0; next_sequence_point = 0;
}

/* ------------------------------------------------------------------------- */
/*   Called when the holding area contains an entire routine of code:        */
/*   backpatches the labels, issues module markers, then dumps the routine   */
/*   into longer-term storage.                                               */
/*   Note that in the code received, all branches have long form, and their  */
/*   contents are not an offset but the label numbers they branch to.        */
/*   Similarly, LABEL operands (those of "jump" instructions) are label      */
/*   numbers.  So this routine must change the label numbers to offsets,     */
/*   slimming the code down as it does so to take advantage of short-form    */
/*   branch operands where possible.                                         */
/* ------------------------------------------------------------------------- */

static int32 adjusted_pc;

static void transfer_to_temp_file(uchar *c)
{   fputc(*c,Temp2_fp);
    adjusted_pc++;
}

static void transfer_to_zcode_area(uchar *c)
{   write_byte_to_memory_block(&zcode_area, adjusted_pc++, *c);
}

static void transfer_routine_z(void)
{   int32 i, j, pc, new_pc, label, long_form, offset_of_next, addr,
          branch_on_true, rstart_pc;
    void (* transfer_byte)(uchar *);

    adjusted_pc = zmachine_pc - zcode_ha_size; rstart_pc = adjusted_pc;

    if (asm_trace_level >= 3)
    {   printf("Backpatching routine at %05lx: initial size %d, %d labels\n",
             (long int) adjusted_pc, zcode_ha_size, next_label);
    }

    transfer_byte =
        (temporary_files_switch)?transfer_to_temp_file:transfer_to_zcode_area;

    /*  (1) Scan through for branches and make short/long decisions in each
            case.  Mark omitted bytes (2nd bytes in branches converted to
            short form) with DELETED_MV.                                     */

    for (i=0, pc=adjusted_pc; i<zcode_ha_size; i++, pc++)
    {   if (zcode_markers[i] == BRANCH_MV)
        {   if (asm_trace_level >= 4)
                printf("Branch detected at offset %04x\n", pc);
            j = (256*zcode_holding_area[i] + zcode_holding_area[i+1]) & 0x7fff;
            if (asm_trace_level >= 4)
                printf("To label %d, which is %d from here\n",
                    j, label_offsets[j]-pc);
            if ((label_offsets[j] >= pc+2) && (label_offsets[j] < pc+64))
            {   if (asm_trace_level >= 4) printf("Short form\n");
                zcode_markers[i+1] = DELETED_MV;
            }
        }
    }

    /*  (2) Calculate the new positions of the labels.  Note that since the
            long/short decision was taken on the basis of the old labels,
            and since the new labels are slightly closer together because
            of branch bytes deleted, there may be a few further branch
            optimisations which are possible but which have been missed
            (if two labels move inside the "short" range as a result of
            a previous optimisation).  However, this is acceptably uncommon. */

    if (next_label > 0)
    {   if (asm_trace_level >= 4)
        {   printf("Opening label: %d\n", first_label);
            for (i=0;i<next_label;i++)
                printf("Label %d offset %04x next -> %d previous -> %d\n",
                    i, label_offsets[i], label_next[i], label_prev[i]);
        }

        for (i=0, pc=adjusted_pc, new_pc=adjusted_pc, label = first_label;
            i<zcode_ha_size; i++, pc++)
        {   while ((label != -1) && (label_offsets[label] == pc))
            {   if (asm_trace_level >= 4)
                    printf("Position of L%d corrected from %04x to %04x\n",
                        label, label_offsets[label], new_pc);
                label_offsets[label] = new_pc;
                label = label_next[label];
            }
           if (zcode_markers[i] != DELETED_MV) new_pc++;
        }
    }

    /*  (3) As we are transferring, replace the label numbers in branch
            operands with offsets to those labels.  Also issue markers, now
            that we know where they occur in the final Z-code area.          */

    for (i=0, new_pc=adjusted_pc; i<zcode_ha_size; i++)
    {   switch(zcode_markers[i])
        { case BRANCH_MV:
            long_form = 1; if (zcode_markers[i+1] == DELETED_MV) long_form = 0;

            j = (256*zcode_holding_area[i] + zcode_holding_area[i+1]) & 0x7fff;
            branch_on_true = ((zcode_holding_area[i]) & 0x80);
            offset_of_next = new_pc + long_form + 1;

            addr = label_offsets[j] - offset_of_next + 2;
            if (addr<-0x2000 || addr>0x1fff) 
                fatalerror("Branch out of range: divide the routine up?");
            if (addr<0) addr+=(int32) 0x10000L;

            addr=addr&0x3fff;
            if (long_form==1)
            {   zcode_holding_area[i] = branch_on_true + addr/256;
                zcode_holding_area[i+1] = addr%256;
            }
            else
            {   if (addr >= 64)
                {   compiler_error("Label out of range for branch");
                    printf("Addr is %04x\n", addr);
                }
                zcode_holding_area[i] = branch_on_true + 0x40 + (addr&0x3f);
            }
            transfer_byte(zcode_holding_area + i); new_pc++;
            break;

          case LABEL_MV:
            j = 256*zcode_holding_area[i] + zcode_holding_area[i+1];
            addr = label_offsets[j] - new_pc;
            if (addr<-0x8000 || addr>0x7fff) 
                fatalerror("Jump out of range: divide the routine up?");
            if (addr<0) addr += (int32) 0x10000L;
            zcode_holding_area[i] = addr/256;
            zcode_holding_area[i+1] = addr%256;
            transfer_byte(zcode_holding_area + i); new_pc++;
            break;

          case DELETED_MV:
            break;

          default:
            switch(zcode_markers[i] & 0x7f)
            {   case NULL_MV: break;
                case VARIABLE_MV:
                case OBJECT_MV:
                case ACTION_MV:
                case IDENT_MV:
                    if (!module_switch) break;
                default:
                    if ((zcode_markers[i] & 0x7f) > LARGEST_BPATCH_MV)
                    {   compiler_error("Illegal code backpatch value");
                        printf("Illegal value of %02x at PC = %04x\n",
                            zcode_markers[i] & 0x7f, new_pc);
                        break;
                    }

                    write_byte_to_memory_block(&zcode_backpatch_table,
                        zcode_backpatch_size++,
                        zcode_markers[i] + 32*(new_pc/65536));
                    write_byte_to_memory_block(&zcode_backpatch_table,
                        zcode_backpatch_size++, (new_pc/256)%256);
                    write_byte_to_memory_block(&zcode_backpatch_table,
                        zcode_backpatch_size++, new_pc%256);
                    break;
            }
            transfer_byte(zcode_holding_area + i); new_pc++;
            break;
        }
    }

    if (asm_trace_level >= 3)
    {   printf("After branch optimisation, routine length is %d bytes\n",
             new_pc - rstart_pc);
    }

    /*  Insert null bytes if necessary to ensure the next routine address is */
    /*  expressible as a packed address                                      */

    {   uchar zero[1];
        zero[0] = 0;
        if (oddeven_packing_switch)
            while ((adjusted_pc%(scale_factor*2))!=0) transfer_byte(zero);
        else
            while ((adjusted_pc%scale_factor)!=0) transfer_byte(zero);
    }

    zmachine_pc = adjusted_pc;
    zcode_ha_size = 0;
}

static void transfer_routine_g(void)
{   int32 i, j, pc, new_pc, label, form_len, offset_of_next, addr,
          rstart_pc;
    void (* transfer_byte)(uchar *);

    adjusted_pc = zmachine_pc - zcode_ha_size; rstart_pc = adjusted_pc;

    if (asm_trace_level >= 3)
    {   printf("Backpatching routine at %05lx: initial size %d, %d labels\n",
             (long int) adjusted_pc, zcode_ha_size, next_label);
    }

    transfer_byte =
        (temporary_files_switch)?transfer_to_temp_file:transfer_to_zcode_area;

    /*  (1) Scan through for branches and make short/long decisions in each
            case.  Mark omitted bytes (bytes 2-4 in branches converted to
            short form) with DELETED_MV.                                     */

    for (i=0, pc=adjusted_pc; i<zcode_ha_size; i++, pc++) {
      if (zcode_markers[i] >= BRANCH_MV && zcode_markers[i] < BRANCHMAX_MV) {
        int opmodeoffset = (zcode_markers[i] - BRANCH_MV);
        int32 opmodebyte;
        if (asm_trace_level >= 4)
            printf("Branch detected at offset %04x\n", pc);
        j = ((zcode_holding_area[i] << 24) 
            | (zcode_holding_area[i+1] << 16)
            | (zcode_holding_area[i+2] << 8)
            | (zcode_holding_area[i+3]));
        offset_of_next = pc + 4;
        addr = (label_offsets[j] - offset_of_next) + 2;
        if (asm_trace_level >= 4)
            printf("To label %d, which is (%d-2) = %d from here\n",
                j, addr, label_offsets[j] - offset_of_next);
        if (addr >= -0x80 && addr < 0x80) {
            if (asm_trace_level >= 4) printf("...Byte form\n");
            zcode_markers[i+1] = DELETED_MV;
            zcode_markers[i+2] = DELETED_MV;
            zcode_markers[i+3] = DELETED_MV;
            opmodebyte = i - ((opmodeoffset+1)/2);
            if ((opmodeoffset & 1) == 0)
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0xF0) | 0x01;
            else
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0x0F) | 0x10;
        }
        else if (addr >= -0x8000 && addr < 0x8000) {
            if (asm_trace_level >= 4) printf("...Short form\n");
            zcode_markers[i+2] = DELETED_MV;
            zcode_markers[i+3] = DELETED_MV;
            opmodebyte = i - ((opmodeoffset+1)/2);
            if ((opmodeoffset & 1) == 0)
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0xF0) | 0x02;
            else
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0x0F) | 0x20;
        }
      }
    }

    /*  (2) Calculate the new positions of the labels.  Note that since the
            long/short decision was taken on the basis of the old labels,
            and since the new labels are slightly closer together because
            of branch bytes deleted, there may be a few further branch
            optimisations which are possible but which have been missed
            (if two labels move inside the "short" range as a result of
            a previous optimisation).  However, this is acceptably uncommon. */
    if (next_label > 0) {
      if (asm_trace_level >= 4) {
        printf("Opening label: %d\n", first_label);
        for (i=0;i<next_label;i++)
            printf("Label %d offset %04x next -> %d previous -> %d\n",
                i, label_offsets[i], label_next[i], label_prev[i]);
      }

      for (i=0, pc=adjusted_pc, new_pc=adjusted_pc, label = first_label;
        i<zcode_ha_size; 
        i++, pc++) {
        while ((label != -1) && (label_offsets[label] == pc)) {
            if (asm_trace_level >= 4)
                printf("Position of L%d corrected from %04x to %04x\n",
                label, label_offsets[label], new_pc);
            label_offsets[label] = new_pc;
            label = label_next[label];
        }
        if (zcode_markers[i] != DELETED_MV) new_pc++;
      }
    }

    /*  (3) As we are transferring, replace the label numbers in branch
            operands with offsets to those labels.  Also issue markers, now
            that we know where they occur in the final Z-code area.          */

    for (i=0, new_pc=adjusted_pc; i<zcode_ha_size; i++) {

      if (zcode_markers[i] >= BRANCH_MV && zcode_markers[i] < BRANCHMAX_MV) {
        form_len = 4;
        if (zcode_markers[i+1] == DELETED_MV) {
            form_len = 1;
        }
        else {
            if (zcode_markers[i+2] == DELETED_MV)
                form_len = 2;
        }
        j = ((zcode_holding_area[i] << 24) 
            | (zcode_holding_area[i+1] << 16)
            | (zcode_holding_area[i+2] << 8)
            | (zcode_holding_area[i+3]));

        /* At the moment, we can safely assume that the branch operand
           is the end of the opcode, so the next opcode starts right
           after it. */
        offset_of_next = new_pc + form_len;

        addr = (label_offsets[j] - offset_of_next) + 2;
        if (asm_trace_level >= 4) {
            printf("Branch at offset %04x: %04x (%s)\n",
                new_pc, addr, ((form_len == 1) ? "byte" :
                ((form_len == 2) ? "short" : "long")));
        }
        if (form_len == 1) {
            if (addr < -0x80 || addr >= 0x80) {
                error("*** Label out of range for byte branch ***");
            }
            zcode_holding_area[i] = (addr) & 0xFF;
        }
        else if (form_len == 2) {
            if (addr < -0x8000 || addr >= 0x8000) {
                error("*** Label out of range for short branch ***");
            }
            zcode_holding_area[i] = (addr >> 8) & 0xFF;
            zcode_holding_area[i+1] = (addr) & 0xFF;
        }
        else {
            zcode_holding_area[i] = (addr >> 24) & 0xFF;
            zcode_holding_area[i+1] = (addr >> 16) & 0xFF;
            zcode_holding_area[i+2] = (addr >> 8) & 0xFF;
            zcode_holding_area[i+3] = (addr) & 0xFF;
        }
        transfer_byte(zcode_holding_area + i); new_pc++;
      }
      else if (zcode_markers[i] == LABEL_MV) {
          error("*** No LABEL opcodes in Glulx ***");
      }
      else if (zcode_markers[i] == DELETED_MV) {
        /* skip it */
      }
      else {
        switch(zcode_markers[i] & 0x7f) {
        case NULL_MV: 
            break;
        case ACTION_MV:
        case IDENT_MV:
            if (!module_switch) break;
        case OBJECT_MV:
        case VARIABLE_MV:
        default:
            if ((zcode_markers[i] & 0x7f) > LARGEST_BPATCH_MV) {
                error("*** Illegal code backpatch value ***");
                printf("Illegal value of %02x at PC = %04x\n",
                zcode_markers[i] & 0x7f, new_pc);
                break;
            }
          /* The backpatch table format for Glulx:
             First, the marker byte (0..LARGEST_BPATCH_MV).
             Then a byte indicating the data size to be patched (1, 2, 4).
             Then the four-byte address (new_pc).
          */
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++,
            zcode_markers[i]);
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++,
            4);
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 24) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 16) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 8) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, (new_pc & 0xFF));
          break;
        }
        transfer_byte(zcode_holding_area + i); new_pc++;
      }
    }

    if (asm_trace_level >= 3)
    {   printf("After branch optimisation, routine length is %d bytes\n",
             new_pc - rstart_pc);
    }

    zmachine_pc = adjusted_pc;
    zcode_ha_size = 0;
}

static void transfer_routine_w(void)
{   int32 i, j, pc, new_pc, label, form_len, offset_of_next, addr,
          rstart_pc;
    void (* transfer_byte)(uchar *);

    adjusted_pc = zmachine_pc - zcode_ha_size; rstart_pc = adjusted_pc;

    if (asm_trace_level >= 3)
    {   printf("Backpatching routine at %05lx: initial size %d, %d labels\n",
             (long int) adjusted_pc, zcode_ha_size, next_label);
    }

    transfer_byte =
        (temporary_files_switch)?transfer_to_temp_file:transfer_to_zcode_area;

    /*  (1) Scan through for branches and make short/long decisions in each
            case.  Mark omitted bytes (bytes 2-4 in branches converted to
            short form) with DELETED_MV.                                     */

    for (i=0, pc=adjusted_pc; i<zcode_ha_size; i++, pc++) {
      if (zcode_markers[i] >= BRANCH_MV && zcode_markers[i] < BRANCHMAX_MV) {
        int opmodeoffset = (zcode_markers[i] - BRANCH_MV);
        int32 opmodebyte;
        if (asm_trace_level >= 4)
            printf("Branch detected at offset %04x\n", pc);
        j = ((zcode_holding_area[i] << 24) 
            | (zcode_holding_area[i+1] << 16)
            | (zcode_holding_area[i+2] << 8)
            | (zcode_holding_area[i+3]));
        offset_of_next = pc + 4;
        addr = (label_offsets[j] - offset_of_next) + 2;
        if (asm_trace_level >= 4)
            printf("To label %d, which is (%d-2) = %d from here\n",
                j, addr, label_offsets[j] - offset_of_next);
        if (addr >= -0x80 && addr < 0x80) {
            if (asm_trace_level >= 4) printf("...Byte form\n");
            zcode_markers[i+1] = DELETED_MV;
            zcode_markers[i+2] = DELETED_MV;
            zcode_markers[i+3] = DELETED_MV;
            opmodebyte = i - ((opmodeoffset+1)/2);
            if ((opmodeoffset & 1) == 0)
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0xF0) | 0x01;
            else
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0x0F) | 0x10;
        }
        else if (addr >= -0x8000 && addr < 0x8000) {
            if (asm_trace_level >= 4) printf("...Short form\n");
            zcode_markers[i+2] = DELETED_MV;
            zcode_markers[i+3] = DELETED_MV;
            opmodebyte = i - ((opmodeoffset+1)/2);
            if ((opmodeoffset & 1) == 0)
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0xF0) | 0x02;
            else
                zcode_holding_area[opmodebyte] = 
                    (zcode_holding_area[opmodebyte] & 0x0F) | 0x20;
        }
      }
    }

    /*  (2) Calculate the new positions of the labels.  Note that since the
            long/short decision was taken on the basis of the old labels,
            and since the new labels are slightly closer together because
            of branch bytes deleted, there may be a few further branch
            optimisations which are possible but which have been missed
            (if two labels move inside the "short" range as a result of
            a previous optimisation).  However, this is acceptably uncommon. */
    if (next_label > 0) {
      if (asm_trace_level >= 4) {
        printf("Opening label: %d\n", first_label);
        for (i=0;i<next_label;i++)
            printf("Label %d offset %04x next -> %d previous -> %d\n",
                i, label_offsets[i], label_next[i], label_prev[i]);
      }

      for (i=0, pc=adjusted_pc, new_pc=adjusted_pc, label = first_label;
        i<zcode_ha_size; 
        i++, pc++) {
        while ((label != -1) && (label_offsets[label] == pc)) {
            if (asm_trace_level >= 4)
                printf("Position of L%d corrected from %04x to %04x\n",
                label, label_offsets[label], new_pc);
            label_offsets[label] = new_pc;
            label = label_next[label];
        }
        if (zcode_markers[i] != DELETED_MV) new_pc++;
      }
    }

    /*  (3) As we are transferring, replace the label numbers in branch
            operands with offsets to those labels.  Also issue markers, now
            that we know where they occur in the final Z-code area.          */

    for (i=0, new_pc=adjusted_pc; i<zcode_ha_size; i++) {

      if (zcode_markers[i] >= BRANCH_MV && zcode_markers[i] < BRANCHMAX_MV) {
        form_len = 4;
        if (zcode_markers[i+1] == DELETED_MV) {
            form_len = 1;
        }
        else {
            if (zcode_markers[i+2] == DELETED_MV)
                form_len = 2;
        }
        j = ((zcode_holding_area[i] << 24) 
            | (zcode_holding_area[i+1] << 16)
            | (zcode_holding_area[i+2] << 8)
            | (zcode_holding_area[i+3]));

        /* At the moment, we can safely assume that the branch operand
           is the end of the opcode, so the next opcode starts right
           after it. */
        offset_of_next = new_pc + form_len;

        addr = (label_offsets[j] - offset_of_next) + 2;
        if (asm_trace_level >= 4) {
            printf("Branch at offset %04x: %04x (%s)\n",
                new_pc, addr, ((form_len == 1) ? "byte" :
                ((form_len == 2) ? "short" : "long")));
        }
        if (form_len == 1) {
            if (addr < -0x80 || addr >= 0x80) {
                error("*** Label out of range for byte branch ***");
            }
            zcode_holding_area[i] = (addr) & 0xFF;
        }
        else if (form_len == 2) {
            if (addr < -0x8000 || addr >= 0x8000) {
                error("*** Label out of range for short branch ***");
            }
            zcode_holding_area[i] = (addr >> 8) & 0xFF;
            zcode_holding_area[i+1] = (addr) & 0xFF;
        }
        else {
            zcode_holding_area[i] = (addr >> 24) & 0xFF;
            zcode_holding_area[i+1] = (addr >> 16) & 0xFF;
            zcode_holding_area[i+2] = (addr >> 8) & 0xFF;
            zcode_holding_area[i+3] = (addr) & 0xFF;
        }
        transfer_byte(zcode_holding_area + i); new_pc++;
      }
      else if (zcode_markers[i] == LABEL_MV) {
          error("*** No LABEL opcodes in Glulx ***");
      }
      else if (zcode_markers[i] == DELETED_MV) {
        /* skip it */
      }
      else {
        switch(zcode_markers[i] & 0x7f) {
        case NULL_MV: 
            break;
        case ACTION_MV:
        case IDENT_MV:
            if (!module_switch) break;
        case OBJECT_MV:
        case VARIABLE_MV:
        default:
            if ((zcode_markers[i] & 0x7f) > LARGEST_BPATCH_MV) {
                error("*** Illegal code backpatch value ***");
                printf("Illegal value of %02x at PC = %04x\n",
                zcode_markers[i] & 0x7f, new_pc);
                break;
            }
          /* The backpatch table format for Glulx:
             First, the marker byte (0..LARGEST_BPATCH_MV).
             Then a byte indicating the data size to be patched (1, 2, 4).
             Then the four-byte address (new_pc).
          */
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++,
            zcode_markers[i]);
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++,
            1);
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 24) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 16) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, ((new_pc >> 8) & 0xFF));
          write_byte_to_memory_block(&zcode_backpatch_table,
            zcode_backpatch_size++, (new_pc & 0xFF));
          break;
        }
        transfer_byte(zcode_holding_area + i); new_pc++;
      }
    }

    if (asm_trace_level >= 3)
    {   printf("After branch optimisation, routine length is %d bytes\n",
             new_pc - rstart_pc);
    }

    //WFIXME
    write_byte_to_memory_block(&zcode_area, zmachine_pc - zcode_ha_size, adjusted_pc - zmachine_pc + zcode_ha_size - 1);

    zmachine_pc = adjusted_pc;
    zcode_ha_size = 0;
}


/* ========================================================================= */
/*   Front ends for the instruction assembler: convenient shorthand forms    */
/*   used in various code generation routines all over Inform.               */
/* ------------------------------------------------------------------------- */

void assemble_jump(int n)
{
    switch (target_machine) {
        case TARGET_ZCODE:
        assemblez_jump(n);
        break;
    
	case TARGET_GLULX:
        assembleg_jump(n);
	break;

	case TARGET_WASM:
	assemblew_branch(br_wc, n);
	break;
    }
}

void assemblez_0(int internal_number)
{   AI.internal_number = internal_number;
    AI.operand_count = 0;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_0_to(int internal_number, assembly_operand o)
{   AI.internal_number = internal_number;
    AI.operand_count = 0;
    AI.store_variable_number = o.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_0_branch(int internal_number, int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 0;
    AI.store_variable_number = -1;
    AI.branch_label_number = label;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

void assemblez_1(int internal_number, assembly_operand o1)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_1_to(int internal_number,
    assembly_operand o1, assembly_operand st)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    AI.store_variable_number = st.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_1_branch(int internal_number,
    assembly_operand o1, int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    AI.branch_label_number = label;
    AI.store_variable_number = -1;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

void assemblez_2(int internal_number,
    assembly_operand o1, assembly_operand o2)
{   AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_3(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3)
{   AI.internal_number = internal_number;
    AI.operand_count = 3;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_3_to(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand st)
{   AI.internal_number = internal_number;
    AI.operand_count = 3;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.store_variable_number = st.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_3_branch(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 3;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.store_variable_number = -1;
    AI.branch_label_number = label;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

void assemblez_4(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4)
{   AI.internal_number = internal_number;
    AI.operand_count = 4;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_5(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4, assembly_operand o5)
{   AI.internal_number = internal_number;
    AI.operand_count = 5;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.operand[4] = o5;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_6(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4, assembly_operand o5, assembly_operand o6)
{   AI.internal_number = internal_number;
    AI.operand_count = 6;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.operand[4] = o5;
    AI.operand[5] = o6;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_4_branch(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4, int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 4;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.store_variable_number = -1;
    AI.branch_label_number = label;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

void assemblez_4_to(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4, assembly_operand st)
{   AI.internal_number = internal_number;
    AI.operand_count = 4;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.store_variable_number = st.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_5_to(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand o3,
    assembly_operand o4, assembly_operand o5, assembly_operand st)
{   AI.internal_number = internal_number;
    AI.operand_count = 5;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.operand[4] = o5;
    AI.store_variable_number = st.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_2_to(int internal_number,
    assembly_operand o1, assembly_operand o2, assembly_operand st)
{   AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.store_variable_number = st.value;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_2_branch(int internal_number,
    assembly_operand o1, assembly_operand o2, int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.branch_label_number = label;
    AI.store_variable_number = -1;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

void assemblez_objcode(int internal_number,
    assembly_operand o1, assembly_operand st, int label, int flag)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    AI.branch_label_number = label;
    AI.store_variable_number = st.value;
    AI.branch_flag = flag;
    assemblez_instruction(&AI);
}

extern void assemblez_inc(assembly_operand o1)
{   int m = 0;
    if ((o1.value >= MAX_LOCAL_VARIABLES) 
        && (o1.value<LOWEST_SYSTEM_VAR_NUMBER))
            m = VARIABLE_MV;
    AI.internal_number = inc_zc;
    AI.operand_count = 1;
    AI.operand[0].value = o1.value;
    AI.operand[0].type = SHORT_CONSTANT_OT;
    AI.operand[0].marker = m;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

extern void assemblez_dec(assembly_operand o1)
{   int m = 0;
    if ((o1.value >= MAX_LOCAL_VARIABLES) 
        && (o1.value<LOWEST_SYSTEM_VAR_NUMBER))
            m = VARIABLE_MV;
    AI.internal_number = dec_zc;
    AI.operand_count = 1;
    AI.operand[0].value = o1.value;
    AI.operand[0].type = SHORT_CONSTANT_OT;
    AI.operand[0].marker = m;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

extern void assemblez_store(assembly_operand o1, assembly_operand o2)
{   int m = 0;
    if ((o1.value >= MAX_LOCAL_VARIABLES)
        && (o1.value<LOWEST_SYSTEM_VAR_NUMBER))
            m = VARIABLE_MV;

    if ((o2.type == VARIABLE_OT) && (o2.value == 0))
    {
        /*  Assemble "pull VAR" rather than "store VAR sp",
            saving 1 byte  */

        AI.internal_number = pull_zc;
        if (instruction_set_number == 6)
        {   AI.operand_count = 0;
            AI.store_variable_number = o1.value;
        }
        else
        {   AI.operand_count = 1;
            AI.operand[0].value = o1.value;
            AI.operand[0].type = SHORT_CONSTANT_OT;
            AI.operand[0].marker = m;
            AI.store_variable_number = -1;
        }
        AI.branch_label_number = -1;
        assemblez_instruction(&AI);
        return;
    }

    if ((o1.type == VARIABLE_OT) && (o1.value == 0))
    {   /*  Assemble "push VAR" rather than "store sp VAR",
            saving 1 byte  */

        AI.internal_number = push_zc;
        AI.operand_count = 1;
        AI.operand[0] = o2;
        AI.store_variable_number = -1;
        AI.branch_label_number = -1;
        assemblez_instruction(&AI);
        return;
    }
    AI.internal_number = store_zc;
    AI.operand_count = 2;
    AI.operand[0].value = o1.value;
    AI.operand[0].type = SHORT_CONSTANT_OT;
    AI.operand[0].marker = m;
    AI.operand[1] = o2;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    assemblez_instruction(&AI);
}

void assemblez_jump(int n)
{   assembly_operand AO;
    if (n==-4) assemblez_0(rtrue_zc);
    else if (n==-3) assemblez_0(rfalse_zc);
    else
    {   AO.type = LONG_CONSTANT_OT; AO.value = n; AO.marker = 0;
        assemblez_1(jump_zc, AO);
    }
}

void assembleg_0(int internal_number)
{   AI.internal_number = internal_number;
    AI.operand_count = 0;
    assembleg_instruction(&AI);
}

void assembleg_1(int internal_number, assembly_operand o1)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    assembleg_instruction(&AI);
}

void assembleg_2(int internal_number, assembly_operand o1,
  assembly_operand o2)
{   AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    assembleg_instruction(&AI);
}

void assembleg_3(int internal_number, assembly_operand o1,
  assembly_operand o2, assembly_operand o3)
{   AI.internal_number = internal_number;
    AI.operand_count = 3;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    assembleg_instruction(&AI);
}

void assembleg_4(int internal_number, assembly_operand o1,
  assembly_operand o2, assembly_operand o3,
  assembly_operand o4)
{   AI.internal_number = internal_number;
    AI.operand_count = 4;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    assembleg_instruction(&AI);
}

void assembleg_5(int internal_number, assembly_operand o1,
  assembly_operand o2, assembly_operand o3,
  assembly_operand o4, assembly_operand o5)
{   AI.internal_number = internal_number;
    AI.operand_count = 5;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2] = o3;
    AI.operand[3] = o4;
    AI.operand[4] = o5;
    assembleg_instruction(&AI);
}

void assembleg_0_branch(int internal_number,
    int label)
{
    AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0].type = CONSTANT_OT;
    AI.operand[0].value = label;
    AI.operand[0].marker = BRANCH_MV;
    assembleg_instruction(&AI);
}

void assembleg_1_branch(int internal_number,
    assembly_operand o1, int label)
{
    /* Some clever optimizations first. A constant is always or never equal
       to zero. */
    if (o1.marker == 0 && is_constant_ot(o1.type)) {
        if ((internal_number == jz_gc && o1.value == 0)
          || (internal_number == jnz_gc && o1.value != 0)) {
            assembleg_0_branch(jump_gc, label);
            /* We clear the "can't reach statement" flag here, 
               so that "if (1)" doesn't produce that warning. */
            execution_never_reaches_here = 0;
            return;
        }
        if ((internal_number == jz_gc && o1.value != 0)
          || (internal_number == jnz_gc && o1.value == 0)) {
            /* assemble nothing at all! */
            return;
        }
    }
    AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1].type = CONSTANT_OT;
    AI.operand[1].value = label;
    AI.operand[1].marker = BRANCH_MV;
    assembleg_instruction(&AI);
}

void assembleg_2_branch(int internal_number,
    assembly_operand o1, assembly_operand o2, int label)
{
    AI.internal_number = internal_number;
    AI.operand_count = 3;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    AI.operand[2].type = CONSTANT_OT;
    AI.operand[2].value = label;
    AI.operand[2].marker = BRANCH_MV;
    assembleg_instruction(&AI);
}

void assembleg_call_1(assembly_operand oaddr, assembly_operand o1, 
  assembly_operand odest)
{
  assembleg_3(callfi_gc, oaddr, o1, odest);
}

void assembleg_call_2(assembly_operand oaddr, assembly_operand o1, 
  assembly_operand o2, assembly_operand odest)
{
  assembleg_4(callfii_gc, oaddr, o1, o2, odest);
}

void assembleg_call_3(assembly_operand oaddr, assembly_operand o1, 
  assembly_operand o2, assembly_operand o3, assembly_operand odest)
{
  assembleg_5(callfiii_gc, oaddr, o1, o2, o3, odest);
}

void assembleg_inc(assembly_operand o1)
{
  AI.internal_number = add_gc;
  AI.operand_count = 3;
  AI.operand[0] = o1;
  AI.operand[1] = one_operand;
  AI.operand[2] = o1;
  assembleg_instruction(&AI);
}

void assembleg_dec(assembly_operand o1)
{
  AI.internal_number = sub_gc;
  AI.operand_count = 3;
  AI.operand[0] = o1;
  AI.operand[1] = one_operand;
  AI.operand[2] = o1;
  assembleg_instruction(&AI);
}

void assembleg_store(assembly_operand o1, assembly_operand o2)
{
    /* Note the order is reversed: "o1 = o2;" */
    assembleg_2(copy_gc, o2, o1);
}

void assembleg_jump(int n)
{
  if (n==-4) {
      assembleg_1(return_gc, one_operand);
  }
  else if (n==-3) {
      assembleg_1(return_gc, zero_operand); 
  }
  else {
      assembleg_0_branch(jump_gc, n);
  }
}

uint32 get_wasm_type(uint32 no_params, uint32 no_returns) {
  uint32 i;

  for (i=0; i < no_wasm_types; i++)
    if (wasm_types[i].no_params == no_params && wasm_types[i].no_returns == no_returns)
      return i;

  ensure_memory_list_available(&wasm_types_memlist, ++no_wasm_types);
  wasm_types[i].no_params  = no_params;
  wasm_types[i].no_returns = no_returns;

  return i;
}

void assemblew_0(int internal_number)
{   AI.internal_number = internal_number;
    AI.operand_count = 0;
    assemblew_instruction(&AI);
}

void assemblew_1(int internal_number, assembly_operand o1)
{   AI.internal_number = internal_number;
    AI.operand_count = 1;
    AI.operand[0] = o1;
    assemblew_instruction(&AI);
}

void assemblew_2(int internal_number, assembly_operand o1,
  assembly_operand o2)
{   AI.internal_number = internal_number;
    AI.operand_count = 2;
    AI.operand[0] = o1;
    AI.operand[1] = o2;
    assemblew_instruction(&AI);
}

void assemblew_load(assembly_operand o1)
{
    switch (o1.type) {
	case STACK_OT:
	    /* it's already on the stack */
	    break;

	case CONSTANT_OT:
	    assemblew_1(i32_const_wc, o1);
	    break;

	case LOCALVAR_OT:
	    assemblew_1(local_get_wc, o1);
	    break;

	default:
	    printf("%d %d\n", o1.type, o1.value);
	    WABORT;
    }
}

void assemblew_tee(assembly_operand o1)
{
    switch (o1.type) {
	case STACK_OT:
	    /* it's already on the stack */
	    break;
	
	case LOCALVAR_OT:
	    assemblew_1(local_tee_wc, o1);
            break;

	default:
	    printf("%d\n", o1.type);
	    WABORT;
    }
}
void assemblew_store(assembly_operand o1)
{
    switch (o1.type) {
	case STACK_OT:
	    /* it's already on the stack */
	    break;
	
	case LOCALVAR_OT:
	    assemblew_1(local_set_wc, o1);
            break;

	default:
	    printf("%d\n", o1.type);
	    WABORT;
    }
}

void assemblew_begin_block(int label, assembly_operand type) {
    push_memory_stack(&blocks_stack);
    blocks_top->type = block_wc;
    blocks_top->label = label;
    blocks_top->push_true = type.value == p1rv_operand.value;
    assemblew_1(block_wc, type);
}

void assemblew_begin_loop(int label, assembly_operand type) {
    push_memory_stack(&blocks_stack);
    blocks_top->type = loop_wc;
    blocks_top->label = label;
    blocks_top->push_true = type.value == p1rv_operand.value;
    assemblew_1(loop_wc, type);
    assemble_label_no(label);
}

void assemblew_branch(int internal_number, int label)
{   
    int index = -1;
    block *p;
   
    if (label < -2) {
	if (internal_number == br_wc) {
            assemblew_load(label == -3 ? zero_operand : one_operand);
	    assemblew_0(return_wc);
	    return;
	}

	/* special index for return */
	index = blocks_stack.no_items;
    }
    else {
	if (!blocks_top) {
            compiler_error("Tried to branch outside of any block.");
	    return;
        }

    
        for (p = blocks_top; p >= blocks_bottom; p--) {
            index++;
            if (p->label == label) {
		if (p->push_true)
		    assemblew_load(one_operand);
                break;
	    }
        }
    
        if (p < blocks_bottom) {
            char buf[80];
            snprintf(buf, 80, "Tried to branch to L%d, but its block does not exist or code is outside of it.", label);
            compiler_error(buf);
            return;
        }
    }
	
    AI.internal_number = internal_number;
    AI.operand_count = 1;

    INITAOTV(AI.operand, BLOCK_OT, index);
    assemblew_instruction(&AI);

    if (label < -2)
	assemblew_0(drop_wc); /* potential return value */
}

extern void assemblew_end_loop(int label) {
    if (!blocks_top || blocks_top->label != label || blocks_top->type != loop_wc) {
	char buf[80];
	if (blocks_top)
	    snprintf(buf, 80, "Tried to end wrong loop L%d. (Expecting L%d.)", label, blocks_top->label);
	else
	    snprintf(buf, 80, "Tried to end wrong loop L%d. (No active blocks.)", label);
        compiler_error(buf);
	return;
    }
    assemblew_0(end_wc);
    pop_memory_stack(&blocks_stack);
}

extern void assemblew_end_block(int label) {
    if (!blocks_top || blocks_top->label != label || blocks_top->type != block_wc) {
	char buf[80];
	if (blocks_top)
	    snprintf(buf, 80, "Tried to end wrong block L%d. (Expecting L%d.)", label, blocks_top->label);
	else
	    snprintf(buf, 80, "Tried to end wrong block L%d. (No active blocks.)", label);
        compiler_error(buf);
	return;
    }
    assemblew_0(end_wc);
    assemble_label_no(blocks_top->label);
    pop_memory_stack(&blocks_stack);
}

void assemblew_begin_if(int label, assembly_operand type) {
    push_memory_stack(&blocks_stack);
    blocks_top->type = if_wc;
    blocks_top->label = label;
    blocks_top->push_true = type.value == p1rv_operand.value;
    assemblew_1(if_wc, type);
}

extern void assemblew_else(int label) {
    if (!blocks_top || blocks_top->label != label || blocks_top->type != if_wc) {
	char buf[80];
	if (blocks_top)
	    snprintf(buf, 80, "Tried to assemble else for L%d without matching if. (Expecting L%d.)", label, blocks_top->label);
	else
	    snprintf(buf, 80, "Tried to assemble else for L%d without matching if. (No active blocks.)", label);
        compiler_error(buf);
	return;
    }
    assemblew_0(else_wc);
    blocks_top->type = else_wc;
}

extern void assemblew_end_if(int label) {
    if (!blocks_top || blocks_top->label != label || blocks_top->type != if_wc && blocks_top->type != else_wc ) {
	char buf[80];
	if (blocks_top)
	    snprintf(buf, 80, "Tried to end wrong if/else L%d. (Expecting L%d.)", label, blocks_top->label);
	else
	    snprintf(buf, 80, "Tried to end wrong if/else L%d. (No active blocks.)", label);
        compiler_error(buf);
	return;
    }
    assemblew_0(end_wc);
    assemble_label_no(blocks_top->label);
    pop_memory_stack(&blocks_stack);
}


/* ========================================================================= */
/*   Parsing and then calling the assembler for @ (assembly language)        */
/*   statements                                                              */
/* ------------------------------------------------------------------------- */

static assembly_operand parse_operand_z(void)
{   assembly_operand AO;

    AO = parse_expression(ASSEMBLY_CONTEXT);
    if (AO.type == EXPRESSION_OT)
    {   ebf_error("variable or constant", "expression");
        AO.type = SHORT_CONSTANT_OT;
    }
    return(AO);
}

static void parse_assembly_z(void)
{   int n, min, max, indirect_addressed, error_flag = FALSE;
    opcodez O;

    AI.operand_count = 0;
    AI.store_variable_number = -1;
    AI.branch_label_number = -1;
    AI.text = NULL;

    opcode_names.enabled = TRUE;
    get_next_token();
    opcode_names.enabled = FALSE;

    if (token_type == DQ_TT)
    {   int i;
        AI.internal_number = -1;

        custom_opcode_z.name = (uchar *) token_text;
        custom_opcode_z.version1 = instruction_set_number;
        custom_opcode_z.version2 = instruction_set_number;
        custom_opcode_z.extension = -1;
        custom_opcode_z.flags = 0;
        custom_opcode_z.op_rules = 0;
        custom_opcode_z.flags2_set = 0;
        custom_opcode_z.no = ZERO;

        for (i=0; token_text[i]!=0; i++)
        {   if (token_text[i] == ':')
            {   token_text[i++] = 0;
                break;
            }
        }
        if (token_text[i] == 0)
            error("Opcode specification should have form \"VAR:102\"");

        n = -1;
        if (strcmp(token_text, "0OP")==0)      n=ZERO;
        if (strcmp(token_text, "1OP")==0)      n=ONE;
        if (strcmp(token_text, "2OP")==0)      n=TWO;
        if (strcmp(token_text, "VAR")==0)      n=VAR;
        if (strcmp(token_text, "EXT")==0)      n=EXT;
        if (strcmp(token_text, "VAR_LONG")==0) n=VAR_LONG;
        if (strcmp(token_text, "EXT_LONG")==0) n=EXT_LONG;

        if (i>0) token_text[i-1] = ':';

        if (n==-1)
        {   ebf_error("Expected 0OP, 1OP, 2OP, VAR, EXT, VAR_LONG or EXT_LONG",
                token_text);
            n = EXT;
        }
        custom_opcode_z.no = n;

        custom_opcode_z.code = atoi(token_text+i);
        while (isdigit(token_text[i])) i++;

        {   max = 0; min = 0;
            switch(n)
            {   case ZERO: case ONE: max = 16; break;
                case VAR: case VAR_LONG: min = 32; max = 64; break;
                case EXT: case EXT_LONG: max = 256; break;
                case TWO: max = 32; break;
            }
            if ((custom_opcode_z.code < min) || (custom_opcode_z.code >= max))
            {   char range[32];
                sprintf(range, "%d to %d", min, max-1);
            error_named("For this operand type, opcode number must be in range",
                    range);
                custom_opcode_z.code = min;
            }
        }

        while (token_text[i++] != 0)
        {   switch(token_text[i-1])
            {   case 'B': custom_opcode_z.flags |= Br; break;
                case 'S': custom_opcode_z.flags |= St; break;
                case 'T': custom_opcode_z.op_rules = TEXT; break;
                case 'I': custom_opcode_z.op_rules = VARIAB; break;
                case 'F': custom_opcode_z.flags2_set = atoi(token_text+i);
                          while (isdigit(token_text[i])) i++; break;
                default:
                    error("Unknown flag: options are B (branch), S (store), \
T (text), I (indirect addressing), F** (set this Flags 2 bit)");
                    break;
            }
        }
        O = custom_opcode_z;
    }
    else
    {   if (token_type != OPCODE_NAME_TT)
        {   ebf_error("an opcode name", token_text);
            panic_mode_error_recovery();
            return;
        }
        AI.internal_number = token_value;
        O = internal_number_to_opcode_z(AI.internal_number);
    }

    indirect_addressed = (O.op_rules == VARIAB);

    if (O.op_rules == TEXT)
    {   get_next_token();
        if (token_type != DQ_TT)
            ebf_error("literal text in double-quotes", token_text);
        AI.text = token_text;
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) return;
        get_next_token();
        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
        {   assemblez_instruction(&AI);
            return;
        }
        ebf_error("semicolon ';' after print string", token_text);
        put_token_back();
        return;
    }

    return_sp_as_variable = TRUE;
    do
    {   get_next_token();

        if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) break;

        if ((token_type == SEP_TT) && (token_value == ARROW_SEP))
        {   if (AI.store_variable_number != -1)
                error("Only one '->' store destination can be given");
            get_next_token();
            if ((token_type != SYMBOL_TT)
                && (token_type != LOCAL_VARIABLE_TT))
                ebf_error("variable name or 'sp'", token_text);
            n = 255;
            if (token_type == LOCAL_VARIABLE_TT) n = token_value;
            else
            {   if (strcmp(token_text, "sp") == 0) n = 0;
                else
                {   if (stypes[token_value] != GLOBAL_VARIABLE_T)
                        error_named(
                            "Store '->' destination not 'sp' or a variable:",
                            token_text);
                    else n = svals[token_value];
                }
            }
            AI.store_variable_number = n;
            continue;
        }

        if ((token_type == SEP_TT) &&
            ((token_value == BRANCH_SEP) || (token_value == NBRANCH_SEP)))
        {   if (AI.branch_label_number != -1)
                error("Only one '?' branch destination can be given");

            AI.branch_flag = (token_value == BRANCH_SEP);

            opcode_names.enabled = TRUE;
            get_next_token();
            opcode_names.enabled = FALSE;

            n = -2;
            if ((token_type == OPCODE_NAME_TT)
                && (token_value == rfalse_zc)) n = -3;
            else
            if ((token_type == OPCODE_NAME_TT)
                && (token_value == rtrue_zc)) n = -4;
            else
            {   if (token_type == SYMBOL_TT)
                {   put_token_back();
                    n = parse_label();
                }
                else
                    ebf_error("label name after '?' or '?~'", token_text);
            }
            AI.branch_label_number = n;
            continue;
        }

        if (AI.operand_count == 8)
        {   error("No assembly instruction may have more than 8 operands");
            panic_mode_error_recovery(); break;
        }

        if ((token_type == SEP_TT) && (token_value == OPEN_SQUARE_SEP))
        {   if (!indirect_addressed)
                error("This opcode does not use indirect addressing");
            if (AI.operand_count > 0)
            error("Indirect addressing can only be used on the first operand");
            AI.operand[AI.operand_count++] = parse_operand_z();
            get_next_token();
            if (!((token_type == SEP_TT) && (token_value == CLOSE_SQUARE_SEP)))
            {   ebf_error("']'", token_text);
                put_token_back();
            }
        }
        else
        {   put_token_back();
            AI.operand[AI.operand_count++] = parse_operand_z();
            if ((indirect_addressed) && (AI.operand_count == 1)
                && (AI.operand[AI.operand_count-1].type == VARIABLE_OT))
            {   AI.operand[AI.operand_count-1].type = SHORT_CONSTANT_OT;
                AI.operand[AI.operand_count-1].marker = VARIABLE_MV;
            }
        }

    } while (TRUE);

    return_sp_as_variable = FALSE;


    if (O.version1 == 0)
    {   error_named("Opcode unavailable in this Z-machine version:",
            opcode_names.keywords[AI.internal_number]);
        return;
    }

    if (((O.flags) & Br) != 0)
    {   if (AI.branch_label_number == -1)
        {   error_flag = TRUE;
            AI.branch_label_number = -2;
        }
    }
    else
    {   if (AI.branch_label_number != -1)
        {   error_flag = TRUE;
            AI.branch_label_number = -1;
        }
    }
    if (((O.flags) & St) != 0)
    {   if (AI.store_variable_number == -1)
        {   if (AI.operand_count == 0)
            {   error_flag = TRUE;
                AI.store_variable_number = 255;
            }
            else
            {   AI.store_variable_number
                    = AI.operand[--AI.operand_count].value;
                if (AI.operand[AI.operand_count].type != VARIABLE_OT)
            error("Store destination (the last operand) is not a variable");
            }
        }
    }
    else
    {   if (AI.store_variable_number != -1)
        {   error_flag = TRUE;
            AI.store_variable_number = -1;
        }
    }

    min = 0; max = 0;
    switch(O.no)
    {   case TWO:      min = 2; max = 2;
                       /* Exception for the V6 set_colour, which can take
                          a third argument, thus forcing it into VAR form: */
                       if ((version_number == 6) && (O.code == 0x1b)) max = 3;
                       /* Also an exception for je, which can take from 1
                          argument (useless) to 4 arguments */
                       if (O.code == 0x01) { min = 1; max = 4; }
                       break;
        case VAR:      min = 0; max = 4; break;
        case VAR_LONG: min = 0; max = 8; break;
        case ONE:      min = 1; max = 1; break;
        case ZERO:     min = 0; max = 0; break;
        case EXT:      min = 0; max = 4; break;
        case EXT_LONG: min = 0; max = 8; break;
    }

    if ((AI.operand_count >= min) && (AI.operand_count <= max))
        assemblez_instruction(&AI);
    else error_flag = TRUE;

    if (error_flag)
    {   make_opcode_syntax_z(O);
        error_named("Assembly mistake: syntax is",
            opcode_syntax_string);
    }
}

static assembly_operand parse_operand_g(void)
{   assembly_operand AO;

    AO = parse_expression(ASSEMBLY_CONTEXT);
    if (AO.type == EXPRESSION_OT)
    {   ebf_error("variable or constant", "expression");
        AO.type = CONSTANT_OT;
    }
    return(AO);
}

static void parse_assembly_g(void)
{
  opcodeg O;
  assembly_operand AO;
  int error_flag = FALSE, is_macro = FALSE;

  AI.operand_count = 0;

  opcode_names.enabled = TRUE;
  opcode_macros.enabled = TRUE;
  get_next_token();
  opcode_names.enabled = FALSE;
  opcode_macros.enabled = FALSE;

  if (token_type == DQ_TT) {
    char *cx;
    int badflags;

    AI.internal_number = -1;

    /* The format is @"FlagsCount:Code". Flags (which are optional)
       can include "S" for store, "SS" for two stores, "B" for branch
       format, "R" if execution never continues after the opcode. The
       Count is the number of arguments (currently limited to 0-9),
       and the Code is a decimal integer representing the opcode
       number.

       So: @"S3:123" for a three-argument opcode (load, load, store)
       whose opcode number is (decimal) 123. Or: @"2:234" for a
       two-argument opcode (load, load) whose number is 234. */

    custom_opcode_g.name = (uchar *) token_text;
    custom_opcode_g.flags = 0;
    custom_opcode_g.op_rules = 0;
    custom_opcode_g.no = 0;

    badflags = FALSE;

    for (cx = token_text; *cx && *cx != ':'; cx++) {
      if (badflags)
      continue;

      switch (*cx) {
      case 'S':
      if (custom_opcode_g.flags & St)
        custom_opcode_g.flags |= St2;
      else
        custom_opcode_g.flags |= St;
      break;
      case 'B':
      custom_opcode_g.flags |= Br;
      break;
      case 'R':
      custom_opcode_g.flags |= Rf;
      break;
      default:
      if (isdigit(*cx)) {
        custom_opcode_g.no = (*cx) - '0';
        break;
      }
      badflags = TRUE;
      error("Unknown custom opcode flag: options are B (branch), \
S (store), SS (two stores), R (execution never continues)");
      break;
      }
    }

    if (*cx != ':') {
      error("Custom opcode must have colon");
    }
    else {
      cx++;
      if (!(*cx))
      error("Custom opcode must have colon followed by opcode number");
      else
      custom_opcode_g.code = atoi(cx);
    }

    O = custom_opcode_g;
  }
  else {
    if (token_type != OPCODE_NAME_TT && token_type != OPCODE_MACRO_TT) {
      ebf_error("an opcode name", token_text);
      panic_mode_error_recovery();
      return;
    }
    AI.internal_number = token_value;
    if (token_type == OPCODE_MACRO_TT) {
      O = internal_number_to_opmacro_g(AI.internal_number);
      is_macro = TRUE;
    }
    else
      O = internal_number_to_opcode_g(AI.internal_number);
  }
  
  return_sp_as_variable = TRUE;

  while (1) {
    get_next_token();
    
    if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)) 
      break;

    if (AI.operand_count == 8) {
      error("No assembly instruction may have more than 8 operands");
      panic_mode_error_recovery(); 
      break;
    }

    if ((O.flags & Br) && (AI.operand_count == O.no-1)) {
      if (!((token_type == SEP_TT) && (token_value == BRANCH_SEP))) {
        error_flag = TRUE;
        error("Branch opcode must have '?' label");
        put_token_back();
      }
      AO.type = CONSTANT_OT;
      AO.value = parse_label();
      AO.marker = BRANCH_MV;
    }
    else {
      put_token_back();
      AO = parse_operand_g();
    }

    AI.operand[AI.operand_count] = AO;
    AI.operand_count++;
  }

  return_sp_as_variable = FALSE;

  if (O.no != AI.operand_count) {
    error_flag = TRUE;
  }

  if (!error_flag) {
    if (is_macro)
      assembleg_macro(&AI);
    else
      assembleg_instruction(&AI);
  }

  if (error_flag) {
    make_opcode_syntax_g(O);
    error_named("Assembly mistake: syntax is",
      opcode_syntax_string);
  }
}

extern void parse_assembly(void)
{
  switch (target_machine) {
    case TARGET_ZCODE:
    parse_assembly_z();
    break;

    case TARGET_GLULX:
    parse_assembly_g();
    break;

    case TARGET_WASM:
    WABORT;
  }
}

/* ========================================================================= */
/*   Data structure management routines                                      */
/* ------------------------------------------------------------------------- */

extern void asm_begin_pass(void)
{   no_instructions = 0;
    zmachine_pc = 0;
    no_sequence_points = 0;
    next_label = 0;
    next_sequence_point = 0;
    zcode_ha_size = 0;
}

extern void init_asm_vars(void)
{   int i;

    for (i=0;i<16;i++) flags2_requirements[i]=0;

    uses_unicode_features = FALSE;
    uses_memheap_features = FALSE;
    uses_acceleration_features = FALSE;
    uses_float_features = FALSE;

    sequence_point_follows = TRUE;
    label_moved_error_already_given = FALSE;

    initialise_memory_block(&zcode_area);
    if (target_machine == TARGET_WASM) {
        initialise_memory_list(&wasm_types_memlist,
            sizeof(wasm_type), 20, (void**)&wasm_types,
            "WebAssembly type definitions");
	no_wasm_types = 0;
    }
}

extern void asm_allocate_arrays(void)
{   if ((debugfile_switch) && (MAX_LABELS < 2000)) MAX_LABELS = 2000;

    initialise_memory_list(&variables_memlist,
        sizeof(variableinfo), 200, (void**)&variables,
        "variables");

    label_offsets = my_calloc(sizeof(int32), MAX_LABELS, "label offsets");
    label_symbols = my_calloc(sizeof(int32), MAX_LABELS, "label symbols");
    label_next = my_calloc(sizeof(int), MAX_LABELS, "label dll 1");
    label_prev = my_calloc(sizeof(int), MAX_LABELS, "label dll 1");
    sequence_point_labels
        = my_calloc(sizeof(int), MAX_LABELS, "sequence point labels");
    sequence_point_locations
        = my_calloc(sizeof(debug_location),
                    MAX_LABELS,
                    "sequence point locations");

    zcode_holding_area = my_malloc(MAX_ZCODE_SIZE,"compiled routine code area");
    zcode_markers = my_malloc(MAX_ZCODE_SIZE, "compiled routine code area");

    named_routine_symbols
        = my_calloc(sizeof(int32), MAX_SYMBOLS, "named routine symbols");

    if (target_machine == TARGET_WASM)
	initialise_memory_stack(&blocks_stack, sizeof(block), 5, (void **)&blocks_top, (void **)&blocks_bottom, "WebAssembly block stack");
}

extern void asm_free_arrays(void)
{
    deallocate_memory_list(&variables_memlist);

    my_free(&label_offsets, "label offsets");
    my_free(&label_symbols, "label symbols");
    my_free(&label_next, "label dll 1");
    my_free(&label_prev, "label dll 2");
    my_free(&sequence_point_labels, "sequence point labels");
    my_free(&sequence_point_locations, "sequence point locations");

    my_free(&zcode_holding_area, "compiled routine code area");
    my_free(&zcode_markers, "compiled routine code markers");

    my_free(&named_routine_symbols, "named routine symbols");
    deallocate_memory_block(&zcode_area);
    
    if (target_machine == TARGET_WASM) {
	deallocate_memory_stack(&blocks_stack);
	deallocate_memory_list(&wasm_types_memlist);
    }
}

/* ========================================================================= */
