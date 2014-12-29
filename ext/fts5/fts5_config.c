/*
** 2014 Jun 09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is an SQLite module implementing full-text search.
*/

#include "fts5Int.h"

#define FTS5_DEFAULT_PAGE_SIZE   1000
#define FTS5_DEFAULT_AUTOMERGE      4

/* Maximum allowed page size */
#define FTS5_MAX_PAGE_SIZE (128*1024)

static int fts5_iswhitespace(char x){
  return (x==' ');
}

static int fts5_isopenquote(char x){
  return (x=='"' || x=='\'' || x=='[' || x=='`');
}

/*
** Argument pIn points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *pIn in 
** the string that is not a white-space character.
*/
static const char *fts5ConfigSkipWhitespace(const char *pIn){
  const char *p = pIn;
  if( p ){
    while( fts5_iswhitespace(*p) ){ p++; }
  }
  return p;
}

/*
** Argument pIn points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *pIn in 
** the string that is not a "bareword" character.
*/
static const char *fts5ConfigSkipBareword(const char *pIn){
  const char *p = pIn;
  while( *p      && *p!=' ' && *p!=':' && *p!='!' && *p!='@' 
      && *p!='#' && *p!='$' && *p!='%' && *p!='^' && *p!='&' 
      && *p!='*' && *p!='(' && *p!=')' && *p!='='
  ){
    p++;
  }
  if( p==pIn ) p = 0;
  return p;
}

static int fts5_isdigit(char a){
  return (a>='0' && a<='9');
}



static const char *fts5ConfigSkipLiteral(const char *pIn){
  const char *p = pIn;
  if( p ){
    switch( *p ){
      case 'n': case 'N':
        if( sqlite3_strnicmp("null", p, 4)==0 ){
          p = &p[4];
        }else{
          p = 0;
        }
        break;
        
      case 'x': case 'X':
        p++;
        if( *p=='\'' ){
          p++;
          while( (*p>='a' && *p<='f') 
              || (*p>='A' && *p<='F') 
              || (*p>='0' && *p<='9') 
          ){
            p++;
          }
          if( *p=='\'' && 0==((p-pIn)%2) ){
            p++;
          }else{
            p = 0;
          }
        }else{
          p = 0;
        }
        break;

      case '\'':
        p++;
        while( p ){
          if( *p=='\'' ){
            p++;
            if( *p!='\'' ) break;
          }
          p++;
          if( *p==0 ) p = 0;
        }
        break;

      default:
        /* maybe a number */
        if( *p=='+' || *p=='-' ) p++;
        while( fts5_isdigit(*p) ) p++;

        /* At this point, if the literal was an integer, the parse is 
        ** finished. Or, if it is a floating point value, it may continue
        ** with either a decimal point or an 'E' character. */
        if( *p=='.' && fts5_isdigit(p[1]) ){
          p += 2;
          while( fts5_isdigit(*p) ) p++;
        }
        if( p==pIn ) p = 0;

        break;
    }
  }

  return p;
}

static int fts5Dequote(char *z){
  char q;
  int iIn = 1;
  int iOut = 0;
  int bRet = 1;
  q = z[0];

  assert( q=='[' || q=='\'' || q=='"' || q=='`' );
  if( q=='[' ) q = ']';  

  while( z[iIn] ){
    if( z[iIn]==q ){
      if( z[iIn+1]!=q ){
        if( z[iIn+1]=='\0' ) bRet = 0;
        break;
      }
      z[iOut++] = q;
      iIn += 2;
    }else{
      z[iOut++] = z[iIn++];
    }
  }
  z[iOut] = '\0';

  return bRet;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
void sqlite3Fts5Dequote(char *z){
  char quote;                     /* Quote character (if any ) */

  assert( 0==fts5_iswhitespace(z[0]) );
  quote = z[0];
  if( quote=='[' || quote=='\'' || quote=='"' || quote=='`' ){
    fts5Dequote(z);
  }
}

/*
** Trim any white-space from the right of nul-terminated string z.
*/
static char *fts5TrimString(char *z){
  int n = strlen(z);
  while( n>0 && fts5_iswhitespace(z[n-1]) ){
    z[--n] = '\0';
  }
  while( fts5_iswhitespace(*z) ) z++;
  return z;
}

/*
** Parse the "special" CREATE VIRTUAL TABLE directive and update
** configuration object pConfig as appropriate.
**
** If successful, object pConfig is updated and SQLITE_OK returned. If
** an error occurs, an SQLite error code is returned and an error message
** may be left in *pzErr. It is the responsibility of the caller to
** eventually free any such error message using sqlite3_free().
*/
static int fts5ConfigParseSpecial(
  Fts5Global *pGlobal,
  Fts5Config *pConfig,            /* Configuration object to update */
  const char *zCmd,               /* Special command to parse */
  int nCmd,                       /* Size of zCmd in bytes */
  const char *zArg,               /* Argument to parse */
  char **pzErr                    /* OUT: Error message */
){
  if( sqlite3_strnicmp("prefix", zCmd, nCmd)==0 ){
    const int nByte = sizeof(int) * FTS5_MAX_PREFIX_INDEXES;
    int rc = SQLITE_OK;
    const char *p;
    if( pConfig->aPrefix ){
      *pzErr = sqlite3_mprintf("multiple prefix=... directives");
      rc = SQLITE_ERROR;
    }else{
      pConfig->aPrefix = sqlite3Fts5MallocZero(&rc, nByte);
    }
    p = zArg;
    while( rc==SQLITE_OK && p[0] ){
      int nPre = 0;
      while( p[0]==' ' ) p++;
      while( p[0]>='0' && p[0]<='9' && nPre<1000 ){
        nPre = nPre*10 + (p[0] - '0');
        p++;
      }
      while( p[0]==' ' ) p++;
      if( p[0]==',' ){
        p++;
      }else if( p[0] ){
        *pzErr = sqlite3_mprintf("malformed prefix=... directive");
        rc = SQLITE_ERROR;
      }
      if( rc==SQLITE_OK && (nPre==0 || nPre>=1000) ){
        *pzErr = sqlite3_mprintf("prefix length out of range: %d", nPre);
        rc = SQLITE_ERROR;
      }
      pConfig->aPrefix[pConfig->nPrefix] = nPre;
      pConfig->nPrefix++;
    }
    return rc;
  }

  if( sqlite3_strnicmp("tokenize", zCmd, nCmd)==0 ){
    int rc = SQLITE_OK;
    const char *p = (const char*)zArg;
    int nArg = strlen(zArg) + 1;
    char **azArg = sqlite3Fts5MallocZero(&rc, sizeof(char*) * nArg);
    char *pDel = sqlite3Fts5MallocZero(&rc, nArg * 2);
    char *pSpace = pDel;

    if( azArg && pSpace ){
      if( pConfig->pTok ){
        *pzErr = sqlite3_mprintf("multiple tokenize=... directives");
        rc = SQLITE_ERROR;
      }else{
        for(nArg=0; p && *p; nArg++){
          const char *p2 = fts5ConfigSkipWhitespace(p);
          if( p2 && *p2=='\'' ){
            p = fts5ConfigSkipLiteral(p2);
          }else{
            p = fts5ConfigSkipBareword(p2);
          }
          if( p ){
            memcpy(pSpace, p2, p-p2);
            azArg[nArg] = pSpace;
            sqlite3Fts5Dequote(pSpace);
            pSpace += (p - p2) + 1;
            p = fts5ConfigSkipWhitespace(p);
          }
        }
        if( p==0 ){
          *pzErr = sqlite3_mprintf("parse error in tokenize directive");
          rc = SQLITE_ERROR;
        }else{
          rc = sqlite3Fts5GetTokenizer(pGlobal, 
              (const char**)azArg, nArg, &pConfig->pTok, &pConfig->pTokApi
          );
          if( rc!=SQLITE_OK ){
            *pzErr = sqlite3_mprintf("error in tokenizer constructor");
          }
        }
      }
    }

    sqlite3_free(azArg);
    sqlite3_free(pDel);
    return rc;
  }

  *pzErr = sqlite3_mprintf("unrecognized directive: \"%s\"", zCmd);
  return SQLITE_ERROR;
}

/*
** Duplicate the string passed as the only argument into a buffer allocated
** by sqlite3_malloc().
**
** Return 0 if an OOM error is encountered.
*/
static char *fts5Strdup(int *pRc, const char *z){
  char *pRet = 0;
  if( *pRc==SQLITE_OK ){
    pRet = sqlite3_mprintf("%s", z);
    if( pRet==0 ) *pRc = SQLITE_NOMEM;
  }
  return pRet;
}

/*
** Allocate an instance of the default tokenizer ("simple") at 
** Fts5Config.pTokenizer. Return SQLITE_OK if successful, or an SQLite error
** code if an error occurs.
*/
static int fts5ConfigDefaultTokenizer(Fts5Global *pGlobal, Fts5Config *pConfig){
  assert( pConfig->pTok==0 && pConfig->pTokApi==0 );
  return sqlite3Fts5GetTokenizer(
      pGlobal, 0, 0, &pConfig->pTok, &pConfig->pTokApi
  );
}

/*
** Arguments nArg/azArg contain the string arguments passed to the xCreate
** or xConnect method of the virtual table. This function attempts to 
** allocate an instance of Fts5Config containing the results of parsing
** those arguments.
**
** If successful, SQLITE_OK is returned and *ppOut is set to point to the
** new Fts5Config object. If an error occurs, an SQLite error code is 
** returned, *ppOut is set to NULL and an error message may be left in
** *pzErr. It is the responsibility of the caller to eventually free any 
** such error message using sqlite3_free().
*/
int sqlite3Fts5ConfigParse(
  Fts5Global *pGlobal,
  sqlite3 *db,
  int nArg,                       /* Number of arguments */
  const char **azArg,             /* Array of nArg CREATE VIRTUAL TABLE args */
  Fts5Config **ppOut,             /* OUT: Results of parse */
  char **pzErr                    /* OUT: Error message */
){
  int rc = SQLITE_OK;             /* Return code */
  Fts5Config *pRet;               /* New object to return */
  int i;

  *ppOut = pRet = (Fts5Config*)sqlite3_malloc(sizeof(Fts5Config));
  if( pRet==0 ) return SQLITE_NOMEM;
  memset(pRet, 0, sizeof(Fts5Config));
  pRet->db = db;
  pRet->iCookie = -1;

  pRet->azCol = (char**)sqlite3Fts5MallocZero(&rc, sizeof(char*) * nArg);
  pRet->zDb = fts5Strdup(&rc, azArg[1]);
  pRet->zName = fts5Strdup(&rc, azArg[2]);
  if( rc==SQLITE_OK && sqlite3_stricmp(pRet->zName, FTS5_RANK_NAME)==0 ){
    *pzErr = sqlite3_mprintf("reserved fts5 table name: %s", pRet->zName);
    rc = SQLITE_ERROR;
  }

  for(i=3; rc==SQLITE_OK && i<nArg; i++){
    char *zDup = fts5Strdup(&rc, azArg[i]);
    if( zDup ){
      char *zCol = 0;
      int bParseError = 0;

      /* Check if this is a quoted column name */
      if( fts5_isopenquote(zDup[0]) ){
        bParseError = fts5Dequote(zDup);
        zCol = zDup;
      }else{
        char *z = (char*)fts5ConfigSkipBareword(zDup);
        if( *z=='\0' ){
          zCol = zDup;
        }else{
          int nCmd = z - zDup;
          z = (char*)fts5ConfigSkipWhitespace(z);
          if( *z!='=' ){
            bParseError = 1;
          }else{
            z++;
            z = fts5TrimString(z);
            if( fts5_isopenquote(*z) ){
              if( fts5Dequote(z) ) bParseError = 1;
            }else{
              char *z2 = (char*)fts5ConfigSkipBareword(z);
              if( *z2 ) bParseError = 1;
            }
            if( bParseError==0 ){
              rc = fts5ConfigParseSpecial(pGlobal, pRet, zDup, nCmd, z, pzErr);
            }
          }
        }
      }

      if( bParseError ){
        assert( *pzErr==0 );
        *pzErr = sqlite3_mprintf("parse error in \"%s\"", zDup);
        rc = SQLITE_ERROR;
      }else if( zCol ){
        if( 0==sqlite3_stricmp(zCol, FTS5_RANK_NAME) 
         || 0==sqlite3_stricmp(zCol, FTS5_ROWID_NAME) 
        ){
          *pzErr = sqlite3_mprintf("reserved fts5 column name: %s", zCol);
          rc = SQLITE_ERROR;
        }else{
          pRet->azCol[pRet->nCol++] = zCol;
          zDup = 0;
        }
      }

      sqlite3_free(zDup);
    }
  }

  /* If a tokenizer= option was successfully parsed, the tokenizer has
  ** already been allocated. Otherwise, allocate an instance of the default
  ** tokenizer (simple) now.  */
  if( rc==SQLITE_OK && pRet->pTok==0 ){
    rc = fts5ConfigDefaultTokenizer(pGlobal, pRet);
  }

  if( rc!=SQLITE_OK ){
    sqlite3Fts5ConfigFree(pRet);
    *ppOut = 0;
  }
  return rc;
}

/*
** Free the configuration object passed as the only argument.
*/
void sqlite3Fts5ConfigFree(Fts5Config *pConfig){
  if( pConfig ){
    int i;
    if( pConfig->pTok && pConfig->pTokApi->xDelete ){
      pConfig->pTokApi->xDelete(pConfig->pTok);
    }
    sqlite3_free(pConfig->zDb);
    sqlite3_free(pConfig->zName);
    for(i=0; i<pConfig->nCol; i++){
      sqlite3_free(pConfig->azCol[i]);
    }
    sqlite3_free(pConfig->azCol);
    sqlite3_free(pConfig->aPrefix);
    sqlite3_free(pConfig->zRank);
    sqlite3_free(pConfig->zRankArgs);
    sqlite3_free(pConfig);
  }
}

/*
** Call sqlite3_declare_vtab() based on the contents of the configuration
** object passed as the only argument. Return SQLITE_OK if successful, or
** an SQLite error code if an error occurs.
*/
int sqlite3Fts5ConfigDeclareVtab(Fts5Config *pConfig){
  int i;
  int rc;
  char *zSql;
  char *zOld;

  zSql = (char*)sqlite3_mprintf("CREATE TABLE x(");
  for(i=0; zSql && i<pConfig->nCol; i++){
    zOld = zSql;
    zSql = sqlite3_mprintf("%s%s%Q", zOld, (i==0?"":", "), pConfig->azCol[i]);
    sqlite3_free(zOld);
  }

  if( zSql ){
    zOld = zSql;
    zSql = sqlite3_mprintf("%s, %Q HIDDEN, %s HIDDEN)", 
        zOld, pConfig->zName, FTS5_RANK_NAME
    );
    sqlite3_free(zOld);
  }

  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_declare_vtab(pConfig->db, zSql);
    sqlite3_free(zSql);
  }
  
  return rc;
}

/*
** Tokenize the text passed via the second and third arguments.
**
** The callback is invoked once for each token in the input text. The
** arguments passed to it are, in order:
**
**     void *pCtx          // Copy of 4th argument to sqlite3Fts5Tokenize()
**     const char *pToken  // Pointer to buffer containing token
**     int nToken          // Size of token in bytes
**     int iStart          // Byte offset of start of token within input text
**     int iEnd            // Byte offset of end of token within input text
**     int iPos            // Position of token in input (first token is 0)
**
** If the callback returns a non-zero value the tokenization is abandoned
** and no further callbacks are issued. 
**
** This function returns SQLITE_OK if successful or an SQLite error code
** if an error occurs. If the tokenization was abandoned early because
** the callback returned SQLITE_DONE, this is not an error and this function
** still returns SQLITE_OK. Or, if the tokenization was abandoned early
** because the callback returned another non-zero value, it is assumed
** to be an SQLite error code and returned to the caller.
*/
int sqlite3Fts5Tokenize(
  Fts5Config *pConfig,            /* FTS5 Configuration object */
  const char *pText, int nText,   /* Text to tokenize */
  void *pCtx,                     /* Context passed to xToken() */
  int (*xToken)(void*, const char*, int, int, int, int)    /* Callback */
){
  return pConfig->pTokApi->xTokenize(pConfig->pTok, pCtx, pText, nText, xToken);
}

/*
** Argument pIn points to the first character in what is expected to be
** a comma-separated list of SQL literals followed by a ')' character.
** If it actually is this, return a pointer to the ')'. Otherwise, return
** NULL to indicate a parse error.
*/
static const char *fts5ConfigSkipArgs(const char *pIn){
  const char *p = pIn;
  
  while( 1 ){
    p = fts5ConfigSkipWhitespace(p);
    p = fts5ConfigSkipLiteral(p);
    p = fts5ConfigSkipWhitespace(p);
    if( p==0 || *p==')' ) break;
    if( *p!=',' ){
      p = 0;
      break;
    }
    p++;
  }

  return p;
}

/*
** Parameter zIn contains a rank() function specification. The format of 
** this is:
**
**   + Bareword (function name)
**   + Open parenthesis - "("
**   + Zero or more SQL literals in a comma separated list
**   + Close parenthesis - ")"
*/
static int fts5ConfigParseRank(
  const char *zIn,                /* Input string */
  char **pzRank,                  /* OUT: Rank function name */
  char **pzRankArgs               /* OUT: Rank function arguments */
){
  const char *p = zIn;
  const char *pRank;
  char *zRank = 0;
  char *zRankArgs = 0;
  int rc = SQLITE_OK;

  *pzRank = 0;
  *pzRankArgs = 0;

  p = fts5ConfigSkipWhitespace(p);
  pRank = p;
  p = fts5ConfigSkipBareword(p);

  if( p ){
    zRank = sqlite3Fts5MallocZero(&rc, 1 + p - pRank);
    if( zRank ) memcpy(zRank, pRank, p-pRank);
  }else{
    rc = SQLITE_ERROR;
  }

  if( rc==SQLITE_OK ){
    p = fts5ConfigSkipWhitespace(p);
    if( *p!='(' ) rc = SQLITE_ERROR;
    p++;
  }
  if( rc==SQLITE_OK ){
    const char *pArgs; 
    p = fts5ConfigSkipWhitespace(p);
    pArgs = p;
    if( *p!=')' ){
      p = fts5ConfigSkipArgs(p);
      if( p==0 ){
        rc = SQLITE_ERROR;
      }else if( p!=pArgs ){
        zRankArgs = sqlite3Fts5MallocZero(&rc, 1 + p - pArgs);
        if( zRankArgs ) memcpy(zRankArgs, pArgs, p-pArgs);
      }
    }
  }

  if( rc!=SQLITE_OK ){
    sqlite3_free(zRank);
    assert( zRankArgs==0 );
  }else{
    *pzRank = zRank;
    *pzRankArgs = zRankArgs;
  }
  return rc;
}

int sqlite3Fts5ConfigSetValue(
  Fts5Config *pConfig, 
  const char *zKey, 
  sqlite3_value *pVal,
  int *pbBadkey
){
  int rc = SQLITE_OK;
  if(      0==sqlite3_stricmp(zKey, "cookie") ){
    pConfig->iCookie = sqlite3_value_int(pVal);
  }

  else if( 0==sqlite3_stricmp(zKey, "pgsz") ){
    int pgsz = 0;
    if( SQLITE_INTEGER==sqlite3_value_numeric_type(pVal) ){
      pgsz = sqlite3_value_int(pVal);
    }
    if( pgsz<=0 || pgsz>FTS5_MAX_PAGE_SIZE ){
      if( pbBadkey ) *pbBadkey = 1;
    }else{
      pConfig->pgsz = pgsz;
    }
  }

  else if( 0==sqlite3_stricmp(zKey, "automerge") ){
    int nAutomerge = -1;
    if( SQLITE_INTEGER==sqlite3_value_numeric_type(pVal) ){
      nAutomerge = sqlite3_value_int(pVal);
    }
    if( nAutomerge<0 || nAutomerge>64 ){
      if( pbBadkey ) *pbBadkey = 1;
    }else{
      if( nAutomerge==1 ) nAutomerge = FTS5_DEFAULT_AUTOMERGE;
      pConfig->nAutomerge = nAutomerge;
    }
  }

  else if( 0==sqlite3_stricmp(zKey, "rank") ){
    const char *zIn = (const char*)sqlite3_value_text(pVal);
    char *zRank;
    char *zRankArgs;
    rc = fts5ConfigParseRank(zIn, &zRank, &zRankArgs);
    if( rc==SQLITE_OK ){
      sqlite3_free(pConfig->zRank);
      sqlite3_free(pConfig->zRankArgs);
      pConfig->zRank = zRank;
      pConfig->zRankArgs = zRankArgs;
    }else if( rc==SQLITE_ERROR ){
      rc = SQLITE_OK;
      if( pbBadkey ) *pbBadkey = 1;
    }
  }else{
    if( pbBadkey ) *pbBadkey = 1;
  }
  return rc;
}

/*
** Load the contents of the %_config table into memory.
*/
int sqlite3Fts5ConfigLoad(Fts5Config *pConfig, int iCookie){
  const char *zSelect = "SELECT k, v FROM %Q.'%q_config'";
  char *zSql;
  sqlite3_stmt *p = 0;
  int rc;

  /* Set default values */
  pConfig->pgsz = FTS5_DEFAULT_PAGE_SIZE;
  pConfig->nAutomerge = FTS5_DEFAULT_AUTOMERGE;

  zSql = sqlite3_mprintf(zSelect, pConfig->zDb, pConfig->zName);
  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &p, 0);
    sqlite3_free(zSql);
  }

  assert( rc==SQLITE_OK || p==0 );
  if( rc==SQLITE_OK ){
    while( SQLITE_ROW==sqlite3_step(p) ){
      const char *zK = (const char*)sqlite3_column_text(p, 0);
      sqlite3_value *pVal = sqlite3_column_value(p, 1);
      sqlite3Fts5ConfigSetValue(pConfig, zK, pVal, 0);
    }
    rc = sqlite3_finalize(p);
  }

  if( rc==SQLITE_OK ){
    pConfig->iCookie = iCookie;
  }
  return rc;
}
