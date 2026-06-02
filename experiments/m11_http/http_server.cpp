#include "EventLoop.h"
#include "HttpRequest.h"   // m11::HttpRequest, m11::Buffer
#include <map>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <memory>

// 每个 HTTP 连接的上下文:接收缓冲 + 解析器
struct HttpConn {
    m11::Buffer buf;
    m11::HttpRequest req;
};

// fd → 连接上下文(方案 B:外挂表,不污染 channel)
std::map<int, HttpConn> g_conns;

void sendData(m7::EventLoop& loop, m7::channel* ch, const char* data, ssize_t len) {
    ssize_t total = 0;

    if (ch->out_buf.empty()) {
        while(total < len) {
            ssize_t n = send(ch->fd, data + total, len - total, MSG_NOSIGNAL);
            if (n > 0) {
                total += n;
            }
            else if (n < 0 && (errno == EINTR)) {
                continue;
            }
            else if (n < 0 && (errno == EAGAIN)) {
                break;
            }
            else {
                return;
            }
        }
    }

    if (total < len) {
        ch->out_buf.append(data + total, len - total);
        printf("[send] EAGAIN, buffered %zu bytes, enable EPOLLOUT\n", ch->out_buf.size());
        if (!(ch->events & EPOLLOUT)) {
            ch->events |= EPOLLOUT;
            loop.modifyChannel(ch);
        }
    }
}


int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(listen_fd, (const sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) { perror("timerfd_create"); return 1; }

    struct itimerspec value;
    value.it_value.tv_sec = 2;     // 首次 2 秒后到期
    value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1;  // 之后每 1 秒一次
    value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) { perror("settime"); return 1; }

    m7::EventLoop loop;

    const int TIMEOUT = 5;
    std::shared_ptr<m7::channel> timer_channel = std::make_shared<m7::channel>();
    timer_channel->fd = timerfd;
    timer_channel->events = EPOLLIN;
    timer_channel->on_read = [timerfd, &loop, TIMEOUT](){
        uint64_t exp = 0;
        read(timerfd, &exp, sizeof(exp));

        time_t now = time(nullptr);
        std::vector<int> timeout_fds;
        loop.forEachChannel([&now, &TIMEOUT, &timeout_fds, &timerfd](m7::channel* ch){
            if (ch->fd == timerfd) return;
            if (ch->timeout_exempt) return;
            if (now - ch->last_active > TIMEOUT) timeout_fds.push_back(ch->fd);
        });

        for (int fd: timeout_fds) {
            printf("[timer] fd=%d idle timeout, closing\n", fd);
            loop.removeChannel(fd);
        }
    };

    loop.addChannel(timer_channel);

    std::shared_ptr<m7::channel> listen_channel = std::make_shared<m7::channel>();
    listen_channel->fd = listen_fd;
    listen_channel->events = EPOLLIN | EPOLLET;
    listen_channel->timeout_exempt = true;
    listen_channel->on_read = [listen_fd, &loop](){
        while (1) {
            int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                perror("accept4");
                break;
            }
            g_conns[client_fd]; 

            std::shared_ptr<m7::channel> client_channel = std::make_shared<m7::channel>();
            client_channel->fd = client_fd;
            client_channel->events = EPOLLIN | EPOLLET;
            client_channel->last_active = time(nullptr);

            m7::channel* ch_raw = client_channel.get();
            client_channel->on_read = [client_fd, &loop, ch_raw](){
                HttpConn& conn = g_conns[client_fd];

                while(1) {
                    char tmp[4096];
                    ssize_t n_read = recv(client_fd, tmp, sizeof(tmp), 0);
                    if (n_read < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN) break;
                        perror("recv");
                        loop.removeChannel(client_fd);   // close 交给 channel 析构
                        g_conns.erase(client_fd);
                        return;
                    }
                    else if (n_read == 0) {
                        loop.removeChannel(client_fd);   // 同上
                        g_conns.erase(client_fd);
                        return;
                    }
                    else {
                        // printf("[Server] fd=%d recv %ld bytes\n", client_fd, n_read);
                        ch_raw->last_active = time(nullptr);
                        conn.buf.append(tmp, n_read);
                        while (true) {
                            m11::ParseResult r = conn.req.parse(&conn.buf);
                            if (r == m11::ParseResult::kComplete) {
                                // 拼响应,发回去
                                std::string body = "Hello, path=" + conn.req.path() + "\n";
                                std::string resp =
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/plain\r\n"
                                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                    "\r\n" + body;
                                sendData(loop, ch_raw, resp.data(), resp.size());

                                conn.req.reset();              // 复用,准备下一条
                                // 继续 while,看 buffer 里还有没有下一条
                            }
                            else if (r == m11::ParseResult::kIncomplete) {
                                break;                         // 没有更多完整请求,等下次 recv
                            }
                            else {  // kError
                                loop.removeChannel(client_fd);
                                g_conns.erase(client_fd);
                                return;
                            }
                        }
                    }
                }
            };
            client_channel->on_write = [client_fd, &loop, ch_raw](){
                while (!ch_raw->out_buf.empty()) {
                    ssize_t n = send(client_fd, ch_raw->out_buf.data(), ch_raw->out_buf.size(), MSG_NOSIGNAL);
                    if (n > 0) {
                        ch_raw->out_buf.erase(0,n);
                        printf("[write] flushed %zd bytes, remain %zu\n", n, ch_raw->out_buf.size());
                    }
                    else if (n < 0 && (errno == EINTR)) {
                        continue;
                    }
                    else if (n < 0 && (errno == EAGAIN)) {
                        break;
                    }
                    else {break;}
                }
                if (ch_raw->out_buf.empty()) {
                    ch_raw->events &= ~EPOLLOUT;
                    loop.modifyChannel(ch_raw);
                    printf("[write] out_buf drained, disable EPOLLOUT\n");
                }
            };
            loop.addChannel(client_channel);
        }
    };
    loop.addChannel(listen_channel);

    loop.loop();
}