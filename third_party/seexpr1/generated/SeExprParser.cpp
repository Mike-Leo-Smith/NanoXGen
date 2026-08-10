/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with SeExprYY_ or SeExpr_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with SeExpr or SeExprYY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define SeExprYYBISON 30802

/* Bison version string.  */
#define SeExprYYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define SeExprYYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define SeExprYYPURE 0

/* Push parsers.  */
#define SeExprYYPUSH 0

/* Pull parsers.  */
#define SeExprYYPULL 1


/* Substitute the variable and function names.  */
#define SeExprparse         SeExprparse
#define SeExprlex           SeExprlex
#define SeExprerror         SeExprerror
#define SeExprdebug         SeExprdebug
#define SeExprnerrs         SeExprnerrs
#define SeExprlval          SeExprlval
#define SeExprchar          SeExprchar
#define SeExprlloc          SeExprlloc

/* First part of user prologue.  */
#line 36 "SeExprParser.y"

#ifndef MAKEDEPEND
#include <algorithm>
#include <vector>
#include <stdio.h>
#endif
#include "SeExprNode.h"
#include "SeExprParser.h"
#include "SeExpression.h"
#include "SeMutex.h"

/******************
 lexer declarations
 ******************/

// declarations of functions and data in SeExprParser.l
int SeExprlex();
int SeExprpos();
extern int SeExpr_start;
extern char* SeExprtext;
struct SeExpr_buffer_state;
SeExpr_buffer_state* SeExpr_scan_string(const char *str);
void SeExpr_delete_buffer(SeExpr_buffer_state*);

/*******************
 parser declarations
 *******************/

// forward declaration
static void SeExprerror(const char* msg);

// local data
static const char* ParseStr;    // string being parsed
static std::string ParseError;  // error (set from SeExprerror)
static SeExprNode* ParseResult; // must set result here since SeExprparse can't return it
static const SeExpression* Expr;// used for parenting created SeExprOp's

/* The list of nodes being built is remembered locally here.
   Eventually (if there are no syntax errors) ownership of the nodes
   will belong solely to the parse tree and the parent expression.
   However, if there is a syntax error, we must loop through this list
   and free any nodes that were allocated before the error to avoid a
   memory leak. */
static std::vector<SeExprNode*> ParseNodes;
inline SeExprNode* Remember(SeExprNode* n,const int startPos,const int endPos) 
    { ParseNodes.push_back(n); n->setPosition(startPos,endPos); return n; }
inline void Forget(SeExprNode* n) 
    { ParseNodes.erase(std::find(ParseNodes.begin(), ParseNodes.end(), n)); }

/* These are handy node constructors for 0-3 arguments */
#define NODE(startPos,endPos,name) Remember(new SeExpr##name(Expr),startPos,endPos)
#define NODE1(startPos,endPos,name,a) Remember(new SeExpr##name(Expr,a),startPos,endPos)
#define NODE2(startPos,endPos,name,a,b) Remember(new SeExpr##name(Expr,a,b),startPos,endPos)
#define NODE3(startPos,endPos,name,a,b,c) Remember(new SeExpr##name(Expr,a,b,c),startPos,endPos)

#line 135 "y.tab.c"

# ifndef SeExprYY_CAST
#  ifdef __cplusplus
#   define SeExprYY_CAST(Type, Val) static_cast<Type> (Val)
#   define SeExprYY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define SeExprYY_CAST(Type, Val) ((Type) (Val))
#   define SeExprYY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef SeExprYY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define SeExprYY_NULLPTR nullptr
#   else
#    define SeExprYY_NULLPTR 0
#   endif
#  else
#   define SeExprYY_NULLPTR ((void*)0)
#  endif
# endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
#ifndef SeExprYY_SEEXPR_Y_TAB_H_INCLUDED
# define SeExprYY_SEEXPR_Y_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef SeExprYYDEBUG
# define SeExprYYDEBUG 0
#endif
#if SeExprYYDEBUG
extern int SeExprdebug;
#endif

/* Token kinds.  */
#ifndef SeExprYYTOKENTYPE
# define SeExprYYTOKENTYPE
  enum SeExprtokentype
  {
    SeExprYYEMPTY = -2,
    SeExprYYEOF = 0,                     /* "end of file"  */
    SeExprYYerror = 256,                 /* error  */
    SeExprYYUNDEF = 257,                 /* "invalid token"  */
    IF = 258,                      /* IF  */
    ELSE = 259,                    /* ELSE  */
    NAME = 260,                    /* NAME  */
    VAR = 261,                     /* VAR  */
    STR = 262,                     /* STR  */
    NUMBER = 263,                  /* NUMBER  */
    AddEq = 264,                   /* AddEq  */
    SubEq = 265,                   /* SubEq  */
    MultEq = 266,                  /* MultEq  */
    DivEq = 267,                   /* DivEq  */
    ExpEq = 268,                   /* ExpEq  */
    ModEq = 269,                   /* ModEq  */
    ARROW = 270,                   /* ARROW  */
    OR = 271,                      /* OR  */
    AND = 272,                     /* AND  */
    EQ = 273,                      /* EQ  */
    NE = 274,                      /* NE  */
    LE = 275,                      /* LE  */
    GE = 276,                      /* GE  */
    UNARY = 277                    /* UNARY  */
  };
  typedef enum SeExprtokentype SeExprtoken_kind_t;
#endif

/* Value type.  */
#if ! defined SeExprYYSTYPE && ! defined SeExprYYSTYPE_IS_DECLARED
union SeExprYYSTYPE
{
#line 92 "SeExprParser.y"

    SeExprNode* n; /* a node is returned for all non-terminals to
		      build the parse tree from the leaves up. */
    double d;      // return value for number tokens
    char* s;       /* return value for name tokens.  Note: the string
		      is allocated with strdup() in the lexer and must
		      be freed with free() */

#line 216 "y.tab.c"

};
typedef union SeExprYYSTYPE SeExprYYSTYPE;
# define SeExprYYSTYPE_IS_TRIVIAL 1
# define SeExprYYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined SeExprYYLTYPE && ! defined SeExprYYLTYPE_IS_DECLARED
typedef struct SeExprYYLTYPE SeExprYYLTYPE;
struct SeExprYYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define SeExprYYLTYPE_IS_DECLARED 1
# define SeExprYYLTYPE_IS_TRIVIAL 1
#endif


extern SeExprYYSTYPE SeExprlval;
extern SeExprYYLTYPE SeExprlloc;

int SeExprparse (void);


#endif /* !SeExprYY_SEEXPR_Y_TAB_H_INCLUDED  */
/* Symbol kind.  */
enum SeExprsymbol_kind_t
{
  SeExprYYSYMBOL_SeExprYYEMPTY = -2,
  SeExprYYSYMBOL_SeExprYYEOF = 0,                      /* "end of file"  */
  SeExprYYSYMBOL_SeExprYYerror = 1,                    /* error  */
  SeExprYYSYMBOL_SeExprYYUNDEF = 2,                    /* "invalid token"  */
  SeExprYYSYMBOL_IF = 3,                         /* IF  */
  SeExprYYSYMBOL_ELSE = 4,                       /* ELSE  */
  SeExprYYSYMBOL_NAME = 5,                       /* NAME  */
  SeExprYYSYMBOL_VAR = 6,                        /* VAR  */
  SeExprYYSYMBOL_STR = 7,                        /* STR  */
  SeExprYYSYMBOL_NUMBER = 8,                     /* NUMBER  */
  SeExprYYSYMBOL_AddEq = 9,                      /* AddEq  */
  SeExprYYSYMBOL_SubEq = 10,                     /* SubEq  */
  SeExprYYSYMBOL_MultEq = 11,                    /* MultEq  */
  SeExprYYSYMBOL_DivEq = 12,                     /* DivEq  */
  SeExprYYSYMBOL_ExpEq = 13,                     /* ExpEq  */
  SeExprYYSYMBOL_ModEq = 14,                     /* ModEq  */
  SeExprYYSYMBOL_15_ = 15,                       /* '('  */
  SeExprYYSYMBOL_16_ = 16,                       /* ')'  */
  SeExprYYSYMBOL_ARROW = 17,                     /* ARROW  */
  SeExprYYSYMBOL_18_ = 18,                       /* ':'  */
  SeExprYYSYMBOL_19_ = 19,                       /* '?'  */
  SeExprYYSYMBOL_OR = 20,                        /* OR  */
  SeExprYYSYMBOL_AND = 21,                       /* AND  */
  SeExprYYSYMBOL_EQ = 22,                        /* EQ  */
  SeExprYYSYMBOL_NE = 23,                        /* NE  */
  SeExprYYSYMBOL_24_ = 24,                       /* '<'  */
  SeExprYYSYMBOL_25_ = 25,                       /* '>'  */
  SeExprYYSYMBOL_LE = 26,                        /* LE  */
  SeExprYYSYMBOL_GE = 27,                        /* GE  */
  SeExprYYSYMBOL_28_ = 28,                       /* '+'  */
  SeExprYYSYMBOL_29_ = 29,                       /* '-'  */
  SeExprYYSYMBOL_30_ = 30,                       /* '*'  */
  SeExprYYSYMBOL_31_ = 31,                       /* '/'  */
  SeExprYYSYMBOL_32_ = 32,                       /* '%'  */
  SeExprYYSYMBOL_UNARY = 33,                     /* UNARY  */
  SeExprYYSYMBOL_34_ = 34,                       /* '!'  */
  SeExprYYSYMBOL_35_ = 35,                       /* '~'  */
  SeExprYYSYMBOL_36_ = 36,                       /* '^'  */
  SeExprYYSYMBOL_37_ = 37,                       /* '['  */
  SeExprYYSYMBOL_38_ = 38,                       /* '='  */
  SeExprYYSYMBOL_39_ = 39,                       /* ';'  */
  SeExprYYSYMBOL_40_ = 40,                       /* '{'  */
  SeExprYYSYMBOL_41_ = 41,                       /* '}'  */
  SeExprYYSYMBOL_42_ = 42,                       /* ','  */
  SeExprYYSYMBOL_43_ = 43,                       /* ']'  */
  SeExprYYSYMBOL_SeExprYYACCEPT = 44,                  /* $accept  */
  SeExprYYSYMBOL_expr = 45,                      /* expr  */
  SeExprYYSYMBOL_optassigns = 46,                /* optassigns  */
  SeExprYYSYMBOL_assigns = 47,                   /* assigns  */
  SeExprYYSYMBOL_assign = 48,                    /* assign  */
  SeExprYYSYMBOL_ifthenelse = 49,                /* ifthenelse  */
  SeExprYYSYMBOL_optelse = 50,                   /* optelse  */
  SeExprYYSYMBOL_e = 51,                         /* e  */
  SeExprYYSYMBOL_optargs = 52,                   /* optargs  */
  SeExprYYSYMBOL_args = 53,                      /* args  */
  SeExprYYSYMBOL_arg = 54                        /* arg  */
};
typedef enum SeExprsymbol_kind_t SeExprsymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define SeExprYY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ SeExprtype_int8;
#elif defined SeExprYY_STDINT_H
typedef int_least8_t SeExprtype_int8;
#else
typedef signed char SeExprtype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ SeExprtype_int16;
#elif defined SeExprYY_STDINT_H
typedef int_least16_t SeExprtype_int16;
#else
typedef short SeExprtype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ SeExprtype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined SeExprYY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t SeExprtype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char SeExprtype_uint8;
#else
typedef short SeExprtype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ SeExprtype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined SeExprYY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t SeExprtype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short SeExprtype_uint16;
#else
typedef int SeExprtype_uint16;
#endif

#ifndef SeExprYYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define SeExprYYPTRDIFF_T __PTRDIFF_TYPE__
#  define SeExprYYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define SeExprYYPTRDIFF_T ptrdiff_t
#  define SeExprYYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define SeExprYYPTRDIFF_T long
#  define SeExprYYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef SeExprYYSIZE_T
# ifdef __SIZE_TYPE__
#  define SeExprYYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define SeExprYYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define SeExprYYSIZE_T size_t
# else
#  define SeExprYYSIZE_T unsigned
# endif
#endif

#define SeExprYYSIZE_MAXIMUM                                  \
  SeExprYY_CAST (SeExprYYPTRDIFF_T,                                 \
           (SeExprYYPTRDIFF_MAXIMUM < SeExprYY_CAST (SeExprYYSIZE_T, -1)  \
            ? SeExprYYPTRDIFF_MAXIMUM                         \
            : SeExprYY_CAST (SeExprYYSIZE_T, -1)))

#define SeExprYYSIZEOF(X) SeExprYY_CAST (SeExprYYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef SeExprtype_uint8 SeExpr_state_t;

/* State numbers in computations.  */
typedef int SeExpr_state_fast_t;

#ifndef SeExprYY_
# if defined SeExprYYENABLE_NLS && SeExprYYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define SeExprYY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef SeExprYY_
#  define SeExprYY_(Msgid) Msgid
# endif
#endif


#ifndef SeExprYY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define SeExprYY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define SeExprYY_ATTRIBUTE_PURE
# endif
#endif

#ifndef SeExprYY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define SeExprYY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define SeExprYY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define SeExprYY_USE(E) ((void) (E))
#else
# define SeExprYY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about SeExprlval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define SeExprYY_INITIAL_VALUE(Value) Value
#endif
#ifndef SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef SeExprYY_INITIAL_VALUE
# define SeExprYY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define SeExprYY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define SeExprYY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef SeExprYY_IGNORE_USELESS_CAST_BEGIN
# define SeExprYY_IGNORE_USELESS_CAST_BEGIN
# define SeExprYY_IGNORE_USELESS_CAST_END
#endif


#define SeExprYY_ASSERT(E) ((void) (0 && (E)))

#if !defined SeExproverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef SeExprYYSTACK_USE_ALLOCA
#  if SeExprYYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define SeExprYYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define SeExprYYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define SeExprYYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef SeExprYYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define SeExprYYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef SeExprYYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define SeExprYYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define SeExprYYSTACK_ALLOC SeExprYYMALLOC
#  define SeExprYYSTACK_FREE SeExprYYFREE
#  ifndef SeExprYYSTACK_ALLOC_MAXIMUM
#   define SeExprYYSTACK_ALLOC_MAXIMUM SeExprYYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined SeExprYYMALLOC || defined malloc) \
             && (defined SeExprYYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef SeExprYYMALLOC
#   define SeExprYYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (SeExprYYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef SeExprYYFREE
#   define SeExprYYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined SeExproverflow */

#if (! defined SeExproverflow \
     && (! defined __cplusplus \
         || (defined SeExprYYLTYPE_IS_TRIVIAL && SeExprYYLTYPE_IS_TRIVIAL \
             && defined SeExprYYSTYPE_IS_TRIVIAL && SeExprYYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union SeExpralloc
{
  SeExpr_state_t SeExprss_alloc;
  SeExprYYSTYPE SeExprvs_alloc;
  SeExprYYLTYPE SeExprls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define SeExprYYSTACK_GAP_MAXIMUM (SeExprYYSIZEOF (union SeExpralloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define SeExprYYSTACK_BYTES(N) \
     ((N) * (SeExprYYSIZEOF (SeExpr_state_t) + SeExprYYSIZEOF (SeExprYYSTYPE) \
             + SeExprYYSIZEOF (SeExprYYLTYPE)) \
      + 2 * SeExprYYSTACK_GAP_MAXIMUM)

# define SeExprYYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables SeExprYYSIZE and SeExprYYSTACKSIZE give the old and new number of
   elements in the stack, and SeExprYYPTR gives the new location of the
   stack.  Advance SeExprYYPTR to a properly aligned location for the next
   stack.  */
# define SeExprYYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        SeExprYYPTRDIFF_T SeExprnewbytes;                                         \
        SeExprYYCOPY (&SeExprptr->Stack_alloc, Stack, SeExprsize);                    \
        Stack = &SeExprptr->Stack_alloc;                                    \
        SeExprnewbytes = SeExprstacksize * SeExprYYSIZEOF (*Stack) + SeExprYYSTACK_GAP_MAXIMUM; \
        SeExprptr += SeExprnewbytes / SeExprYYSIZEOF (*SeExprptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined SeExprYYCOPY_NEEDED && SeExprYYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef SeExprYYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define SeExprYYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, SeExprYY_CAST (SeExprYYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define SeExprYYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          SeExprYYPTRDIFF_T SeExpri;                      \
          for (SeExpri = 0; SeExpri < (Count); SeExpri++)   \
            (Dst)[SeExpri] = (Src)[SeExpri];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !SeExprYYCOPY_NEEDED */

/* SeExprYYFINAL -- State number of the termination state.  */
#define SeExprYYFINAL  40
/* SeExprYYLAST -- Last index in SeExprYYTABLE.  */
#define SeExprYYLAST   692

/* SeExprYYNTOKENS -- Number of terminals.  */
#define SeExprYYNTOKENS  44
/* SeExprYYNNTS -- Number of nonterminals.  */
#define SeExprYYNNTS  11
/* SeExprYYNRULES -- Number of rules.  */
#define SeExprYYNRULES  60
/* SeExprYYNSTATES -- Number of states.  */
#define SeExprYYNSTATES  139

/* SeExprYYMAXUTOK -- Last valid token kind.  */
#define SeExprYYMAXUTOK   277


/* SeExprYYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by SeExprlex, with out-of-bounds checking.  */
#define SeExprYYTRANSLATE(SeExprYYX)                                \
  (0 <= (SeExprYYX) && (SeExprYYX) <= SeExprYYMAXUTOK                     \
   ? SeExprYY_CAST (SeExprsymbol_kind_t, SeExprtranslate[SeExprYYX])        \
   : SeExprYYSYMBOL_SeExprYYUNDEF)

/* SeExprYYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by SeExprlex.  */
static const SeExprtype_int8 SeExprtranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    34,     2,     2,     2,    32,     2,     2,
      15,    16,    30,    28,    42,    29,     2,    31,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,    18,    39,
      24,    38,    25,    19,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    37,     2,    43,    36,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    40,     2,    41,    35,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      17,    20,    21,    22,    23,    26,    27,    33
};

#if SeExprYYDEBUG
/* SeExprYYRLINE[SeExprYYN] -- Source line where rule number SeExprYYN was defined.  */
static const SeExprtype_int16 SeExprrline[] =
{
       0,   137,   137,   138,   143,   144,   148,   149,   154,   155,
     156,   159,   162,   165,   168,   171,   174,   175,   178,   181,
     184,   187,   190,   193,   197,   202,   203,   204,   209,   210,
     211,   212,   213,   214,   215,   216,   217,   218,   219,   220,
     221,   222,   223,   224,   225,   226,   227,   228,   229,   230,
     231,   234,   239,   240,   241,   246,   247,   252,   253,   257,
     258
};
#endif

/** Accessing symbol of state STATE.  */
#define SeExprYY_ACCESSING_SYMBOL(State) SeExprYY_CAST (SeExprsymbol_kind_t, SeExprstos[State])

#if SeExprYYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   SeExprYYSYMBOL.  No bounds checking.  */
static const char *SeExprsymbol_name (SeExprsymbol_kind_t SeExprsymbol) SeExprYY_ATTRIBUTE_UNUSED;

/* SeExprYYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at SeExprYYNTOKENS, nonterminals.  */
static const char *const SeExprtname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "IF", "ELSE", "NAME",
  "VAR", "STR", "NUMBER", "AddEq", "SubEq", "MultEq", "DivEq", "ExpEq",
  "ModEq", "'('", "')'", "ARROW", "':'", "'?'", "OR", "AND", "EQ", "NE",
  "'<'", "'>'", "LE", "GE", "'+'", "'-'", "'*'", "'/'", "'%'", "UNARY",
  "'!'", "'~'", "'^'", "'['", "'='", "';'", "'{'", "'}'", "','", "']'",
  "$accept", "expr", "optassigns", "assigns", "assign", "ifthenelse",
  "optelse", "e", "optargs", "args", "arg", SeExprYY_NULLPTR
};

static const char *
SeExprsymbol_name (SeExprsymbol_kind_t SeExprsymbol)
{
  return SeExprtname[SeExprsymbol];
}
#endif

#define SeExprYYPACT_NINF (-57)

#define SeExprpact_value_is_default(Yyn) \
  ((Yyn) == SeExprYYPACT_NINF)

#define SeExprYYTABLE_NINF (-1)

#define SeExprtable_value_is_error(Yyn) \
  0

/* SeExprYYPACT[STATE-NUM] -- Index in SeExprYYTABLE of the portion describing
   STATE-NUM.  */
static const SeExprtype_int16 SeExprpact[] =
{
      57,    25,    23,   101,   -57,    72,    72,    72,    72,    72,
      72,    15,    57,   -57,   -57,   593,    72,    72,    72,    72,
      72,    72,    72,    61,    72,    72,    72,    72,    72,    72,
      72,    72,    26,   -57,   529,   -33,   -33,   -33,   -33,   184,
     -57,   -57,   593,    18,    72,    72,    72,    72,    72,    72,
      72,    72,    72,    72,    72,    72,    72,    72,    72,    72,
     551,   232,   253,   274,   295,   316,   337,   -57,   593,    27,
      22,   -57,   358,   379,   400,   421,   442,   463,   484,   505,
     -57,    72,    55,   572,   629,   645,   105,   105,   655,   655,
     655,   655,   113,   113,   -33,   -33,   -33,   -33,   134,     2,
     -57,   -57,   -57,   -57,   -57,   -57,   -57,    61,   -57,   -57,
     -57,   -57,   -57,   -57,   -57,   -57,   208,    61,    72,   -57,
       8,   -57,    72,    58,   612,   114,   101,    30,     8,   159,
     -57,    69,   -57,    -1,   -57,     8,   -57,    34,   -57
};

/* SeExprYYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when SeExprYYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const SeExprtype_int8 SeExprdefact[] =
{
       0,     0,    53,    52,    54,     0,     0,     0,     0,     0,
       0,     0,     0,     6,     8,     3,     0,     0,     0,     0,
       0,     0,     0,    55,     0,     0,     0,     0,     0,     0,
       0,     0,    53,    52,     0,    40,    41,    42,    43,     0,
       1,     7,     2,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    60,    59,     0,
      56,    57,     0,     0,     0,     0,     0,     0,     0,     0,
      28,     0,     0,     0,    32,    33,    34,    35,    36,    37,
      38,    39,    44,    45,    46,    47,    48,    49,     0,     0,
      17,    18,    19,    20,    21,    22,    50,     0,    16,    10,
      11,    12,    13,    14,    15,     9,     0,    55,     0,    30,
       4,    58,     0,     0,    31,     0,     0,     0,     5,     0,
      51,    25,    29,     0,    24,     4,    27,     0,    26
};

/* SeExprYYPGOTO[NTERM-NUM].  */
static const SeExprtype_int8 SeExprpgoto[] =
{
     -57,   -57,   -56,    82,   -11,   -50,   -57,     0,   -29,   -57,
     -23
};

/* SeExprYYDEFGOTO[NTERM-NUM].  */
static const SeExprtype_uint8 SeExprdefgoto[] =
{
       0,    11,   127,   128,    13,    14,   134,    68,    69,    70,
      71
};

/* SeExprYYTABLE[SeExprYYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If SeExprYYTABLE_NINF, syntax error.  */
static const SeExprtype_uint8 SeExprtable[] =
{
      15,    41,     1,    58,    59,    34,    35,    36,    37,    38,
      39,     1,    42,   125,   126,    40,    60,    61,    62,    63,
      64,    65,    66,    82,    72,    73,    74,    75,    76,    77,
      78,    79,    17,    18,    19,    20,    21,    22,    23,   135,
      16,    23,   120,   106,    83,    84,    85,    86,    87,    88,
      89,    90,    91,    92,    93,    94,    95,    96,    97,    98,
       1,    24,     2,     3,   107,     4,    32,    33,    67,     4,
     117,   131,     5,   133,   130,   138,     5,    32,    33,   137,
       4,   116,    12,   136,   121,     6,     7,     5,   123,     6,
       7,     8,     9,     0,    10,     8,     9,     0,    10,     0,
       6,     7,     0,     0,     0,     0,     8,     9,     0,    10,
      25,    26,    27,    28,    29,    30,     0,    41,   124,     0,
       0,     0,   129,    17,    18,    19,    20,    21,    22,    49,
      50,    51,    52,    53,    54,    55,    56,    57,     0,    31,
       0,    58,    59,    55,    56,    57,     0,     0,     0,    58,
      59,    43,    24,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,     0,     0,     0,
      58,    59,     0,     0,     0,     0,    43,   119,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
      56,    57,     0,     0,     0,    58,    59,     0,     0,     0,
       0,    43,   132,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,     0,     0,     0,
      58,    59,     0,     0,     0,    43,    81,    44,    45,    46,
      47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
      57,     0,     0,     0,    58,    59,     0,     0,     0,    43,
     122,    44,    45,    46,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,     0,     0,     0,    58,    59,
      43,   100,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,    57,     0,     0,     0,    58,
      59,    43,   101,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,     0,     0,     0,
      58,    59,    43,   102,    44,    45,    46,    47,    48,    49,
      50,    51,    52,    53,    54,    55,    56,    57,     0,     0,
       0,    58,    59,    43,   103,    44,    45,    46,    47,    48,
      49,    50,    51,    52,    53,    54,    55,    56,    57,     0,
       0,     0,    58,    59,    43,   104,    44,    45,    46,    47,
      48,    49,    50,    51,    52,    53,    54,    55,    56,    57,
       0,     0,     0,    58,    59,    43,   105,    44,    45,    46,
      47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
      57,     0,     0,     0,    58,    59,    43,   108,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
      56,    57,     0,     0,     0,    58,    59,    43,   109,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,     0,     0,     0,    58,    59,    43,   110,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    53,
      54,    55,    56,    57,     0,     0,     0,    58,    59,    43,
     111,    44,    45,    46,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,     0,     0,     0,    58,    59,
      43,   112,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,    57,     0,     0,     0,    58,
      59,    43,   113,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,     0,     0,     0,
      58,    59,    43,   114,    44,    45,    46,    47,    48,    49,
      50,    51,    52,    53,    54,    55,    56,    57,     0,     0,
       0,    58,    59,     0,   115,    80,    43,     0,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
      56,    57,     0,     0,     0,    58,    59,    99,    43,     0,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    53,
      54,    55,    56,    57,     0,     0,     0,    58,    59,    43,
     118,    44,    45,    46,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,     0,     0,     0,    58,    59,
      43,     0,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,    57,     0,     0,     0,    58,
      59,    44,    45,    46,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,     0,     0,     0,    58,    59,
      46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
      56,    57,     0,     0,     0,    58,    59,    47,    48,    49,
      50,    51,    52,    53,    54,    55,    56,    57,     0,     0,
       0,    58,    59,    53,    54,    55,    56,    57,     0,     0,
       0,    58,    59
};

static const SeExprtype_int16 SeExprcheck[] =
{
       0,    12,     3,    36,    37,     5,     6,     7,     8,     9,
      10,     3,    12,     5,     6,     0,    16,    17,    18,    19,
      20,    21,    22,     5,    24,    25,    26,    27,    28,    29,
      30,    31,     9,    10,    11,    12,    13,    14,    15,    40,
      15,    15,    40,    16,    44,    45,    46,    47,    48,    49,
      50,    51,    52,    53,    54,    55,    56,    57,    58,    59,
       3,    38,     5,     6,    42,     8,     5,     6,     7,     8,
      15,    41,    15,     4,    16,    41,    15,     5,     6,   135,
       8,    81,     0,   133,   107,    28,    29,    15,   117,    28,
      29,    34,    35,    -1,    37,    34,    35,    -1,    37,    -1,
      28,    29,    -1,    -1,    -1,    -1,    34,    35,    -1,    37,
       9,    10,    11,    12,    13,    14,    -1,   128,   118,    -1,
      -1,    -1,   122,     9,    10,    11,    12,    13,    14,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    -1,    38,
      -1,    36,    37,    30,    31,    32,    -1,    -1,    -1,    36,
      37,    17,    38,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    -1,    -1,    -1,
      36,    37,    -1,    -1,    -1,    -1,    17,    43,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    -1,    -1,    -1,    36,    37,    -1,    -1,    -1,
      -1,    17,    43,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    -1,    -1,    -1,
      36,    37,    -1,    -1,    -1,    17,    42,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,    31,
      32,    -1,    -1,    -1,    36,    37,    -1,    -1,    -1,    17,
      42,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,    31,    32,    -1,    -1,    -1,    36,    37,
      17,    39,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    32,    -1,    -1,    -1,    36,
      37,    17,    39,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    -1,    -1,    -1,
      36,    37,    17,    39,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    -1,    -1,
      -1,    36,    37,    17,    39,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    32,    -1,
      -1,    -1,    36,    37,    17,    39,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    30,    31,    32,
      -1,    -1,    -1,    36,    37,    17,    39,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,    31,
      32,    -1,    -1,    -1,    36,    37,    17,    39,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    -1,    -1,    -1,    36,    37,    17,    39,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,    31,    32,    -1,    -1,    -1,    36,    37,    17,    39,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,    32,    -1,    -1,    -1,    36,    37,    17,
      39,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,    31,    32,    -1,    -1,    -1,    36,    37,
      17,    39,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    32,    -1,    -1,    -1,    36,
      37,    17,    39,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    -1,    -1,    -1,
      36,    37,    17,    39,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    -1,    -1,
      -1,    36,    37,    -1,    39,    16,    17,    -1,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    -1,    -1,    -1,    36,    37,    16,    17,    -1,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,    32,    -1,    -1,    -1,    36,    37,    17,
      18,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,    31,    32,    -1,    -1,    -1,    36,    37,
      17,    -1,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    32,    -1,    -1,    -1,    36,
      37,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,    31,    32,    -1,    -1,    -1,    36,    37,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    -1,    -1,    -1,    36,    37,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    -1,    -1,
      -1,    36,    37,    28,    29,    30,    31,    32,    -1,    -1,
      -1,    36,    37
};

/* SeExprYYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const SeExprtype_int8 SeExprstos[] =
{
       0,     3,     5,     6,     8,    15,    28,    29,    34,    35,
      37,    45,    47,    48,    49,    51,    15,     9,    10,    11,
      12,    13,    14,    15,    38,     9,    10,    11,    12,    13,
      14,    38,     5,     6,    51,    51,    51,    51,    51,    51,
       0,    48,    51,    17,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    36,    37,
      51,    51,    51,    51,    51,    51,    51,     7,    51,    52,
      53,    54,    51,    51,    51,    51,    51,    51,    51,    51,
      16,    42,     5,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    16,
      39,    39,    39,    39,    39,    39,    16,    42,    39,    39,
      39,    39,    39,    39,    39,    39,    51,    15,    18,    43,
      40,    54,    42,    52,    51,     5,     6,    46,    47,    51,
      16,    41,    43,     4,    50,    40,    49,    46,    41
};

/* SeExprYYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const SeExprtype_int8 SeExprr1[] =
{
       0,    44,    45,    45,    46,    46,    47,    47,    48,    48,
      48,    48,    48,    48,    48,    48,    48,    48,    48,    48,
      48,    48,    48,    48,    49,    50,    50,    50,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    51,    51,    51,    51,    51,
      51,    51,    51,    51,    51,    52,    52,    53,    53,    54,
      54
};

/* SeExprYYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const SeExprtype_int8 SeExprr2[] =
{
       0,     2,     2,     1,     0,     1,     1,     2,     1,     4,
       4,     4,     4,     4,     4,     4,     4,     4,     4,     4,
       4,     4,     4,     0,     8,     0,     4,     2,     3,     7,
       4,     5,     3,     3,     3,     3,     3,     3,     3,     3,
       2,     2,     2,     2,     3,     3,     3,     3,     3,     3,
       4,     6,     1,     1,     1,     0,     1,     1,     3,     1,
       1
};


enum { SeExprYYENOMEM = -2 };

#define SeExprerrok         (SeExprerrstatus = 0)
#define SeExprclearin       (SeExprchar = SeExprYYEMPTY)

#define SeExprYYACCEPT        goto SeExpracceptlab
#define SeExprYYABORT         goto SeExprabortlab
#define SeExprYYERROR         goto SeExprerrorlab
#define SeExprYYNOMEM         goto SeExprexhaustedlab


#define SeExprYYRECOVERING()  (!!SeExprerrstatus)

#define SeExprYYBACKUP(Token, Value)                                    \
  do                                                              \
    if (SeExprchar == SeExprYYEMPTY)                                        \
      {                                                           \
        SeExprchar = (Token);                                         \
        SeExprlval = (Value);                                         \
        SeExprYYPOPSTACK (SeExprlen);                                       \
        SeExprstate = *SeExprssp;                                         \
        goto SeExprbackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        SeExprerror (SeExprYY_("syntax error: cannot back up")); \
        SeExprYYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use SeExprYYerror or SeExprYYUNDEF. */
#define SeExprYYERRCODE SeExprYYUNDEF

/* SeExprYYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef SeExprYYLLOC_DEFAULT
# define SeExprYYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = SeExprYYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = SeExprYYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = SeExprYYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = SeExprYYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            SeExprYYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            SeExprYYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define SeExprYYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if SeExprYYDEBUG

# ifndef SeExprYYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define SeExprYYFPRINTF fprintf
# endif

# define SeExprYYDPRINTF(Args)                        \
do {                                            \
  if (SeExprdebug)                                  \
    SeExprYYFPRINTF Args;                             \
} while (0)


/* SeExprYYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef SeExprYYLOCATION_PRINT

#  if defined SeExprYY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private SeExprYY_LOCATION_PRINT macros.  */
#   define SeExprYYLOCATION_PRINT(File, Loc)  SeExprYY_LOCATION_PRINT(File, *(Loc))

#  elif defined SeExprYYLTYPE_IS_TRIVIAL && SeExprYYLTYPE_IS_TRIVIAL

/* Print *SeExprYYLOCP on SeExprYYO.  Private, do not rely on its existence. */

SeExprYY_ATTRIBUTE_UNUSED
static int
SeExpr_location_print_ (FILE *SeExpro, SeExprYYLTYPE const * const SeExprlocp)
{
  int res = 0;
  int end_col = 0 != SeExprlocp->last_column ? SeExprlocp->last_column - 1 : 0;
  if (0 <= SeExprlocp->first_line)
    {
      res += SeExprYYFPRINTF (SeExpro, "%d", SeExprlocp->first_line);
      if (0 <= SeExprlocp->first_column)
        res += SeExprYYFPRINTF (SeExpro, ".%d", SeExprlocp->first_column);
    }
  if (0 <= SeExprlocp->last_line)
    {
      if (SeExprlocp->first_line < SeExprlocp->last_line)
        {
          res += SeExprYYFPRINTF (SeExpro, "-%d", SeExprlocp->last_line);
          if (0 <= end_col)
            res += SeExprYYFPRINTF (SeExpro, ".%d", end_col);
        }
      else if (0 <= end_col && SeExprlocp->first_column < end_col)
        res += SeExprYYFPRINTF (SeExpro, "-%d", end_col);
    }
  return res;
}

#   define SeExprYYLOCATION_PRINT  SeExpr_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private SeExprYY_LOCATION_PRINT macros.  */
#   define SeExprYY_LOCATION_PRINT(File, Loc)  SeExprYYLOCATION_PRINT(File, &(Loc))

#  else

#   define SeExprYYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private SeExprYY_LOCATION_PRINT macros.  */
#   define SeExprYY_LOCATION_PRINT  SeExprYYLOCATION_PRINT

#  endif
# endif /* !defined SeExprYYLOCATION_PRINT */


# define SeExprYY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (SeExprdebug)                                                            \
    {                                                                     \
      SeExprYYFPRINTF (stderr, "%s ", Title);                                   \
      SeExpr_symbol_print (stderr,                                            \
                  Kind, Value, Location); \
      SeExprYYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on SeExprYYO.  |
`-----------------------------------*/

static void
SeExpr_symbol_value_print (FILE *SeExpro,
                       SeExprsymbol_kind_t SeExprkind, SeExprYYSTYPE const * const SeExprvaluep, SeExprYYLTYPE const * const SeExprlocationp)
{
  FILE *SeExproutput = SeExpro;
  SeExprYY_USE (SeExproutput);
  SeExprYY_USE (SeExprlocationp);
  if (!SeExprvaluep)
    return;
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  SeExprYY_USE (SeExprkind);
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on SeExprYYO.  |
`---------------------------*/

static void
SeExpr_symbol_print (FILE *SeExpro,
                 SeExprsymbol_kind_t SeExprkind, SeExprYYSTYPE const * const SeExprvaluep, SeExprYYLTYPE const * const SeExprlocationp)
{
  SeExprYYFPRINTF (SeExpro, "%s %s (",
             SeExprkind < SeExprYYNTOKENS ? "token" : "nterm", SeExprsymbol_name (SeExprkind));

  SeExprYYLOCATION_PRINT (SeExpro, SeExprlocationp);
  SeExprYYFPRINTF (SeExpro, ": ");
  SeExpr_symbol_value_print (SeExpro, SeExprkind, SeExprvaluep, SeExprlocationp);
  SeExprYYFPRINTF (SeExpro, ")");
}

/*------------------------------------------------------------------.
| SeExpr_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
SeExpr_stack_print (SeExpr_state_t *SeExprbottom, SeExpr_state_t *SeExprtop)
{
  SeExprYYFPRINTF (stderr, "Stack now");
  for (; SeExprbottom <= SeExprtop; SeExprbottom++)
    {
      int SeExprbot = *SeExprbottom;
      SeExprYYFPRINTF (stderr, " %d", SeExprbot);
    }
  SeExprYYFPRINTF (stderr, "\n");
}

# define SeExprYY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (SeExprdebug)                                                  \
    SeExpr_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the SeExprYYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
SeExpr_reduce_print (SeExpr_state_t *SeExprssp, SeExprYYSTYPE *SeExprvsp, SeExprYYLTYPE *SeExprlsp,
                 int SeExprrule)
{
  int SeExprlno = SeExprrline[SeExprrule];
  int SeExprnrhs = SeExprr2[SeExprrule];
  int SeExpri;
  SeExprYYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             SeExprrule - 1, SeExprlno);
  /* The symbols being reduced.  */
  for (SeExpri = 0; SeExpri < SeExprnrhs; SeExpri++)
    {
      SeExprYYFPRINTF (stderr, "   $%d = ", SeExpri + 1);
      SeExpr_symbol_print (stderr,
                       SeExprYY_ACCESSING_SYMBOL (+SeExprssp[SeExpri + 1 - SeExprnrhs]),
                       &SeExprvsp[(SeExpri + 1) - (SeExprnrhs)],
                       &(SeExprlsp[(SeExpri + 1) - (SeExprnrhs)]));
      SeExprYYFPRINTF (stderr, "\n");
    }
}

# define SeExprYY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (SeExprdebug)                          \
    SeExpr_reduce_print (SeExprssp, SeExprvsp, SeExprlsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int SeExprdebug;
#else /* !SeExprYYDEBUG */
# define SeExprYYDPRINTF(Args) ((void) 0)
# define SeExprYY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define SeExprYY_STACK_PRINT(Bottom, Top)
# define SeExprYY_REDUCE_PRINT(Rule)
#endif /* !SeExprYYDEBUG */


/* SeExprYYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef SeExprYYINITDEPTH
# define SeExprYYINITDEPTH 200
#endif

/* SeExprYYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   SeExprYYSTACK_ALLOC_MAXIMUM < SeExprYYSTACK_BYTES (SeExprYYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef SeExprYYMAXDEPTH
# define SeExprYYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
SeExprdestruct (const char *SeExprmsg,
            SeExprsymbol_kind_t SeExprkind, SeExprYYSTYPE *SeExprvaluep, SeExprYYLTYPE *SeExprlocationp)
{
  SeExprYY_USE (SeExprvaluep);
  SeExprYY_USE (SeExprlocationp);
  if (!SeExprmsg)
    SeExprmsg = "Deleting";
  SeExprYY_SYMBOL_PRINT (SeExprmsg, SeExprkind, SeExprvaluep, SeExprlocationp);

  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  SeExprYY_USE (SeExprkind);
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int SeExprchar;

/* The semantic value of the lookahead symbol.  */
SeExprYYSTYPE SeExprlval;
/* Location data for the lookahead symbol.  */
SeExprYYLTYPE SeExprlloc
# if defined SeExprYYLTYPE_IS_TRIVIAL && SeExprYYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
/* Number of syntax errors so far.  */
int SeExprnerrs;




/*----------.
| SeExprparse.  |
`----------*/

int
SeExprparse (void)
{
    SeExpr_state_fast_t SeExprstate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int SeExprerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow SeExproverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    SeExprYYPTRDIFF_T SeExprstacksize = SeExprYYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    SeExpr_state_t SeExprssa[SeExprYYINITDEPTH];
    SeExpr_state_t *SeExprss = SeExprssa;
    SeExpr_state_t *SeExprssp = SeExprss;

    /* The semantic value stack: array, bottom, top.  */
    SeExprYYSTYPE SeExprvsa[SeExprYYINITDEPTH];
    SeExprYYSTYPE *SeExprvs = SeExprvsa;
    SeExprYYSTYPE *SeExprvsp = SeExprvs;

    /* The location stack: array, bottom, top.  */
    SeExprYYLTYPE SeExprlsa[SeExprYYINITDEPTH];
    SeExprYYLTYPE *SeExprls = SeExprlsa;
    SeExprYYLTYPE *SeExprlsp = SeExprls;

  int SeExprn;
  /* The return value of SeExprparse.  */
  int SeExprresult;
  /* Lookahead symbol kind.  */
  SeExprsymbol_kind_t SeExprtoken = SeExprYYSYMBOL_SeExprYYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  SeExprYYSTYPE SeExprval;
  SeExprYYLTYPE SeExprloc;

  /* The locations where the error started and ended.  */
  SeExprYYLTYPE SeExprerror_range[3];



#define SeExprYYPOPSTACK(N)   (SeExprvsp -= (N), SeExprssp -= (N), SeExprlsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int SeExprlen = 0;

  SeExprYYDPRINTF ((stderr, "Starting parse\n"));

  SeExprchar = SeExprYYEMPTY; /* Cause a token to be read.  */

  SeExprlsp[0] = SeExprlloc;
  goto SeExprsetstate;


/*------------------------------------------------------------.
| SeExprnewstate -- push a new state, which is found in SeExprstate.  |
`------------------------------------------------------------*/
SeExprnewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  SeExprssp++;


/*--------------------------------------------------------------------.
| SeExprsetstate -- set current state (the top of the stack) to SeExprstate.  |
`--------------------------------------------------------------------*/
SeExprsetstate:
  SeExprYYDPRINTF ((stderr, "Entering state %d\n", SeExprstate));
  SeExprYY_ASSERT (0 <= SeExprstate && SeExprstate < SeExprYYNSTATES);
  SeExprYY_IGNORE_USELESS_CAST_BEGIN
  *SeExprssp = SeExprYY_CAST (SeExpr_state_t, SeExprstate);
  SeExprYY_IGNORE_USELESS_CAST_END
  SeExprYY_STACK_PRINT (SeExprss, SeExprssp);

  if (SeExprss + SeExprstacksize - 1 <= SeExprssp)
#if !defined SeExproverflow && !defined SeExprYYSTACK_RELOCATE
    SeExprYYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      SeExprYYPTRDIFF_T SeExprsize = SeExprssp - SeExprss + 1;

# if defined SeExproverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        SeExpr_state_t *SeExprss1 = SeExprss;
        SeExprYYSTYPE *SeExprvs1 = SeExprvs;
        SeExprYYLTYPE *SeExprls1 = SeExprls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if SeExproverflow is a macro.  */
        SeExproverflow (SeExprYY_("memory exhausted"),
                    &SeExprss1, SeExprsize * SeExprYYSIZEOF (*SeExprssp),
                    &SeExprvs1, SeExprsize * SeExprYYSIZEOF (*SeExprvsp),
                    &SeExprls1, SeExprsize * SeExprYYSIZEOF (*SeExprlsp),
                    &SeExprstacksize);
        SeExprss = SeExprss1;
        SeExprvs = SeExprvs1;
        SeExprls = SeExprls1;
      }
# else /* defined SeExprYYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (SeExprYYMAXDEPTH <= SeExprstacksize)
        SeExprYYNOMEM;
      SeExprstacksize *= 2;
      if (SeExprYYMAXDEPTH < SeExprstacksize)
        SeExprstacksize = SeExprYYMAXDEPTH;

      {
        SeExpr_state_t *SeExprss1 = SeExprss;
        union SeExpralloc *SeExprptr =
          SeExprYY_CAST (union SeExpralloc *,
                   SeExprYYSTACK_ALLOC (SeExprYY_CAST (SeExprYYSIZE_T, SeExprYYSTACK_BYTES (SeExprstacksize))));
        if (! SeExprptr)
          SeExprYYNOMEM;
        SeExprYYSTACK_RELOCATE (SeExprss_alloc, SeExprss);
        SeExprYYSTACK_RELOCATE (SeExprvs_alloc, SeExprvs);
        SeExprYYSTACK_RELOCATE (SeExprls_alloc, SeExprls);
#  undef SeExprYYSTACK_RELOCATE
        if (SeExprss1 != SeExprssa)
          SeExprYYSTACK_FREE (SeExprss1);
      }
# endif

      SeExprssp = SeExprss + SeExprsize - 1;
      SeExprvsp = SeExprvs + SeExprsize - 1;
      SeExprlsp = SeExprls + SeExprsize - 1;

      SeExprYY_IGNORE_USELESS_CAST_BEGIN
      SeExprYYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  SeExprYY_CAST (long, SeExprstacksize)));
      SeExprYY_IGNORE_USELESS_CAST_END

      if (SeExprss + SeExprstacksize - 1 <= SeExprssp)
        SeExprYYABORT;
    }
#endif /* !defined SeExproverflow && !defined SeExprYYSTACK_RELOCATE */


  if (SeExprstate == SeExprYYFINAL)
    SeExprYYACCEPT;

  goto SeExprbackup;


/*-----------.
| SeExprbackup.  |
`-----------*/
SeExprbackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  SeExprn = SeExprpact[SeExprstate];
  if (SeExprpact_value_is_default (SeExprn))
    goto SeExprdefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* SeExprYYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (SeExprchar == SeExprYYEMPTY)
    {
      SeExprYYDPRINTF ((stderr, "Reading a token\n"));
      SeExprchar = SeExprlex ();
    }

  if (SeExprchar <= SeExprYYEOF)
    {
      SeExprchar = SeExprYYEOF;
      SeExprtoken = SeExprYYSYMBOL_SeExprYYEOF;
      SeExprYYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (SeExprchar == SeExprYYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      SeExprchar = SeExprYYUNDEF;
      SeExprtoken = SeExprYYSYMBOL_SeExprYYerror;
      SeExprerror_range[1] = SeExprlloc;
      goto SeExprerrlab1;
    }
  else
    {
      SeExprtoken = SeExprYYTRANSLATE (SeExprchar);
      SeExprYY_SYMBOL_PRINT ("Next token is", SeExprtoken, &SeExprlval, &SeExprlloc);
    }

  /* If the proper action on seeing token SeExprYYTOKEN is to reduce or to
     detect an error, take that action.  */
  SeExprn += SeExprtoken;
  if (SeExprn < 0 || SeExprYYLAST < SeExprn || SeExprcheck[SeExprn] != SeExprtoken)
    goto SeExprdefault;
  SeExprn = SeExprtable[SeExprn];
  if (SeExprn <= 0)
    {
      if (SeExprtable_value_is_error (SeExprn))
        goto SeExprerrlab;
      SeExprn = -SeExprn;
      goto SeExprreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (SeExprerrstatus)
    SeExprerrstatus--;

  /* Shift the lookahead token.  */
  SeExprYY_SYMBOL_PRINT ("Shifting", SeExprtoken, &SeExprlval, &SeExprlloc);
  SeExprstate = SeExprn;
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++SeExprvsp = SeExprlval;
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END
  *++SeExprlsp = SeExprlloc;

  /* Discard the shifted token.  */
  SeExprchar = SeExprYYEMPTY;
  goto SeExprnewstate;


/*-----------------------------------------------------------.
| SeExprdefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
SeExprdefault:
  SeExprn = SeExprdefact[SeExprstate];
  if (SeExprn == 0)
    goto SeExprerrlab;
  goto SeExprreduce;


/*-----------------------------.
| SeExprreduce -- do a reduction.  |
`-----------------------------*/
SeExprreduce:
  /* SeExprn is the number of a rule to reduce with.  */
  SeExprlen = SeExprr2[SeExprn];

  /* If SeExprYYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets SeExprYYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to SeExprYYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that SeExprYYVAL may be used uninitialized.  */
  SeExprval = SeExprvsp[1-SeExprlen];

  /* Default location. */
  SeExprYYLLOC_DEFAULT (SeExprloc, (SeExprlsp - SeExprlen), SeExprlen);
  SeExprerror_range[1] = SeExprloc;
  SeExprYY_REDUCE_PRINT (SeExprn);
  switch (SeExprn)
    {
  case 2: /* expr: assigns e  */
#line 137 "SeExprParser.y"
                                { ParseResult = NODE2((SeExprloc).first_column,(SeExprloc).last_column,BlockNode, (SeExprvsp[-1].n), (SeExprvsp[0].n)); }
#line 1569 "y.tab.c"
    break;

  case 3: /* expr: e  */
#line 138 "SeExprParser.y"
                                { ParseResult = (SeExprvsp[0].n); }
#line 1575 "y.tab.c"
    break;

  case 4: /* optassigns: %empty  */
#line 143 "SeExprParser.y"
                                { (SeExprval.n) = NODE((SeExprloc).first_column,(SeExprloc).last_column,Node); /* create empty node */; }
#line 1581 "y.tab.c"
    break;

  case 5: /* optassigns: assigns  */
#line 144 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1587 "y.tab.c"
    break;

  case 6: /* assigns: assign  */
#line 148 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,Node, (SeExprvsp[0].n)); /* create var list */}
#line 1593 "y.tab.c"
    break;

  case 7: /* assigns: assigns assign  */
#line 149 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[-1].n); (SeExprvsp[-1].n)->addChild((SeExprvsp[0].n)); /* add to list */}
#line 1599 "y.tab.c"
    break;

  case 8: /* assign: ifthenelse  */
#line 154 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1605 "y.tab.c"
    break;

  case 9: /* assign: VAR '=' e ';'  */
#line 155 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), (SeExprvsp[-1].n));  }
#line 1611 "y.tab.c"
    break;

  case 10: /* assign: VAR AddEq e ';'  */
#line 156 "SeExprParser.y"
                                   {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,AddNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1619 "y.tab.c"
    break;

  case 11: /* assign: VAR SubEq e ';'  */
#line 159 "SeExprParser.y"
                                   {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,SubNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1627 "y.tab.c"
    break;

  case 12: /* assign: VAR MultEq e ';'  */
#line 162 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,MulNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1635 "y.tab.c"
    break;

  case 13: /* assign: VAR DivEq e ';'  */
#line 165 "SeExprParser.y"
                                   {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,DivNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1643 "y.tab.c"
    break;

  case 14: /* assign: VAR ExpEq e ';'  */
#line 168 "SeExprParser.y"
                                   {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,ExpNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1651 "y.tab.c"
    break;

  case 15: /* assign: VAR ModEq e ';'  */
#line 171 "SeExprParser.y"
                                   {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,ModNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1659 "y.tab.c"
    break;

  case 16: /* assign: NAME '=' e ';'  */
#line 174 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), (SeExprvsp[-1].n));  }
#line 1665 "y.tab.c"
    break;

  case 17: /* assign: NAME AddEq e ';'  */
#line 175 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,AddNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1673 "y.tab.c"
    break;

  case 18: /* assign: NAME SubEq e ';'  */
#line 178 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,SubNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1681 "y.tab.c"
    break;

  case 19: /* assign: NAME MultEq e ';'  */
#line 181 "SeExprParser.y"
                                     {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,MulNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1689 "y.tab.c"
    break;

  case 20: /* assign: NAME DivEq e ';'  */
#line 184 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,DivNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1697 "y.tab.c"
    break;

  case 21: /* assign: NAME ExpEq e ';'  */
#line 187 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,ExpNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1705 "y.tab.c"
    break;

  case 22: /* assign: NAME ModEq e ';'  */
#line 190 "SeExprParser.y"
                                    {SeExprNode* varNode=NODE1((SeExprlsp[-3]).first_column,(SeExprlsp[-3]).first_column,VarNode, (SeExprvsp[-3].s));
                                SeExprNode* opNode=NODE2((SeExprlsp[-1]).first_column,(SeExprlsp[-1]).first_column,ModNode,varNode,(SeExprvsp[-1].n));
                                (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AssignNode, (SeExprvsp[-3].s), opNode);}
#line 1713 "y.tab.c"
    break;

  case 24: /* ifthenelse: IF '(' e ')' '{' optassigns '}' optelse  */
#line 198 "SeExprParser.y"
                                { (SeExprval.n) = NODE3((SeExprloc).first_column,(SeExprloc).last_column,IfThenElseNode, (SeExprvsp[-5].n), (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1719 "y.tab.c"
    break;

  case 25: /* optelse: %empty  */
#line 202 "SeExprParser.y"
                                { (SeExprval.n) = NODE((SeExprloc).first_column,(SeExprloc).last_column,Node); /* create empty node */ }
#line 1725 "y.tab.c"
    break;

  case 26: /* optelse: ELSE '{' optassigns '}'  */
#line 203 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[-1].n); }
#line 1731 "y.tab.c"
    break;

  case 27: /* optelse: ELSE ifthenelse  */
#line 204 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1737 "y.tab.c"
    break;

  case 28: /* e: '(' e ')'  */
#line 209 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[-1].n); }
#line 1743 "y.tab.c"
    break;

  case 29: /* e: '[' e ',' e ',' e ']'  */
#line 210 "SeExprParser.y"
                                { (SeExprval.n) = NODE3((SeExprloc).first_column,(SeExprloc).last_column,VecNode, (SeExprvsp[-5].n), (SeExprvsp[-3].n), (SeExprvsp[-1].n)); }
#line 1749 "y.tab.c"
    break;

  case 30: /* e: e '[' e ']'  */
#line 211 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,SubscriptNode, (SeExprvsp[-3].n), (SeExprvsp[-1].n)); }
#line 1755 "y.tab.c"
    break;

  case 31: /* e: e '?' e ':' e  */
#line 212 "SeExprParser.y"
                                { (SeExprval.n) = NODE3((SeExprloc).first_column,(SeExprloc).last_column,CondNode, (SeExprvsp[-4].n), (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1761 "y.tab.c"
    break;

  case 32: /* e: e OR e  */
#line 213 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,OrNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1767 "y.tab.c"
    break;

  case 33: /* e: e AND e  */
#line 214 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AndNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1773 "y.tab.c"
    break;

  case 34: /* e: e EQ e  */
#line 215 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,EqNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1779 "y.tab.c"
    break;

  case 35: /* e: e NE e  */
#line 216 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,NeNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1785 "y.tab.c"
    break;

  case 36: /* e: e '<' e  */
#line 217 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,LtNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1791 "y.tab.c"
    break;

  case 37: /* e: e '>' e  */
#line 218 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,GtNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1797 "y.tab.c"
    break;

  case 38: /* e: e LE e  */
#line 219 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,LeNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1803 "y.tab.c"
    break;

  case 39: /* e: e GE e  */
#line 220 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,GeNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1809 "y.tab.c"
    break;

  case 40: /* e: '+' e  */
#line 221 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1815 "y.tab.c"
    break;

  case 41: /* e: '-' e  */
#line 222 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,NegNode, (SeExprvsp[0].n)); }
#line 1821 "y.tab.c"
    break;

  case 42: /* e: '!' e  */
#line 223 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,NotNode, (SeExprvsp[0].n)); }
#line 1827 "y.tab.c"
    break;

  case 43: /* e: '~' e  */
#line 224 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,InvertNode, (SeExprvsp[0].n)); }
#line 1833 "y.tab.c"
    break;

  case 44: /* e: e '+' e  */
#line 225 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,AddNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1839 "y.tab.c"
    break;

  case 45: /* e: e '-' e  */
#line 226 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,SubNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1845 "y.tab.c"
    break;

  case 46: /* e: e '*' e  */
#line 227 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,MulNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1851 "y.tab.c"
    break;

  case 47: /* e: e '/' e  */
#line 228 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,DivNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1857 "y.tab.c"
    break;

  case 48: /* e: e '%' e  */
#line 229 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,ModNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1863 "y.tab.c"
    break;

  case 49: /* e: e '^' e  */
#line 230 "SeExprParser.y"
                                { (SeExprval.n) = NODE2((SeExprloc).first_column,(SeExprloc).last_column,ExpNode, (SeExprvsp[-2].n), (SeExprvsp[0].n)); }
#line 1869 "y.tab.c"
    break;

  case 50: /* e: NAME '(' optargs ')'  */
#line 231 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,FuncNode, (SeExprvsp[-3].s)); 
				  // add args directly and discard arg list node
				  (SeExprval.n)->addChildren((SeExprvsp[-1].n)); Forget((SeExprvsp[-1].n)); }
#line 1877 "y.tab.c"
    break;

  case 51: /* e: e ARROW NAME '(' optargs ')'  */
#line 235 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,FuncNode, (SeExprvsp[-3].s)); 
				  (SeExprval.n)->addChild((SeExprvsp[-5].n));
				  // add args directly and discard arg list node
				  (SeExprval.n)->addChildren((SeExprvsp[-1].n)); Forget((SeExprvsp[-1].n)); }
#line 1886 "y.tab.c"
    break;

  case 52: /* e: VAR  */
#line 239 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,VarNode, (SeExprvsp[0].s)); }
#line 1892 "y.tab.c"
    break;

  case 53: /* e: NAME  */
#line 240 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,VarNode, (SeExprvsp[0].s)); }
#line 1898 "y.tab.c"
    break;

  case 54: /* e: NUMBER  */
#line 241 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,NumNode, (SeExprvsp[0].d)); /*printf("line %d",@$.last_column);*/}
#line 1904 "y.tab.c"
    break;

  case 55: /* optargs: %empty  */
#line 246 "SeExprParser.y"
                                { (SeExprval.n) = NODE((SeExprloc).first_column,(SeExprloc).last_column,Node); /* create empty node */}
#line 1910 "y.tab.c"
    break;

  case 56: /* optargs: args  */
#line 247 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1916 "y.tab.c"
    break;

  case 57: /* args: arg  */
#line 252 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,Node, (SeExprvsp[0].n)); /* create arg list */}
#line 1922 "y.tab.c"
    break;

  case 58: /* args: args ',' arg  */
#line 253 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[-2].n); (SeExprvsp[-2].n)->addChild((SeExprvsp[0].n)); /* add to list */}
#line 1928 "y.tab.c"
    break;

  case 59: /* arg: e  */
#line 257 "SeExprParser.y"
                                { (SeExprval.n) = (SeExprvsp[0].n); }
#line 1934 "y.tab.c"
    break;

  case 60: /* arg: STR  */
#line 258 "SeExprParser.y"
                                { (SeExprval.n) = NODE1((SeExprloc).first_column,(SeExprloc).last_column,StrNode, (SeExprvsp[0].s));}
#line 1940 "y.tab.c"
    break;


#line 1944 "y.tab.c"

      default: break;
    }
  /* User semantic actions sometimes alter SeExprchar, and that requires
     that SeExprtoken be updated with the new translation.  We take the
     approach of translating immediately before every use of SeExprtoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     SeExprYYABORT, SeExprYYACCEPT, or SeExprYYERROR immediately after altering SeExprchar or
     if it invokes SeExprYYBACKUP.  In the case of SeExprYYABORT or SeExprYYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of SeExprYYERROR or SeExprYYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  SeExprYY_SYMBOL_PRINT ("-> $$ =", SeExprYY_CAST (SeExprsymbol_kind_t, SeExprr1[SeExprn]), &SeExprval, &SeExprloc);

  SeExprYYPOPSTACK (SeExprlen);
  SeExprlen = 0;

  *++SeExprvsp = SeExprval;
  *++SeExprlsp = SeExprloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int SeExprlhs = SeExprr1[SeExprn] - SeExprYYNTOKENS;
    const int SeExpri = SeExprpgoto[SeExprlhs] + *SeExprssp;
    SeExprstate = (0 <= SeExpri && SeExpri <= SeExprYYLAST && SeExprcheck[SeExpri] == *SeExprssp
               ? SeExprtable[SeExpri]
               : SeExprdefgoto[SeExprlhs]);
  }

  goto SeExprnewstate;


/*--------------------------------------.
| SeExprerrlab -- here on detecting error.  |
`--------------------------------------*/
SeExprerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  SeExprtoken = SeExprchar == SeExprYYEMPTY ? SeExprYYSYMBOL_SeExprYYEMPTY : SeExprYYTRANSLATE (SeExprchar);
  /* If not already recovering from an error, report this error.  */
  if (!SeExprerrstatus)
    {
      ++SeExprnerrs;
      SeExprerror (SeExprYY_("syntax error"));
    }

  SeExprerror_range[1] = SeExprlloc;
  if (SeExprerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (SeExprchar <= SeExprYYEOF)
        {
          /* Return failure if at end of input.  */
          if (SeExprchar == SeExprYYEOF)
            SeExprYYABORT;
        }
      else
        {
          SeExprdestruct ("Error: discarding",
                      SeExprtoken, &SeExprlval, &SeExprlloc);
          SeExprchar = SeExprYYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto SeExprerrlab1;


/*---------------------------------------------------.
| SeExprerrorlab -- error raised explicitly by SeExprYYERROR.  |
`---------------------------------------------------*/
SeExprerrorlab:
  /* Pacify compilers when the user code never invokes SeExprYYERROR and the
     label SeExprerrorlab therefore never appears in user code.  */
  if (0)
    SeExprYYERROR;
  ++SeExprnerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this SeExprYYERROR.  */
  SeExprYYPOPSTACK (SeExprlen);
  SeExprlen = 0;
  SeExprYY_STACK_PRINT (SeExprss, SeExprssp);
  SeExprstate = *SeExprssp;
  goto SeExprerrlab1;


/*-------------------------------------------------------------.
| SeExprerrlab1 -- common code for both syntax error and SeExprYYERROR.  |
`-------------------------------------------------------------*/
SeExprerrlab1:
  SeExprerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      SeExprn = SeExprpact[SeExprstate];
      if (!SeExprpact_value_is_default (SeExprn))
        {
          SeExprn += SeExprYYSYMBOL_SeExprYYerror;
          if (0 <= SeExprn && SeExprn <= SeExprYYLAST && SeExprcheck[SeExprn] == SeExprYYSYMBOL_SeExprYYerror)
            {
              SeExprn = SeExprtable[SeExprn];
              if (0 < SeExprn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (SeExprssp == SeExprss)
        SeExprYYABORT;

      SeExprerror_range[1] = *SeExprlsp;
      SeExprdestruct ("Error: popping",
                  SeExprYY_ACCESSING_SYMBOL (SeExprstate), SeExprvsp, SeExprlsp);
      SeExprYYPOPSTACK (1);
      SeExprstate = *SeExprssp;
      SeExprYY_STACK_PRINT (SeExprss, SeExprssp);
    }

  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++SeExprvsp = SeExprlval;
  SeExprYY_IGNORE_MAYBE_UNINITIALIZED_END

  SeExprerror_range[2] = SeExprlloc;
  ++SeExprlsp;
  SeExprYYLLOC_DEFAULT (*SeExprlsp, SeExprerror_range, 2);

  /* Shift the error token.  */
  SeExprYY_SYMBOL_PRINT ("Shifting", SeExprYY_ACCESSING_SYMBOL (SeExprn), SeExprvsp, SeExprlsp);

  SeExprstate = SeExprn;
  goto SeExprnewstate;


/*-------------------------------------.
| SeExpracceptlab -- SeExprYYACCEPT comes here.  |
`-------------------------------------*/
SeExpracceptlab:
  SeExprresult = 0;
  goto SeExprreturnlab;


/*-----------------------------------.
| SeExprabortlab -- SeExprYYABORT comes here.  |
`-----------------------------------*/
SeExprabortlab:
  SeExprresult = 1;
  goto SeExprreturnlab;


/*-----------------------------------------------------------.
| SeExprexhaustedlab -- SeExprYYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
SeExprexhaustedlab:
  SeExprerror (SeExprYY_("memory exhausted"));
  SeExprresult = 2;
  goto SeExprreturnlab;


/*----------------------------------------------------------.
| SeExprreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
SeExprreturnlab:
  if (SeExprchar != SeExprYYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      SeExprtoken = SeExprYYTRANSLATE (SeExprchar);
      SeExprdestruct ("Cleanup: discarding lookahead",
                  SeExprtoken, &SeExprlval, &SeExprlloc);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this SeExprYYABORT or SeExprYYACCEPT.  */
  SeExprYYPOPSTACK (SeExprlen);
  SeExprYY_STACK_PRINT (SeExprss, SeExprssp);
  while (SeExprssp != SeExprss)
    {
      SeExprdestruct ("Cleanup: popping",
                  SeExprYY_ACCESSING_SYMBOL (+*SeExprssp), SeExprvsp, SeExprlsp);
      SeExprYYPOPSTACK (1);
    }
#ifndef SeExproverflow
  if (SeExprss != SeExprssa)
    SeExprYYSTACK_FREE (SeExprss);
#endif

  return SeExprresult;
}

#line 261 "SeExprParser.y"


      /* SeExprerror - Report an error.  This is called by the parser.
	 (Note: the "msg" param is useless as it is usually just "parse error".
	 so it's ignored.)
      */
static void SeExprerror(const char* /*msg*/)
{
    // find start of line containing error
    int pos = SeExprpos(), lineno = 1, start = 0, end = strlen(ParseStr);
    bool multiline = 0;
    for (int i = start; i < pos; i++)
	if (ParseStr[i] == '\n') { start = i + 1; lineno++; multiline=1; }

    // find end of line containing error
    for (int i = end; i > pos; i--)
	if (ParseStr[i] == '\n') { end = i - 1; multiline=1; }

    ParseError = SeExprtext[0] ? "Syntax error" : "Unexpected end of expression";
    if (multiline) {
	char buff[30];
	snprintf(buff, 30, " at line %d", lineno);
	ParseError += buff;
    }
    if (SeExprtext[0]) {
	ParseError += " near '";
	ParseError += SeExprtext;
    }
    ParseError += "':\n    ";

    int s = std::max(start, pos-30);
    int e = std::min(end, pos+30);

    if (s != start) ParseError += "...";
    ParseError += std::string(ParseStr, s, e-s+1);
    if (e != end) ParseError += "...";
}


/* CallParser - This is our entrypoint from the rest of the expr library. 
   A string is passed in and a parse tree is returned.	If the tree is null,
   an error string is returned.  Any flags set during parsing are passed
   along.
 */

extern void resetCounters(std::vector<char*>* stringTokens);

static SeExprInternal::Mutex mutex;
int SeExprlex_destroy  (void);
bool SeExprParse(SeExprNode*& parseTree, std::string& error, int& errorStart, int& errorEnd,
    const SeExpression* expr, const char* str, 
    std::vector<char*>* stringTokens)
{
    SeExprInternal::AutoMutex locker(mutex);

    // glue around crippled C interface - ugh!
    Expr = expr;
    ParseStr = str;
    resetCounters(stringTokens); // reset lineNumber and columnNumber in scanner
    SeExpr_buffer_state* buffer = SeExpr_scan_string(str);
    ParseResult = 0;
    int resultCode = SeExprparse();
    SeExpr_delete_buffer(buffer);
    SeExprlex_destroy ();
    if (resultCode == 0) {
	// success
	error = "";
	parseTree = ParseResult;
    }
    else {
	// failure
	error = ParseError;
        errorStart=SeExprlloc.first_column;
        errorEnd=SeExprlloc.last_column;
	parseTree = 0;
	// gather list of nodes with no parent
	std::vector<SeExprNode*> delnodes;
	std::vector<SeExprNode*>::iterator iter;
	for (iter = ParseNodes.begin(); iter != ParseNodes.end(); iter++)
	    if (!(*iter)->parent()) { delnodes.push_back(*iter); }
	// now delete them (they will delete their own children)
	for (iter = delnodes.begin(); iter != delnodes.end(); iter++)
	    delete *iter;
    }
    ParseNodes.clear();

    return parseTree != 0;
}
