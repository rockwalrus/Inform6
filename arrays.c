/* ------------------------------------------------------------------------- */
/*   "arrays" :  Parses array declarations and constructs arrays from them;  */
/*               likewise global variables, which are in some ways a         */
/*               simpler form of the same thing.                             */
/*                                                                           */
/*   Part of Inform 6.36                                                     */
/*   copyright (c) Graham Nelson 1993 - 2021                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#include "header.h"

/* ------------------------------------------------------------------------- */
/*   Arrays defined below:                                                   */
/*                                                                           */
/*    uchar  dynamic_array_area[]         Initial values for the bytes of    */
/*                                        the dynamic array area             */
/*    uchar  static_array_area[]          Initial values for the bytes of    */
/*                                        the static array area              */
/*    int32  global_initial_value[n]      The initialised value of the nth   */
/*                                        global variable (counting 0 - 239, */
/*                                        or higher for Glulx)               */
/*                                                                           */
/*   The "dynamic array area" is the Z-machine area holding the current      */
/*   values of the global variables (in 240x2 = 480 bytes) followed by any   */
/*   (dynamic) arrays which may be defined.  Owing to a poor choice of name  */
/*   some years ago, this is also called the "static data area", which is    */
/*   why the memory setting for its maximum extent is "MAX_STATIC_DATA".     */
/*                                                                           */
/*   In Glulx, we don't keep the global variables in dynamic_array_area.     */
/*   Array data starts at the start.                                         */
/*                                                                           */
/*   We can also store arrays (but not globals) into static memory (ROM).    */
/*   The storage for these goes, unsurprisingly, into static_array_area --   */
/*   a separate allocation of MAX_STATIC_DATA bytes.                         */
/* ------------------------------------------------------------------------- */
uchar   *dynamic_array_area;           /* See above                          */
memory_list dynamic_array_area_memlist;
int dynamic_array_area_size;           /* Size in bytes                      */

int32   *global_initial_value;         /* Allocated to no_globals            */
static memory_list global_initial_value_memlist;

int no_globals;                        /* Number of global variables used
                                          by the programmer (Inform itself
                                          uses the top seven -- but these do
                                          not count)                         */
                                       /* In Glulx, Inform uses the bottom 
                                          ten.                               */

uchar   *static_array_area;
memory_list static_array_area_memlist;
int static_array_area_size;

int no_arrays;
arrayinfo *arrays;
static memory_list arrays_memlist;

static int array_entry_size,           /* 1 for byte array, 2 for word array */
           array_base;                 /* Offset in dynamic array area of the
                                          array being constructed.  During the
                                          same time, dynamic_array_area_size
                                          is the offset of the initial entry
                                          in the array: so for "table" and
                                          "string" arrays, these numbers are
                                          different (by 2 and 1 bytes resp)  */

                                       /* In Glulx, of course, that will be
                                          4 instead of 2.                    */

/* Complete the array. Fill in the size field (if it has one) and 
   advance foo_array_area_size.
*/
extern void finish_array(int32 i, int is_static)
{
  uchar *area;
  int area_size;
  
  if (!is_static) {
      ensure_memory_list_available(&dynamic_array_area_memlist, dynamic_array_area_size+array_base+1*array_entry_size);
      area = dynamic_array_area;
      area_size = dynamic_array_area_size;
  }
  else {
      ensure_memory_list_available(&static_array_area_memlist, static_array_area_size+array_base+1*array_entry_size);
      area = static_array_area;
      area_size = static_array_area_size;
  }

  if (i == 0) {
      error("An array must have at least one entry");
  }
  
    /*  Write the array size into the 0th byte/word of the array, if it's
        a "table" or "string" array                                          */
  if (target_machine == TARGET_ZCODE) {

    if (array_base != area_size)
    {   if (area_size-array_base==2)
        {   area[array_base]   = i/256;
            area[array_base+1] = i%256;
        }
        else
        {   if (i>=256)
                error("A 'string' array can have at most 256 entries");
            area[array_base] = i;
        }
    }

  }
  else {
    if (array_base != area_size)
    {   if (area_size-array_base==4)
        {   
            area[array_base]   = (i >> 24) & 0xFF;
            area[array_base+1] = (i >> 16) & 0xFF;
            area[array_base+2] = (i >> 8) & 0xFF;
            area[array_base+3] = (i) & 0xFF;
        }
        else
        {   if (i>=256)
                error("A 'string' array can have at most 256 entries");
            area[array_base] = i;
        }
    }
    
  }

  /*  Move on the static/dynamic array size so that it now points to the
      next available free space                                              */

  if (!is_static) {
      dynamic_array_area_size += i*array_entry_size;
  }
  else {
      static_array_area_size += i*array_entry_size;
  }

}

/* Fill in array entry i (in either the static or dynamic area).
   When this is called, foo_array_area_size is the end of the previous
   array; we're writing after that.
*/
extern void array_entry(int32 i, int is_static, assembly_operand VAL)
{
  uchar *area;
  int area_size;
  
  if (!is_static) {
      ensure_memory_list_available(&dynamic_array_area_memlist, dynamic_array_area_size+(i+1)*array_entry_size);
      area = dynamic_array_area;
      area_size = dynamic_array_area_size;
  }
  else {
      ensure_memory_list_available(&static_array_area_memlist, static_array_area_size+(i+1)*array_entry_size);
      area = static_array_area;
      area_size = static_array_area_size;
  }
  
  if (target_machine == TARGET_ZCODE) {
    /*  Array entry i (initial entry has i=0) is set to Z-machine value j    */

    if (array_entry_size==1)
    {   area[area_size+i] = (VAL.value)%256;

        if (VAL.marker != 0)
           error("Entries in byte arrays and strings must be known constants");

        /*  If the entry is too large for a byte array, issue a warning
            and truncate the value                                           */
        else
        if (VAL.value >= 256)
            warning("Entry in '->', 'string' or 'buffer' array not in range 0 to 255");
    }
    else
    {
        int32 addr = area_size + 2*i;
        area[addr]   = (VAL.value)/256;
        area[addr+1] = (VAL.value)%256;
        if (VAL.marker != 0) {
            if (!is_static) {
                backpatch_zmachine(VAL.marker, DYNAMIC_ARRAY_ZA,
                    addr);
            }
            else {
                backpatch_zmachine(VAL.marker, STATIC_ARRAY_ZA,
                    addr);
            }
        }
    }
  }
  else {
    /*  Array entry i (initial entry has i=0) is set to value j              */

    if (array_entry_size==1)
    {   area[area_size+i] = (VAL.value) & 0xFF;

        if (VAL.marker != 0)
           error("Entries in byte arrays and strings must be known constants");

        /*  If the entry is too large for a byte array, issue a warning
            and truncate the value                                           */
        else
        if (VAL.value >= 256)
            warning("Entry in '->', 'string' or 'buffer' array not in range 0 to 255");
    }
    else if (array_entry_size==4)
    {
        int32 addr = area_size + 4*i;
        area[addr]   = (VAL.value >> 24) & 0xFF;
        area[addr+1] = (VAL.value >> 16) & 0xFF;
        area[addr+2] = (VAL.value >> 8) & 0xFF;
        area[addr+3] = (VAL.value) & 0xFF;
        if (VAL.marker != 0) {
            if (!is_static) {
                backpatch_zmachine(VAL.marker, DYNAMIC_ARRAY_ZA,
                    addr);
            }
            else {
                /* We can't use backpatch_zmachine() because that only applies to RAM. Instead we add an entry to staticarray_backpatch_table.
                   A backpatch entry is five bytes: *_MV followed by the array offset (in static array area). */
                write_byte_to_memory_block(&staticarray_backpatch_table,
                    staticarray_backpatch_size++,
                    VAL.marker);
                write_byte_to_memory_block(&staticarray_backpatch_table,
                    staticarray_backpatch_size++, ((addr >> 24) & 0xFF));
                write_byte_to_memory_block(&staticarray_backpatch_table,
                    staticarray_backpatch_size++, ((addr >> 16) & 0xFF));
                write_byte_to_memory_block(&staticarray_backpatch_table,
                    staticarray_backpatch_size++, ((addr >> 8) & 0xFF));
                write_byte_to_memory_block(&staticarray_backpatch_table,
                    staticarray_backpatch_size++, (addr & 0xFF));
            }
        }
    }
    else
    {
        error("Somehow created an array of shorts");
    }
  }
}

/* ------------------------------------------------------------------------- */
/*   Global and Array directives.                                            */
/*                                                                           */
/*      Global <variablename> |                                              */
/*                            | = <value>                                    */
/*                            | <array specification>                        */
/*                                                                           */
/*      Array <arrayname> [static] <array specification>                     */
/*                                                                           */
/*   where an array specification is:                                        */
/*                                                                           */
/*      | ->       |  <number-of-entries>                                    */
/*      | -->      |  <entry-1> ... <entry-n>                                */
/*      | string   |  [ <entry-1> [;] <entry-2> ... <entry-n> ];             */
/*      | table                                                              */
/*      | buffer                                                             */
/*                                                                           */
/*   The "static" keyword (arrays only) places the array in static memory.   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

extern void set_variable_value(int i, int32 v)
{
    /* This can be called during module-load to create a new global,
       so we call ensure. */
    ensure_memory_list_available(&global_initial_value_memlist, i+1);
    global_initial_value[i]=v;
}

/*  There are four ways to initialise arrays:                                */

#define UNSPECIFIED_AI  -1
#define NULLS_AI        0
#define DATA_AI         1
#define ASCII_AI        2
#define BRACKET_AI      3

extern void make_global(int array_flag, int name_only)
{
    /*  array_flag is TRUE for an Array directive, FALSE for a Global;
        name_only is only TRUE for parsing an imported variable name, so
        array_flag is always FALSE in that case.                             */

    int32 i;
    int array_type, data_type;
    int is_static = FALSE;
    assembly_operand AO;
    
    int extraspace;

    int32 global_symbol;
    const char *global_name;
    debug_location_beginning beginning_debug_location =
        get_token_location_beginning();

    directive_keywords.enabled = FALSE;
    get_next_token();
    i = token_value;
    global_symbol = i;
    global_name = token_text;

    if (target_machine == TARGET_ZCODE) {
        if ((token_type==SYMBOL_TT) && (stypes[i]==GLOBAL_VARIABLE_T)
            && (svals[i] >= LOWEST_SYSTEM_VAR_NUMBER))
            goto RedefinitionOfSystemVar;
    }
    else {
        if ((token_type==SYMBOL_TT) && (stypes[i]==GLOBAL_VARIABLE_T))
            goto RedefinitionOfSystemVar;
    }

    if (token_type != SYMBOL_TT)
    {   discard_token_location(beginning_debug_location);
        if (array_flag)
            ebf_error("new array name", token_text);
        else ebf_error("new global variable name", token_text);
        panic_mode_error_recovery(); return;
    }

    if (!(sflags[i] & UNKNOWN_SFLAG))
    {   discard_token_location(beginning_debug_location);
        if (array_flag)
            ebf_symbol_error("new array name", token_text, typename(stypes[i]), slines[i]);
        else ebf_symbol_error("new global variable name", token_text, typename(stypes[i]), slines[i]);
        panic_mode_error_recovery(); return;
    }

    if ((!array_flag) && (sflags[i] & USED_SFLAG))
        error_named("Variable must be defined before use:", token_text);

    directive_keywords.enabled = TRUE;
    get_next_token();
    directive_keywords.enabled = FALSE;
    if ((token_type==DIR_KEYWORD_TT)&&(token_value==STATIC_DK)) {
        if (array_flag) {
            is_static = TRUE;
        }
        else {
            error("Global variables cannot be static");
        }
    }
    else {
        put_token_back();
    }
    
    if (array_flag)
    {   if (!is_static) {
            assign_symbol(i, dynamic_array_area_size, ARRAY_T);
        }
        else {
            assign_symbol(i, static_array_area_size, STATIC_ARRAY_T);
        }
        ensure_memory_list_available(&arrays_memlist, no_arrays+1);
        arrays[no_arrays].symbol = i;
    }
    else
    {   if (target_machine == TARGET_ZCODE && no_globals==233)
        {   discard_token_location(beginning_debug_location);
            error("All 233 global variables already declared");
            panic_mode_error_recovery();
            return;
        }
        
        ensure_memory_list_available(&variables_memlist, MAX_LOCAL_VARIABLES+no_globals+1);
        variables[MAX_LOCAL_VARIABLES+no_globals].token = i;
        variables[MAX_LOCAL_VARIABLES+no_globals].usage = FALSE;
        assign_symbol(i, MAX_LOCAL_VARIABLES+no_globals, GLOBAL_VARIABLE_T);

        if (name_only) {
            import_symbol(i);
        }
        else {
            ensure_memory_list_available(&global_initial_value_memlist, no_globals+1);
            global_initial_value[no_globals++]=0;
        }
    }

    directive_keywords.enabled = TRUE;

    RedefinitionOfSystemVar:

    if (name_only)
    {   discard_token_location(beginning_debug_location);
        return;
    }

    get_next_token();

    if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
    {   if (array_flag)
        {   discard_token_location(beginning_debug_location);
            ebf_error("array definition", token_text);
        }
        put_token_back();
        if (debugfile_switch && !array_flag)
        {   debug_file_printf("<global-variable>");
            debug_file_printf("<identifier>%s</identifier>", global_name);
            debug_file_printf("<address>");
            write_debug_global_backpatch(svals[global_symbol]);
            debug_file_printf("</address>");
            write_debug_locations
                (get_token_location_end(beginning_debug_location));
            debug_file_printf("</global-variable>");
        }
        return;
    }

    if (!array_flag)
    {
        /* is_static is always false in this case */
        if ((token_type == SEP_TT) && (token_value == SETEQUALS_SEP))
        {   AO = parse_expression(CONSTANT_CONTEXT);
            if (target_machine == TARGET_ZCODE) {
                if (AO.marker != 0)
                    backpatch_zmachine(AO.marker, DYNAMIC_ARRAY_ZA,
                        2*(no_globals-1));
            }
            else {
            if (AO.marker != 0)
                backpatch_zmachine(AO.marker, GLOBALVAR_ZA,
                4*(no_globals-1));
            }
            global_initial_value[no_globals-1] = AO.value;
            if (debugfile_switch)
            {   debug_file_printf("<global-variable>");
                debug_file_printf("<identifier>%s</identifier>", global_name);
                debug_file_printf("<address>");
                write_debug_global_backpatch(svals[global_symbol]);
                debug_file_printf("</address>");
                write_debug_locations
                    (get_token_location_end(beginning_debug_location));
                debug_file_printf("</global-variable>");
            }
            return;
        }

        obsolete_warning("more modern to use 'Array', not 'Global'");

        if (target_machine == TARGET_ZCODE) {
            backpatch_zmachine(ARRAY_MV, DYNAMIC_ARRAY_ZA, 2*(no_globals-1));
            global_initial_value[no_globals-1]
                = dynamic_array_area_size+variables_offset;
        }
        else {
            backpatch_zmachine(ARRAY_MV, GLOBALVAR_ZA, 4*(no_globals-1));
            global_initial_value[no_globals-1]
                = dynamic_array_area_size;
        }
    }

    array_type = BYTE_ARRAY; data_type = UNSPECIFIED_AI;

         if ((!array_flag) &&
             ((token_type==DIR_KEYWORD_TT)&&(token_value==DATA_DK)))
                 data_type=NULLS_AI;
    else if ((!array_flag) &&
             ((token_type==DIR_KEYWORD_TT)&&(token_value==INITIAL_DK)))
                 data_type=DATA_AI;
    else if ((!array_flag) &&
             ((token_type==DIR_KEYWORD_TT)&&(token_value==INITSTR_DK)))
                 data_type=ASCII_AI;

    else if ((token_type==SEP_TT)&&(token_value==ARROW_SEP))
             array_type = BYTE_ARRAY;
    else if ((token_type==SEP_TT)&&(token_value==DARROW_SEP))
             array_type = WORD_ARRAY;
    else if ((token_type==DIR_KEYWORD_TT)&&(token_value==STRING_DK))
             array_type = STRING_ARRAY;
    else if ((token_type==DIR_KEYWORD_TT)&&(token_value==TABLE_DK))
             array_type = TABLE_ARRAY;
    else if ((token_type==DIR_KEYWORD_TT)&&(token_value==BUFFER_DK))
             array_type = BUFFER_ARRAY;
    else
    {   discard_token_location(beginning_debug_location);
        if (array_flag)
            ebf_error
              ("'->', '-->', 'string', 'table' or 'buffer'", token_text);
        else
            ebf_error
              ("'=', '->', '-->', 'string', 'table' or 'buffer'", token_text);
        panic_mode_error_recovery();
        return;
    }

    array_entry_size=1;
    if ((array_type==WORD_ARRAY) || (array_type==TABLE_ARRAY))
        array_entry_size=WORDSIZE;

    get_next_token();
    if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
    {   discard_token_location(beginning_debug_location);
        error("No array size or initial values given");
        put_token_back();
        return;
    }

    switch(data_type)
    {   case UNSPECIFIED_AI:
            if ((token_type == SEP_TT) && (token_value == OPEN_SQUARE_SEP))
                data_type = BRACKET_AI;
            else
            {   data_type = NULLS_AI;
                if (token_type == DQ_TT) data_type = ASCII_AI;
                get_next_token();
                if (!((token_type == SEP_TT) && (token_value == SEMICOLON_SEP)))
                    data_type = DATA_AI;
                put_token_back();
                put_token_back();
            }
            break;
        case NULLS_AI: obsolete_warning("use '->' instead of 'data'"); break;
        case DATA_AI:  obsolete_warning("use '->' instead of 'initial'"); break;
        case ASCII_AI: obsolete_warning("use '->' instead of 'initstr'"); break;
    }

    /*  Leave room to write the array size in later, if string/table array   */
    
    extraspace = 0;
    if ((array_type==STRING_ARRAY) || (array_type==TABLE_ARRAY))
        extraspace += array_entry_size;
    if (array_type==BUFFER_ARRAY)
        extraspace += WORDSIZE;
    
    if (!is_static) {
        array_base = dynamic_array_area_size;
        dynamic_array_area_size += extraspace;
    }
    else {
        array_base = static_array_area_size;
        static_array_area_size += extraspace;
    }

    arrays[no_arrays].type = array_type;
    arrays[no_arrays].loc = is_static;

    switch(data_type)
    {
        case NULLS_AI:

            AO = parse_expression(CONSTANT_CONTEXT);

            CalculatedArraySize:

            if (AO.marker != 0)
            {   error("Array sizes must be known now, not defined later");
                break;
            }

            if (target_machine == TARGET_ZCODE) {
                if ((AO.value <= 0) || (AO.value >= 32768))
                {   error("An array must have between 1 and 32767 entries");
                    AO.value = 1;
                }
            }
            else {
                if (AO.value <= 0 || (AO.value & 0x80000000))
                {   error("An array may not have 0 or fewer entries");
                    AO.value = 1;
                }
            }

            {   for (i=0; i<AO.value; i++) array_entry(i, is_static, zero_operand);
            }
            break;

        case DATA_AI:

            /*  In this case the array is initialised to the sequence of
                constant values supplied on the same line                    */

            i=0;
            do
            {   get_next_token();
                if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
                    break;

                if ((token_type == SEP_TT)
                    && ((token_value == OPEN_SQUARE_SEP)
                        || (token_value == CLOSE_SQUARE_SEP)))
                {   discard_token_location(beginning_debug_location);
                    error("Missing ';' to end the initial array values "
                          "before \"[\" or \"]\"");
                    return;
                }
                put_token_back();

                AO = parse_expression(ARRAY_CONTEXT);

                if (i == 0)
                {   get_next_token();
                    put_token_back();
                    if ((token_type == SEP_TT)
                        && (token_value == SEMICOLON_SEP))
                    {   data_type = NULLS_AI;
                        goto CalculatedArraySize;
                    }
                }

                array_entry(i, is_static, AO);
                i++;
            } while (TRUE);
            put_token_back();
            break;

        case ASCII_AI:

            /*  In this case the array is initialised to the ASCII values of
                the characters of a given "quoted string"                    */

            get_next_token();
            if (token_type != DQ_TT)
            {   ebf_error("literal text in double-quotes", token_text);
                token_text = "error";
            }

            {   assembly_operand chars;

                int j;
                INITAO(&chars);
                for (i=0,j=0; token_text[j]!=0; i++,j+=textual_form_length)
                {
                    int32 unicode; int zscii;
                    unicode = text_to_unicode(token_text+j);
                    if (target_machine != TARGET_ZCODE)
                    {
                        if (array_entry_size == 1 && (unicode < 0 || unicode >= 256))
                        {
                            error("Unicode characters beyond Latin-1 cannot be used in a byte array");
                        }
                        else
                        {
                            chars.value = unicode;
                        }
                    }
                    else  /* Z-code */
                    {                          
                        zscii = unicode_to_zscii(unicode);
                        if ((zscii != 5) && (zscii < 0x100)) chars.value = zscii;
                        else
                        {   unicode_char_error("Character can only be used if declared in \
advance as part of 'Zcharacter table':", unicode);
                            chars.value = '?';
                        }
                    }
                    chars.marker = 0;
                    set_constant_ot(&chars);
                    array_entry(i, is_static, chars);
                }
            }
            break;

        case BRACKET_AI:

            /*  In this case the array is initialised to the sequence of
                constant values given over a whole range of compiler-lines,
                between square brackets [ and ]                              */

            i = 0;
            while (TRUE)
            {   get_next_token();
                if ((token_type == SEP_TT) && (token_value == SEMICOLON_SEP))
                    continue;
                if ((token_type == SEP_TT) && (token_value == CLOSE_SQUARE_SEP))
                    break;
                if ((token_type == SEP_TT) && (token_value == OPEN_SQUARE_SEP))
                {   /*  Minimal error recovery: we assume that a ] has
                        been missed, and the programmer is now starting
                        a new routine                                        */

                    ebf_error("']'", token_text);
                    put_token_back(); break;
                }
                put_token_back();
                array_entry(i, is_static, parse_expression(ARRAY_CONTEXT));
                i++;
            }
    }

    finish_array(i, is_static);

    if (debugfile_switch)
    {
        int32 new_area_size;
        debug_file_printf("<array>");
        debug_file_printf("<identifier>%s</identifier>", global_name);
        debug_file_printf("<value>");
        write_debug_array_backpatch(svals[global_symbol]);
        debug_file_printf("</value>");
        new_area_size = (!is_static ? dynamic_array_area_size : static_array_area_size);
        debug_file_printf
            ("<byte-count>%d</byte-count>",
             new_area_size - array_base);
        debug_file_printf
            ("<bytes-per-element>%d</bytes-per-element>",
             array_entry_size);
        debug_file_printf
            ("<zeroth-element-holds-length>%s</zeroth-element-holds-length>",
             (array_type == STRING_ARRAY || array_type == TABLE_ARRAY) ?
                 "true" : "false");
        get_next_token();
        write_debug_locations(get_token_location_end(beginning_debug_location));
        put_token_back();
        debug_file_printf("</array>");
    }

    if ((array_type==BYTE_ARRAY) || (array_type==WORD_ARRAY)) i--;
    if (array_type==BUFFER_ARRAY) i+=WORDSIZE-1;
    arrays[no_arrays++].size = i;
}

extern int32 begin_table_array(void)
{
    /*  The "box" statement needs to be able to construct table
        arrays of strings like this. (Static data, but we create a dynamic
        array for maximum backwards compatibility.) */

    array_base = dynamic_array_area_size;
    array_entry_size = WORDSIZE;

    /*  Leave room to write the array size in later                          */

    dynamic_array_area_size += array_entry_size;

    return array_base;
}

extern int32 begin_word_array(void)
{
    /*  The "random(a, b, ...)" function needs to be able to construct
        word arrays like this. (Static data, but we create a dynamic
        array for maximum backwards compatibility.) */

    array_base = dynamic_array_area_size;
    array_entry_size = WORDSIZE;

    return array_base;
}

/* ========================================================================= */
/*   Data structure management routines                                      */
/* ------------------------------------------------------------------------- */

extern void init_arrays_vars(void)
{   dynamic_array_area = NULL;
    static_array_area = NULL;
    arrays = NULL;
    global_initial_value = NULL;
    variables = NULL;
}

extern void arrays_begin_pass(void)
{
    int ix, totalvar;
    
    no_arrays = 0; 
    if (target_machine == TARGET_ZCODE) {
        no_globals = 0;
        /* The compiler-defined globals start at 239 and go down, so
           we need to initialize the entire list from the start. */
        totalvar = MAX_ZCODE_GLOBAL_VARS;
    }
    else {
        /* The compiler-defined globals run from 0 to 10. */
        no_globals = 11;
        totalvar = no_globals;
    }
    
    ensure_memory_list_available(&global_initial_value_memlist, totalvar);
    for (ix=0; ix<totalvar; ix++) {
        global_initial_value[ix] = 0;
    }
    
    ensure_memory_list_available(&variables_memlist, MAX_LOCAL_VARIABLES+totalvar);
    for (ix=0; ix<MAX_LOCAL_VARIABLES+totalvar; ix++) {
        variables[ix].token = 0;
        variables[ix].usage = FALSE;
    }
    
    dynamic_array_area_size = 0;

    if (target_machine == TARGET_ZCODE) {
        int ix;
        /* This initial segment of dynamic_array_area is never used. It's
           notionally space for the global variables, but that data is
           kept in the global_initial_value array. Nonetheless, all the
           Z-compiler math is set up with the idea that arrays start at
           WORDSIZE * MAX_ZCODE_GLOBAL_VARS, so we need the blank segment.
        */
        dynamic_array_area_size = WORDSIZE * MAX_ZCODE_GLOBAL_VARS;
        ensure_memory_list_available(&dynamic_array_area_memlist, dynamic_array_area_size);
        for (ix=0; ix<WORDSIZE * MAX_ZCODE_GLOBAL_VARS; ix++)
            dynamic_array_area[ix] = 0;
    }
    
    static_array_area_size = 0;
}

extern void arrays_allocate_arrays(void)
{
    initialise_memory_list(&dynamic_array_area_memlist,
        sizeof(uchar), 10000, (void**)&dynamic_array_area,
        "dynamic array data");
    initialise_memory_list(&static_array_area_memlist,
        sizeof(uchar), 0, (void**)&static_array_area,
        "static array data");
    initialise_memory_list(&arrays_memlist,
        sizeof(arrayinfo), 64, (void**)&arrays,
        "array info");
    initialise_memory_list(&global_initial_value_memlist,
        sizeof(int32), 200, (void**)&global_initial_value,
        "global variable values");
}

extern void arrays_free_arrays(void)
{
    deallocate_memory_list(&dynamic_array_area_memlist);
    deallocate_memory_list(&static_array_area_memlist);
    deallocate_memory_list(&arrays_memlist);
    deallocate_memory_list(&global_initial_value_memlist);
}

/* ========================================================================= */
