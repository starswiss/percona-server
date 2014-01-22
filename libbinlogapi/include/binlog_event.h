/*
Copyright (c) 2003, 2011, 2013, Oracle and/or its affiliates. All rights
reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of
the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
02110-1301  USA
*/

/**
  @addtogroup Replication
  @{

  @file binlog_event.h

  @brief Contains the classes representing events occuring in the replication
  stream. Each event is represented as a byte sequence with logical divisions
  as event header, event specific data and event footer. The header and footer
  are common to all the events and are represented as two different subclasses.
*/

#ifndef BINLOG_EVENT_INCLUDED
#define	BINLOG_EVENT_INCLUDED

#include "debug_vars.h"
/*
 The header contains functions macros for reading and storing in
 machine independent format (low byte first).
*/
#include "byteorder.h"
#include "wrapper_functions.h"
#include "cassert"
#include <zlib.h> //for checksum calculations
#include <stdint.h>
#ifdef min //definition of min() and max() in std and libmysqlclient
           //can be/are different
#undef min
#endif
#ifdef max
#undef max
#endif
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <vector>


/*
  The symbols below are a part of the common definitions between
  the MySQL server and the client. Since they should not be a part of
  this library but the server, these should be placed in a header file
  common to the library and the MySQL server code, so that if they are
  updated in the server code, it is reflected in the libbinlogevent also.

  TODO: Collect all the variables here and create a common header file,
  placing it in libbinlogevent/include.
*/
#ifndef SYSTEM_CHARSET_MBMAXLEN
#define SYSTEM_CHARSET_MBMAXLEN 3
#endif
#ifndef NAME_CHAR_LEN
#define NAME_CHAR_LEN   64                     /* Field/table name length */
#endif
#ifndef NAME_LEN
#define NAME_LEN (NAME_CHAR_LEN*SYSTEM_CHARSET_MBMAXLEN)
#endif
/* Length of the server_version_split array in FDE class */
#ifndef ST_SERVER_VER_SPLIT_LEN
#define ST_SERVER_VER_SPLIT_LEN 3
#endif

/**
   binlog_version 3 is MySQL 4.x; 4 is MySQL 5.0.0.
   Compared to version 3, version 4 has:
   - a different Start_event, which includes info about the binary log
   (sizes of headers); this info is included for better compatibility if the
   master's MySQL version is different from the slave's.
   - all events have a unique ID (the triplet (server_id, timestamp at server
   start, other) to be sure an event is not executed more than once in a
   multimaster setup, example:
   @verbatim
                M1
              /   \
             v     v
             M2    M3
             \     /
              v   v
                S
   @endverbatim
   if a query is run on M1, it will arrive twice on S, so we need that S
   remembers the last unique ID it has processed, to compare and know if the
   event should be skipped or not. Example of ID: we already have the server id
   (4 bytes), plus:
   timestamp_when_the_master_started (4 bytes), a counter (a sequence number
   which increments every time we write an event to the binlog) (3 bytes).
   Q: how do we handle when the counter is overflowed and restarts from 0 ?

   - Query and Load (Create or Execute) events may have a more precise
     timestamp (with microseconds), number of matched/affected/warnings rows
   and fields of session variables: SQL_MODE,
   FOREIGN_KEY_CHECKS, UNIQUE_CHECKS, SQL_AUTO_IS_NULL, the collations and
   charsets, the PASSWORD() version (old/new/...).
*/
#define BINLOG_VERSION    4

/**
  Check if jump value is within buffer limits.

  @param jump         Number of positions we want to advance.
  @param buf_start    Pointer to buffer start.
  @param buf_current  Pointer to the current position on buffer.
  @param buf_len      Buffer length.

  @return             Number of bytes available on event buffer.
*/
template <class T> T available_buffer(const char* buf_start,
                                      const char* buf_current,
                                      T buf_len)
{
  return buf_len - (buf_current - buf_start);
}


/**
  Check if jump value is within buffer limits.

  @param jump         Number of positions we want to advance.
  @param buf_start    Pointer to buffer start
  @param buf_current  Pointer to the current position on buffer.
  @param buf_len      Buffer length.

  @retval      True   If jump value is within buffer limits.
  @retval      False  Otherwise.
*/
template <class T> bool valid_buffer_range(T jump,
                                           const char* buf_start,
                                           const char* buf_current,
                                           T buf_len)
{
  return (jump <= available_buffer(buf_start, buf_current, buf_len));
}


/**
  Enumeration of group types formed while transactions.
  The structure of a group is as follows:
  @verbatim
  Group {
        SID (16 byte UUID):         The source identifier for the group
        GNO (8 byte unsigned int):  The group number for the group
        COMMIT_FLAG (boolean):      True if this is the last group of the
                                    transaction
        LGID (8 byte unsigned int): Local group identifier: this is 1 for the
                                    first group in the binary log, 2 for the
                                    next one, etc. This is like an
                                    auto_increment primary key on the binary log.
        }
  @endverbatim
*/
enum enum_group_type
{
  AUTOMATIC_GROUP= 0,
  GTID_GROUP,
  ANONYMOUS_GROUP,
  INVALID_GROUP,
  UNDEFINED_GROUP
};

/**
  G_COMMIT_TS status variable stores the logical timestamp when the transaction
  entered the commit phase. This wll be used to apply transactions in parallel
  on the slave.
 */
#define G_COMMIT_TS  1

/*
  Constants used by Query_event.
*/

/**
   The maximum number of updated databases that a status of
   Query-log-event can carry.  It can be redefined within a range
   [1.. OVER_MAX_DBS_IN_EVENT_MTS].
*/
#define MAX_DBS_IN_EVENT_MTS 16

/**
   When the actual number of databases exceeds MAX_DBS_IN_EVENT_MTS
   the value of OVER_MAX_DBS_IN_EVENT_MTS is is put into the
   mts_accessed_dbs status.
*/
#define OVER_MAX_DBS_IN_EVENT_MTS 254

/**
  size of prepare and commit sequence numbers in the status vars in bytes
*/
#define COMMIT_SEQ_LEN  8


/**
  Max number of possible extra bytes in a replication event compared to a
  packet (i.e. a query) sent from client to master;
  First, an auxiliary log_event status vars estimation:
*/
#define MAX_SIZE_LOG_EVENT_STATUS (1U + 4          /* type, flags2 */   + \
                                   1U + 8          /* type, sql_mode */ + \
                                   1U + 1 + 255    /* type, length, catalog */ + \
                                   1U + 4          /* type, auto_increment */ + \
                                   1U + 6          /* type, charset */ + \
                                   1U + 1 + 255    /* type, length, time_zone */ + \
                                   1U + 2          /* type, lc_time_names_number */ + \
                                   1U + 2          /* type, charset_database_number */ + \
                                   1U + 8          /* type, table_map_for_update */ + \
                                   1U + 4          /* type, master_data_written */ + \
                                                   /* type, db_1, db_2, ... */  \
                                   1U + (MAX_DBS_IN_EVENT_MTS * (1 + NAME_LEN)) + \
                                   3U +            /* type, microseconds */ + \
                                   1U + 16 + 1 + 60/* type, user_len, user, host_len, host */)


#define SEQ_UNINIT -1

/**
  @namespace binary_log

  The namespace contains classes representing events that can occur in a
  replication stream.
*/
namespace binary_log
{
/*
  Reads string from buf.

  Reads str from buf in the following format:
   1. Read length stored on buf first index, as it only has 1 byte values
      bigger than 255 where lost.
   2. Set str pointer to buf second index.
  Despite str contains the complete stored string, when it is read until
  len its value will be truncated if original length was bigger than 255.

  @param buf source pointer
  @param buf_end
  @param str destination pointer
  @param len length to which the buffer should be read

  @retval 1 error
  @retval 0 success
*/
static inline int read_str_at_most_255_bytes(const char **buf,
                                             const char *buf_end,
                                             const char **str,
                                             uint8_t *len)
{
  if (*buf + ((unsigned int) (unsigned char) **buf) >= buf_end)
    return 1;
  *len= (unsigned char) **buf;
  *str= (*buf) + 1;
  (*buf)+= (unsigned int)*len + 1;
  return 0;
}

/**
   This flag only makes sense for Format_description_event. It is set
   when the event is written, and *reset* when a binlog file is
   closed (yes, it's the only case when MySQL modifies an already written
   part of the binlog).  Thus it is a reliable indicator that the binlog was
   closed correctly.  (Stop_event is not enough, there's always a
   small chance that mysqld crashes in the middle of insert and end of
   the binlog would look like a Stop_event).

   This flag is used to detect a restart after a crash, and to provide
   "unbreakable" binlog. The problem is that on a crash storage engines
   rollback automatically, while binlog does not.  To solve this we use this
   flag and automatically append ROLLBACK to every non-closed binlog (append
   virtually, on reading, file itself is not changed). If this flag is found,
   mysqlbinlog simply prints "ROLLBACK". Replication master does not abort on
   binlog corruption, but takes it as EOF, and replication slave forces a
   rollback in this case.

   Note, that old binlogs does not have this flag set, so we get a
   a backward-compatible behaviour.
*/
#define LOG_EVENT_BINLOG_IN_USE_F       0x1

/**
  Enumeration type for the different types of log events.
*/
enum Log_event_type
{
  /**
    Every time you update this enum (when you add a type), you have to
    fix Format_description_event::Format_description_event().
  */
  UNKNOWN_EVENT= 0,
  START_EVENT_V3= 1,
  QUERY_EVENT= 2,
  STOP_EVENT= 3,
  ROTATE_EVENT= 4,
  INTVAR_EVENT= 5,
  LOAD_EVENT= 6,
  SLAVE_EVENT= 7,
  CREATE_FILE_EVENT= 8,
  APPEND_BLOCK_EVENT= 9,
  EXEC_LOAD_EVENT= 10,
  DELETE_FILE_EVENT= 11,
  /**
    NEW_LOAD_EVENT is like LOAD_EVENT except that it has a longer
    sql_ex, allowing multibyte TERMINATED BY etc; both types share the
    same class (Load_event)
  */
  NEW_LOAD_EVENT= 12,
  RAND_EVENT= 13,
  USER_VAR_EVENT= 14,
  FORMAT_DESCRIPTION_EVENT= 15,
  XID_EVENT= 16,
  BEGIN_LOAD_QUERY_EVENT= 17,
  EXECUTE_LOAD_QUERY_EVENT= 18,

  TABLE_MAP_EVENT = 19,

  /**
    The PRE_GA event numbers were used for 5.1.0 to 5.1.15 and are
    therefore obsolete.
   */
  PRE_GA_WRITE_ROWS_EVENT = 20,
  PRE_GA_UPDATE_ROWS_EVENT = 21,
  PRE_GA_DELETE_ROWS_EVENT = 22,

  /**
    The V1 event numbers are used from 5.1.16 until mysql-trunk-xx
  */
  WRITE_ROWS_EVENT_V1 = 23,
  UPDATE_ROWS_EVENT_V1 = 24,
  DELETE_ROWS_EVENT_V1 = 25,

  /**
    Something out of the ordinary happened on the master
   */
  INCIDENT_EVENT= 26,

  /**
    Heartbeat event to be send by master at its idle time
    to ensure master's online status to slave
  */
  HEARTBEAT_LOG_EVENT= 27,

  /**
    In some situations, it is necessary to send over ignorable
    data to the slave: data that a slave can handle in case there
    is code for handling it, but which can be ignored if it is not
    recognized.
  */
  IGNORABLE_LOG_EVENT= 28,
  ROWS_QUERY_LOG_EVENT= 29,

  /** Version 2 of the Row events */
  WRITE_ROWS_EVENT = 30,
  UPDATE_ROWS_EVENT = 31,
  DELETE_ROWS_EVENT = 32,

  GTID_LOG_EVENT= 33,
  ANONYMOUS_GTID_LOG_EVENT= 34,

  PREVIOUS_GTIDS_LOG_EVENT= 35,

  /**
    A user defined event
  */
  USER_DEFINED_EVENT= 36,
  /**
    Add new events here - right above this comment!
    Existing events (except ENUM_END_EVENT) should never change their numbers
  */
  ENUM_END_EVENT /* end marker */
};

/**
 The length of the array server_version, which is used to store the version
 of MySQL server.
 We could have used SERVER_VERSION_LENGTH, but this introduces an
 obscure dependency - if somebody decided to change SERVER_VERSION_LENGTH
 this would break the replication protocol
 both of these are used to initialize the array server_version
 SERVER_VERSION_LENGTH is used for global array server_version
 and ST_SERVER_VER_LEN for the Start_event_v3 member server_version
*/

#define ST_SERVER_VER_LEN 50

/*
   Event header offsets;
   these point to places inside the fixed header.
*/
#define EVENT_TYPE_OFFSET    4
#define SERVER_ID_OFFSET     5
#define EVENT_LEN_OFFSET     9
#define LOG_POS_OFFSET       13
#define FLAGS_OFFSET         17

/** start event post-header (for v3 and v4) */
#define ST_BINLOG_VER_OFFSET  0
#define ST_SERVER_VER_OFFSET  2
#define ST_CREATED_OFFSET     (ST_SERVER_VER_OFFSET + ST_SERVER_VER_LEN)
#define ST_COMMON_HEADER_LEN_OFFSET (ST_CREATED_OFFSET + 4)

#define LOG_EVENT_HEADER_LEN 19U    /* the fixed header length */
#define OLD_HEADER_LEN       13U    /* the fixed header length in 3.23 */

/**
   Fixed header length, where 4.x and 5.0 agree. That is, 5.0 may have a longer
   header (it will for sure when we have the unique event's ID), but at least
   the first 19 bytes are the same in 4.x and 5.0. So when we have the unique
   event's ID, LOG_EVENT_HEADER_LEN will be something like 26, but
   LOG_EVENT_MINIMAL_HEADER_LEN will remain 19.
*/
#define LOG_EVENT_MINIMAL_HEADER_LEN 19U


/**
  Enumeration spcifying checksum algorithm used to encode a binary log event
*/
enum enum_binlog_checksum_alg
{
  /**
    Events are without checksum though its generator is checksum-capable
    New Master (NM).
  */
  BINLOG_CHECKSUM_ALG_OFF= 0,
  /** CRC32 of zlib algorithm */
  BINLOG_CHECKSUM_ALG_CRC32= 1,
  /** the cut line: valid alg range is [1, 0x7f] */
  BINLOG_CHECKSUM_ALG_ENUM_END,
  /**
    Special value to tag undetermined yet checksum or events from
    checksum-unaware servers
  */
  BINLOG_CHECKSUM_ALG_UNDEF= 255
};

#define CHECKSUM_CRC32_SIGNATURE_LEN 4

/**
   defined statically while there is just one alg implemented
*/
#define BINLOG_CHECKSUM_LEN CHECKSUM_CRC32_SIGNATURE_LEN
#define BINLOG_CHECKSUM_ALG_DESC_LEN 1  /* 1 byte checksum alg descriptor */

/**
  Convenience function to get the string representation of a binlog event.
*/
const char* get_event_type_str(Log_event_type type);


/**
  Calculate a long checksum for a memoryblock.

  @param crc       start value for crc
  @param pos       pointer to memory block
  @param length    length of the block

  @return checksum for a memory block
*/
inline uint32_t checksum_crc32(uint32_t crc, const unsigned char *pos, size_t length)
{
  return (uint32_t)crc32(crc, pos, length);
}

/**
  This method copies the string pointed to by src (including
  the terminating null byte ('\0')) to the array pointed to by dest.
  The strings may not overlap, and the destination string dest must be
  large enough to receive the copy.

  @param src  the source string
  @param dest the destination string

  @return     pointer to the end of the string dest
*/
char *bapi_stpcpy(char *dst, const char *src);


/**
  bapi_strmake(dest,src,length) moves length characters, or until end, of src to
  dest and appends a closing NUL to dest.
  Note that if strlen(src) >= length then dest[length] will be set to \0
  bapi_strmake() returns pointer to closing null
  @param dst    the destintion string
  @param src    the source string
  @param length length to copy from source to destination
*/
char *bapi_strmake(char *dst, const char *src, size_t length);

#define LOG_EVENT_HEADER_SIZE 20

/*
  Forward declaration of Format_description_event class to be used in class
  Log_event_header
*/
class Format_description_event;

/**
  @class Log_event_footer

  The footer, in the current version of the MySQL server, only contains
  the checksum algorithm descriptor. The descriptor is contained in the
  FDE of the binary log. This is common for all the events contained in
  that binary log, and defines the algorithm used to checksum
  the events contained in the binary log.

  @anchor Table_common_footer
  The footer contains the following:
  <table>
  <caption>Common-Footer</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>checkusm_alg</td>
    <td>enum_checksum_alg</td>
    <td>Algorithm used to checksum the events contained in the binary log</td>
  </table>

  @note checksum *value* is not stored in the event. On master's side, it
       is calculated before writing into the binary log, depending on the
       updated event data. On the slave, the checksum value is retrieved
       from a particular offset and checked for corruption, by computing
       a new value. It is not required after that. Therefore, it is not
       required to store the value in the instance as a class member.
*/
class Log_event_footer
{
public:

  enum_binlog_checksum_alg
  static get_checksum_alg(const char* buf, unsigned long len);

  static bool event_checksum_test(unsigned char* buf,
                                  unsigned long event_len,
                                  enum_binlog_checksum_alg alg);

  /* Constructors */
  Log_event_footer()
  : checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF)
  {
  }

  Log_event_footer(enum_binlog_checksum_alg checksum_alg_arg)
  : checksum_alg(checksum_alg_arg)
  {
  }

  /**
     @verbatim
     Master side:
     The value is set by caller of FD(Format Description) constructor
     In the FD case it's propagated into the last byte
     of post_header_len[].
     Slave side:
     On the slave side the value is assigned from post_header_len[last]
     of the last seen FD event.
     @endverbatim
     TODO: Revisit this comment when encoder is moved in libbinlogevent
  */
  enum_binlog_checksum_alg checksum_alg;
};

/**
  @class Log_event_header

  The Common-Header always has the same form and length within one
  version of MySQL.  Each event type specifies a format and length
  of the Post-Header.  The length of the Common-Header is the same
  for all events of the same type.

  @anchor Table_common_header
    The binary format of Common-Header is as follows:
  <table>
  <caption>Common-Header</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>when</td>
    <td>4 byte unsigned integer, represented by type struct timeval</td>
    <td>The time when the query started, in seconds since 1970.
    </td>
  </tr>

  <tr>
    <td>type_code</td>
    <td>1 byte enumeration</td>
    <td>See enum #Log_event_type.</td>
  </tr>

  <tr>
    <td>unmasked_server_id</td>
    <td>4 byte unsigned integer</td>
    <td>Server ID of the server that created the event.</td>
  </tr>

  <tr>
    <td>data_written</td>
    <td>4 byte unsigned integer</td>
    <td>The total size of this event, in bytes.  In other words, this
    is the sum of the sizes of Common-Header, Post-Header, and Body.
    </td>
  </tr>

  <tr>
    <td>log_pos</td>
    <td>4 byte unsigned integer</td>
    <td>The position of the next event in the master binary log, in
    bytes from the beginning of the file.  In a binlog that is not a
    relay log, this is just the position of the next event, in bytes
    from the beginning of the file.  In a relay log, this is
    the position of the next event in the master's binlog.
    </td>
  </tr>

  <tr>
    <td>flags</td>
    <td>2 byte bitfield</td>
    <td>16 or less flags depending on the version of the binary log.</td>
  </tr>
  </table>

  Summing up the numbers above, we see that the total size of the
  common header is 19 bytes.
*/
class Log_event_header
{
public:
  /*
    Timestamp on the master(for debugging and replication of
    NOW()/TIMESTAMP).  It is important for queries and LOAD DATA
    INFILE. This is set at the event's creation time, except for Query
    and Load (and other events) events where this is set at the query's
    execution time, which guarantees good replication (otherwise, we
    could have a query and its event with different timestamps).
  */
  struct timeval when;

  /**
    Event type extracted from the header. In the server, it is decoded
    by read_log_event(), but adding here for complete decoding.
  */
  Log_event_type  type_code;

  /*
    The server id read from the Binlog.
  */
  unsigned int unmasked_server_id;

  /* Length of an event, which will be written by write() function */
  unsigned long data_written;

  /*
    The offset in the log where this event originally appeared (it is
    preserved in relay logs, making SHOW SLAVE STATUS able to print
    coordinates of the event in the master's binlog). Note: when a
    transaction is written by the master to its binlog (wrapped in
    BEGIN/COMMIT) the log_pos of all the queries it contains is the
    one of the BEGIN (this way, when one does SHOW SLAVE STATUS it
    sees the offset of the BEGIN, which is logical as rollback may
    occur), except the COMMIT query which has its real offset.
  */
  unsigned long long log_pos;

  /*
    16 or less flags depending on the version of the binary log.
    See the definitions above for LOG_EVENT_TIME_F,
    LOG_EVENT_FORCED_ROTATE_F, LOG_EVENT_THREAD_SPECIFIC_F, and
    LOG_EVENT_SUPPRESS_USE_F for notes.
  */
  uint16_t flags;

  /**
    The following type definition is to be used whenever data is placed
    and manipulated in a common buffer. Use this typedef for buffers
    that contain data containing binary and character data.
  */
  typedef unsigned char Byte;

  Log_event_header()
  : log_pos(0), flags(0)
  {
    when.tv_sec= 0;
    when.tv_usec= 0;
  }
  Log_event_header(const char* buf,
                   const Format_description_event *description_event);

  ~Log_event_header() {}
};

/**
    This is the abstract base class for binary log events.

  @section Bianry_log_event_binary_format Binary Format

  @anchor Binary_log_event_format
  Any @c Binary_log_event saved on disk consists of the following four
  components.

  - Common-Header
  - Post-Header
  - Body
  - Footer

  Common header has the same format and length in a given MySQL version. It is
  documented @ref Table_common_header "here".

  The Body may be of different format and length even for different events of
  the same type. The binary formats of Post-Header and Body are documented
  separately in each subclass.

  Footer is common to all the events in a given MySQL version. It is documented
  @ref Table_common_footer "here".

  @anchor packed_integer
  - Some events, used for RBR use a special format for efficient representation
  of unsigned integers, called Packed Integer.  A Packed Integer has the
  capacity of storing up to 8-byte integers, while small integers
  still can use 1, 3, or 4 bytes.  The value of the first byte
  determines how to read the number, according to the following table:

  <table>
  <caption>Format of Packed Integer</caption>

  <tr>
    <th>First byte</th>
    <th>Format</th>
  </tr>

  <tr>
    <td>0-250</td>
    <td>The first byte is the number (in the range 0-250), and no more
    bytes are used.</td>
  </tr>

  <tr>
    <td>252</td>
    <td>Two more bytes are used.  The number is in the range
    251-0xffff.</td>
  </tr>

  <tr>
    <td>253</td>
    <td>Three more bytes are used.  The number is in the range
    0xffff-0xffffff.</td>
  </tr>

  <tr>
    <td>254</td>
    <td>Eight more bytes are used.  The number is in the range
    0xffffff-0xffffffffffffffff.</td>
  </tr>

  </table>

  - Strings are stored in various formats.  The format of each string
  is documented separately.

*/
class Binary_log_event
{
public:

  /*
     The number of types we handle in Format_description_event (UNKNOWN_EVENT
     is not to be handled, it does not exist in binlogs, it does not have a
     format).
  */
  static const int LOG_EVENT_TYPES= (ENUM_END_EVENT - 2);

  /**
    The lengths for the fixed data part of each event.
    This is an enum that provides post-header lengths for all events.
  */
  enum enum_post_header_length{
    // where 3.23, 4.x and 5.0 agree
    QUERY_HEADER_MINIMAL_LEN= (4 + 4 + 1 + 2),
    // where 5.0 differs: 2 for length of N-bytes vars.
    QUERY_HEADER_LEN=(QUERY_HEADER_MINIMAL_LEN + 2),
    STOP_HEADER_LEN= 0,
    LOAD_HEADER_LEN= (4 + 4 + 4 + 1 +1 + 4),
    START_V3_HEADER_LEN= (2 + ST_SERVER_VER_LEN + 4),
    // this is FROZEN (the Rotate post-header is frozen)
    ROTATE_HEADER_LEN= 8,
    INTVAR_HEADER_LEN= 0,
    CREATE_FILE_HEADER_LEN= 4,
    APPEND_BLOCK_HEADER_LEN= 4,
    EXEC_LOAD_HEADER_LEN= 4,
    DELETE_FILE_HEADER_LEN= 4,
    NEW_LOAD_HEADER_LEN= LOAD_HEADER_LEN,
    RAND_HEADER_LEN= 0,
    USER_VAR_HEADER_LEN= 0,
    FORMAT_DESCRIPTION_HEADER_LEN= (START_V3_HEADER_LEN + 1 + LOG_EVENT_TYPES),
    XID_HEADER_LEN= 0,
    BEGIN_LOAD_QUERY_HEADER_LEN= APPEND_BLOCK_HEADER_LEN,
    ROWS_HEADER_LEN_V1= 8,
    TABLE_MAP_HEADER_LEN= 8,
    EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN= (4 + 4 + 4 + 1),
    EXECUTE_LOAD_QUERY_HEADER_LEN= (QUERY_HEADER_LEN +\
                                    EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN),
    INCIDENT_HEADER_LEN= 2,
    HEARTBEAT_HEADER_LEN= 0,
    IGNORABLE_HEADER_LEN= 0,
    ROWS_HEADER_LEN_V2= 10
  }; // end enum_post_header_length

  Binary_log_event()
  {
  }

  Binary_log_event(const char **buf, uint16_t binlog_version,
                   const char *server_version);
  // TODO: Uncomment when the dependency of log_event on this class in removed
  /**
    Returns short information about the event
  */
  //virtual void print_event_info(std::ostream& info)=0;
  /**
    Returns detailed information about the event
  */
  //virtual void print_long_info(std::ostream& info);
  virtual ~Binary_log_event() = 0;

  /**
   * Helper method
   */
  enum Log_event_type get_event_type() const
  {
    return (enum Log_event_type) m_header.type_code;
  }

  virtual Log_event_type get_type_code()= 0;
  virtual bool is_valid() const= 0;
  /**
    Return a pointer to the header of the log event
  */
  Log_event_header *header() const
  {
    return const_cast<Log_event_header*>(&m_header);
  }
  /**
    Return a pointer to the footer of the log event
  */
  Log_event_footer *footer() const
  {
    return const_cast<Log_event_footer*>(&m_footer);
  }

private:
  Log_event_header m_header;
  Log_event_footer m_footer;
};


/**
  @class Query_event

  A @c Query_event is created for each query that modifies the
  database, unless the query is logged row-based.

  @section Query_event_binary_format Binary format

  //TODO: Add Documentation Binary format for the events
  See @ref Binary_log_event_binary_format "Binary format for log events" for
  a general discussion and introduction to the binary format of binlog
  events.

  The Post-Header has five components:

  <table>
  <caption>Post-Header for Query_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>thread_id</td>
    <td>4 byte unsigned integer</td>
    <td>The ID of the thread that issued this statement. It is needed for
        temporary tables.</td>
  </tr>

  <tr>
    <td>query_exec_time</td>
    <td>4 byte unsigned integer</td>
    <td>The time from when the query started to when it was logged in
    the binlog, in seconds.</td>
  </tr>

  <tr>
    <td>db_len</td>
    <td>1 byte integer</td>
    <td>The length of the name of the currently selected database.</td>
  </tr>

  <tr>
    <td>error_code</td>
    <td>2 byte unsigned integer</td>
    <td>Error code generated by the master. If the master fails, the
    slave will fail with the same error code.
    </td>
  </tr>

  <tr>
    <td>status_vars_len</td>
    <td>2 byte unsigned integer</td>
    <td>The length of the status_vars block of the Body, in bytes. This is not
        present for binlog version 1 and 3. See
    @ref Query_event_status_vars "below".
    </td>
  </tr>
  </table>

  The Body has the following components:

  <table>
  <caption>Body for Query_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>@anchor Query_event_status_vars status_vars</td>
    <td>status_vars_len bytes</td>
    <td>Zero or more status variables.  Each status variable consists
    of one byte identifying the variable stored, followed by the value
    of the variable.  The possible variables are listed separately in
    the table @ref Table_query_event_status_vars "below".  MySQL
    always writes events in the order defined below; however, it is
    capable of reading them in any order.  </td>
  </tr>

  <tr>
    <td>m_db</td>
    <td>db_len + 1</td>
    <td>The currently selected database, as a null-terminated string.

    (The trailing zero is redundant since the length is already known;
    it is db_len from Post-Header.)
    </td>
  </tr>

  <tr>
    <td>m_query</td>
    <td>variable length string without trailing zero, extending to the
    end of the event (determined by the length field of the
    Common-Header)
    </td>
    <td>The SQL query.</td>
  </tr>
  </table>

  The following table lists the status variables that may appear in
  the status_vars field.

  @anchor Table_query_event_status_vars
  <table>
  <caption>Status variables for Query_event</caption>

  <tr>
    <th>Status variable</th>
    <th>1 byte identifier</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>flags2</td>
    <td>Q_FLAGS2_CODE == 0</td>
    <td>4 byte bitfield</td>
    <td>The flags in @c thd->options, binary AND-ed with @c
    OPTIONS_WRITTEN_TO_BIN_LOG.  The @c thd->options bitfield contains
    options for "SELECT".  @c OPTIONS_WRITTEN identifies those options
    that need to be written to the binlog (not all do).  Specifically,
    @c OPTIONS_WRITTEN_TO_BIN_LOG equals (@c OPTION_AUTO_IS_NULL | @c
    OPTION_NO_FOREIGN_KEY_CHECKS | @c OPTION_RELAXED_UNIQUE_CHECKS |
    @c OPTION_NOT_AUTOCOMMIT), or 0x0c084000 in hex.

    These flags correspond to the SQL variables SQL_AUTO_IS_NULL,
    FOREIGN_KEY_CHECKS, UNIQUE_CHECKS, and AUTOCOMMIT, documented in
    the "SET Syntax" section of the MySQL Manual.

    This field is always written to the binlog in version >= 5.0, and
    never written in version < 5.0.
    </td>
  </tr>

  <tr>
    <td>sql_mode</td>
    <td>Q_SQL_MODE_CODE == 1</td>
    <td>8 byte bitfield</td>
    <td>The @c sql_mode variable.  See the section "SQL Modes" in the
    MySQL manual, and see sql_priv.h for a list of the possible
    flags. Currently (2007-10-04), the following flags are available:
    <pre>
    MODE_REAL_AS_FLOAT==0x1
    MODE_PIPES_AS_CONCAT==0x2
    MODE_ANSI_QUOTES==0x4
    MODE_IGNORE_SPACE==0x8
    MODE_NOT_USED==0x10
    MODE_ONLY_FULL_GROUP_BY==0x20
    MODE_NO_UNSIGNED_SUBTRACTION==0x40
    MODE_NO_DIR_IN_CREATE==0x80
    MODE_POSTGRESQL==0x100
    MODE_ORACLE==0x200
    MODE_MSSQL==0x400
    MODE_DB2==0x800
    MODE_MAXDB==0x1000
    MODE_NO_KEY_OPTIONS==0x2000
    MODE_NO_TABLE_OPTIONS==0x4000
    MODE_NO_FIELD_OPTIONS==0x8000
    MODE_MYSQL323==0x10000
    MODE_MYSQL323==0x20000
    MODE_MYSQL40==0x40000
    MODE_ANSI==0x80000
    MODE_NO_AUTO_VALUE_ON_ZERO==0x100000
    MODE_NO_BACKSLASH_ESCAPES==0x200000
    MODE_STRICT_TRANS_TABLES==0x400000
    MODE_STRICT_ALL_TABLES==0x800000
    MODE_NO_ZERO_IN_DATE==0x1000000
    MODE_NO_ZERO_DATE==0x2000000
    MODE_INVALID_DATES==0x4000000
    MODE_ERROR_FOR_DIVISION_BY_ZERO==0x8000000
    MODE_TRADITIONAL==0x10000000
    MODE_NO_AUTO_CREATE_USER==0x20000000
    MODE_HIGH_NOT_PRECEDENCE==0x40000000
    MODE_PAD_CHAR_TO_FULL_LENGTH==0x80000000
    </pre>
    All these flags are replicated from the server.  However, all
    flags except @c MODE_NO_DIR_IN_CREATE are honored by the slave;
    the slave always preserves its old value of @c
    MODE_NO_DIR_IN_CREATE.

    This field is always written to the binlog.
    </td>
  </tr>

  <tr>
    <td>catalog</td>
    <td>Q_CATALOG_NZ_CODE == 6</td>
    <td>Variable-length string: the length in bytes (1 byte) followed
    by the characters (at most 255 bytes)
    </td>
    <td>Stores the client's current catalog.  Every database belongs
    to a catalog, the same way that every table belongs to a
    database.  Currently, there is only one catalog, "std".

    This field is written if the length of the catalog is > 0;
    otherwise it is not written.
    </td>
  </tr>

  <tr>
    <td>auto_increment</td>
    <td>Q_AUTO_INCREMENT == 3</td>
    <td>two 2 byte unsigned integers, totally 2+2=4 bytes</td>

    <td>The two variables auto_increment_increment and
    auto_increment_offset, in that order.  For more information, see
    "System variables" in the MySQL manual.

    This field is written if auto_increment > 1.  Otherwise, it is not
    written.
    </td>
  </tr>

  <tr>
    <td>charset</td>
    <td>Q_CHARSET_CODE == 4</td>
    <td>three 2 byte unsigned integers, totally 2+2+2=6 bytes</td>
    <td>The three variables character_set_client,
    collation_connection, and collation_server, in that order.
    character_set_client is a code identifying the character set and
    collation used by the client to encode the query.
    collation_connection identifies the character set and collation
    that the master converts the query to when it receives it; this is
    useful when comparing literal strings.  collation_server is the
    default character set and collation used when a new database is
    created.

    See also "Connection Character Sets and Collations" in the MySQL
    5.1 manual.

    All three variables are codes identifying a (character set,
    collation) pair.  To see which codes map to which pairs, run the
    query "SELECT id, character_set_name, collation_name FROM
    COLLATIONS".

    Cf. Q_CHARSET_DATABASE_CODE below.

    This field is always written.
    </td>
  </tr>

  <tr>
    <td>time_zone</td>
    <td>Q_TIME_ZONE_CODE == 5</td>
    <td>Variable-length string: the length in bytes (1 byte) followed
    by the characters (at most 255 bytes).
    <td>The time_zone of the master.

    See also "System Variables" and "MySQL Server Time Zone Support"
    in the MySQL manual.

    This field is written if the length of the time zone string is >
    0; otherwise, it is not written.
    </td>
  </tr>

  <tr>
    <td>lc_time_names_number</td>
    <td>Q_LC_TIME_NAMES_CODE == 7</td>
    <td>2 byte integer</td>
    <td>A code identifying a table of month and day names.  The
    mapping from codes to languages is defined in @c sql_locale.cc.

    This field is written if it is not 0, i.e., if the locale is not
    en_US.
    </td>
  </tr>

  <tr>
    <td>charset_database_number</td>
    <td>Q_CHARSET_DATABASE_CODE == 8</td>
    <td>2 byte integer</td>

    <td>The value of the collation_database system variable (in the
    source code stored in @c thd->variables.collation_database), which
    holds the code for a (character set, collation) pair as described
    above (see Q_CHARSET_CODE).

    collation_database was used in old versions (???WHEN).  Its value
    was loaded when issuing a "use db" query and could be changed by
    issuing a "SET collation_database=xxx" query.  It used to affect
    the "LOAD DATA INFILE" and "CREATE TABLE" commands.

    In newer versions, "CREATE TABLE" has been changed to take the
    character set from the database of the created table, rather than
    the character set of the current database.  This makes a
    difference when creating a table in another database than the
    current one.  "LOAD DATA INFILE" has not yet changed to do this,
    but there are plans to eventually do it, and to make
    collation_database read-only.

    This field is written if it is not 0.
    </td>
  </tr>
  <tr>
    <td>table_map_for_update</td>
    <td>Q_TABLE_MAP_FOR_UPDATE_CODE == 9</td>
    <td>8 byte integer</td>

    <td>The value of the table map that is to be updated by the
    multi-table update query statement. Every bit of this variable
    represents a table, and is set to 1 if the corresponding table is
    to be updated by this statement.

    The value of this variable is set when executing a multi-table update
    statement and used by slave to apply filter rules without opening
    all the tables on slave. This is required because some tables may
    not exist on slave because of the filter rules.
    </td>
  </tr>
  <tr>
    <td>master_data_written</td>
    <td>Q_MASTER_DATA_WRITTEN_CODE == 10</td>
    <td>4 byte bitfield</td>

    <td>The value of the original length of a Query_event that comes from a
    master. Master's event is relay-logged with storing the original size of
    event in this field by the IO thread. The size is to be restored by reading
    Q_MASTER_DATA_WRITTEN_CODE-marked event from the relay log.

    This field is not written to slave's server binlog by the SQL thread.
    This field only exists in relay logs where master has binlog_version<4 i.e.
    server_version < 5.0 and the slave has binlog_version=4.
    </td>
  </tr>
  <tr>
    <td>binlog_invoker</td>
    <td>Q_INVOKER == 11</td>
    <td>2 Variable-length strings: the length in bytes (1 byte) followed
    by characters (user), again followed by length in bytes (1 byte) followed
    by characters(host)</td>

    <td>The value of boolean variable m_binlog_invoker is set TRUE if
    CURRENT_USER() is called in account management statements. SQL thread
    uses it as a default definer in CREATE/ALTER SP, SF, Event, TRIGGER or
    VIEW statements.

    The field Q_INVOKER has length of user stored in 1 byte followed by the
    user string which is assigned to 'user' and the length of host stored in
    1 byte followed by host string which is assigned to 'host'.
    </td>
  </tr>
  <tr>
    <td>mts_accessed_dbs</td>
    <td>Q_UPDATED_DB_NAMES == 12</td>
    <td>1 byte character, and a 2-D array</td>
    <td>The total number and the names to of the databases accessed is stored,
        to be propagated to the slave in order to facilitate the parallel
        applying of the Query events.
    </td>
  </tr>
  <tr>
    <td>commit_seq_no</td>
    <td>Q_COMMIT_TS</td>
    <td>8 byte integer</td>
    <td>Stores the logical timestamp when the transaction
        entered the commit phase. This wll be used to apply transactions
        in parallel on the slave.  </td>
  </tr>
  </table>

  @subsection Query_event_notes_on_previous_versions Notes on Previous Versions

  * Status vars were introduced in version 5.0.  To read earlier
  versions correctly, check the length of the Post-Header.

  * The status variable Q_CATALOG_CODE == 2 existed in MySQL 5.0.x,
  where 0<=x<=3.  It was identical to Q_CATALOG_CODE, except that the
  string had a trailing '\0'.  The '\0' was removed in 5.0.4 since it
  was redundant (the string length is stored before the string).  The
  Q_CATALOG_CODE will never be written by a new master, but can still
  be understood by a new slave.

  * See Q_CHARSET_DATABASE_CODE in the table above.

  * When adding new status vars, please don't forget to update the
  MAX_SIZE_LOG_EVENT_STATUS, and update function code_name

*/

class Query_event: public Binary_log_event
{
public:
  /** query event post-header */
  enum Query_event_post_header_offset{
    Q_THREAD_ID_OFFSET= 0,
    Q_EXEC_TIME_OFFSET= 4,
    Q_DB_LEN_OFFSET= 8,
    Q_ERR_CODE_OFFSET= 9,
    Q_STATUS_VARS_LEN_OFFSET= 11,
    Q_DATA_OFFSET= QUERY_HEADER_LEN
  };

  /* these are codes, not offsets; not more than 256 values (1 byte). */
  enum Query_event_status_vars
  {
    Q_FLAGS2_CODE= 0,
    Q_SQL_MODE_CODE,
    /*
      Q_CATALOG_CODE is catalog with end zero stored; it is used only by MySQL
      5.0.x where 0<=x<=3. We have to keep it to be able to replicate these
      old masters.
    */
    Q_CATALOG_CODE,
    Q_AUTO_INCREMENT,
    Q_CHARSET_CODE,
    Q_TIME_ZONE_CODE,
    /*
      Q_CATALOG_NZ_CODE is catalog withOUT end zero stored; it is used by MySQL
      5.0.x where x>=4. Saves one byte in every Query_event in binlog,
      compared to Q_CATALOG_CODE. The reason we didn't simply re-use
      Q_CATALOG_CODE is that then a 5.0.3 slave of this 5.0.x (x>=4)
      master would crash (segfault etc) because it would expect a 0 when there
      is none.
    */
    Q_CATALOG_NZ_CODE,
    Q_LC_TIME_NAMES_CODE,
    Q_CHARSET_DATABASE_CODE,
    Q_TABLE_MAP_FOR_UPDATE_CODE,
    Q_MASTER_DATA_WRITTEN_CODE,
    Q_INVOKER,
    /*
      Q_UPDATED_DB_NAMES status variable collects information of accessed
      databases i.e. the total number and the names to be propagated to the
      slave in order to facilitate the parallel applying of the Query events.
    */
    Q_UPDATED_DB_NAMES,
    Q_MICROSECONDS,
    /**
      Q_COMMIT_TS status variable stores the logical timestamp when the
      transaction entered the commit phase. This wll be used to apply
      transactions in parallel on the slave.
   */
   Q_COMMIT_TS
  };

private:
  std::string m_user;
  std::string m_host;
  std::string m_catalog;
  std::string m_time_zone_str;
  std::string m_db;
  std::string m_query;

protected:
  /* Required by the MySQL server class Log_event::Query_event */
  unsigned long data_len;
  /* Pointer to the end of the buffer shown below */
  unsigned long query_data_written;
  /*
    Copies data into the buffer in the following fashion
    +--------+-----------+------+------+---------+----+-------+----+
    | catlog | time_zone | user | host | db name | \0 | Query | \0 |
    +--------+-----------+------+------+---------+----+-------+----+
  */
  int fill_data_buf(unsigned char* dest);
  static char const *code_name(int code);

public:
  /* data members defined in order they are packed and written into the log */
  uint32_t thread_id;
  uint32_t query_exec_time;
  uint32_t db_len;
  uint16_t error_code;

  /*
    We want to be able to store a variable number of N-bit status vars:
    (generally N=32; but N=64 for SQL_MODE) a user may want to log the number
    of affected rows (for debugging) while another does not want to lose 4
    bytes in this.
    The storage on disk is the following:
    status_vars_len is part of the post-header,
    status_vars are in the variable-length part, after the post-header, before
    the db & query.
    status_vars on disk is a sequence of pairs (code, value) where 'code' means
    'sql_mode', 'affected' etc. Sometimes 'value' must be a short string, so
    its first byte is its length. For now the order of status vars is:
    flags2 - sql_mode - catalog - autoinc - charset
    We should add the same thing to Load_event, but in fact
    LOAD DATA INFILE is going to be logged with a new type of event (logging of
    the plain text query), so Load_event would be frozen, so no need. The
    new way of logging LOAD DATA INFILE would use a derived class of
    Query_event, so automatically benefit from the work already done for
    status variables in Query_event.
 */
  uint16_t status_vars_len;
  /*
    If we already know the length of the query string
    we pass it with q_len, so we would not have to call strlen()
    otherwise, set it to 0, in which case, we compute it with strlen()
  */
  uint32_t q_len;

  /* The members below represent the status variable block */

  /*
    'flags2' is a second set of flags (on top of those in Log_event), for
    session variables. These are thd->options which is & against a mask
    (OPTIONS_WRITTEN_TO_BIN_LOG).
    flags2_inited helps make a difference between flags2==0 (3.23 or 4.x
    master, we don't know flags2, so use the slave server's global options) and
    flags2==0 (5.0 master, we know this has a meaning of flags all down which
    must influence the query).
  */
  bool flags2_inited;
  bool sql_mode_inited;
  bool charset_inited;

  uint32_t flags2;
  /* In connections sql_mode is 32 bits now but will be 64 bits soon */
  uint64_t sql_mode;
  uint16_t auto_increment_increment, auto_increment_offset;
  char charset[6];
  unsigned int time_zone_len; /* 0 means uninited */
  /*
    Binlog format 3 and 4 start to differ (as far as class members are
    concerned) from here.
  */
  unsigned int catalog_len;                    // <= 255 char; 0 means uninited
  uint16_t lc_time_names_number; /* 0 means en_US */
  uint16_t charset_database_number;
  /*
    map for tables that will be updated for a multi-table update query
    statement, for other query statements, this will be zero.
  */
  uint64_t table_map_for_update;
  /*
    Holds the original length of a Query_event that comes from a
    master of version < 5.0 (i.e., binlog_version < 4). When the IO
    thread writes the relay log, it augments the Query_event with a
    Q_MASTER_DATA_WRITTEN_CODE status_var that holds the original event
    length. This field is initialized to non-zero in the SQL thread when
    it reads this augmented event. SQL thread does not write
    Q_MASTER_DATA_WRITTEN_CODE to the slave's server binlog.
  */
  uint32_t master_data_written;

  /*
    number of updated databases by the query and their names. This info
    is requested by both Coordinator and Worker.
  */
  unsigned char mts_accessed_dbs;
  char mts_accessed_db_names[MAX_DBS_IN_EVENT_MTS][NAME_LEN];

  /**
    Prepare and commit sequence number. will be set to 0 if the event is not a
    transaction starter.
  */
  int64_t commit_seq_no;

  Query_event(const char* query_arg, const char* catalog_arg,
              const char* db_arg, uint32_t query_length,
              unsigned long thread_id_arg,
              unsigned long long sql_mode_arg,
              unsigned long auto_increment_increment_arg,
              unsigned long auto_increment_offset_arg,
              unsigned int number,
              unsigned long long table_map_for_update_arg,
              int errcode,
              unsigned int db_arg_len, unsigned int catalog_arg_len);

  Query_event(const char* buf, unsigned int event_len,
              const Format_description_event *description_event,
              Log_event_type event_type);
  Query_event();
  virtual ~Query_event()
  {
  }

  Log_event_type get_type_code() { return QUERY_EVENT;}
  bool is_valid() const {  return !m_query.empty(); }


  /*
    Define getters and setters for the string members
  */
  void set_user(const std::string &s)
  {
    m_user= s;
  }
  std::string get_user() const
  {
    return m_user;
  }
  void set_host(const std::string &s)
  {
    m_host= s;
  }
  std::string get_host() const
  {
    return m_host;
  }
  void set_time_zone_str(const std::string &s)
  {
    m_time_zone_str= s;
    time_zone_len= m_time_zone_str.length();
  }
  std::string get_time_zone_str() const
  {
    return m_time_zone_str;
  }
  void set_catalog(const std::string &s)
  {
    m_catalog= s;
    catalog_len= m_catalog.length();
  }
  std::string get_catalog() const
  {
    return m_catalog;
  }
  void set_db(const std::string &s)
  {
    m_db= s;
    db_len= m_db.length();
  }
  std::string get_db() const
  {
    return m_db;
  }
  void set_query(const std::string &s)
  {
    m_query= s;
    q_len= m_query.length();
  }
  std::string get_query() const
  {
    return m_query;
  }


  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
};


/**
  @class Rotate_event

  When a binary log file exceeds a size limit, a ROTATE_EVENT is written
  at the end of the file that points to the next file in the squence.
  This event is information for the slave to know the name of the next
  binary log it is going to receive.

  ROTATE_EVENT is generated locally and written to the binary log
  on the master. It is written to the relay log on the slave when FLUSH LOGS
  occurs, and when receiving a ROTATE_EVENT from the master.
  In the latter case, there will be two rotate events in total originating
  on different servers.

  @section Rotate_event_binary_format Binary Format

  <table>
  <caption>Post-Header for Rotate_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>position</td>
    <td>8 byte integer</td>
    <td>The position within the binary log to rotate to.</td>
  </tr>

  </table>

  The Body has one component:

  <table>
  <caption>Body for Rotate_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>new_log_ident</td>
    <td>variable length string without trailing zero, extending to the
    end of the event (determined by the length field of the
    Common-Header)
    </td>
    <td>Name of the binlog to rotate to.</td>
  </tr>

  </table>
*/
class Rotate_event: public Binary_log_event
{
public:
  const char* new_log_ident;
  unsigned int ident_len;
  unsigned int flags;
  uint64_t pos;

  enum {
    /* Values taken by the flag member variable */
    DUP_NAME= 2, // if constructor should dup the string argument
    RELAY_LOG= 4 // rotate event for the relay log
  };

  enum {
    /* Rotate event post_header */
    R_POS_OFFSET= 0,
    R_IDENT_OFFSET= 8
  };

  Rotate_event() {}
  Rotate_event(const char* buf, unsigned int event_len,
               const Format_description_event *description_event);

  Log_event_type get_type_code() { return ROTATE_EVENT; }
  //TODO: is_valid() is to be handled as a separate patch
  bool is_valid() const { return new_log_ident != 0; }

  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);

  ~Rotate_event()
  {
    if (flags & DUP_NAME)
      bapi_free((void*) new_log_ident);
  }
};

/**
  @class Start_event_v3

  Start_event_v3 is the Start_event of binlog format 3 (MySQL 3.23 and
  4.x).

  @section Start_event_v3_binary_format Binary Format

  Format_description_event derives from Start_event_v3; it is
  the Start_event of binlog format 4 (MySQL 5.0), that is, the
  event that describes the other events' Common-Header/Post-Header
  lengths. This event is sent by MySQL 5.0 whenever it starts sending
  a new binlog if the requested position is >4 (otherwise if ==4 the
  event will be sent naturally).
  The Post-Header has four components:

  <table>
  <caption>Post-Header for Start_event_v3</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>created</td>
    <td>4 byte unsigned integer</td>
    <td>The creation timestamp, if non-zero,
        is the time in seconds when this event was created</td>
  </tr>
  <tr>
    <td>binlog_version</td>
    <td>2 byte unsigned integer</td>
    <td>This is 1 in MySQL 3.23 and 3 in MySQL 4.0 and 4.1
        (In MySQL 5.0 and up, FORMAT_DESCRIPTION_EVENT is
        used instead of START_EVENT_V3 and for them its 4).</td>
  </tr>
  <tr>
    <td>server_version</td>
    <td>char array of 50 bytes</td>
    <td>The MySQL server's version (example: 4.0.14-debug-log),
        padded with 0x00 bytes on the right</td>
  </tr>
  <tr>
    <td>dont_set_created</td>
    <td>type bool</td>
    <td>Set to 1 when you dont want to have created time in the log</td>
  </table>
*/

class Start_event_v3: public Binary_log_event
 {
 public:
/*
    If this event is at the start of the first binary log since server
    startup 'created' should be the timestamp when the event (and the
    binary log) was created.  In the other case (i.e. this event is at
    the start of a binary log created by FLUSH LOGS or automatic
    rotation), 'created' should be 0.  This "trick" is used by MySQL
    >=4.0.14 slaves to know whether they must drop stale temporary
    tables and whether they should abort unfinished transaction.

    Note that when 'created'!=0, it is always equal to the event's
    timestamp; indeed Start_event is written only in log.cc where
    the first constructor below is called, in which 'created' is set
    to 'when'.  So in fact 'created' is a useless variable. When it is
    0 we can read the actual value from timestamp ('when') and when it
    is non-zero we can read the same value from timestamp
    ('when'). Conclusion:
     - we use timestamp to print when the binlog was created.
     - we use 'created' only to know if this is a first binlog or not.
     In 3.23.57 we did not pay attention to this identity, so mysqlbinlog in
     3.23.57 does not print 'created the_date' if created was zero. This is now
     fixed.
  */
  time_t created;
  uint16_t binlog_version;
  char server_version[ST_SERVER_VER_LEN];
   /*
    We set this to 1 if we don't want to have the created time in the log,
    which is the case when we rollover to a new log.
  */
  bool dont_set_created;
  Log_event_type get_type_code() { return START_EVENT_V3;}
  protected:
  /**
     The constructor below will be  used only by Format_description_event
     constructor
  */
  Start_event_v3();
  public:
  Start_event_v3(const char* buf,
                 const Format_description_event* description_event);
  //TODO: Add definition for them
  void print_event_info(std::ostream& info) { }
  void print_long_info(std::ostream& info) { }
};

/**
  @class Format_description_event
  For binlog version 4.
  This event is saved by threads which read it, as they need it for future
  use (to decode the ordinary events).

  @section Format_description_event_binary_format Binary Format

  The Post-Header has six components:

  <table>
  <caption>Post-Header for Format_description_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>created_ts</td>
    <td>4 byte unsigned integer</td>
    <td>The creation timestamp, if non-zero,
        is the time in seconds when this event was created</td>
  </tr>

  <tr>
    <td>common_header_len</td>
    <td>1 byte unsigned integer</td>
    <td>The length of the event header. This value includes the extra_headers
        field, so this header length - 19 yields the size
        of the extra_headers field.</td>
  </tr>
  <tr>
    <td>post_header_len</td>
    <td>array of type 1 byte unsigned integer</td>
    <td>The lengths for the fixed data part of each event</td>
  </tr>
  <tr>
    <td>server_version_split</td>
    <td>unsigned char array</td>
    <td>Stores the server version of the server
        and splits them in three parts</td>
  </tr>
  <tr>
    <td>event_type_permutation</td>
    <td>const array of type 1 byte unsigned integer</td>
    <td>Provides mapping between the event types of
        some previous versions > 5.1 GA to current event_types</td>
  </tr>
    <tr>
    <td>number_of_event_types</td>
    <td>1 byte unsigned integer</td>
    <td>number of event types present in the server</td>
  </tr>
  </table>
*/
class Format_description_event: public virtual Start_event_v3
{
public:
    uint32_t created_ts;
    /**
     The size of the fixed header which _all_ events have
     (for binlogs written by this version, this is equal to
     LOG_EVENT_HEADER_LEN), except FORMAT_DESCRIPTION_EVENT and ROTATE_EVENT
     (those have a header of size LOG_EVENT_MINIMAL_HEADER_LEN).
    */
    uint8_t common_header_len;
    /*
     The list of post-headers' lengths followed
     by the checksum alg decription byte
  */
    uint8_t *post_header_len;
    unsigned char server_version_split[ST_SERVER_VER_SPLIT_LEN];
    /**
     In some previous version > 5.1 GA event types are assigned
     different event id numbers than in the present version, so we
     must map those id's to the our current event id's. This
     mapping is done using event_type_permutation
    */
    const uint8_t *event_type_permutation;
    Format_description_event(uint8_t binlog_ver,
                             const char* server_ver);
    Format_description_event(const char* buf, unsigned int event_len,
                             const Format_description_event *description_event);

    uint8_t number_of_event_types;
    Log_event_type get_type_code() { return FORMAT_DESCRIPTION_EVENT; }
    unsigned long get_product_version() const;
    bool is_version_before_checksum() const;
    void calc_server_version_split();
    bool is_valid() const { return 1; }
    void print_event_info(std::ostream& info);
    void print_long_info(std::ostream& info);
    ~Format_description_event();

};
/**
  @class Stop_event

  A stop event is written to the log files under these circumstances:
  - A master writes the event to the binary log when it shuts down.
  - A slave writes the event to the relay log when it shuts down or
    when a RESET SLAVE statement is executed.

  @section Stop_event_binary_format Binary Format

  The Post-Header and Body for this event type are empty; it only has
  the Common-Header.
*/

class Stop_event: public Binary_log_event
{
public:
  Stop_event() : Binary_log_event()
  {
  }
  Stop_event(const char* buf,
             const Format_description_event *description_event)
  : Binary_log_event(&buf, description_event->binlog_version,
                     description_event->server_version)
  {
  }

  Log_event_type get_type_code() { return STOP_EVENT; }
  bool is_valid() const { return 1; }

  void print_event_info(std::ostream& info) {};
  void print_long_info(std::ostream& info);
};


/**
  @class User_var_event

  Written every time a statement uses a user variable; precedes other
  events for the statement. Indicates the value to use for the user
  variable in the next statement. This is written only before a QUERY_EVENT
  and is not used with row-based logging

  The Post-Header has following components:

  <table>
  <caption>Post-Header for Format_description_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>Value_type</td>
    <td>enum</td>
    <td>The user variable type.</td>
  </tr>
  <tr>
    <td>User_var_event_data</td>
    <td>enum</td>
    <td>User_var event data</td>
  </tr>
  <tr>
    <td>name</td>
    <td>const char pointer</td>
    <td>User variable name.</td>
  </tr>
  <tr>
    <td>name_len</td>
    <td>unsigned int</td>
    <td>Length of the user variable name</td>
  </tr>
  <tr>
    <td>val</td>
    <td>char pointer</td>
    <td>value of the user variable.</td>
  </tr>
  <tr>
    <td>val_len</td>
    <td>unsigned long</td>
    <td>Length of the value of the user variable</td>
  </tr>
  <tr>
    <td>type</td>
    <td>enum Value_type</td>
    <td>Type of the user variable</td>
  </tr>
  <tr>
    <td>charset_number</td>
    <td>unsigned int</td>
    <td>The number of the character set for the user variable (needed for a
        string variable). The character set number is really a collation
        number that indicates a character set/collation pair.</td>
  </tr>
  <tr>
    <td>is_null</td>
    <td>bool</td>
    <td>Non-zero if the variable value is the SQL NULL value, 0 otherwise.</td>
  </tr>
  </table>
*/
class User_var_event: public Binary_log_event
{
public:
  enum Value_type {
    STRING_TYPE,
    REAL_TYPE,
    INT_TYPE,
    ROW_TYPE,
    DECIMAL_TYPE,
    VALUE_TYPE_COUNT
    };
  enum {
    UNDEF_F,
    UNSIGNED_F
  };
  enum User_var_event_data
  {
    UV_VAL_LEN_SIZE= 4,
    UV_VAL_IS_NULL= 1,
    UV_VAL_TYPE_SIZE= 1,
    UV_NAME_LEN_SIZE= 4,
    UV_CHARSET_NUMBER_SIZE= 4
  };
  User_var_event(const char *name_arg, unsigned int name_len_arg, char *val_arg,
                 unsigned long val_len_arg, Value_type type_arg,
                 unsigned int charset_number_arg, unsigned char flags_arg)
  {
    name= name_arg;
    name_len= name_len_arg;
    val= val_arg;
    val_len= val_len_arg;
    type=(Value_type) type_arg;
    charset_number= charset_number_arg;
    flags= flags_arg;
    is_null= !val;
  }

  User_var_event(const char* buf, unsigned int event_len,
                 const Format_description_event *description_event);
  const char *name;
  unsigned int name_len;
  char *val;
  uint32_t val_len;
  Value_type type;
  unsigned int charset_number;
  bool is_null;
  unsigned char flags;
  Log_event_type get_type_code() {return USER_VAR_EVENT; }
  bool is_valid() const { return 1; }
  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
  std::string static get_value_type_string(enum Value_type type)
  {
    switch(type)
    {
      case STRING_TYPE:return "String";
      case REAL_TYPE:return "Real";
      case INT_TYPE:return "Integer";
      case ROW_TYPE:return "Row";
      case DECIMAL_TYPE:return "Decimal";
      case VALUE_TYPE_COUNT:return "Value type count";
      default:return "Unknown";
    }
  }
};

/**
  @class Ignorable_event

  Base class for ignorable log events. Events deriving from
  this class can be safely ignored by slaves that cannot
  recognize them. Newer slaves, will be able to read and
  handle them. This has been designed to be an open-ended
  architecture, so adding new derived events shall not harm
  the old slaves that support ignorable log event mechanism
  (they will just ignore unrecognized ignorable events).

  @note The only thing that makes an event ignorable is that it has
  the LOG_EVENT_IGNORABLE_F flag set.  It is not strictly necessary
  that ignorable event types derive from Ignorable_event; they may
  just as well derive from Binary_log_event and Log_event and pass
  LOG_EVENT_IGNORABLE_F as argument to the Log_event constructor.

  @section Ignoarble_event_binary_format Binary format

  The Post-Header and Body for this event type are empty; it only has
  the Common-Header.
*/
class Ignorable_event: public Binary_log_event
{
public:
  Ignorable_event(const char *buf, const Format_description_event *descr_event)
  :Binary_log_event(&buf, descr_event->binlog_version,
                    descr_event->server_version)
  { }
  Ignorable_event() { }; // For the thd ctor of Ignorable_log_event
  virtual Log_event_type get_type_code() { return IGNORABLE_LOG_EVENT; }
  //TODO: Add definitions
  void print_event_info(std::ostream& info) { }
  void print_long_info(std::ostream& info) { }
};

/**
  @class Rows_query_event

  Rows query event type, which is a subclass
  of the Ignorable_event, to record the original query for the rows
  events in RBR. This event can be used to display the original query as
  comments by SHOW BINLOG EVENTS query, or mysqlbinlog client when the
  --verbose option is given twice

  @section Rows_query_var_event_binary_format Binary Format

  The Post-Header for this event type is empty. The Body has one
  component:

  <table>
  <caption>Body for Rows_query_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>m_rows_query</td>
    <td>char array</td>
    <td>Records the original query executed in RBR </td>
  </tr>
  </table>
*/
class Rows_query_event: public virtual Ignorable_event
{
public:
  Rows_query_event(const char *buf, unsigned int event_len,
                   const Format_description_event *descr_event);
  Rows_query_event()
  {
  }
  virtual ~Rows_query_event();
protected:
  char *m_rows_query;
};

/**
  @class Intvar_event

  An Intvar_event will be created just before a Query_event,
  if the query uses one of the variables LAST_INSERT_ID or INSERT_ID.
  Each Intvar_event holds the value of one of these variables.

  @section Intvar_event_binary_format Binary Format

  The Post-Header for this event type is empty. The Body has two
  components:

  <table>
  <caption>Body for Intvar_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>type</td>
    <td>1 byte enumeration</td>
    <td>One byte identifying the type of variable stored.  Currently,
    two identifiers are supported: LAST_INSERT_ID_EVENT == 1 and
    INSERT_ID_EVENT == 2.
    </td>
  </tr>

  <tr>
    <td>val</td>
    <td>8 byte unsigned integer</td>
    <td>The value of the variable.</td>
  </tr>

  </table>
*/
class Intvar_event: public Binary_log_event
{
public:
  uint8_t  type;
  uint64_t  val;

  /*
    The enum recognizes the type of variables that can occur in an
    INTVAR_EVENT. The two types supported are LAST_INSERT_ID and
    INSERT_ID, in accordance to the SQL query using LAST_INSERT_ID
    or INSERT_ID.
  */
  enum Int_event_type
  {
    INVALID_INT_EVENT,
    LAST_INSERT_ID_EVENT,
    INSERT_ID_EVENT
  };

  /**
    moving from pre processor symbols from global scope in log_event.h
    to an enum inside the class, since these are used only by
    members of this class itself.
  */
  enum Intvar_event_offset
  {
    I_TYPE_OFFSET= 0,
    I_VAL_OFFSET= 1
  };

  /**
    This method returns the string representing the type of the variable
    used in the event. Changed the definition to be similar to that
    previously defined in log_event.cc.
  */
  std::string get_var_type_string()
  {
    switch(type)
    {
    case INVALID_INT_EVENT:
      return "INVALID_INT";
    case LAST_INSERT_ID_EVENT:
      return "LAST_INSERT_ID";
    case INSERT_ID_EVENT:
      return "INSERT_ID";
    default: /* impossible */
      return "UNKNOWN";
    }
  }

  Intvar_event(const char* buf,
               const Format_description_event *description_event);

  Intvar_event(uint8_t type_arg, uint64_t val_arg)
  : type(type_arg), val(val_arg)
  {
  }

  ~Intvar_event() {}

  Log_event_type get_type_code() { return INTVAR_EVENT; }

  /*
    is_valid() is event specific sanity checks to determine that the
    object is correctly initialized. This is redundant here, because
    no new allocation is done in the constructor of the event.
    Else, they contain the value indicating whether the event was
    correctly initialized.
  */
  bool is_valid() const { return 1; }
  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
};

/**
  @class Incident_event

   Class representing an incident, an occurance out of the ordinary,
   that happened on the master.

   The event is used to inform the slave that something out of the
   ordinary happened on the master that might cause the database to be
   in an inconsistent state.

  @section Incident_event_binary_format Binary Format

   <table id="IncidentFormat">
   <caption>Incident event format</caption>
   <tr>
     <th>Symbol</th>
     <th>Format</th>
     <th>Description</th>
   </tr>
   <tr>
     <td>INCIDENT</td>
     <td align="right">2</td>
     <td>Incident number as an unsigned integer</td>
   </tr>
   <tr>
     <td>MSGLEN</td>
     <td align="right">1</td>
     <td>Message length as an unsigned integer</td>
   </tr>
   <tr>
     <td>MESSAGE</td>
     <td align="right">MSGLEN</td>
     <td>The message, if present. Not null terminated.</td>
   </tr>
   </table>

*/
class Incident_event: public Binary_log_event
{
public:
  /**
    Enumeration of the incidents that can occur for the server.
  */
  enum Incident {
  /** No incident */
  INCIDENT_NONE = 0,
  /** There are possibly lost events in the replication stream */
  INCIDENT_LOST_EVENTS = 1,
  /** Shall be last event of the enumeration */
  INCIDENT_COUNT
  };

  Incident_event(Incident incident_arg)
  : Binary_log_event(), incident(incident_arg), message(NULL),
    message_length(0)
  {
  }
  Incident_event(const char *buf, unsigned int event_len,
                 const Format_description_event *description_event);

  Log_event_type get_type_code() { return INCIDENT_EVENT; }
  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
protected:
  Incident incident;
  char *message;
  size_t message_length;
};

/**
  @class Xid_event

  An XID event is generated for a commit of a transaction that modifies one or
  more tables of an XA-capable storage engine. Strictly speaking, Xid_event
  is used if thd->transaction.xid_state.xid.get_my_xid() returns non-zero.

  @section Xid_event_binary_format Binary Format

The Body has the following component:

  <table>
  <caption>Body for Xid_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>xid</td>
    <td>8 byte unsigned integer</td>
    <td>The XID transaction number.</td>
  </tr>
  </table>
  The Post-Header and Body for this event type are empty; it only has
  the common header.
*/
class Xid_event: public Binary_log_event
{
public:
    Xid_event()
    {
    }
    Xid_event(const char *buf, const Format_description_event *fde);
    uint64_t xid;
    Log_event_type get_type_code() { return XID_EVENT; }
    bool is_valid() const { return 1; }
    void print_event_info(std::ostream& info);
    void print_long_info(std::ostream& info);
};


/**
  @class Rand_event

  Logs random seed used by the next RAND(), and by PASSWORD() in 4.1.0.
  4.1.1 does not need it (it's repeatable again) so this event needn't be
  written in 4.1.1 for PASSWORD() (but the fact that it is written is just a
  waste, it does not cause bugs).

  The state of the random number generation consists of 128 bits,
  which are stored internally as two 64-bit numbers.

  @section Rand_event_binary_format Binary Format

  The Post-Header for this event type is empty.  The Body has two
  components:

  <table>
  <caption>Body for Rand_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>seed1</td>
    <td>8 byte unsigned integer</td>
    <td>64 bit random seed1.</td>
  </tr>

  <tr>
    <td>seed2</td>
    <td>8 byte unsigned integer</td>
    <td>64 bit random seed2.</td>
  </tr>
  </table>
*/
class Rand_event: public Binary_log_event
{
  public:
  unsigned long long seed1;
  unsigned long long seed2;
  enum Rand_event_data
  {
    RAND_SEED1_OFFSET= 0,
    RAND_SEED2_OFFSET= 8
  };
  Rand_event(unsigned long long seed1_arg, unsigned long long seed2_arg)
  {
    seed1= seed1_arg;
    seed2= seed2_arg;
  }
  Rand_event(const char* buf,
             const Format_description_event *description_event);
  Log_event_type get_type_code() { return RAND_EVENT ; }
  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
};


/**
  @struct  gtid_info
  Structure to hold the members declared in the class Gtid_log_event
  those member are objects of classes defined in server(rpl_gtid.h).
  As we can not move all the classes defined there(in rpl_gtid.h) in libbinlogevent
  so this structure was created, to provide a way to map the decoded
  value in Gtid_event ctor and the class members defined in rpl_gtid.h,
  these classes are also the members of Gtid_log_event(subclass of this in server code)

  The structure contains the following components.
  <table>
  <caption>Structure gtid_info</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>
  <tr>
    <td>type</td>
    <td>enum_group_type</td>
    <td>Group type of the groups created while transaction</td>
  </tr>
  <tr>
    <td>bytes_to_copy</td>
    <td>size_t</td>
    <td>Number of bytes to copy from the buffer, this is
        used as the size of array uuid_buf</td>
  </tr>
  <tr>
    <td>uuid_buf</td>
    <td>unsigned char array</td>
    <td>This stores the Uuid of the server on which transaction
        is happening</td>
  </tr>
  <tr>
    <td>rpl_gtid_sidno</td>
    <td>4 bytes integer</td>
    <td>SIDNO (source ID number, first component of GTID)</td>
  </tr>
  <tr>
    <td>rpl_gtid_gno</td>
    <td>8 bytes integer</td>
    <td>GNO (group number, second component of GTID)</td>
  </tr>
  </table>
*/
struct gtid_info
{
  uint8_t type;
  //TODO Replace 16 with BYTE_LENGTH defined in struct Uuid in rpl_gtid.h
  const static size_t bytes_to_copy= 16;
  unsigned char uuid_buf[bytes_to_copy];
  int32_t  rpl_gtid_sidno;
  int64_t  rpl_gtid_gno;
};


/**
  @class Gtid_event
  GTID stands for Global Transaction IDentifier
  It is composed of two parts:
    - SID for Source Identifier, and
    - GNO for Group Number.
  The basic idea is to
     -  Associate an identifier, the Global Transaction IDentifier or GTID,
        to every transaction.
     -  When a transaction is copied to a slave, re-executed on the slave,
        and written to the slave's binary log, the GTID is preserved.
     -  When a  slave connects to a master, the slave uses GTIDs instead of
        (file, offset)

  @section Gtid_event_binary_format Binary Format

  The Body has five components:

  <table>
  <caption>Body for Gtid_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>commit_seq_no</td>
    <td>8 bytes signed integer</td>
    <td>Prepare and commit sequence number. will be set to 0 if the event is
        not a transaction starter</td>
  </tr>
  <tr>
    <td>ENCODED_FLAG_LENGTH</td>
    <td>4 bytes static const integer</td>
    <td>Length of the commit_flag in event encoding</td>
  </tr>
  <tr>
    <td>ENCODED_SID_LENGTH</td>
    <td>4 bytes static const integer</td>
    <td>Length of SID in event encoding</td>
  </tr>
  <tr>
    <td>ENCODED_GNO_LENGTH</td>
    <td>4 bytes static const integer</td>
    <td>Length of GNO in event encoding.</td>
  </tr>
  <tr>
    <td>commit_flag</td>
    <td>bool</td>
    <td>True if this is the last group of the transaction</td>
  </tr>
  </table>

*/
class Gtid_event: public Binary_log_event
{
public:
  int64_t commit_seq_no;
  Gtid_event(const char *buffer, uint32_t event_len,
             const Format_description_event *descr_event);
  Gtid_event(bool commit_flag_arg): commit_flag(commit_flag_arg) {}
  Log_event_type get_type_code()
  {
    Log_event_type ret= (gtid_info_struct.type == 2 ?
                         ANONYMOUS_GTID_LOG_EVENT : GTID_LOG_EVENT);
    return ret;
  }
  //TODO: Add definitions for these methods
  void print_event_info(std::ostream& info) { }
  void print_long_info(std::ostream& info) { }
protected:
  static const int ENCODED_FLAG_LENGTH= 1;
  static const int ENCODED_SID_LENGTH= 16;// Uuid::BYTE_LENGTH;
  static const int ENCODED_GNO_LENGTH= 8;
  gtid_info gtid_info_struct;
  bool commit_flag;
};

/**
  @class Previous_gtids_event

  @section Previous_gtids_event_binary_format Binary Format

  The Post-Header for this event type is empty.  The Body has two
  components:

  <table>
  <caption>Body for Previous_gtids_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>buf</td>
    <td>unsigned char array</td>
    <td>It contains the Gtids executed in the
        last binary log file.</td>
  </tr>

  <tr>
    <td>buf_size</td>
    <td>4 byte integer</td>
    <td>Size of the above buffer</td>
  </tr>
  </table>
*/
class Previous_gtids_event : public Binary_log_event
{
public:
  Previous_gtids_event(const char *buf, unsigned int event_len,
                       const Format_description_event *descr_event);
  Previous_gtids_event() { } //called by the ctor with Gtid_set parameter
  Log_event_type get_type_code() { return PREVIOUS_GTIDS_LOG_EVENT ; }
  void print_event_info(std::ostream& info) { }
  void print_long_info(std::ostream& info) { }
protected:
  int buf_size;
  const unsigned char *buf;
};


/**
  @class Heartbeat_event

  Replication event to ensure to slave that master is alive.
  The event is originated by master's dump thread and sent straight to
  slave without being logged. Slave itself does not store it in relay log
  but rather uses a data for immediate checks and throws away the event.

  Two members of the class log_ident and Binary_log_event::log_pos comprise
  @see the rpl_event_coordinates instance. The coordinates that a heartbeat
  instance carries correspond to the last event master has sent from
  its binlog.

  @section Heartbeat_event_binary_format Binary Format

  The Body has one component:

  <table>
  <caption>Body for Heartbeat_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>log_ident</td>
    <td>variable length string without trailing zero, extending to the
    end of the event</td>
    <td>Name of the current binlog being written to.</td>
  </tr>
  </table>
*/
class Heartbeat_event: public Binary_log_event
{
public:
  Heartbeat_event(const char* buf, unsigned int event_len,
                  const Format_description_event *description_event);

  const char* get_log_ident() { return log_ident; }
  unsigned int get_ident_len() { return ident_len; }

protected:
  const char* log_ident;
  unsigned int ident_len;                      /** filename length */
};

/**
  @class Unknown_event

  An unknown event should never occur. It is never written to a binary log.
  If an event is read from a binary log that cannot be recognized as
  something else, it is treated as UNKNOWN_EVENT.

  The Post-Header and Body for this event type are empty; it only has
  the Common-Header.
*/
class Unknown_event: public Binary_log_event
{
public:
  Unknown_event() {}
  Unknown_event(const char* buf,
                const Format_description_event *description_event)
  : Binary_log_event(&buf,
                     description_event->binlog_version,
                     description_event->server_version)
  {
  }

  Log_event_type get_type_code() { return UNKNOWN_EVENT;}
  void print_event_info(std::ostream& info);
  void print_long_info(std::ostream& info);
};
} // end namespace binary_log
/**
  @} (end of group Replication)
*/
#endif	/* BINLOG_EVENT_INCLUDED */
