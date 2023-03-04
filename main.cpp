#include <arpa/inet.h>
#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./timer/list_time.h"
#include "http/http_conn.h"
#include "lock/locker.h"
#include "threadpool/threadpool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

//#define LISTENFDET //边缘触发非阻塞
#define LISTENFDLT //水平触发阻塞

/* 定义在 http_conn.cpp 中，用于修改描述符 */
extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

/* 设置定时器相关参数 */
static int pipefd[2];
static sort_timer_list timer_lst;
static int epollfd = 0;

/* 信号处理函数 */
void sig_handler(int sig) {
  /* 为保证函数的可重入性，保留原来的errno */
  int save_errno = errno;
  int msg = sig;
  send(pipefd[1], (char *)&msg, 1, 0);
  errno = save_errno;
}

void addsig(int sig, void(handler)(int), bool restart = true) {
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = handler;
  if (restart) {
    sa.sa_flags |= SA_RESTART;
  }
  sigfillset(&sa.sa_mask);
  assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler() {
  timer_lst.tick();
  alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
  assert(user_data);
  close(user_data->sockfd);
  printf("close fd: %d \n", user_data->sockfd);
  http_conn::m_user_count--;
}

void show_error(int connfd, const char *info) {
  printf("%s", info);
  send(connfd, info, strlen(info), 0);
  close(connfd);
}

int main(int argc, char *argv[]) {
  if (argc <= 2) {
    printf("usage: %s ip_address port_number\n", basename(argv[0]));
    return 1;
  }
  const char *ip = argv[1];
  int port = atoi(argv[2]);

  /* 忽略SIGPIPE信号 */
  addsig(SIGPIPE, SIG_IGN);

  /* 创建线程池 */
  threadpool<http_conn> *pool = NULL;
  try {
    pool = new threadpool<http_conn>;
  } catch (...) {
    return 1;
  }

  /* 预先为每个可能的客户连接分配一个http_conn 对象 */
  http_conn *users = new http_conn[MAX_FD];
  assert(users);
  int user_count = 0;

  int listenfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(listenfd >= 0);
  struct linger tmp = {1, 0};
  setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

  int ret = 0;
  struct sockaddr_in address;
  bzero(&address, sizeof(address));
  address.sin_family = AF_INET;
  inet_pton(AF_INET, ip, &address.sin_addr);
  address.sin_port = htons(port);

  ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
  assert(ret >= 0);

  ret = listen(listenfd, 5);
  assert(ret >= 0);

  /* 创建内核事件表 */
  epoll_event events[MAX_EVENT_NUMBER];
  int epollfd = epoll_create(5);
  assert(epollfd != -1);
  addfd(epollfd, listenfd, false); // 默认LT模式
  http_conn::m_epollfd = epollfd;

  /*  创建管道用于定时器 */
  ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
  assert(ret != -1);
  setnonblocking(pipefd[1]);
  addfd(epollfd, pipefd[0], false);

  addsig(SIGALRM, sig_handler, false);
  addsig(SIGTERM, sig_handler, false);
  bool stop_server = false;

  client_data *users_timer = new client_data[MAX_FD];

  bool timeout = false;
  alarm(TIMESLOT);

  while (!stop_server) {
    int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
    if ((number < 0) && (errno != EINTR)) {
      printf("epoll failure\n");
      break;
    }

    for (int i = 0; i < number; i++) {
      int sockfd = events[i].data.fd;
      if (sockfd == listenfd) {
        struct sockaddr_in client_address;
        socklen_t client_addrlength = sizeof(client_address);
#ifdef LISTENFDLT
        int connfd = accept(listenfd, (struct sockaddr *)&client_address,
                            &client_addrlength);
        if (connfd < 0) {
          printf("errno is: %d\n", errno);
          continue;
        }
        if (http_conn::m_user_count >= MAX_FD) {
          show_error(connfd, "Internal server busy");
          continue;
        }
        /* 初始化客户连接 */
        users[connfd].init(connfd, client_address);

        /*   初始化client_data数据
             创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
         */
        users_timer[connfd].address = client_address;
        users_timer[connfd].sockfd = connfd;
        util_timer *timer = new util_timer;
        timer->user_data = &users_timer[connfd];
        timer->cb_func = cb_func;
        time_t cur = time(NULL);
        timer->expire = cur + 3 * TIMESLOT;
        users_timer[connfd].timer = timer;
        timer_lst.add_timer(timer);

#endif

      } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {

        //服务器端关闭连接，移除对应的定时器
        util_timer *timer = users_timer[sockfd].timer;
        timer->cb_func(&users_timer[sockfd]);

        if (timer) {
          timer_lst.del_timer(timer);
        }

        // /* 如果发生异常，直接关闭客户端连接 */
        // users[sockfd].close_conn();
      }
      //处理信号
      else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
        int sig;
        char signals[1024];
        ret = recv(pipefd[0], signals, sizeof(signals), 0);
        if (ret == -1) {
          continue;
        } else if (ret == 0) {
          continue;
        } else {
          for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
            case SIGALRM: {
              timeout = true;
              break;
            }
            case SIGTERM: {
              stop_server = true;
            }
            }
          }
        }
      }
      /* 处理客户连接上接收到的数据 */
      else if (events[i].events & EPOLLIN) {
        /* 根据读的结果，决定和是将任务添加到线程池还是关闭连接 */
        util_timer *timer = users_timer[sockfd].timer;
        if (users[sockfd].read()) {
          printf("deal with the client(%s)",
                 inet_ntoa(users[sockfd].get_address()->sin_addr));

          /* 如果监测到读事件，将该事件放入请求队列 */
          pool->append(users + sockfd);

          /* 若有数据传输，则将定时器往后延迟3个单位,并对新的定时器在链表上的位置进行调整
           */
          if (timer) {
            time_t cur = time(NULL);
            timer->expire = cur + 3 * TIMESLOT;
            printf("%s", "adjust timer once");

            timer_lst.adjust_timer(timer);
          }

        } else {
          timer->cb_func(&users_timer[sockfd]);
          if (timer) {
            timer_lst.del_timer(timer);
          }

          // users[sockfd].close_conn();
        }
      } else if (events[i].events & EPOLLOUT) {
        /* 根据写的结果决定是否关闭 */
        util_timer *timer = users_timer[sockfd].timer;
        if (!users[sockfd].write()) {
          printf("send data to the client(%s)",
                 inet_ntoa(users[sockfd].get_address()->sin_addr));

          /* 若有数据传输，则将定时器往后延迟3个单位,并对新的定时器在链表上的位置进行调整
           */
          if (timer) {
            time_t cur = time(NULL);
            timer->expire = cur + 3 * TIMESLOT;
            printf("%s", "adjust timer once");

            timer_lst.adjust_timer(timer);
          } else {
            timer->cb_func(&users_timer[sockfd]);
            if (timer) {
              timer_lst.del_timer(timer);
            }
          }
        }
      }
    }
    if (timeout) {
      timer_handler();
      timeout = false;
    }
  }

  close(epollfd);
  close(listenfd);
  close(pipefd[1]);
  close(pipefd[0]);
  delete[] users;
  delete[] users_timer;
  delete pool;
  return 0;
}
