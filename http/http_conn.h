#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include "../CGImysql/sql_connection_pool.h"
#include "../lock/locker.h"
#include "../log/log.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

class http_conn {

public:
  /* 文件名最大长度 */
  static const int FILENAME_LEN = 200;
  /* 读缓冲大小 */
  static const int READ_BUFFER_SIZE = 2048;
  /* 写缓冲大小 */
  static const int WRITE_BUFFER_SIZE = 1024;

  /* HTTP请求方法，目前仅支持GET */
  enum METHOD {
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATCH
  };

  /* 解析客户请求时，主状态机所处的状态 */
  enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0, /* 当前正在分析请求行 */
    CHECK_STATE_HEADER,          /* 当前正在分析请求头 */
    CHECK_STATE_CONTENT          /* 请求体 */
  };

  /* 服务器处理HTTP请求的可能结果 */
  enum HTTP_CODE {
    NO_REQUEST,        /* 请求不完整 */
    GET_REQUEST,       /* 获取一个完整的GET请求 */
    BAD_REQUEST,       /* 客户请求语法错误 */
    NO_RESOURCE,       /* 没有对应的资源 */
    FORBIDDEN_REQUEST, /* 客户对资源没有足够的访问权限 */
    FILE_REQUEST,      /* 请求文件 */
    INTERNAL_ERROR,
    CLOSED_CONNECTION
  };

  /* 行读取状态 */
  enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
  http_conn(){};
  ~http_conn(){};

public:
  /* 初始化新的连接 */
  void init(int sockfd, const sockaddr_in &addr);
  /* 关闭连接 */
  void close_conn(bool real_close = true);
  /* 处理客户请求 */
  void process();
  /* 非阻塞读 */
  bool read();
  /* 非阻塞写 */
  bool write();
  sockaddr_in *get_address() { return &m_address; }
  void initmysql_result(connection_pool *connPool);

private:
  /* 初始化连接 */
  void init();
  /* 解析HTTP 请求 */
  HTTP_CODE process_read();
  /* 填充HTTP应答 */
  bool process_write(HTTP_CODE ret);

  /* 下面这一组函数被 process_read 调用以分析HTTP请求 */
  HTTP_CODE parse_request_line(char *text);
  HTTP_CODE parse_headers(char *text);
  HTTP_CODE parse_content(char *text);
  HTTP_CODE do_request();
  char *get_line() { return m_read_buf + m_start_line; }
  LINE_STATUS parse_line();

  /* 下面这一组函数被 process_write 调用填充 HTTP 应答 */
  void unmap();
  bool add_response(const char *format, ...);
  bool add_content(const char *content);
  bool add_status_line(int status, const char *title);
  bool add_headers(int content_length);
  bool add_content_type();
  bool add_content_length(int content_length);
  bool add_linger();
  bool add_blank_line();

public:
  /* 所有socket上的事件都被注册到一个epoll内核事件表中，因此需要设置 static */
  static int m_epollfd;
  /* 统计数量 */
  static int m_user_count;
  MYSQL *mysql;

private:
  /* 该HTTP连接的socket和对方的socket地址 */
  int m_sockfd;
  sockaddr_in m_address;

  /* 读缓冲 */
  char m_read_buf[READ_BUFFER_SIZE];
  /* 表示读缓冲中已经读入的客户数据的最后一个字节的下一个位置 */
  int m_read_idx;
  /* 当前正在分析的字符在读缓冲中的位置 */
  int m_checked_idx;
  /* 当前正在解析的行起始位置 */
  int m_start_line;
  /* 写缓冲 */
  char m_write_buf[WRITE_BUFFER_SIZE];
  /* 写缓冲区中待发送的字节数 */
  int m_write_idx;

  /* 主状态机当前所处的状态 */
  CHECK_STATE m_check_state;
  /* 请求方法 */
  METHOD m_method;

  /* 客户请求目标文件的完整路径，内容等于doc_root + m_url
   * ，doc_root表示网址根目录 */
  char m_real_file[FILENAME_LEN];
  /* 客户请求目标文件的文件名 */
  char *m_url;
  /* HTTP版本号 */
  char *m_version;
  /* 主机名 */
  char *m_host;
  /* HTTP请求消息体长度 */
  int m_content_length;
  /* HTTP请求是否保持连接 */
  bool m_linger;

  /* 客户请求的目标文件被mmap到内存中启示位置 */
  char *m_file_address;
  /* 目标文件状态，用于判断文件
   * 是否存在，是否为根目录，是否可读，并获取文件大小等信息
   */
  struct stat m_file_stat;
  /* 采用write来执行写操作，所以定义下面两个成员
   * 其中 m_iv_count 表示被写入内存块的数量
   */
  struct iovec m_iv[2];
  int m_iv_count;

  int cgi;        // 是否启用的POST
  char *m_string; //存储请求头数据
  int bytes_have_send;
  int bytes_to_send;
};

#endif