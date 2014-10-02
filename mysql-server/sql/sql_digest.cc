/* Copyright (c) 2008, 2014, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/*
  This code needs extra visibility in the lexer structures
*/

#include "my_global.h"
#include "my_sys.h"
#include "my_md5.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_signal.h"
#include "sql_get_diagnostics.h"
#include "sql_string.h"
#include "sql_digest.h"
#include "sql_digest_stream.h"

/* Generated code */
#include "sql_yacc.h"
#include "lex_token.h"

/* Name pollution from sql/sql_lex.h */
#ifdef LEX_YYSTYPE
#undef LEX_YYSTYPE
#endif

#define LEX_YYSTYPE YYSTYPE*

#define SIZE_OF_A_TOKEN 2

/**
  Read a single token from token array.
*/
inline int read_token(const sql_digest_storage *digest_storage,
                      int index, uint *tok)
{
  int safe_byte_count= digest_storage->m_byte_count;

  if (index + SIZE_OF_A_TOKEN <= safe_byte_count &&
      safe_byte_count <= MAX_DIGEST_STORAGE_SIZE)
  {
    const unsigned char *src= & digest_storage->m_token_array[index];
    *tok= src[0] | (src[1] << 8);
    return index + SIZE_OF_A_TOKEN;
  }

  /* The input byte stream is exhausted. */
  *tok= 0;
  return MAX_DIGEST_STORAGE_SIZE + 1;
}

/**
  Store a single token in token array.
*/
inline void store_token(sql_digest_storage* digest_storage, uint token)
{
  DBUG_ASSERT(digest_storage->m_byte_count >= 0);
  DBUG_ASSERT(digest_storage->m_byte_count <= MAX_DIGEST_STORAGE_SIZE);

  if (digest_storage->m_byte_count + SIZE_OF_A_TOKEN <= MAX_DIGEST_STORAGE_SIZE)
  {
    unsigned char* dest= & digest_storage->m_token_array[digest_storage->m_byte_count];
    dest[0]= token & 0xff;
    dest[1]= (token >> 8) & 0xff;
    digest_storage->m_byte_count+= SIZE_OF_A_TOKEN;
  }
  else
  {
    digest_storage->m_full= true;
  }
}

/**
  Read an identifier from token array.
*/
inline int read_identifier(const sql_digest_storage* digest_storage,
                           int index, char ** id_string, int *id_length)
{
  int new_index;
  DBUG_ASSERT(index <= digest_storage->m_byte_count);
  DBUG_ASSERT(digest_storage->m_byte_count <= MAX_DIGEST_STORAGE_SIZE);

  /*
    token + length + string are written in an atomic way,
    so we do always expect a length + string here
  */
  const unsigned char *src= & digest_storage->m_token_array[index];
  uint length= src[0] | (src[1] << 8);
  *id_string= (char *) (src + 2);
  *id_length= length;

  new_index= index + SIZE_OF_A_TOKEN + length;
  DBUG_ASSERT(new_index <= digest_storage->m_byte_count);
  return new_index;
}

/**
  Store an identifier in token array.
*/
inline void store_token_identifier(sql_digest_storage* digest_storage,
                                   uint token,
                                   size_t id_length, const char *id_name)
{
  DBUG_ASSERT(digest_storage->m_byte_count >= 0);
  DBUG_ASSERT(digest_storage->m_byte_count <= MAX_DIGEST_STORAGE_SIZE);

  size_t bytes_needed= 2 * SIZE_OF_A_TOKEN + id_length;
  if (digest_storage->m_byte_count + bytes_needed <= MAX_DIGEST_STORAGE_SIZE)
  {
    unsigned char* dest= & digest_storage->m_token_array[digest_storage->m_byte_count];
    /* Write the token */
    dest[0]= token & 0xff;
    dest[1]= (token >> 8) & 0xff;
    /* Write the string length */
    dest[2]= id_length & 0xff;
    dest[3]= (id_length >> 8) & 0xff;
    /* Write the string data */
    if (id_length > 0)
      memcpy((char *)(dest + 4), id_name, id_length);
    digest_storage->m_byte_count+= bytes_needed;
  }
  else
  {
    digest_storage->m_full= true;
  }
}

void compute_digest_md5(const sql_digest_storage *digest_storage, unsigned char *md5)
{
  compute_md5_hash((char *) md5,
                   (const char *) digest_storage->m_token_array,
                   digest_storage->m_byte_count);
}

/*
  Iterate token array and updates digest_text.
*/
void compute_digest_text(const sql_digest_storage* digest_storage,
                         char* digest_text,
                         size_t digest_text_length,
                         bool *truncated_ptr)
{
  DBUG_ASSERT(digest_storage != NULL);
  bool truncated= false;
  int byte_count= digest_storage->m_byte_count;
  char *digest_output= digest_text;
  int bytes_needed= 0;
  uint tok= 0;
  int current_byte= 0;
  lex_token_string *tok_data;

  /* -4 is to make sure extra space for '...' and a '\0' at the end. */
  int bytes_available= digest_text_length - 4;

  if (byte_count <= 0 || byte_count > MAX_DIGEST_STORAGE_SIZE)
  {
    *digest_text= '\0';
    *truncated_ptr= false;
    return;
  }

  /* Convert text to utf8 */
  const CHARSET_INFO *from_cs= get_charset(digest_storage->m_charset_number, MYF(0));
  const CHARSET_INFO *to_cs= &my_charset_utf8_bin;

  if (from_cs == NULL)
  {
    /*
      Can happen, as we do dirty reads on digest_storage,
      which can be written to in another thread.
    */
    *digest_text= '\0';
    *truncated_ptr= false;
    return;
  }

  /*
     Max converted size is number of characters * max multibyte length of the
     target charset, which is 4 for UTF8.
   */
  const uint max_converted_size= MAX_DIGEST_STORAGE_SIZE * 4;
  char id_buffer[max_converted_size];
  char *id_string;
  size_t  id_length;
  bool convert_text= !my_charset_same(from_cs, to_cs);

  DBUG_ASSERT(byte_count <= MAX_DIGEST_STORAGE_SIZE);

  while ((current_byte < byte_count) &&
         (bytes_available > 0) &&
         !truncated)
  {
    current_byte= read_token(digest_storage, current_byte, &tok);

    if (tok <= 0 || tok >= array_elements(lex_token_array))
    {
      *digest_text='\0';
      *truncated_ptr= false;
      return;
    }

    tok_data= &lex_token_array[tok];

    switch (tok)
    {
    /* All identifiers are printed with their name. */
    case IDENT:
    case IDENT_QUOTED:
      {
        char *id_ptr;
        int id_len;
        uint err_cs= 0;

        /* Get the next identifier from the storage buffer. */
        current_byte= read_identifier(digest_storage, current_byte,
                                      &id_ptr, &id_len);
        if (convert_text)
        {
          /* Verify that the converted text will fit. */
          if (to_cs->mbmaxlen*id_len > max_converted_size)
          {
            truncated= true;
            break;
          }
          /* Convert identifier string into the storage character set. */
          id_length= my_convert(id_buffer, max_converted_size, to_cs,
                                id_ptr, id_len, from_cs, &err_cs);
          id_string= id_buffer;
        }
        else
        {
          id_string= id_ptr;
          id_length= id_len;
        }

        if (id_length == 0 || err_cs != 0)
        {
          truncated= true;
          break;
        }
        /* Copy the converted identifier into the digest string. */
        bytes_needed= id_length + (tok == IDENT ? 1 : 3);
        if (bytes_needed <= bytes_available)
        {
          if (tok == IDENT_QUOTED)
            *digest_output++= '`';
          if (id_length > 0)
          {
            memcpy(digest_output, id_string, id_length);
            digest_output+= id_length;
          }
          if (tok == IDENT_QUOTED)
            *digest_output++= '`';
          *digest_output++= ' ';
          bytes_available-= bytes_needed;
        }
        else
        {
          truncated= true;
        }
      }
      break;

    /* Everything else is printed as is. */
    default:
      /*
        Make sure not to overflow digest_text buffer.
        +1 is to make sure extra space for ' '.
      */
      int tok_length= tok_data->m_token_length;
      bytes_needed= tok_length + 1;

      if (bytes_needed <= bytes_available)
      {
        strncpy(digest_output, tok_data->m_token_string, tok_length);
        digest_output+= tok_length;
        if (tok_data->m_append_space)
        {
          *digest_output++= ' ';
        }
        bytes_available-= bytes_needed;
      }
      else
      {
        truncated= true;
      }
      break;
    }
  }

  /* Truncate digest text in case of long queries. */
  if (digest_storage->m_full || truncated)
  {
    strcpy(digest_output, "...");
    digest_output+= 3;
  }

  *truncated_ptr= truncated;
  *digest_output= '\0';
}

static inline uint peek_token(const sql_digest_storage *digest, int index)
{
  uint token;
  DBUG_ASSERT(index >= 0);
  DBUG_ASSERT(index + SIZE_OF_A_TOKEN <= digest->m_byte_count);
  DBUG_ASSERT(digest->m_byte_count <=  MAX_DIGEST_STORAGE_SIZE);

  token= ((digest->m_token_array[index + 1])<<8) | digest->m_token_array[index];
  return token;
}

/**
  Function to read last two tokens from token array. If an identifier
  is found, do not look for token before that.
*/
static inline void peek_last_two_tokens(const sql_digest_storage* digest_storage,
                                        int last_id_index, uint *t1, uint *t2)
{
  int byte_count= digest_storage->m_byte_count;
  int peek_index= byte_count - SIZE_OF_A_TOKEN;

  if (last_id_index <= peek_index)
  {
    /* Take last token. */
    *t1= peek_token(digest_storage, peek_index);

    peek_index-= SIZE_OF_A_TOKEN;
    if (last_id_index <= peek_index)
    {
      /* Take 2nd token from last. */
      *t2= peek_token(digest_storage, peek_index);
    }
    else
    {
      *t2= TOK_UNUSED;
    }
  }
  else
  {
    *t1= TOK_UNUSED;
    *t2= TOK_UNUSED;
  }
}

sql_digest_state* digest_add_token(sql_digest_state *state,
                                   uint token,
                                   LEX_YYSTYPE yylval)
{
  sql_digest_storage *digest_storage= NULL;

  digest_storage= &state->m_digest_storage;

  /*
    Stop collecting further tokens if digest storage is full or
    if END token is received.
  */
  if (digest_storage->m_full || token == END_OF_INPUT)
    return NULL;

  /*
    Take last_token 2 tokens collected till now. These tokens will be used
    in reduce for normalisation. Make sure not to consider ID tokens in reduce.
  */
  uint last_token;
  uint last_token2;

  switch (token)
  {
    case BIN_NUM:
    case DECIMAL_NUM:
    case FLOAT_NUM:
    case HEX_NUM:
    case LEX_HOSTNAME:
    case LONG_NUM:
    case NUM:
    case TEXT_STRING:
    case NCHAR_STRING:
    case ULONGLONG_NUM:
    case PARAM_MARKER:
    {
      /*
        REDUCE:
        TOK_GENERIC_VALUE := BIN_NUM | DECIMAL_NUM | ... | ULONGLONG_NUM
      */
      token= TOK_GENERIC_VALUE;
    }
    /* fall through */
    case NULL_SYM:
    {
      peek_last_two_tokens(digest_storage, state->m_last_id_index,
                           &last_token, &last_token2);

      if ((last_token2 == TOK_GENERIC_VALUE ||
           last_token2 == TOK_GENERIC_VALUE_LIST ||
           last_token2 == NULL_SYM) &&
          (last_token == ','))
      {
        /*
          REDUCE:
          TOK_GENERIC_VALUE_LIST :=
            (TOK_GENERIC_VALUE|NULL_SYM) ',' (TOK_GENERIC_VALUE|NULL_SYM)

          REDUCE:
          TOK_GENERIC_VALUE_LIST :=
            TOK_GENERIC_VALUE_LIST ',' (TOK_GENERIC_VALUE|NULL_SYM)
        */
        digest_storage->m_byte_count-= 2*SIZE_OF_A_TOKEN;
        token= TOK_GENERIC_VALUE_LIST;
      }
      /*
        Add this token or the resulting reduce to digest storage.
      */
      store_token(digest_storage, token);
      break;
    }
    case ')':
    {
      peek_last_two_tokens(digest_storage, state->m_last_id_index,
                           &last_token, &last_token2);

      if (last_token == TOK_GENERIC_VALUE &&
          last_token2 == '(')
      {
        /*
          REDUCE:
          TOK_ROW_SINGLE_VALUE :=
            '(' TOK_GENERIC_VALUE ')'
        */
        digest_storage->m_byte_count-= 2*SIZE_OF_A_TOKEN;
        token= TOK_ROW_SINGLE_VALUE;

        /* Read last two tokens again */
        peek_last_two_tokens(digest_storage, state->m_last_id_index,
                             &last_token, &last_token2);

        if ((last_token2 == TOK_ROW_SINGLE_VALUE ||
             last_token2 == TOK_ROW_SINGLE_VALUE_LIST) &&
            (last_token == ','))
        {
          /*
            REDUCE:
            TOK_ROW_SINGLE_VALUE_LIST :=
              TOK_ROW_SINGLE_VALUE ',' TOK_ROW_SINGLE_VALUE

            REDUCE:
            TOK_ROW_SINGLE_VALUE_LIST :=
              TOK_ROW_SINGLE_VALUE_LIST ',' TOK_ROW_SINGLE_VALUE
          */
          digest_storage->m_byte_count-= 2*SIZE_OF_A_TOKEN;
          token= TOK_ROW_SINGLE_VALUE_LIST;
        }
      }
      else if (last_token == TOK_GENERIC_VALUE_LIST &&
               last_token2 == '(')
      {
        /*
          REDUCE:
          TOK_ROW_MULTIPLE_VALUE :=
            '(' TOK_GENERIC_VALUE_LIST ')'
        */
        digest_storage->m_byte_count-= 2*SIZE_OF_A_TOKEN;
        token= TOK_ROW_MULTIPLE_VALUE;

        /* Read last two tokens again */
        peek_last_two_tokens(digest_storage, state->m_last_id_index,
                             &last_token, &last_token2);

        if ((last_token2 == TOK_ROW_MULTIPLE_VALUE ||
             last_token2 == TOK_ROW_MULTIPLE_VALUE_LIST) &&
            (last_token == ','))
        {
          /*
            REDUCE:
            TOK_ROW_MULTIPLE_VALUE_LIST :=
              TOK_ROW_MULTIPLE_VALUE ',' TOK_ROW_MULTIPLE_VALUE

            REDUCE:
            TOK_ROW_MULTIPLE_VALUE_LIST :=
              TOK_ROW_MULTIPLE_VALUE_LIST ',' TOK_ROW_MULTIPLE_VALUE
          */
          digest_storage->m_byte_count-= 2*SIZE_OF_A_TOKEN;
          token= TOK_ROW_MULTIPLE_VALUE_LIST;
        }
      }
      /*
        Add this token or the resulting reduce to digest storage.
      */
      store_token(digest_storage, token);
      break;
    }
    case IDENT:
    case IDENT_QUOTED:
    {
      YYSTYPE *lex_token= yylval;
      char *yytext= lex_token->lex_str.str;
      size_t yylen= lex_token->lex_str.length;

      /* Add this token and identifier string to digest storage. */
      store_token_identifier(digest_storage, token, yylen, yytext);

      /* Update the index of last identifier found. */
      state->m_last_id_index= digest_storage->m_byte_count;
      break;
    }
    default:
    {
      /* Add this token to digest storage. */
      store_token(digest_storage, token);
      break;
    }
  }

  return state;
}

