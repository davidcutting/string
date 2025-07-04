#include <string/socket.hpp>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <functional>
#include <beman/execution26/execution.hpp>

namespace ex = ::beman::execution26;

constexpr int PORT = 8080;
constexpr int BACKLOG = 64;
constexpr int QUEUE_DEPTH = 256;
constexpr size_t BUFFER_SIZE = 1024;

// Wrapper around io_uring providing a custom scheduler
class UringScheduler {
public:
    UringScheduler() {
        if (io_uring_queue_init(QUEUE_DEPTH, &ring_, 0) < 0) {
            throw std::runtime_error("Failed to initialize io_uring");
        }
    }

    ~UringScheduler() {
        io_uring_queue_exit(&ring_);
    }

    io_uring& ring() { return ring_; }

    void run() {
        while (true) {
            int submitted = io_uring_submit(&ring_);
            if (submitted < 0) {
                std::cerr << "io_uring_submit failed: " << strerror(-submitted) << std::endl;
                break;
            }

            io_uring_cqe* cqe;
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                std::cerr << "io_uring_wait_cqe failed: " << strerror(-ret) << std::endl;
                break;
            }

            auto* cb = reinterpret_cast<std::function<void(int)>*>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            
            if (cb) {
                (*cb)(res);
                delete cb;
            }
        }
    }

private:
    io_uring ring_;
};

// Sender for async read operation using io_uring
class ReadSender {
public:
    using sender_concept = ex::sender_t;

    ReadSender(UringScheduler& sched, int fd, void* buf, size_t len)
        : sched_(sched), fd_(fd), buf_(buf), len_(len) {}

    template <ex::receiver R>
    class Operation {
    public:
        Operation(UringScheduler& sched, int fd, void* buf, size_t len, R r)
            : sched_(sched), fd_(fd), buf_(buf), len_(len), r_(std::move(r)) {}

        void start() noexcept {
            auto* sqe = io_uring_get_sqe(&sched_.ring());
            if (!sqe) {
                ex::set_error(std::move(r_), std::make_exception_ptr(
                    std::runtime_error("Failed to get SQE")));
                return;
            }

            io_uring_prep_read(sqe, fd_, buf_, len_, 0);
            
            auto* cb = new std::function<void(int)>([this](int res) {
                if (res < 0) {
                    ex::set_error(std::move(r_), std::make_exception_ptr(
                        std::runtime_error("Read failed: " + std::string(strerror(-res)))));
                } else {
                    ex::set_value(std::move(r_), res);
                }
            });
            
            io_uring_sqe_set_data(sqe, cb);
        }

    private:
        UringScheduler& sched_;
        int fd_;
        void* buf_;
        size_t len_;
        R r_;
    };

    template <typename R>
    auto connect(R r) && -> Operation<R> {
        return Operation<R>{sched_, fd_, buf_, len_, std::move(r)};
    }

    // Required for sender concept
    template <typename Env>
    auto get_completion_signatures(Env&&) const {
        return ex::completion_signatures<ex::set_value_t(int), ex::set_error_t(std::exception_ptr)>{};
    }

private:
    UringScheduler& sched_;
    int fd_;
    void* buf_;
    size_t len_;
};

// Sender for async accept operation using io_uring
class AcceptSender {
public:
    using sender_concept = ex::sender_t;

    AcceptSender(UringScheduler& sched, int fd, sockaddr* addr, socklen_t* addrlen)
        : sched_(sched), fd_(fd), addr_(addr), addrlen_(addrlen) {}

    template <ex::receiver R>
    class Operation {
    public:
        Operation(UringScheduler& sched, int fd, sockaddr* addr, socklen_t* addrlen, R r)
            : sched_(sched), fd_(fd), addr_(addr), addrlen_(addrlen), r_(std::move(r)) {}

        void start() noexcept {
            auto* sqe = io_uring_get_sqe(&sched_.ring());
            if (!sqe) {
                ex::set_error(std::move(r_), std::make_exception_ptr(
                    std::runtime_error("Failed to get SQE")));
                return;
            }

            io_uring_prep_accept(sqe, fd_, addr_, addrlen_, 0);
            
            auto* cb = new std::function<void(int)>([this](int res) {
                if (res < 0) {
                    ex::set_error(std::move(r_), std::make_exception_ptr(
                        std::runtime_error("Accept failed: " + std::string(strerror(-res)))));
                } else {
                    ex::set_value(std::move(r_), res);
                }
            });
            
            io_uring_sqe_set_data(sqe, cb);
        }

    private:
        UringScheduler& sched_;
        int fd_;
        sockaddr* addr_;
        socklen_t* addrlen_;
        R r_;
    };

    template <typename R>
    auto connect(R r) && -> Operation<R> {
        return Operation<R>{sched_, fd_, addr_, addrlen_, std::move(r)};
    }

    // Required for sender concept
    template <typename Env>
    auto get_completion_signatures(Env&&) const {
        return ex::completion_signatures<ex::set_value_t(int), ex::set_error_t(std::exception_ptr)>{};
    }

private:
    UringScheduler& sched_;
    int fd_;
    sockaddr* addr_;
    socklen_t* addrlen_;
};

// Sender for async write operation using io_uring
class WriteSender {
public:
    using sender_concept = ex::sender_t;

    WriteSender(UringScheduler& sched, int fd, const void* buf, size_t len)
        : sched_(sched), fd_(fd), buf_(buf), len_(len) {}

    template <ex::receiver R>
    class Operation {
    public:
        Operation(UringScheduler& sched, int fd, const void* buf, size_t len, R r)
            : sched_(sched), fd_(fd), buf_(buf), len_(len), r_(std::move(r)) {}

        void start() noexcept {
            auto* sqe = io_uring_get_sqe(&sched_.ring());
            if (!sqe) {
                ex::set_error(std::move(r_), std::make_exception_ptr(
                    std::runtime_error("Failed to get SQE")));
                return;
            }

            io_uring_prep_write(sqe, fd_, buf_, len_, 0);
            
            auto* cb = new std::function<void(int)>([this](int res) {
                if (res < 0) {
                    ex::set_error(std::move(r_), std::make_exception_ptr(
                        std::runtime_error("Write failed: " + std::string(strerror(-res)))));
                } else {
                    ex::set_value(std::move(r_), res);
                }
            });
            
            io_uring_sqe_set_data(sqe, cb);
        }

    private:
        UringScheduler& sched_;
        int fd_;
        const void* buf_;
        size_t len_;
        R r_;
    };

    template <typename R>
    auto connect(R r) && -> Operation<R> {
        return Operation<R>{sched_, fd_, buf_, len_, std::move(r)};
    }

private:
    UringScheduler& sched_;
    int fd_;
    const void* buf_;
    size_t len_;
};

// Factory functions
ReadSender async_read(UringScheduler& sched, int fd, void* buf, size_t len) {
    return ReadSender{sched, fd, buf, len};
}

AcceptSender async_accept(UringScheduler& sched, int fd, sockaddr* addr, socklen_t* addrlen) {
    return AcceptSender{sched, fd, addr, addrlen};
}

WriteSender async_write(UringScheduler& sched, int fd, const void* buf, size_t len) {
    return WriteSender{sched, fd, buf, len};
}

// Utility functions
int make_server_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    // Enable SO_REUSEADDR
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock);
        throw std::runtime_error("Failed to set SO_REUSEADDR");
    }

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        throw std::runtime_error("Failed to set non-blocking");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        throw std::runtime_error("Failed to bind socket");
    }

    if (listen(sock, BACKLOG) < 0) {
        close(sock);
        throw std::runtime_error("Failed to listen on socket");
    }

    return sock;
}

void handle_client(UringScheduler& uring, int client_fd) {
    auto buffer = std::make_shared<std::array<char, BUFFER_SIZE>>();
    
    // Create a proper receiver for the read operation
    struct ReadReceiver {
        UringScheduler& uring;
        int client_fd;
        std::shared_ptr<std::array<char, BUFFER_SIZE>> buffer;
        
        using receiver_concept = ex::receiver_t;
        
        void set_value(int bytes_read) && {
            if (bytes_read <= 0) {
                std::cout << "Client disconnected or read error\n";
                close(client_fd);
                return;
            }
            
            std::cout << "Received " << bytes_read << " bytes from client\n";
            
            // Create receiver for write operation
            struct WriteReceiver {
                int client_fd;
                using receiver_concept = ex::receiver_t;
                
                void set_value(int bytes_written) && {
                    std::cout << "Echoed " << bytes_written << " bytes back\n";
                    close(client_fd);
                }
                
                void set_error(std::exception_ptr) && {
                    std::cerr << "Write error\n";
                    close(client_fd);
                }
                
                void set_stopped() && {
                    close(client_fd);
                }
            };
            
            auto write_op = async_write(uring, client_fd, buffer->data(), bytes_read)
                .connect(WriteReceiver{client_fd});
            write_op.start();
        }
        
        void set_error(std::exception_ptr) && {
            std::cerr << "Read error\n";
            close(client_fd);
        }
        
        void set_stopped() && {
            close(client_fd);
        }
    };
    
    auto read_op = async_read(uring, client_fd, buffer->data(), buffer->size())
        .connect(ReadReceiver{uring, client_fd, buffer});
    read_op.start();
}

void accept_loop(UringScheduler& uring, int server_fd) {
    auto client_addr = std::make_shared<sockaddr_in>();
    auto client_len = std::make_shared<socklen_t>(sizeof(*client_addr));
    
    // Create a proper receiver for the accept operation
    struct AcceptReceiver {
        UringScheduler& uring;
        int server_fd;
        std::shared_ptr<sockaddr_in> client_addr;
        std::shared_ptr<socklen_t> client_len;
        
        using receiver_concept = ex::receiver_t;
        
        void set_value(int client_fd) && {
            if (client_fd < 0) {
                std::cerr << "Accept failed\n";
                // Continue accepting despite the error
                accept_loop(uring, server_fd);
                return;
            }
            
            // Set client socket to non-blocking
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            }
            
            std::cout << "Accepted new client connection (fd=" << client_fd << ")\n";
            handle_client(uring, client_fd);
            
            // Continue accepting new connections
            accept_loop(uring, server_fd);
        }
        
        void set_error(std::exception_ptr) && {
            std::cerr << "Accept error\n";
            // Continue accepting despite the error
            accept_loop(uring, server_fd);
        }
        
        void set_stopped() && {
            // Server stopped
        }
    };
    
    auto accept_op = async_accept(uring, server_fd, 
                                reinterpret_cast<sockaddr*>(client_addr.get()), 
                                client_len.get())
        .connect(AcceptReceiver{uring, server_fd, client_addr, client_len});
    accept_op.start();
}

int main() {
    try {
        UringScheduler uring;
        int server_fd = make_server_socket(PORT);
        
        std::cout << "TCP Echo Server listening on port " << PORT << "...\n";
        std::cout << "Press Ctrl+C to stop\n";

        // Start the accept loop
        accept_loop(uring, server_fd);

        // Run the io_uring event loop
        uring.run();
        
        close(server_fd);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

// auto main(int argc, char* argv[]) -> int
// {
//     std::cout << "Resolving address..." << std::endl;
//     const auto resolved = net::resolve({
//         .address = "localhost",
//         .port = "25678",
//         .address_family = net::AddressFamily::Unspecified,
//         .socket_protocol = net::SocketProtocol::TCP,
//         .operation_type = net::OperationType::Bidirectional,
//     });

//     if (resolved == nullptr)
//     {
//         std::cout << "Failed to resolve hostname." << std::endl;
//         return EXIT_FAILURE;
//     }



//     std::cout << "Binding socket..." << std::endl;
//     const auto[socket, result] = net::bind(resolved);

//     if (socket < 0)
//     {
//         std::cout << "Failed to bind socket." << std::endl;
//         return EXIT_FAILURE;
//     }



//     std::cout << "Listening on socket..." << std::endl;
//     const auto listen_result = net::listen(socket);

//     if (listen_result.value() != 0)
//     {
//         std::cout << "Failed to listen." << std::endl;
//         net::close(socket);
//         return EXIT_FAILURE;
//     }




//     std::cout << "Shutting down..." << std::endl;
//     net::close(socket);

//     return EXIT_SUCCESS;
// }