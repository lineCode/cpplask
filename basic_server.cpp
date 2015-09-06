#include <basic_server.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <csignal>

#include <map>
#include <vector>
#include <sstream>
#include <atomic>
#include <thread>
#include <algorithm>

namespace cpplask {

namespace
{
    std::atomic<bool> g_keep_going;
}

extern "C" {
    static void signal_handler(int)
    {
        std::cerr << "SIGINT received" << std::endl;
        g_keep_going = false; 
    }
}

sigset_t stop_signals() {
    sigset_t set;
    sigset_t orig_set;
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &orig_set);
    return orig_set;
}

void restore_signals(sigset_t& signals) {
    pthread_sigmask(SIG_SETMASK, &signals, NULL);
}

struct listen_socket {
    int listen_socket_fd;
    listen_socket(uint32_t port) : 
        listen_socket_fd(-1) {

        sockaddr_in server_address;

        try {
            listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (listen_socket_fd < 0) {
                throw std::runtime_error("error during socket opening");
            }

            bzero((char *) &server_address, sizeof(server_address));

            server_address.sin_family = AF_INET;
            server_address.sin_addr.s_addr = INADDR_ANY;
            server_address.sin_port = htons(port);

            if (bind(listen_socket_fd, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
                throw std::runtime_error(std::string("Error during socket binding: ") + strerror(errno));
            }
        }
        catch (...) {
            if (listen_socket_fd >= 0) {
                close(listen_socket_fd);
            }
            throw;
        }
    }
    ~listen_socket() {
        close(listen_socket_fd);
    }
};

void serve(cpplask::service_t& service, int listen_socket_fd);
void basic_serve(cpplask::service_t& service, uint32_t port) {
    g_keep_going=true;
    listen_socket listen_socket(port);
    std::cerr << "listening on port " << port << std::endl;
    auto orig = stop_signals();
    serve(service, listen_socket.listen_socket_fd);
    restore_signals(orig);
    std::cerr << "Exiting" << std::endl;
}

auto parse_request(std::string bufferstr) -> auto {
    char* buffer = const_cast<char*>(bufferstr.c_str());

    char* strtok_state;
    char* current_line_buf = 0;

    std::string verb = strtok_r(buffer, " ", &strtok_state);
    std::string path = strtok_r(NULL, " ", &strtok_state);
    std::map<std::string, std::string> query_params;

    std::string version = strtok_r(NULL, "\n", &strtok_state);
    std::map<std::string, std::string> headers;
    do {
        current_line_buf = strtok_r(NULL, "\n", &strtok_state);
        const std::string current_line(current_line_buf);
        size_t index_of_colon = current_line.find(":"); 
        if (index_of_colon != std::string::npos) {
            const std::string name = current_line.substr(0, index_of_colon);
            const std::string value = current_line.substr(index_of_colon+2, current_line.length()-index_of_colon-3);
            headers[name] = value;
        }
    } while (strlen(current_line_buf) > 1);
    std::cerr << verb << " " << path << "\n";

    return std::make_pair(path, headers); 
}

void serve(cpplask::service_t& service, int listen_socket_fd) {

    struct sigaction new_action;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = 0;
    new_action.sa_handler = signal_handler;
    sigaction(SIGINT, &new_action, NULL);
    sigaction(SIGQUIT, &new_action, NULL);
    sigaction(SIGTERM, &new_action, NULL);

    std::vector<std::pair<int,std::string>> incoming_fds;

    while (g_keep_going) {
        listen(listen_socket_fd,5);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_socket_fd, &rfds);
        int max_fd = listen_socket_fd;
        for (auto&& fdpair : incoming_fds) {
            int fd = std::get<0>(fdpair);
            max_fd = std::max(max_fd, fd);
            FD_SET(fd, &rfds);
        }

        sigset_t mask;
        sigemptyset (&mask);

        int pselect_rtn = pselect(max_fd+1, &rfds, NULL, NULL, NULL, &mask);
        if (pselect_rtn < 0) {
            perror("pselect");
        }
        if (!g_keep_going) {
            break;
        }
        if (pselect_rtn > 0 && FD_ISSET(listen_socket_fd, &rfds)) {
            // new client connecting    
            std::cerr << "client connected" << std::endl;
            sockaddr_in client_address;
            socklen_t client_address_len = sizeof(client_address);

            int accept_socket_fd = accept(listen_socket_fd, 
                     (struct sockaddr *) &client_address, 
                     &client_address_len);
            if (accept_socket_fd < 0) {
                throw std::runtime_error("Error during accept");
            }
            incoming_fds.emplace_back(accept_socket_fd, std::string());
        }


        incoming_fds.erase(std::remove_if(incoming_fds.begin(), incoming_fds.end(), [&](auto&& fdpair) {
            int& accept_socket_fd = std::get<0>(fdpair);
            std::string& buffstr = std::get<1>(fdpair);
            
            if (FD_ISSET(accept_socket_fd, &rfds)) {

                char buffer[2048] = {0};
                int num_read = read(accept_socket_fd,buffer,2040);
                if (num_read <= 0) {
                    throw std::runtime_error("ERROR reading from socket");
                }

                buffstr += std::string(buffer, num_read);
                std::string marker = "\r\n\r\n";

                if (std::search(buffstr.begin(), buffstr.end(), marker.begin(), marker.end()) == buffstr.end()) {
                    return false;
                }

                auto parameters = parse_request(buffstr);
                cpplask::request_t req(std::get<0>(parameters), std::get<1>(parameters));
                service.serve(req);

                std::string message = req.response().str();

                std::stringstream ss;
                ss << "HTTP/1.1 " << req.response().code() << " " << req.response().status() << "\n";
                ss << "Content-Type: text/plain\n";
                ss << "Content-length: " << message.size() << "\n\n";
                ss << message;
                int num_written = write(accept_socket_fd,ss.str().c_str(),ss.str().size());
                if (num_written < 0) {
                    throw std::runtime_error("Error during writing to socket");
                }
                close(accept_socket_fd);
                return true;
            }
            else {
                return false;
            }
        }), incoming_fds.end());
    }
} 

}
