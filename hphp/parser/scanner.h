/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2016 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_PARSER_SCANNER_H_
#define incl_HPHP_PARSER_SCANNER_H_

#include <sstream>
#include <cstdint>
#include <string>
#include <limits>
#include <cstdlib>
#include <limits.h>

#include "hphp/util/exception.h"
#include "hphp/util/portability.h"
#include "hphp/parser/location.h"
#include "hphp/parser/hphp.tab.hpp"

#ifndef YY_TYPEDEF_YY_SIZE_T
#define YY_TYPEDEF_YY_SIZE_T
typedef size_t yy_size_t;
#endif

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

using TokenID = int;

struct ScannerToken {
  void reset() {
    m_num = 0;
    m_text.clear();
  }

  TokenID num() const {
    return m_num;
  }

  void setNum(TokenID num) {
    m_num = num;
  }

  void set(TokenID num, const char* t) {
    m_num = num;
    m_text = t;
  }

  void set(TokenID num, const std::string& t) {
    m_num = num;
    m_text = t;
  }

  void operator++(TokenID) {
    ++m_num;
  }

  ScannerToken& operator=(const ScannerToken& other) {
    m_num = other.m_num;
    m_text = other.m_text;
    return *this;
  }

  const std::string& text() const {
    return m_text;
  }

  bool same(const char* s) const {
    return strcasecmp(m_text.c_str(), s) == 0;
  }

  void setText(const char* t, int len) {
    m_text = std::string(t, len);
  }

  void setText(const char* t) {
    m_text = t;
  }

  void setText(const std::string& t) {
    m_text = t;
  }

  void setText(const ScannerToken& token) {
    m_text = token.m_text;
  }

  bool check() const {
    return m_check;
  }

  void setCheck() {
    m_check = true;
  }

  void xhpLabel(bool prefix = true);
  bool htmlTrim(); // true if non-empty after trimming
  void xhpDecode();  // xhp supports more entities than html

protected:
  /* Internal token id. */
  TokenID m_num{0};
  std::string m_text;
  bool m_check{false};
};

struct LookaheadToken {
  ScannerToken token;
  Location loc;
  int t;
};

struct LookaheadSlab {
  static const int SlabSize = 32;
  LookaheadToken m_data[SlabSize];
  int m_beginPos;
  int m_endPos;
  LookaheadSlab* m_next;
};

struct TokenStore {
  LookaheadSlab* m_head;
  LookaheadSlab* m_tail;
  TokenStore() {
    m_head = nullptr;
    m_tail = nullptr;
  }
  ~TokenStore() {
    LookaheadSlab* s = m_head;
    LookaheadSlab* next;
    while (s) {
      next = s->m_next;
      delete s;
      s = next;
    }
  }
  bool empty() {
    return !m_head || (m_head->m_beginPos == m_head->m_endPos);
  }
  struct iterator {
    LookaheadSlab* m_slab;
    int m_pos;
    const LookaheadToken& operator*() const {
      return m_slab->m_data[m_pos];
    }
    LookaheadToken& operator*() {
      return m_slab->m_data[m_pos];
    }
    const LookaheadToken* operator->() const {
      return m_slab->m_data + m_pos;
    }
    LookaheadToken* operator->() {
      return m_slab->m_data + m_pos;
    }
    void next() {
      if (!m_slab) return;
      ++m_pos;
      if (m_pos < m_slab->m_endPos) return;
      m_slab = m_slab->m_next;
      if (!m_slab) return;
      m_pos = m_slab->m_beginPos;
      return;
    }
    iterator& operator++() {
      next();
      return *this;
    }
    iterator operator++(int) {
      iterator it = *this;
      next();
      return it;
    }
    bool operator==(const iterator& it) const {
      if (m_slab != it.m_slab) return false;
      if (!m_slab) return true;
      return (m_pos == it.m_pos);
    }
  };
  iterator begin();
  iterator end();
  void popFront();
  iterator appendNew();
};

///////////////////////////////////////////////////////////////////////////////

struct Scanner {
  enum Type {
    AllowShortTags       = 0x01, // allow <?
    AllowAspTags         = 0x02, // allow <% %>
    ReturnAllTokens      = 0x04, // return comments and whitespaces
    AllowXHPSyntax       = 0x08, // allow XHP syntax
    AllowHipHopSyntax    = 0x18, // allow HipHop-specific syntax (which
                                 // includes XHP syntax)
  };

public:
  Scanner(const std::string& filename, int type, bool md5 = false);
  Scanner(std::istream &stream, int type, const char *fileName = "",
          bool md5 = false);
  Scanner(const char *source, int len, int type, const char *fileName = "",
          bool md5 = false);
  ~Scanner();

  const std::string &getMd5() const {
    return m_md5;
  }

  int scanToken(ScannerToken &t, Location &l);
  int fetchToken(ScannerToken &t, Location &l);
  void nextLookahead(TokenStore::iterator& pos);
  bool tryParseNSType(TokenStore::iterator& pos);
  bool tryParseTypeList(TokenStore::iterator& pos);
  bool tryParseFuncTypeList(TokenStore::iterator& pos);
  bool tryParseNonEmptyLambdaParams(TokenStore::iterator& pos);
  void parseApproxParamDefVal(TokenStore::iterator& pos);

  /**
   * Called by parser or tokenizer.
   */
  int getNextToken(ScannerToken &t, Location &l);
  const std::string &getError() const { return m_error;}
  Location *getLocation() const { return m_loc;}

  /**
   * Implemented in hphp.x, as they need to call yy functions.
   */
  void init();
  void reset();
  int scan();

  /**
   * Called by lex.yy.cpp for YY_INPUT (see hphp.x)
   */
  int read(char *text, yy_size_t &result, yy_size_t max);
  // Overload for older versions of flex.
  int read(char *text, int &result, yy_size_t max);

  /**
   * Called by scanner rules.
   */
  bool shortTags() const { return (m_type & AllowShortTags) == AllowShortTags;}
  bool aspTags() const { return (m_type & AllowAspTags) == AllowAspTags;}
  bool full() const { return (m_type & ReturnAllTokens) == ReturnAllTokens;}
  int lastToken() const { return m_lastToken;}
  void setToken(const char *rawText, int rawLeng, int type = -1) {
    m_token->setText(rawText, rawLeng);
    incLoc(rawText, rawLeng, type);
  }
  void stepPos(const char *rawText, int rawLeng, int type = -1) {
    if (shortTags()) {
      m_token->setText(rawText, rawLeng);
    }
    incLoc(rawText, rawLeng, type);
  }
  void setToken(const char *rawText, int rawLeng,
                const char *ytext, int yleng, int type = -1) {
    if (full()) {
      m_token->setText(rawText, rawLeng);
    } else {
      m_token->setText(ytext, yleng);
    }
    incLoc(rawText, rawLeng, type);
  }
  // also used for YY_FATAL_ERROR in hphp.x
  void error(ATTRIBUTE_PRINTF_STRING const char* fmt, ...)
    ATTRIBUTE_PRINTF(2,3);
  void warn(ATTRIBUTE_PRINTF_STRING const char* fmt, ...)
    ATTRIBUTE_PRINTF(2,3);
  std::string escape(const char *str, int len, char quote_type) const;

  /**
   * Called by scanner rules for doc comments.
   */
  void setDocComment(const char *ytext, int yleng) {
    m_docComment.assign(ytext, yleng);
  }
  void setDocComment(const std::string& com) {
    m_docComment = com;
  }
  std::string detachDocComment() {
    std::string dc = m_docComment;
    m_docComment.clear();
    return dc;
  }

  /**
   * Called by scanner rules for HEREDOC/NOWDOC.
   */
  void setHeredocLabel(const char *label, int len) {
    m_heredocLabel.assign(label, len);
  }
  int getHeredocLabelLen() const {
    return m_heredocLabel.length();
  }
  const char *getHeredocLabel() const {
    return m_heredocLabel.data();
  }
  void resetHeredoc() {
    m_heredocLabel.clear();
  }

  /**
   * Enables HipHop syntax for this file.
   */
  void setHHFile() {
    m_isHHFile = 1;
  }

  bool isHHFile() const {
    return m_isHHFile;
  }

  bool isXHPSyntaxEnabled() const {
    return ((m_type & AllowXHPSyntax) == AllowXHPSyntax) || m_isHHFile;
  }

  bool isHHSyntaxEnabled() const {
    return ((m_type & AllowHipHopSyntax) == AllowHipHopSyntax) || m_isHHFile;
  }

  int getLookaheadLtDepth() {
    return m_lookaheadLtDepth;
  }

private:
  bool tryParseShapeType(TokenStore::iterator& pos);
  bool tryParseShapeMemberList(TokenStore::iterator& pos);

  bool nextIfToken(TokenStore::iterator& pos, int tok);

  void computeMd5();

  std::string m_filename;
  bool m_streamOwner;
  std::istream *m_stream;
  std::stringstream m_sstream; // XHP helper
  const char *m_source;
  int m_len;
  int m_pos;
  std::string m_md5;

  enum State {
    Start = -1,
    NoLineFeed,
    HadLineFeed,
  };
  State m_state;

  int m_type;
  void *m_yyscanner;

  // These fields are used to temporarily hold pointers to token/location
  // storage while the lexer is active to facilitate functions such as
  // setToken() and incLoc()
  ScannerToken *m_token;
  Location *m_loc;

  std::string m_error;
  std::string m_docComment;
  std::string m_heredocLabel;

  // fields for XHP parsing
  int m_lastToken;
  void incLoc(const char *rawText, int rawLeng, int type);
  bool m_isHHFile;

  TokenStore m_lookahead;
  int m_lookaheadLtDepth;
};

///////////////////////////////////////////////////////////////////////////////
}

#endif // incl_HPHP_PARSER_SCANNER_H_
